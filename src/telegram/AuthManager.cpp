#include "telegram/AuthManager.h"

#include <app/Config.h>
#include <common/Logger.h>
#include <telegram/TelegramClient.h>

#include <cstdlib>
#include <iostream>
#include <istream>
#include <limits>
#include <utility>

namespace tg::telegram {
namespace {

bool IsError(const td_api::object_ptr<td_api::Object>& object) {
  return object != nullptr && object->get_id() == td_api::error::ID;
}

std::string ErrorText(const td_api::object_ptr<td_api::Object>& object) {
  if (!IsError(object)) return {};
  // Read-only: the object is borrowed, so downcast without moving it out.
  const auto* error = static_cast<const td_api::error*>(object.get());
  return "code " + std::to_string(error->code_) + ": " + error->message_;
}

}  // namespace

const char* AuthStateName(std::int32_t state_id) {
  switch (state_id) {
    case td_api::authorizationStateWaitTdlibParameters::ID:
      return "waitTdlibParameters";
    case td_api::authorizationStateWaitPhoneNumber::ID:
      return "waitPhoneNumber";
    case td_api::authorizationStateWaitCode::ID:
      return "waitCode";
    case td_api::authorizationStateWaitOtherDeviceConfirmation::ID:
      return "waitOtherDeviceConfirmation";
    case td_api::authorizationStateWaitRegistration::ID:
      return "waitRegistration";
    case td_api::authorizationStateWaitPassword::ID:
      return "waitPassword";
    case td_api::authorizationStateReady::ID:
      return "ready";
    case td_api::authorizationStateLoggingOut::ID:
      return "loggingOut";
    case td_api::authorizationStateClosing::ID:
      return "closing";
    case td_api::authorizationStateClosed::ID:
      return "closed";
    case td_api::authorizationStateWaitPremiumPurchase::ID:
      return "waitPremiumPurchase";
    case td_api::authorizationStateWaitEmailAddress::ID:
      return "waitEmailAddress";
    case td_api::authorizationStateWaitEmailCode::ID:
      return "waitEmailCode";
    default:
      return "unknown";
  }
}

AuthManager::AuthManager(ITdSender& sender, const app::TelegramConfig& config)
    : sender_(sender), config_(config) {
  // Environment first (scripted / non-interactive use), terminal prompt
  // second (interactive login, as in td_example.cpp). This makes the first
  // login a single run: start, read the SMS, type the code at the prompt —
  // no second run, so the code cannot go stale.
  code_provider_ = [] {
    std::string value = EnvInputProvider("TG_TELEGRAM_CODE");
    if (!value.empty()) return value;
    return PromptTerminal("Enter authentication code: ", std::cin, false);
  };
  password_provider_ = [] {
    std::string value = EnvInputProvider("TG_TELEGRAM_PASSWORD");
    if (!value.empty()) return value;
    return PromptTerminal("Enter authentication password: ", std::cin, true);
  };
}

std::string EnvInputProvider(const char* variable) {
  const char* value = std::getenv(variable);
  return value != nullptr ? std::string(value) : std::string{};
}

std::string PromptTerminal(const char* prompt, std::istream& in,
                           bool whole_line) {
  std::cout << prompt << std::flush;
  std::string value;
  if (whole_line) {
    std::getline(in, value);
  } else {
    in >> value;
    // operator>> leaves the trailing newline behind; a later getline (the
    // 2FA password prompt) would otherwise read that newline as an empty
    // answer. Consume the rest of the line.
    in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  }
  return value;
}

void AuthManager::HandleAuthorizationState(
    td_api::object_ptr<td_api::AuthorizationState>&& state) {
  OnState(std::move(state));
}

void AuthManager::HandleResponse(
    std::uint64_t /*request_id*/, const td_api::object_ptr<td_api::Object>& object) {
  if (IsError(object)) {
    const std::string text = ErrorText(object);
    spdlog::error("auth: request failed ({})", text);
    // A rejected setTdlibParameters (bad api_id/api_hash) or wrong code would
    // leave TDLib waiting for the same answer; fail loudly so the operator
    // sees it instead of a silent login loop.
    if (text.find("PHONE_NUMBER_UNSET") != std::string::npos ||
        text.find("API_ID") != std::string::npos ||
        text.find("API_HASH") != std::string::npos ||
        text.find("PHONE_NUMBER_INVALID") != std::string::npos ||
        text.find("PHONE_CODE_INVALID") != std::string::npos ||
        text.find("PASSWORD") != std::string::npos) {
      SetFailed(text);
    }
  }
}

bool AuthManager::IsReady() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ready_;
}

bool AuthManager::HasFailed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return failed_;
}

std::string AuthManager::StateDescription() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ready_) return "ready";
  if (failed_) return "failed";
  return state_name_;
}

