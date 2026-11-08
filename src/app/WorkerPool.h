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

class WorkerPool {
 public:
  using Task = std::function<void()>;

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
  // stopping (caller decides whether that loses data).
  bool Post(Task task) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (stopping_.load(std::memory_order_relaxed)) return false;
      if (!cv_can_push_.wait_for(lock, post_timeout_, [&] {
            return stopping_.load(std::memory_order_relaxed) ||
                   queue_.size() < capacity_;
          })) {
        spdlog::error("worker pool: queue full ({} tasks), dropping task",
                      queue_.size());
        return false;
      }
      if (stopping_.load(std::memory_order_relaxed)) return false;
      queue_.push(std::move(task));
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
    return queue_.size();
  }

 private:
  bool IdleLocked() const {
    return queue_.empty() && active_.load(std::memory_order_relaxed) == 0;
  }

  void Loop() {
    for (;;) {
      Task task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_pop_.wait(lock, [&] {
          return !queue_.empty() || stopping_.load(std::memory_order_relaxed);
        });
        if (queue_.empty()) {
          if (stopping_.load(std::memory_order_relaxed)) return;
          continue;
        }
        task = std::move(queue_.front());
        queue_.pop();
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
  std::queue<Task> queue_;
  mutable std::mutex mutex_;
  std::condition_variable cv_pop_;
  std::condition_variable cv_can_push_;
  std::condition_variable cv_idle_;
  std::atomic<size_t> active_{0};
  std::atomic<bool> stopping_{false};
};

}  // namespace tg::app

#endif  // TG_APP_WORKERPOOL_H_
