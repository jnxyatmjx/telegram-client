#ifndef TG_FEATURES_MEDIA_THUMBNAIL_SEARCHHANDLER_H_
#define TG_FEATURES_MEDIA_THUMBNAIL_SEARCHHANDLER_H_

// SearchHandler (v5 §2.2, §2.4, §2.6/§2.7 step 4): the /search (and
// auto-search) execution chain. Same extraction/download path as
// MediaThumbnailHandler, but produces a SearchReq and ONLY a SearchReq —
// never ProcessImages.
//
// SearchReq.id / SearchReq.oid are uint32 MTProto bare IDs (server side is
// Telethon): the dialog-id -> bare-id conversion happens here, with range
// checks. Out-of-range = loud error + NOT sent, never truncated (v5 §1.3).
//
// SearchReq is deliberately NOT written to the Outbox (v5 §2.4): the
// correlation table mapping (oid, id) -> message identity is memory-only and
// dies with the process; a new search is simply triggered again by the bot.

#include <features/media_thumbnail/MediaExtractor.h>
#include <features/media_thumbnail/MediaThumbnailHandler.h>
#include <telegram/FileDownloader.h>
#include <telegram/TelegramClient.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace tg::app {
struct AppContext;
}  // namespace tg::app

namespace tg::features::media_thumbnail {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

// In-memory (oid, id) -> message identity table (v5 §2.4, §3.3). Workers
// register when sending; the CQ thread looks up when a SearchRsp arrives.
class SearchCorrelationTable {
 public:
  struct Entry {
    std::int64_t chat_id = 0;
    std::int64_t message_id = 0;
    std::string image_id;
  };

  static std::uint64_t Key(std::uint32_t oid, std::uint32_t id) {
    return (static_cast<std::uint64_t>(oid) << 32) | id;
  }

  void Register(std::uint32_t oid, std::uint32_t id, Entry entry);
  std::optional<Entry> Lookup(std::uint32_t oid, std::uint32_t id) const;
  std::size_t size() const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::uint64_t, Entry> entries_;
};

class SearchHandler {
 public:
  SearchHandler(MediaExtractor extractor, telegram::ITdSender& sender,
                telegram::FileDownloader& files);

  // Blocking (runs on a worker thread).
  PipelineResult SendSearchImages(const td_api::message& target,
                                  app::AppContext& ctx) const;

  SearchCorrelationTable& correlation() { return correlation_; }

 private:
  MediaExtractor extractor_;
  telegram::ITdSender& sender_;
  telegram::FileDownloader& files_;
  // Mutable: SendSearchImages() is const but registers the (oid, id) entry
  // in the correlation table (internal bookkeeping, mutex-guarded inside).
  mutable SearchCorrelationTable correlation_;
};

}  // namespace tg::features::media_thumbnail

#endif  // TG_FEATURES_MEDIA_THUMBNAIL_SEARCHHANDLER_H_
