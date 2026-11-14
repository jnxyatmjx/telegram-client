// Several searches must overlap: downloads start together, and a server
// that handles one request at a time per stream still sees N requests in
// flight before it writes any reply (one SearchImages stream per search).
// ProcessImages is not part of this test.

#include <app/AppContext.h>
#include <app/Config.h>
#include <app/WorkerPool.h>
#include <features/media_thumbnail/MediaExtractor.h>
#include <features/media_thumbnail/SearchHandler.h>
#include <infra/Outbox.h>
#include <td/telegram/td_api.h>
#include <telegram/FileDownloader.h>
#include <transport/GrpcTransport.h>
#include <transport/SearchImagesClient.h>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include "image_stream.grpc.pb.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace td_api = td::td_api;

namespace {

constexpr int kN = 4;
constexpr std::int64_t kChatId = -1'001'234'567'890LL;

class HoldingSender : public tg::telegram::ITdSender {
 public:
  std::string path;
  bool downloads_overlapped = false;

  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback) override {
    if (request->get_id() != td_api::downloadFile::ID) {
      td::ClientManager::Response response;
      response.object = td_api::make_object<td_api::ok>();
      callback(std::move(response));
      return 1;
    }
    const auto file_id =
        static_cast<const td_api::downloadFile&>(*request).file_id_;
    // ProcessImages keeps priority 32. Search must ask for less.
    EXPECT_LT(static_cast<const td_api::downloadFile&>(*request).priority_, 32);
    td::ClientManager::Response response;
    auto file = td_api::make_object<td_api::file>();
    file->id_ = file_id;
    auto local = td_api::make_object<td_api::localFile>();
    local->path_ = path;
    local->is_downloading_completed_ = true;
    file->local_ = std::move(local);
    response.object = std::move(file);

    std::vector<tg::telegram::ResponseCallback> fire;
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++starts_;
      held_.push_back(std::move(callback));
      held_responses_.push_back(std::move(response));
      if (starts_ >= kN) {
        downloads_overlapped = fired_ == 0;
        fire.swap(held_);
        // responses moved below
      } else {
        return 1;
      }
    }
    std::vector<td::ClientManager::Response> responses;
    {
      std::lock_guard<std::mutex> lock(mu_);
      responses.swap(held_responses_);
      fired_ += static_cast<int>(fire.size());
    }
    for (std::size_t i = 0; i < fire.size(); ++i) {
      fire[i](std::move(responses[i]));
    }
    return 1;
  }

 private:
  std::mutex mu_;
  int starts_ = 0;
  int fired_ = 0;
  std::vector<tg::telegram::ResponseCallback> held_;
  std::vector<td::ClientManager::Response> held_responses_;
};

class OverlapService : public ImageProcessor::Service {
 public:
  std::atomic<int> active{0};
  std::atomic<int> peak{0};
  std::atomic<int> replied{0};
  // Replies already sent at the moment `peak` first reached kN. 0 means
  // every request was in flight before any reply was written.
  std::atomic<int> replied_when_full{-1};

  ::grpc::Status ProcessImages(
      ::grpc::ServerContext*,
      ::grpc::ServerReaderWriter<::ImageReply, ::ImageRequest>*) override {
    return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "not used");
  }

  ::grpc::Status SearchImages(
      ::grpc::ServerContext*,
      ::grpc::ServerReaderWriter<::SearchRsp, ::SearchReq>* stream) override {
    ::SearchReq req;
    if (!stream->Read(&req)) return ::grpc::Status::OK;
    const int now = active.fetch_add(1) + 1;
    int seen = peak.load();
    while (seen < now && !peak.compare_exchange_weak(seen, now)) {
    }
    if (now >= kN) {
      int expected = -1;
      replied_when_full.compare_exchange_strong(expected, replied.load());
    }
    // Stay inside the RPC without holding a lock so the other streams'
    // handlers can Read and count as in flight before anyone replies.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ::SearchRsp rsp;
    rsp.set_id(req.id());
    rsp.set_oid(req.oid());
    rsp.set_result("ok");
    stream->Write(rsp);
    active.fetch_sub(1);
    replied.fetch_add(1);
    return ::grpc::Status::OK;
  }
};

