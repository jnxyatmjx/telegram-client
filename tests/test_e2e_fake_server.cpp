// Fake-server end-to-end (chunk 3; IMPLEMENTATION_TASK.md acceptance test):
// a local thumbnail file through the REAL pipeline — MediaThumbnailHandler /
// SearchHandler -> real FileDownloader (a fake ITdSender answers downloadFile
// with the local file) -> real GrpcTransport + ProcessImages/SearchImages
// clients + AsyncStreamers -> in-process fake ImageProcessor server -> the
// reply round-trips by image_id / (oid, id), and the Outbox record goes
// PENDING -> DONE. No live Telegram login, no real server.
//
// The fake server listens on a Unix domain socket, not TCP loopback: this
// cloud VM's runtime intercepts direct TCP connections (other_tcp is
// denied), so a 127.0.0.1 loopback client gets the platform's block message
// instead of the server's HTTP/2 SETTINGS frame. The pipeline under test is
// transport-agnostic — production still uses a TCP target from config.

#include <app/AppContext.h>
#include <app/Config.h>
#include <common/Idempotency.h>
#include <features/media_thumbnail/MediaExtractor.h>
#include <features/media_thumbnail/MediaThumbnailHandler.h>
#include <features/media_thumbnail/SearchHandler.h>
#include <infra/Outbox.h>
#include <td/telegram/td_api.h>
#include <telegram/FileDownloader.h>
#include <telegram/TelegramClient.h>
#include <transport/GrpcTransport.h>
#include <transport/ProcessImagesClient.h>
#include <transport/SearchImagesClient.h>

#include <grpcpp/grpcpp.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "image_stream.grpc.pb.h"
#include "image_stream.pb.h"

namespace td_api = td::td_api;  // shorthand for td::td_api::*
namespace media = tg::features::media_thumbnail;
namespace app = tg::app;
namespace infra = tg::infra;

namespace {

// The thumbnail on disk: a small fake JPEG (SOI marker + payload). The
// pipeline never decodes it — it only reads the bytes and streams them.
const std::string kThumbBytes =
    std::string("\xff\xd8\xff\xe0", 4) + "fake-thumbnail-payload-0123456789";

constexpr std::int64_t kChatId = -1'001'234'567'890LL;  // channel dialog id
constexpr std::int64_t kMessageId = 777;
constexpr std::uint32_t kBareOid = 1'234'567'890;  // -chat_id - 1e12
constexpr std::uint32_t kBareId = 777;

std::string TempPath(const std::string& tag) {
  return "/tmp/tg-e2e-" + tag + "-" +
         std::to_string(static_cast<long long>(::getpid()));
}

// Fake ImageProcessor server: echoes every request, and records what it
// received so the test can assert memo / image_data / owner_id.
class FakeImageProcessor final : public ImageProcessor::Service {
 public:
  ::grpc::Status ProcessImages(
      ::grpc::ServerContext* /*context*/,
      ::grpc::ServerReaderWriter<::ImageReply, ::ImageRequest>* stream)
      override {
    ::ImageRequest req;
    while (stream->Read(&req)) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        process_requests_.push_back(req);
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
      {
        std::lock_guard<std::mutex> lock(mutex_);
        search_requests_.push_back(req);
      }
      ::SearchRsp rsp;
      rsp.set_id(req.id());
      rsp.set_oid(req.oid());
      rsp.set_result("found:" + std::to_string(req.oid()) + ":" +
                     std::to_string(req.id()));
      if (!stream->Write(rsp)) break;
    }
    return ::grpc::Status::OK;
  }

  std::vector<::ImageRequest> process_requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return process_requests_;
  }
  std::vector<::SearchReq> search_requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return search_requests_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<::ImageRequest> process_requests_;
  std::vector<::SearchReq> search_requests_;
};

// Fake TDLib sender: answers downloadFile inline with a file whose local
// path is the thumbnail on disk (download already complete). Everything
// else is an error — the pipeline must not ask for anything else.
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
      auto local = td_api::make_object<td_api::localFile>();
      local->path_ = thumbnail_path;
      local->is_downloading_completed_ = true;
      file->local_ = std::move(local);
      response.object = std::move(file);
    } else {
      response.object =
          td_api::make_object<td_api::error>(400, "unexpected request");
    }
    callback(std::move(response));
    return static_cast<std::int64_t>(response.request_id);
  }

 private:
  std::uint64_t next_id_ = 1;
};

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

