// deleteFile cache cleanup (Hong requirement, 2026-11-11): downloaded files
// must not accumulate in TDLib's files_directory. FileDownloader now
// reference-counts consumers (one per DownloadAsync call; shared file_ids
// share one download and one cached file) and sends td_api::deleteFile
// exactly once, when the LAST consumer releases its claim. Handlers call
// Release after the bytes are safe (Outbox BLOB for ProcessImages /
// enqueued SearchReq for SearchImages). A failed download deletes nothing;
// Release of an unknown file_id is a no-op; a deleteFile error is ignored
// (best-effort cleanup, never fatal).
//
// NOTE: these tests use DownloadAsync + explicit Release (NOT the
// future-based Download() wrapper): Download() also registers a consumer,
// and its test callers never Release — correct for the per-test instances
// here, but the wrapper would leak the refcount inside these tests.
#include <telegram/FileDownloader.h>
#include <telegram/TelegramClient.h>

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace td_api = td::td_api;
namespace telegram = tg::telegram;

namespace {

constexpr auto kWait = std::chrono::seconds(5);
// Bounded wait for a parked downloadFile response, so a failed ASSERT
// cannot hang teardown.
constexpr auto kGateTimeout = std::chrono::seconds(10);

// Records every request by TDLib type id. downloadFile is answered with a
// completed file; deleteFile is answered with ok (optionally with an error
// to prove the failure path is ignored); anything else gets a generic
// error. DownloadFile responses can be parked on a background thread
// (ArmDownloadGate) so a test can register a second consumer while the
// first download is still in flight — the concurrent-dedup path.
class RecordingFakeSender : public telegram::ITdSender {
 public:
  ~RecordingFakeSender() override { JoinDeferred(); }

  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    telegram::ResponseCallback callback) override {
    td::ClientManager::Response response;
    response.request_id = next_id_++;
    if (request->get_id() == td_api::downloadFile::ID) {
      const std::int32_t file_id =
          static_cast<td_api::downloadFile*>(request.get())->file_id_;
      Record(td_api::downloadFile::ID);
      download_file_ids_.push_back(file_id);
      if (download_error_left > 0) {
        --download_error_left;
        response.object = td_api::make_object<td_api::error>(
            400, "File could not be downloaded");
      } else {
        response.object = CompletedFile(file_id);
      }
      if (gate_armed_) {
        auto future = gate_future_;  // shared: safe across threads
        deferred_threads_.emplace_back(
            [cb = std::move(callback), resp = std::move(response),
             future]() mutable {
              future.wait_for(kGateTimeout);
              cb(std::move(resp));
            });
        return static_cast<std::int64_t>(response.request_id);
      }
    } else if (request->get_id() == td_api::deleteFile::ID) {
      const std::int32_t file_id =
          static_cast<td_api::deleteFile*>(request.get())->file_id_;
      Record(td_api::deleteFile::ID);
      delete_file_ids_.push_back(file_id);
      if (delete_error_left > 0) {
        --delete_error_left;
        response.object =
            td_api::make_object<td_api::error>(404, "File not found");
      } else {
        response.object = td_api::make_object<td_api::ok>();
      }
    } else {
      Record(request->get_id());
      response.object =
          td_api::make_object<td_api::error>(400, "unexpected request");
    }
    callback(std::move(response));
    return static_cast<std::int64_t>(response.request_id);
  }

  // Park subsequent downloadFile responses on a background thread until
  // OpenDownloadGate: the request is "in flight" while more consumers
  // register for the same file_id.
  void ArmDownloadGate() {
    auto gate = std::make_unique<std::promise<void>>();
    gate_future_ = gate->get_future().share();
    gate_ = std::move(gate);
    gate_armed_ = true;
  }

  void OpenDownloadGate() {
    gate_armed_ = false;
    gate_->set_value();
  }

  void JoinDeferred() {
    for (auto& t : deferred_threads_) {
      if (t.joinable()) t.join();
    }
    deferred_threads_.clear();
  }

  int sent_count(std::int32_t type_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sent_by_type_.find(type_id);
    return it != sent_by_type_.end() ? it->second : 0;
  }

  int delete_count(std::int32_t file_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    int n = 0;
    for (const auto id : delete_file_ids_) {
      if (id == file_id) ++n;
    }
    return n;
  }

