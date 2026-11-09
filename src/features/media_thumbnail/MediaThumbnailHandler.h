#ifndef TG_FEATURES_MEDIA_THUMBNAIL_MEDIATHUMBNAILHANDLER_H_
#define TG_FEATURES_MEDIA_THUMBNAIL_MEDIATHUMBNAILHANDLER_H_

// MediaThumbnailHandler (v5 §2.2, §2.6 step 4): the /process execution
// chain. NOT an update handler — BotCommandHandler kicks it off with the
// target message after resolving it.
//
// Fully async (2026-11-09): SendProcessImages only EXTRACTS the media and
// starts the download, then returns — no worker thread ever blocks on a
// download. The chain is stages, each posted to the worker pool:
//   [1] extract tier (worker) -> DownloadAsync (returns immediately)
//   [2] download done (Receive thread callback) -> post finish stage
//   [3] finish (worker): read bytes -> SHA-256 (log only) -> ImageRequest
//       {image_id, image_data, memo=caption, owner_id=chat_id} ->
//       Outbox(PENDING) -> enqueue ProcessImages stream (non-blocking try;
//       a full queue leaves the record PENDING for the retry thread, v5
//       §2.5 "write queue full").
// The outcome is logged in the finish stage; there is no return value to
// wait on — a message's slow download occupies no thread, so it cannot
// block another message's processing.

#include <features/media_thumbnail/MediaExtractor.h>
#include <telegram/FileDownloader.h>
#include <telegram/TelegramClient.h>

#include <string>

namespace tg::app {
struct AppContext;
}  // namespace tg::app

namespace tg::features::media_thumbnail {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

class MediaThumbnailHandler {
 public:
  MediaThumbnailHandler(MediaExtractor extractor, telegram::ITdSender& sender,
                        telegram::FileDownloader& files);

  // Async kick-off (runs on a worker thread): extracts + starts the
  // download, then returns immediately. Never blocks.
  void SendProcessImages(const td_api::message& target,
                         app::AppContext& ctx) const;

 private:
  // Receive-thread callback: download finished (or failed). Posts the
  // finish stage to the pool; does no I/O itself.
  void OnDownloaded(ExtractedMedia media,
                    telegram::FileDownloader::DownloadResult result,
                    app::AppContext& ctx) const;
  // Worker-thread finish stage: read bytes -> request -> Outbox -> Enqueue.
  void FinishProcess(ExtractedMedia media, std::string local_path,
                     app::AppContext& ctx) const;

  MediaExtractor extractor_;
  telegram::ITdSender& sender_;
  telegram::FileDownloader& files_;
};

}  // namespace tg::features::media_thumbnail

#endif  // TG_FEATURES_MEDIA_THUMBNAIL_MEDIATHUMBNAILHANDLER_H_
