#include "telegram/TelegramClient.h"

#include <app/Config.h>
#include <common/Logger.h>
#include <telegram/AuthManager.h>

#include <chrono>

namespace tg::telegram {

TelegramClient::TelegramClient() = default;

TelegramClient::~TelegramClient() { Stop(); }

void TelegramClient::Start(const app::TelegramConfig& config) {
  if (running_.load(std::memory_order_acquire)) {
    spdlog::warn("telegram: Start() called while already running");
    return;
  }

  auth_ = std::make_unique<AuthManager>(*this, config);

  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true,
                                        std::memory_order_acq_rel)) {
    return;
  }
  client_id_ = manager_.create_client_id();
  // v5: request ids start at 1; 0 is reserved for updates.
  next_request_id_.store(1, std::memory_order_relaxed);
  receive_thread_ = std::thread([this] { ReceiveLoop(); });
  // Bootstrap: TDLib only creates the Td actor — which emits the initial
  // updateAuthorizationState — on the first send() for a client id.
  // create_client_id() alone only reserves the id, so a client that never
  // sends a request receives no update at all and authorization waits
  // forever (live bug, 2026-11-08: state stayed "unknown" for the full
  // 300 s auth timeout). The official example does the same
  // (td_example.cpp: getOption("version")). The response is ignored; the
  // request exists only to force actor creation.
  Send(td_api::make_object<td_api::getOption>("version"),
       [](td::ClientManager::Response&&) {});
  spdlog::info("telegram: client {} started", static_cast<int>(client_id_));
}

void TelegramClient::Stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }

  // v5 §3.6 step 4: politely close the TDLib session first. The close
  // response/state wakes the Receive loop so the join below is quick.
  if (client_id_ != 0) {
    manager_.send(client_id_, next_request_id_.fetch_add(1),
                  td_api::make_object<td_api::close>());
  }
  if (receive_thread_.joinable()) {
    receive_thread_.join();
  }

  CompleteAllPendingWithError();
  client_id_ = 0;
  spdlog::info("telegram: client stopped");
}

std::int64_t TelegramClient::Send(td_api::object_ptr<td_api::Function>&& request,
                                  ResponseCallback callback) {
  const std::uint64_t request_id =
      next_request_id_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    callbacks_.emplace(request_id, std::move(callback));
  }
  manager_.send(client_id_, request_id, std::move(request));
  return static_cast<std::int64_t>(request_id);
}

std::future<td::ClientManager::Response> TelegramClient::SendWithFuture(
    td_api::object_ptr<td_api::Function>&& request) {
  auto promise = std::make_shared<std::promise<td::ClientManager::Response>>();
  auto future = promise->get_future();
  Send(std::move(request), [promise](td::ClientManager::Response&& response) {
    promise->set_value(std::move(response));
  });
  return future;
}

void TelegramClient::ReceiveLoop() {
  // Single caller of manager_.receive() in the whole process (v5 §3.3).
  // 1s poll: shutdown responsiveness over idle CPU (v5 sketches 10s; that
  // would make Stop() wait up to 10s on an idle session — deviation noted
  // in README).
  while (running_.load(std::memory_order_acquire)) {
    auto response = manager_.receive(1.0);
    if (response.object == nullptr) {
      continue;
    }
    if (response.request_id == 0) {
      td_api::object_ptr<td_api::Update> update =
          td_api::move_object_as<td_api::Update>(std::move(response.object));
      HandleUpdate(std::move(update));
    } else {
      CompleteRequest(response.request_id, std::move(response.object));
    }
  }
}

void TelegramClient::HandleUpdate(td_api::object_ptr<td_api::Update>&& update) {
  switch (update->get_id()) {
    case td_api::updateAuthorizationState::ID: {
      auto auth_update =
          td_api::move_object_as<td_api::updateAuthorizationState>(std::move(update));
      auth_->HandleAuthorizationState(std::move(auth_update->authorization_state_));
      return;
    }
    case td_api::updateFile::ID: {
      if (file_update_hook_) {
        auto file_update =
            td_api::move_object_as<td_api::updateFile>(std::move(update));
        file_update_hook_(*file_update);
      }
      return;
    }
    default:
      if (update_sink_) {
        update_sink_(std::move(update));
      }
      return;
  }
}

void TelegramClient::CompleteRequest(std::uint64_t request_id,
                                     td_api::object_ptr<td_api::Object> object) {
  ResponseCallback callback;
  {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    const auto it = callbacks_.find(request_id);
    if (it == callbacks_.end()) {
      spdlog::warn("telegram: no waiter for request {}", request_id);
      return;
    }
    callback = std::move(it->second);
    callbacks_.erase(it);
  }
  td::ClientManager::Response response;
  response.client_id = client_id_;
  response.request_id = request_id;
  response.object = std::move(object);
  callback(std::move(response));
}

void TelegramClient::CompleteAllPendingWithError() {
  std::vector<ResponseCallback> pending;
  {
    std::lock_guard<std::mutex> lock(callbacks_mutex_);
    pending.reserve(callbacks_.size());
    for (auto& [id, cb] : callbacks_) {
      pending.push_back(std::move(cb));
    }
    callbacks_.clear();
  }
  for (auto& callback : pending) {
    td::ClientManager::Response response;
    response.request_id = 0;
    response.object = td_api::make_object<td_api::error>(
        500, "client is shutting down");
    callback(std::move(response));
  }
}

}  // namespace tg::telegram