  // N downloadFile requests are answered with an error (download failure).
  int download_error_left = 0;
  // N deleteFile requests are answered with an error (best-effort proof).
  int delete_error_left = 0;

 private:
  static td_api::object_ptr<td_api::Object> CompletedFile(
      std::int32_t file_id) {
    auto file = td_api::make_object<td_api::file>();
    file->id_ = file_id;
    auto remote = td_api::make_object<td_api::remoteFile>();
    remote->unique_id_ = "uid-" + std::to_string(file_id);
    file->remote_ = std::move(remote);
    auto local = td_api::make_object<td_api::localFile>();
    local->path_ = "/tmp/tg_client_delete_test_" + std::to_string(file_id);
    local->is_downloading_completed_ = true;
    file->local_ = std::move(local);
    return file;
  }

  void Record(std::int32_t type_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++sent_by_type_[type_id];
  }

  std::uint64_t next_id_ = 1;
  mutable std::mutex mutex_;
  std::map<std::int32_t, int> sent_by_type_;
  std::vector<std::int32_t> download_file_ids_;
  std::vector<std::int32_t> delete_file_ids_;
  std::atomic<bool> gate_armed_{false};
  std::unique_ptr<std::promise<void>> gate_;
  std::shared_future<void> gate_future_;
  std::vector<std::thread> deferred_threads_;
};

// One consumer: download -> Release -> deleteFile exactly once, with the
// right file_id, and only AFTER the callback delivered the bytes.
TEST(DeleteFileCleanup, LastReleaseSendsDeleteFileOnce) {
  RecordingFakeSender sender;
  telegram::FileDownloader files(sender);

  std::promise<std::string> done;
  files.DownloadAsync(4242, [&done](telegram::FileDownloader::DownloadResult r) {
    if (r.error.empty()) done.set_value(r.local_path);
    else done.set_exception(std::make_exception_ptr(std::runtime_error(r.error)));
  });
  auto future = done.get_future();
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(future.get(), "/tmp/tg_client_delete_test_4242");

  // The consumer holds the claim: no delete while the file is in use.
  EXPECT_EQ(sender.sent_count(td_api::deleteFile::ID), 0);

  files.Release(4242);
  EXPECT_EQ(sender.sent_count(td_api::deleteFile::ID), 1);
  EXPECT_EQ(sender.delete_count(4242), 1);  // the right file_id
  // The download itself was a single request (nothing else re-sent).
  EXPECT_EQ(sender.sent_count(td_api::downloadFile::ID), 1);
}

