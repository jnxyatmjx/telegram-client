// MediaChannelHandler (Telethon migration Chunk 2; Python handmedias
// 1255-1269): whitelist chats whose media flows through the pipeline
// AUTOMATICALLY (rpc "process" = the Python main chain, "search" =
// SearchImages). Disabled by default; bare-id chat matching; only photo /
// video / animation content claims an update.
#include <app/AppContext.h>
#include <app/Config.h>
#include <app/WorkerPool.h>
#include <features/media_channels/MediaChannelHandler.h>
#include <features/media_thumbnail/MediaExtractor.h>
#include <features/media_thumbnail/MediaThumbnailHandler.h>
#include <features/media_thumbnail/SearchHandler.h>
#include <infra/Outbox.h>
#include <telegram/TelegramClient.h>
#include <transport/AsyncStreamer.h>

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace td_api = td::td_api;

namespace app = tg::app;
namespace common = tg::common;
namespace media_channels = tg::features::media_channels;
namespace media = tg::features::media_thumbnail;
namespace transport = tg::transport;

namespace {

constexpr std::int64_t kBare = 1'234'567'890;
constexpr std::int64_t kSgDialog = -1'001'234'567'890LL;   // -1000-bare
constexpr std::int64_t kGroupDialog = -2'345'678'901;      // basic group
constexpr std::int64_t kOtherDialog = -1'009'999'999'999;  // another channel
constexpr std::int64_t kMessageId = 555;

class FakeTdSender : public tg::telegram::ITdSender {
 public:
  std::string thumbnail_path;

  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback) override {
    td::ClientManager::Response response;
    response.request_id = next_id_++;
    if (request->get_id() == td_api::downloadFile::ID) {
      auto file = td_api::make_object<td_api::file>();
      file->id_ = 4242;
      auto remote = td_api::make_object<td_api::remoteFile>();
      remote->unique_id_ = "u-mc";
      file->remote_ = std::move(remote);
      auto local = td_api::make_object<td_api::localFile>();
      local->path_ = thumbnail_path;
      local->is_downloading_completed_ = true;
      file->local_ = std::move(local);
      response.object = std::move(file);
    } else {
      response.object =
          td_api::make_object<td_api::error>(400, "unexpected");
    }
    callback(std::move(response));
    return static_cast<std::int64_t>(response.request_id);
  }

 private:
  std::uint64_t next_id_ = 1;
};

bool WaitFor(const std::function<bool()>& condition) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

td_api::object_ptr<td_api::Update> MakeUpdate(
    td_api::object_ptr<td_api::MessageContent> content,
    std::int64_t chat_id) {
  auto message = td_api::make_object<td_api::message>();
  message->id_ = kMessageId;
  message->chat_id_ = chat_id;
  message->content_ = std::move(content);
  auto update = td_api::make_object<td_api::updateNewMessage>();
  update->message_ = std::move(message);
  return update;
}

td_api::object_ptr<td_api::MessageContent> MakePhotoContent() {
  auto photo = td_api::make_object<td_api::photo>();
  auto size = td_api::make_object<td_api::photoSize>();
  size->type_ = "m";
  size->width_ = 320;
  size->height_ = 240;
  auto file = td_api::make_object<td_api::file>();
  file->id_ = 4242;
  auto remote = td_api::make_object<td_api::remoteFile>();
  remote->unique_id_ = "u-mc";
  file->remote_ = std::move(remote);
  auto local = td_api::make_object<td_api::localFile>();
  file->local_ = std::move(local);
  size->photo_ = std::move(file);
  photo->sizes_.push_back(std::move(size));
  auto content = td_api::make_object<td_api::messagePhoto>();
  content->photo_ = std::move(photo);
  return content;
}

td_api::object_ptr<td_api::MessageContent> MakeTextContent() {
  auto content = td_api::make_object<td_api::messageText>();
  auto formatted = td_api::make_object<td_api::formattedText>();
  formatted->text_ = "plain text";
  content->text_ = std::move(formatted);
  return content;
}

td_api::object_ptr<td_api::MessageContent> MakeVideoContent() {
  // TL: video is a media OBJECT; the message content wrapper is messageVideo.
  auto content = td_api::make_object<td_api::messageVideo>();
  auto video = td_api::make_object<td_api::video>();
  auto file = td_api::make_object<td_api::file>();
  file->id_ = 4242;
  auto remote = td_api::make_object<td_api::remoteFile>();
  remote->unique_id_ = "u-mc-v";
  file->remote_ = std::move(remote);
  auto local = td_api::make_object<td_api::localFile>();
  file->local_ = std::move(local);
  video->video_ = std::move(file);
  content->video_ = std::move(video);
  return content;
}

