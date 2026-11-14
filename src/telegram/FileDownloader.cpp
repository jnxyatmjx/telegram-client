#include "telegram/FileDownloader.h"

#include <common/Logger.h>
#include <telegram/FloodRetry.h>
#include <telegram/TdErrors.h>
#include <telegram/TelegramClient.h>

#include <stdexcept>
#include <utility>

namespace tg::telegram {

FileDownloader::FileDownloader(ITdSender& sender, FloodRetry* flood_retry)
    : sender_(sender), flood_retry_(flood_retry) {}

void FileDownloader::DownloadAsync(std::int32_t file_id,
                                   DownloadCallback on_done,
                                   std::int32_t priority) {
  bool already_pending = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& callbacks = pending_[file_id];
    callbacks.push_back(std::move(on_done));
    already_pending = callbacks.size() > 1;
    // One consumer claim per call: the caller must Release(file_id) after
    // it has stored the bytes, and the cached file is deleted when the
    // last claim is dropped (deleteFile cache cleanup).
    ++use_count_[file_id];
    if (!already_pending) priority_[file_id] = priority;
  }
  if (already_pending) {
    // A downloadFile request for this file is already in flight (or its
    // flood retry is scheduled — the callbacks stay registered through a
    // FloodWait); the callback rides along and fires when it completes.
    // No second request, and the in-flight priority is left as-is.
    return;
  }
  SendDownloadFile(file_id);
}

void FileDownloader::Release(std::int32_t file_id) {
  bool delete_now = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = use_count_.find(file_id);
    // Defensive: no entry / already at zero — nothing to release.
    if (it == use_count_.end() || it->second <= 0) return;
    if (--it->second == 0) {
      use_count_.erase(it);
      delete_now = true;
    }
  }
  if (!delete_now) return;
  // Last consumer is done with the bytes: remove the cached local file
  // (telegram.files_directory) from TDLib's cache so completed downloads
  // do not accumulate on disk. Fire-and-forget, best-effort: an error is
  // logged and never retried, and it must never be fatal.
  spdlog::info("file_downloader: last consumer done — deleteFile {}",
               file_id);
  auto request = td_api::make_object<td_api::deleteFile>();
  request->file_id_ = file_id;
  sender_.Send(
      std::move(request),
      [file_id](td::ClientManager::Response&& response) {
        if (response.object == nullptr) return;  // shutdown filler
        if (response.object->get_id() == td_api::error::ID) {
          const auto error =
              td_api::move_object_as<td_api::error>(response.object);
          spdlog::warn("file_downloader: deleteFile {} failed (ignored): "
                       "code {}: {}", file_id, error->code_,
                       error->message_);
        }
      });
}

void FileDownloader::SendDownloadFile(std::int32_t file_id) {
  std::int32_t priority = 32;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = priority_.find(file_id);
    if (it != priority_.end()) priority = it->second;
  }
  auto request = td_api::make_object<td_api::downloadFile>();
  request->file_id_ = file_id;
  request->priority_ = priority;
  request->offset_ = 0;
  request->limit_ = 0;  // 0 -> TDLib picks its default chunk limit
  // synchronous=true: the response only arrives once the file is complete,
  // which makes this single request sufficient even if updateFile races.
  request->synchronous_ = true;

  const std::int32_t id = file_id;
  sender_.Send(std::move(request), [this, id](td::ClientManager::Response&& response) {
    if (response.object == nullptr) {
      return;  // shutdown filler; FailAll completes the pending callbacks
    }
    if (response.object->get_id() == td_api::error::ID) {
      const auto error = td_api::move_object_as<td_api::error>(response.object);
      if (IsFloodWait(*error)) {
        HandleFloodWait(id, *error);
        return;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        flood_attempts_.erase(id);
      }
      FailOne(id, "downloadFile failed: code " + std::to_string(error->code_) +
                      ": " + error->message_);
      return;
    }
    if (response.object->get_id() == td_api::file::ID) {
      const auto file = td_api::move_object_as<td_api::file>(response.object);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        flood_attempts_.erase(id);
      }
      MaybeComplete(id, *file);
    }
  });
}