// Shared file_id (forwarded photo / same message processed twice): with
// the download genuinely in flight, both consumers get the bytes from ONE
// downloadFile request; the first Release must not delete — only the last
// one may, exactly once. (Sequential consumers — the second DownloadAsync
// arriving after the first download already completed — each get their own
// download; the refcount covers that case too via the same Release pairs.)
TEST(DeleteFileCleanup, SharedFileIdDeletesOnlyAtTheLastRelease) {
  RecordingFakeSender sender;
  telegram::FileDownloader files(sender);

  std::promise<std::string> first_promise, second_promise;
  auto first = first_promise.get_future();
  auto second = second_promise.get_future();

  // Park the downloadFile response: both consumers must be able to
  // register while the download is still in flight.
  sender.ArmDownloadGate();
  files.DownloadAsync(4242, [&first_promise](telegram::FileDownloader::DownloadResult r) {
    if (r.error.empty()) first_promise.set_value(r.local_path);
    else first_promise.set_exception(std::make_exception_ptr(std::runtime_error(r.error)));
  });
  files.DownloadAsync(4242, [&second_promise](telegram::FileDownloader::DownloadResult r) {
    if (r.error.empty()) second_promise.set_value(r.local_path);
    else second_promise.set_exception(std::make_exception_ptr(std::runtime_error(r.error)));
  });
  EXPECT_EQ(sender.sent_count(td_api::downloadFile::ID), 1);  // shared request

  sender.OpenDownloadGate();  // release: both callbacks fire
  ASSERT_EQ(first.wait_for(kWait), std::future_status::ready);
  ASSERT_EQ(second.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(first.get(), second.get());  // both callbacks, same path
  sender.JoinDeferred();

  // First consumer done: the other still needs the file — no delete yet.
  files.Release(4242);
  EXPECT_EQ(sender.sent_count(td_api::deleteFile::ID), 0);

  // Last consumer done: exactly one deleteFile.
  files.Release(4242);
  EXPECT_EQ(sender.sent_count(td_api::deleteFile::ID), 1);
  EXPECT_EQ(sender.delete_count(4242), 1);
}

// A failed download fires the callbacks with an error and no consumer will
// use the file — the claims are dropped and NOTHING is deleted (the file
// may not exist / be partial; TDLib cache is left as-is). The later
// Release from the (never-consumed) consumer is a defensive no-op.
TEST(DeleteFileCleanup, FailedDownloadSendsNoDeleteFile) {
  RecordingFakeSender sender;
  telegram::FileDownloader files(sender);
  sender.download_error_left = 1;

  std::promise<std::string> failed;
  files.DownloadAsync(4242, [&failed](telegram::FileDownloader::DownloadResult r) {
    if (r.error.empty()) failed.set_value(r.local_path);
    else failed.set_exception(std::make_exception_ptr(std::runtime_error(r.error)));
  });
  auto future = failed.get_future();
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_THROW(future.get(), std::runtime_error);  // the error path fired

  files.Release(4242);  // claim already dropped by the failure: no-op
  EXPECT_EQ(sender.sent_count(td_api::deleteFile::ID), 0);
  // The FileDownloader stays usable: a retry download still completes and
  // its cleanup still works.
  std::promise<std::string> retried;
  files.DownloadAsync(4242, [&retried](telegram::FileDownloader::DownloadResult r) {
    if (r.error.empty()) retried.set_value(r.local_path);
    else retried.set_exception(std::make_exception_ptr(std::runtime_error(r.error)));
  });
  ASSERT_EQ(retried.get_future().wait_for(kWait), std::future_status::ready);
  files.Release(4242);
  EXPECT_EQ(sender.sent_count(td_api::deleteFile::ID), 1);
}

// Defensive: Release of a file_id that was never downloaded, and a double
// Release of a downloaded one — no crash, no extra deleteFile.
TEST(DeleteFileCleanup, ReleaseWithoutDownloadOrDoubleReleaseIsANoOp) {
  RecordingFakeSender sender;
  telegram::FileDownloader files(sender);

  files.Release(999);  // never downloaded
  EXPECT_EQ(sender.sent_count(td_api::deleteFile::ID), 0);

  std::promise<std::string> done;
  files.DownloadAsync(4242, [&done](telegram::FileDownloader::DownloadResult r) {
    if (r.error.empty()) done.set_value(r.local_path);
    else done.set_exception(std::make_exception_ptr(std::runtime_error(r.error)));
  });
  ASSERT_EQ(done.get_future().wait_for(kWait), std::future_status::ready);

  files.Release(4242);
  EXPECT_EQ(sender.sent_count(td_api::deleteFile::ID), 1);
  files.Release(4242);  // double release: count already gone
  files.Release(999);   // still unknown
  EXPECT_EQ(sender.sent_count(td_api::deleteFile::ID), 1);
  EXPECT_EQ(sender.delete_count(4242), 1);  // exactly once
}

// The delete is best-effort: a deleteFile error response is ignored and
// must not break the downloader — the next file's lifecycle still works.
TEST(DeleteFileCleanup, DeleteFileErrorIsIgnored) {
  RecordingFakeSender sender;
  telegram::FileDownloader files(sender);
  sender.delete_error_left = 1;  // the first deleteFile "fails"

  std::promise<std::string> done;
  files.DownloadAsync(4242, [&done](telegram::FileDownloader::DownloadResult r) {
    if (r.error.empty()) done.set_value(r.local_path);
    else done.set_exception(std::make_exception_ptr(std::runtime_error(r.error)));
  });
  ASSERT_EQ(done.get_future().wait_for(kWait), std::future_status::ready);
  files.Release(4242);  // delete answered with an error — swallowed

  std::promise<std::string> second;
  files.DownloadAsync(4243, [&second](telegram::FileDownloader::DownloadResult r) {
    if (r.error.empty()) second.set_value(r.local_path);
    else second.set_exception(std::make_exception_ptr(std::runtime_error(r.error)));
  });
  ASSERT_EQ(second.get_future().wait_for(kWait), std::future_status::ready);
  files.Release(4243);  // this one succeeds
  EXPECT_EQ(sender.delete_count(4243), 1);
}

}  // namespace
