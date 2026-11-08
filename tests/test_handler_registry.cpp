// UpdateDispatcher (v5 §3.2/§3.3): registration order = priority; the FIRST
// claiming handler owns the update (td_api objects are move-only); Handle
// runs on a WorkerPool thread, never the caller's; unclaimed updates are
// dropped; a full worker queue drops the task loudly instead of blocking the
// Receive thread.
#include <app/AppContext.h>
#include <app/WorkerPool.h>
#include <infra/Outbox.h>
#include <telegram/TelegramClient.h>
#include <telegram/UpdateDispatcher.h>
#include <telegram/UpdateHandler.h>
#include <transport/AsyncStreamer.h>

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace td_api = td::td_api;  // shorthand for td::td_api::*

namespace app = tg::app;
namespace transport = tg::transport;

namespace {

class FakeTdSender : public tg::telegram::ITdSender {
 public:
  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback) override {
    td::ClientManager::Response response;
    response.request_id = next_id_++;
    response.object = td_api::make_object<td_api::error>(400, "fake");
    callback(std::move(response));
    return static_cast<std::int64_t>(response.request_id);
  }

 private:
  std::uint64_t next_id_ = 1;
};

// Counting handler: claims `claim_` updates, records Handle() invocations.
class FakeHandler : public tg::telegram::IUpdateHandler {
 public:
  FakeHandler(std::string name, bool claim)
      : name_(std::move(name)), claim_(claim) {}

  bool CanHandle(
      const td_api::object_ptr<td_api::Update>&) const override {
    return claim_;
  }

  void Handle(td_api::object_ptr<td_api::Update>& update,
              app::AppContext&) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++handled_;
    handled_thread_ = std::this_thread::get_id();
    update_nonnull_ = update != nullptr;
  }

  const char* Name() const override { return name_.c_str(); }

  int handled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handled_;
  }
  std::thread::id handled_thread() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handled_thread_;
  }
  bool update_nonnull() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return update_nonnull_;
  }

 private:
  std::string name_;
  bool claim_;
  mutable std::mutex mutex_;
  int handled_ = 0;
  std::thread::id handled_thread_{};
  bool update_nonnull_ = false;
};

td_api::object_ptr<td_api::Update> MakeUpdate() {
  auto update = td_api::make_object<td_api::updateNewMessage>();
  auto message = td_api::make_object<td_api::message>();
  message->id_ = 1;
  message->chat_id_ = 100;
  auto content = td_api::make_object<td_api::messageText>();
  auto text = td_api::make_object<td_api::formattedText>();
  text->text_ = "hi";
  content->text_ = std::move(text);
  message->content_ = std::move(content);
  update->message_ = std::move(message);
  return update;
}

// Full fixture: real WorkerPool + dispatcher + a minimal AppContext (the
// handlers ignore it, but the reference must be valid).
class DispatcherTest : public ::testing::Test {
 protected:
  void SetUp() override {
    outbox_path_ =
        "/tmp/tg_client_disp_outbox_" + std::to_string(getpid()) + ".sqlite";
    std::remove(outbox_path_.c_str());

    pool_ = std::make_shared<app::WorkerPool>(2u);
    files_ = std::make_unique<tg::telegram::FileDownloader>(sender_);
    process_streamer_ = std::make_unique<transport::ProcessImagesStreamer>(
        transport::ProcessImagesStreamer::Options{"ProcessImages"});
    search_streamer_ = std::make_unique<transport::SearchImagesStreamer>(
        transport::SearchImagesStreamer::Options{"SearchImages"});
    outbox_ = std::make_unique<tg::infra::Outbox>(outbox_path_);
    ctx_ = std::make_unique<app::AppContext>(app::AppContext{
        sender_, *files_, *process_streamer_, *search_streamer_, *outbox_,
        config_});
    dispatcher_ = std::make_unique<tg::telegram::UpdateDispatcher>(pool_);
  }

