// AutoSearchHandler (v5 §2.7): disabled by default; when enabled, an allow-
// listed chat's media triggers SearchImages ONLY (never ProcessImages, never
// Outbox); dedup within TTL.
#include <app/AppContext.h>
#include <app/WorkerPool.h>
#include <features/auto_search/AutoSearchHandler.h>
#include <features/media_thumbnail/SearchHandler.h>
#include <infra/Outbox.h>
#include <telegram/FileDownloader.h>
#include <telegram/FloodRetry.h>
#include <telegram/TelegramClient.h>
#include <transport/AsyncStreamer.h>

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <thread>
#include <vector>
#include <string>

namespace td_api = td::td_api;  // shorthand for td::td_api::*

namespace app = tg::app;
namespace auto_search = tg::features::auto_search;
namespace media = tg::features::media_thumbnail;
namespace transport = tg::transport;

namespace {

class FakeTdSender : public tg::telegram::ITdSender {
 public:
  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback) override {
    td::ClientManager::Response response;
    response.request_id = next_id_++;
    if (request->get_id() == td_api::downloadFile::ID) {
      auto file = td_api::make_object<td_api::file>();
      file->id_ = 4242;
      auto remote = td_api::make_object<td_api::remoteFile>();
      remote->unique_id_ = "u-auto";
      file->remote_ = std::move(remote);
      auto local = td_api::make_object<td_api::localFile>();
      local->path_ = thumbnail_path;
      local->is_downloading_completed_ = true;
      file->local_ = std::move(local);
      response.object = std::move(file);
    } else {
      response.object = td_api::make_object<td_api::error>(400, "unexpected");
    }
    callback(std::move(response));
    return static_cast<std::int64_t>(response.request_id);
  }

  std::string thumbnail_path;

 private:
  std::uint64_t next_id_ = 1;
};

td_api::object_ptr<td_api::photoSize> MakeSize(int32_t file_id) {
  auto size = td_api::make_object<td_api::photoSize>();
  size->type_ = "m";
  size->width_ = 320;
  size->height_ = 240;
  auto file = td_api::make_object<td_api::file>();
  file->id_ = file_id;
  auto remote = td_api::make_object<td_api::remoteFile>();
  remote->unique_id_ = "u-auto";
  file->remote_ = std::move(remote);
  auto local = td_api::make_object<td_api::localFile>();
  file->local_ = std::move(local);
  size->photo_ = std::move(file);
  return size;
}

// Photo message in the allowlisted chat, message id 555, no caption.
td_api::object_ptr<td_api::updateNewMessage> MakePhotoUpdate(
    std::int64_t chat_id) {
  auto message = td_api::make_object<td_api::message>();
  message->id_ = 555;
  message->chat_id_ = chat_id;
  auto photo = td_api::make_object<td_api::photo>();
  photo->sizes_.push_back(MakeSize(4242));
  auto content = td_api::make_object<td_api::messagePhoto>();
  content->photo_ = std::move(photo);
  message->content_ = std::move(content);
  auto update = td_api::make_object<td_api::updateNewMessage>();
  update->message_ = std::move(message);
  return update;
}

td_api::object_ptr<td_api::updateNewMessage> MakeTextUpdate(
    std::int64_t chat_id) {
  auto message = td_api::make_object<td_api::message>();
  message->id_ = 556;
  message->chat_id_ = chat_id;
  auto sender = td_api::make_object<td_api::messageSenderUser>();
  sender->user_id_ = 42;
  message->sender_id_ = std::move(sender);
  auto content = td_api::make_object<td_api::messageText>();
  auto text = td_api::make_object<td_api::formattedText>();
  text->text_ = "just chatting";
  content->text_ = std::move(text);
  message->content_ = std::move(content);
  auto update = td_api::make_object<td_api::updateNewMessage>();
  update->message_ = std::move(message);
  return update;
}

