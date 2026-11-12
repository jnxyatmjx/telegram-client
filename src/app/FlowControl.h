#ifndef TG_APP_FLOWCONTROL_H_
#define TG_APP_FLOWCONTROL_H_

// Flow-control window for the ProcessImages path (retry design §2(c)).
//
// Caps the number of "sent-but-unreplied" process requests: copies accepted
// into a lane's write queue (not yet written — write-ok flips their records
// to SENT and the slot stays held) plus records already SENT and awaiting
// their ImageReply. MediaThumbnailHandler::FinishProcess and
// Application::SweepOutbox TryAcquire() before every Enqueue; when
// TryAcquire() fails the record stays PENDING/due and a later sweep retries
// it, so the server's queue depth never grows beyond the window and
// per-image latency stays roughly flat. Slots are released when a request's
// fate resolves: reply DONE, write lost, safety-net requeue, permanent-
// failure dead-letter of a SENT record.
//
// SearchImages bypasses the window entirely (it has no Outbox).
//
// The counter is signed and may transiently go negative through rare races
// (a reply and the safety net resolving the same record, an id re-processed
// while in flight). That direction is benign — an under-count only softens
// the window — while saturating at zero would lose a legitimate release and
// eventually wedge the pipeline at the cap.

#include <atomic>
#include <cstdint>

namespace tg::app {

class FlowControl {
 public:
  explicit FlowControl(std::int64_t window) : window_(window) {}

  // False = window full: do NOT enqueue; leave the record for the sweep.
  bool TryAcquire() {
    if (window_ <= 0) return true;  // 0 disables the window
    std::int64_t current = in_flight_.load(std::memory_order_acquire);
    for (;;) {
      if (current >= window_) return false;
      if (in_flight_.compare_exchange_weak(current, current + 1,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
        return true;
      }
    }
  }

  void Release() {
    if (window_ <= 0) return;
    in_flight_.fetch_sub(1, std::memory_order_acq_rel);
  }

  std::int64_t in_flight() const {
    return in_flight_.load(std::memory_order_acquire);
  }
  std::int64_t window() const { return window_; }

 private:
  std::int64_t window_;
  std::atomic<std::int64_t> in_flight_{0};
};

}  // namespace tg::app

#endif  // TG_APP_FLOWCONTROL_H_
