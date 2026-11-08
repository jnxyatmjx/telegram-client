#ifndef TG_FEATURES_MEDIA_THUMBNAIL_MEDIATHUMBNAILHANDLER_H_
#define TG_FEATURES_MEDIA_THUMBNAIL_MEDIATHUMBNAILHANDLER_H_

// MediaThumbnailHandler (v5 §2.2, §2.6 step 4): the /process execution chain.
// NOT an update handler — BotCommandHandler calls SendProcessImages with the
// target message after resolving it.
//
// Chain: extract tier -> download thumbnail (future, 30s) -> read bytes ->
// SHA-256 (log only) -> ImageRequest {image_id, image_data, memo=caption,
// owner_id=chat_id} -> Outbox(PENDING) -> enqueue ProcessImages stream.
// Enqueue failure leaves the Outbox record PENDING for the retry thread
// (v5 §2.5 "write queue full").

#include <features/media_thumbnail/MediaExtractor.h>
#include <telegram/FileDownloader.h>
#include <telegram/TelegramClient.h>

#include <string>

namespace tg::app {
struct AppContext;
}  // namespace tg::app

namespace tg::features::media_thumbnail {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

struct PipelineResult {
  bool ok = false;
  std::string image_id;
  std::string error;
};

class MediaThumbnailHandler {
 public:
  MediaThumbnailHandler(MediaExtractor extractor, telegram::ITdSender& sender,
                        telegram::FileDownloader& files);

  // Blocking (runs on a worker thread).
  PipelineResult SendProcessImages(const td_api::message& target,
                                   app::AppContext& ctx) const;

 private:
  MediaExtractor extractor_;
  telegram::ITdSender& sender_;
  telegram::FileDownloader& files_;
};

}  // namespace tg::features::media_thumbnail

#endif  // TG_FEATURES_MEDIA_THUMBNAIL_MEDIATHUMBNAILHANDLER_H_
