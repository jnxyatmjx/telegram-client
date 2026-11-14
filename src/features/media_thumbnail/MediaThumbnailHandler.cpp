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
  // owner_id needs a bare chat id (int64 — ImageRequest.owner_id is int64
  // in the proto; Telegram user ids above 2^32 exist, e.g. 7974301646, so
  // this must NOT be the uint32-capped ToBareChatId, which is only for
  // SearchReq.oid), and the memo needs a configured KEYME (the server
  // expects ciphertext).
  if (!common::ToBareChatId64(media->chat_id)) {
    spdlog::error("process: chat_id {} is not a valid bare id — NOT "
                  "sending",
                  media->chat_id);
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
      },
      telegram::FileDownloader::kProcessPriority);
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
  // Every path below either stores the bytes in the Outbox BLOB or fails
  // before storing them; either way this consumer is done with the local
  // file and drops its claim (files_.Release). Release AFTER the Put —
  // deleting the cached file before the BLOB store would break a consumer
  // that has not read it yet (shared file_id) or leave nothing for a
  // never-stored retry.
  auto bytes = infra::ReadFileBytes(local_path, ctx.config.media_thumbnail.max_thumb_bytes);
  if (!bytes) {
    spdlog::error("process: cannot read downloaded media: {}", local_path);
    files_.Release(media.file_id);
    return;
  }

  // Protocol alignment (Telethon migration Chunk 1):
  //   owner_id: bare MTProto chat id as int64 (ImageRequest.owner_id is
  //     int64 in the proto; the uint32 cap applies only to SearchReq.oid).
  const auto bare = common::ToBareChatId64(media.chat_id);
  if (!bare) {
    spdlog::error("process: chat_id {} is not a valid bare id — NOT "
                  "sending",
                  media.chat_id);
    files_.Release(media.file_id);
    return;
  }
  //   image_id: str(message_id) — message-level dedup, Python protocol.
  const std::string image_id = common::MakeWireImageId(media.message_id);
  //   memo: AES-256-GCM ciphertext of the cleaned caption.
  auto memo = memo_cipher_.Encrypt(media.caption);
  if (!memo) {
    spdlog::error("process: memo encryption failed for message {} — NOT "
                  "sending (never send plaintext)", media.message_id);
    files_.Release(media.file_id);
    return;
  }

  spdlog::info("process: msg {} {}x{} {} bytes sha256={} owner={} (bare) "
               "memo_len={}", image_id, media.width, media.height,
               bytes->size(), infra::Sha256::Hex(*bytes), *bare,
               memo->size());

  ImageRequest request;
  request.set_image_id(image_id);
  request.set_image_data(std::move(*bytes));  // move, no extra copy (v5 §2.3)
  request.set_memo(std::move(*memo));
  request.set_owner_id(*bare);

  // Outbox first (v5 §2.2 step [4]): if the enqueue fails the record is
  // re-armed FAILED due-now and the retry thread replays it by image_id.
  //
  // next_retry_at is the SEND GRACE deadline, not an ack timeout (retry
  // design §3.1): it only has to carry the record until the write outcome
  // is known — MarkSent re-arms it to outbox.sent_timeout_ms when the
  // write lands (SENT), write-lost re-arms it to due-now when the request
  // is dropped. The old grpc.ack_timeout_ms here made every re-send ride
  // the full ack deadline, which dead-lettered slow-but-alive servers
  // (design G1).
  const std::int64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  // Re-process of an id still awaiting a reply: its previous send holds a
  // flow-control slot as SENT. The Put below supersedes that send (its
  // late reply, if any, hits MarkDone-false and releases nothing), so the
  // old slot returns here — otherwise the window slowly leaks shut.
  const auto existing = ctx.outbox.Get(image_id);
  if (ctx.flow_control != nullptr && existing.has_value() &&
      existing->state == infra::OutboxState::kSent) {
    ctx.flow_control->Release();
  }
  infra::OutboxRecord record;
  record.image_id = image_id;
  record.image_data = request.image_data();
  record.memo = request.memo();
  record.owner_id = request.owner_id();
  record.state = infra::OutboxState::kPending;
  record.next_retry_at_ms = now_ms + ctx.config.outbox.send_grace_ms;
  ctx.outbox.Put(record);

  // Bytes are safe in the Outbox BLOB — this consumer is done with the
  // local file. Release only NOW (deleteFile cache cleanup fires at the
  // last consumer): retry uses the BLOB, not the file, so this also holds
  // when the Enqueue below fails and the record stays PENDING.
  files_.Release(media.file_id);

  // Flow-control window (retry design §2(c)): cap sent-but-unreplied
  // process requests. When full, leave the record PENDING (due after the
  // grace) — the retry sweep picks it up once a slot frees. Never Enqueue
  // past the cap.
  if (ctx.flow_control != nullptr && !ctx.flow_control->TryAcquire()) {
    spdlog::warn("process: {} held in outbox — flow-control window full "
                 "({} in flight, cap {})", image_id,
                 ctx.flow_control->in_flight(), ctx.flow_control->window());
    return;
  }

  if (!ctx.process_images.Enqueue(std::move(request), kEnqueueTimeout)) {
    if (ctx.flow_control != nullptr) ctx.flow_control->Release();
    // Not fatal: FAILED due-now is replayed by the retry thread within
    // seconds, instead of waiting out the whole grace/ack deadline (G2).
    ctx.outbox.MarkFailed(image_id, "queue full", 0);
    spdlog::warn("process: {} write queue full; re-armed FAILED in outbox",
                 image_id);
    return;
  }
  spdlog::info("process: dispatched {}", image_id);
}

}  // namespace tg::features::media_thumbnail
