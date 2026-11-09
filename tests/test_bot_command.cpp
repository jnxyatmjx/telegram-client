// BotCommandHandler + CommandParser (v5 §2.6, mode a): only the configured
// bot's commands are accepted; other senders / unknown commands ignored;
// target resolution via reply-to and via "chat_id message_id" args.
#include <app/AppContext.h>
#include <app/WorkerPool.h>
#include <features/bot_command/BotCommandHandler.h>
#include <features/bot_command/CommandParser.h>
#include <features/media_thumbnail/MediaExtractor.h>
#include <infra/FileStore.h>
#include <infra/Outbox.h>
#include <telegram/FileDownloader.h>
#include <telegram/TelegramClient.h>
#include <transport/AsyncStreamer.h>

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>
#include <string>

namespace td_api = td::td_api;  // shorthand for td::td_api::*

namespace app = tg::app;
namespace bot_command = tg::features::bot_command;
namespace media = tg::features::media_thumbnail;
namespace transport = tg::transport;

namespace {

// Synchronous fake TDLib: answers getMessage and downloadFile inline.
// For the async regression test it can also DELAY a downloadFile response:
// the response is delivered later, from a background thread, which is how
// a slow TDLib download really arrives (Send itself returns at once).
class FakeTdSender : public tg::telegram::ITdSender {
 public:
  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback)
      override {
    const std::string type = TypeOf(request);
    ++sent_[type];
    td::ClientManager::Response response;
    response.request_id = next_id_++;
    const std::int32_t file_id =
        request->get_id() == td_api::downloadFile::ID
            ? static_cast<td_api::downloadFile*>(request.get())->file_id_
            : 0;
    response.object = Answer(std::move(request));
    if (file_id != 0 && slow_file_ids_.count(file_id) > 0) {
      // Slow download: deliver the response later, from another thread.
      slow_threads_.emplace_back(
          [cb = std::move(callback), resp = std::move(response),
           delay = slow_delay_ms_]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            cb(std::move(resp));
          });
    } else {
      callback(std::move(response));
    }
    return static_cast<std::int64_t>(response.request_id);
  }

  int sent_count(const std::string& type) const {
    const auto it = sent_.find(type);
    return it != sent_.end() ? it->second : 0;
  }

  void set_target_message(td_api::object_ptr<td_api::message> message) {
    target_message_ = std::move(message);
  }

  // Extra targets, looked up by message id for getMessage.
  void add_target_message(td_api::object_ptr<td_api::message> message) {
    targets_[message->id_] = std::move(message);
  }

  // file_ids whose downloadFile response is delayed (background thread).
  void set_slow_downloads(std::set<std::int32_t> file_ids, int delay_ms) {
    slow_file_ids_ = std::move(file_ids);
    slow_delay_ms_ = delay_ms;
  }

  void join_slow_threads() {
    for (auto& t : slow_threads_) {
      if (t.joinable()) t.join();
    }
    slow_threads_.clear();
  }

  std::string thumbnail_path;
  std::int64_t target_chat_id_ = 0;
  std::int64_t target_message_id_ = 0;

 private:
  static std::string TypeOf(td_api::object_ptr<td_api::Function>& request) {
    switch (request->get_id()) {
      case td_api::getMessage::ID: return "getMessage";
      case td_api::downloadFile::ID: return "downloadFile";
      default: return "other";
    }
  }

  td_api::object_ptr<td_api::Object> Answer(
      td_api::object_ptr<td_api::Function>&& request) {
    switch (request->get_id()) {
      case td_api::getMessage::ID: {
        // One consumer per test: hand the canned message over (moved).
        const auto message_id =
            static_cast<td_api::getMessage*>(request.get())->message_id_;
        const auto it = targets_.find(message_id);
        if (it != targets_.end()) {
          auto message = std::move(it->second);
          targets_.erase(it);
          return message;
        }
        if (target_message_ == nullptr) {
          return td_api::make_object<td_api::error>(404, "Message not found");
        }
        return std::move(target_message_);
      }
      case td_api::downloadFile::ID: {
        auto file = td_api::make_object<td_api::file>();
        file->id_ = 4242;
        auto remote = td_api::make_object<td_api::remoteFile>();
        remote->unique_id_ = "uid-thumb";
        file->remote_ = std::move(remote);
        auto local = td_api::make_object<td_api::localFile>();
        local->path_ = thumbnail_path;
        local->is_downloading_completed_ = true;
        file->local_ = std::move(local);
        return file;
      }
      default:
        return td_api::make_object<td_api::error>(400, "unexpected request");
    }
  }

  std::uint64_t next_id_ = 1;
  std::map<std::string, int> sent_;
  td_api::object_ptr<td_api::message> target_message_;
  std::map<std::int64_t, td_api::object_ptr<td_api::message>> targets_;
  std::set<std::int32_t> slow_file_ids_;
  int slow_delay_ms_ = 2000;
  std::vector<std::thread> slow_threads_;
};

