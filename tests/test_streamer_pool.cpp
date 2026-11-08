// StreamerPool: N parallel connections share the load. A 3-lane pool
// against an in-process fake ImageProcessor server must open exactly one
// stream per lane (3 connections), deal 9 requests round-robin (3 per
// lane), and round-trip every reply by image_id.
#include <app/Config.h>
#include <transport/GrpcTransport.h>
#include <transport/ProcessImagesClient.h>
#include <transport/StreamerPool.h>

#include <grpcpp/grpcpp.h>

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_set>
#include <vector>

namespace {

// Fake server: echoes every ProcessImages request and counts, per stream,
// how many messages arrived — one stream per lane/connection.
class CountingImageProcessor final : public ImageProcessor::Service {
 public:
  ::grpc::Status ProcessImages(
      ::grpc::ServerContext* /*context*/,
      ::grpc::ServerReaderWriter<::ImageReply, ::ImageRequest>* stream)
      override {
    std::size_t index;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      index = per_stream_counts_.size();
      per_stream_counts_.push_back(0);
    }
    ::ImageRequest req;
    while (stream->Read(&req)) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        per_stream_counts_[index]++;
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
      ::grpc::ServerReaderWriter<::SearchRsp, ::SearchReq>* stream) override {
    ::SearchReq req;
    while (stream->Read(&req)) {
      ::SearchRsp rsp;
      rsp.set_id(req.id());
      rsp.set_oid(req.oid());
      rsp.set_result("found");
      if (!stream->Write(rsp)) break;
    }
    return ::grpc::Status::OK;
  }

  std::vector<int> per_stream_counts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return per_stream_counts_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<int> per_stream_counts_;
};

class StreamerPoolTest : public ::testing::Test {
 protected:
  void SetUp() override {
    socket_path_ = "/tmp/tg_pool_test_" + std::to_string(getpid()) + ".sock";
    std::remove(socket_path_.c_str());

    ::grpc::ServerBuilder builder;
    builder.AddListeningPort("unix://" + socket_path_,
                             ::grpc::InsecureServerCredentials());
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);

    tg::app::GrpcConfig grpc;
    grpc.target = "unix://" + socket_path_;
    grpc.tls = false;
    grpc.connections = 3;
    transport_ = std::make_unique<tg::transport::GrpcTransport>(grpc);
    ASSERT_EQ(transport_->lane_count(), 3u);

    std::vector<tg::transport::ProcessImagesStreamer::Options> options;
    for (std::size_t lane = 0; lane < 3; ++lane) {
      clients_.push_back(
          std::make_unique<tg::transport::ProcessImagesClient>(*transport_, lane));
      tg::transport::ProcessImagesStreamer::Options opt;
      opt.name = "ProcessImages#" + std::to_string(lane);
      opt.start_fn = clients_.back()->start_fn();
      opt.make_context = [this] { return transport_->MakeContext(); };
      opt.pump_interval_ms = 10;
      opt.initial_backoff_ms = 50;
      opt.max_backoff_ms = 200;
      options.push_back(std::move(opt));
    }
    pool_ = std::make_unique<
        tg::transport::StreamerPool<ImageRequest, ImageReply>>(
        std::move(options));
    pool_->SetReplyHandler([this](const ImageReply& reply) {
      std::lock_guard<std::mutex> lock(mutex_);
      replies_.insert(reply.image_id());
      cv_.notify_all();
    });
    pool_->Start();
    ASSERT_TRUE(WaitForRunning());
  }

  void TearDown() override {
    if (pool_) pool_->Stop();
    if (server_ != nullptr) {
      server_->Shutdown(std::chrono::system_clock::now() +
                        std::chrono::seconds(2));
      server_->Wait();
      server_.reset();
    }
    std::remove(socket_path_.c_str());
  }

  bool WaitForRunning() {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      if (pool_->IsStreamRunning()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }

  bool WaitForReplies(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(10),
                        [&] { return replies_.size() >= count; });
  }

  std::string socket_path_;
  CountingImageProcessor service_;
  std::unique_ptr<::grpc::Server> server_;
  std::unique_ptr<tg::transport::GrpcTransport> transport_;
  std::vector<std::unique_ptr<tg::transport::ProcessImagesClient>> clients_;
  std::unique_ptr<tg::transport::StreamerPool<ImageRequest, ImageReply>> pool_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::unordered_set<std::string> replies_;
};

TEST_F(StreamerPoolTest, ThreeLanesShareNineRequestsEvenly) {
  constexpr int kRequests = 9;
  for (int i = 0; i < kRequests; ++i) {
    ImageRequest req;
    req.set_image_id("img-" + std::to_string(i));
    req.set_image_data("thumb");
    ASSERT_TRUE(pool_->Enqueue(std::move(req), std::chrono::seconds(5)))
        << "enqueue img-" << i << " failed";
  }
  ASSERT_TRUE(WaitForReplies(kRequests)) << "only " << replies_.size()
                                         << " of " << kRequests
                                         << " replies arrived";
  for (int i = 0; i < kRequests; ++i) {
    EXPECT_TRUE(replies_.count("img-" + std::to_string(i)) > 0)
        << "missing reply for img-" << i;
  }

  // One stream per lane (3 connections), 3 messages each — round-robin.
  const std::vector<int> counts = service_.per_stream_counts();
  ASSERT_EQ(counts.size(), 3u)
      << "expected one stream per lane, got " << counts.size();
  for (const int c : counts) {
    EXPECT_EQ(c, kRequests / 3) << "lanes did not share the load evenly";
  }
}

}  // namespace
