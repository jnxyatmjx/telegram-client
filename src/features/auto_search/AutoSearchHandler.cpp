#include "features/auto_search/AutoSearchHandler.h"

#include <app/AppContext.h>
#include <app/WorkerPool.h>
#include <common/Idempotency.h>
#include <common/Logger.h>
#include <telegram/FloodRetry.h>
#include <telegram/TdErrors.h>

#include <memory>
#include <utility>

namespace tg::features::auto_search {

AutoSearchHandler::AutoSearchHandler(const app::AutoSearchConfig& config,
                                     media_thumbnail::SearchHandler& search)
    : config_(config), search_(search) {
  for (const std::int64_t id : config_.chats) AddAllow(id);
}

bool AutoSearchHandler::IsMediaMessage(const td_api::message& message) {
  if (message.content_ == nullptr) return false;
  switch (message.content_->get_id()) {
    case td_api::messagePhoto::ID:
    case td_api::messageVideo::ID:
    case td_api::messageAnimation::ID:
      return true;
    case td_api::messageDocument::ID: {
      // Same files MediaExtractor will actually send: image/* bytes, or a
      // video/mp4 thumbnail (app_bot_client.handlermedia). Other MIME types
      // are not searchable.
      const auto* content = static_cast<const td_api::messageDocument*>(
          message.content_.get());
      if (content->document_ == nullptr) return false;
      const std::string& mime = content->document_->mime_type_;
      if (mime.compare(0, 6, "image/") == 0) return true;
      return mime == "video/mp4";
    }
    default:
      return false;
  }
}

bool AutoSearchHandler::IsAllowlisted(std::int64_t chat_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return allow_set_.count(chat_id) > 0;
}

void AutoSearchHandler::AddAllow(std::int64_t chat_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (allow_set_.insert(chat_id).second) allow_order_.push_back(chat_id);
}

void AutoSearchHandler::NoteResolve(ResolveRecord record) {
  std::lock_guard<std::mutex> lock(mutex_);
  resolve_records_.push_back(std::move(record));
}

std::vector<std::int64_t> AutoSearchHandler::allowlist() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return allow_order_;
}

std::vector<AutoSearchHandler::ResolveRecord>
AutoSearchHandler::resolve_records() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return resolve_records_;
}

void AutoSearchHandler::LogEffectiveAllowlist() const {
  std::string joined;
  std::size_t n = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    n = allow_order_.size();
    for (const std::int64_t id : allow_order_) {
      if (!joined.empty()) joined += ", ";
      joined += std::to_string(id);
    }
  }
  spdlog::info("auto_search: effective allowlist ({}): {}", n, joined);
}

bool AutoSearchHandler::RecentlySearched(std::uint32_t oid,
                                         std::uint32_t id) const {
  const auto key = media_thumbnail::SearchCorrelationTable::Key(oid, id);
  const auto ttl = std::chrono::seconds(config_.dedup_ttl_seconds);
  std::lock_guard<std::mutex> lock(mutex_);
  // Opportunistic sweep of expired entries.
  const auto now = std::chrono::steady_clock::now();
  for (auto it = recent_.begin(); it != recent_.end();) {
    if (now - it->second > ttl) {
      it = recent_.erase(it);
    } else {
      ++it;
    }
  }
  return recent_.count(key) > 0;
}

bool AutoSearchHandler::CanHandle(
    const td_api::object_ptr<td_api::Update>& update) const {
  // Default config: enabled == false -> never claims (phase 2, v5 §2.7).
  if (!config_.enabled) return false;
  if (update == nullptr || update->get_id() != td_api::updateNewMessage::ID) {
    return false;
  }
  const auto& new_message =
      static_cast<const td_api::updateNewMessage&>(*update);
  const td_api::message* message = new_message.message_.get();
  if (message == nullptr) return false;
  // chat_id allowlist only; sender intentionally unchecked (§2.7 step 2).
  if (!IsAllowlisted(message->chat_id_)) return false;
  return IsMediaMessage(*message);
}

