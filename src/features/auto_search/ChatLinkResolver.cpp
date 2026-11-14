#include "features/auto_search/ChatLinkResolver.h"

#include <app/WorkerPool.h>
#include <common/Idempotency.h>
#include <common/Logger.h>
#include <features/auto_search/SearchReply.h>
#include <telegram/FloodRetry.h>
#include <telegram/TdErrors.h>

#include <utility>

namespace tg::features::auto_search {
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

bool StartsWithHttp(const std::string& url) {
  return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
}

std::string AsciiLower(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

std::optional<std::string> PublicUsername(const td_api::usernames* names) {
  if (names == nullptr) return std::nullopt;
  if (!names->active_usernames_.empty() &&
      !names->active_usernames_.front().empty()) {
    return names->active_usernames_.front();
  }
  if (!names->editable_username_.empty()) return names->editable_username_;
  return std::nullopt;
}

}  // namespace

struct ChatLinkResolver::Batch {
  BaseMap bases;
  std::vector<std::int64_t> left;
  bool finished = false;
  Done done;
};

ChatLinkResolver::ChatLinkResolver(const app::AutoSearchConfig& config,
                                   telegram::ITdSender* sender,
                                   app::WorkerPool* workers,
                                   telegram::FloodRetry* flood,
                                   ChatLinkResolveOptions options)
    : options_(options),
      sender_(sender),
      workers_(workers),
      flood_(flood),
      clock_([] { return std::chrono::steady_clock::now(); }) {
  for (const auto& link : config.chat_links) {
    const std::string url = NormalizeLinkBase(link.url);
    if (link.numeric) {
      by_id_[link.dialog_id] = url;
      if (link.bare_id > 0) by_id_[link.bare_id] = url;
      if (link.dialog_id > 0) {
        by_id_[-link.dialog_id - common::kSupergroupMark] = url;
      }
    } else if (!link.username.empty()) {
      by_username_[AsciiLower(link.username)] = url;
    }
  }
  timer_ = std::thread([this] { TimerLoop(); });
}

ChatLinkResolver::~ChatLinkResolver() { Shutdown(); }

void ChatLinkResolver::SetChatUrlLookup(ChatUrlLookup lookup) {
  std::lock_guard<std::mutex> lock(mutex_);
  chat_url_ = std::move(lookup);
}

void ChatLinkResolver::SetClockForTest(
    std::function<std::chrono::steady_clock::time_point()> clock) {
  std::lock_guard<std::mutex> lock(mutex_);
  clock_ = std::move(clock);
}

std::chrono::steady_clock::time_point ChatLinkResolver::Now() const {
  if (clock_) return clock_();
  return std::chrono::steady_clock::now();
}

std::optional<std::string> ChatLinkResolver::LookupReady(std::int64_t cid) const {
  if (const auto it = by_id_.find(cid); it != by_id_.end()) return it->second;
  if (chat_url_) {
    if (const auto url = chat_url_(cid);
        url.has_value() && StartsWithHttp(*url)) {
      return NormalizeLinkBase(*url);
    }
  }
  const auto cached = cache_.find(cid);
  if (cached != cache_.end() && cached->second.expires > Now() &&
      cached->second.positive) {
    return cached->second.url;
  }
  return std::nullopt;
}

bool ChatLinkResolver::NegativeHit(std::int64_t cid) const {
  const auto cached = cache_.find(cid);
  return cached != cache_.end() && cached->second.expires > Now() &&
         !cached->second.positive;
}

void ChatLinkResolver::Remember(std::int64_t cid, bool positive, std::string url) {
  CacheEntry entry;
  entry.positive = positive;
  entry.url = std::move(url);
  const auto ttl = positive ? options_.positive_ttl : options_.negative_ttl;
  entry.expires = Now() + ttl;
  cache_[cid] = std::move(entry);
}

void ChatLinkResolver::Resolve(std::vector<std::int64_t> cids, Done done) {
  std::vector<std::int64_t> unique;
  unique.reserve(cids.size());
  for (const std::int64_t cid : cids) {
    bool seen = false;
    for (const std::int64_t have : unique) {
      if (have == cid) {
        seen = true;
        break;
      }
    }
    if (!seen) unique.push_back(cid);
  }

  auto batch = std::make_shared<Batch>();
  batch->done = std::move(done);
  std::vector<std::int64_t> pending;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      for (const std::int64_t cid : unique) batch->bases[cid] = std::nullopt;
    } else {
      for (const std::int64_t cid : unique) {
        if (const auto ready = LookupReady(cid)) {
          batch->bases[cid] = *ready;
        } else if (NegativeHit(cid)) {
          batch->bases[cid] = std::nullopt;
        } else if (cid <= 0 || sender_ == nullptr || workers_ == nullptr) {
          batch->bases[cid] = std::nullopt;
        } else {
          pending.push_back(cid);
          batch->left.push_back(cid);
        }
      }
    }
  }
  if (pending.empty()) {
    if (batch->done) batch->done(std::move(batch->bases));
    return;
  }
  Arm(batch);
  for (const std::int64_t cid : pending) StartCid(batch, cid);
}

