#include "features/commands/DialogScanner.h"

#include <app/AppContext.h>
#include <common/Idempotency.h>
#include <common/Logger.h>
#include <telegram/TdErrors.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>

namespace tg::features::commands {
namespace td_api = td::td_api;
namespace {

const td_api::error* AsError(const td::ClientManager::Response& response) {
  if (response.object == nullptr) return nullptr;
  if (response.object->get_id() != td_api::error::ID) return nullptr;
  return static_cast<const td_api::error*>(response.object.get());
}

std::string ErrorText(const td::ClientManager::Response& response) {
  if (const auto* error = AsError(response)) {
    return std::to_string(error->code_) + ": " + error->message_;
  }
  return "<not an error>";
}

// The scan chain runs on worker tasks; a rejected Post would otherwise kill
// the scan, so run the step inline on the Receive thread instead (every step
// is a cheap TDLib send / string build — safe outside the pool).
void PostOrRun(app::AppContext& ctx, std::function<void()> task,
               const char* what) {
  if (ctx.workers.Post(std::move(task))) return;
  spdlog::warn("dialog_scan: worker pool rejected the {} step — running it "
               "inline", what);
}

}  // namespace

// --- ChatView findings (Python check_status 1319-1350 wording) ------------

bool ChatView::HasFinding() const {
  if (banned) return true;
  if (kind == Kind::kBasicGroup) {
    if (deactivated) return true;
    if (migrated) return true;
    if (left) return true;
    return false;
  }
  if (kind == Kind::kChannel) {
    if (scam) return true;
    if (fake) return true;
    if (left) return true;
    if (probe_failed) return true;
  }
  return false;
}

std::string ChatView::FindingReason() const {
  if (banned) return "已被封禁/删除（Forbidden）";
  if (kind == Kind::kBasicGroup) {
    if (deactivated) return "基础群已停用（deactivated）";
    if (migrated) return "已迁移为超级群（migrated_to）";
    if (left) return "已退出（left）";
  }
  if (kind == Kind::kChannel) {
    if (scam) return "被标记为诈骗（scam）";
    if (fake) return "被标记为仿冒（fake）";
    if (left) return "已退出（left）";
    if (probe_failed) {
      if (probe_error.find("CHANNEL_PRIVATE") != std::string::npos) {
        return "频道私有/已失效（ChannelPrivateError）";
      }
      return "检测出错：" + probe_error;
    }
  }
  return "";
}

std::string ChatView::Link() const {
  if (!username.empty()) return "https://t.me/" + username;
  if (kind == Kind::kChannel) return "https://t.me/c/" + std::to_string(bare_id);
  return "（私有群，无公开链接）";
}

// --- DialogScanner ---------------------------------------------------------

DialogScanner::DialogScanner(telegram::ITdSender& sender, Config config)
    : sender_(sender), config_(config),
      resume_timer_([this] { OnResumeTimer(); }) {}

DialogScanner::~DialogScanner() { Shutdown(); }

void DialogScanner::Shutdown() {
  stopping_.store(true, std::memory_order_release);
  resume_timer_.Shutdown();
  std::lock_guard<std::mutex> lock(mutex_);
  running_ = false;
  on_done_ = nullptr;
  retry_step_ = nullptr;
  ctx_ = nullptr;
}

bool DialogScanner::running() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return running_;
}

bool DialogScanner::Scan(app::AppContext& ctx, Mode mode,
                         std::function<void(std::vector<ChatView>)> on_done) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_.load(std::memory_order_acquire) || running_) return false;
    running_ = true;
    mode_ = mode;
    on_done_ = std::move(on_done);
    loading_archive_ = false;
    chat_ids_.clear();
    next_chat_ = 0;
    views_.clear();
    current_ = ChatView{};
    ctx_ = &ctx;
  }
  LoadMoreChats(ctx);
  return true;
}

