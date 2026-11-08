#ifndef TG_TELEGRAM_AUTHMANAGER_H_
#define TG_TELEGRAM_AUTHMANAGER_H_

// AuthManager: drives TDLib's authorization state machine (architecture v5
// §3.5). TelegramClient routes updateAuthorizationState here from the Receive
// thread; AuthManager answers with the appropriate request via ITdSender.
//
// This TDLib version has no separate encryption-key stage: setTdlibParameters
// carries database_encryption_key directly (flat parameter form).
//
// Interactive inputs (code, password) come from providers. Defaults read
// TG_TELEGRAM_CODE / TG_TELEGRAM_PASSWORD from the environment; an empty
// answer fails the login loudly with a log message instead of hanging.

#include <td/telegram/td_api.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <chrono>

namespace tg::app {
struct TelegramConfig;
}  // namespace tg::app

namespace tg::telegram {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

class ITdSender;

class AuthManager {
 public:
  using InputProvider = std::function<std::string()>;

  AuthManager(ITdSender& sender, const app::TelegramConfig& config);

  // Receive thread. Answers the new authorization state.
  void HandleAuthorizationState(
      td_api::object_ptr<td_api::AuthorizationState>&& state);

  // Called with the (error) response to one of our own auth requests.
  void HandleResponse(std::uint64_t request_id,
                      const td_api::object_ptr<td_api::Object>& object);

  bool IsReady() const;
  bool HasFailed() const;
  // Human-readable current state for logs / health checks.
  std::string StateDescription() const;

  bool WaitForReady(std::chrono::milliseconds timeout) const;

  void SetCodeProvider(InputProvider provider) { code_provider_ = std::move(provider); }
  void SetPasswordProvider(InputProvider provider) {
    password_provider_ = std::move(provider);
  }

 private:
  void OnState(td_api::object_ptr<td_api::AuthorizationState>&& state);
  void Send(td_api::object_ptr<td_api::Function>&& request);
  void SetFailed(const std::string& reason);
  std::string RequestInput(const char* what, InputProvider& provider);

  ITdSender& sender_;
  const app::TelegramConfig& config_;

  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  std::string state_name_ = "unknown";
  bool ready_ = false;
  bool failed_ = false;

  InputProvider code_provider_;
  InputProvider password_provider_;
};

// Default providers: read from the environment, empty when unset.
std::string EnvInputProvider(const char* variable);

// Short human name for an AuthorizationState type id (local helper; the
// generated td_api.h has no to_string(int32)).
const char* AuthStateName(std::int32_t state_id);

}  // namespace tg::telegram

#endif  // TG_TELEGRAM_AUTHMANAGER_H_
