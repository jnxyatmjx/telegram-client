#ifndef TG_FEATURES_AUTO_SEARCH_CHATLINKRESOLVER_H_
#define TG_FEATURES_AUTO_SEARCH_CHATLINKRESOLVER_H_

// Link base for one search-hit cid, resolved before the reply is formatted.
//
// Order (app_bot_client build_message_link / whoami):
//   1. features.auto_search.chat_links (numeric key: -100 id or bare id)
//   2. chathis chat_url when it starts with http(s) — read only, never written
//   3. in-memory cache (positive, or a negative that has not expired)
//   4. TDLib: dialog id -(cid)-10^12 → getChat → getSupergroup →
//      active_usernames[0] or editable_username → https://t.me/<username>/
//      An @name chat_links entry wins over that public URL.
//   5. otherwise no base (the formatter uses https://t.me/c/<cid>/<mid>)
//
// TDLib callbacks only post to the worker pool. A batch of distinct cids
// finishes together, or after `timeout` (default 3s) with whatever is known.
// A timeout is not cached. A definitive miss is cached for negative_ttl.

#include <app/Config.h>
#include <telegram/TelegramClient.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace td_api = td::td_api;

namespace tg::app {
class WorkerPool;
}

namespace tg::telegram {
class FloodRetry;
}

namespace tg::features::auto_search {

struct ChatLinkResolveOptions {
  std::chrono::milliseconds timeout{3000};
  std::chrono::seconds positive_ttl{std::chrono::hours(24)};
  std::chrono::seconds negative_ttl{std::chrono::hours(1)};
};

class ChatLinkResolver {
 public:
  // nullopt = no http(s) base; the caller falls back to t.me/c/.
  using BaseMap = std::unordered_map<std::int64_t, std::optional<std::string>>;
  using Done = std::function<void(BaseMap)>;
  // Returns the stored chat_url (any string). Non-http values are ignored.
  using ChatUrlLookup = std::function<std::optional<std::string>(std::int64_t)>;

  ChatLinkResolver(const app::AutoSearchConfig& config,
                   telegram::ITdSender* sender, app::WorkerPool* workers,
                   telegram::FloodRetry* flood,
                   ChatLinkResolveOptions options = {});
  ~ChatLinkResolver();

  ChatLinkResolver(const ChatLinkResolver&) = delete;
  ChatLinkResolver& operator=(const ChatLinkResolver&) = delete;

  void SetChatUrlLookup(ChatUrlLookup lookup);

  // Test seam: cache expiry uses this clock. The resolve timeout uses
  // steady_clock so a short `timeout` still fires in real time.
  void SetClockForTest(
      std::function<std::chrono::steady_clock::time_point()> clock);

  // `done` runs on the caller when every cid is known without TDLib,
  // otherwise on the worker pool. Empty `cids` calls `done` immediately.
  void Resolve(std::vector<std::int64_t> cids, Done done);

  // Drops in-flight batches without calling their callbacks. Joins the
  // deadline thread. Safe to call more than once.
  void Shutdown();

 private:
  struct Batch;
  struct CacheEntry {
    bool positive = false;
    std::string url;
    std::chrono::steady_clock::time_point expires{};
  };

  using MakeRequest = std::function<td_api::object_ptr<td_api::Function>()>;
  using OnResponse = std::function<void(td::ClientManager::Response)>;

  std::chrono::steady_clock::time_point Now() const;
  std::optional<std::string> LookupReady(std::int64_t cid) const;
  bool NegativeHit(std::int64_t cid) const;
  void Remember(std::int64_t cid, bool positive, std::string url);

  void SendOff(const std::shared_ptr<Batch>& batch, int attempt,
               MakeRequest make_request, OnResponse on_response);
  void StartCid(const std::shared_ptr<Batch>& batch, std::int64_t cid);
  void OnChat(const std::shared_ptr<Batch>& batch, std::int64_t cid,
              td::ClientManager::Response response);
  void OnSupergroup(const std::shared_ptr<Batch>& batch, std::int64_t cid,
                    td::ClientManager::Response response);
  void Complete(const std::shared_ptr<Batch>& batch, std::int64_t cid,
                std::optional<std::string> base);
  void FinishTimeout(const std::shared_ptr<Batch>& batch);
  void Arm(const std::shared_ptr<Batch>& batch);
  void TimerLoop();

  ChatLinkResolveOptions options_;
  telegram::ITdSender* sender_ = nullptr;
  app::WorkerPool* workers_ = nullptr;
  telegram::FloodRetry* flood_ = nullptr;
  ChatUrlLookup chat_url_;
  std::function<std::chrono::steady_clock::time_point()> clock_;

  std::unordered_map<std::int64_t, std::string> by_id_;
  std::unordered_map<std::string, std::string> by_username_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::unordered_map<std::int64_t, CacheEntry> cache_;
  struct Alarm {
    std::chrono::steady_clock::time_point when;
    std::weak_ptr<Batch> batch;
  };
  std::vector<Alarm> alarms_;
  bool stopped_ = false;
  std::thread timer_;
};

}  // namespace tg::features::auto_search

#endif  // TG_FEATURES_AUTO_SEARCH_CHATLINKRESOLVER_H_
