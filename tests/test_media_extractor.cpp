// MediaExtractor (Telethon migration Chunk 1): photo takes the LARGEST tier
// (full image, no size cap), video/animation thumbnail-only, messageDocument
// by MIME type (image/* full file, video/* thumbnail, else rejected),
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

td_api::object_ptr<td_api::thumbnail> MakeThumbnail(int32_t file_id,
                                                    const std::string& uid,
                                                    int w = 320, int h = 180) {
  auto thumb = td_api::make_object<td_api::thumbnail>();
  thumb->format_ = td_api::make_object<td_api::thumbnailFormatJpeg>();
  thumb->width_ = w;
  thumb->height_ = h;
  auto file = td_api::make_object<td_api::file>();
  file->id_ = file_id;
  auto remote = td_api::make_object<td_api::remoteFile>();
  remote->unique_id_ = uid;
  file->remote_ = std::move(remote);
  thumb->file_ = std::move(file);
  return thumb;
}

td_api::object_ptr<td_api::document> MakeDocument(const std::string& mime) {
  auto document = td_api::make_object<td_api::document>();
  document->file_name_ = "attachment";
  document->mime_type_ = mime;
  return document;
}

td_api::object_ptr<td_api::message> MakeDocumentMessage(
    const std::string& mime, const std::string& caption = "") {
  auto message = td_api::make_object<td_api::message>();
  message->id_ = 800;
  message->chat_id_ = -1'001'234'567'890LL;
  auto content = td_api::make_object<td_api::messageDocument>();
  content->document_ = MakeDocument(mime);
  content->caption_ = MakeCaption(caption);
  message->content_ = std::move(content);
  return message;
}

}  // namespace

TEST(MediaExtractor, PhotoPicksLargestTierFullImage) {
  app::MediaThumbnailConfig config;
  media::MediaExtractor extractor(config);
  auto message = MakePhotoMessage();

  const auto media = extractor.Extract(*message);
  ASSERT_TRUE(media.has_value());
  EXPECT_EQ(media->kind, media::MediaKind::kPhoto);
  EXPECT_EQ(media->file_id, 4);  // 1280x1280 "y": the full image, no cap
  EXPECT_EQ(media->width, 1280);
  EXPECT_EQ(media->height, 1280);
  EXPECT_EQ(media->file_unique_id, "u-y");
  EXPECT_EQ(media->caption, "hello caption");  // memo source
  EXPECT_EQ(media->chat_id, -1'001'234'567'890LL);
  EXPECT_EQ(media->message_id, 777);
}

TEST(MediaExtractor, PhotoTieBreaksByLargerEdge) {
  app::MediaThumbnailConfig config;
  media::MediaExtractor extractor(config);

  auto message = MakePhotoMessage();
  auto& sizes = static_cast<td_api::messagePhoto*>(message->content_.get())
                    ->photo_->sizes_;
  sizes.push_back(MakeSize("z", 1600, 1024, 5, "u-z"));  // same area as 1280²

  const auto media = extractor.Extract(*message);
  ASSERT_TRUE(media.has_value());
  // Area tie (1600*1024 == 1280*1280): the larger max edge (1600) wins.
  EXPECT_EQ(media->file_id, 5);
  EXPECT_EQ(media->file_unique_id, "u-z");
}

TEST(MediaExtractor, PhotoFallsBackToFirstTierWithFile) {
  app::MediaThumbnailConfig config;
  media::MediaExtractor extractor(config);

  auto message = td_api::make_object<td_api::message>();
  message->id_ = 778;
  message->chat_id_ = -1'001'234'567'890LL;
  auto photo = td_api::make_object<td_api::photo>();
  // No dimensions known anywhere: the first entry carrying a file wins.
  photo->sizes_.push_back(MakeSize("i", 0, 0, 9, "u-i"));
  auto content = td_api::make_object<td_api::messagePhoto>();
  content->photo_ = std::move(photo);
  message->content_ = std::move(content);

  const auto media = extractor.Extract(*message);
  ASSERT_TRUE(media.has_value());
  EXPECT_EQ(media->file_id, 9);
  EXPECT_EQ(media->file_unique_id, "u-i");
}

