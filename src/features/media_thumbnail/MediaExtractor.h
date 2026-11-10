#ifndef TG_FEATURES_MEDIA_THUMBNAIL_MEDIAEXTRACTOR_H_
#define TG_FEATURES_MEDIA_THUMBNAIL_MEDIAEXTRACTOR_H_

// MediaExtractor (v5 §2.2 step [2], §2.6 step 3): picks WHAT to download.
// Telethon migration Chunk 1 (Hong decision 5 of 2026-11-10) aligns the
// choice with the Python client (app_client.py download_task 996-1041):
//  - photo: the FULL image — the largest size tier (no 320px cap anymore;
//    Python sends download_media(file=bytes), the original).
//  - video: the thumbnail ONLY (TDLib exposes exactly one thumbnail per
//    video — the highest-quality one Telegram provides; Python's
//    download_media(thumb=-1) is the same pick), never the video bytes.
//  - document (messageDocument) by MIME type, like Python:
//      image/* -> the full image file
//      video/* -> the thumbnail (highest quality available)
//      else    -> rejected
//  - animation: thumbnail only (kept from the pre-migration behavior).
//
// Pure and side-effect free; exercised by tests/test_media_extractor.cpp.

#include <td/telegram/td_api.h>

#include <app/Config.h>
#include <optional>
#include <string>

namespace tg::features::media_thumbnail {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

enum class MediaKind {
  kPhoto,
  kVideo,
  kAnimation,
  kDocument,
};

struct ExtractedMedia {
  MediaKind kind = MediaKind::kPhoto;
  std::int64_t chat_id = 0;      // TDLib dialog id; owner_id goes out bare
  std::int64_t message_id = 0;   // TDLib message id; also the wire image_id
  std::string caption;           // memo source; "" when none (encrypted!)
  std::int32_t file_id = 0;      // td file id to download
  std::string file_unique_id;    // remoteFile.unique_id (logs/correlation)
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::string media_name;        // video/document file_name, for logs
};

class MediaExtractor {
 public:
  explicit MediaExtractor(const app::MediaThumbnailConfig& config);

  // nullopt when the message carries no supported media, or the chosen file
  // is unusable (no photo tier / video without thumbnail / document MIME
  // outside image|video). `reason` (optional) receives why.
  std::optional<ExtractedMedia> Extract(const td_api::message& message,
                                        std::string* reason = nullptr) const;

  // Photo tier selection: the LARGEST downloadable entry by pixel area (ties
  // broken by the larger max edge — that is the full-resolution original,
  // Telegram orders sizes small->large), falling back to the first entry that
  // has a file when no dimensions are known. nullptr when no entry has a file.
  static const td_api::photoSize* PickLargestPhotoSize(
      const td_api::photo& photo);

 private:
  app::MediaThumbnailConfig config_;
};

}  // namespace tg::features::media_thumbnail

#endif  // TG_FEATURES_MEDIA_THUMBNAIL_MEDIAEXTRACTOR_H_
