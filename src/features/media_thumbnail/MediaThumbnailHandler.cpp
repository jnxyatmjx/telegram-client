#include "features/media_thumbnail/MediaThumbnailHandler.h"

#include <app/AppContext.h>
#include <common/Idempotency.h>
#include <common/Logger.h>
#include <infra/FileStore.h>
#include <infra/Outbox.h>
#include <infra/Sha256.h>
#include <transport/AsyncStreamer.h>

#include <chrono>
#include <utility>

namespace tg::features::media_thumbnail {

namespace {
// The finish stage must not block a worker: a single non-blocking Enqueue
// attempt; a full queue leaves the Outbox record PENDING for the retry
// thread (v5 §2.5). No timeout wait — the queue is normally far from full.
constexpr auto kEnqueueTimeout = std::chrono::milliseconds(0);
}  // namespace

MediaThumbnailHandler::MediaThumbnailHandler(MediaExtractor extractor,
                                             telegram::ITdSender& sender,
                                             telegram::FileDownloader& files)
    : extractor_(std::move(extractor)), sender_(sender), files_(files) {}

void MediaThumbnailHandler::SendProcessImages(
    const td_api::message& target, app::AppContext& ctx) const {
  std::string why;
  const auto media = extractor_.Extract(target, &why);
  if (!media) {
    spdlog::error("process: extract failed: {}", why);
    return;
  }

  // [2] start the download and return immediately — the worker thread is
  // free for the next message while this one downloads.
  const ExtractedMedia media_copy = *media;
  files_.DownloadAsync(
      media->file_id,
      [this, media_copy, &ctx](telegram::FileDownloader::DownloadResult result) {
        OnDownloaded(media_copy, std::move(result), ctx);
      });
}

void MediaThumbnailHandler::OnDownloaded(
    ExtractedMedia media, telegram::FileDownloader::DownloadResult result,
    app::AppContext& ctx) const {
  // Runs on the TDLib Receive thread (or inline, on the calling thread,
  // when the sender answers synchronously, e.g. in tests). Cheap only:
  // post the finish stage, do no I/O here.
  if (!result.error.empty()) {
    spdlog::error("process: download failed: {}", result.error);
    return;
  }
  const std::string image_id = common::MakeImageId(
      media.chat_id, media.message_id, media.file_unique_id);
  if (!ctx.workers.Post([this, media, path = result.local_path,
                         &ctx]() mutable {
        FinishProcess(std::move(media), std::move(path), ctx);
      })) {
    spdlog::error("process: worker pool rejected finish stage for {}",
                  image_id);
  }
}

void MediaThumbnailHandler::FinishProcess(ExtractedMedia media,
                                          std::string local_path,
                                          app::AppContext& ctx) const {
  // [3] read bytes, hash (log only), build the request.
  auto bytes = infra::ReadFileBytes(local_path, ctx.config.media_thumbnail.max_thumb_bytes);
  if (!bytes) {
    spdlog::error("process: cannot read downloaded thumbnail: {}", local_path);
    return;
  }

  const std::string image_id =
      common::MakeImageId(media.chat_id, media.message_id, media.file_unique_id);
  spdlog::info("process: {} {}x{} {} bytes sha256={} memo_len={}",
               image_id, media.width, media.height, bytes->size(),
               infra::Sha256::Hex(*bytes),
               media.caption.size());

  ImageRequest request;
  request.set_image_id(image_id);
  request.set_image_data(std::move(*bytes));  // move, no extra copy (v5 §2.3)
  request.set_memo(media.caption);           // caption ONLY (v5 §1.2)
  request.set_owner_id(media.chat_id);       // int64, no conversion

  // Outbox first (v5 §2.2 step [4]): if the enqueue fails the record stays
  // PENDING and the retry thread replays it by image_id.
  infra::OutboxRecord record;
  record.image_id = image_id;
  record.image_data = request.image_data();
  record.memo = request.memo();
  record.owner_id = request.owner_id();
  record.state = infra::OutboxState::kPending;
  ctx.outbox.Put(record);

  if (!ctx.process_images.Enqueue(std::move(request), kEnqueueTimeout)) {
    // Not fatal: PENDING record is picked up by the Outbox retry thread.
    spdlog::warn("process: {} write queue full; left PENDING in outbox",
                 image_id);
    return;
  }
  spdlog::info("process: dispatched {}", image_id);
}

}  // namespace tg::features::media_thumbnail
