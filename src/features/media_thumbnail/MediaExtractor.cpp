#include "features/media_thumbnail/MediaExtractor.h"

#include <common/Logger.h>

namespace tg::features::media_thumbnail {
namespace {

// remoteFile.unique_id is TDLib's analogue of the Bot API file_unique_id.
// Fall back to the (session-scoped) file id string when remote info is not
// populated yet — image_id stays well-formed, dedup just gets a weaker key.
std::string UniqueIdOf(const td_api::file& file) {
  if (file.remote_ != nullptr && !file.remote_->unique_id_.empty()) {
    return file.remote_->unique_id_;
  }
  return std::to_string(file.id_);
}

std::string CaptionOf(const td_api::formattedText& text) { return text.text_; }

}  // namespace

MediaExtractor::MediaExtractor(const app::MediaThumbnailConfig& config)
    : config_(config) {}

const td_api::photoSize* MediaExtractor::PickPhotoSize(
    const td_api::photo& photo, int max_edge_px) {
  const td_api::photoSize* best_fit = nullptr;
  const td_api::photoSize* smallest = nullptr;
  for (const auto& size : photo.sizes_) {
    if (size == nullptr) continue;
    if (smallest == nullptr) {
      smallest = size.get();
    } else {
      const int smallest_edge = std::min(smallest->width_, smallest->height_);
      const int this_edge = std::min(size->width_, size->height_);
      if (this_edge < smallest_edge) smallest = size.get();
    }
    const int largest_edge = std::max(size->width_, size->height_);
    if (largest_edge <= max_edge_px) {
      if (best_fit == nullptr ||
          largest_edge >
              std::max(best_fit->width_, best_fit->height_)) {
        best_fit = size.get();
      }
    }
  }
  // Preferred: tier within the limit. Fallback: smallest known tier so we
  // still send a thumbnail when every size exceeds max_edge_px.
  return best_fit != nullptr ? best_fit : smallest;
}

std::optional<ExtractedMedia> MediaExtractor::Extract(
    const td_api::message& message, std::string* reason) const {
  auto fail = [&reason](std::string why) {
    if (reason != nullptr) *reason = std::move(why);
    return std::nullopt;
  };
  if (message.content_ == nullptr) {
    return fail("message has no content");
  }

  ExtractedMedia media;
  media.chat_id = message.chat_id_;
  media.message_id = message.id_;

  switch (message.content_->get_id()) {
    case td_api::messagePhoto::ID: {
      // Read-only: the message is borrowed, so downcast without moving.
      const auto* content = static_cast<const td_api::messagePhoto*>(
          message.content_.get());
      if (content->photo_ == nullptr || content->photo_->sizes_.empty()) {
        return fail("photo message has no sizes");
      }
      const td_api::photoSize* size =
          PickPhotoSize(*content->photo_, config_.max_edge_px);
      if (size == nullptr || size->photo_ == nullptr) {
        return fail("photo message has no usable size tier");
      }
      media.kind = MediaKind::kPhoto;
      media.file_id = size->photo_->id_;
      media.file_unique_id = UniqueIdOf(*size->photo_);
      media.width = size->width_;
      media.height = size->height_;
      media.caption =
          content->caption_ != nullptr ? CaptionOf(*content->caption_) : "";
      break;
    }
    case td_api::messageVideo::ID: {
      const auto* content = static_cast<const td_api::messageVideo*>(
          message.content_.get());
      if (content->video_ == nullptr || content->video_->thumbnail_ == nullptr ||
          content->video_->thumbnail_->file_ == nullptr) {
        return fail("video message has no thumbnail (never send the video)");
      }
      media.kind = MediaKind::kVideo;
      media.file_id = content->video_->thumbnail_->file_->id_;
      media.file_unique_id = UniqueIdOf(*content->video_->thumbnail_->file_);
      media.width = content->video_->thumbnail_->width_;
      media.height = content->video_->thumbnail_->height_;
      media.media_name = content->video_->file_name_;
      media.caption =
          content->caption_ != nullptr ? CaptionOf(*content->caption_) : "";
      break;
    }
    case td_api::messageAnimation::ID: {
      const auto* content = static_cast<const td_api::messageAnimation*>(
          message.content_.get());
      if (content->animation_ == nullptr ||
          content->animation_->thumbnail_ == nullptr ||
          content->animation_->thumbnail_->file_ == nullptr) {
        return fail("animation message has no thumbnail");
      }
      media.kind = MediaKind::kAnimation;
      media.file_id = content->animation_->thumbnail_->file_->id_;
      media.file_unique_id = UniqueIdOf(*content->animation_->thumbnail_->file_);
      media.width = content->animation_->thumbnail_->width_;
      media.height = content->animation_->thumbnail_->height_;
      media.media_name = content->animation_->file_name_;
      media.caption =
          content->caption_ != nullptr ? CaptionOf(*content->caption_) : "";
      break;
    }
    default:
      return fail("unsupported message content type " +
                  std::to_string(message.content_->get_id()));
  }

  return media;
}

}  // namespace tg::features::media_thumbnail
