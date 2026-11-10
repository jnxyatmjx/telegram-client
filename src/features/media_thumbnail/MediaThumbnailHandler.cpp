#include "features/media_thumbnail/MediaThumbnailHandler.h"

#include <app/AppContext.h>
#include <common/Idempotency.h>
#include <common/Logger.h>
#include <infra/FileStore.h>
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
                                             telegram::FileDownloader& files,
                                             common::MemoCipher memo_cipher)
    : extractor_(std::move(extractor)),
      sender_(sender),
      files_(files),
      memo_cipher_(std::move(memo_cipher)) {}

void MediaThumbnailHandler::SendProcessImages(
    const td_api::message& target, app::AppContext& ctx) const {
  std::string why;
  const auto media = extractor_.Extract(target, &why);
  if (!media) {
    spdlog::error("process: extract failed: {}", why);
    return;
  }
  // Fail BEFORE downloading when the request could never be built anyway:
  // owner_id needs a bare chat id in uint32 range (v5 §1.3 hard rule), and
  // the memo needs a configured KEYME (the server expects ciphertext).
  if (!common::ToBareChatId(media->chat_id)) {
    spdlog::error("process: chat_id {} does not fit uint32 bare id — NOT "
                  "sending (no truncation)", media->chat_id);
    return;
  }
  if (!memo_cipher_.IsConfigured()) {
    spdlog::error("process: KEYME not configured — cannot encrypt memo, "
                  "NOT sending message {} (never send plaintext)",
                  media->message_id);
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
  const std::string image_id = common::MakeWireImageId(media.message_id);
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
    spdlog::error("process: cannot read downloaded media: {}", local_path);
    return;
  }

  // Protocol alignment (Telethon migration Chunk 1):
  //   owner_id: bare MTProto chat id, uint32 range-checked (the server and
  //     the reply {"cid"} both live in the bare-id world).
  const auto bare = common::ToBareChatId(media.chat_id);
  if (!bare) {
    spdlog::error("process: chat_id {} does not fit uint32 bare id — NOT "
                  "sending (no truncation)", media.chat_id);
    return;
  }
  //   image_id: str(message_id) — message-level dedup, Python protocol.
  const std::string image_id = common::MakeWireImageId(media.message_id);
  //   memo: AES-256-GCM ciphertext of the cleaned caption.
  auto memo = memo_cipher_.Encrypt(media.caption);
  if (!memo) {
    spdlog::error("process: memo encryption failed for message {} — NOT "
                  "sending (never send plaintext)", media.message_id);
    return;
  }

  spdlog::info("process: msg {} {}x{} {} bytes sha256={} owner={} (bare) "
               "memo_len={}", image_id, media.width, media.height,
               bytes->size(), infra::Sha256::Hex(*bytes), bare->oid,
               memo->size());

  ImageRequest request;
  request.set_image_id(image_id);
  request.set_image_data(std::move(*bytes));  // move, no extra copy (v5 §2.3)
  request.set_memo(std::move(*memo));
  request.set_owner_id(static_cast<std::int64_t>(bare->oid));

  // Outbox first (v5 §2.2 step [4]): if the enqueue fails the record stays
  // PENDING and the retry thread replays it by image_id (now the message id
  // string — message-level dedup, same replay semantics).
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
