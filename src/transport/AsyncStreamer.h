#ifndef TG_TRANSPORT_ASYNCSTREAMER_H_
#define TG_TRANSPORT_ASYNCSTREAMER_H_

// AsyncStreamer<Request, Reply> (architecture v5 §3.4): one fully-async
// bidirectional gRPC stream with its own CompletionQueue and CQ thread.
// Two instances exist in the process (ProcessImages / SearchImages), sharing
// one Channel but fully isolated from each other.
//
// Thread rules (v5 §3.3):
//  - ONLY the CQ thread calls Write / Read / StartCall / WritesDone / Finish.
//  - Workers and the Outbox Retry thread only Enqueue() — never blocking on
//    gRPC, only on the bounded write queue (v5 §6 backpressure).
//  - The request buffer for an in-flight Write must outlive the op, so the
//    dequeued request moves into pending_write_ until the kWrite tag lands.
//
// Teardown contract (gRPC async API): Finish() must be the LAST operation on
// a stream, and the stream object may only be destroyed once its Finish tag
// has been delivered. So every failure path funnels into FailStream(), which
// issues Finish once; OnFinish() is the only place that destroys the stream.
// Finish() must also wait for a WritesDone already in flight to complete:
// calling Finish() while WritesDone is pending is a gRPC API misuse that
// aborts the process (call_op_set.h assertion, live 2026-11-12), so
// FailStream defers Finish in that case and OnWritesDone() issues it.
// Stop() respects this too: it lets the CQ thread drain the stream (cancel
// -> WritesDone -> Finish -> OnFinish) and joins it BEFORE shutting the CQ
// down. Shutting the CQ down first would strand the stream with an
// outstanding Read and the server would hang in Read() forever.
//
// State machine (all single-threaded inside CqLoop):
//   Connect -> kStart(ok)   -> StartRead + PumpWrite -> kWrite/kRead loop
//   kStart(false) / kWrite(false) / kRead(false) -> WritesDone -> Finish
//   kFinish -> destroy stream -> reconnect with backoff (1s -> 2s -> ... cap)
//   Backoff counter resets on a successful kStart.
//
// On successful (re)connect SetOnReconnected fires — Application uses it to
// sweep the Outbox so unacked records replay by image_id (v5 §2.5).
//
// The CQ loop uses AsyncNext with a short timeout rather than a grpc::Alarm
// to wake the write pump; enqueue latency is bounded by the poll interval
// (50ms) — simple and alarm-lifetime-free (deviation noted in README).

#include <grpc/support/time.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/async_stream.h>
#include <grpcpp/support/time.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <utility>

namespace tg::transport {

template <typename Request, typename Reply>
class AsyncStreamer {
 public:
  using StreamPtr = std::unique_ptr<
      grpc::ClientAsyncReaderWriterInterface<Request, Reply>>;
  // Creates (PrepareAsync) the stream on the given context/CQ; the streamer
  // then calls StartCall(tag) itself.
  using StartFn = std::function<StreamPtr(grpc::ClientContext* context,
                                          grpc::CompletionQueue* cq)>;
  using ReplyHandler = std::function<void(const Reply& reply)>;
  using StatusHandler = std::function<void(const grpc::Status& status)>;

  struct Options {
    std::string name;  // "ProcessImages" / "SearchImages"
    StartFn start_fn{};  // stub PrepareAsync method
    std::function<std::unique_ptr<grpc::ClientContext>()> make_context{};
    std::int64_t initial_backoff_ms = 1'000;  // v5 §2.5: 1s -> 2s -> 4s ...
    std::int64_t max_backoff_ms = 30'000;
    std::size_t queue_capacity = 1'000;  // bounded write queue (v5 §6)
    std::int64_t pump_interval_ms = 50;
  };

  explicit AsyncStreamer(Options options) : options_(std::move(options)) {}

  ~AsyncStreamer() { Stop(); }

  AsyncStreamer(const AsyncStreamer&) = delete;
  AsyncStreamer& operator=(const AsyncStreamer&) = delete;

  void SetReplyHandler(ReplyHandler handler) {
    reply_handler_ = std::move(handler);
  }
  // Fires on the CQ thread after a (re)connect succeeds; use it to sweep the
  // Outbox for unacked records.
  void SetOnReconnected(std::function<void()> handler) {
    on_reconnected_ = std::move(handler);
  }
  // Fires on the CQ thread when a stream breaks with a status (used to map
  // INVALID_ARGUMENT / PERMISSION_DENIED to Outbox dead-letter).
  void SetOnStreamFailed(StatusHandler handler) {
    on_stream_failed_ = std::move(handler);
  }

