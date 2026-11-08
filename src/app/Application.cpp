#include "app/Application.h"

#include <common/Logger.h>
#include <telegram/AuthManager.h>

#include <csignal>
#include <chrono>
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
      outbox_(config_.outbox.db_path),
      workers_(std::make_shared<WorkerPool>(
          static_cast<std::size_t>(config_.worker_count),
          10'000,  // handler queue capacity
          std::chrono::milliseconds(1'000))),
      files_(telegram_),
      transport_(config_.grpc),
      process_handler_(MediaExtractor(config_.media_thumbnail), telegram_,
                       files_),
      search_handler_(MediaExtractor(config_.media_thumbnail), telegram_,
                      files_),
      bot_command_handler_(config_.bot_command, process_handler_,
                           search_handler_),
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
      telegram_, files_, process_pool_, search_pool_, outbox_,
      config_});
  WireHandlers();
  WireStreamerCallbacks();
}

Application::~Application() {
  StopOutboxRetryThread();
  process_pool_.Stop();
  search_pool_.Stop();
  telegram_.Stop();
  workers_->Stop(std::chrono::seconds(0));
}

void Application::WireHandlers() {
  dispatcher_.Register(std::shared_ptr<telegram::IUpdateHandler>(
      &bot_command_handler_, [](telegram::IUpdateHandler*) {}));  // non-owning
  // Phase-2 trigger; disabled by default -> claims nothing.
  dispatcher_.Register(auto_search_handler_);
}

void Application::WireStreamerCallbacks() {
  // ImageReply -> Outbox DONE (v5 §2.4: log + metrics only, never reply into
  // Telegram by default).
  process_pool_.SetReplyHandler(
      [this](const ImageReply& reply) {
        const bool known = outbox_.MarkDone(reply.image_id(), reply.result());
        if (known) {
          spdlog::info("process: DONE {} result: {:.200}", reply.image_id(),
                       reply.result());
        } else {
          spdlog::warn("process: reply for unknown image_id {}", reply.image_id());
        }
      });

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

  // Permanent gRPC failure -> dead-letter all pending (v5 §2.5).
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
      });

  // Stream (re)connected -> sweep the Outbox: unacked records replay by
  // image_id, server dedupes (v5 §2.5).
  process_pool_.SetOnReconnected([this] { SweepOutbox(); });
  search_pool_.SetOnReconnected([this] { SweepOutbox(); });
}

void Application::SweepOutbox() {
  const auto due = outbox_.DueForRetry(NowMs(), kSweepBatch);
  for (const auto& record : due) {
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
      spdlog::warn("outbox: retry sweep could not enqueue {} — will retry",
                   record.image_id);
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
      "application: starting — workers={}, outbox={}, grpc target={}, lanes={}",
      config_.worker_count, config_.outbox.db_path, config_.grpc.target,
      transport_.lane_count());

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
    // shutdown below. The retry thread was never started (its Stop is a
    // no-op in that case); streamers and telegram were started above, so
    // they still need their ordered Stop calls.
  } else if (auth_result != 0) {
    return auth_result;
  } else {
    StartOutboxRetryThread();

    // Signal wait: both stop paths are relaxed atomic stores (async-signal
    // safe), polled here.
    while (!ShutdownRequested() && !g_global_stop.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(kSignalPollInterval);
    }
  }

  // v5 §3.6 ordered shutdown:
  //  1. (updates already stopped being *executed* by draining the pool below)
  workers_->Stop(kWorkerDrainTimeout);          // 2. drain in-flight work
  process_pool_.Stop();                     // 3. WritesDone/Finish/CQ join
  search_pool_.Stop();
  telegram_.Stop();                             // 4. close TDLib session
  StopOutboxRetryThread();                      // 5. flush + join retry
  spdlog::info("application: stopped cleanly (outbox pending: {})",
               outbox_.PendingCount());
  return 0;
}

}  // namespace tg::app
