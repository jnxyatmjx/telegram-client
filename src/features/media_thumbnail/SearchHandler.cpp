#include "features/media_thumbnail/SearchHandler.h"

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
// Non-blocking Enqueue attempt in the finish stage (same reason as
// MediaThumbnailHandler: the worker must not wait on a full queue).
constexpr auto kEnqueueTimeout = std::chrono::milliseconds(0);
}  // namespace

void SearchCorrelationTable::Register(std::uint32_t oid, std::uint32_t id,
                                      Entry entry) {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_[Key(oid, id)] = std::move(entry);
}

std::optional<SearchCorrelationTable::Entry> SearchCorrelationTable::Lookup(
    std::uint32_t oid, std::uint32_t id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = entries_.find(Key(oid, id));
  if (it == entries_.end()) return std::nullopt;
  return it->second;
}

std::size_t SearchCorrelationTable::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return entries_.size();
}

SearchHandler::SearchHandler(MediaExtractor extractor,
                             telegram::ITdSender& sender,
                             telegram::FileDownloader& files)
    : extractor_(std::move(extractor)), sender_(sender), files_(files) {}

void SearchHandler::SendSearchImages(const td_api::message& target,
                                     app::AppContext& ctx) const {
  std::string why;
  const auto media = extractor_.Extract(target, &why);
  if (!media) {
    spdlog::error("search: extract failed: {}", why);
    return;
  }

  // Bare-ID conversion WITH range checks (v5 §1.3 hard rule 1). Pure
  // computation — safe to do here, before the download starts.
  const auto oid = common::ToBareChatId(media->chat_id);
  if (!oid) {
    spdlog::error("search: chat_id {} does not fit uint32 bare id — NOT "
                  "sending (no truncation)", media->chat_id);
    return;
  }
  const auto bare_message_id = common::ToBareMessageId(media->message_id);
  if (!bare_message_id) {
    spdlog::error("search: message_id {} does not fit uint32 — NOT sending "
                  "(no truncation)", media->message_id);
    return;
  }

  // Start the download and return immediately — the worker thread is free
  // for the next message while this one downloads.
  const ExtractedMedia media_copy = *media;
  files_.DownloadAsync(
      media->file_id,
      [this, media_copy, &ctx](telegram::FileDownloader::DownloadResult result) {
        OnDownloaded(media_copy, std::move(result), ctx);
      });
}

void SearchHandler::OnDownloaded(
    ExtractedMedia media, telegram::FileDownloader::DownloadResult result,
    app::AppContext& ctx) const {
  // Runs on the TDLib Receive thread (or inline, on the calling thread,
  // when the sender answers synchronously, e.g. in tests). Cheap only:
  // post the finish stage, do no I/O here.
  if (!result.error.empty()) {
    spdlog::error("search: download failed: {}", result.error);
    return;
  }
  const std::string image_id = common::MakeImageId(
      media.chat_id, media.message_id, media.file_unique_id);
  if (!ctx.workers.Post([this, media, path = result.local_path,
                         &ctx]() mutable {
        FinishSearch(std::move(media), std::move(path), ctx);
      })) {
    spdlog::error("search: worker pool rejected finish stage for {}",
                  image_id);
  }
}

void SearchHandler::FinishSearch(ExtractedMedia media, std::string local_path,
                                 app::AppContext& ctx) const {
  // Every path below either moves the bytes into the (enqueued or dropped)
  // SearchReq or fails before building it; either way this consumer is done
  // with the local file and drops its claim (files_.Release -> deleteFile
  // cache cleanup at the last consumer). SearchReq is NOT in the Outbox, so
  // there is nothing after FinishSearch that would read the file again.
  auto bytes =
      infra::ReadFileBytes(local_path, ctx.config.media_thumbnail.max_thumb_bytes);
  if (!bytes) {
    spdlog::error("search: cannot read downloaded thumbnail: {}", local_path);
    files_.Release(media.file_id);
    return;
  }

  // Bare IDs again (same conversion as the kick-off stage; both are pure).
  const auto oid = common::ToBareChatId(media.chat_id);
  const auto bare_message_id = common::ToBareMessageId(media.message_id);
  if (!oid || !bare_message_id) {
    files_.Release(media.file_id);
    return;  // already checked; cannot happen
  }

  const std::string image_id =
      common::MakeImageId(media.chat_id, media.message_id, media.file_unique_id);
  spdlog::info("search: oid={} id={} {} {} bytes sha256={}", oid->oid,
               *bare_message_id, image_id, bytes->size(),
               infra::Sha256::Hex(*bytes));

  SearchReq request;
  request.set_id(*bare_message_id);
  request.set_oid(oid->oid);
  request.set_image_data(std::move(*bytes));

  correlation_.Register(oid->oid, *bare_message_id,
                        {media.chat_id, media.message_id, image_id});

  // The bytes are now owned by the request — whether it is enqueued or
  // dropped (a dropped SearchReq is recovered by re-triggering, not by
  // re-reading the file), this consumer is done with the local file.
  const bool enqueued =
      ctx.search_images.Enqueue(std::move(request), kEnqueueTimeout);
  files_.Release(media.file_id);

  if (!enqueued) {
    // SearchReq is not in the Outbox: re-triggering the search is the
    // recovery path (v5 §2.4).
    spdlog::warn("search: {} write queue full; SearchReq dropped (not replayed)",
                 image_id);
    return;
  }
  spdlog::info("search: dispatched {}", image_id);
}

}  // namespace tg::features::media_thumbnail
