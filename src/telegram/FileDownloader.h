#ifndef TG_TELEGRAM_FILEDOWNLOADER_H_
#define TG_TELEGRAM_FILEDOWNLOADER_H_

// FileDownloader (architecture v5 §2.2 step [2]-[3]): fully async — no
// thread ever blocks waiting for a download.
//
// The production path is DownloadAsync(): the caller passes a completion
// callback and returns immediately; the callback fires when the download
// completes (downloadFile response with synchronous=true) or fails, or
// from an updateFile relayed by TelegramClient's Receive thread —
// whichever completes first wins. Concurrent downloads of the same file
// share one downloadFile request; every registered callback fires.
//
// The callback runs on the TDLib Receive thread (or inline, on the
// calling thread, when the sender answers synchronously, e.g. in tests).
// It MUST be cheap: post the next pipeline stage to a worker pool, never
// block, never do file I/O. No worker thread is ever occupied by a
// download in flight — that is the whole point: one message's slow
// download cannot block another message's processing.
//
// The future-based Download() is a thin wrapper for tests / simple
// callers: the timeout is enforced by the caller via future::wait_for, as
// before. The async core itself has no timer — a download that never
// completes simply never resolves its callback, and no thread is blocked
// waiting for it (TDLib itself completes or fails the download).
//
// Receive thread rules: OnUpdateFile only completes callbacks (cheap); the
// map is guarded by one mutex (v5 §3.3). Callbacks are always invoked
// WITHOUT holding the mutex.

#include <td/telegram/td_api.h>

#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace tg::telegram {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

class ITdSender;

class FileDownloader {
 public:
  // Exactly one of the two fields is set.
  struct DownloadResult {
    std::string local_path;  // set on success
    std::string error;       // set on failure (local_path empty)
  };
  using DownloadCallback = std::function<void(DownloadResult result)>;

  explicit FileDownloader(ITdSender& sender);

  // Async: registers on_done and returns immediately. No thread blocks.
  void DownloadAsync(std::int32_t file_id, DownloadCallback on_done);

  // Future-based wrapper (tests / simple callers): resolves with the local
  // file path once downloaded; rejects (std::runtime_error) on request
  // error. `timeout` is NOT enforced here — the caller waits on the
  // future with wait_for(timeout), as before.
  std::future<std::string> Download(
      std::int32_t file_id,
      std::chrono::milliseconds timeout = std::chrono::seconds(30));

  // TelegramClient Receive thread.
  void OnUpdateFile(const td_api::updateFile& update);

  // Completes everything still pending with an error (shutdown path).
  void FailAll(const std::string& reason);

  size_t pending_count() const;

 private:
  void MaybeComplete(std::int32_t file_id, const td_api::file& file);
  void FailOne(std::int32_t file_id, const std::string& error);

  ITdSender& sender_;
  mutable std::mutex mutex_;
  // One downloadFile request per file_id; every registered callback fires
  // on completion (concurrent downloads of the same file share the request).
  std::map<std::int32_t, std::vector<DownloadCallback>> pending_;
};

}  // namespace tg::telegram

#endif  // TG_TELEGRAM_FILEDOWNLOADER_H_