td_api::object_ptr<td_api::message> Photo(std::int64_t message_id,
                                          std::int32_t file_id) {
  auto message = td_api::make_object<td_api::message>();
  message->chat_id_ = kChatId;
  message->id_ = message_id;
  auto content = td_api::make_object<td_api::messagePhoto>();
  auto photo = td_api::make_object<td_api::photo>();
  auto size = td_api::make_object<td_api::photoSize>();
  size->type_ = "y";
  size->width_ = 320;
  size->height_ = 240;
  auto file = td_api::make_object<td_api::file>();
  file->id_ = file_id;
  auto remote = td_api::make_object<td_api::remoteFile>();
  remote->id_ = "r" + std::to_string(file_id);
  remote->unique_id_ = "u" + std::to_string(file_id);
  file->remote_ = std::move(remote);
  file->local_ = td_api::make_object<td_api::localFile>();
  size->photo_ = std::move(file);
  photo->sizes_.push_back(std::move(size));
  content->photo_ = std::move(photo);
  message->content_ = std::move(content);
  return message;
}

}  // namespace

TEST(SearchParallel, DownloadsAndRpcsOverlapBeforeAnyReply) {
  const std::string thumb =
      "/tmp/tg_search_parallel_thumb_" + std::to_string(getpid()) + ".bin";
  const std::string sock =
      "/tmp/tg_search_parallel_" + std::to_string(getpid()) + ".sock";
  const std::string db =
      "/tmp/tg_search_parallel_" + std::to_string(getpid()) + ".sqlite";
  std::remove(sock.c_str());
  std::remove(db.c_str());
  {
    std::ofstream out(thumb, std::ios::binary);
    out << "thumb";
  }

  OverlapService service;
  ::grpc::ServerBuilder builder;
  builder.SetSyncServerOption(::grpc::ServerBuilder::NUM_CQS, kN);
  builder.SetSyncServerOption(::grpc::ServerBuilder::MIN_POLLERS, kN);
  builder.SetSyncServerOption(::grpc::ServerBuilder::MAX_POLLERS, kN * 2);
  builder.AddListeningPort("unix://" + sock, ::grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);

  tg::app::GrpcConfig grpc;
  grpc.target = "unix://" + sock;
  grpc.tls = false;
  tg::transport::GrpcTransport transport(grpc);

  std::vector<std::unique_ptr<tg::transport::SearchImagesClient>> clients;
  std::vector<tg::transport::SearchImagesStreamer::Options> options;
  for (int i = 0; i < kN; ++i) {
    clients.push_back(
        std::make_unique<tg::transport::SearchImagesClient>(transport, 0));
    tg::transport::SearchImagesStreamer::Options opt;
    opt.name = "SearchImages#" + std::to_string(i);
    opt.start_fn = clients.back()->start_fn();
    opt.make_context = [&transport] { return transport.MakeContext(); };
    opt.pump_interval_ms = 10;
    opt.initial_backoff_ms = 20;
    opt.max_backoff_ms = 100;
    options.push_back(std::move(opt));
  }
  tg::transport::StreamerPool<SearchReq, SearchRsp> pool(std::move(options));
  pool.Start();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!pool.IsStreamRunning() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(pool.IsStreamRunning());

  HoldingSender sender;
  sender.path = thumb;
  tg::telegram::FileDownloader files(sender);
  tg::app::WorkerPool workers(4);
  tg::features::media_thumbnail::SearchHandler handler(
      tg::features::media_thumbnail::MediaExtractor({}), sender, files);
  tg::infra::Outbox outbox(db);
  tg::app::Config config;
  tg::transport::StreamerPool<ImageRequest, ImageReply> process(
      std::vector<tg::transport::ProcessImagesStreamer::Options>{
          tg::transport::ProcessImagesStreamer::Options{"ProcessImages"}});
  tg::app::AppContext ctx{workers, sender, files, process, pool, outbox, config};

  for (int i = 0; i < kN; ++i) {
    auto message = Photo(100 + i, 1000 + i);
    handler.SendSearchImages(*message, ctx);
  }

  const auto reply_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (service.replied.load() < kN &&
         std::chrono::steady_clock::now() < reply_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_GE(service.peak.load(), kN) << "peak in flight " << service.peak.load();
  EXPECT_GE(service.replied.load(), kN);
  EXPECT_EQ(service.replied_when_full.load(), 0);
  EXPECT_TRUE(sender.downloads_overlapped);
  workers.WaitForIdle(std::chrono::seconds(2));

  pool.Stop();
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
  std::remove(thumb.c_str());
  std::remove(sock.c_str());
  std::remove(db.c_str());
}