td_api::object_ptr<td_api::formattedText> MakeText(const std::string& text) {
  auto t = td_api::make_object<td_api::formattedText>();
  t->text_ = text;
  return t;
}

td_api::object_ptr<td_api::file> MakeFile(int32_t id, const std::string& uid,
                                          const std::string& local_path) {
  auto file = td_api::make_object<td_api::file>();
  file->id_ = id;
  auto remote = td_api::make_object<td_api::remoteFile>();
  remote->unique_id_ = uid;
  file->remote_ = std::move(remote);
  auto local = td_api::make_object<td_api::localFile>();
  local->path_ = local_path;
  local->is_downloading_completed_ = true;
  file->local_ = std::move(local);
  return file;
}

// A target photo message: one 320x240 tier + caption.
td_api::object_ptr<td_api::message> MakePhotoMessage(
    std::int64_t chat_id, std::int64_t message_id, const std::string& thumb_path,
    std::int32_t file_id = 4242, const std::string& uid = "uid-photo") {
  auto message = td_api::make_object<td_api::message>();
  message->id_ = message_id;
  message->chat_id_ = chat_id;
  auto size = td_api::make_object<td_api::photoSize>();
  size->type_ = "m";
  size->width_ = 320;
  size->height_ = 240;
  size->photo_ = MakeFile(file_id, uid, thumb_path);
  auto photo = td_api::make_object<td_api::photo>();
  photo->sizes_.push_back(std::move(size));
  auto content = td_api::make_object<td_api::messagePhoto>();
  content->photo_ = std::move(photo);
  content->caption_ = MakeText("the caption");
  message->content_ = std::move(content);
  return message;
}

td_api::object_ptr<td_api::updateNewMessage> MakeCommandUpdate(
    std::int64_t sender_id, std::int64_t chat_id, const std::string& text,
    bool with_reply_to, std::int64_t reply_target_message_id = 0) {
  auto message = td_api::make_object<td_api::message>();
  message->id_ = 900;
  message->chat_id_ = chat_id;
  auto sender = td_api::make_object<td_api::messageSenderUser>();
  sender->user_id_ = sender_id;
  message->sender_id_ = std::move(sender);
  if (with_reply_to) {
    auto reply = td_api::make_object<td_api::messageReplyToMessage>();
    reply->chat_id_ = chat_id;
    reply->message_id_ = reply_target_message_id;
    message->reply_to_ = std::move(reply);
  }
  auto content = td_api::make_object<td_api::messageText>();
  content->text_ = MakeText(text);
  message->content_ = std::move(content);

  auto update = td_api::make_object<td_api::updateNewMessage>();
  update->message_ = std::move(message);
  return update;
}

template <typename Req, typename Rep>
typename transport::AsyncStreamer<Req, Rep>::Options MakeStreamerOptions(
    const std::string& name) {
  typename transport::AsyncStreamer<Req, Rep>::Options options;
  options.name = name;
  options.queue_capacity = 10;
  return options;  // start_fn unset: no network; Enqueue just queues
}

