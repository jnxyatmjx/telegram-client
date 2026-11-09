#include "telegram/FileDownloader.h"

#include <common/Logger.h>
#include <telegram/TelegramClient.h>

#include <stdexcept>
#include <utility>

namespace tg::telegram {

FileDownloader::FileDownloader(ITdSender& sender) : sender_(sender) {}

void FileDownloader::DownloadAsync(std::int32_t file_id,
                                   DownloadCallback on_done) {
  bool already_pending = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& callbacks = pending_[file_id];
    callbacks.push_back(std::move(on_done));
    already_pending = callbacks.size() > 1;
  }
  if (already_pending) {
    // A downloadFile request for this file is already in flight; the
    // callback rides along and fires when it completes. No second request.
    return;
  }

  auto request = td_api::make_object<td_api::downloadFile>();
  request->file_id_ = file_id;
  request->priority_ = 32;  // arbitrary medium priority
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
      FailOne(id, "downloadFile failed: code " + std::to_string(error->code_) +
                      ": " + error->message_);
      return;
    }
    if (response.object->get_id() == td_api::file::ID) {
      const auto file = td_api::move_object_as<td_api::file>(response.object);
      MaybeComplete(id, *file);
    }
  });
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
  }
  for (auto& callback : callbacks) {
    callback(DownloadResult{"", error});
  }
}

void FileDownloader::FailAll(const std::string& reason) {
  std::map<std::int32_t, std::vector<DownloadCallback>> failed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    failed = std::move(pending_);
    pending_.clear();
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