class AutoSearchTest : public ::testing::Test {
 protected:
  void SetUp() override {
    thumb_path_ = "/tmp/tg_client_auto_thumb_" + std::to_string(getpid()) + ".jpg";
    {
      std::ofstream out(thumb_path_, std::ios::binary);
      out << "\xFF\xD8" "fake-jpeg-thumb";
    }
    outbox_path_ =
        "/tmp/tg_client_auto_outbox_" + std::to_string(getpid()) + ".sqlite";
    std::remove(outbox_path_.c_str());

    sender_.thumbnail_path = thumb_path_;
    files_ = std::make_unique<tg::telegram::FileDownloader>(sender_);
    // The async pipeline posts continuations here; the tests wait for them.
    workers_ = std::make_unique<app::WorkerPool>(2u);
    search_handler_ = std::make_unique<media::SearchHandler>(
        media::MediaExtractor(config_.media_thumbnail), sender_, *files_);
    // Single-lane pools: the handler tests inspect lane 0's queue.
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
  }

  // Fresh handler per config; tests tweak config_ first.
  std::unique_ptr<auto_search::AutoSearchHandler> MakeHandler() {
    return std::make_unique<auto_search::AutoSearchHandler>(
        config_.auto_search, *search_handler_);
  }

  void TearDown() override {
    // Drain the pool first: queued finish stages use ctx_/outbox_/streamers.
    workers_.reset();
    ctx_.reset();
    outbox_.reset();
    search_streamer_.reset();
    process_streamer_.reset();
    files_.reset();
    search_handler_.reset();
    std::remove(thumb_path_.c_str());
    std::remove(outbox_path_.c_str());
  }

  // The pipeline is async: Handle returns after kicking it off, so the
  // tests poll for the queued request instead of asserting immediately.
  template <typename Pool>
  bool WaitForQueued(Pool& pool, std::size_t count) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      if (pool.lane(0)->QueuedCount() >= count) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

  std::string thumb_path_;
  std::string outbox_path_;
  app::Config config_;
  FakeTdSender sender_;
  std::unique_ptr<tg::telegram::FileDownloader> files_;
  std::unique_ptr<app::WorkerPool> workers_;
  std::unique_ptr<media::SearchHandler> search_handler_;
  std::unique_ptr<transport::StreamerPool<ImageRequest, ImageReply>>
      process_streamer_;
  std::unique_ptr<transport::StreamerPool<SearchReq, SearchRsp>>
      search_streamer_;
  std::unique_ptr<tg::infra::Outbox> outbox_;
  std::unique_ptr<app::AppContext> ctx_;
};

}  // namespace

TEST(AutoSearchDefaults, DisabledByDefault) {
  app::AutoSearchConfig config;  // untouched
  EXPECT_FALSE(config.enabled);
  EXPECT_TRUE(config.chats.empty());
  EXPECT_EQ(config.dedup_ttl_seconds, 600);  // 10 min (v5 §2.7)
}

TEST_F(AutoSearchTest, DisabledHandlerClaimsNothing) {
  // config_.auto_search left at defaults: enabled == false.
  auto handler = MakeHandler();
  td_api::object_ptr<td_api::Update> base = MakePhotoUpdate(-1'001'234'567'890LL);
  EXPECT_FALSE(handler->CanHandle(base));
}

TEST_F(AutoSearchTest, EnabledAllowlistedMediaTriggersSearchOnly) {
  config_.auto_search.enabled = true;
  config_.auto_search.chats = {-1'001'234'567'890LL};
  auto handler = MakeHandler();

  td_api::object_ptr<td_api::Update> base = MakePhotoUpdate(-1'001'234'567'890LL);
  ASSERT_TRUE(handler->CanHandle(base));
  handler->Handle(base, *ctx_);

  // SearchImages got the request...
  ASSERT_TRUE(WaitForQueued(*search_streamer_, 1u));
  EXPECT_EQ(search_streamer_->lane(0)->QueuedCount(), 1u);
  const auto request = search_streamer_->lane(0)->PeekFront();
  ASSERT_TRUE(request.has_value());
  // ...with uint32 bare ids (supergroup -1001234567890 -> 1234567890, msg 555)
  EXPECT_EQ(request->oid(), 1234567890u);
  EXPECT_EQ(request->id(), 555u);
  EXPECT_FALSE(request->image_data().empty());
  // ...and it is registered for SearchRsp correlation (v5 §2.4).
  EXPECT_TRUE(handler->RecentlySearched(1234567890u, 555u));

  // NEVER ProcessImages, NEVER Outbox (v5 §2.7 step 5).
  EXPECT_EQ(process_streamer_->lane(0)->QueuedCount(), 0u);
  EXPECT_EQ(outbox_->PendingCount(), 0);
}