// Step 1: loadChats until the 404 "all chats loaded" error — the main list
// first, then the archive list (loading_archive_). Python's get_dialogs /
// iter_dialogs counts archived dialogs too; TDLib keeps them in a separate
// chatListArchive that is never loaded unless asked for.
void DialogScanner::LoadMoreChats(app::AppContext& ctx) {
  bool archive = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    archive = loading_archive_;
  }
  auto request = td_api::make_object<td_api::loadChats>();
  request->chat_list_ =
      archive ? td_api::make_object<td_api::chatListArchive>()
              : td_api::object_ptr<td_api::ChatList>{};
  request->limit_ = 100;  // TDLib caps the limit at 100 anyway
  sender_.Send(
      std::move(request),
      [this, &ctx](td::ClientManager::Response&& response) {
        auto response_ptr =
            std::make_shared<td::ClientManager::Response>(std::move(response));
        PostOrRun(ctx, [this, &ctx, response_ptr] {
          if (stopping_.load(std::memory_order_acquire)) return;
          if (const auto* error = AsError(*response_ptr)) {
            if (error->code_ == 404) {
              bool archive = false;
              {
                std::lock_guard<std::mutex> lock(mutex_);
                archive = loading_archive_;
                if (!archive) loading_archive_ = true;
              }
              if (!archive) {
                LoadMoreChats(ctx);  // main list done — now the archive
                return;
              }
              FetchChatList(ctx);  // both lists loaded
              return;
            }
            if (telegram::IsFloodWait(*error)) {
              FloodPause(ctx, *error, [this, &ctx] { LoadMoreChats(ctx); });
              return;
            }
            spdlog::error("dialog_scan: loadChats failed: {} — scan aborted",
                          ErrorText(*response_ptr));
            Finish(ctx, false);
            return;
          }
          LoadMoreChats(ctx);  // Ok -> load the next chunk
        }, "loadChats");
      });
}

// Step 2: the ordered list of everything loaded in step 1 (main list).
void DialogScanner::FetchChatList(app::AppContext& ctx) {
  auto request = td_api::make_object<td_api::getChats>();
  request->chat_list_ = nullptr;  // main chat list
  request->limit_ = config_.dialog_limit;
  sender_.Send(
      std::move(request),
      [this, &ctx](td::ClientManager::Response&& response) {
        auto response_ptr =
            std::make_shared<td::ClientManager::Response>(std::move(response));
        PostOrRun(ctx, [this, &ctx, response_ptr] {
          if (stopping_.load(std::memory_order_acquire)) return;
          if (const auto* error = AsError(*response_ptr)) {
            if (telegram::IsFloodWait(*error)) {
              FloodPause(ctx, *error, [this, &ctx] { FetchChatList(ctx); });
              return;
            }
            spdlog::error("dialog_scan: getChats failed: {} — scan aborted",
                          ErrorText(*response_ptr));
            Finish(ctx, false);
            return;
          }
          const auto& chats =
              static_cast<const td_api::chats&>(*response_ptr->object);
          spdlog::info("dialog_scan: {} dialogs in main list (limit {})",
                       chats.chat_ids_.size(), config_.dialog_limit);
          FetchArchiveChatList(ctx, chats.chat_ids_);
        }, "getChats");
      });
}