void AutoSearchHandler::Handle(td_api::object_ptr<td_api::Update>& update,
                               app::AppContext& ctx) {
  const auto new_message =
      td_api::move_object_as<td_api::updateNewMessage>(std::move(update));
  const td_api::message* message = new_message->message_.get();
  if (message == nullptr) return;

  // Belt and braces: the dispatcher only calls Handle for claimed updates,
  // but Handle is also reachable directly — re-check the claim conditions
  // (allowlisted chat + media message) before doing any work.
  if (!IsAllowlisted(message->chat_id_) || !IsMediaMessage(*message)) {
    return;
  }

  const auto oid = common::ToBareChatId(message->chat_id_);
  const auto bare_id = common::ToBareMessageId(message->id_);
  if (!oid || !bare_id) {
    // v5 §1.3 hard rule 1: loud fail, never truncate.
    spdlog::error(
        "auto_search: chat_id {} / message_id {} do not fit uint32 bare ids "
        "— NOT searching", message->chat_id_, message->id_);
    return;
  }

  if (RecentlySearched(oid->oid, *bare_id)) {
    spdlog::info("auto_search: (oid={}, id={}) already searched within TTL — "
                 "skipped", oid->oid, *bare_id);
    return;
  }

  {
    const auto key = media_thumbnail::SearchCorrelationTable::Key(oid->oid, *bare_id);
    std::lock_guard<std::mutex> lock(mutex_);
    recent_[key] = std::chrono::steady_clock::now();
  }

  // SearchImages ONLY (never ProcessImages, v5 §2.7 step 5). Async
  // kick-off: returns immediately once the download starts; the outcome
  // is logged by the pipeline's finish stage.
  search_.SendSearchImages(*message, ctx);
}

namespace {

bool IsError(const td::ClientManager::Response& response) {
  return response.object == nullptr ||
         response.object->get_id() == td_api::error::ID;
}

const td_api::error* AsError(const td::ClientManager::Response& response) {
  if (response.object == nullptr ||
      response.object->get_id() != td_api::error::ID) {
    return nullptr;
  }
  return static_cast<const td_api::error*>(response.object.get());
}

std::string ErrorText(const td::ClientManager::Response& response) {
  if (const auto* error = AsError(response)) {
    return std::to_string(error->code_) + ": " + error->message_;
  }
  if (response.object == nullptr) return "no object";
  return "unexpected object";
}

std::string ChatTypeName(const td_api::chat& chat) {
  if (chat.type_ == nullptr) return "unknown";
  switch (chat.type_->get_id()) {
    case td_api::chatTypeSupergroup::ID: {
      const auto& type =
          static_cast<const td_api::chatTypeSupergroup&>(*chat.type_);
      return type.is_channel_ ? "channel" : "supergroup";
    }
    case td_api::chatTypeBasicGroup::ID:
      return "basic group";
    case td_api::chatTypePrivate::ID:
      return "private";
    case td_api::chatTypeSecret::ID:
      return "secret";
    default:
      return "unknown";
  }
}

bool IsChannelOrSupergroup(const td_api::chat& chat) {
  return chat.type_ != nullptr &&
         chat.type_->get_id() == td_api::chatTypeSupergroup::ID;
}

// left / banned / restricted-but-not-a-member: updates will not arrive.
bool IsJoined(const td_api::ChatMemberStatus& status) {
  switch (status.get_id()) {
    case td_api::chatMemberStatusLeft::ID:
    case td_api::chatMemberStatusBanned::ID:
      return false;
    case td_api::chatMemberStatusRestricted::ID:
      return static_cast<const td_api::chatMemberStatusRestricted&>(status)
          .is_member_;
    default:
      return true;
  }
}

struct ResolveJob {
  AutoSearchHandler* self = nullptr;
  telegram::ITdSender* sender = nullptr;
  telegram::FloodRetry* flood = nullptr;
  app::WorkerPool* workers = nullptr;
  std::vector<app::AutoSearchChatSpec> specs;
  std::size_t index = 0;
  std::int64_t self_id = 0;
};

using MakeRequest = std::function<td_api::object_ptr<td_api::Function>()>;
using OnResponse = std::function<void(td::ClientManager::Response)>;

// TDLib invokes the Send callback on the receive thread. Post the result
// to the worker pool before any further work. FloodWait is retried from
// the FloodRetry timer thread, which only calls Send.
void SendOffReceive(const std::shared_ptr<ResolveJob>& job, int attempt,
                    MakeRequest make_request, OnResponse on_response) {
  // Build the request before the callback lambda moves `make_request`.
  // Argument evaluation order is unspecified; calling make_request() as a
  // Send argument can run after that move and throw bad_function_call.
  auto request = make_request();
  job->sender->Send(
      std::move(request),
      [job, attempt, make_request = std::move(make_request),
       on_response = std::move(on_response)](
          td::ClientManager::Response&& response) mutable {
        auto held =
            std::make_shared<td::ClientManager::Response>(std::move(response));
        if (!job->workers->Post([job, attempt,
                                 make_request = std::move(make_request),
                                 on_response = std::move(on_response),
                                 held]() mutable {
              if (const auto* error = AsError(*held);
                  error != nullptr && telegram::IsFloodWait(*error) &&
                  job->flood != nullptr &&
                  job->flood->Schedule(
                      *error, attempt,
                      [job, attempt, make_request, on_response]() mutable {
                        SendOffReceive(job, attempt + 1, std::move(make_request),
                                       std::move(on_response));
                      })) {
                return;
              }
              on_response(std::move(*held));
            })) {
          spdlog::error(
              "auto_search: worker pool rejected allowlist resolution");
        }
      });
}

}  // namespace

