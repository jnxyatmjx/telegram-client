// Production wiring for the ProcessImages flow-control window.
//
// tests/test_flow_control.cpp builds its own reply callback and calls
// Release() itself, so it stays green when Application forgets to. This
// test constructs Application, sends through its ProcessImages pool, and
// lets the server's ImageReply hit the callback WireStreamerCallbacks
// installed. A missing Release leaves in_flight at the cap after the
// records are DONE.

#include <app/Application.h>
#include <app/Config.h>
#include <app/FlowControl.h>
#include <infra/Outbox.h>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "image_stream.grpc.pb.h"
#include "image_stream.pb.h"

namespace tg::app {

// Friend of Application. Only the regression test uses it.
class ProcessImagesFlowControlProbe {
 public:
  static FlowControl& flow(Application& app) { return app.flow_control_; }
  static infra::Outbox& outbox(Application& app) { return app.outbox_; }
  static transport::StreamerPool<ImageRequest, ImageReply>& pool(
      Application& app) {
    return app.process_pool_;
  }
  static void OnReply(Application& app, const ImageReply& reply) {
    app.OnProcessImageReply(reply);
  }
};

}  // namespace tg::app

namespace {

namespace infra = tg::infra;
using tg::app::Application;
using tg::app::Config;
using tg::app::ProcessImagesFlowControlProbe;

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool WaitFor(const std::function<bool()>& pred, std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}

// Holds each reply until the test allows it, so the window can be observed
// full. Client writes still complete inside the HTTP/2 window (see
// test_flow_control.cpp).
class BudgetedEcho final : public ImageProcessor::Service {
 public:
  ::grpc::Status ProcessImages(
      ::grpc::ServerContext* /*context*/,
      ::grpc::ServerReaderWriter<::ImageReply, ::ImageRequest>* stream)
      override {
    ::ImageRequest req;
    while (stream->Read(&req)) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        received_.push_back(req.image_id());
      }
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::seconds(30), [this] {
              return replies_allowed_ > replies_sent_;
            })) {
          break;
        }
        ++replies_sent_;
      }
      ::ImageReply reply;
      reply.set_image_id(req.image_id());
      reply.set_result("ok:" + req.image_id());
      if (!stream->Write(reply)) break;
    }
    return ::grpc::Status::OK;
  }

  ::grpc::Status SearchImages(
      ::grpc::ServerContext* /*context*/,
      ::grpc::ServerReaderWriter<::SearchRsp, ::SearchReq>* /*stream*/)
      override {
    return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "not used");
  }

  void AllowOneMoreReply() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++replies_allowed_;
    }
    cv_.notify_all();
  }

  std::vector<std::string> received() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::string> received_;
  int replies_allowed_ = 0;
  int replies_sent_ = 0;
};

struct ServerStop {
  ::grpc::Server* server = nullptr;
  ~ServerStop() {
    if (server == nullptr) return;
    server->Shutdown(std::chrono::system_clock::now() +
                     std::chrono::seconds(2));
    server->Wait();
  }
};

void PutPending(infra::Outbox& outbox, const std::string& image_id) {
  infra::OutboxRecord record;
  record.image_id = image_id;
  record.image_data = "thumb";
  record.memo = "caption";
  record.owner_id = 42;
  record.state = infra::OutboxState::kPending;
  // Not due: a reconnect sweep must not send this ahead of the test.
  record.next_retry_at_ms = NowMs() + 3'600'000;
  outbox.Put(record);
}

ImageRequest MakeRequest(const std::string& image_id) {
  ImageRequest request;
  request.set_image_id(image_id);
  request.set_image_data("thumb");
  request.set_memo("caption");
  request.set_owner_id(42);
  return request;
}

