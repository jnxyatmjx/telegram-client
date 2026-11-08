#ifndef TG_INFRA_OUTBOX_H_
#define TG_INFRA_OUTBOX_H_

// Outbox on SQLite (architecture v5 §2.1, §2.5, §6): retry-only persistence
// for ImageRequest deliveries. The normal path Handler -> Streamer never
// touches disk; the Outbox exists so process crashes, network loss, and
// stream breaks cannot lose a thumbnail. Server dedupes on image_id, so
// at-least-once replay after reconnect is safe.
//
// SearchReq is NOT stored here: the (oid,id) correlation table is memory-only
// and old searches must not be replayed after a restart (v5 §2.4).
//
// State machine (all transitions under one mutex, one SQLite connection in
// WAL mode — v5 §3.3):
//   PENDING  -- ImageReply(image_id) --> DONE
//   PENDING  -- enqueue for retry --> PENDING (attempts++, next_retry_at future)
//   PENDING  -- retryable failure (attempts exhausted) --> FAILED
//   PENDING/FAILED -- permanent failure (INVALID_ARGUMENT, PERMISSION_DENIED)
//                     --> DEAD_LETTER

#include <sqlite3.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace tg::infra {

enum class OutboxState : int {
  kPending = 0,
  kDone = 1,
  kFailed = 2,
  kDeadLetter = 3,
};

struct OutboxRecord {
  std::string image_id;   // idempotency key ("<chat_id>:<message_id>:<uid>")
  std::string image_data; // thumbnail bytes
  std::string memo;       // caption only (v5 §1.2)
  std::int64_t owner_id = 0;
  OutboxState state = OutboxState::kPending;
  int attempts = 0;
  std::int64_t next_retry_at_ms = 0;  // epoch ms
  std::string last_status;            // reply result / error text
  std::int64_t created_at_ms = 0;
  std::int64_t updated_at_ms = 0;
};

class Outbox {
 public:
  // Opens (creating if needed) the database at db_path in WAL mode.
  // Throws std::runtime_error when SQLite cannot open the file.
  explicit Outbox(const std::string& db_path);
  ~Outbox();

  Outbox(const Outbox&) = delete;
  Outbox& operator=(const Outbox&) = delete;

  // Inserts or replaces the record (state forced to PENDING, attempts kept
  // from the record). Same image_id re-inserted = re-arm for replay.
  void Put(const OutboxRecord& record);

  std::optional<OutboxRecord> Get(const std::string& image_id) const;

  // ImageReply arrived: DONE, result recorded. False if unknown id.
  bool MarkDone(const std::string& image_id, const std::string& result);

  // Re-arm for retry: attempts++, next_retry_at = now + delay. Dead-letters
  // instead when attempts reach max_attempts. Returns false if unknown id.
  bool MarkRequeued(const std::string& image_id, std::int64_t retry_delay_ms,
                    int max_attempts, const std::string& note);

  // Retryable failure now (e.g. enqueue failed): FAILED + backoff.
  bool MarkFailed(const std::string& image_id, const std::string& error,
                  std::int64_t retry_delay_ms);

  // Permanent failure (INVALID_ARGUMENT / PERMISSION_DENIED / config error).
  bool MarkDeadLetter(const std::string& image_id, const std::string& error);

  // PENDING/FAILED records whose next_retry_at has passed.
  std::vector<OutboxRecord> DueForRetry(std::int64_t now_ms,
                                        std::size_t limit) const;

  std::int64_t CountByState(OutboxState state) const;
  std::int64_t PendingCount() const { return CountByState(OutboxState::kPending); }

  // Removes DONE records older than the cutoff (housekeeping, v5 §6).
  void PurgeDoneBefore(std::int64_t cutoff_ms);

 private:
  OutboxRecord RowToRecord(sqlite3_stmt* statement) const;

  mutable std::mutex mutex_;
  sqlite3* db_ = nullptr;
};

}  // namespace tg::infra

#endif  // TG_INFRA_OUTBOX_H_