class BotCommandTest : public ::testing::Test {
 protected:
  void SetUp() override {
    thumb_path_ = "/tmp/tg_client_thumb_" + std::to_string(getpid()) + ".jpg";
    {
      std::ofstream out(thumb_path_, std::ios::binary);
      out << "\xFF\xD8" "fake-jpeg-thumb";
    }
    outbox_path_ =
        "/tmp/tg_client_botcmd_outbox_" + std::to_string(getpid()) + ".sqlite";
    std::remove(outbox_path_.c_str());

    config_.bot_command.command_bot_user_id = 777;  // the command bot (mode a)
    config_.media_thumbnail.max_thumb_bytes = 1024 * 1024;

    sender_.thumbnail_path = thumb_path_;
    sender_.target_chat_id_ = -1'001'234'567'890LL;
    sender_.target_message_id_ = 42;
    sender_.set_target_message(
        MakePhotoMessage(sender_.target_chat_id_, sender_.target_message_id_,
                         thumb_path_));

    files_ = std::make_unique<tg::telegram::FileDownloader>(sender_);
    // The async pipeline posts continuations here; the tests wait for them.
    workers_ = std::make_unique<app::WorkerPool>(2u);
    // Single-lane pools: the handler tests inspect lane 0's queue.
    process_streamer_ = std::make_unique<
        transport::StreamerPool<ImageRequest, ImageReply>>(
        std::vector<transport::AsyncStreamer<ImageRequest, ImageReply>::Options>{
            MakeStreamerOptions<ImageRequest, ImageReply>("ProcessImages")});
    search_streamer_ = std::make_unique<
        transport::StreamerPool<SearchReq, SearchRsp>>(
        std::vector<transport::AsyncStreamer<SearchReq, SearchRsp>::Options>{
            MakeStreamerOptions<SearchReq, SearchRsp>("SearchImages")});
    outbox_ = std::make_unique<tg::infra::Outbox>(outbox_path_);
    ctx_ = std::make_unique<app::AppContext>(app::AppContext{
        *workers_, sender_, *files_, *process_streamer_, *search_streamer_,
        *outbox_, config_});

    search_handler_ = std::make_unique<media::SearchHandler>(
        media::MediaExtractor(config_.media_thumbnail), sender_, *files_);
    handler_ = std::make_unique<bot_command::BotCommandHandler>(
        config_.bot_command,
        media::MediaThumbnailHandler(
            media::MediaExtractor(config_.media_thumbnail), sender_, *files_),
        *search_handler_);
  }

  void TearDown() override {
    handler_.reset();
    search_handler_.reset();
    // Drain the pool first: queued finish stages use ctx_/outbox_/streamers.
    workers_.reset();
    ctx_.reset();
    outbox_.reset();
    process_streamer_.reset();
    search_streamer_.reset();
    files_.reset();
    sender_.join_slow_threads();
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
  std::unique_ptr<transport::StreamerPool<ImageRequest, ImageReply>>
      process_streamer_;
  std::unique_ptr<transport::StreamerPool<SearchReq, SearchRsp>>
      search_streamer_;
  std::unique_ptr<tg::infra::Outbox> outbox_;
  std::unique_ptr<app::AppContext> ctx_;
  std::unique_ptr<media::SearchHandler> search_handler_;
  std::unique_ptr<bot_command::BotCommandHandler> handler_;
};

}  // namespace

// --- CommandParser ---------------------------------------------------------

TEST(CommandParser, ParsesNameAndArgs) {
  const auto parsed = bot_command::CommandParser::Parse("/process 12 34");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->name, "process");
  ASSERT_EQ(parsed->args.size(), 2u);
  EXPECT_EQ(parsed->args[0], "12");
  EXPECT_EQ(parsed->args[1], "34");
  ASSERT_TRUE(parsed->target_chat_id.has_value());
  EXPECT_EQ(*parsed->target_chat_id, 12);
  ASSERT_TRUE(parsed->target_message_id.has_value());
  EXPECT_EQ(*parsed->target_message_id, 34);
}

TEST(CommandParser, StripsBotSuffixAndLowercases) {
  const auto parsed = bot_command::CommandParser::Parse("/SEARCH@My_Bot hello");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->name, "search");
}

TEST(CommandParser, RejectsNonCommands) {
  EXPECT_FALSE(bot_command::CommandParser::Parse("hello world").has_value());
  EXPECT_FALSE(bot_command::CommandParser::Parse("").has_value());
  EXPECT_FALSE(bot_command::CommandParser::Parse("/").has_value());
}

