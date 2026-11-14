#ifndef TG_TELEGRAM_FLOODRETRY_H_
#define TG_TELEGRAM_FLOODRETRY_H_

// FloodRetry — the shared FloodWait backoff for the TDLib call sites that
// have no feature-level pause (Telethon migration chunk 3).
//
// HistoryBackfill and DialogScanner pause their WHOLE feature on a 429 via
// common/ResumeTimer and retry the same step. The main pipeline has no such
// owner for three paths — FileDownloader's downloadFile, BotCommandHandler's
// getMessage and MessageSender's sendMessage — and before chunk 3 a 429 on
// any of them just failed the request (a media message was dropped; a reply
// chunk was lost) while the caller kept issuing the NEXT request at once.
// FloodRetry gives those paths one consistent behaviour: re-issue the SAME
// request after Telegram's retry-after (a 429 means the request was
// REJECTED, not applied — re-sending it cannot duplicate anything), bounded
// by max_attempts so a sustained rate limit can never spin.
//
// Contract:
//  - Schedule(error, attempt, retry): arms `retry` to run after
//    retry-after + pad (parsed by telegram/TdErrors — no call site parses
//    the 429 text itself). `attempt` is the 1-based number of the attempt
//    that just failed; the CALLER tracks it and passes attempt + 1 on its
//    next 429. Returns false when attempt > max_attempts — the closure is
//    not run and the caller should give up (drop the message / fail the
//    callbacks) with its own error log.
//  - The closure runs on the timer thread and must be cheap: send a TDLib
//    request (TelegramClient::Send is thread-safe) and/or post the next
//    stage to the worker pool — never block, never do file I/O.
//  - Shutdown(): drops every pending retry and stops the timer thread.
//    Call it BEFORE tearing down what the closures touch (Application does
//    this first in its destructor, ahead of telegram_.Stop()). Idempotent.
//
// One instance is shared (Application::flood_retry_) — one timer thread for
// the whole process; Schedule is safe from any thread.

#include <td/telegram/td_api.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace tg::common {
class ResumeTimer;
}

namespace tg::telegram {
namespace td_api = td::td_api;

class FloodRetry {
 public:
  struct Options {
    // Telethon sleeps e.seconds + 1 after a FloodWait; every flood pause in
    // this codebase pads the same way (history_backfill.flood_pad_seconds).
    int pad_seconds = 1;
    // 429 whose message carries no parseable retry-after: back off this long
    // instead (the same fallback the chunk 2 call sites use).
    std::int64_t fallback_seconds = 5;
    // Re-issue cap per request. Bounded so a long rate limit can never turn
    // into a retry loop; 3 matches the "3 tries then dead-letter" posture of
    // v5 §2.5.
    int max_attempts = 3;
  };

  FloodRetry();
  explicit FloodRetry(Options options);
  ~FloodRetry();  // Shutdown()

  FloodRetry(const FloodRetry&) = delete;
  FloodRetry& operator=(const FloodRetry&) = delete;

  // Arms `retry` per the rules above; false = dropped (attempt cap or shut
  // down) — give up at the call site.
  // `rank` breaks ties when several retries are due together: a higher
  // rank runs first. Search passes 1; process and control notices stay 0.
  // The due time itself is unchanged, so a later search retry does not
  // jump ahead of an earlier process retry.
  bool Schedule(const td_api::error& error, int attempt,
                std::function<void()> retry, int rank = 0);

  // Drops all pending retries and joins the timer thread.
  void Shutdown();

  // Test hook: pending (not yet fired) retry count.
  std::size_t pending_count() const;

 private:
  struct Pending {
    std::int64_t due_ms;
    std::int64_t delay_ms;  // for the log line when it fires
    int attempt;
    int rank = 0;
    std::function<void()> retry;
  };

  void OnFire();
  // Arms the timer for the earliest pending due time (no-op when empty).
  void ArmForEarliestLocked();

  Options options_;
  mutable std::mutex mutex_;
  std::vector<Pending> pending_;
  bool stopped_ = false;
  // Declared last: its thread is joined first in Shutdown (and the
  // destructor), before any other member is torn down.
  std::unique_ptr<common::ResumeTimer> timer_;
};

}  // namespace tg::telegram

#endif  // TG_TELEGRAM_FLOODRETRY_H_