class AllowlistDriver {
 public:
  static void LogParsed(AutoSearchHandler* self) {
    std::string joined;
    std::size_t n = 0;
    if (!self->config_.chat_specs.empty()) {
      n = self->config_.chat_specs.size();
      for (const auto& spec : self->config_.chat_specs) {
        if (!joined.empty()) joined += ", ";
        joined += spec.raw;
      }
    } else {
      n = self->config_.chats.size();
      for (const std::int64_t id : self->config_.chats) {
        if (!joined.empty()) joined += ", ";
        joined += std::to_string(id);
      }
    }
    spdlog::info("auto_search: parsed chats ({}): {}", n, joined);
    if (self->config_.enabled && n == 0) {
      spdlog::warn(
          "auto_search: enabled but chats list is empty — check "
          "indentation/format of features.auto_search.chats");
    }
  }

  static void Start(AutoSearchHandler* self, telegram::ITdSender& sender,
                    telegram::FloodRetry* flood, app::WorkerPool& workers) {
    self->resolve_finished_.store(false);
    LogParsed(self);
    if (!self->config_.enabled) {
      spdlog::info("auto_search: disabled — not resolving chat allowlist");
      self->resolve_finished_.store(true);
      return;
    }
    auto job = std::make_shared<ResolveJob>();
    job->self = self;
    job->sender = &sender;
    job->flood = flood;
    job->workers = &workers;
    job->specs = self->config_.chat_specs;
    if (job->specs.empty()) {
      for (const std::int64_t id : self->config_.chats) {
        app::AutoSearchChatSpec spec;
        spec.raw = std::to_string(id);
        spec.numeric = true;
        spec.dialog_id = id;
        job->specs.push_back(spec);
      }
    }
    if (job->specs.empty()) {
      spdlog::info("auto_search: effective allowlist (0):");
      self->resolve_finished_.store(true);
      return;
    }
    SendOffReceive(
        job, 1, [] { return td_api::make_object<td_api::getMe>(); },
        [job](td::ClientManager::Response response) {
          if (!IsError(response) &&
              response.object->get_id() == td_api::user::ID) {
            job->self_id =
                static_cast<const td_api::user&>(*response.object).id_;
          } else {
            spdlog::warn(
                "auto_search: getMe failed ({}); membership will not be "
                "checked",
                ErrorText(response));
          }
          Advance(job);
        });
  }

 private:
  static std::string Label(const app::AutoSearchChatSpec& spec) {
    return spec.username.empty() ? spec.raw : ("@" + spec.username);
  }

  static void FinishOne(const std::shared_ptr<ResolveJob>& job,
                        const td_api::chat* chat, bool member) {
    auto* self = job->self;
    const auto& spec = job->specs[job->index];
    const std::string label = Label(spec);
    AutoSearchHandler::ResolveRecord record;
    record.raw = spec.raw;
    if (chat == nullptr) {
      record.failure = spec.numeric ? "getChat failed" : "not found";
      self->NoteResolve(record);
    } else {
      record.chat_id = chat->id_;
      const std::string type = ChatTypeName(*chat);
      const bool channel = IsChannelOrSupergroup(*chat);
      spdlog::info("auto_search: {} -> chat_id {} (title \"{}\", type {})",
                   label, chat->id_, chat->title_, type);
      if (!spec.numeric && !channel) {
        record.failure = "not a channel or supergroup";
        spdlog::warn(
            "auto_search: {} — chat_id {} has type {}, not a channel or "
            "supergroup; not added to the allowlist",
            label, chat->id_, type);
        self->NoteResolve(record);
      } else {
        if (spec.numeric && !channel) {
          spdlog::warn(
              "auto_search: {} — chat_id {} has type {}, not a channel or "
              "supergroup; id kept on the allowlist",
              label, chat->id_, type);
        }
        self->AddAllow(chat->id_);
        record.added = true;
        record.member = member;
        if (!member) {
          record.failure = "not a member";
          spdlog::warn(
              "auto_search: {} — account is not a member of chat_id {} "
              "(title \"{}\", type {}); updates for this chat will not arrive",
              label, chat->id_, chat->title_, type);
        }
        self->NoteResolve(record);
      }
    }
    ++job->index;
    Advance(job);
  }