TEST(CommandParser, RejectsNonNumericTargetArgs) {
  EXPECT_FALSE(bot_command::CommandParser::Parse("/process abc 34")
                   ->target_chat_id.has_value());
  EXPECT_FALSE(bot_command::CommandParser::Parse("/process 12")
                   ->target_chat_id.has_value());
  EXPECT_FALSE(bot_command::CommandParser::Parse("/process 12 34 56")
                   ->target_chat_id.has_value());
}

// --- Classify --------------------------------------------------------------

TEST(BotCommandClassify, KnownAndUnknownCommands) {
  app::BotCommandConfig config;
  config.process_command = "/process";
  config.search_command = "/search";
  EXPECT_EQ(bot_command::BotCommandHandler::Classify("/process", config),
            bot_command::BotCommandHandler::Action::kProcess);
  EXPECT_EQ(bot_command::BotCommandHandler::Classify("/search 12 34", config),
            bot_command::BotCommandHandler::Action::kSearch);
  EXPECT_EQ(bot_command::BotCommandHandler::Classify("/process@Bot", config),
            bot_command::BotCommandHandler::Action::kProcess);
  EXPECT_EQ(bot_command::BotCommandHandler::Classify("/unknown", config),
            bot_command::BotCommandHandler::Action::kIgnore);
  EXPECT_EQ(bot_command::BotCommandHandler::Classify("not a command", config),
            bot_command::BotCommandHandler::Action::kIgnore);
}

// --- Handler behaviour -----------------------------------------------------

TEST_F(BotCommandTest, ProcessCommandFromConfiguredBotViaReplyTo) {
  auto update = MakeCommandUpdate(777, -1'001'234'567'890LL, "/process", true, 42);
  td_api::object_ptr<td_api::Update> base = std::move(update);

  ASSERT_TRUE(handler_->CanHandle(base));
  handler_->Handle(base, *ctx_);

  EXPECT_EQ(sender_.sent_count("getMessage"), 1);
  ASSERT_TRUE(WaitForQueued(*process_streamer_, 1));
  EXPECT_EQ(process_streamer_->lane(0)->QueuedCount(), 1);

  const auto request = process_streamer_->lane(0)->PeekFront();
  ASSERT_TRUE(request.has_value());
  // image_id = "<chat_id>:<message_id>:<file_unique_id>" (v5 §1.2)
  EXPECT_EQ(request->image_id(), "-1001234567890:42:uid-photo");
  EXPECT_EQ(request->memo(), "the caption");  // memo = caption ONLY
  EXPECT_EQ(request->owner_id(), -1'001'234'567'890LL);  // int64, no conversion

  // Outbox got the PENDING record for retry (v5 §2.2).
  const auto record = outbox_->Get(request->image_id());
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->state, tg::infra::OutboxState::kPending);
  EXPECT_EQ(record->memo, "the caption");
}

TEST_F(BotCommandTest, SearchCommandTargetsArgsGivenChatAndMessage) {
  auto update = MakeCommandUpdate(777, -1'001'234'567'890LL,
                                  "/search -1001234567890 42", false);
  td_api::object_ptr<td_api::Update> base = std::move(update);

  ASSERT_TRUE(handler_->CanHandle(base));
  handler_->Handle(base, *ctx_);

  ASSERT_TRUE(WaitForQueued(*search_streamer_, 1));
  EXPECT_EQ(search_streamer_->lane(0)->QueuedCount(), 1);
  EXPECT_EQ(process_streamer_->lane(0)->QueuedCount(), 0);  // search never processes

  const auto request = search_streamer_->lane(0)->PeekFront();
  ASSERT_TRUE(request.has_value());
  // bare-ID conversion: supergroup dialog -> bare oid, uint32 range-checked
  EXPECT_EQ(request->oid(), 1234567890u);
  EXPECT_EQ(request->id(), 42u);
  EXPECT_FALSE(request->image_data().empty());
  // SearchReq is NOT in the Outbox (v5 §2.4)
  EXPECT_FALSE(outbox_->Get("-1001234567890:42:uid-photo").has_value());
}