TEST_F(AutoSearchTest, NonAllowlistedChatIgnored) {
  config_.auto_search.enabled = true;
  config_.auto_search.chats = {-1'001'234'567'890LL};
  auto handler = MakeHandler();

  td_api::object_ptr<td_api::Update> base = MakePhotoUpdate(-987654321LL);
  EXPECT_FALSE(handler->CanHandle(base));
  handler->Handle(base, *ctx_);
  EXPECT_EQ(search_streamer_->lane(0)->QueuedCount(), 0u);
}

TEST_F(AutoSearchTest, NonMediaMessageIgnored) {
  config_.auto_search.enabled = true;
  config_.auto_search.chats = {-1'001'234'567'890LL};
  auto handler = MakeHandler();

  td_api::object_ptr<td_api::Update> base = MakeTextUpdate(-1'001'234'567'890LL);
  EXPECT_FALSE(handler->CanHandle(base));
  handler->Handle(base, *ctx_);
  EXPECT_EQ(search_streamer_->lane(0)->QueuedCount(), 0u);
}

TEST_F(AutoSearchTest, DedupSkipsSecondHandleWithinTtl) {
  config_.auto_search.enabled = true;
  config_.auto_search.chats = {-1'001'234'567'890LL};
  auto handler = MakeHandler();

  td_api::object_ptr<td_api::Update> first = MakePhotoUpdate(-1'001'234'567'890LL);
  ASSERT_TRUE(handler->CanHandle(first));
  handler->Handle(first, *ctx_);
  ASSERT_TRUE(WaitForQueued(*search_streamer_, 1u));

  // Same (chat, message) again before TTL expiry: skipped.
  td_api::object_ptr<td_api::Update> second = MakePhotoUpdate(-1'001'234'567'890LL);
  ASSERT_TRUE(handler->CanHandle(second));  // claim ok; Handle dedupes
  handler->Handle(second, *ctx_);
  EXPECT_EQ(search_streamer_->lane(0)->QueuedCount(), 1u);
  EXPECT_TRUE(handler->RecentlySearched(1234567890u, 555u));

  // A different message id in the same chat is NOT deduped.
  td_api::object_ptr<td_api::Update> third = MakePhotoUpdate(-1'001'234'567'890LL);
  static_cast<td_api::updateNewMessage&>(*third).message_->id_ = 557;
  handler->Handle(third, *ctx_);
  ASSERT_TRUE(WaitForQueued(*search_streamer_, 2u));
  EXPECT_EQ(search_streamer_->lane(0)->QueuedCount(), 2u);
}

td_api::object_ptr<td_api::updateNewMessage> MakeDocumentUpdate(
    const std::string& mime) {
  auto message = td_api::make_object<td_api::message>();
  message->id_ = 900;
  message->chat_id_ = -1'001'234'567'890LL;
  auto document = td_api::make_object<td_api::document>();
  document->mime_type_ = mime;
  auto content = td_api::make_object<td_api::messageDocument>();
  content->document_ = std::move(document);
  message->content_ = std::move(content);
  auto update = td_api::make_object<td_api::updateNewMessage>();
  update->message_ = std::move(message);
  return update;
}