bool AuthManager::WaitForReady(std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(mutex_);
  return cv_.wait_for(lock, timeout, [&] { return ready_ || failed_; });
}

void AuthManager::Send(td_api::object_ptr<td_api::Function>&& request) {
  sender_.Send(
      std::move(request), [this](td::ClientManager::Response&& response) {
        HandleResponse(response.request_id, response.object);
      });
}

void AuthManager::SetFailed(const std::string& reason) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    failed_ = true;
  }
  spdlog::error("auth: FAILED — {}", reason);
  cv_.notify_all();
}

std::string AuthManager::RequestInput(const char* what,
                                      InputProvider& provider) {
  // Note: a terminal prompt blocks the Receive thread until the user types
  // (or EOF). TDLib queues further updates meanwhile, and the 300 s auth
  // timeout in Application still bounds the wait.
  std::string value = provider ? provider() : std::string{};
  if (value.empty()) {
    SetFailed(std::string("Telegram asked for ") + what +
              " but no input was provided (type it at the terminal prompt, "
              "or set TG_TELEGRAM_CODE / TG_TELEGRAM_PASSWORD for "
              "non-interactive use)");
    return {};
  }
  return value;
}

void AuthManager::OnState(td_api::object_ptr<td_api::AuthorizationState>&& state) {
  const std::int32_t state_id = state->get_id();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_name_ = AuthStateName(state_id);
  }
  spdlog::info("auth: authorization state -> {}", state_name_);

  switch (state_id) {
    case td_api::authorizationStateWaitTdlibParameters::ID: {
      // Flat parameter form of this TDLib version (no separate
      // checkDatabaseEncryptionKey stage).
      auto params = td_api::make_object<td_api::setTdlibParameters>();
      params->use_test_dc_ = config_.use_test_dc;
      params->database_directory_ = config_.database_directory;
      params->files_directory_ = config_.files_directory;
      params->database_encryption_key_ = config_.encryption_key;
      params->use_file_database_ = true;
      params->use_chat_info_database_ = true;
      params->use_message_database_ = true;
      params->use_secret_chats_ = false;
      params->api_id_ = config_.api_id;
      params->api_hash_ = config_.api_hash;
      params->system_language_code_ = "en";
      params->device_model_ = "tg-client";
      params->system_version_ = "tdlib-grpc";
      params->application_version_ = "0.1.0";
      Send(std::move(params));
      break;
    }
    case td_api::authorizationStateWaitPhoneNumber::ID: {
      auto request = td_api::make_object<td_api::setAuthenticationPhoneNumber>();
      request->phone_number_ = config_.phone;
      // Null settings -> TDLib defaults (this schema has no
      // firebaseAuthenticationSettingsUndefined variant).
      request->settings_ = nullptr;
      Send(std::move(request));
      break;
    }
    case td_api::authorizationStateWaitCode::ID: {
      std::string code = RequestInput("a login code", code_provider_);
      if (code.empty()) return;
      auto request = td_api::make_object<td_api::checkAuthenticationCode>();
      request->code_ = std::move(code);
      Send(std::move(request));
      break;
    }
    case td_api::authorizationStateWaitPassword::ID: {
      std::string password = RequestInput("the 2FA password", password_provider_);
      if (password.empty()) return;
      auto request = td_api::make_object<td_api::checkAuthenticationPassword>();
      request->password_ = std::move(password);
      Send(std::move(request));
      break;
    }
    case td_api::authorizationStateReady::ID: {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ready_ = true;
      }
      spdlog::info("auth: READY");
      cv_.notify_all();
      break;
    }
    case td_api::authorizationStateLoggingOut::ID:
    case td_api::authorizationStateClosing::ID:
      spdlog::info("auth: {}", state_name_);
      break;
    case td_api::authorizationStateClosed::ID:
      spdlog::info("auth: closed");
      cv_.notify_all();
      break;
    case td_api::authorizationStateWaitOtherDeviceConfirmation::ID: {
      const auto wait =
          td_api::move_object_as<td_api::authorizationStateWaitOtherDeviceConfirmation>(
              state);
      spdlog::warn("auth: confirm on other device: {}", wait->link_);
      break;
    }
    case td_api::authorizationStateWaitRegistration::ID:
      SetFailed("account registration is not supported by this client");
      break;
    case td_api::authorizationStateWaitEmailAddress::ID:
    case td_api::authorizationStateWaitEmailCode::ID:
      SetFailed("email-based login is not supported by this client");
      break;
    case td_api::authorizationStateWaitPremiumPurchase::ID:
      SetFailed("premium purchase state is not supported by this client");
      break;
    default:
      spdlog::warn("auth: unhandled authorization state {}", state_id);
      break;
  }
}

}  // namespace tg::telegram