void FileDownloader::HandleFloodWait(std::int32_t file_id,
                                     const td_api::error& error) {
  // Receive thread. The 429 means the download was refused, not applied:
  // the callbacks stay registered and the same downloadFile is re-issued
  // after retry-after + pad. attempt is tracked per file_id under mutex_.
  if (flood_retry_ == nullptr) {
    // Pre-chunk-3 behaviour (tests / unbuilt wiring): fail fast.
    FailOne(file_id, "downloadFile failed: code " +
                         std::to_string(error.code_) + ": " + error.message_);
    return;
  }
  int attempt = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_.count(file_id) == 0) return;  // already failed/completed
    attempt = ++flood_attempts_[file_id];
  }
  spdlog::warn("file_downloader: downloadFile {} FloodWait — retrying via "
               "flood scheduler", file_id);
  const bool scheduled = flood_retry_->Schedule(
      error, attempt, [this, file_id] {
        // Timer thread. FailAll may have cleared everything since; sending
        // a download nobody waits for would be pointless.
        bool still_wanted = false;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          still_wanted = pending_.count(file_id) > 0;
        }
        if (still_wanted) SendDownloadFile(file_id);
      });
  if (!scheduled) {
    int attempts = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      attempts = flood_attempts_[file_id];
      flood_attempts_.erase(file_id);
    }
    FailOne(file_id, "downloadFile still flood-limited after " +
                         std::to_string(attempts) +
                         " attempts — giving up");
  }
}

std::future<std::string> FileDownloader::Download(
    std::int32_t file_id, std::chrono::milliseconds /*timeout*/) {
  // Timeout is enforced by the caller via future::wait_for, as before.
  auto promise = std::make_shared<std::promise<std::string>>();
  auto future = promise->get_future();
  DownloadAsync(file_id, [promise](DownloadResult result) {
    if (result.error.empty()) {
      promise->set_value(std::move(result.local_path));
    } else {
      promise->set_exception(
          std::make_exception_ptr(std::runtime_error(result.error)));
    }
  });
  return future;
}

void FileDownloader::OnUpdateFile(const td_api::updateFile& update) {
  if (update.file_ == nullptr) return;
  MaybeComplete(update.file_->id_, *update.file_);
}

void FileDownloader::MaybeComplete(std::int32_t file_id, const td_api::file& file) {
  if (file.local_ == nullptr || !file.local_->is_downloading_completed_ ||
      file.local_->path_.empty()) {
    return;
  }
  std::vector<DownloadCallback> callbacks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pending_.find(file_id);
    if (it == pending_.end()) return;
    callbacks = std::move(it->second);
    pending_.erase(it);
    priority_.erase(file_id);
  }
  // Invoke WITHOUT holding the mutex: the callback posts to a worker pool
  // and must never block the Receive thread.
  const std::string path = file.local_->path_;
  spdlog::info("file_downloader: file {} ready at {}", file_id, path);
  for (auto& callback : callbacks) {
    callback(DownloadResult{path, ""});
  }
}

void FileDownloader::FailOne(std::int32_t file_id, const std::string& error) {
  std::vector<DownloadCallback> callbacks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pending_.find(file_id);
    if (it == pending_.end()) return;
    callbacks = std::move(it->second);
    pending_.erase(it);
    priority_.erase(file_id);
    // The callbacks fire with an error, so no consumer will use the file:
    // drop one claim per fired callback. Decrement — do NOT erase the
    // whole entry — because an earlier successful download of the same
    // file_id may still have live consumers holding claims (they Release
    // later). NO deleteFile on failure: the file may not exist or be
    // partial; TDLib's cache is left as-is (pre-existing behaviour).
    int& count = use_count_[file_id];
    count -= static_cast<int>(callbacks.size());
    if (count <= 0) use_count_.erase(file_id);
  }
  for (auto& callback : callbacks) {
    callback(DownloadResult{"", error});
  }
}

void FileDownloader::FailAll(const std::string& reason) {
  // Shutdown path. flood_retry_ is NOT shut down here — it may be shared
  // (Application passes one instance around), and Application already
  // Shutdown()s it before tearing Telegram down. A retry that still fires
  // after this point is a no-op: pending_ was just cleared, and the
  // closure's still_wanted check refuses to send a download nobody
  // waits for.
  std::map<std::int32_t, std::vector<DownloadCallback>> failed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    failed = std::move(pending_);
    pending_.clear();
    flood_attempts_.clear();
    priority_.clear();
    // Shutdown: the process is exiting, so no consumer will consume and no
    // deleteFile is sent (clearing the claims prevents a straggler Release
    // from deleting during teardown).
    use_count_.clear();
  }
  for (auto& [id, callbacks] : failed) {
    (void)id;
    for (auto& callback : callbacks) {
      callback(DownloadResult{"", "download aborted: " + reason});
    }
  }
}

size_t FileDownloader::pending_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_.size();
}

}  // namespace tg::telegram
