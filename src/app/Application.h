#ifndef TG_APP_APPLICATION_H_
#define TG_APP_APPLICATION_H_

// Application (v5 §3.1, §3.5, §3.6): owns every component, wires them, runs
// the 9-thread structure (Main, TDLib Receive, 4 Workers, 2 CQ, Outbox
// Retry) and performs the ordered shutdown.

#include <app/AppContext.h>
#include <app/Config.h>
#include <app/WorkerPool.h>
#include <features/auto_search/AutoSearchHandler.h>
#include <features/bot_command/BotCommandHandler.h>
#include <features/media_thumbnail/MediaExtractor.h>
#include <features/media_thumbnail/MediaThumbnailHandler.h>
#include <features/media_thumbnail/SearchHandler.h>
#include <infra/Outbox.h>
#include <telegram/FileDownloader.h>
#include <telegram/TelegramClient.h>
#include <telegram/UpdateDispatcher.h>
#include <transport/GrpcTransport.h>
#include <transport/ProcessImagesClient.h>
#include <transport/SearchImagesClient.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace tg::app {

// Async-signal-safe shutdown hook: signal handlers call this (a relaxed
// atomic store); Application::Run() polls it together with its own flag.
void RequestGlobalShutdown();

class Application {
 public:
  // Throws std::runtime_error when the Outbox cannot be opened.
  explicit Application(Config config);
  ~Application();

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  // Signal-safe (may be called from a signal handler): a relaxed atomic
  // store only.
  void RequestShutdown() { stop_requested_.store(true, std::memory_order_relaxed); }

  bool ShutdownRequested() const {
    return stop_requested_.load(std::memory_order_relaxed);
  }

  // Blocks until SIGINT/SIGTERM (or auth failure); returns the process exit
  // code. Performs the v5 §3.6 ordered shutdown on the way out.
  int Run();

 private:
  void WireHandlers();
  void WireStreamerCallbacks();
  void StartOutboxRetryThread();
  void StopOutboxRetryThread();
  // One pass: re-enqueue due Outbox records (retry thread + reconnect hook).
  void SweepOutbox();
  int WaitForAuthOrFailure();

  Config config_;

  // Declaration order == initialization order == dependency order.
  infra::Outbox outbox_;
  std::shared_ptr<WorkerPool> workers_;
  telegram::TelegramClient telegram_;
  telegram::FileDownloader files_;
  transport::GrpcTransport transport_;
  transport::ProcessImagesClient process_client_;
  transport::SearchImagesClient search_client_;
  transport::ProcessImagesStreamer process_streamer_;
  transport::SearchImagesStreamer search_streamer_;
  features::media_thumbnail::MediaThumbnailHandler process_handler_;
  features::media_thumbnail::SearchHandler search_handler_;
  features::bot_command::BotCommandHandler bot_command_handler_;
  std::shared_ptr<features::auto_search::AutoSearchHandler> auto_search_handler_;
  telegram::UpdateDispatcher dispatcher_;
  std::unique_ptr<AppContext> ctx_;

  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> retry_running_{false};
  std::thread retry_thread_;
};

}  // namespace tg::app

#endif  // TG_APP_APPLICATION_H_