void ChatLinkResolver::Shutdown() {
  std::thread timer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ && !timer_.joinable()) return;
    stopped_ = true;
    timer = std::move(timer_);
  }
  cv_.notify_all();
  if (timer.joinable()) timer.join();
}

void ChatLinkResolver::SendOff(const std::shared_ptr<Batch>& batch, int attempt,
                               MakeRequest make_request, OnResponse on_response) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || batch->finished) return;
  }
  auto request = make_request();
  sender_->Send(
      std::move(request),
      [this, batch, attempt, make_request = std::move(make_request),
       on_response = std::move(on_response)](
          td::ClientManager::Response&& response) mutable {
        auto held = std::make_shared<td::ClientManager::Response>(
            std::move(response));
        auto task = [this, batch, attempt,
                     make_request = std::move(make_request),
                     on_response = std::move(on_response), held]() mutable {
          bool flood = false;
          {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_ || batch->finished) return;
            if (const auto* error = AsError(*held);
                error != nullptr && telegram::IsFloodWait(*error) &&
                flood_ != nullptr) {
              flood = flood_->Schedule(
                  *error, attempt,
                  [this, batch, attempt, make_request, on_response]() mutable {
                    SendOff(batch, attempt + 1, std::move(make_request),
                            std::move(on_response));
                  },
                  /*rank=*/1);
            }
          }
          if (flood) return;
          on_response(std::move(*held));
        };
        if (workers_ == nullptr ||
            !workers_->Post(std::move(task), app::TaskPriority::kSearch)) {
          FinishTimeout(batch);
        }
      });
}

void ChatLinkResolver::StartCid(const std::shared_ptr<Batch>& batch,
                                std::int64_t cid) {
  const std::int64_t dialog = -cid - common::kSupergroupMark;
  SendOff(
      batch, 1,
      [dialog] { return td_api::make_object<td_api::getChat>(dialog); },
      [this, batch, cid](td::ClientManager::Response response) {
        OnChannelChat(batch, cid, std::move(response));
      });
}

void ChatLinkResolver::OnChannelChat(const std::shared_ptr<Batch>& batch,
                                     std::int64_t cid,
                                     td::ClientManager::Response response) {
  std::int64_t supergroup_id = 0;
  bool confirmed = false;
  if (!IsError(response) && response.object->get_id() == td_api::chat::ID) {
    const auto chat =
        td_api::move_object_as<td_api::chat>(std::move(response.object));
    if (chat->type_ != nullptr &&
        chat->type_->get_id() == td_api::chatTypeSupergroup::ID) {
      supergroup_id =
          static_cast<const td_api::chatTypeSupergroup&>(*chat->type_)
              .supergroup_id_;
      confirmed = true;
    }
  }
  if (!confirmed) supergroup_id = cid;
  SendOff(
      batch, 1,
      [supergroup_id] {
        return td_api::make_object<td_api::getSupergroup>(supergroup_id);
      },
      [this, batch, cid, confirmed](td::ClientManager::Response info) {
        OnChannelInfo(batch, cid, std::move(info), confirmed);
      });
}

