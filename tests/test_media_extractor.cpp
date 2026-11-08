// MediaExtractor: photo size tier selection, video/animation thumbnail-only,
// unsupported content rejected.
#include <features/media_thumbnail/MediaExtractor.h>

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

namespace td_api = td::td_api;  // shorthand for td::td_api::*

namespace media = tg::features::media_thumbnail;
namespace app = tg::app;

namespace {

td_api::object_ptr<td_api::photoSize> MakeSize(const std::string& type, int w,
                                               int h, int32_t file_id,
                                               const std::string& uid) {
  auto size = td_api::make_object<td_api::photoSize>();
  size->type_ = type;
  size->width_ = w;
  size->height_ = h;
  auto file = td_api::make_object<td_api::file>();
  file->id_ = file_id;
  auto remote = td_api::make_object<td_api::remoteFile>();
  remote->id_ = "remote-" + std::to_string(file_id);
  remote->unique_id_ = uid;
  file->remote_ = std::move(remote);
  auto local = td_api::make_object<td_api::localFile>();
  file->local_ = std::move(local);
  size->photo_ = std::move(file);
  return size;
}

td_api::object_ptr<td_api::photo> MakePhotoWithSizes() {
  auto photo = td_api::make_object<td_api::photo>();
  photo->has_stickers_ = false;
  photo->sizes_.push_back(MakeSize("s", 100, 100, 1, "u-s"));
  photo->sizes_.push_back(MakeSize("m", 320, 240, 2, "u-m"));
  photo->sizes_.push_back(MakeSize("x", 640, 640, 3, "u-x"));
  photo->sizes_.push_back(MakeSize("y", 1280, 1280, 4, "u-y"));
  return photo;
}

td_api::object_ptr<td_api::formattedText> MakeCaption(const std::string& text) {
  auto caption = td_api::make_object<td_api::formattedText>();
  caption->text_ = text;
  return caption;
}

td_api::object_ptr<td_api::message> MakePhotoMessage() {
  auto message = td_api::make_object<td_api::message>();
  message->id_ = 777;
  message->chat_id_ = -1'001'234'567'890LL;
  auto content = td_api::make_object<td_api::messagePhoto>();
  content->photo_ = MakePhotoWithSizes();
  content->caption_ = MakeCaption("hello caption");
  message->content_ = std::move(content);
  return message;
}

}  // namespace

TEST(MediaExtractor, PhotoPicksLargestTierWithinMaxEdge) {
  app::MediaThumbnailConfig config;      // max_edge_px = 320
  media::MediaExtractor extractor(config);
  auto message = MakePhotoMessage();

  const auto media = extractor.Extract(*message);
  ASSERT_TRUE(media.has_value());
  EXPECT_EQ(media->kind, media::MediaKind::kPhoto);
  EXPECT_EQ(media->file_id, 2);          // 320x240: largest with max edge <= 320
  EXPECT_EQ(media->width, 320);
  EXPECT_EQ(media->height, 240);
  EXPECT_EQ(media->file_unique_id, "u-m");
  EXPECT_EQ(media->caption, "hello caption");  // memo source
  EXPECT_EQ(media->chat_id, -1'001'234'567'890LL);
  EXPECT_EQ(media->message_id, 777);
}

TEST(MediaExtractor, PhotoFallsBackToSmallestWhenAllTooBig) {
  app::MediaThumbnailConfig config;
  config.max_edge_px = 50;  // nothing fits
  media::MediaExtractor extractor(config);
  auto message = MakePhotoMessage();

  const auto media = extractor.Extract(*message);
  ASSERT_TRUE(media.has_value());
  EXPECT_EQ(media->file_id, 1);  // smallest tier, still a thumbnail
}

TEST(MediaExtractor, VideoUsesThumbnailOnly) {
  app::MediaThumbnailConfig config;
  media::MediaExtractor extractor(config);

  auto message = td_api::make_object<td_api::message>();
  message->id_ = 55;
  message->chat_id_ = -987654321;
  auto video = td_api::make_object<td_api::video>();
  video->duration_ = 60;
  video->width_ = 1920;
  video->height_ = 1080;
  video->file_name_ = "clip.mp4";
  auto thumb = td_api::make_object<td_api::thumbnail>();
  thumb->format_ = td_api::make_object<td_api::thumbnailFormatJpeg>();
  thumb->width_ = 320;
  thumb->height_ = 180;
  auto thumb_file = td_api::make_object<td_api::file>();
  thumb_file->id_ = 10;
  auto remote = td_api::make_object<td_api::remoteFile>();
  remote->unique_id_ = "u-thumb";
  thumb_file->remote_ = std::move(remote);
  thumb->file_ = std::move(thumb_file);
  video->thumbnail_ = std::move(thumb);
  auto video_file = td_api::make_object<td_api::file>();
  video_file->id_ = 99;  // the original video — must never be picked
  video->video_ = std::move(video_file);

  auto content = td_api::make_object<td_api::messageVideo>();
  content->video_ = std::move(video);
  content->caption_ = MakeCaption("vid caption");
  message->content_ = std::move(content);

  const auto media = extractor.Extract(*message);
  ASSERT_TRUE(media.has_value());
  EXPECT_EQ(media->kind, media::MediaKind::kVideo);
  EXPECT_EQ(media->file_id, 10);  // thumbnail, not the video file (99)
  EXPECT_EQ(media->file_unique_id, "u-thumb");
  EXPECT_EQ(media->caption, "vid caption");
}

TEST(MediaExtractor, VideoWithoutThumbnailIsRejected) {
  app::MediaThumbnailConfig config;
  media::MediaExtractor extractor(config);

  auto message = td_api::make_object<td_api::message>();
  auto video = td_api::make_object<td_api::video>();
  auto content = td_api::make_object<td_api::messageVideo>();
  content->video_ = std::move(video);
  message->content_ = std::move(content);

  std::string reason;
  EXPECT_FALSE(extractor.Extract(*message, &reason).has_value());
  EXPECT_FALSE(reason.empty());
}

TEST(MediaExtractor, AnimationUsesThumbnail) {
  app::MediaThumbnailConfig config;
  media::MediaExtractor extractor(config);

  auto message = td_api::make_object<td_api::message>();
  auto animation = td_api::make_object<td_api::animation>();
  auto thumb = td_api::make_object<td_api::thumbnail>();
  auto thumb_file = td_api::make_object<td_api::file>();
  thumb_file->id_ = 21;
  auto remote = td_api::make_object<td_api::remoteFile>();
  remote->unique_id_ = "u-gif";
  thumb_file->remote_ = std::move(remote);
  thumb->file_ = std::move(thumb_file);
  animation->thumbnail_ = std::move(thumb);
  auto content = td_api::make_object<td_api::messageAnimation>();
  content->animation_ = std::move(animation);
  message->content_ = std::move(content);

  const auto media = extractor.Extract(*message);
  ASSERT_TRUE(media.has_value());
  EXPECT_EQ(media->kind, media::MediaKind::kAnimation);
  EXPECT_EQ(media->file_id, 21);
  EXPECT_EQ(media->caption, "");  // no caption -> memo stays empty
}

TEST(MediaExtractor, UnsupportedContentIsRejected) {
  app::MediaThumbnailConfig config;
  media::MediaExtractor extractor(config);

  auto message = td_api::make_object<td_api::message>();
  auto content = td_api::make_object<td_api::messageText>();
  content->text_ = MakeCaption("/process");
  message->content_ = std::move(content);

  std::string reason;
  EXPECT_FALSE(extractor.Extract(*message, &reason).has_value());
}
