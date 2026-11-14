#include "telegram/FloodRetry.h"

#include <common/Logger.h>
#include <common/ResumeTimer.h>
#include <telegram/TdErrors.h>

#include <algorithm>
#include <chrono>

namespace tg::telegram {
namespace {

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

FloodRetry::FloodRetry() : FloodRetry(Options{}) {}

FloodRetry::FloodRetry(Options options)
    : options_(options),
      timer_(std::make_unique<common::ResumeTimer>(
          [this] { OnFire(); })) {}

FloodRetry::~FloodRetry() { Shutdown(); }

bool FloodRetry::Schedule(const td_api::error& error, int attempt,
                          std::function<void()> retry, int rank) {
  if (attempt > options_.max_attempts) {
    // The caller logs the give-up with its own context (which request it
    // was); nothing is scheduled.
    return false;
  }
  // retry-after from the 429 text (TdErrors is the only parser), padded the
  // Telethon way; an unparseable message falls back to fallback_seconds.
  const std::int64_t seconds =
      RetryAfterOr(error, options_.fallback_seconds) + options_.pad_seconds;
  const std::int64_t delay_ms = seconds * 1000;
  bool armed = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return false;
    pending_.push_back(Pending{NowMs() + delay_ms, delay_ms, attempt, rank,
                               std::move(retry)});
    ArmForEarliestLocked();
    armed = true;
  }
  spdlog::warn("flood_retry: 429 — retry {}/{} scheduled in {} s", attempt,
               options_.max_attempts, seconds);
  return armed;
}

void FloodRetry::OnFire() {
  std::vector<Pending> due;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    const std::int64_t now = NowMs();
    auto split = std::stable_partition(
        pending_.begin(), pending_.end(),
        [now](const Pending& p) { return p.due_ms > now; });
    due.assign(std::make_move_iterator(split),
               std::make_move_iterator(pending_.end()));
    pending_.erase(split, pending_.end());
    ArmForEarliestLocked();
  }
  // Same due time: search (higher rank) before process. A later due time
  // still waits; rank never pulls a retry forward.
  std::stable_sort(due.begin(), due.end(),
                   [](const Pending& a, const Pending& b) {
                     return a.rank > b.rank;
                   });
  // Outside the mutex: each closure sends a TDLib request and/or posts to
  // the worker pool (cheap, non-blocking).
  for (Pending& item : due) {
    if (item.retry) item.retry();
  }
}

void FloodRetry::ArmForEarliestLocked() {
  // Requires mutex_ held.
  std::int64_t earliest = 0;
  for (const Pending& p : pending_) {
    if (earliest == 0 || p.due_ms < earliest) earliest = p.due_ms;
  }
  if (earliest == 0) return;
  const std::int64_t now = NowMs();
  timer_->Arm(std::chrono::milliseconds(earliest > now ? earliest - now : 0));
}

void FloodRetry::Shutdown() {
  std::vector<Pending> dropped;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    stopped_ = true;
    dropped = std::move(pending_);
    pending_.clear();
  }
  // Joins the timer thread; after this no closure can fire.
  timer_->Shutdown();
  if (!dropped.empty()) {
    spdlog::warn("flood_retry: shutdown — dropped {} pending retry(ies)",
                 dropped.size());
  }
}

std::size_t FloodRetry::pending_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pending_.size();
}

}  // namespace tg::telegram