void ChatLinkResolver::OnChannelInfo(const std::shared_ptr<Batch>& batch,
                                     std::int64_t cid,
                                     td::ClientManager::Response response,
                                     bool confirmed) {
  std::optional<std::string> username;
  const bool is_group = !IsError(response) &&
                        response.object->get_id() == td_api::supergroup::ID;
  if (is_group) {
    username = PublicUsername(
        static_cast<const td_api::supergroup&>(*response.object).usernames_.get());
  }
  if (username.has_value()) {
    Accept(batch, cid, *username, "channel/supergroup username");
    return;
  }
  if (confirmed || is_group) {
    const std::string why = IsError(response) ? ErrorText(response)
                                              : "no public username";
    GiveUp(batch, cid,
           "channel/supergroup has no public username (" + why + ")");
    return;
  }
  ProbeUser(batch, cid);
}

void ChatLinkResolver::ProbeUser(const std::shared_ptr<Batch>& batch,
                                 std::int64_t cid) {
  SendOff(
      batch, 1,
      [cid] { return td_api::make_object<td_api::getUser>(cid); },
      [this, batch, cid](td::ClientManager::Response response) {
        OnUser(batch, cid, std::move(response), /*after_private=*/false);
      });
}

void ChatLinkResolver::OnUser(const std::shared_ptr<Batch>& batch,
                              std::int64_t cid,
                              td::ClientManager::Response response,
                              bool after_private) {
  if (!IsError(response) && response.object->get_id() == td_api::user::ID) {
    const auto& user = static_cast<const td_api::user&>(*response.object);
    if (const auto username = PublicUsername(user.usernames_.get())) {
      Accept(batch, cid, *username, "user/bot username");
      return;
    }
    GiveUp(batch, cid, after_private
                           ? "private chat loaded but user/bot has no public username"
                           : "user/bot has no public username");
    return;
  }
  const std::string why = ErrorText(response);
  if (after_private) {
    GiveUp(batch, cid,
           "private chat loaded but getUser still failed (" + why +
               "); searchPublicChat needs a username");
    return;
  }
  ProbePrivate(batch, cid, why);
}

void ChatLinkResolver::ProbePrivate(const std::shared_ptr<Batch>& batch,
                                    std::int64_t cid, std::string user_error) {
  // Private chat id == user id. This can load a user the account already
  // knows. searchPublicChat is not possible without a username.
  SendOff(
      batch, 1,
      [cid] { return td_api::make_object<td_api::getChat>(cid); },
      [this, batch, cid, user_error = std::move(user_error)](
          td::ClientManager::Response response) {
        OnPrivate(batch, cid, user_error, std::move(response));
      });
}

void ChatLinkResolver::OnPrivate(const std::shared_ptr<Batch>& batch,
                                 std::int64_t cid, std::string user_error,
                                 td::ClientManager::Response response) {
  if (!IsError(response) && response.object->get_id() == td_api::chat::ID) {
    SendOff(
        batch, 1,
        [cid] { return td_api::make_object<td_api::getUser>(cid); },
        [this, batch, cid](td::ClientManager::Response again) {
          OnUser(batch, cid, std::move(again), /*after_private=*/true);
        });
    return;
  }
  ProbeBasic(batch, cid,
             "getUser: " + user_error + "; getChat: " + ErrorText(response));
}

void ChatLinkResolver::ProbeBasic(const std::shared_ptr<Batch>& batch,
                                  std::int64_t cid, std::string detail) {
  SendOff(
      batch, 1,
      [cid] { return td_api::make_object<td_api::getBasicGroup>(cid); },
      [this, batch, cid, detail = std::move(detail)](
          td::ClientManager::Response response) {
        OnBasic(batch, cid, detail, std::move(response));
      });
}

void ChatLinkResolver::OnBasic(const std::shared_ptr<Batch>& batch,
                               std::int64_t cid, std::string detail,
                               td::ClientManager::Response response) {
  if (!IsError(response) &&
      response.object->get_id() == td_api::basicGroup::ID) {
    GiveUp(batch, cid, "basic group has no public link");
    return;
  }
  GiveUp(batch, cid,
         "user/bot unknown to this account (" + detail +
             "); searchPublicChat needs a username");
}