// Step 2b: the archive list's chats, merged with the main list's ids.
void DialogScanner::FetchArchiveChatList(
    app::AppContext& ctx, std::vector<std::int64_t> main_ids) {
  auto request = td_api::make_object<td_api::getChats>();
  request->chat_list_ = td_api::make_object<td_api::chatListArchive>();
  request->limit_ = config_.dialog_limit;
  sender_.Send(
      std::move(request),
      [this, &ctx, main_ids = std::move(main_ids)](
          td::ClientManager::Response&& response) mutable {
        auto response_ptr =
            std::make_shared<td::ClientManager::Response>(std::move(response));
        PostOrRun(ctx, [this, &ctx, response_ptr, main_ids = std::move(main_ids)] {
          if (stopping_.load(std::memory_order_acquire)) return;
          if (const auto* error = AsError(*response_ptr)) {
            if (telegram::IsFloodWait(*error)) {
              FloodPause(ctx, *error, [this, &ctx, main_ids] {
                FetchArchiveChatList(ctx, main_ids);
              });
              return;
            }
            spdlog::error(
                "dialog_scan: getChats(archive) failed: {} — scan aborted",
                ErrorText(*response_ptr));
            Finish(ctx, false);
            return;
          }
          const auto& chats =
              static_cast<const td_api::chats&>(*response_ptr->object);
          spdlog::info("dialog_scan: {} dialogs in archive list",
                       chats.chat_ids_.size());
          // Merge: main first, then archive (a chat lives in exactly one
          // list, but dedupe defensively), truncated to dialog_limit
          // total — Python's get_dialogs(limit=1202) is a total.
          std::vector<std::int64_t> merged = std::move(main_ids);
          for (const std::int64_t id : chats.chat_ids_) {
            if (std::find(merged.begin(), merged.end(), id) == merged.end()) {
              merged.push_back(id);
            }
          }
          if (config_.dialog_limit > 0 &&
              static_cast<int>(merged.size()) > config_.dialog_limit) {
            merged.resize(static_cast<std::size_t>(config_.dialog_limit));
          }
          {
            std::lock_guard<std::mutex> lock(mutex_);
            chat_ids_ = std::move(merged);
            next_chat_ = 0;
          }
          spdlog::info("dialog_scan: {} dialogs enumerated (limit {})",
                       chat_ids_.size(), config_.dialog_limit);
          ResolveNext(ctx);
        }, "getChats(archive)");
      });
}

// Step 3 loop: resolve one enumerated chat at a time (the sequential chain
// is the natural throttle Python got from its explicit 0.1 s sleep).
void DialogScanner::ResolveNext(app::AppContext& ctx) {
  std::int64_t chat_id = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (next_chat_ < chat_ids_.size()) chat_id = chat_ids_[next_chat_];
  }
  if (chat_id == 0) {
    Finish(ctx, true);
    return;
  }
  SendGetChat(ctx, chat_id);
}

void DialogScanner::SendGetChat(app::AppContext& ctx, std::int64_t chat_id) {
  auto request = td_api::make_object<td_api::getChat>();
  request->chat_id_ = chat_id;
  sender_.Send(
      std::move(request),
      [this, &ctx, chat_id](td::ClientManager::Response&& response) {
        auto response_ptr =
            std::make_shared<td::ClientManager::Response>(std::move(response));
        PostOrRun(ctx, [this, &ctx, chat_id, response_ptr] {
          if (stopping_.load(std::memory_order_acquire)) return;
          if (const auto* error = AsError(*response_ptr)) {
            if (telegram::IsFloodWait(*error)) {
              FloodPause(ctx, *error,
                         [this, &ctx, chat_id] { SendGetChat(ctx, chat_id); });
              return;
            }
            // Forbidden / private / deleted: Python's Forbidden findings
            // come from exactly this failure; skip and continue.
            spdlog::warn("dialog_scan: getChat({}) failed: {} — skipped",
                         chat_id, ErrorText(*response_ptr));
            Advance(ctx);
            return;
          }
          HandleEnumeratedChat(
              td_api::move_object_as<td_api::chat>(
                  std::move(response_ptr->object)),
              ctx);
        }, "getChat");
      });
}

void DialogScanner::HandleEnumeratedChat(
    td_api::object_ptr<td_api::chat> chat, app::AppContext& ctx) {
  if (chat == nullptr || chat->type_ == nullptr) {
    Advance(ctx);
    return;
  }
  const std::int64_t dialog_id = chat->id_;
  std::string title = chat->title_;
  // Seed the pending view with what the chat object already carries; the
  // Continue* step fills in the peer-type details. A FloodWait retry then
  // only needs the peer id in its closure.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = ChatView{};
    current_.dialog_id = dialog_id;
    current_.title = title;
  }

  if (chat->type_->get_id() == td_api::chatTypePrivate::ID) {
    const auto* type =
        static_cast<const td_api::chatTypePrivate*>(chat->type_.get());
    if (title.empty()) {
      // Python handlersm 887: unnamed users are banned/deleted — skip.
      spdlog::warn("dialog_scan: private chat {} has no title (banned or "
                   "deleted user) — skipped", dialog_id);
      Advance(ctx);
      return;
    }
    SendGetUser(ctx, type->user_id_);
    return;
  }
  if (chat->type_->get_id() == td_api::chatTypeBasicGroup::ID) {
    const auto* type =
        static_cast<const td_api::chatTypeBasicGroup*>(chat->type_.get());
    if (title.empty()) {
      spdlog::warn("dialog_scan: basic group {} has no title — skipped",
                   dialog_id);
      Advance(ctx);
      return;
    }
    SendGetBasicGroup(ctx, type->basic_group_id_);
    return;
  }
  if (chat->type_->get_id() == td_api::chatTypeSupergroup::ID) {
    const auto* type =
        static_cast<const td_api::chatTypeSupergroup*>(chat->type_.get());
    if (title.empty()) {
      spdlog::warn("dialog_scan: supergroup {} has no title — skipped",
                   dialog_id);
      Advance(ctx);
      return;
    }
    SendGetSupergroup(ctx, type->supergroup_id_);
    return;
  }
  // chatTypeSecretChat etc. — not part of the Python listing.
  Advance(ctx);
}

