#ifndef TG_COMMON_RESUMETIMER_H_
#define TG_COMMON_RESUMETIMER_H_

// ResumeTimer — one-slot delay scheduler for FloodWait backoff (Telethon
// migration Chunk 2). Telethon's FloodWaitError handler is
// `await asyncio.sleep(e.seconds + 1)`; on the C++ side there is no event
// loop to sleep on, and no thread may block on I/O — so the pause is a
// deadline and this tiny thread fires when it passes.
//
// Contract:
//  - Arm(delay): (re-)arms the single slot; a later Arm replaces an earlier
//    one. `on_fire` runs on the timer thread once the deadline passes —
//    keep it cheap (send a TDLib request, post to the worker pool), never
//    blocking.
//  - Disarm(): cancels the pending slot without firing.
//  - Shutdown(): stops the thread; pending firings are dropped. Called
//    before the owner's other members are torn down.
//
// The timer thread exists even while idle (one condition_variable wait) —
// negligible, and it keeps both HistoryBackfill and DialogScanner free of
// their own polling loops.

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace tg::common {

class ResumeTimer {
 public:
  using Clock = std::chrono::steady_clock;
  using Callback = std::function<void()>;

  // `on_fire` is invoked on the timer thread.
  explicit ResumeTimer(Callback on_fire);
  ~ResumeTimer();  // calls Shutdown()

  ResumeTimer(const ResumeTimer&) = delete;
  ResumeTimer& operator=(const ResumeTimer&) = delete;

  void Arm(std::chrono::milliseconds delay);
  void Disarm();
  void Shutdown();

 private:
  void Run();

  Callback on_fire_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<Clock::time_point> deadline_;
  std::uint64_t generation_ = 0;  // disarms by invalidating older deadlines
  bool stopped_ = false;
  std::thread thread_;
};

}  // namespace tg::common

#endif  // TG_COMMON_RESUMETIMER_H_
