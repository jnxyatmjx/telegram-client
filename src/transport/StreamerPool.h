#ifndef TG_TRANSPORT_STREAMERPOOL_H_
#define TG_TRANSPORT_STREAMERPOOL_H_

// StreamerPool: N parallel connections ("lanes") for one RPC, sharing the
// load. Each lane is one AsyncStreamer on its own Channel (its own TCP/HTTP2
// connection) — a single channel multiplexes streams but stays on one
// connection, so real connection-level parallelism needs one channel per
// lane. Requests are dealt round-robin across lanes; replies are matched by
// id (image_id for ProcessImages, (oid,id) for SearchImages), so lane order
// does not matter and no ordering is assumed.
//
// Handlers (reply / reconnected / stream-failed) are installed on every
// lane: a reconnect on any lane fires OnReconnected (Application's sweep is
// idempotent), and a permanent failure on any lane fires OnStreamFailed.
//
// Concurrency: lanes are fixed once Start() is called (AddLane after Start
// is ignored), so Enqueue only reads the lane list. The round-robin counter
// is atomic; Enqueue is safe from worker threads and the retry thread.

#include <transport/AsyncStreamer.h>

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace tg::transport {

template <typename Request, typename Reply>
class StreamerPool {
 public:
  using Streamer = AsyncStreamer<Request, Reply>;
  using ReplyHandler = typename Streamer::ReplyHandler;
  using StatusHandler = typename Streamer::StatusHandler;
  using WrittenHandler = typename Streamer::WrittenHandler;

  StreamerPool() = default;
  explicit StreamerPool(std::vector<typename Streamer::Options> options_list) {
    for (auto& options : options_list) AddLane(std::move(options));
  }
  ~StreamerPool() { Stop(); }

  StreamerPool(const StreamerPool&) = delete;
  StreamerPool& operator=(const StreamerPool&) = delete;

  // Adds one lane. Must be called before Start(); after Start it is
  // ignored (the lane list is immutable once running).
  void AddLane(typename Streamer::Options options) {
    if (started_.load(std::memory_order_acquire)) {
      spdlog::warn("streamer pool: AddLane after Start() ignored");
      return;
    }
    auto lane = std::make_unique<Streamer>(std::move(options));
    if (reply_handler_) lane->SetReplyHandler(reply_handler_);
    if (on_reconnected_) lane->SetOnReconnected(on_reconnected_);
    if (on_stream_failed_) lane->SetOnStreamFailed(on_stream_failed_);
    if (on_written_) lane->SetOnWritten(on_written_);
    if (on_write_lost_) lane->SetOnWriteLost(on_write_lost_);
    lanes_.push_back(std::move(lane));
  }

  std::size_t lane_count() const { return lanes_.size(); }

  // Raw lane access (diagnostics / tests only — e.g. per-lane queue depth).
  // Returns nullptr when out of range. The lanes are owned by the pool.
  Streamer* lane(std::size_t index) {
    return index < lanes_.size() ? lanes_[index].get() : nullptr;
  }

  void Start() {
    if (started_.exchange(true, std::memory_order_acq_rel)) return;
    for (auto& lane : lanes_) lane->Start();
  }

  void Stop() {
    if (!started_.exchange(false, std::memory_order_acq_rel)) return;
    for (auto& lane : lanes_) lane->Stop();
  }

  bool IsStarted() const { return started_.load(std::memory_order_acquire); }
  // True once every lane's stream is up.
  bool IsStreamRunning() const {
    if (lanes_.empty()) return false;
    for (const auto& lane : lanes_) {
      if (!lane->IsStreamRunning()) return false;
    }
    return true;
  }

  // Round-robin across lanes. One lane per call: if that lane's queue stays
  // full for `timeout` the call fails (the caller retries); the next call
  // goes to the following lane.
  bool Enqueue(Request request, std::chrono::milliseconds timeout) {
    if (lanes_.empty()) return false;
    const std::size_t lane =
        next_.fetch_add(1, std::memory_order_relaxed) % lanes_.size();
    return lanes_[lane]->Enqueue(std::move(request), timeout);
  }

  void SetReplyHandler(ReplyHandler handler) {
    reply_handler_ = handler;
    for (auto& lane : lanes_) lane->SetReplyHandler(handler);
  }
  void SetOnReconnected(std::function<void()> handler) {
    on_reconnected_ = handler;
    for (auto& lane : lanes_) lane->SetOnReconnected(handler);
  }
  void SetOnStreamFailed(StatusHandler handler) {
    on_stream_failed_ = handler;
    for (auto& lane : lanes_) lane->SetOnStreamFailed(handler);
  }
  // Write-outcome observers, forwarded to every lane. Only the ProcessImages
  // pool has them wired (Application); SearchImages has no Outbox and never
  // sets them — unset callbacks are a no-op in the streamer.
  void SetOnWritten(WrittenHandler handler) {
    on_written_ = handler;
    for (auto& lane : lanes_) lane->SetOnWritten(handler);
  }
  void SetOnWriteLost(WrittenHandler handler) {
    on_write_lost_ = handler;
    for (auto& lane : lanes_) lane->SetOnWriteLost(handler);
  }

 private:
  std::vector<std::unique_ptr<Streamer>> lanes_;
  std::atomic<std::size_t> next_{0};
  std::atomic<bool> started_{false};
  ReplyHandler reply_handler_;
  std::function<void()> on_reconnected_;
  StatusHandler on_stream_failed_;
  WrittenHandler on_written_;
  WrittenHandler on_write_lost_;
};

}  // namespace tg::transport

#endif  // TG_TRANSPORT_STREAMERPOOL_H_