TEST(MediaExtractor, PhotoSkipsFilelessTiers) {
  app::MediaThumbnailConfig config;
  media::MediaExtractor extractor(config);

  auto message = td_api::make_object<td_api::message>();
  message->id_ = 779;
  message->chat_id_ = -1'001'234'567'890LL;
  auto photo = td_api::make_object<td_api::photo>();
  // "y" is nominally largest but its file has not been filled in yet; the
  // extractor must not pick (or fail on) it.
  auto empty = td_api::make_object<td_api::photoSize>();
  empty->type_ = "y";
  empty->width_ = 1280;
  empty->height_ = 1280;  // no photo_
  photo->sizes_.push_back(std::move(empty));
  photo->sizes_.push_back(MakeSize("m", 320, 240, 2, "u-m"));
  auto content = td_api::make_object<td_api::messagePhoto>();
  content->photo_ = std::move(photo);
  message->content_ = std::move(content);

  const auto media = extractor.Extract(*message);
  ASSERT_TRUE(media.has_value());
  EXPECT_EQ(media->file_id, 2);
  EXPECT_EQ(media->file_unique_id, "u-m");
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
  video->thumbnail_ = MakeThumbnail(10, "u-thumb");
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
  EXPECT_EQ(media->media_name, "clip.mp4");
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
  animation->thumbnail_ = MakeThumbnail(21, "u-gif");
  auto content = td_api::make_object<td_api::messageAnimation>();
  content->animation_ = std::move(animation);
  message->content_ = std::move(content);

  const auto media = extractor.Extract(*message);
  ASSERT_TRUE(media.has_value());
  EXPECT_EQ(media->kind, media::MediaKind::kAnimation);
  EXPECT_EQ(media->file_id, 21);
  EXPECT_EQ(media->caption, "");  // no caption -> memo stays empty
}

TEST(MediaExtractor, ImageDocumentSendsFullFile) {
  app::MediaThumbnailConfig config;
  media::MediaExtractor extractor(config);

  auto message = MakeDocumentMessage("image/png", "doc caption");
  auto* content =
      static_cast<td_api::messageDocument*>(message->content_.get());
  auto file = td_api::make_object<td_api::file>();
  file->id_ = 31;
  auto remote = td_api::make_object<td_api::remoteFile>();
  remote->unique_id_ = "u-docimg";
  file->remote_ = std::move(remote);
  content->document_->document_ = std::move(file);

  const auto media = extractor.Extract(*message);
  ASSERT_TRUE(media.has_value());
  EXPECT_EQ(media->kind, media::MediaKind::kDocument);
  EXPECT_EQ(media->file_id, 31);  // the image itself, not a thumbnail
  EXPECT_EQ(media->file_unique_id, "u-docimg");
  EXPECT_EQ(media->caption, "doc caption");
  EXPECT_EQ(media->media_name, "attachment");
}

TEST(MediaExtractor, VideoDocumentSendsThumbnail) {
  app::MediaThumbnailConfig config;
  media::MediaExtractor extractor(config);

  auto message = MakeDocumentMessage("video/quicktime");
  auto* content =
      static_cast<td_api::messageDocument*>(message->content_.get());
  content->document_->thumbnail_ = MakeThumbnail(32, "u-docvid");
  auto video_file = td_api::make_object<td_api::file>();
  video_file->id_ = 555;  // the video bytes — must never be picked
  content->document_->document_ = std::move(video_file);

  const auto media = extractor.Extract(*message);
  ASSERT_TRUE(media.has_value());
  EXPECT_EQ(media->kind, media::MediaKind::kDocument);
  EXPECT_EQ(media->file_id, 32);  // thumbnail, not the video (555)
  EXPECT_EQ(media->file_unique_id, "u-docvid");
}

TEST(MediaExtractor, VideoDocumentWithoutThumbnailIsRejected) {
  app::MediaThumbnailConfig config;
  media::MediaExtractor extractor(config);

  auto message = MakeDocumentMessage("video/mp4");
  auto* content =
      static_cast<td_api::messageDocument*>(message->content_.get());
  auto video_file = td_api::make_object<td_api::file>();
  video_file->id_ = 556;
  content->document_->document_ = std::move(video_file);  // no thumbnail_

  std::string reason;
  EXPECT_FALSE(extractor.Extract(*message, &reason).has_value());
  EXPECT_NE(reason.find("thumbnail"), std::string::npos);
}

TEST(MediaExtractor, NonMediaDocumentMimeIsRejected) {
  app::MediaThumbnailConfig config;
  media::MediaExtractor extractor(config);

  auto message = MakeDocumentMessage("application/pdf");
  auto* content =
      static_cast<td_api::messageDocument*>(message->content_.get());
  auto file = td_api::make_object<td_api::file>();
  file->id_ = 40;
  content->document_->document_ = std::move(file);

  std::string reason;
  EXPECT_FALSE(extractor.Extract(*message, &reason).has_value());
  EXPECT_NE(reason.find("application/pdf"), std::string::npos);
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
