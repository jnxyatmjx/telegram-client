#ifndef TG_TELEGRAM_FILEDOWNLOADER_H_
#define TG_TELEGRAM_FILEDOWNLOADER_H_

// FileDownloader (architecture v5 §2.2 step [2]-[3]): workers call Download()
// and block on the returned future (caller-side timeout, default 30s per v5);
// completion arrives either from the downloadFile response (synchronous=true)
// or from an updateFile relayed by TelegramClient's Receive thread — the
// first of the two to complete wins.
//
// Receive thread rules: OnUpdateFile only fills promises (cheap); the map is
// guarded by one mutex (v5 §3.3).

#include <td/telegram/td_api.h>

#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace tg::telegram {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

class ITdSender;

class FileDownloader {
 public:
  explicit FileDownloader(ITdSender& sender);

  // Resolves with the local file path once downloaded; rejects (exception
  // std::runtime_error) on request error.
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

  ITdSender& sender_;
  mutable std::mutex mutex_;
  std::map<std::int32_t, std::promise<std::string>> pending_;
};

}  // namespace tg::telegram

#endif  // TG_TELEGRAM_FILEDOWNLOADER_H_
