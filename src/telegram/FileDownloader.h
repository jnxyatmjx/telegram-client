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
// FloodWait (chunk 3): a 429 on downloadFile does NOT fail the request.
// The callbacks stay registered and the SAME downloadFile is re-issued
// after retry-after + pad through the shared telegram::FloodRetry, at most
// max_attempts times; giving up fails the callbacks with an error naming
// the flood limit. While the retry is pending, new DownloadAsync calls for
// the same file ride along (they only append callbacks), so a rate-limited
// burst cannot spin: nothing is re-sent before retry-after passes.
//
// Cache cleanup (deleteFile, Hong requirement 2026-11-11): every
// DownloadAsync call counts as one consumer (use_count_, shared file_ids —
// a forwarded photo, or the same message processed twice — must not have
// one consumer delete the file out from under another). A consumer calls
// Release(file_id) ONCE, after it has stored the bytes (Outbox BLOB for
// ProcessImages / enqueued SearchReq for SearchImages) — never while it
// may still read the file. When the count reaches 0 the local cached file
// in telegram.files_directory is removed from TDLib's cache with
// td_api::deleteFile (fire-and-forget, best-effort: an error is logged,
// never fatal). A FAILED download deletes nothing (the file may not exist
// or be partial — TDLib cache is left as-is); FailAll (shutdown) deletes
// nothing either.
//
// Receive thread rules: OnUpdateFile only completes callbacks (cheap); the
// maps are guarded by one mutex (v5 §3.3). Callbacks are always invoked
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
class FloodRetry;

class FileDownloader {
 public:
  // Exactly one of the two fields is set.
  struct DownloadResult {
    std::string local_path;  // set on success
    std::string error;       // set on failure (local_path empty)
  };
  using DownloadCallback = std::function<void(DownloadResult result)>;

  // `flood_retry` is the shared 429 scheduler (Application passes one for
  // every instance); null keeps the pre-chunk-3 behaviour — a 429 fails the
  // download immediately. Production code MUST pass a FloodRetry.
  explicit FileDownloader(ITdSender& sender, FloodRetry* flood_retry = nullptr);

  // Async: registers on_done and returns immediately. No thread blocks.
  // Counts one consumer for the file: the caller must Release(file_id)
  // after it has consumed the bytes (see the cache-cleanup note above).
  void DownloadAsync(std::int32_t file_id, DownloadCallback on_done);

  // Drops one consumer's claim on the file (any thread; same mutex_ as the
  // download bookkeeping). At count 0 the cached local file is deleted from
  // TDLib's cache via td_api::deleteFile — fire-and-forget, best-effort:
  // the response is ignored (errors logged, never fatal). Unknown file_id /
  // already-zero count: no-op (defensive).
  void Release(std::int32_t file_id);

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
  // Sends one downloadFile for `file_id` (initial attempt or flood retry).
  void SendDownloadFile(std::int32_t file_id);
  // Response callback saw a 429: schedule the re-send (or give up).
  void HandleFloodWait(std::int32_t file_id, const td_api::error& error);

  ITdSender& sender_;
  FloodRetry* flood_retry_;  // not owned; may be null (see ctor comment)
  mutable std::mutex mutex_;
  // One downloadFile request per file_id; every registered callback fires
  // on completion (concurrent downloads of the same file share the request).
  std::map<std::int32_t, std::vector<DownloadCallback>> pending_;
  // FloodWait re-issue count per file_id; a file_id with entries here still
  // has its callbacks in `pending_` (they are only removed by
  // MaybeComplete/FailOne), which is what keeps concurrent callers from
  // double-sending while a flood retry is scheduled.
  std::map<std::int32_t, int> flood_attempts_;
  // Consumers that still need the local file (deleteFile refcount): one
  // increment per DownloadAsync call, one decrement per Release. At 0 the
  // cached file is deleted. Incremented on EVERY DownloadAsync call —
  // including during the download itself — so a consumer cannot release a
  // file another consumer has not received yet. Cleared by FailAll (no
  // deletes during shutdown); decremented by FailOne (a failed download
  // has no consumers left, and nothing is deleted for it).
  std::map<std::int32_t, int> use_count_;
};

}  // namespace tg::telegram

#endif  // TG_TELEGRAM_FILEDOWNLOADER_H_