td_api::object_ptr<td_api::message> MakePhotoMessage() {
  auto message = td_api::make_object<td_api::message>();
  message->id_ = kMessageId;
  message->chat_id_ = kChatId;
  auto content = td_api::make_object<td_api::messagePhoto>();
  auto photo = td_api::make_object<td_api::photo>();
  photo->has_stickers_ = false;
  photo->sizes_.push_back(MakeSize("s", 100, 100, 1, "u-s"));
  photo->sizes_.push_back(MakeSize("m", 320, 240, 2, "u-m"));  // picked tier
  photo->sizes_.push_back(MakeSize("x", 640, 640, 3, "u-x"));
  photo->sizes_.push_back(MakeSize("y", 1280, 1280, 4, "u-y"));
  content->photo_ = std::move(photo);
  auto caption = td_api::make_object<td_api::formattedText>();
  caption->text_ = "hello caption";
  content->caption_ = std::move(caption);
  message->content_ = std::move(content);
  return message;
}

}  // namespace

class E2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    socket_path_ = TempPath("sock") + ".sock";
    thumb_path_ = TempPath("thumb") + ".bin";
    db_path_ = TempPath("outbox") + ".sqlite";
    std::remove(socket_path_.c_str());
    std::remove(thumb_path_.c_str());
    std::remove(db_path_.c_str());

    {  // The local thumbnail file the "download" resolves to.
      std::ofstream out(thumb_path_, std::ios::binary);
      ASSERT_TRUE(out.good());
      out.write(kThumbBytes.data(),
                static_cast<std::streamsize>(kThumbBytes.size()));
    }
    sender_.thumbnail_path = thumb_path_;

    // Fake server on the unix socket.
    ::grpc::ServerBuilder builder;
    builder.AddListeningPort("unix://" + socket_path_,
                             ::grpc::InsecureServerCredentials());
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);

    // Real transport + clients + streamers over the same channel.
    config_.grpc.target = "unix://" + socket_path_;
    config_.grpc.tls = false;
    transport_ = std::make_unique<tg::transport::GrpcTransport>(config_.grpc);
    process_client_ =
        std::make_unique<tg::transport::ProcessImagesClient>(*transport_);
    search_client_ =
        std::make_unique<tg::transport::SearchImagesClient>(*transport_);

    tg::transport::ProcessImagesStreamer::Options process_options;
    process_options.name = "ProcessImages";
    process_options.start_fn = process_client_->start_fn();
    process_options.make_context = [this] {
      return transport_->MakeContext();
    };
    process_options.pump_interval_ms = 10;
    process_options.initial_backoff_ms = 50;
    process_options.max_backoff_ms = 200;
    process_streamer_ = std::make_unique<tg::transport::ProcessImagesStreamer>(
        std::move(process_options));

    tg::transport::SearchImagesStreamer::Options search_options;
    search_options.name = "SearchImages";
    search_options.start_fn = search_client_->start_fn();
    search_options.make_context = [this] {
      return transport_->MakeContext();
    };
    search_options.pump_interval_ms = 10;
    search_options.initial_backoff_ms = 50;
    search_options.max_backoff_ms = 200;
    search_streamer_ = std::make_unique<tg::transport::SearchImagesStreamer>(
        std::move(search_options));

    // Reply handlers wired exactly like Application::WireStreamerCallbacks.
    process_streamer_->SetReplyHandler([this](const ImageReply& reply) {
      const bool known = outbox_->MarkDone(reply.image_id(), reply.result());
      EXPECT_TRUE(known) << "reply for unknown image_id " << reply.image_id();
      std::lock_guard<std::mutex> lock(mutex_);
      process_replies_.push_back(reply);
      cv_.notify_all();
    });
    search_streamer_->SetReplyHandler([this](const SearchRsp& reply) {
      const auto entry =
          search_handler_->correlation().Lookup(reply.oid(), reply.id());
      std::lock_guard<std::mutex> lock(mutex_);
      if (entry.has_value()) resolved_search_ = *entry;
      search_replies_.push_back(reply);
      cv_.notify_all();
    });

    outbox_ = std::make_unique<infra::Outbox>(db_path_);
    media::MediaExtractor extractor(config_.media_thumbnail);
    process_handler_ = std::make_unique<media::MediaThumbnailHandler>(
        extractor, sender_, files_);
    search_handler_ = std::make_unique<media::SearchHandler>(extractor, sender_,
                                                            files_);
    ctx_ = std::make_unique<app::AppContext>(app::AppContext{
        sender_, files_, *process_streamer_, *search_streamer_, *outbox_,
        config_});

    process_streamer_->Start();
    search_streamer_->Start();
    ASSERT_TRUE(WaitForRunning(*process_streamer_));
    ASSERT_TRUE(WaitForRunning(*search_streamer_));
  }

  void TearDown() override {
    // Streamers first (they drain their streams), then the server — the
    // order the AsyncStreamer teardown fix requires.
    if (process_streamer_) process_streamer_->Stop();
    if (search_streamer_) search_streamer_->Stop();
    if (server_ != nullptr) {
      server_->Shutdown(std::chrono::system_clock::now() +
                        std::chrono::seconds(2));
      server_->Wait();
      server_.reset();
    }
    ctx_.reset();
    search_handler_.reset();
    process_handler_.reset();
    outbox_.reset();
    std::remove(socket_path_.c_str());
    std::remove(thumb_path_.c_str());
    std::remove(db_path_.c_str());
  }

  bool WaitForRunning(const tg::transport::ProcessImagesStreamer& s) {
    return WaitForRunningImpl([&] { return s.IsStreamRunning(); });
  }
  bool WaitForRunning(const tg::transport::SearchImagesStreamer& s) {
    return WaitForRunningImpl([&] { return s.IsStreamRunning(); });
  }
  template <typename Pred>
  bool WaitForRunningImpl(Pred pred) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      if (pred()) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
  }
  bool WaitForProcessReplies(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(10),
                        [&] { return process_replies_.size() >= count; });
  }
  bool WaitForSearchReplies(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(10),
                        [&] { return search_replies_.size() >= count; });
  }

  std::string socket_path_;
  std::string thumb_path_;
  std::string db_path_;
  FakeImageProcessor service_;
  std::unique_ptr<::grpc::Server> server_;
  app::Config config_;
  std::unique_ptr<tg::transport::GrpcTransport> transport_;
  std::unique_ptr<tg::transport::ProcessImagesClient> process_client_;
  std::unique_ptr<tg::transport::SearchImagesClient> search_client_;
  std::unique_ptr<tg::transport::ProcessImagesStreamer> process_streamer_;
  std::unique_ptr<tg::transport::SearchImagesStreamer> search_streamer_;
  FakeTdSender sender_;
  tg::telegram::FileDownloader files_{sender_};
  std::unique_ptr<infra::Outbox> outbox_;
  std::unique_ptr<media::MediaThumbnailHandler> process_handler_;
  std::unique_ptr<media::SearchHandler> search_handler_;
  std::unique_ptr<app::AppContext> ctx_;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<ImageReply> process_replies_;
  std::vector<SearchRsp> search_replies_;
  std::optional<media::SearchCorrelationTable::Entry> resolved_search_;
};

