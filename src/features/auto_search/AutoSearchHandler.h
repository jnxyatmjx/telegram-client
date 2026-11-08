#ifndef TG_FEATURES_AUTO_SEARCH_AUTOSEARCHHANDLER_H_
#define TG_FEATURES_AUTO_SEARCH_AUTOSEARCHHANDLER_H_

// AutoSearchHandler (v5 §2.7, phase 2): a user posts an image/video/animation
// in an ALLOWLISTED chat -> triggers SearchImages ONLY. Never ProcessImages.
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

#include <app/Config.h>
#include <features/media_thumbnail/SearchHandler.h>
#include <telegram/UpdateHandler.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace tg::features::auto_search {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

class AutoSearchHandler : public telegram::IUpdateHandler {
 public:
  AutoSearchHandler(const app::AutoSearchConfig& config,
                    media_thumbnail::SearchHandler& search);

  bool CanHandle(
      const td_api::object_ptr<td_api::Update>& update) const override;
  void Handle(td_api::object_ptr<td_api::Update>& update,
              app::AppContext& ctx) override;
  const char* Name() const override { return "AutoSearchHandler"; }

  // True when (oid, id) was searched within the TTL window. Test hook.
  bool RecentlySearched(std::uint32_t oid, std::uint32_t id) const;

  static bool IsMediaMessage(const td_api::message& message);

 private:
  bool IsAllowlisted(std::int64_t chat_id) const;

  app::AutoSearchConfig config_;
  media_thumbnail::SearchHandler& search_;

  mutable std::mutex mutex_;
  // Mutable: RecentlySearched() is const but opportunistically sweeps
  // expired entries (a cache, guarded by the mutable mutex above).
  mutable std::unordered_map<std::uint64_t, std::chrono::steady_clock::time_point>
      recent_;
};

}  // namespace tg::features::auto_search

#endif  // TG_FEATURES_AUTO_SEARCH_AUTOSEARCHHANDLER_H_