TEST_F(AutoSearchTest, ImageAndMp4DocumentsAreMedia) {
  config_.auto_search.enabled = true;
  config_.auto_search.chats = {-1'001'234'567'890LL};
  auto handler = MakeHandler();
  td_api::object_ptr<td_api::Update> jpeg = MakeDocumentUpdate("image/jpeg");
  td_api::object_ptr<td_api::Update> mp4 = MakeDocumentUpdate("video/mp4");
  td_api::object_ptr<td_api::Update> pdf = MakeDocumentUpdate("application/pdf");
  td_api::object_ptr<td_api::Update> webm = MakeDocumentUpdate("video/webm");
  EXPECT_TRUE(handler->CanHandle(jpeg));
  EXPECT_TRUE(handler->CanHandle(mp4));
  EXPECT_FALSE(handler->CanHandle(pdf));
  EXPECT_FALSE(handler->CanHandle(webm));
}

TEST_F(AutoSearchTest, EmptyAllowlistClaimsNothingEvenWhenEnabled) {
  config_.auto_search.enabled = true;
  config_.auto_search.chats.clear();
  auto handler = MakeHandler();
  td_api::object_ptr<td_api::Update> base = MakePhotoUpdate(-1'001'234'567'890LL);
  EXPECT_FALSE(handler->CanHandle(base));
}

namespace {

class ResolveFakeSender : public tg::telegram::ITdSender {
 public:
  int search_floods_left = 0;

  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback) override {
    td::ClientManager::Response response;
    response.request_id = ++next_id_;
    const auto id = request->get_id();
    if (id == td_api::getMe::ID) {
      auto user = td_api::make_object<td_api::user>();
      user->id_ = 42;
      response.object = std::move(user);
    } else if (id == td_api::searchPublicChat::ID) {
      const auto& search =
          static_cast<const td_api::searchPublicChat&>(*request);
      if (search_floods_left > 0) {
        --search_floods_left;
        response.object = td_api::make_object<td_api::error>(
            429, "Too Many Requests: retry after 0");
      } else if (search.username_ == "missing") {
        response.object = td_api::make_object<td_api::error>(
            400, "USERNAME_NOT_OCCUPIED");
      } else if (search.username_ == "bob") {
        response.object = MakeChat(700, "Bob", /*channel=*/false);
      } else if (search.username_ == "publicxme") {
        response.object = MakeChat(-100111, "Public X", /*channel=*/true);
      } else if (search.username_ == "leftchat") {
        response.object = MakeChat(-100222, "Left Channel", /*channel=*/true);
      } else {
        response.object =
            td_api::make_object<td_api::error>(400, "unexpected username");
      }
    } else if (id == td_api::getChat::ID) {
      const auto chat_id = static_cast<const td_api::getChat&>(*request).chat_id_;
      response.object = MakeChat(chat_id, "Configured", /*channel=*/true);
    } else if (id == td_api::getChatMember::ID) {
      const auto chat_id =
          static_cast<const td_api::getChatMember&>(*request).chat_id_;
      auto member = td_api::make_object<td_api::chatMember>();
      if (chat_id == -100222) {
        member->status_ = td_api::make_object<td_api::chatMemberStatusLeft>();
      } else {
        member->status_ = td_api::make_object<td_api::chatMemberStatusMember>();
      }
      response.object = std::move(member);
    } else {
      response.object = td_api::make_object<td_api::error>(400, "unexpected");
    }
    callback(std::move(response));
    return response.request_id;
  }

 private:
  static td_api::object_ptr<td_api::chat> MakeChat(std::int64_t id,
                                                  const std::string& title,
                                                  bool channel) {
    auto chat = td_api::make_object<td_api::chat>();
    chat->id_ = id;
    chat->title_ = title;
    if (channel) {
      chat->type_ =
          td_api::make_object<td_api::chatTypeSupergroup>(id, /*is_channel=*/true);
    } else {
      chat->type_ = td_api::make_object<td_api::chatTypePrivate>(id);
    }
    return chat;
  }

  std::int64_t next_id_ = 0;
};

bool WaitUntil(const std::function<bool()>& pred) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

const auto_search::AutoSearchHandler::ResolveRecord* FindRecord(
    const std::vector<auto_search::AutoSearchHandler::ResolveRecord>& records,
    const std::string& raw) {
  for (const auto& record : records) {
    if (record.raw == raw) return &record;
  }
  return nullptr;
}

}  // namespace