TEST_F(E2ETest, ProcessImagesThumbnailFileRoundTrip) {
  const std::string expected_image_id =
      tg::common::MakeImageId(kChatId, kMessageId, "u-m");

  auto message = MakePhotoMessage();
  const auto result = process_handler_->SendProcessImages(*message, *ctx_);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.image_id, expected_image_id);

  // The reply round-trips by image_id, and the Outbox record is DONE.
  ASSERT_TRUE(WaitForProcessReplies(1));
  ASSERT_EQ(process_replies_.size(), 1u);
  EXPECT_EQ(process_replies_[0].image_id(), expected_image_id);
  EXPECT_EQ(process_replies_[0].result(), "ok:" + expected_image_id);

  const auto record = outbox_->Get(expected_image_id);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->state, infra::OutboxState::kDone);

  // The server received exactly the request the pipeline built from the
  // file: thumbnail bytes, caption-only memo, chat id as owner.
  const auto received = service_.process_requests();
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received[0].image_id(), expected_image_id);
  EXPECT_EQ(received[0].image_data(), kThumbBytes);
  EXPECT_EQ(received[0].memo(), "hello caption");
  EXPECT_EQ(received[0].owner_id(), kChatId);
}

TEST_F(E2ETest, SearchImagesBareIdRoundTrip) {
  const std::string expected_image_id =
      tg::common::MakeImageId(kChatId, kMessageId, "u-m");

  auto message = MakePhotoMessage();
  const auto result = search_handler_->SendSearchImages(*message, *ctx_);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.image_id, expected_image_id);

  // The reply round-trips by (oid, id) and resolves through the
  // correlation table back to the original message.
  ASSERT_TRUE(WaitForSearchReplies(1));
  ASSERT_EQ(search_replies_.size(), 1u);
  EXPECT_EQ(search_replies_[0].oid(), kBareOid);
  EXPECT_EQ(search_replies_[0].id(), kBareId);
  EXPECT_EQ(search_replies_[0].result(), "found:1234567890:777");

  ASSERT_TRUE(resolved_search_.has_value());
  EXPECT_EQ(resolved_search_->chat_id, kChatId);
  EXPECT_EQ(resolved_search_->message_id, kMessageId);
  EXPECT_EQ(resolved_search_->image_id, expected_image_id);

  // The server received the bare uint32 ids, never the TDLib dialog id.
  const auto received = service_.search_requests();
  ASSERT_EQ(received.size(), 1u);
  EXPECT_EQ(received[0].oid(), kBareOid);
  EXPECT_EQ(received[0].id(), kBareId);
  EXPECT_EQ(received[0].image_data(), kThumbBytes);
}
