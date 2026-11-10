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
//   [3] finish (worker): read bytes -> SHA-256 (log only) -> ImageRequest ->
//       Outbox(PENDING) -> enqueue ProcessImages stream (non-blocking try;
//       a full queue leaves the record PENDING for the retry thread, v5
//       §2.5 "write queue full").
// The outcome is logged in the finish stage; there is no return value to
// wait on — a message's slow download occupies no thread, so it cannot
// block another message's processing.
//
// Wire protocol (Telethon migration Chunk 1, Hong decisions 1-3/5 of
// 2026-11-10; Python app_client.py insert_task.sender 1383-1410):
//   image_id = str(message_id)          — decimal message id string; the
//                                         server dedupes per message.
//   memo     = AES-GCM(caption)         — MemoCipher ciphertext, never the
//                                         plaintext caption.
//   owner_id = bare MTProto chat id     — int64 via ToBareChatId64 (no
//                                         uint32 cap: Telegram user ids
//                                         above 2^32 exist; the uint32 cap
//                                         applies only to SearchReq.oid).
// A request that cannot be built (no bare id / memo cipher unconfigured /
// encryption failed) is refused loudly and nothing is sent.

#include <common/MemoCipher.h>
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
                        telegram::FileDownloader& files,
                        common::MemoCipher memo_cipher);

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
  common::MemoCipher memo_cipher_;
};

}  // namespace tg::features::media_thumbnail

#endif  // TG_FEATURES_MEDIA_THUMBNAIL_MEDIATHUMBNAILHANDLER_H_