void DialogScanner::SendGetUser(app::AppContext& ctx, std::int64_t user_id) {
  auto request = td_api::make_object<td_api::getUser>();
  request->user_id_ = user_id;
  sender_.Send(
      std::move(request),
      [this, &ctx, user_id](td::ClientManager::Response&& response) {
        auto response_ptr =
            std::make_shared<td::ClientManager::Response>(std::move(response));
        PostOrRun(ctx, [this, &ctx, user_id, response_ptr] {
          if (stopping_.load(std::memory_order_acquire)) return;
          if (const auto* error = AsError(*response_ptr)) {
            if (telegram::IsFloodWait(*error)) {
              FloodPause(ctx, *error,
                         [this, &ctx, user_id] { SendGetUser(ctx, user_id); });
              return;
            }
            spdlog::warn("dialog_scan: getUser failed: {} — skipped",
                         ErrorText(*response_ptr));
            Advance(ctx);
            return;
          }
          ContinueUser(td_api::move_object_as<td_api::user>(
                           std::move(response_ptr->object)),
                       ctx);
        }, "getUser");
      });
}

void DialogScanner::SendGetBasicGroup(app::AppContext& ctx,
                                      std::int64_t group_id) {
  auto request = td_api::make_object<td_api::getBasicGroup>();
  request->basic_group_id_ = group_id;
  sender_.Send(
      std::move(request),
      [this, &ctx, group_id](td::ClientManager::Response&& response) {
        auto response_ptr =
            std::make_shared<td::ClientManager::Response>(std::move(response));
        PostOrRun(ctx, [this, &ctx, group_id, response_ptr] {
          if (stopping_.load(std::memory_order_acquire)) return;
          if (const auto* error = AsError(*response_ptr)) {
            if (telegram::IsFloodWait(*error)) {
              FloodPause(ctx, *error, [this, &ctx, group_id] {
                SendGetBasicGroup(ctx, group_id);
              });
              return;
            }
            spdlog::warn("dialog_scan: getBasicGroup({}) failed: {} — "
                         "skipped", group_id, ErrorText(*response_ptr));
            Advance(ctx);
            return;
          }
          ContinueBasicGroup(
              td_api::move_object_as<td_api::basicGroup>(
                  std::move(response_ptr->object)),
              ctx);
        }, "getBasicGroup");
      });
}

void DialogScanner::SendGetSupergroup(app::AppContext& ctx,
                                      std::int64_t supergroup_id) {
  auto request = td_api::make_object<td_api::getSupergroup>();
  request->supergroup_id_ = supergroup_id;
  sender_.Send(
      std::move(request),
      [this, &ctx, supergroup_id](td::ClientManager::Response&& response) {
        auto response_ptr =
            std::make_shared<td::ClientManager::Response>(std::move(response));
        PostOrRun(ctx, [this, &ctx, supergroup_id, response_ptr] {
          if (stopping_.load(std::memory_order_acquire)) return;
          if (const auto* error = AsError(*response_ptr)) {
            if (telegram::IsFloodWait(*error)) {
              FloodPause(ctx, *error, [this, &ctx, supergroup_id] {
                SendGetSupergroup(ctx, supergroup_id);
              });
              return;
            }
            spdlog::warn("dialog_scan: getSupergroup({}) failed: {} — "
                         "skipped", supergroup_id, ErrorText(*response_ptr));
            Advance(ctx);
            return;
          }
          ContinueSupergroup(
              td_api::move_object_as<td_api::supergroup>(
                  std::move(response_ptr->object)),
              ctx);
        }, "getSupergroup");
      });
}