TEST_F(BotCommandTest, SecondCommandCompletesWhileFirstDownloadPending) {
  // Async regression test (2026-11-09): message A's download is slow
  // (2 s), message B's is instant. With the old blocking pipeline the
  // worker handling A sat in future::wait_for for the whole download, so
  // B could not finish until A did. With the async pipeline, B's request
  // reaches the streamer queue while A is still downloading.
  sender_.set_slow_downloads({4242}, 2000);
  sender_.add_target_message(MakePhotoMessage(sender_.target_chat_id_, 42,
                                              thumb_path_, 4242, "uid-photo-a"));
  sender_.add_target_message(MakePhotoMessage(sender_.target_chat_id_, 43,
                                              thumb_path_, 4243, "uid-photo-b"));

  auto update_a =
      MakeCommandUpdate(777, -1'001'234'567'890LL, "/process", true, 42);
  td_api::object_ptr<td_api::Update> base_a = std::move(update_a);
  handler_->Handle(base_a, *ctx_);
  auto update_b =
      MakeCommandUpdate(777, -1'001'234'567'890LL, "/process", true, 43);
  td_api::object_ptr<td_api::Update> base_b = std::move(update_b);
  handler_->Handle(base_b, *ctx_);

  // B finishes first: well before A's 2 s download, the queue holds
  // exactly B's request.
  ASSERT_TRUE(WaitForQueued(*process_streamer_, 1));
  EXPECT_EQ(process_streamer_->lane(0)->QueuedCount(), 1u);
  const auto first = process_streamer_->lane(0)->PeekFront();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->image_id(), "-1001234567890:43:uid-photo-b");

  // A lands later, after its slow download completes.
  ASSERT_TRUE(WaitForQueued(*process_streamer_, 2));
  EXPECT_EQ(process_streamer_->lane(0)->QueuedCount(), 2u);
}

TEST_F(BotCommandTest, OtherSenderIsIgnored) {
  auto update = MakeCommandUpdate(111, -1'001'234'567'890LL, "/process", true, 42);
  td_api::object_ptr<td_api::Update> base = std::move(update);
  EXPECT_FALSE(handler_->CanHandle(base));
  handler_->Handle(base, *ctx_);
  EXPECT_EQ(process_streamer_->lane(0)->QueuedCount(), 0);
  EXPECT_EQ(sender_.sent_count("getMessage"), 0);
}

TEST_F(BotCommandTest, UnknownCommandIsIgnored) {
  auto update = MakeCommandUpdate(777, -1'001'234'567'890LL, "/boom", true, 42);
  td_api::object_ptr<td_api::Update> base = std::move(update);
  EXPECT_FALSE(handler_->CanHandle(base));
  handler_->Handle(base, *ctx_);
  EXPECT_EQ(process_streamer_->lane(0)->QueuedCount(), 0);
}

TEST_F(BotCommandTest, CommandWithoutTargetIsIgnored) {
  auto update = MakeCommandUpdate(777, -1'001'234'567'890LL, "/process", false);
  td_api::object_ptr<td_api::Update> base = std::move(update);
  EXPECT_TRUE(handler_->CanHandle(base));  // right sender + right command...
  handler_->Handle(base, *ctx_);           // ...but no target -> nothing sent
  EXPECT_EQ(sender_.sent_count("getMessage"), 0);
  EXPECT_EQ(process_streamer_->lane(0)->QueuedCount(), 0);
}

TEST_F(BotCommandTest, GetMessageErrorIsSwallowed) {
  sender_.set_target_message(nullptr);  // fake answers 404
  auto update = MakeCommandUpdate(777, -1'001'234'567'890LL, "/process", true, 42);
  td_api::object_ptr<td_api::Update> base = std::move(update);
  handler_->Handle(base, *ctx_);
  EXPECT_EQ(process_streamer_->lane(0)->QueuedCount(), 0);
}

TEST_F(BotCommandTest, DisabledHandlerClaimsNothing) {
  config_.bot_command.enabled = false;
  media::SearchHandler search_handler(
      media::MediaExtractor(config_.media_thumbnail), sender_, *files_);
  auto disabled = std::make_unique<bot_command::BotCommandHandler>(
      config_.bot_command,
      media::MediaThumbnailHandler(
          media::MediaExtractor(config_.media_thumbnail), sender_, *files_),
      search_handler);
  auto update = MakeCommandUpdate(777, -1'001'234'567'890LL, "/process", true, 42);
  td_api::object_ptr<td_api::Update> base = std::move(update);
  EXPECT_FALSE(disabled->CanHandle(base));
}
