#ifndef TG_APP_APPLICATION_H_
#define TG_APP_APPLICATION_H_

// Application (v5 §3.1, §3.5, §3.6): owns every component, wires them, runs
// the thread structure (Main, TDLib Receive, 4 Workers, 2 x grpc.connections
// CQ, Outbox Retry) and performs the ordered shutdown.

#include <app/AppContext.h>
#include <app/Config.h>
#include <app/WorkerPool.h>
#include <features/auto_search/AutoSearchHandler.h>
#include <features/auto_search/ChatLinkResolver.h>
#include <features/bot_command/BotCommandHandler.h>
#include <features/chat_state/ChatStateStore.h>
#include <features/chat_state/SqliteChatStateBackend.h>
#include <features/commands/ControlCommandsHandler.h>
#include <features/commands/DialogScanner.h>
#include <features/history_backfill/HistoryBackfill.h>
#include <features/media_channels/MediaChannelHandler.h>
#include <features/media_thumbnail/MediaExtractor.h>
#include <features/media_thumbnail/MediaThumbnailHandler.h>
#include <features/media_thumbnail/SearchHandler.h>
#include <infra/Outbox.h>
#include <telegram/FileDownloader.h>
#include <telegram/FloodRetry.h>
#include <telegram/MessageSender.h>
#include <telegram/TelegramClient.h>
#include <telegram/UpdateDispatcher.h>
#include <transport/GrpcTransport.h>
#include <transport/ProcessImagesClient.h>
#include <transport/SearchImagesClient.h>
#include <transport/StreamerPool.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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
  // Test seam: drives the production ImageReply handler and reads the
  // flow-control counter. Production code does not use it.
  friend class ProcessImagesFlowControlProbe;

  void WireHandlers();
  void WireStreamerCallbacks();
  // ProcessImages ImageReply. MarkDone(PENDING/FAILED/SENT → DONE) releases
  // the flow-control slot taken before that request was enqueued. A false
  // MarkDone (already DONE, DEAD_LETTER, or an unknown id) releases nothing.
  void OnProcessImageReply(const ImageReply& reply);
  // SearchRsp. Posted off the SearchImages completion-queue thread so link
  // resolution and the Telegram reply cannot stall that stream's read/write.
  void OnSearchReply(const SearchRsp& reply);
  void StartOutboxRetryThread();
  void StopOutboxRetryThread();
  // Chat-state flush loop (Chunk 2): FlushActive every
  // chat_state.flush_interval_seconds; joined + one final full flush on
  // stop. No-op unless chat_state.enabled.
  void StartStateFlushThread();
  void StopStateFlushThread();
  // One pass: re-enqueue due Outbox records (retry thread + reconnect hook).
  void SweepOutbox();
  int WaitForAuthOrFailure();

  Config config_;

  // Declaration order == initialization order == dependency order.
  infra::Outbox outbox_;
  std::shared_ptr<WorkerPool> workers_;
  // Shared TDLib FloodWait scheduler (chunk 3): one timer thread for the
  // whole process. FileDownloader, MessageSender and BotCommandHandler
  // re-issue their 429-refused requests through it (bounded attempts);
  // HistoryBackfill / DialogScanner keep their own feature-level pauses.
  // Declared before everything that uses it, so it is destroyed last;
  // Shutdown() is called first in the destructor body — before
  // telegram_.Stop() — so no pending closure fires against a closed
  // TDLib session.
  telegram::FloodRetry flood_retry_;
  telegram::TelegramClient telegram_;
  telegram::FileDownloader files_;
  transport::GrpcTransport transport_;
  // One client + one stream per lane (grpc.connections lanes); the pools
  // deal requests round-robin across lanes.
  std::vector<std::unique_ptr<transport::ProcessImagesClient>> process_clients_;
  std::vector<std::unique_ptr<transport::SearchImagesClient>> search_clients_;
  transport::StreamerPool<ImageRequest, ImageReply> process_pool_;
  transport::StreamerPool<SearchReq, SearchRsp> search_pool_;
  // Flow-control window for the process path (retry design §2(c)): caps
  // sent-but-unreplied ProcessImages requests at outbox.max_in_flight.
  // Declared before ctx_, which hands a pointer to the handlers; released
  // on reply-DONE / write-lost / safety-net requeue / permanent-failure
  // dead-letter of a SENT record.
  FlowControl flow_control_;
  // Per-chat watermark/link state driven by ImageReply.result. With
  // chat_state.enabled it persists to the local SQLite `chathis` table
  // (30 s flush of active chats) and restores on startup; otherwise it is
  // memory-only (Chunk 1 behaviour). Declared after telegram_: its TDLib
  // resolution callbacks complete during telegram_.Stop() in the destructor
  // body, before members are destroyed.
  features::chat_state::ChatStateStore chat_state_;
  features::media_thumbnail::MediaThumbnailHandler process_handler_;
  features::media_thumbnail::SearchHandler search_handler_;
  // Chunk 2: unified sendMessage (markdown, 4096-char split), history
  // backfill engine, dialog enumeration and the control-chat command
  // surface (/rsme //rsor //xm ...). Declaration order: messenger_ feeds
  // backfill_ and control_commands_handler_; backfill_ and dialog_scanner_
  // are referenced by control_commands_handler_.
  telegram::MessageSender messenger_;
  features::history_backfill::HistoryBackfill backfill_;
  features::commands::DialogScanner dialog_scanner_;
  features::commands::ControlCommandsHandler control_commands_handler_;
  std::shared_ptr<features::media_channels::MediaChannelHandler>
      media_channel_handler_;
  features::bot_command::BotCommandHandler bot_command_handler_;
  std::shared_ptr<features::auto_search::AutoSearchHandler> auto_search_handler_;
  // Search-hit link bases. Reads chathis; never writes it. TDLib username
  // lookups run on the worker pool with a short timeout.
  features::auto_search::ChatLinkResolver chat_link_resolver_;
  telegram::UpdateDispatcher dispatcher_;
  std::unique_ptr<AppContext> ctx_;

  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> retry_running_{false};
  std::thread retry_thread_;
  std::atomic<bool> flush_running_{false};
  std::thread flush_thread_;
};

}  // namespace tg::app

#endif  // TG_APP_APPLICATION_H_
