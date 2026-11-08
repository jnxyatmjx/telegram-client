#include "telegram/FileDownloader.h"

#include <common/Logger.h>
#include <telegram/TelegramClient.h>

#include <stdexcept>
#include <utility>
#include <vector>

namespace tg::telegram {

FileDownloader::FileDownloader(ITdSender& sender) : sender_(sender) {}

std::future<std::string> FileDownloader::Download(std::int32_t file_id,
                                                  std::chrono::milliseconds timeout) {
  std::promise<std::string> promise;
  std::future<std::string> future = promise.get_future();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.emplace(file_id, std::move(promise));
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
      return;  // shutdown filler; FailAll handles the promise
    }
    if (response.object->get_id() == td_api::error::ID) {
      std::promise<std::string> promise;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pending_.find(id);
        if (it == pending_.end()) return;
        promise = std::move(it->second);
        pending_.erase(it);
      }
      const auto error = td_api::move_object_as<td_api::error>(response.object);
      promise.set_exception(std::make_exception_ptr(std::runtime_error(
          "downloadFile failed: code " + std::to_string(error->code_) + ": " +
          error->message_)));
      return;
    }
    if (response.object->get_id() == td_api::file::ID) {
      const auto file = td_api::move_object_as<td_api::file>(response.object);
      MaybeComplete(id, *file);
    }
  });

  (void)timeout;  // enforced by the caller waiting on the future (v5 §2.2)
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
  std::promise<std::string> promise;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = pending_.find(file_id);
    if (it != pending_.end()) {
      promise = std::move(it->second);
      pending_.erase(it);
      found = true;
    }
  }
  if (found) {
    // set_value on a worker thread blocking in .get() — safe, no lock held.
    promise.set_value(file.local_->path_);
    spdlog::info("file_downloader: file {} ready at {}", file_id,
                 file.local_->path_);
  }
}

void FileDownloader::FailAll(const std::string& reason) {
  std::vector<std::promise<std::string>> failed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    failed.reserve(pending_.size());
    for (auto& [id, promise] : pending_) {
      failed.push_back(std::move(promise));
    }
    pending_.clear();
  }
  for (auto& promise : failed) {
    promise.set_exception(
        std::make_exception_ptr(std::runtime_error("download aborted: " + reason)));
  }
}

size_t FileDownloader::pending_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_.size();
}

}  // namespace tg::telegram