  void Start() {
    if (started_.exchange(true, std::memory_order_acq_rel)) return;
    if (!options_.start_fn) {
      spdlog::warn("{}: no start function configured — streamer not started",
                   options_.name);
      started_.store(false, std::memory_order_release);
      return;
    }
    cq_thread_ = std::thread([this] { CqLoop(); });
  }

  // v5 §3.6 step 3: stop enqueueing, let the CQ thread drain the stream
  // (it cancels the live call, waits for the Finish tag, then exits),
  // join it, and only then shut the CQ down.
  //
  // The order matters: shutting the CQ down first strands the stream with
  // an outstanding Read, so the stream is destroyed without its Finish tag
  // and the cancel never reaches the server — the server then hangs in
  // Read() forever and server->Wait() never returns (this was the
  // test_async_streamer teardown hang).
  void Stop() {
    if (!started_.exchange(false, std::memory_order_acq_rel)) return;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      stopping_ = true;
    }
    cv_can_push_.notify_all();
    if (cq_thread_.joinable()) {
      cq_thread_.join();
    }
    // CQ thread is gone; all ops have completed (Finish tag delivered).
    cq_.Shutdown();
    stream_.reset();
    context_.reset();
  }

  // Any thread. Blocks up to `timeout` while the queue is full; false means
  // the request was NOT accepted (caller leaves it in the Outbox, or drops
  // it for SearchReq per v5 §2.4).
  bool Enqueue(Request request, std::chrono::milliseconds timeout) {
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      if (stopping_.load(std::memory_order_relaxed)) return false;
      if (!cv_can_push_.wait_for(lock, timeout, [&] {
            return stopping_.load(std::memory_order_relaxed) ||
                   queue_.size() < options_.queue_capacity;
          })) {
        return false;
      }
      if (stopping_.load(std::memory_order_relaxed)) return false;
      queue_.push(std::move(request));
    }
    return true;
  }

  std::size_t QueuedCount() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    return queue_.size();
  }

  // Copy of the front-of-queue request (observability + tests; requests are
  // small values except image_data, so tests only).
  std::optional<Request> PeekFront() const {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.empty()) return std::nullopt;
    return queue_.front();
  }

  bool IsStreamRunning() const {
    return state_.load(std::memory_order_acquire) == StreamState::kRunning;
  }
  bool IsStarted() const { return started_.load(std::memory_order_acquire); }

 private:
  enum class TagKind { kStart, kWrite, kRead, kWritesDone, kFinish };
  struct Tag {
    TagKind kind;
  };

  enum class StreamState { kIdle, kConnecting, kRunning, kBackingOff };

  static gpr_timespec DeadlineIn(std::int64_t millis) {
    const gpr_timespec now = gpr_now(GPR_CLOCK_REALTIME);
    return gpr_time_add(now, gpr_time_from_millis(millis, GPR_TIMESPAN));
  }

  void CqLoop() {
    Connect();
    while (!stopping_.load(std::memory_order_relaxed)) {
      void* tag = nullptr;
      bool ok = false;
      const auto next =
          cq_.AsyncNext(&tag, &ok, DeadlineIn(options_.pump_interval_ms));
      if (next == grpc::CompletionQueue::NextStatus::SHUTDOWN) {
        break;
      }
      if (next == grpc::CompletionQueue::NextStatus::TIMEOUT) {
        if (stopping_.load(std::memory_order_relaxed)) {
          // Shutdown requested: cancel the live call from the CQ thread
          // (only it may touch the stream/context). The pending
          // Read/StartCall then completes with ok=false, the stream drains
          // through WritesDone -> Finish -> OnFinish, and the loop exits
          // at the top. Cancelling from Stop() itself would race with
          // Connect() resetting context_ on this thread.
          if (context_) context_->TryCancel();
          continue;
        }
        // Idle wake-up: give the write queue a chance (Enqueue cannot
        // interrupt a blocked AsyncNext).
        PumpWrite();
        continue;
      }
      const auto* stream_tag = static_cast<const Tag*>(tag);
      switch (stream_tag->kind) {
        case TagKind::kStart: OnStart(ok); break;
        case TagKind::kWrite: OnWriteDone(ok); break;
        case TagKind::kRead: OnReadDone(ok); break;
        case TagKind::kWritesDone: OnWritesDone(); break;
        case TagKind::kFinish: OnFinish(); break;
      }
    }
    state_.store(StreamState::kIdle, std::memory_order_release);
  }

  void Connect() {
    stream_.reset();
    context_.reset();
    read_open_ = false;
    read_closed_ = false;
    write_in_progress_ = false;
    pending_write_.reset();
    finish_requested_ = false;
    finish_called_ = false;
    writes_done_called_ = false;
    finish_status_ = grpc::Status();

    context_ = options_.make_context ? options_.make_context()
                                     : std::make_unique<grpc::ClientContext>();
    stream_ = options_.start_fn(context_.get(), &cq_);
    if (stream_ == nullptr) {
      // Could not even prepare the call (dead channel) — back off, retry.
      state_.store(StreamState::kBackingOff, std::memory_order_release);
      BackoffAndReconnect();
      return;
    }
    state_.store(StreamState::kConnecting, std::memory_order_release);
    stream_->StartCall(&start_tag_);
  }

  void OnStart(bool ok) {
    if (!ok) {
      FailStream(grpc::Status(grpc::StatusCode::UNAVAILABLE,
                              options_.name + ": StartCall failed"));
      return;
    }
    connect_attempts_ = 0;
    state_.store(StreamState::kRunning, std::memory_order_release);
    if (reconnected_) {
      // Reconnect sweep: replay unacked Outbox records (v5 §2.5).
      if (on_reconnected_) on_reconnected_();
    }
    reconnected_ = true;
    StartRead();
    PumpWrite();
  }

  void StartRead() {
    if (read_open_ || finish_requested_) return;
    reply_ = Reply{};
    stream_->Read(&reply_, &read_tag_);
    read_open_ = true;
  }

  void PumpWrite() {
    if (stopping_.load(std::memory_order_relaxed)) return;
    if (state_.load(std::memory_order_acquire) != StreamState::kRunning) return;
    if (write_in_progress_ || read_closed_ || finish_requested_) return;
    Request next;
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      if (queue_.empty()) return;
      next = std::move(queue_.front());
      queue_.pop();
    }
    write_in_progress_ = true;
    pending_write_ = std::move(next);  // must outlive the Write op
    stream_->Write(*pending_write_, &write_tag_);
  }

  void OnWriteDone(bool ok) {
    write_in_progress_ = false;
    pending_write_.reset();
    if (!ok) {
      FailStream(grpc::Status(grpc::StatusCode::UNAVAILABLE,
                              options_.name + ": Write failed"));
      return;
    }
    PumpWrite();
  }

  void OnReadDone(bool ok) {
    read_open_ = false;
    if (ok) {
      if (reply_handler_) reply_handler_(reply_);
      StartRead();
      return;
    }
    // Server half-closed / stream broken: no further client writes. Queued
    // requests are dropped by FailStream — their Outbox records replay after
    // the reconnect (v5 §2.5).
    read_closed_ = true;
    TryWritesDone();
  }

  void TryWritesDone() {
    if (writes_done_called_ || finish_requested_ || stream_ == nullptr) return;
    writes_done_called_ = true;
    stream_->WritesDone(&writes_done_tag_);
  }

  void OnWritesDone() {
    // WritesDone completed. Finish() must be issued exactly once and only
    // AFTER WritesDone has completed: calling Finish() while a WritesDone
    // is still in flight is a gRPC API misuse — grpc_call_start_batch
    // rejects it and the process aborts on an assertion in call_op_set.h
    // (seen live 2026-11-12 when the keepalive watchdog closed the
    // transport mid-backfill: the Read failed first so WritesDone went
    // out, then the Write failed and FailStream called Finish() while
    // WritesDone was still pending). So FailStream defers Finish when a
    // WritesDone is in flight, and this handler issues it.
    if (finish_called_ || stream_ == nullptr) return;
    finish_requested_ = true;
    finish_called_ = true;
    stream_->Finish(&finish_status_, &finish_tag_);
  }

  void OnFinish() {
    // The Finish tag landed: ALL other ops on this stream are complete.
    // Destroying the stream here is safe.
    const grpc::Status status = finish_status_;
    stream_.reset();
    context_.reset();
    read_open_ = false;
    read_closed_ = true;
    write_in_progress_ = false;
    pending_write_.reset();

    if (!status.ok()) {
      spdlog::warn("{}: stream finished with error: {} ({})", options_.name,
                   status.error_message(),
                   static_cast<int>(status.error_code()));
    } else {
      spdlog::info("{}: stream closed cleanly by server", options_.name);
    }

    // Drop unsent queue contents — their Outbox records are still PENDING
    // and replay after reconnect (v5 §2.5).
    ClearQueue();

    state_.store(StreamState::kBackingOff, std::memory_order_release);
    BackoffAndReconnect();
  }

  // Failure entry point: exactly one Finish is issued on this stream; the
  // stream object is destroyed only when its kFinish tag arrives.
  void FailStream(const grpc::Status& status) {
    if (finish_requested_) return;
    finish_requested_ = true;

    if (on_stream_failed_) on_stream_failed_(status);
    spdlog::warn("{}: stream failed: {} ({}), draining", options_.name,
                 status.error_message(), static_cast<int>(status.error_code()));

    ClearQueue();
    state_.store(StreamState::kBackingOff, std::memory_order_release);

    if (stream_ != nullptr && !finish_called_) {
      if (writes_done_called_) {
        // A WritesDone is still in flight (the Read failed first, so
        // TryWritesDone issued it before this Write failure arrived).
        // Do NOT call Finish() yet: Finish() while WritesDone is pending
        // is a gRPC API misuse that aborts the process (call_op_set.h
        // assertion, live 2026-11-12). OnWritesDone() issues Finish()
        // when the WritesDone tag lands; OnFinish() then destroys the
        // stream and reconnects.
        return;
      }
      finish_called_ = true;
      finish_status_ = status;
      // Last op on the stream; OnFinish() will destroy + reconnect.
      stream_->Finish(&finish_status_, &finish_tag_);
      return;
    }
    // No stream to finish (PrepareAsync returned null): reconnect directly.
    stream_.reset();
    context_.reset();
    BackoffAndReconnect();
  }

  void ClearQueue() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    std::queue<Request> empty;
    queue_.swap(empty);
  }

  void BackoffAndReconnect() {
    if (stopping_.load(std::memory_order_relaxed)) return;
    ++connect_attempts_;
    std::int64_t delay = options_.initial_backoff_ms;
    for (std::int64_t i = 1;
         i < connect_attempts_ && delay < options_.max_backoff_ms; ++i) {
      delay = std::min<std::int64_t>(delay * 2, options_.max_backoff_ms);
    }
    delay = std::min<std::int64_t>(delay, options_.max_backoff_ms);
    spdlog::info("{}: reconnecting in {} ms (attempt {})", options_.name,
                 delay, connect_attempts_);

    // Interruptible sleep: small slices so Stop() stays responsive.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(delay);
    while (!stopping_.load(std::memory_order_relaxed)) {
      if (std::chrono::steady_clock::now() >= deadline) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (stopping_.load(std::memory_order_relaxed)) return;
    Connect();
  }

  Options options_;
  grpc::CompletionQueue cq_;
  std::thread cq_thread_;
  std::atomic<bool> started_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<StreamState> state_{StreamState::kIdle};

  // Stream state — touched only by the CQ thread.
  std::unique_ptr<grpc::ClientContext> context_;
  StreamPtr stream_;
  Reply reply_;
  std::optional<Request> pending_write_;
  bool read_open_ = false;
  bool read_closed_ = false;
  bool write_in_progress_ = false;
  bool finish_requested_ = false;
  bool finish_called_ = false;
  bool writes_done_called_ = false;
  bool reconnected_ = false;
  std::int64_t connect_attempts_ = 0;
  grpc::Status finish_status_;

  // Tags — stable addresses, one per in-flight op kind.
  Tag start_tag_{TagKind::kStart};
  Tag write_tag_{TagKind::kWrite};
  Tag read_tag_{TagKind::kRead};
  Tag writes_done_tag_{TagKind::kWritesDone};
  Tag finish_tag_{TagKind::kFinish};

  // Write queue — producers (workers, retry thread) vs CQ thread.
  std::queue<Request> queue_;
  mutable std::mutex queue_mutex_;
  std::condition_variable cv_can_push_;

  ReplyHandler reply_handler_;
  std::function<void()> on_reconnected_;
  StatusHandler on_stream_failed_;
};

}  // namespace tg::transport

#endif  // TG_TRANSPORT_ASYNCSTREAMER_H_
