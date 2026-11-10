#include "features/media_thumbnail/MediaExtractor.h"

#include <common/Logger.h>

#include <algorithm>
#include <cstring>

namespace tg::features::media_thumbnail {
namespace {

// remoteFile.unique_id is TDLib's analogue of the Bot API file_unique_id.
// Fall back to the (session-scoped) file id string when remote info is not
// populated yet — the identity stays well-formed, logging just gets weaker.
std::string UniqueIdOf(const td_api::file& file) {
  if (file.remote_ != nullptr && !file.remote_->unique_id_.empty()) {
    return file.remote_->unique_id_;
  }
  return std::to_string(file.id_);
}

std::string CaptionOf(const td_api::formattedText& text) { return text.text_; }

bool HasPhotoFile(const td_api::photoSize& size) {
  return size.photo_ != nullptr;
}

}  // namespace

MediaExtractor::MediaExtractor(const app::MediaThumbnailConfig& config)
    : config_(config) {}

const td_api::photoSize* MediaExtractor::PickLargestPhotoSize(
    const td_api::photo& photo) {
  const td_api::photoSize* largest = nullptr;
  std::int64_t largest_area = -1;
  int largest_edge = -1;
  const td_api::photoSize* first_with_file = nullptr;
  for (const auto& size : photo.sizes_) {
    if (size == nullptr) continue;
    if (first_with_file == nullptr && HasPhotoFile(*size)) {
      first_with_file = size.get();
    }
    // Only tiers that actually carry a file are candidates; a dimension-only
    // entry must not win over a downloadable smaller tier.
    if (!HasPhotoFile(*size)) continue;
    const std::int64_t area =
        static_cast<std::int64_t>(size->width_) * size->height_;
    const int edge = std::max(size->width_, size->height_);
    if (area > largest_area || (area == largest_area && edge > largest_edge)) {
      largest = size.get();
      largest_area = area;
      largest_edge = edge;
    }
  }
  // The largest tier (full image); if no entry carries dimensions, the first
  // entry with a file at all.
  return largest != nullptr ? largest : first_with_file;
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
      // Full image (Hong decision 5): the largest tier, no size cap —
      // Python download_media(file=bytes) sends the original.
      const auto* content = static_cast<const td_api::messagePhoto*>(
          message.content_.get());
      if (content->photo_ == nullptr || content->photo_->sizes_.empty()) {
        return fail("photo message has no sizes");
      }
      const td_api::photoSize* size =
          PickLargestPhotoSize(*content->photo_);
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
      // Highest-quality thumbnail only (TDLib has exactly one per video —
      // the pick Telethon's thumb=-1 makes), never the video bytes.
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
    case td_api::messageDocument::ID: {
      // Document branch (Python download_task 1026-1038): image/* sends the
      // full image file, video/* sends the thumbnail; other MIMEs rejected.
      const auto* content = static_cast<const td_api::messageDocument*>(
          message.content_.get());
      if (content->document_ == nullptr) {
        return fail("document message has no document");
      }
      const std::string& mime = content->document_->mime_type_;
      media.media_name = content->document_->file_name_;
      media.caption =
          content->caption_ != nullptr ? CaptionOf(*content->caption_) : "";
      const auto starts_with = [&mime](const char* prefix) {
        return mime.compare(0, std::strlen(prefix), prefix) == 0;
      };
      if (starts_with("image/")) {
        if (content->document_->document_ == nullptr) {
          return fail("image document has no file");
        }
        media.kind = MediaKind::kDocument;
        media.file_id = content->document_->document_->id_;
        media.file_unique_id = UniqueIdOf(*content->document_->document_);
      } else if (starts_with("video/")) {
        if (content->document_->thumbnail_ == nullptr ||
            content->document_->thumbnail_->file_ == nullptr) {
          return fail("video document has no thumbnail (never send the video)");
        }
        media.kind = MediaKind::kDocument;
        media.file_id = content->document_->thumbnail_->file_->id_;
        media.file_unique_id =
            UniqueIdOf(*content->document_->thumbnail_->file_);
        media.width = content->document_->thumbnail_->width_;
        media.height = content->document_->thumbnail_->height_;
      } else {
        return fail("unsupported document mime type: " + mime);
      }
      break;
    }
    default:
      return fail("unsupported message content type " +
                  std::to_string(message.content_->get_id()));
  }

  return media;
}

}  // namespace tg::features::media_thumbnail