void DialogScanner::SendProbeSupergroup(app::AppContext& ctx) {
  std::int64_t supergroup_id = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    supergroup_id = current_.bare_id;
  }
  auto request = td_api::make_object<td_api::getSupergroupFullInfo>();
  request->supergroup_id_ = supergroup_id;
  sender_.Send(
      std::move(request),
      [this, &ctx](td::ClientManager::Response&& response) {
        auto response_ptr =
            std::make_shared<td::ClientManager::Response>(std::move(response));
        PostOrRun(ctx, [this, &ctx, response_ptr] {
          if (stopping_.load(std::memory_order_acquire)) return;
          ChatView view;
          if (const auto* error = AsError(*response_ptr)) {
            if (telegram::IsFloodWait(*error)) {
              FloodPause(ctx, *error,
                         [this, &ctx] { SendProbeSupergroup(ctx); });
              return;
            }
            // The probe finding: private / dead channel (Python
            // ChannelPrivateError) or whatever else the probe reported.
            std::lock_guard<std::mutex> lock(mutex_);
            current_.probe_failed = true;
            current_.probe_error = std::to_string(error->code_) + ": " +
                                   error->message_;
            view = std::move(current_);
            current_ = ChatView{};
          } else {
            std::lock_guard<std::mutex> lock(mutex_);
            view = std::move(current_);
            current_ = ChatView{};
          }
          Push(std::move(view), ctx);
        }, "getSupergroupFullInfo");
      });
}

void DialogScanner::ContinueBasicGroup(
    td_api::object_ptr<td_api::basicGroup> group, app::AppContext& ctx) {
  if (group == nullptr) {
    Advance(ctx);
    return;
  }
  bool migrated = false;
  std::int64_t migrated_bare = 0;
  ChatView view;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (group->upgraded_to_supergroup_id_ != 0) {
      // Python resolve_active_chat_entity: a migrated group is listed as
      // its new supergroup (handlersm 851-858 drops the old id and keeps
      // the new one). current_ is rebuilt when the new chat resolves.
      migrated = true;
      migrated_bare = group->upgraded_to_supergroup_id_;
      current_ = ChatView{};
    } else {
      current_.bare_id = group->id_;
      current_.kind = ChatView::Kind::kBasicGroup;
      current_.deactivated = !group->is_active_;
      current_.left =
          group->status_ != nullptr &&
          group->status_->get_id() == td_api::chatMemberStatusLeft::ID;
      current_.banned =
          group->status_ != nullptr &&
          group->status_->get_id() == td_api::chatMemberStatusBanned::ID;
      view = std::move(current_);
      current_ = ChatView{};
    }
  }
  if (migrated) {
    ContinueMigrated(migrated_bare, ctx);
    return;
  }
  Push(std::move(view), ctx);
}

void DialogScanner::ContinueMigrated(std::int64_t supergroup_bare_id,
                                     app::AppContext& ctx) {
  const auto candidates = common::DialogIdCandidates(supergroup_bare_id);
  if (candidates.empty()) {
    Advance(ctx);
    return;
  }
  SendGetChat(ctx, candidates.front());
}