  void TearDown() override {
    dispatcher_.reset();
    ctx_.reset();
    outbox_.reset();
    search_streamer_.reset();
    process_streamer_.reset();
    files_.reset();
    pool_->Stop(std::chrono::seconds(5));
    std::remove(outbox_path_.c_str());
  }

  std::string outbox_path_;
  app::Config config_;
  FakeTdSender sender_;
  std::shared_ptr<app::WorkerPool> pool_;
  std::unique_ptr<tg::telegram::FileDownloader> files_;
  std::unique_ptr<transport::ProcessImagesStreamer> process_streamer_;
  std::unique_ptr<transport::SearchImagesStreamer> search_streamer_;
  std::unique_ptr<tg::infra::Outbox> outbox_;
  std::unique_ptr<app::AppContext> ctx_;
  std::unique_ptr<tg::telegram::UpdateDispatcher> dispatcher_;
};

}  // namespace

TEST_F(DispatcherTest, HandlerCountTracksRegistration) {
  EXPECT_EQ(dispatcher_->handler_count(), 0u);
  dispatcher_->Register(std::make_shared<FakeHandler>("a", true));
  dispatcher_->Register(std::make_shared<FakeHandler>("b", false));
  EXPECT_EQ(dispatcher_->handler_count(), 2u);
}

TEST_F(DispatcherTest, FirstClaimantWins) {
  auto first = std::make_shared<FakeHandler>("first", true);
  auto second = std::make_shared<FakeHandler>("second", true);
  dispatcher_->Register(first);
  dispatcher_->Register(second);

  dispatcher_->Dispatch(MakeUpdate(), *ctx_);
  ASSERT_TRUE(pool_->WaitForIdle(std::chrono::seconds(5)));

  EXPECT_EQ(first->handled(), 1);
  EXPECT_EQ(second->handled(), 0);
}

TEST_F(DispatcherTest, UnclaimedUpdateIsDropped) {
  auto handler = std::make_shared<FakeHandler>("declines", false);
  dispatcher_->Register(handler);

  dispatcher_->Dispatch(MakeUpdate(), *ctx_);
  ASSERT_TRUE(pool_->WaitForIdle(std::chrono::seconds(5)));

  EXPECT_EQ(handler->handled(), 0);
}

TEST_F(DispatcherTest, HandleRunsOnWorkerThreadWithOwnedUpdate) {
  auto handler = std::make_shared<FakeHandler>("worker", true);
  dispatcher_->Register(handler);

  dispatcher_->Dispatch(MakeUpdate(), *ctx_);
  ASSERT_TRUE(pool_->WaitForIdle(std::chrono::seconds(5)));

  EXPECT_EQ(handler->handled(), 1);
  EXPECT_NE(handler->handled_thread(), std::this_thread::get_id());
  EXPECT_TRUE(handler->update_nonnull());
}

TEST_F(DispatcherTest, FullQueueDropsUpdateInsteadOfBlocking) {
  // 1 worker, capacity 1, 50ms post timeout: the second update cannot fit.
  auto small_pool = std::make_shared<app::WorkerPool>(
      1u, 1u, std::chrono::milliseconds(50));
  tg::telegram::UpdateDispatcher small_dispatcher(small_pool);
  auto handler = std::make_shared<FakeHandler>("slow-consumer", true);
  small_dispatcher.Register(handler);

  // Occupy the single worker with a blocked task.
  std::promise<void> entered;
  auto blocked = entered.get_future();
  auto gate = std::make_shared<std::promise<void>>();
  ASSERT_TRUE(small_pool->Post([gate, &entered] {
    entered.set_value();
    gate->get_future().wait();
  }));
  blocked.wait();

  small_dispatcher.Dispatch(MakeUpdate(), *ctx_);   // fills the queue
  small_dispatcher.Dispatch(MakeUpdate(), *ctx_);   // queue full -> dropped

  gate->set_value();  // unblock the worker
  ASSERT_TRUE(small_pool->WaitForIdle(std::chrono::seconds(5)));
  small_pool->Stop(std::chrono::seconds(5));

  // Only the first dispatched update was handled; the dropped one is gone.
  EXPECT_EQ(handler->handled(), 1);
}