  static void AfterChat(const std::shared_ptr<ResolveJob>& job,
                        td::ClientManager::Response response) {
    auto* self = job->self;
    const auto& spec = job->specs[job->index];
    const std::string label = Label(spec);
    if (IsError(response) || response.object->get_id() != td_api::chat::ID) {
      if (spec.numeric) {
        spdlog::warn(
            "auto_search: {} — getChat failed ({}); id kept on the allowlist",
            label, ErrorText(response));
        AutoSearchHandler::ResolveRecord record;
        record.raw = spec.raw;
        record.chat_id = spec.dialog_id;
        record.added = true;
        record.failure = "getChat failed";
        self->AddAllow(spec.dialog_id);
        self->NoteResolve(record);
      } else {
        spdlog::warn("auto_search: {} — not found ({})", label,
                     ErrorText(response));
        AutoSearchHandler::ResolveRecord record;
        record.raw = spec.raw;
        record.failure = "not found";
        self->NoteResolve(record);
      }
      ++job->index;
      Advance(job);
      return;
    }
    auto chat =
        td_api::move_object_as<td_api::chat>(std::move(response.object));
    if (job->self_id == 0 || !IsChannelOrSupergroup(*chat)) {
      FinishOne(job, chat.get(), /*member=*/true);
      return;
    }
    const std::int64_t chat_id = chat->id_;
    auto held =
        std::make_shared<td_api::object_ptr<td_api::chat>>(std::move(chat));
    SendOffReceive(
        job, 1,
        [chat_id, self_id = job->self_id] {
          return td_api::make_object<td_api::getChatMember>(
              chat_id, td_api::make_object<td_api::messageSenderUser>(self_id));
        },
        [job, held](td::ClientManager::Response member_response) {
          bool member = true;
          if (!IsError(member_response) &&
              member_response.object->get_id() == td_api::chatMember::ID) {
            const auto& member_obj =
                static_cast<const td_api::chatMember&>(*member_response.object);
            member = member_obj.status_ != nullptr &&
                     IsJoined(*member_obj.status_);
          } else {
            spdlog::warn(
                "auto_search: getChatMember failed for chat_id {} ({}); "
                "membership not confirmed",
                (*held)->id_, ErrorText(member_response));
          }
          FinishOne(job, held->get(), member);
        });
  }

  static void Advance(const std::shared_ptr<ResolveJob>& job) {
    auto* self = job->self;
    if (job->index >= job->specs.size()) {
      self->LogEffectiveAllowlist();
      self->resolve_finished_.store(true);
      return;
    }
    const auto& spec = job->specs[job->index];
    if (!spec.numeric && spec.username.empty()) {
      spdlog::warn("auto_search: \"{}\" — not a chat id or public username",
                   spec.raw);
      AutoSearchHandler::ResolveRecord record;
      record.raw = spec.raw;
      record.failure = "invalid";
      self->NoteResolve(record);
      ++job->index;
      Advance(job);
      return;
    }
    if (spec.numeric) {
      const std::int64_t id = spec.dialog_id;
      SendOffReceive(
          job, 1, [id] { return td_api::make_object<td_api::getChat>(id); },
          [job](td::ClientManager::Response response) {
            AfterChat(job, std::move(response));
          });
      return;
    }
    const std::string username = spec.username;
    SendOffReceive(
        job, 1,
        [username] {
          return td_api::make_object<td_api::searchPublicChat>(username);
        },
        [job](td::ClientManager::Response response) {
          AfterChat(job, std::move(response));
        });
  }
};

void AutoSearchHandler::ResolveAllowlist(telegram::ITdSender& sender,
                                        telegram::FloodRetry* flood,
                                        app::WorkerPool& workers) {
  AllowlistDriver::Start(this, sender, flood, workers);
}

}  // namespace tg::features::auto_search
