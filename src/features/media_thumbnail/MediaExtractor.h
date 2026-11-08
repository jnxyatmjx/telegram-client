#ifndef TG_FEATURES_MEDIA_THUMBNAIL_MEDIAEXTRACTOR_H_
#define TG_FEATURES_MEDIA_THUMBNAIL_MEDIAEXTRACTOR_H_

// MediaExtractor (v5 §2.2 step [2], §2.6 step 3): picks WHAT to download.
//  - photo: one size tier from photo.sizes — the largest entry whose largest
//    edge fits max_edge_px (320 by default); if none fits, the smallest
//    entry (still a thumbnail, never the full-size original).
//  - video / animation: the thumbnail ONLY, never the original video bytes.
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
};

struct ExtractedMedia {
  MediaKind kind = MediaKind::kPhoto;
  std::int64_t chat_id = 0;      // TDLib dialog id (used as-is)
  std::int64_t message_id = 0;   // TDLib message id (bare MTProto seq)
  std::string caption;           // memo source; "" when none (v5 §1.2)
  std::int32_t file_id = 0;      // td file id to download
  std::string file_unique_id;    // remoteFile.unique_id (Bot API analogue)
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::string media_name;        // video/animation file_name, for logs
};

class MediaExtractor {
 public:
  explicit MediaExtractor(const app::MediaThumbnailConfig& config);

  // nullopt when the message carries no photo/video/animation, or no usable
  // thumbnail tier. `reason` (optional) receives why.
  std::optional<ExtractedMedia> Extract(const td_api::message& message,
                                        std::string* reason = nullptr) const;

  // Photo tier selection: largest entry with max(w,h) <= max_edge_px,
  // else the smallest entry. nullptr when sizes is empty.
  static const td_api::photoSize* PickPhotoSize(const td_api::photo& photo,
                                                int max_edge_px);

 private:
  app::MediaThumbnailConfig config_;
};

}  // namespace tg::features::media_thumbnail

#endif  // TG_FEATURES_MEDIA_THUMBNAIL_MEDIAEXTRACTOR_H_
