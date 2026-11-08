#ifndef TG_TELEGRAM_UPDATEDISPATCHER_H_
#define TG_TELEGRAM_UPDATEDISPATCHER_H_

// UpdateDispatcher (architecture v5 §3.2/§3.3): called on the TDLib Receive
// thread with a fresh update. Asks registered handlers CanHandle() in
// registration order and Posts the FIRST claiming handler's Handle() onto the
// worker pool. td_api objects are move-only, so exactly one handler owns an
// update — CanHandle must be a cheap read-only check.
//
// If no handler claims an update it is dropped (debug log): phase 1 ignores
// everything that is not a bot command (or an allowlisted auto-search media
// message when that feature is on).

#include <td/telegram/td_api.h>

#include <memory>
#include <mutex>
#include <vector>

namespace tg::app {
struct AppContext;
class WorkerPool;
}  // namespace tg::app

namespace tg::telegram {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

class IUpdateHandler;

class UpdateDispatcher {
 public:
  // `pool` must outlive the dispatcher.
  explicit UpdateDispatcher(std::shared_ptr<app::WorkerPool> pool);

  // Registration order defines priority (first claimant wins).
  void Register(std::shared_ptr<IUpdateHandler> handler);
  size_t handler_count() const;

  // Receive thread. `ctx` must outlive the posted task.
  void Dispatch(td_api::object_ptr<td_api::Update>&& update,
                app::AppContext& ctx);

 private:
  std::shared_ptr<app::WorkerPool> pool_;
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<IUpdateHandler>> handlers_;
};

}  // namespace tg::telegram

#endif  // TG_TELEGRAM_UPDATEDISPATCHER_H_