TEST(ApplicationFlowControlTest, ImageReplyReleasesSlotDuplicateDoesNot) {
  const std::string socket_path =
      "/tmp/tg_app_flow_" + std::to_string(getpid()) + ".sock";
  const std::string db_path =
      "/tmp/tg_app_flow_" + std::to_string(getpid()) + ".sqlite";
  struct TmpCleanup {
    std::string socket;
    std::string db;
    ~TmpCleanup() {
      std::remove(socket.c_str());
      std::remove(db.c_str());
      std::remove((db + "-wal").c_str());
      std::remove((db + "-shm").c_str());
    }
  } cleanup{socket_path, db_path};
  std::remove(socket_path.c_str());
  std::remove(db_path.c_str());
  std::remove((db_path + "-wal").c_str());
  std::remove((db_path + "-shm").c_str());

  BudgetedEcho service;
  ::grpc::ServerBuilder builder;
  builder.AddListeningPort("unix://" + socket_path,
                           ::grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);
  ServerStop stop{server.get()};

  Config config;
  config.worker_count = 1;
  config.grpc.target = "unix://" + socket_path;
  config.grpc.tls = false;
  config.grpc.connections = 1;
  config.grpc.reconnect_backoff_ms = 50;
  config.grpc.max_reconnect_backoff_ms = 200;
  config.grpc.queue_capacity = 100;
  config.outbox.db_path = db_path;
  config.outbox.max_in_flight = 2;
  config.outbox.sent_timeout_ms = 600'000;
  config.outbox.send_grace_ms = 600'000;
  config.chat_state.enabled = false;
  config.auto_search.enabled = false;
  config.media_channels.enabled = false;

  // Destroyed before ServerStop: the process stream is cancelled before
  // the fake server is shut down, so server->Wait() can return.
  Application app(std::move(config));
  auto& flow = ProcessImagesFlowControlProbe::flow(app);
  auto& outbox = ProcessImagesFlowControlProbe::outbox(app);
  auto& pool = ProcessImagesFlowControlProbe::pool(app);
  pool.Start();
  ASSERT_TRUE(WaitFor([&] { return pool.IsStreamRunning(); },
                      std::chrono::seconds(10)))
      << "ProcessImages stream did not come up";

  for (const char* id : {"img-a", "img-b"}) {
    PutPending(outbox, id);
    ASSERT_TRUE(flow.TryAcquire()) << id;
    ASSERT_TRUE(pool.Enqueue(MakeRequest(id), std::chrono::milliseconds(2000)))
        << id;
  }
  ASSERT_TRUE(WaitFor(
      [&] {
        return flow.in_flight() == 2 &&
               outbox.CountByState(infra::OutboxState::kSent) == 2;
      },
      std::chrono::seconds(10)));
  EXPECT_FALSE(flow.TryAcquire());
  EXPECT_EQ(flow.in_flight(), 2);

  // One production ImageReply frees exactly one slot. The result is not
  // {"cid","mid"} JSON; the slot must still come back (release happens
  // before the watermark parse returns).
  service.AllowOneMoreReply();
  ASSERT_TRUE(WaitFor([&] { return flow.in_flight() == 1; },
                      std::chrono::seconds(10)))
      << "in_flight stayed " << flow.in_flight()
      << " after a successful ImageReply (slot leaked)";
  ASSERT_TRUE(WaitFor(
      [&] { return service.received().size() >= 1; },
      std::chrono::seconds(2)));
  const std::string done_id = service.received().front();
  ASSERT_TRUE(WaitFor(
      [&] {
        const auto record = outbox.Get(done_id);
        return record.has_value() &&
               record->state == infra::OutboxState::kDone;
      },
      std::chrono::seconds(5)));

  // Same handler, same still-held slot: duplicate, unknown, and
  // dead-letter replies must not Release().
  ImageReply duplicate;
  duplicate.set_image_id(done_id);
  duplicate.set_result("ok-again");
  ProcessImagesFlowControlProbe::OnReply(app, duplicate);
  EXPECT_EQ(flow.in_flight(), 1);
  EXPECT_EQ(outbox.Get(done_id)->state, infra::OutboxState::kDone);

  ImageReply unknown;
  unknown.set_image_id("no-such-image");
  unknown.set_result("missing");
  ProcessImagesFlowControlProbe::OnReply(app, unknown);
  EXPECT_EQ(flow.in_flight(), 1);
  EXPECT_FALSE(outbox.Get("no-such-image").has_value());

  PutPending(outbox, "dead-lettered");
  ASSERT_TRUE(outbox.MarkDeadLetter("dead-lettered", "permanent"));
  ImageReply dead;
  dead.set_image_id("dead-lettered");
  dead.set_result("nope");
  ProcessImagesFlowControlProbe::OnReply(app, dead);
  EXPECT_EQ(flow.in_flight(), 1);
  EXPECT_EQ(outbox.Get("dead-lettered")->state,
            infra::OutboxState::kDeadLetter);

  // The remaining in-flight reply drains the window back to empty.
  service.AllowOneMoreReply();
  ASSERT_TRUE(WaitFor([&] { return flow.in_flight() == 0; },
                      std::chrono::seconds(10)))
      << "in_flight=" << flow.in_flight() << " after both replies";
  EXPECT_EQ(outbox.CountByState(infra::OutboxState::kSent), 0);
  EXPECT_EQ(outbox.CountByState(infra::OutboxState::kDone), 2);
  EXPECT_EQ(outbox.Get("dead-lettered")->state,
            infra::OutboxState::kDeadLetter);

  PutPending(outbox, "img-c");
  ASSERT_TRUE(flow.TryAcquire());
  EXPECT_EQ(flow.in_flight(), 1);
  ASSERT_TRUE(pool.Enqueue(MakeRequest("img-c"),
                           std::chrono::milliseconds(2000)));
  ASSERT_TRUE(WaitFor(
      [&] {
        const auto record = outbox.Get("img-c");
        return record.has_value() &&
               record->state == infra::OutboxState::kSent;
      },
      std::chrono::seconds(10)));
  service.AllowOneMoreReply();
  ASSERT_TRUE(WaitFor(
      [&] {
        const auto record = outbox.Get("img-c");
        return flow.in_flight() == 0 && record.has_value() &&
               record->state == infra::OutboxState::kDone;
      },
      std::chrono::seconds(10)));

  // A second delivery of img-c, through the same method the streamer
  // callback calls, must not drive the counter negative.
  ImageReply again;
  again.set_image_id("img-c");
  again.set_result("ok:img-c");
  ProcessImagesFlowControlProbe::OnReply(app, again);
  EXPECT_EQ(flow.in_flight(), 0);
  EXPECT_EQ(outbox.Get("img-c")->state, infra::OutboxState::kDone);

  pool.Stop();
}

}  // namespace
