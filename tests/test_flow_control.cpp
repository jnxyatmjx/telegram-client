// Flow-control window for the ProcessImages path (retry design §2(c)),
// exercised through the exact wiring Application uses:
//   TryAcquire -> Enqueue -> write-ok MarkSent (slot stays held)
//   -> ImageReply MarkDone -> Release.
// The fake server gates its replies, so the test can hold the window full
// while it proves the 61st send is blocked, then release one reply's worth
// of slots and watch the send go through. Real sockets (Unix domain — the
// VM blocks TCP loopback, see test_async_streamer.cpp).
#include <app/FlowControl.h>
#include <infra/Outbox.h>
#include <transport/AsyncStreamer.h>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "image_stream.grpc.pb.h"
#include "image_stream.pb.h"

namespace {

namespace infra = tg::infra;
using tg::app::FlowControl;
using Streamer = tg::transport::AsyncStreamer<ImageRequest, ImageReply>;

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

ImageRequest MakeRequest(const std::string& image_id) {
  ImageRequest req;
  req.set_image_id(image_id);
  req.set_image_data("\x01\x02thumb");
  req.set_memo("caption only");
  req.set_owner_id(-1'001'234'567'890LL);
  return req;
}

// Echo server that HOLDS every reply until released: replies release
// flow-control slots, and the test must see the window full before the
// first reply lands.
class GatedEchoProcessor final : public ImageProcessor::Service {
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
      received_cv_.notify_all();
      {
        std::unique_lock<std::mutex> lock(mutex_);
        // Hold THIS request's reply (and keep the handler from reading the
        // next one) until the test releases the window. Client-side writes
        // still complete: they are inside the stream's HTTP/2 window.
        cv_.wait_for(lock, std::chrono::seconds(30),
                     [this] { return release_; });
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

  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      release_ = true;
    }
    cv_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable received_cv_;
  std::vector<std::string> received_;
  bool release_ = false;
};

TEST(FlowControlTest, AcquireReleaseSemantics) {
  FlowControl flow(3);
  ASSERT_TRUE(flow.TryAcquire());
  ASSERT_TRUE(flow.TryAcquire());
  ASSERT_TRUE(flow.TryAcquire());
  EXPECT_EQ(flow.in_flight(), 3);
  EXPECT_FALSE(flow.TryAcquire());  // the 4th is blocked
  EXPECT_EQ(flow.in_flight(), 3);   // a failed acquire releases its claim
  flow.Release();
  EXPECT_EQ(flow.in_flight(), 2);
  EXPECT_TRUE(flow.TryAcquire());
  EXPECT_EQ(flow.in_flight(), 3);
  flow.Release();
  flow.Release();
  flow.Release();
  EXPECT_EQ(flow.in_flight(), 0);

  // 0 disables the window (and never counts).
  FlowControl unlimited(0);
  for (int i = 0; i < 100; ++i) EXPECT_TRUE(unlimited.TryAcquire());
  unlimited.Release();
  EXPECT_EQ(unlimited.in_flight(), 0);
}

TEST(FlowControlTest, WindowBlocksThe61stSendAndReplyReleasesASlot) {
  const std::string socket_path =
      "/tmp/tg_flow_control_test_" + std::to_string(getpid()) + ".sock";
  const std::string db_path =
      "/tmp/tg_flow_control_test_" + std::to_string(getpid()) + ".sqlite";
  std::remove(socket_path.c_str());
  std::remove(db_path.c_str());

  GatedEchoProcessor service;
  ::grpc::ServerBuilder builder;
  builder.AddListeningPort("unix://" + socket_path,
                           ::grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);

  auto channel = ::grpc::CreateChannel("unix://" + socket_path,
                                       ::grpc::InsecureChannelCredentials());
  auto stub = ImageProcessor::NewStub(channel);
  Streamer::Options options;
  options.name = "ProcessImages";
  options.queue_capacity = 1'000;
  options.initial_backoff_ms = 50;
  options.max_backoff_ms = 200;
  options.pump_interval_ms = 10;
  auto shared_stub = std::shared_ptr<ImageProcessor::Stub>(std::move(stub));
  options.start_fn = [shared_stub](::grpc::ClientContext* context,
                                   ::grpc::CompletionQueue* cq) {
    return shared_stub->PrepareAsyncProcessImages(context, cq);
  };

  infra::Outbox outbox(db_path);
  FlowControl flow(60);  // the production default (outbox.max_in_flight)

  std::mutex mutex;
  std::condition_variable cv;
  int written_count = 0;
  int done_count = 0;

  Streamer streamer(std::move(options));
  // Application's process-path wiring (Application.cpp WireStreamerCallbacks).
  streamer.SetOnWritten([&](const ImageRequest& request) {
    if (outbox.MarkSent(request.image_id(), NowMs() + 600'000)) {
      std::lock_guard<std::mutex> lock(mutex);
      ++written_count;
      cv.notify_all();
      return;
    }
    flow.Release();  // duplicate copy of an already-resolved record
  });
  streamer.SetOnWriteLost([&](const ImageRequest& request) {
    flow.Release();
    outbox.MarkFailed(request.image_id(), "write lost", 0);
  });
  streamer.SetReplyHandler([&](const ImageReply& reply) {
    if (outbox.MarkDone(reply.image_id(), reply.result())) {
      flow.Release();
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++done_count;
    }
    cv.notify_all();
  });
  streamer.Start();
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(10);
  while (!streamer.IsStreamRunning() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(streamer.IsStreamRunning());

  auto put_pending = [&](const std::string& id) {
    infra::OutboxRecord record;
    record.image_id = id;
    record.image_data = "thumb";
    record.memo = "caption";
    record.owner_id = -1'001'234'567'890LL;
    record.state = infra::OutboxState::kPending;
    outbox.Put(record);
  };

  // Fill the window with 60 in-flight sends.
  for (int i = 0; i < 60; ++i) {
    const std::string id = "img-" + std::to_string(i);
    put_pending(id);
    ASSERT_TRUE(flow.TryAcquire()) << "window rejected slot " << i;
    ASSERT_TRUE(streamer.Enqueue(MakeRequest(id),
                                std::chrono::milliseconds(1000)));
  }
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(10),
                            [&] { return written_count >= 60; }));
  }
  // The window is full: the 61st send must be blocked and its record left
  // PENDING for the sweep (retry design §2(c)).
  put_pending("img-60");
  EXPECT_EQ(flow.in_flight(), 60);
  EXPECT_FALSE(flow.TryAcquire());
  EXPECT_EQ(flow.in_flight(), 60);
  EXPECT_EQ(outbox.Get("img-60")->state, infra::OutboxState::kPending);
  EXPECT_EQ(outbox.CountByState(infra::OutboxState::kSent), 60);

  // One reply releases one slot.
  service.Release();
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(10),
                            [&] { return flow.in_flight() < 60; }));
  }
  EXPECT_TRUE(flow.TryAcquire());
  ASSERT_TRUE(streamer.Enqueue(MakeRequest("img-60"),
                               std::chrono::milliseconds(1000)));

  // Everything round-trips; the window drains to empty and every record
  // ends DONE (61 = the original 60 + the released 61st).
  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(20),
                            [&] { return done_count >= 61; }));
  }
  for (int i = 0; i < 200 && flow.in_flight() != 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(flow.in_flight(), 0);
  EXPECT_EQ(outbox.CountByState(infra::OutboxState::kDone), 61);
  EXPECT_EQ(outbox.CountByState(infra::OutboxState::kSent), 0);
  EXPECT_EQ(outbox.CountByState(infra::OutboxState::kPending), 0);

  streamer.Stop();
  server->Shutdown(std::chrono::system_clock::now() +
                   std::chrono::seconds(2));
  server->Wait();
  std::remove(socket_path.c_str());
  std::remove(db_path.c_str());
}

}  // namespace