td_api::object_ptr<td_api::MessageContent> MakeAnimationContent() {
  auto content = td_api::make_object<td_api::messageAnimation>();
  auto animation = td_api::make_object<td_api::animation>();
  animation->animation_ = td_api::make_object<td_api::file>();
  content->animation_ = std::move(animation);
  return content;
}

}  // namespace

class MediaChannelsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    thumb_path_ =
        "/tmp/tg_client_mc_thumb_" + std::to_string(getpid()) + ".jpg";
    {
      std::ofstream out(thumb_path_, std::ios::binary);
      out << "\xFF\xD8" "fake-jpeg-thumb";
    }
    outbox_path_ =
        "/tmp/tg_client_mc_outbox_" + std::to_string(getpid()) + ".sqlite";
    std::remove(outbox_path_.c_str());

    config_.media_thumbnail.keyme = "mc-test-key";
    sender_.thumbnail_path = thumb_path_;
    files_ = std::make_unique<tg::telegram::FileDownloader>(sender_);
    workers_ = std::make_unique<app::WorkerPool>(4u);
    process_streamer_ = std::make_unique<
        transport::StreamerPool<ImageRequest, ImageReply>>(
        std::vector<transport::ProcessImagesStreamer::Options>{
            transport::ProcessImagesStreamer::Options{"ProcessImages"}});
    search_streamer_ = std::make_unique<
        transport::StreamerPool<SearchReq, SearchRsp>>(
        std::vector<transport::SearchImagesStreamer::Options>{
            transport::SearchImagesStreamer::Options{"SearchImages"}});
    outbox_ = std::make_unique<tg::infra::Outbox>(outbox_path_);
    ctx_ = std::make_unique<app::AppContext>(app::AppContext{
        *workers_, sender_, *files_, *process_streamer_, *search_streamer_,
        *outbox_, config_});
    process_handler_ = std::make_unique<media::MediaThumbnailHandler>(
        media::MediaExtractor(config_.media_thumbnail), sender_, *files_,
        common::MemoCipher(config_.media_thumbnail.keyme));
    search_handler_ = std::make_unique<media::SearchHandler>(
        media::MediaExtractor(config_.media_thumbnail), sender_, *files_);
  }

  media_channels::MediaChannelHandler MakeHandler() {
    return media_channels::MediaChannelHandler(channels_config_,
                                               *process_handler_,
                                               *search_handler_);
  }

  void TearDown() override {
    workers_.reset();
    ctx_.reset();
    outbox_.reset();
    search_streamer_.reset();
    process_streamer_.reset();
    search_handler_.reset();
    process_handler_.reset();
    files_.reset();
    std::remove(thumb_path_.c_str());
    std::remove(outbox_path_.c_str());
  }

  bool WaitForQueued(std::size_t count) {
    return WaitFor([this, count] {
      return search_streamer_->lane(0)->QueuedCount() >= count;
    });
  }

  std::string thumb_path_;
  std::string outbox_path_;
  app::Config config_;
  app::MediaChannelsConfig channels_config_;
  FakeTdSender sender_;
  std::unique_ptr<tg::telegram::FileDownloader> files_;
  std::unique_ptr<app::WorkerPool> workers_;
  std::unique_ptr<transport::StreamerPool<ImageRequest, ImageReply>>
      process_streamer_;
  std::unique_ptr<transport::StreamerPool<SearchReq, SearchRsp>>
      search_streamer_;
  std::unique_ptr<tg::infra::Outbox> outbox_;
  std::unique_ptr<app::AppContext> ctx_;
  std::unique_ptr<media::MediaThumbnailHandler> process_handler_;
  std::unique_ptr<media::SearchHandler> search_handler_;
};

TEST_F(MediaChannelsTest, DisabledByDefaultNeverClaims) {
  // channels_config_ keeps the default enabled=false — the opt-in behaviour
  // must not fire even for a whitelisted chat with media.
  channels_config_.chats = {kBare};
  const auto handler = MakeHandler();
  auto update = MakeUpdate(MakePhotoContent(), kSgDialog);
  EXPECT_FALSE(handler.CanHandle(update));
}

