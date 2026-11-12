// AsyncStreamer CQ state machine against an in-process fake ImageProcessor
// server (IMPLEMENTATION_TASK.md item 6): request/reply round-trip keyed by
// image_id, bounded-queue backpressure, and reconnect after the server
// restarts (OnReconnected fires, queued work can flow again).
//
// The fake server listens on a Unix domain socket, not TCP loopback: this
// cloud VM's runtime intercepts direct TCP connections (other_tcp is
// denied), so a 127.0.0.1 loopback client gets the platform's block message
// instead of the server's HTTP/2 SETTINGS frame. The streamer logic under
// test (CQ state machine, reconnect, backpressure) is transport-agnostic —
// production still uses a TCP target from config.
//
// This test opens real sockets; the reconnect case waits on the stream, so
// CMake gives it a 120 s timeout (see tests/CMakeLists.txt).

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

using Streamer = tg::transport::AsyncStreamer<ImageRequest, ImageReply>;

// Fake server: echoes every request back as a reply with the same image_id
// and result "ok:<image_id>". SearchImages is not used by this test.
class FakeImageProcessor final : public ImageProcessor::Service {
 public:
  ::grpc::Status ProcessImages(
      ::grpc::ServerContext* /*context*/,
      ::grpc::ServerReaderWriter<::ImageReply, ::ImageRequest>* stream)
      override {
    ::ImageRequest req;
    while (stream->Read(&req)) {
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
};

// Fake server for the half-close regression test (second live crash
// 2026-11-12 12:11:41): on the FIRST stream it reads one request and then
// returns immediately without replying, so the client's Read fails while
// the client still has a Write in flight (burst). On later streams it
// echoes normally, so the reconnected stream can round-trip.
class HalfCloseImageProcessor final : public ImageProcessor::Service {
 public:
  ::grpc::Status ProcessImages(
      ::grpc::ServerContext* /*context*/,
      ::grpc::ServerReaderWriter<::ImageReply, ::ImageRequest>* stream)
      override {
    ::ImageRequest req;
    if (!stream->Read(&req)) {
      return ::grpc::Status::OK;
    }
    if (close_first_.exchange(false)) {
      // First stream: close without replying. The client's Read fails;
      // a burst Write is (very likely) still outstanding, so this is the
      // "read fails first while a Write is in flight" interleaving.
      return ::grpc::Status::OK;
    }
    // Later streams: echo.
    do {
      ::ImageReply reply;
      reply.set_image_id(req.image_id());
      reply.set_result("ok:" + req.image_id());
      if (!stream->Write(reply)) break;
    } while (stream->Read(&req));
    return ::grpc::Status::OK;
  }

  ::grpc::Status SearchImages(
      ::grpc::ServerContext* /*context*/,
      ::grpc::ServerReaderWriter<::SearchRsp, ::SearchReq>* /*stream*/)
      override {
    return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "not used");
  }

 private:
  std::atomic<bool> close_first_{true};
};

ImageRequest MakeRequest(const std::string& image_id) {
  ImageRequest req;
  req.set_image_id(image_id);
  req.set_image_data("\x01\x02thumb");
  req.set_memo("caption only");  // caption only, never JSON (v5 §1.2)
  req.set_owner_id(-1'001'234'567'890LL);
  return req;
}

class StreamerTest : public ::testing::Test {
 protected:
  void StartServer() {
    socket_path_ = "/tmp/tg-streamer-test-" +
                   std::to_string(static_cast<long long>(::getpid())) +
                   ".sock";
    std::remove(socket_path_.c_str());  // stale file from an earlier run
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort("unix://" + socket_path_,
                             ::grpc::InsecureServerCredentials());
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
  }

