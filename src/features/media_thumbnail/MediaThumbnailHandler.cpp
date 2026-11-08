#include "features/media_thumbnail/MediaThumbnailHandler.h"

#include <app/AppContext.h>
#include <common/Idempotency.h>
#include <common/Logger.h>
#include <infra/FileStore.h>
#include <infra/Outbox.h>
#include <infra/Sha256.h>
#include <transport/AsyncStreamer.h>

#include <chrono>

namespace tg::features::media_thumbnail {

namespace {
constexpr auto kDownloadTimeout = std::chrono::seconds(30);   // v5 §2.2
constexpr auto kEnqueueTimeout = std::chrono::seconds(5);
}  // namespace

MediaThumbnailHandler::MediaThumbnailHandler(MediaExtractor extractor,
                                             telegram::ITdSender& sender,
                                             telegram::FileDownloader& files)
    : extractor_(std::move(extractor)), sender_(sender), files_(files) {}

PipelineResult MediaThumbnailHandler::SendProcessImages(
    const td_api::message& target, app::AppContext& ctx) const {
  PipelineResult result;

  std::string why;
  const auto media = extractor_.Extract(target, &why);
  if (!media) {
    result.error = "extract failed: " + why;
    return result;
  }

  // [2] download the selected thumbnail tier
  std::string local_path;
  try {
    auto future = files_.Download(media->file_id, kDownloadTimeout);
    if (future.wait_for(kDownloadTimeout) != std::future_status::ready) {
      result.error = "download timed out after 30s";
      return result;
    }
    local_path = future.get();
  } catch (const std::exception& e) {
    result.error = std::string("download failed: ") + e.what();
    return result;
  }

  // [4] read bytes, hash (log only), build the request
  auto bytes = infra::ReadFileBytes(local_path, ctx.config.media_thumbnail.max_thumb_bytes);
  if (!bytes) {
    result.error = "cannot read downloaded thumbnail: " + local_path;
    return result;
  }

  const std::string image_id =
      common::MakeImageId(media->chat_id, media->message_id, media->file_unique_id);
  spdlog::info("process: {} {}x{} {} bytes sha256={} memo_len={}",
               image_id, media->width, media->height, bytes->size(),
               infra::Sha256::Hex(*bytes),
               media->caption.size());

  ImageRequest request;
  request.set_image_id(image_id);
  request.set_image_data(std::move(*bytes));  // move, no extra copy (v5 §2.3)
  request.set_memo(media->caption);           // caption ONLY (v5 §1.2)
  request.set_owner_id(media->chat_id);       // int64, no conversion

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
    result.image_id = image_id;
    result.error = "write queue full/timeout; left PENDING in outbox";
    return result;
  }

  result.ok = true;
  result.image_id = image_id;
  return result;
}

}  // namespace tg::features::media_thumbnail
