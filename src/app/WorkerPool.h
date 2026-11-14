#ifndef TG_APP_WORKERPOOL_H_
#define TG_APP_WORKERPOOL_H_

// Bounded-queue worker pool (architecture v5 §3.2 "Worker Pool").
//
// The TDLib Receive thread only hands updates to the Dispatcher, which Posts
// tasks here — it never executes handlers and never waits for them (§3.3).
// The queue is bounded so a pathological handler backlog cannot exhaust
// memory; if the queue stays full for the whole Post timeout the update is
// dropped with an error log (documented in README). In phase 1 only bot
// commands trigger real work, so this is not on the hot path.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include <spdlog/spdlog.h>

namespace tg::app {

// Search work is taken ahead of process work. A run of search tasks is
// capped so a continuous search burst still lets a waiting process task
// run (one process task after kMaxSearchBurst search tasks).
enum class TaskPriority { kNormal = 0, kSearch = 1 };

class WorkerPool {
 public:
  using Task = std::function<void()>;
  static constexpr int kMaxSearchBurst = 4;

  WorkerPool(size_t thread_count, size_t queue_capacity = 10'000,
             std::chrono::milliseconds post_timeout = std::chrono::milliseconds(1'000))
      : capacity_(queue_capacity), post_timeout_(post_timeout) {
    threads_.reserve(thread_count);
    for (size_t i = 0; i < thread_count; ++i) {
      threads_.emplace_back([this] { Loop(); });
    }
  }

  ~WorkerPool() { Stop(std::chrono::seconds(0)); }

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  // Returns false when the queue stayed full past the timeout or the pool is
  // stopping (caller decides whether that loses data). `kSearch` is served
  // before `kNormal` (see kMaxSearchBurst). Existing callers stay kNormal.
  bool Post(Task task, TaskPriority priority = TaskPriority::kNormal) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (stopping_.load(std::memory_order_relaxed)) return false;
      if (!cv_can_push_.wait_for(lock, post_timeout_, [&] {
            return stopping_.load(std::memory_order_relaxed) ||
                   QueuedLocked() < capacity_;
          })) {
        spdlog::error("worker pool: queue full ({} tasks), dropping task",
                      QueuedLocked());
        return false;
      }
      if (stopping_.load(std::memory_order_relaxed)) return false;
      if (priority == TaskPriority::kSearch) {
        search_.push(std::move(task));
      } else {
        normal_.push(std::move(task));
      }
    }
    cv_pop_.notify_one();
    return true;
  }

  // Waits until all queued/running tasks finished, or the timeout expires.
  bool WaitForIdle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_idle_.wait_for(lock, timeout, [&] { return IdleLocked(); });
  }

  // Stops accepting new tasks; drains up to `drain_timeout`, then joins.
  void Stop(std::chrono::milliseconds drain_timeout) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (stopping_.exchange(true, std::memory_order_relaxed)) return;
    }
    cv_can_push_.notify_all();
    cv_pop_.notify_all();

    if (drain_timeout.count() > 0) {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_idle_.wait_for(lock, drain_timeout, [&] { return IdleLocked(); });
    }
    for (auto& t : threads_) {
      if (t.joinable()) t.join();
    }
    threads_.clear();
  }

  size_t queued() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return QueuedLocked();
  }

 private:
  size_t QueuedLocked() const { return normal_.size() + search_.size(); }

  bool IdleLocked() const {
    return QueuedLocked() == 0 && active_.load(std::memory_order_relaxed) == 0;
  }

  void Loop() {
    for (;;) {
      Task task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_pop_.wait(lock, [&] {
          return QueuedLocked() > 0 || stopping_.load(std::memory_order_relaxed);
        });
        if (QueuedLocked() == 0) {
          if (stopping_.load(std::memory_order_relaxed)) return;
          continue;
        }
        const bool take_search =
            !search_.empty() &&
            (normal_.empty() || search_streak_ < kMaxSearchBurst);
        if (take_search) {
          task = std::move(search_.front());
          search_.pop();
          search_streak_ = normal_.empty() ? 0 : search_streak_ + 1;
        } else {
          task = std::move(normal_.front());
          normal_.pop();
          search_streak_ = 0;
        }
        active_.fetch_add(1, std::memory_order_relaxed);
      }
      task();
      {
        std::unique_lock<std::mutex> lock(mutex_);
        active_.fetch_sub(1, std::memory_order_relaxed);
      }
      cv_can_push_.notify_all();
      cv_idle_.notify_all();
    }
  }

  const size_t capacity_;
  const std::chrono::milliseconds post_timeout_;
  std::vector<std::thread> threads_;
  std::queue<Task> normal_;
  std::queue<Task> search_;
  int search_streak_ = 0;
  mutable std::mutex mutex_;
  std::condition_variable cv_pop_;
  std::condition_variable cv_can_push_;
  std::condition_variable cv_idle_;
  std::atomic<size_t> active_{0};
  std::atomic<bool> stopping_{false};
};

}  // namespace tg::app

#endif  // TG_APP_WORKERPOOL_H_