  // Same socket, but the half-close service (first stream closes after one
  // request without replying; later streams echo).
  void StartHalfCloseServer() {
    socket_path_ = "/tmp/tg-streamer-test-" +
                   std::to_string(static_cast<long long>(::getpid())) +
                   ".sock";
    std::remove(socket_path_.c_str());
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort("unix://" + socket_path_,
                             ::grpc::InsecureServerCredentials());
    builder.RegisterService(&half_close_service_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
  }

  void StopServer() {
    if (server_ != nullptr) {
      // Shutdown with a deadline: a plain Shutdown()/Wait() never returns
      // while a bidirectional stream is still open (the fake server's
      // ProcessImages handler is blocked in Read() on the live stream).
      // The deadline force-cancels in-flight calls, so the handler's Read
      // returns false and Wait() returns; the client then sees the stream
      // break and reconnects. This is what "killing the server" means for
      // a long-lived stream — a graceful shutdown would wait forever.
      server_->Shutdown(std::chrono::system_clock::now() +
                        std::chrono::seconds(2));
      server_->Wait();
      server_.reset();
    }
    std::remove(socket_path_.c_str());
  }

  std::string Target() const { return "unix://" + socket_path_; }

  // Builds a streamer over a fresh channel to the current server port.
  std::unique_ptr<Streamer> MakeStreamer(std::size_t queue_capacity = 1'000) {
    channel_ = ::grpc::CreateChannel(Target(),
                                     ::grpc::InsecureChannelCredentials());
    auto stub = ImageProcessor::NewStub(channel_);
    Streamer::Options options;
    options.name = "ProcessImages";
    options.queue_capacity = queue_capacity;
    options.initial_backoff_ms = 50;  // fast reconnect in tests
    options.max_backoff_ms = 200;
    options.pump_interval_ms = 10;
    // Capture the stub by shared ownership: the StartFn outlives this call.
    auto shared_stub =
        std::shared_ptr<ImageProcessor::Stub>(std::move(stub));
    options.start_fn = [shared_stub](::grpc::ClientContext* context,
                                     ::grpc::CompletionQueue* cq) {
      return shared_stub->PrepareAsyncProcessImages(context, cq);
    };
    auto streamer = std::make_unique<Streamer>(std::move(options));
    streamer->SetReplyHandler([this](const ImageReply& reply) {
      std::lock_guard<std::mutex> lock(mutex_);
      replies_.push_back(reply);
      cv_.notify_all();
    });
    streamer->SetOnReconnected([this] {
      std::lock_guard<std::mutex> lock(mutex_);
      ++reconnected_;
      cv_.notify_all();
    });
    return streamer;
  }

  // Waits up to `timeout` for at least `count` replies; false on timeout.
  bool WaitForReplies(std::size_t count, std::chrono::seconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [&] { return replies_.size() >= count; });
  }

  bool WaitForReconnected(int count, std::chrono::seconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [&] { return reconnected_ >= count; });
  }

