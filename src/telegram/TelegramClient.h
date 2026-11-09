#ifndef TG_TELEGRAM_TELEGRAMCLIENT_H_
#define TG_TELEGRAM_TELEGRAMCLIENT_H_

// TelegramClient (architecture v5 §3.2, §3.5): owns the single td::ClientManager
// and the TDLib Receive thread — the ONLY caller of ClientManager::receive().
//
// Thread rules:
//  - Send(): any thread (TDLib guarantees send is safe). Async only: the
//    response arrives via a callback on the Receive thread — there is NO
//    blocking wait API (SendWithFuture was removed 2026-11-09: a future
//    invites a worker thread to block on a TDLib response, which is
//    exactly the cross-message blocking this design forbids).
//  - ReceiveLoop(): its own thread; hands updates to the update sink (the
//    Dispatcher, which only posts to the worker pool), authorization updates
//    to the AuthManager, and updateFile to the FileDownloader hook. It never
//    downloads, reads files, or touches gRPC.
//
// ITdSender exists so handlers (AuthManager, FileDownloader, BotCommandHandler)
// and their unit tests can run against a fake sender; TelegramClient is the
// only production implementation.

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tg::app {
struct TelegramConfig;
struct AppContext;
}  // namespace tg::app

namespace tg::telegram {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

class AuthManager;
class UpdateDispatcher;

using ResponseCallback =
    std::function<void(td::ClientManager::Response&& response)>;

class ITdSender {
 public:
  virtual ~ITdSender() = default;
  // Returns the request id (> 0, never 0 — TDLib uses 0 for updates).
  // Async: the response arrives via `callback` on the Receive thread.
  virtual std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                            ResponseCallback callback) = 0;
};

class TelegramClient : public ITdSender {
 public:
  TelegramClient();
  ~TelegramClient() override;

  TelegramClient(const TelegramClient&) = delete;
  TelegramClient& operator=(const TelegramClient&) = delete;

  // Creates the TDLib client and starts the Receive thread. The authorization
  // state machine is driven by AuthManager from here on.
  void Start(const app::TelegramConfig& config);

  // v5 §3.6 step 4: sends close(), waits briefly for the closed state, then
  // stops the Receive thread; pending responses are completed with an error.
  void Stop();

  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    ResponseCallback callback) override;

  bool IsRunning() const { return running_.load(std::memory_order_acquire); }

  AuthManager& auth() { return *auth_; }

  // Wired by Application before Start():
  //  - update sink: dispatcher entry point (receives every non-auth,
  //    non-file update, moved).
  //  - file update hook: FileDownloader::OnUpdateFile (borrowed const ref).
  using UpdateSink =
      std::function<void(td_api::object_ptr<td_api::Update>&& update)>;
  void SetUpdateSink(UpdateSink sink) { update_sink_ = std::move(sink); }
  using FileUpdateHook = std::function<void(const td_api::updateFile& file)>;
  void SetFileUpdateHook(FileUpdateHook hook) {
    file_update_hook_ = std::move(hook);
  }

 private:
  void ReceiveLoop();
  void HandleUpdate(td_api::object_ptr<td_api::Update>&& update);
  void CompleteRequest(std::uint64_t request_id,
                       td_api::object_ptr<td_api::Object> object);
  void CompleteAllPendingWithError();

  td::ClientManager manager_;
  td::ClientManager::ClientId client_id_{0};

  // Set from config in Start(). When true, every incoming update is logged
  // (type + full content) before routing.
  bool log_updates_{false};

  std::atomic<std::uint64_t> next_request_id_{1};
  std::mutex callbacks_mutex_;
  std::unordered_map<std::uint64_t, ResponseCallback> callbacks_;

  std::thread receive_thread_;
  std::atomic<bool> running_{false};

  std::unique_ptr<AuthManager> auth_;
  UpdateSink update_sink_;
  FileUpdateHook file_update_hook_;
};

}  // namespace tg::telegram

#endif  // TG_TELEGRAM_TELEGRAMCLIENT_H_
