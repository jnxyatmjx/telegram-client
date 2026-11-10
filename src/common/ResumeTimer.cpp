#include "common/ResumeTimer.h"

namespace tg::common {

ResumeTimer::ResumeTimer(Callback on_fire)
    : on_fire_(std::move(on_fire)), thread_([this] { Run(); }) {}

ResumeTimer::~ResumeTimer() { Shutdown(); }

void ResumeTimer::Arm(std::chrono::milliseconds delay) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    deadline_ = Clock::now() + delay;
    ++generation_;
  }
  cv_.notify_all();
}

void ResumeTimer::Disarm() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    deadline_.reset();
    ++generation_;
  }
  cv_.notify_all();
}

void ResumeTimer::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    deadline_.reset();
    ++generation_;
  }
  cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void ResumeTimer::Run() {
  std::uint64_t seen_generation = 0;
  while (true) {
    Callback fire;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      while (true) {
        if (stopped_) return;
        if (deadline_.has_value() && generation_ != seen_generation) {
          break;  // a deadline to wait for
        }
        if (deadline_.has_value()) {
          // Same deadline we already fired (generation unchanged): wait for
          // a re-arm instead of hot-looping at the passed deadline.
          cv_.wait(lock);
          continue;
        }
        cv_.wait(lock);
      }
      const auto deadline = *deadline_;
      const std::uint64_t generation = generation_;
      const bool ready = cv_.wait_until(lock, deadline, [&] {
        return stopped_ || generation_ != generation;
      });
      if (stopped_) return;
      if (ready) continue;  // re-armed/disarmed while waiting: re-evaluate
      seen_generation = generation;  // mark this deadline as consumed
      fire = on_fire_;
    }
    // Outside the mutex: the callback may call Arm()/Disarm() again.
    fire();
  }
}

}  // namespace tg::common