  bool WaitForRunning(const Streamer& streamer, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (streamer.IsStreamRunning()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }

  FakeImageProcessor service_;
  HalfCloseImageProcessor half_close_service_;
  std::unique_ptr<::grpc::Server> server_;
  std::string socket_path_;
  std::shared_ptr<::grpc::Channel> channel_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<ImageReply> replies_;
  int reconnected_ = 0;
};

TEST_F(StreamerTest, RoundTripByImageId) {
  StartServer();
  auto streamer = MakeStreamer();
  streamer->Start();
  ASSERT_TRUE(WaitForRunning(*streamer, std::chrono::seconds(10)));

  ASSERT_TRUE(streamer->Enqueue(MakeRequest("c1:m1:uid-a"),
                                std::chrono::milliseconds(1000)));
  ASSERT_TRUE(streamer->Enqueue(MakeRequest("c1:m2:uid-b"),
                                std::chrono::milliseconds(1000)));
  ASSERT_TRUE(WaitForReplies(2, std::chrono::seconds(10)));

  ASSERT_EQ(replies_.size(), 2u);
  EXPECT_EQ(replies_[0].image_id(), "c1:m1:uid-a");
  EXPECT_EQ(replies_[0].result(), "ok:c1:m1:uid-a");
  EXPECT_EQ(replies_[1].image_id(), "c1:m2:uid-b");
  EXPECT_EQ(replies_[1].result(), "ok:c1:m2:uid-b");
  EXPECT_EQ(streamer->QueuedCount(), 0u);

  streamer->Stop();
  EXPECT_FALSE(streamer->Enqueue(MakeRequest("c1:m3:uid-c"),
                                 std::chrono::milliseconds(100)));
  StopServer();
}

TEST_F(StreamerTest, BoundedQueueRejectsWhenFull) {
  // No server, no Start(): the queue just fills, so the second Enqueue
  // must time out instead of growing without bound (v5 §6 backpressure).
  Streamer::Options options;
  options.name = "ProcessImages";
  options.queue_capacity = 1;
  options.start_fn = [](::grpc::ClientContext* /*context*/,
                        ::grpc::CompletionQueue* /*cq*/) -> Streamer::StreamPtr {
    return nullptr;
  };
  auto streamer = std::make_unique<Streamer>(std::move(options));
  ASSERT_TRUE(streamer->Enqueue(MakeRequest("c1:m1:uid-a"),
                                std::chrono::milliseconds(100)));
  EXPECT_EQ(streamer->QueuedCount(), 1u);
  EXPECT_FALSE(streamer->Enqueue(MakeRequest("c1:m2:uid-b"),
                                 std::chrono::milliseconds(100)));
  const auto front = streamer->PeekFront();
  ASSERT_TRUE(front.has_value());
  EXPECT_EQ(front->image_id(), "c1:m1:uid-a");
  streamer->Stop();
}

TEST_F(StreamerTest, ReconnectAfterServerRestart) {
  StartServer();
  auto streamer = MakeStreamer();
  streamer->Start();
  ASSERT_TRUE(WaitForRunning(*streamer, std::chrono::seconds(10)));
  ASSERT_TRUE(streamer->Enqueue(MakeRequest("c1:m1:before"),
                                std::chrono::milliseconds(1000)));
  ASSERT_TRUE(WaitForReplies(1, std::chrono::seconds(10)));

  // Kill the server: the stream must break and the streamer must notice.
  StopServer();
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(10);
  while (streamer->IsStreamRunning() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_FALSE(streamer->IsStreamRunning());

  // Bring the server back on the same socket; the streamer must reconnect
  // on its own (backoff is 50–200 ms in this test) and work must flow.
  ::grpc::ServerBuilder builder;
  builder.AddListeningPort("unix://" + socket_path_,
                           ::grpc::InsecureServerCredentials());
  builder.RegisterService(&service_);
  server_ = builder.BuildAndStart();
  ASSERT_NE(server_, nullptr);

  ASSERT_TRUE(WaitForReconnected(1, std::chrono::seconds(10)));
  ASSERT_TRUE(WaitForRunning(*streamer, std::chrono::seconds(10)));
  ASSERT_TRUE(streamer->Enqueue(MakeRequest("c1:m2:after"),
                                std::chrono::milliseconds(1000)));
  ASSERT_TRUE(WaitForReplies(2, std::chrono::seconds(10)));
  EXPECT_EQ(replies_[1].image_id(), "c1:m2:after");
  EXPECT_EQ(replies_[1].result(), "ok:c1:m2:after");

  streamer->Stop();
  StopServer();
}

TEST_F(StreamerTest, ReconnectAfterServerKillWithPendingWrites) {
  // Regression test for the live crash 2026-11-12: during an /xm backfill
  // the ProcessImages stream was busy (write queue full) when the keepalive
  // watchdog closed the transport, and the process aborted on a gRPC
  // assertion in call_op_set.h. Root cause: the Read failed first so a
  // WritesDone went out, then the Write failed and FailStream called
  // Finish() while that WritesDone was still in flight — a gRPC API
  // misuse. The fix defers Finish until the WritesDone tag lands. This
  // test kills the server while writes are pending and requires a clean
  // reconnect afterwards (no abort, no hang, work flows again).
  StartServer();
  auto streamer = MakeStreamer();
  streamer->Start();
  ASSERT_TRUE(WaitForRunning(*streamer, std::chrono::seconds(10)));
  ASSERT_TRUE(streamer->Enqueue(MakeRequest("c1:m1:before"),
                                std::chrono::milliseconds(1000)));
  ASSERT_TRUE(WaitForReplies(1, std::chrono::seconds(10)));

  // Queue several more writes so the stream is busy when the server dies.
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(streamer->Enqueue(
        MakeRequest("c1:burst-" + std::to_string(i)),
        std::chrono::milliseconds(1000)));
  }

  // Kill the server mid-burst: the stream must break and the streamer must
  // notice (and must not abort on the WritesDone/Finish ordering).
  StopServer();
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(10);
  while (streamer->IsStreamRunning() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_FALSE(streamer->IsStreamRunning());

  // Bring the server back on the same socket; the streamer must reconnect
  // on its own and work must flow again.
  ::grpc::ServerBuilder builder;
  builder.AddListeningPort("unix://" + socket_path_,
                           ::grpc::InsecureServerCredentials());
  builder.RegisterService(&service_);
  server_ = builder.BuildAndStart();
  ASSERT_NE(server_, nullptr);

  ASSERT_TRUE(WaitForReconnected(1, std::chrono::seconds(10)));
  ASSERT_TRUE(WaitForRunning(*streamer, std::chrono::seconds(10)));
  const std::size_t before = replies_.size();
  ASSERT_TRUE(streamer->Enqueue(MakeRequest("c1:m2:after"),
                                std::chrono::milliseconds(1000)));
  ASSERT_TRUE(WaitForReplies(before + 1, std::chrono::seconds(10)));
  EXPECT_EQ(replies_.back().image_id(), "c1:m2:after");
  EXPECT_EQ(replies_.back().result(), "ok:c1:m2:after");

  streamer->Stop();
  StopServer();
}

TEST_F(StreamerTest, ReconnectAfterServerHalfCloseWithWriteInFlight) {
  // Regression test for the second live crash 2026-11-12 12:11:41: the
  // server half-closes (Read fails) while the client still has a Write in
  // flight, and the old code issued WritesDone immediately — colliding
  // with the outstanding Write on the shared write-side CallOpSet, which
  // aborts the process on the same call_op_set.h:972 assertion. The fix
  // defers WritesDone until the in-flight Write lands. This test drives
  // that interleaving (burst writes + server closes after one request)
  // and requires a clean reconnect afterwards (no abort, no hang, work
  // flows again).
  StartHalfCloseServer();
  auto streamer = MakeStreamer();
  streamer->Start();
  ASSERT_TRUE(WaitForRunning(*streamer, std::chrono::seconds(10)));

  // Burst: the first request is read by the server (which then closes the
  // stream without replying); the remaining writes keep a Write in flight
  // when the client's Read fails.
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(streamer->Enqueue(
        MakeRequest("c1:burst-" + std::to_string(i)),
        std::chrono::milliseconds(1000)));
  }

  // The stream must break (server closed it) and the streamer must
  // reconnect on its own (backoff is 50–200 ms in this test) and work
  // must flow again on the new stream.
  ASSERT_TRUE(WaitForReconnected(1, std::chrono::seconds(10)));
  ASSERT_TRUE(WaitForRunning(*streamer, std::chrono::seconds(10)));
  const std::size_t before = replies_.size();
  ASSERT_TRUE(streamer->Enqueue(MakeRequest("c1:m2:after"),
                                std::chrono::milliseconds(1000)));
  ASSERT_TRUE(WaitForReplies(before + 1, std::chrono::seconds(10)));
  EXPECT_EQ(replies_.back().image_id(), "c1:m2:after");
  EXPECT_EQ(replies_.back().result(), "ok:c1:m2:after");

  streamer->Stop();
  StopServer();
}

}  // namespace