void ChatLinkResolver::Accept(const std::shared_ptr<Batch>& batch,
                              std::int64_t cid, const std::string& username,
                              const char* kind) {
  std::string url;
  bool from_config = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = by_username_.find(AsciiLower(username));
        it != by_username_.end()) {
      url = it->second;
      from_config = true;
    } else {
      url = NormalizeLinkBase("https://t.me/" + username);
    }
    if (!stopped_) Remember(cid, true, url);
  }
  if (from_config) {
    spdlog::info("search: cid {} -> {} (chat_links)", cid, url);
  } else {
    spdlog::info("search: cid {} -> {} ({})", cid, url, kind);
  }
  Complete(batch, cid, std::move(url));
}

void ChatLinkResolver::GiveUp(const std::shared_ptr<Batch>& batch,
                              std::int64_t cid, const std::string& reason) {
  spdlog::info("search: cid {} — {} — using https://t.me/c/{}/", cid, reason,
               cid);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopped_) Remember(cid, false, {});
  }
  Complete(batch, cid, std::nullopt);
}

void ChatLinkResolver::Complete(const std::shared_ptr<Batch>& batch,
                                std::int64_t cid,
                                std::optional<std::string> base) {
  Done done;
  BaseMap bases;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || batch->finished) return;
    bool found = false;
    for (auto it = batch->left.begin(); it != batch->left.end(); ++it) {
      if (*it == cid) {
        batch->left.erase(it);
        found = true;
        break;
      }
    }
    if (!found) return;
    batch->bases[cid] = std::move(base);
    if (!batch->left.empty()) return;
    batch->finished = true;
    done = std::move(batch->done);
    bases = std::move(batch->bases);
  }
  if (done) done(std::move(bases));
}

void ChatLinkResolver::FinishTimeout(const std::shared_ptr<Batch>& batch) {
  Done done;
  BaseMap bases;
  std::vector<std::int64_t> left;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || batch->finished) return;
    batch->finished = true;
    left = batch->left;
    batch->left.clear();
    for (const std::int64_t cid : left) batch->bases[cid] = std::nullopt;
    done = std::move(batch->done);
    bases = std::move(batch->bases);
  }
  for (const std::int64_t cid : left) {
    spdlog::info(
        "search: cid {} — username lookup timed out — using https://t.me/c/{}/",
        cid, cid);
  }
  if (done) done(std::move(bases));
}

void ChatLinkResolver::Arm(const std::shared_ptr<Batch>& batch) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    alarms_.push_back(
        Alarm{std::chrono::steady_clock::now() + options_.timeout, batch});
  }
  cv_.notify_all();
}

void ChatLinkResolver::TimerLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopped_) {
    if (alarms_.empty()) {
      cv_.wait(lock, [&] { return stopped_ || !alarms_.empty(); });
      continue;
    }
    auto next = alarms_.begin();
    for (auto it = alarms_.begin(); it != alarms_.end(); ++it) {
      if (it->when < next->when) next = it;
    }
    const auto when = next->when;
    if (cv_.wait_until(lock, when, [&] {
          return stopped_ || alarms_.empty() ||
                 [&] {
                   auto earliest = alarms_.begin();
                   for (auto it = alarms_.begin(); it != alarms_.end(); ++it) {
                     if (it->when < earliest->when) earliest = it;
                   }
                   return earliest->when < when;
                 }();
        })) {
      continue;
    }
    std::vector<std::shared_ptr<Batch>> due;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = alarms_.begin(); it != alarms_.end();) {
      if (it->when <= now) {
        if (auto batch = it->batch.lock()) due.push_back(std::move(batch));
        it = alarms_.erase(it);
      } else {
        ++it;
      }
    }
    lock.unlock();
    for (const auto& batch : due) {
      if (workers_ != nullptr &&
          workers_->Post([this, batch] { FinishTimeout(batch); },
                         app::TaskPriority::kSearch)) {
        continue;
      }
      FinishTimeout(batch);
    }
    lock.lock();
  }
}

}  // namespace tg::features::auto_search