void DialogScanner::ContinueSupergroup(
    td_api::object_ptr<td_api::supergroup> group, app::AppContext& ctx) {
  if (group == nullptr) {
    Advance(ctx);
    return;
  }
  bool audit = false;
  ChatView view;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    current_.bare_id = group->id_;
    current_.kind = ChatView::Kind::kChannel;
    current_.is_channel = group->is_channel_;
    if (group->usernames_ != nullptr &&
        !group->usernames_->active_usernames_.empty()) {
      current_.username = group->usernames_->active_usernames_.front();
    }
    if (group->verification_status_ != nullptr) {
      current_.scam = group->verification_status_->is_scam_;
      current_.fake = group->verification_status_->is_fake_;
    }
    if (group->restriction_info_ != nullptr) {
      current_.restricted =
          !group->restriction_info_->restriction_reason_.empty();
    }
    current_.left =
        group->status_ != nullptr &&
        group->status_->get_id() == td_api::chatMemberStatusLeft::ID;
    current_.banned =
        group->status_ != nullptr &&
        group->status_->get_id() == td_api::chatMemberStatusBanned::ID;
    audit = mode_ == Mode::kAudit;
    if (!audit) {
      view = std::move(current_);
      current_ = ChatView{};
    }
  }
  if (audit) {
    // Python check_status 1344-1349: probe with GetFullChannelRequest — a
    // private / dead channel errors here.
    SendProbeSupergroup(ctx);
    return;
  }
  Push(std::move(view), ctx);
}

void DialogScanner::ContinueUser(td_api::object_ptr<td_api::user> user,
                                 app::AppContext& ctx) {
  if (user == nullptr) {
    Advance(ctx);
    return;
  }
  std::string username;
  if (user->usernames_ != nullptr &&
      !user->usernames_->active_usernames_.empty()) {
    username = user->usernames_->active_usernames_.front();
  }
  const std::string first_name = user->first_name_;
  ChatView view;
  bool skip = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    current_.kind = ChatView::Kind::kUser;
    current_.bare_id = user->id_;
    current_.username = std::move(username);
    current_.first_name = first_name;
    if (current_.title.empty()) current_.title = first_name;
    skip = current_.title.empty();  // banned/deleted (Python handlersm 887)
    view = std::move(current_);
    current_ = ChatView{};
  }
  if (skip) {
    spdlog::warn("dialog_scan: user {} has no name (banned or deleted) — "
                 "skipped", user->id_);
    Advance(ctx);
    return;
  }
  Push(std::move(view), ctx);
}

void DialogScanner::Push(ChatView view, app::AppContext& ctx) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    views_.push_back(std::move(view));
    current_ = ChatView{};
  }
  Advance(ctx);
}

void DialogScanner::Advance(app::AppContext& ctx) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++next_chat_;
  }
  ResolveNext(ctx);
}

void DialogScanner::Finish(app::AppContext& ctx, bool ok) {
  std::function<void(std::vector<ChatView>)> on_done;
  std::vector<ChatView> views;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    running_ = false;
    ctx_ = nullptr;
    if (!on_done_) return;
    on_done = std::move(on_done_);
    on_done_ = nullptr;
    if (ok) views = std::move(views_);
    views_.clear();
  }
  if (!ok) {
    spdlog::error("dialog_scan: scan failed — reporting failure");
  } else {
    spdlog::info("dialog_scan: scan done — {} dialogs", views.size());
  }
  if (!ctx.workers.Post([on_done, views = std::move(views)]() mutable {
        on_done(std::move(views));
      })) {
    // Pool rejected (shutdown drain): run inline — the callback only builds
    // strings and sends messages (async).
    on_done(std::move(views));
  }
}

void DialogScanner::FloodPause(app::AppContext& ctx,
                               const td_api::error& error,
                               std::function<void()> retry) {
  const std::int64_t seconds = telegram::RetryAfterOr(error, 5);
  const auto delay =
      std::chrono::milliseconds((seconds + config_.flood_pad_seconds) * 1000);
  spdlog::warn("dialog_scan: FloodWait ({} s) — pausing {} ms", seconds,
               delay.count());
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_.load(std::memory_order_acquire)) return;
    retry_step_ = std::move(retry);
    ctx_ = &ctx;
  }
  resume_timer_.Arm(delay);
}

void DialogScanner::OnResumeTimer() {
  std::function<void()> retry;
  app::AppContext* ctx = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    retry = std::move(retry_step_);
    retry_step_ = nullptr;
    ctx = ctx_;
  }
  if (retry == nullptr || ctx == nullptr ||
      stopping_.load(std::memory_order_acquire)) {
    return;
  }
  if (!ctx->workers.Post(std::move(retry))) {
    spdlog::warn("dialog_scan: worker pool rejected the resume step");
  }
}

}  // namespace tg::features::commands
