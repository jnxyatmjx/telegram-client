#ifndef TG_TELEGRAM_UPDATEHANDLER_H_
#define TG_TELEGRAM_UPDATEHANDLER_H_

// Handler contract (architecture v5 §3.5). A handler claims only the update
// types it cares about; CanHandle must be cheap because the Dispatcher runs
// it on the TDLib Receive thread. Handle runs on a WorkerPool thread and may
// block (download, file IO, gRPC enqueue).

#include <td/telegram/td_api.h>

namespace tg::app {
struct AppContext;
}  // namespace tg::app

namespace tg::telegram {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

class IUpdateHandler {
 public:
  virtual ~IUpdateHandler() = default;

  IUpdateHandler(const IUpdateHandler&) = delete;
  IUpdateHandler& operator=(const IUpdateHandler&) = delete;

  virtual bool CanHandle(
      const td_api::object_ptr<td_api::Update>& update) const = 0;

  // `update` is owned by the dispatcher task; move out of it only if the
  // handler takes over the object's lifetime.
  virtual void Handle(td_api::object_ptr<td_api::Update>& update,
                      app::AppContext& ctx) = 0;

  virtual const char* Name() const = 0;

  // Dispatcher posts these handlers on the worker pool's search queue so
  // they run ahead of queued process work. Default is the process queue.
  virtual bool PrefersSearchQueue() const { return false; }

 protected:
  IUpdateHandler() = default;
};

}  // namespace tg::telegram

#endif  // TG_TELEGRAM_UPDATEHANDLER_H_