TEST_F(MediaChannelsTest, ClaimsOnlyWhitelistedChatsAndOnlyMedia) {
  channels_config_.enabled = true;
  channels_config_.chats = {kBare};
  const auto handler = MakeHandler();

  // Bare-id matching across the TDLib id forms of the SAME chat.
  EXPECT_TRUE(handler.IsWhitelisted(kSgDialog));    // supergroup form
  EXPECT_TRUE(handler.IsWhitelisted(kBare));        // already bare
  EXPECT_TRUE(handler.IsWhitelisted(-kBare));       // basic group form
  EXPECT_FALSE(handler.IsWhitelisted(kOtherDialog));
  EXPECT_FALSE(handler.IsWhitelisted(0));
  EXPECT_FALSE(handler.IsWhitelisted(-42'000'000'000'000'000LL));

  // Media content claims.
  auto photo = MakeUpdate(MakePhotoContent(), kSgDialog);
  EXPECT_TRUE(handler.CanHandle(photo));
  auto video = MakeUpdate(MakeVideoContent(), kSgDialog);
  EXPECT_TRUE(handler.CanHandle(video));
  auto animation = MakeUpdate(MakeAnimationContent(), kSgDialog);
  EXPECT_TRUE(handler.CanHandle(animation));

  // Text in a whitelisted chat, or media elsewhere: no claim.
  auto text = MakeUpdate(MakeTextContent(), kSgDialog);
  EXPECT_FALSE(handler.CanHandle(text));
  auto elsewhere = MakeUpdate(MakePhotoContent(), kOtherDialog);
  EXPECT_FALSE(handler.CanHandle(elsewhere));

  // Not an updateNewMessage.
  td_api::object_ptr<td_api::Update> service =
      td_api::make_object<td_api::updateChatReadInbox>();
  static_cast<td_api::updateChatReadInbox&>(*service).unread_count_ = 1;
  EXPECT_FALSE(handler.CanHandle(service));
}

TEST_F(MediaChannelsTest, ProcessRpcRunsRealtimePipeline) {
  // rpc "process" (default): the Python main chain — media goes through
  // extract -> download -> Outbox(image_id = str(message_id)) -> queue.
  channels_config_.enabled = true;
  channels_config_.chats = {kBare};
  auto handler = MakeHandler();  // Handle() is non-const
  auto update = MakeUpdate(MakePhotoContent(), kSgDialog);
  ASSERT_TRUE(handler.CanHandle(update));
  handler.Handle(update, *ctx_);

  ASSERT_TRUE(WaitFor([this] { return outbox_->PendingCount() == 1; }));
  const auto record = outbox_->Get("555");  // str(kMessageId)
  ASSERT_TRUE(record.has_value());
  EXPECT_FALSE(record->image_data.empty());
  EXPECT_EQ(process_streamer_->lane(0)->QueuedCount(), 1u);
  EXPECT_EQ(search_streamer_->lane(0)->QueuedCount(), 0u);
}

TEST_F(MediaChannelsTest, SearchRpcQueuesSearchRequest) {
  // rpc "search": the SAME message goes to SearchImages instead.
  channels_config_.enabled = true;
  channels_config_.chats = {kBare};
  channels_config_.rpc = "search";
  auto handler = MakeHandler();  // Handle() is non-const
  auto update = MakeUpdate(MakePhotoContent(), kSgDialog);
  ASSERT_TRUE(handler.CanHandle(update));
  handler.Handle(update, *ctx_);

  ASSERT_TRUE(WaitForQueued(1u));
  const auto request = search_streamer_->lane(0)->PeekFront();
  ASSERT_TRUE(request.has_value());
  // uint32 bare ids: supergroup -1001234567890 -> 1234567890, msg 555.
  EXPECT_EQ(request->oid(), 1234567890u);
  EXPECT_EQ(request->id(), 555u);
  EXPECT_FALSE(request->image_data().empty());
  // The correlation entry ties (oid, id) back to the message identity.
  const auto entry = search_handler_->correlation().Lookup(
      static_cast<std::uint32_t>(kBare),
      static_cast<std::uint32_t>(kMessageId));
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->chat_id, kSgDialog);
  EXPECT_EQ(entry->message_id, kMessageId);
  EXPECT_EQ(outbox_->PendingCount(), 0);  // SearchReq is outbox-less (v5 §2.4)
}