TEST(AutoSearchResolve, UsernamesIdsMembershipAndClaim) {
  ResolveFakeSender sender;
  sender.search_floods_left = 1;  // first searchPublicChat is a 429
  app::WorkerPool workers(2);
  tg::telegram::FloodRetry flood(tg::telegram::FloodRetry::Options{
      /*pad_seconds=*/0, /*fallback_seconds=*/0, /*max_attempts=*/3});

  app::AutoSearchConfig config;
  config.enabled = true;
  app::AutoSearchChatSpec name;
  name.raw = "@publicxme";
  name.username = "publicxme";
  app::AutoSearchChatSpec missing;
  missing.raw = "missing";
  missing.username = "missing";
  app::AutoSearchChatSpec left;
  left.raw = "@leftchat";
  left.username = "leftchat";
  app::AutoSearchChatSpec user;
  user.raw = "https://t.me/bob";
  user.username = "bob";
  app::AutoSearchChatSpec numeric;
  numeric.raw = "-100333";
  numeric.numeric = true;
  numeric.dialog_id = -100333;
  config.chat_specs = {name, missing, left, user, numeric};
  config.chats = {-100333};

  auto files = std::make_unique<tg::telegram::FileDownloader>(sender);
  media::SearchHandler search(media::MediaExtractor(app::MediaThumbnailConfig{}),
                             sender, *files);
  auto_search::AutoSearchHandler handler(config, search);

  // Username is not allowlisted until searchPublicChat returns.
  td_api::object_ptr<td_api::Update> early = MakePhotoUpdate(-100111);
  EXPECT_FALSE(handler.CanHandle(early));

  handler.ResolveAllowlist(sender, &flood, workers);
  ASSERT_TRUE(WaitUntil([&] { return handler.resolve_finished(); }));

  const auto records = handler.resolve_records();
  const auto* pub = FindRecord(records, "@publicxme");
  ASSERT_NE(pub, nullptr);
  EXPECT_TRUE(pub->added);
  EXPECT_TRUE(pub->member);
  EXPECT_EQ(pub->chat_id, -100111);
  EXPECT_TRUE(pub->failure.empty());

  const auto* gone = FindRecord(records, "missing");
  ASSERT_NE(gone, nullptr);
  EXPECT_FALSE(gone->added);
  EXPECT_EQ(gone->failure, "not found");

  const auto* left_rec = FindRecord(records, "@leftchat");
  ASSERT_NE(left_rec, nullptr);
  EXPECT_TRUE(left_rec->added);
  EXPECT_FALSE(left_rec->member);
  EXPECT_EQ(left_rec->failure, "not a member");
  EXPECT_EQ(left_rec->chat_id, -100222);

  const auto* bob = FindRecord(records, "https://t.me/bob");
  ASSERT_NE(bob, nullptr);
  EXPECT_FALSE(bob->added);
  EXPECT_EQ(bob->failure, "not a channel or supergroup");

  const auto* num = FindRecord(records, "-100333");
  ASSERT_NE(num, nullptr);
  EXPECT_TRUE(num->added);
  EXPECT_TRUE(num->member);
  EXPECT_EQ(num->chat_id, -100333);

  const auto ids = handler.allowlist();
  EXPECT_NE(std::find(ids.begin(), ids.end(), -100111), ids.end());
  EXPECT_NE(std::find(ids.begin(), ids.end(), -100222), ids.end());
  EXPECT_NE(std::find(ids.begin(), ids.end(), -100333), ids.end());
  EXPECT_EQ(std::find(ids.begin(), ids.end(), 700), ids.end());

  td_api::object_ptr<td_api::Update> resolved = MakePhotoUpdate(-100111);
  EXPECT_TRUE(handler.CanHandle(resolved));
  td_api::object_ptr<td_api::Update> other = MakePhotoUpdate(-100999);
  EXPECT_FALSE(handler.CanHandle(other));
}
