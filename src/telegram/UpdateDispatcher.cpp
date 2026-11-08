#include "telegram/UpdateDispatcher.h"

#include <app/WorkerPool.h>
#include <common/Logger.h>
#include <telegram/UpdateHandler.h>

namespace tg::telegram {

UpdateDispatcher::UpdateDispatcher(std::shared_ptr<app::WorkerPool> pool)
    : pool_(std::move(pool)) {}

void UpdateDispatcher::Register(std::shared_ptr<IUpdateHandler> handler) {
  if (handler == nullptr) return;
  std::lock_guard<std::mutex> lock(mutex_);
  handlers_.push_back(std::move(handler));
}

size_t UpdateDispatcher::handler_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return handlers_.size();
}

void UpdateDispatcher::Dispatch(td_api::object_ptr<td_api::Update>&& update,
                                app::AppContext& ctx) {
  std::shared_ptr<IUpdateHandler> claimant;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& handler : handlers_) {
      if (handler->CanHandle(update)) {
        claimant = handler;
        break;
      }
    }
  }
  if (claimant == nullptr) {
    spdlog::debug("dispatcher: no handler for update {}", update->get_id());
    return;
  }

  // Receive thread only moves the update into the task; it never executes
  // handler logic (v5 §3.3). The update is move-only, so it rides inside a
  // shared_ptr: the task must stay copyable for std::function (WorkerPool).
  const std::int32_t update_id = update->get_id();
  auto shared_update =
      std::make_shared<td_api::object_ptr<td_api::Update>>(std::move(update));
  const bool posted = pool_->Post([&ctx, claimant, shared_update]() mutable {
    claimant->Handle(*shared_update, ctx);
  });
  if (!posted) {
    spdlog::error("dispatcher: worker pool rejected update {} — dropped",
                  update_id);
  }
}

}  // namespace tg::telegram
