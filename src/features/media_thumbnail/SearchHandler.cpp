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
constexpr auto kDownloadTimeout = std::chrono::seconds(30);
constexpr auto kEnqueueTimeout = std::chrono::seconds(5);
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

PipelineResult SearchHandler::SendSearchImages(const td_api::message& target,
                                               app::AppContext& ctx) const {
  PipelineResult result;

  std::string why;
  const auto media = extractor_.Extract(target, &why);
  if (!media) {
    result.error = "extract failed: " + why;
    return result;
  }

  // Bare-ID conversion WITH range checks (v5 §1.3 hard rule 1).
  const auto oid = common::ToBareChatId(media->chat_id);
  if (!oid) {
    result.error = "chat_id " + std::to_string(media->chat_id) +
                   " does not fit uint32 bare id — NOT sending (no truncation)";
    spdlog::error("search: {}", result.error);
    return result;
  }
  const auto bare_message_id = common::ToBareMessageId(media->message_id);
  if (!bare_message_id) {
    result.error = "message_id " + std::to_string(media->message_id) +
                   " does not fit uint32 — NOT sending (no truncation)";
    spdlog::error("search: {}", result.error);
    return result;
  }

  auto future = files_.Download(media->file_id, kDownloadTimeout);
  std::string local_path;
  try {
    if (future.wait_for(kDownloadTimeout) != std::future_status::ready) {
      result.error = "download timed out after 30s";
      return result;
    }
    local_path = future.get();
  } catch (const std::exception& e) {
    result.error = std::string("download failed: ") + e.what();
    return result;
  }

  auto bytes =
      infra::ReadFileBytes(local_path, ctx.config.media_thumbnail.max_thumb_bytes);
  if (!bytes) {
    result.error = "cannot read downloaded thumbnail: " + local_path;
    return result;
  }

  const std::string image_id =
      common::MakeImageId(media->chat_id, media->message_id, media->file_unique_id);
  spdlog::info("search: oid={} id={} {} {} bytes sha256={}", oid->oid,
               *bare_message_id, image_id, bytes->size(),
               infra::Sha256::Hex(*bytes));

  SearchReq request;
  request.set_id(*bare_message_id);
  request.set_oid(oid->oid);
  request.set_image_data(std::move(*bytes));

  correlation_.Register(oid->oid, *bare_message_id,
                        {media->chat_id, media->message_id, image_id});

  if (!ctx.search_images.Enqueue(std::move(request), kEnqueueTimeout)) {
    // SearchReq is not in the Outbox: re-triggering the search is the
    // recovery path (v5 §2.4).
    result.image_id = image_id;
    result.error = "write queue full/timeout; SearchReq dropped (not replayed)";
    return result;
  }

  result.ok = true;
  result.image_id = image_id;
  return result;
}

}  // namespace tg::features::media_thumbnail
