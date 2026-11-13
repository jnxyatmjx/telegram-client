#include "app/Application.h"

#include <common/Logger.h>
#include <telegram/AuthManager.h>

#include <csignal>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace tg::app {
namespace td_api = td::td_api;  // shorthand for td::td_api::*
namespace {

using features::media_thumbnail::MediaExtractor;
using features::media_thumbnail::MediaThumbnailHandler;
using features::media_thumbnail::SearchHandler;

constexpr auto kWorkerDrainTimeout = std::chrono::seconds(10);  // v5 §3.6
constexpr auto kAuthReadyTimeout = std::chrono::seconds(300);
constexpr auto kSignalPollInterval = std::chrono::milliseconds(200);
// Auth wait polls in slices of this length so a SIGINT/SIGTERM during the
// (up to 300 s) auth wait is noticed promptly.
constexpr auto kAuthPollInterval = std::chrono::milliseconds(200);
// Distinct Run() result: shutdown was requested while waiting for auth.
// (0 = authorized, 1 = auth failed/timed out.)
constexpr int kAuthInterrupted = 2;
constexpr std::size_t kSweepBatch = 100;

// Permanent failures: retrying cannot help (v5 §2.5).
bool IsPermanentGrpcError(const grpc::StatusCode code) {
  switch (code) {
    case grpc::StatusCode::INVALID_ARGUMENT:
    case grpc::StatusCode::PERMISSION_DENIED:
    case grpc::StatusCode::UNIMPLEMENTED:
    case grpc::StatusCode::NOT_FOUND:
      return true;
    default:
      return false;
  }
}

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

namespace {
std::atomic<bool> g_global_stop{false};
}  // namespace

void RequestGlobalShutdown() {
  g_global_stop.store(true, std::memory_order_relaxed);
}

Application::Application(Config config)
    : config_(std::move(config)),
      // reopen_grace = send_grace: SENT records recovered at startup replay
      // on the first sweep after the same short grace a fresh send gets.
      outbox_(config_.outbox.db_path, config_.outbox.send_grace_ms),
      workers_(std::make_shared<WorkerPool>(
          static_cast<std::size_t>(config_.worker_count),
          10'000,  // handler queue capacity
          std::chrono::milliseconds(1'000))),
      // One shared FloodWait scheduler; the pad is the Telethon "+1" (the
      // same knob HistoryBackfill and DialogScanner pause with). fallback /
      // attempt cap stay at the FloodRetry defaults (5 s / 3).
      flood_retry_(telegram::FloodRetry::Options{
          config_.history_backfill.flood_pad_seconds,
          /*fallback_seconds=*/5,
          /*max_attempts=*/3}),
      files_(telegram_, &flood_retry_),
      transport_(config_.grpc),
      // Flow-control window for the process path (retry design §2(c)).
      flow_control_(config_.outbox.max_in_flight),
      // chat_state.enabled=false keeps the store memory-only (Chunk 1
      // behaviour); otherwise the Python `chathis` SQLite schema backs it.
      chat_state_(telegram_,
                  config_.chat_state.enabled
                      ? features::chat_state::MakeSqliteChatStateBackend(
                            config_.chat_state.db_path)
                      : nullptr),
      process_handler_(MediaExtractor(config_.media_thumbnail), telegram_,
                       files_,
                       common::MemoCipher(config_.media_thumbnail.keyme)),
      search_handler_(MediaExtractor(config_.media_thumbnail), telegram_,
                      files_),
      messenger_(telegram_, &flood_retry_),
      backfill_(telegram_, process_handler_, chat_state_, messenger_,
                features::history_backfill::HistoryBackfillConfig{
                    config_.history_backfill.enabled,
                    config_.history_backfill.page_limit,
                    config_.history_backfill.flood_pad_seconds}),
      dialog_scanner_(telegram_,
                      features::commands::DialogScanner::Config{
                          config_.commands.dialog_limit,
                          config_.history_backfill.flood_pad_seconds}),
      control_commands_handler_(config_.commands, config_.bot_command,
                                messenger_, dialog_scanner_, backfill_),
      media_channel_handler_(
          std::make_shared<features::media_channels::MediaChannelHandler>(
              config_.media_channels, process_handler_, search_handler_)),
      bot_command_handler_(config_.bot_command, process_handler_,
                           search_handler_, &flood_retry_),
      auto_search_handler_(std::make_shared<features::auto_search::AutoSearchHandler>(
          config_.auto_search, search_handler_)),
      dispatcher_(workers_) {
  // Lanes: one channel + one ProcessImages stream + one SearchImages stream
  // each (grpc.connections, default 1). The pools deal requests round-robin
  // across lanes; reply matching is by id, so lane order does not matter.
  const std::size_t lanes = transport_.lane_count();
  for (std::size_t lane = 0; lane < lanes; ++lane) {
    process_clients_.push_back(
        std::make_unique<transport::ProcessImagesClient>(transport_, lane));
    search_clients_.push_back(
        std::make_unique<transport::SearchImagesClient>(transport_, lane));
    transport::AsyncStreamer<ImageRequest, ImageReply>::Options process_options;
    process_options.name = "ProcessImages#" + std::to_string(lane);
    process_options.start_fn = process_clients_.back()->start_fn();
    process_options.make_context = [this] { return transport_.MakeContext(); };
    process_options.initial_backoff_ms = config_.grpc.reconnect_backoff_ms;
    process_options.max_backoff_ms = config_.grpc.max_reconnect_backoff_ms;
    process_options.queue_capacity =
        static_cast<std::size_t>(config_.grpc.queue_capacity);
    process_pool_.AddLane(std::move(process_options));
    transport::AsyncStreamer<SearchReq, SearchRsp>::Options search_options;
    search_options.name = "SearchImages#" + std::to_string(lane);
    search_options.start_fn = search_clients_.back()->start_fn();
    search_options.make_context = [this] { return transport_.MakeContext(); };
    search_options.initial_backoff_ms = config_.grpc.reconnect_backoff_ms;
    search_options.max_backoff_ms = config_.grpc.max_reconnect_backoff_ms;
    search_options.queue_capacity =
        static_cast<std::size_t>(config_.grpc.queue_capacity);
    search_pool_.AddLane(std::move(search_options));
  }

  ctx_ = std::make_unique<AppContext>(AppContext{
      *workers_, telegram_, files_, process_pool_, search_pool_, outbox_,
      config_, &flow_control_});
  WireHandlers();
  WireStreamerCallbacks();
}

Application::~Application() {
  // Async shutdown order (2026-11-09): stop the source of new work first.
  // flood_retry_.Shutdown() drops every pending FloodWait re-send (chunk 3)
  // so none fires against a closed TDLib session; backfill_.Shutdown() /
  // dialog_scanner_.Shutdown() refuse new TDLib
  // sends, so pending callbacks during telegram_.Stop() no-op cleanly.
  // telegram_.Stop() completes every pending TDLib callback (getMessage,
  // downloadFile), which fires the download callbacks, which post their
  // finish stages to the pool — so the pool must still be running here,
  // and is drained after. FailAll is the safety net for any download
  // still pending after the TDLib callbacks completed.
  flood_retry_.Shutdown();
  backfill_.Shutdown();
  dialog_scanner_.Shutdown();
  telegram_.Stop();
  files_.FailAll("shutting down");
  workers_->Stop(std::chrono::seconds(0));
  process_pool_.Stop();
  search_pool_.Stop();
  StopOutboxRetryThread();
  StopStateFlushThread();
}

void Application::WireHandlers() {
  dispatcher_.Register(std::shared_ptr<telegram::IUpdateHandler>(
      &bot_command_handler_, [](telegram::IUpdateHandler*) {}));  // non-owning
  // Phase-2 trigger; disabled by default -> claims nothing.
  dispatcher_.Register(auto_search_handler_);
  // Chunk 2: whitelist-channel auto pipeline (Python handmedias); disabled
  // by default. With auto_search AND media_channels enabled for the same
  // chat, auto_search (registered first) claims the update — first
  // claimant wins (UpdateDispatcher semantics).
  dispatcher_.Register(media_channel_handler_);
  // Chunk 2: control-chat commands (/rsme //rsor //xm ...).
  dispatcher_.Register(std::shared_ptr<telegram::IUpdateHandler>(
      &control_commands_handler_, [](telegram::IUpdateHandler*) {}));  // non-owning
}

void Application::OnProcessImageReply(const ImageReply& reply) {
  // MarkDone accepts PENDING, FAILED and SENT. True means this reply
  // completed the in-flight request, so the slot FinishProcess or
  // SweepOutbox acquired comes back. False (already DONE, DEAD_LETTER, or
  // an unknown id) must not Release — a second reply would under-count.
  // Skipping Release on a true MarkDone leaves the slot held after the
  // record is DONE, and the sent-timeout safety net never sees DONE rows,
  // so the window stays full until the process restarts.
  const bool completed = outbox_.MarkDone(reply.image_id(), reply.result());
  if (completed) {
    flow_control_.Release();
    spdlog::info("process: DONE {} result: {:.200}", reply.image_id(),
                 reply.result());
  } else {
    const auto existing = outbox_.Get(reply.image_id());
    if (existing.has_value() &&
        existing->state == infra::OutboxState::kDone) {
      spdlog::info("process: duplicate reply for {} (already done) — "
                   "ignored",
                   reply.image_id());
    } else {
      spdlog::warn("process: reply for unknown image_id {}",
                   reply.image_id());
    }
  }
  const auto parsed =
      features::chat_state::ParseReplyResult(reply.result());
  if (!parsed.has_value()) {
    // The server contract is {"cid": <bare id>, "mid": <msg id>}
    // (Python insertIdUpdate logs a decode failure the same way).
    spdlog::warn("process: reply result for {} is not a {{\"cid\","
                 "\"mid\"}} JSON — watermark not updated",
                 reply.image_id());
    return;
  }
  chat_state_.ApplyReply(parsed->cid, parsed->mid);
}

void Application::WireStreamerCallbacks() {
  // ImageReply -> Outbox DONE (v5 §2.4: log + metrics only, never reply into
  // Telegram by default) and release the flow-control slot when that DONE
  // completes an in-flight request. Also the {"cid","mid"} watermark update
  // (Telethon migration Chunk 1: parse result; a malformed result is logged
  // and dropped, never crashes the pipeline). The slot release happens
  // inside OnProcessImageReply before the watermark parse, so a non-JSON
  // result still frees the window.
  process_pool_.SetReplyHandler(
      [this](const ImageReply& reply) { OnProcessImageReply(reply); });

  // SearchRsp -> correlation table lookup (v5 §2.4), log only.
  search_pool_.SetReplyHandler(
      [this](const SearchRsp& reply) {
        const auto entry = search_handler_.correlation().Lookup(
            reply.oid(), reply.id());
        if (entry.has_value()) {
          spdlog::info("search: rsp for {} (oid={}, id={}) result: {:.200}",
                       entry->image_id, reply.oid(), reply.id(), reply.result());
        } else {
          spdlog::warn("search: rsp for unknown (oid={}, id={})", reply.oid(),
                       reply.id());
        }
      });

  // Write-ok: the request left the client (gRPC accepted it into the
  // transport). PENDING/FAILED -> SENT until the safety deadline, out of
  // the retry set — this is what removes the duplicate-send class (retry
  // design §3.3: "whatever the server's latency"). A false MarkSent means
  // this was a duplicate copy of a record already SENT/DONE/dead: its
  // window slot returns immediately (the record's own slot resolves
  // through its own path).
  process_pool_.SetOnWritten([this](const ImageRequest& request) {
    if (outbox_.MarkSent(request.image_id(),
                         NowMs() + config_.outbox.sent_timeout_ms)) {
      return;
    }
    flow_control_.Release();
  });

  // Write-lost (write failed, or dropped from a breaking stream's queue):
  // the request provably never reached the server. The copy's window slot
  // returns unconditionally; the record re-arms FAILED due-now when it was
  // still PENDING (MarkFailed's PENDING guard is a no-op for SENT /
  // DONE / DEAD_LETTER — a lost DUPLICATE copy must not re-arm a record
  // whose real request is already on the wire). The reconnect / periodic
  // sweep replays it within seconds, not the old full ack deadline (fixes
  // G2/G3).
  process_pool_.SetOnWriteLost([this](const ImageRequest& request) {
    flow_control_.Release();
    outbox_.MarkFailed(request.image_id(), "write lost", 0);
  });

  // Permanent gRPC failure -> dead-letter all pending (v5 §2.5), and the
  // SENT records too (retry design §4 Q5): a server that rejects the
  // stream outright will reject the replay, and a dead-lettered SENT
  // record's window slot only a reply or the safety net would release.
  // PENDING/FAILED records release nothing here — their queued copies
  // resolve through write-lost (FailStream's ClearQueue runs after this).
  process_pool_.SetOnStreamFailed(
      [this](const grpc::Status& status) {
        if (!IsPermanentGrpcError(status.error_code())) return;
        const auto pending = outbox_.DueForRetry(NowMs() + 24 * 3600 * 1000LL,
                                                 10'000);
        for (const auto& record : pending) {
          outbox_.MarkDeadLetter(record.image_id, status.error_message());
          spdlog::error("process: dead-lettered {} ({})", record.image_id,
                        status.error_message());
        }
        const auto sent =
            outbox_.SentPastDeadline(NowMs() + 24 * 3600 * 1000LL, 10'000);
        for (const auto& record : sent) {
          outbox_.MarkDeadLetter(record.image_id, status.error_message());
          flow_control_.Release();
          spdlog::error("process: dead-lettered sent {} ({})",
                        record.image_id, status.error_message());
        }
      });

  // Stream (re)connected -> sweep the Outbox: unacked records replay by
  // image_id, server dedupes (v5 §2.5).
  process_pool_.SetOnReconnected([this] { SweepOutbox(); });
  search_pool_.SetOnReconnected([this] { SweepOutbox(); });
}

void Application::SweepOutbox() {
  // Flow-control window (retry design §2(c)): never deal more than
  // outbox.max_in_flight sent-but-unreplied process requests to the server.
  // A window-blocked record stays PENDING/FAILED due — picked up by a later
  // sweep once replies / the safety net free slots. Window-blocked records
  // are never MarkRequeued, so window pressure cannot burn attempts.
  bool window_full_logged = false;
  const auto due = outbox_.DueForRetry(NowMs(), kSweepBatch);
  for (const auto& record : due) {
    if (!flow_control_.TryAcquire()) {
      if (!window_full_logged) {
        window_full_logged = true;
        spdlog::warn(
            "outbox: flow-control window full ({} in flight, cap {}) — "
            "deferring sends", flow_control_.in_flight(),
            flow_control_.window());
      }
      continue;
    }
    ImageRequest request;
    request.set_image_id(record.image_id);
    request.set_image_data(record.image_data);
    request.set_memo(record.memo);
    request.set_owner_id(record.owner_id);
    const bool enqueued = process_pool_.Enqueue(
        std::move(request),
        std::chrono::milliseconds(config_.grpc.enqueue_timeout_ms));
    if (enqueued) {
      outbox_.MarkRequeued(record.image_id, config_.outbox.retry_backoff_ms,
                           config_.outbox.max_attempts, "requeued for send");
    } else {
      flow_control_.Release();
      spdlog::warn("outbox: retry sweep could not enqueue {} — will retry",
                   record.image_id);
    }
  }

  // Safety net (retry design §3.1/§3.2): records whose request WAS written
  // but that got no reply within outbox.sent_timeout_ms. Fate unknown (the
  // stream may have died after the write) — one at-least-once re-send,
  // server dedup is the backstop. Requeued (PENDING, attempts++, dead-letter
  // at max_attempts) and the held window slot released; the next sweep
  // re-acquires when it re-sends. This is the only place a SENT record
  // re-enters the retry set on a timer, and where `attempts` now means
  // only "un-acked re-sends" (fixes G1: a slow-but-alive server can no
  // longer dead-letter records).
  const auto stale = outbox_.SentPastDeadline(NowMs(), kSweepBatch);
  for (const auto& record : stale) {
    if (outbox_.MarkRequeued(record.image_id, config_.outbox.retry_backoff_ms,
                             config_.outbox.max_attempts, "ack timeout")) {
      flow_control_.Release();
      spdlog::warn("outbox: {} sent-but-unreplied past ack timeout "
                   "({} ms) — requeued (attempts {})",
                   record.image_id, config_.outbox.sent_timeout_ms,
                   record.attempts + 1);
    }
  }
}

void Application::StartOutboxRetryThread() {
  retry_running_.store(true, std::memory_order_release);
  retry_thread_ = std::thread([this] {
    const auto interval =
        std::chrono::milliseconds(config_.outbox.retry_interval_ms);
    while (retry_running_.load(std::memory_order_acquire)) {
      SweepOutbox();
      outbox_.PurgeDoneBefore(NowMs() -
                              config_.outbox.purge_done_after_seconds * 1000LL);
      const auto deadline = std::chrono::steady_clock::now() + interval;
      while (retry_running_.load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  });
}

void Application::StopOutboxRetryThread() {
  if (!retry_running_.exchange(false, std::memory_order_acq_rel)) return;
  if (retry_thread_.joinable()) {
    retry_thread_.join();
  }
}

void Application::StartStateFlushThread() {
  if (!config_.chat_state.enabled) return;
  flush_running_.store(true, std::memory_order_release);
  flush_thread_ = std::thread([this] {
    // Python schedule.every(30).schedule2db: flush entries whose watermark
    // advanced within active_window_seconds.
    const auto interval =
        std::chrono::seconds(config_.chat_state.flush_interval_seconds);
    while (flush_running_.load(std::memory_order_acquire)) {
      chat_state_.FlushActive(
          NowMs(), config_.chat_state.active_window_seconds);
      const auto deadline = std::chrono::steady_clock::now() + interval;
      while (flush_running_.load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  });
}

void Application::StopStateFlushThread() {
  if (!flush_running_.exchange(false, std::memory_order_acq_rel)) return;
  if (flush_thread_.joinable()) {
    flush_thread_.join();
  }
  if (config_.chat_state.enabled) {
    // Final pass with an unbounded window: persist EVERY entry, not just
    // the 45 s-active ones — a shutdown must not lose a watermark that
    // advanced since its last periodic flush (Python lost exactly that on
    // exit; we can afford the extra write).
    chat_state_.FlushActive(NowMs(), std::numeric_limits<int>::max());
  }
}

int Application::WaitForAuthOrFailure() {
  // Poll in short slices: a plain 300 s WaitForReady() would swallow
  // SIGINT/SIGTERM for the whole auth wait, because the signal handler
  // only sets a flag that nothing checks until the wait returns.
  const auto deadline = std::chrono::steady_clock::now() + kAuthReadyTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ShutdownRequested() || g_global_stop.load(std::memory_order_relaxed)) {
      spdlog::info("telegram: shutdown requested while waiting for authorization");
      return kAuthInterrupted;
    }
    if (telegram_.auth().WaitForReady(kAuthPollInterval)) break;
  }
  if (telegram_.auth().IsReady()) return 0;
  if (ShutdownRequested() || g_global_stop.load(std::memory_order_relaxed)) {
    return kAuthInterrupted;
  }
  spdlog::error(
      "telegram: authorization did not complete (state: {}). A live login "
      "needs: api_id/api_hash, phone, TG_TELEGRAM_CODE (and "
      "TG_TELEGRAM_PASSWORD for 2FA). Not retrying.",
      telegram_.auth().StateDescription());
  return 1;
}

int Application::Run() {
  spdlog::info(
      "application: starting — workers={}, outbox={}, chat_state={}, grpc "
      "target={}, lanes={}",
      config_.worker_count, config_.outbox.db_path,
      config_.chat_state.enabled
          ? config_.chat_state.db_path
          : "<memory-only>",
      config_.grpc.target, transport_.lane_count());

  telegram_.SetUpdateSink(
      [this](td_api::object_ptr<td_api::Update>&& update) {
        dispatcher_.Dispatch(std::move(update), *ctx_);
      });
  telegram_.SetFileUpdateHook(
      [this](const td_api::updateFile& file) { files_.OnUpdateFile(file); });

  process_pool_.Start();
  search_pool_.Start();
  telegram_.Start(config_.telegram);

  const int auth_result = WaitForAuthOrFailure();
  if (auth_result == kAuthInterrupted) {
    // Shutdown requested during auth: skip straight to the ordered
    // shutdown below. The retry and flush threads were never started
    // (their Stops are no-ops in that case); streamers and telegram were
    // started above, so they still need their ordered Stop calls.
  } else if (auth_result != 0) {
    return auth_result;
  } else {
    // Startup restore (Python load_chat_state): the persisted `chathis`
    // table back into memory. Restored non-empty URLs (placeholder or real)
    // count as resolved — no retry storm across restarts. A storage failure
    // throws: startup-critical, same as opening the Outbox.
    if (config_.chat_state.enabled) {
      const std::size_t restored = chat_state_.Restore();
      spdlog::info("application: chat state restored — {} chats from {}",
                   restored, config_.chat_state.db_path);
    }
    StartOutboxRetryThread();
    StartStateFlushThread();

    // Signal wait: both stop paths are relaxed atomic stores (async-signal
    // safe), polled here.
    while (!ShutdownRequested() && !g_global_stop.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(kSignalPollInterval);
    }
  }

  // v5 §3.6 ordered shutdown, async edition (2026-11-09): stop the
  // source of new work first. backfill/scanner Shutdown refuse new TDLib
  // sends; telegram_.Stop() completes every pending TDLib callback, which
  // fires the download callbacks, which post their finish stages to the
  // pool — so the pool is drained AFTER telegram, and the streamers only
  // after the pool (finish stages Enqueue to them).
  backfill_.Shutdown();                         // 0. refuse new jobs/pages
  dialog_scanner_.Shutdown();                   //    (pending callbacks no-op)
  flood_retry_.Shutdown();                      //    drop FloodWait re-sends
  telegram_.Stop();                             // 1. close TDLib session
  files_.FailAll("shutting down");              // 2. safety net for downloads
  workers_->Stop(kWorkerDrainTimeout);          // 3. drain in-flight work
  process_pool_.Stop();                     // 4. WritesDone/Finish/CQ join
  search_pool_.Stop();
  StopOutboxRetryThread();                      // 5. flush + join retry
  StopStateFlushThread();                       // 6. final full flush + join
  spdlog::info("application: stopped cleanly (outbox pending: {})",
               outbox_.PendingCount());
  return 0;
}

}  // namespace tg::app
