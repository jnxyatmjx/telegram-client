#ifndef TG_FEATURES_AUTO_SEARCH_AUTOSEARCHHANDLER_H_
#define TG_FEATURES_AUTO_SEARCH_AUTOSEARCHHANDLER_H_

// AutoSearchHandler (v5 §2.7, phase 2): a photo, video, animation, image/*
// document, or video/mp4 document posted in an ALLOWLISTED chat (including
// a forward of one of those) triggers SearchImages ONLY. Never ProcessImages.
// The SearchRsp is answered later, as a reply to that same message.
//
// DISABLED BY DEFAULT (features.auto_search.enabled: false). CanHandle short-
// circuits on the enabled flag, so with default config this handler claims
// nothing.
//
// Dedup: in-memory (oid, id) set with TTL (default 10 min) — the same media
// may also be picked up by a bot /search command; the pair is recorded when
// the request goes out and survives until TTL expiry. Process restart clears
// it, which is acceptable: a re-search is an idempotent read (v5 §2.7).
//
// Sender is deliberately NOT checked: channel posts may have no known sender
// (v5 §2.7 step 2).
//
// Allowlist entries are TDLib dialog ids and/or public usernames
// (@name, name, t.me/name). Numeric ids are active immediately.
// Usernames are resolved with searchPublicChat after authorization, on a
// worker thread (never the TDLib receive thread). FloodWait is retried
// through FloodRetry. Dedup and the SearchRsp reply path are unchanged.

#include <app/Config.h>
#include <features/media_thumbnail/SearchHandler.h>
#include <telegram/UpdateHandler.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tg::app {
class WorkerPool;
}

namespace tg::telegram {
class FloodRetry;
class ITdSender;
}

namespace tg::features::auto_search {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

class AllowlistDriver;

class AutoSearchHandler : public telegram::IUpdateHandler {
  friend class AllowlistDriver;

 public:
  AutoSearchHandler(const app::AutoSearchConfig& config,
                    media_thumbnail::SearchHandler& search);

  bool CanHandle(
      const td_api::object_ptr<td_api::Update>& update) const override;
  void Handle(td_api::object_ptr<td_api::Update>& update,
              app::AppContext& ctx) override;
  const char* Name() const override { return "AutoSearchHandler"; }
  bool PrefersSearchQueue() const override { return true; }

  // True when (oid, id) was searched within the TTL window. Test hook.
  bool RecentlySearched(std::uint32_t oid, std::uint32_t id) const;

  static bool IsMediaMessage(const td_api::message& message);

  // After authorization. Resolves username entries and logs every entry
  // (numeric ones via getChat). Returns immediately; work continues on
  // `workers`. No-op when auto-search is disabled.
  void ResolveAllowlist(telegram::ITdSender& sender,
                        telegram::FloodRetry* flood,
                        app::WorkerPool& workers);

  // Test hooks.
  std::vector<std::int64_t> allowlist() const;
  bool resolve_finished() const { return resolve_finished_.load(); }

  struct ResolveRecord {
    std::string raw;
    std::optional<std::int64_t> chat_id;
    bool added = false;
    bool member = false;
    // Empty on success. Otherwise "not found", "not a channel or
    // supergroup", "not a member", "getChat failed", "invalid".
    std::string failure;
  };
  std::vector<ResolveRecord> resolve_records() const;

 private:
  bool IsAllowlisted(std::int64_t chat_id) const;
  void AddAllow(std::int64_t chat_id);
  void NoteResolve(ResolveRecord record);
  void LogEffectiveAllowlist() const;

  app::AutoSearchConfig config_;
  media_thumbnail::SearchHandler& search_;

  mutable std::mutex mutex_;
  // Mutable: RecentlySearched() is const but opportunistically sweeps
  // expired entries (a cache, guarded by the mutable mutex above).
  mutable std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point>
      recent_;
  std::unordered_set<std::int64_t> allow_set_;
  std::vector<std::int64_t> allow_order_;
  std::vector<ResolveRecord> resolve_records_;
  std::atomic<bool> resolve_finished_{false};
};

}  // namespace tg::features::auto_search

#endif  // TG_FEATURES_AUTO_SEARCH_AUTOSEARCHHANDLER_H_
