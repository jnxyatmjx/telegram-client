#include "infra/Outbox.h"

#include <common/Logger.h>

#include <chrono>
#include <stdexcept>
#include <utility>

namespace tg::infra {
namespace {

constexpr const char* kSchema = R"(
CREATE TABLE IF NOT EXISTS outbox (
  image_id       TEXT PRIMARY KEY,
  image_data     BLOB NOT NULL,
  memo           TEXT NOT NULL,
  owner_id       INTEGER NOT NULL,
  state          INTEGER NOT NULL,
  attempts       INTEGER NOT NULL,
  next_retry_at  INTEGER NOT NULL,
  last_status    TEXT NOT NULL,
  created_at     INTEGER NOT NULL,
  updated_at     INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS outbox_retry_idx ON outbox (state, next_retry_at);
)";

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

class Statement {
 public:
  Statement(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK) {
      throw std::runtime_error("outbox: prepare failed: " +
                               std::string(sqlite3_errmsg(db)));
    }
  }
  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  sqlite3_stmt* get() const { return statement_; }

  // Returns true when a row is available.
  bool Step(sqlite3* db) const {
    const int rc = sqlite3_step(statement_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw std::runtime_error("outbox: step failed: " +
                             std::string(sqlite3_errmsg(db)));
  }

  void Reset() {
    sqlite3_reset(statement_);
    sqlite3_clear_bindings(statement_);
  }

 private:
  sqlite3_stmt* statement_ = nullptr;
};

void BindText(sqlite3_stmt* statement, int index, const std::string& value) {
  sqlite3_bind_text(statement, index, value.data(),
                    static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

void BindBlob(sqlite3_stmt* statement, int index, const std::string& value) {
  sqlite3_bind_blob(statement, index, value.data(),
                    static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

void BindInt(sqlite3_stmt* statement, int index, std::int64_t value) {
  sqlite3_bind_int64(statement, index, value);
}

std::string ColumnText(sqlite3_stmt* statement, int index) {
  const auto* text = sqlite3_column_text(statement, index);
  const int size = sqlite3_column_bytes(statement, index);
  return text != nullptr ? std::string(reinterpret_cast<const char*>(text),
                                       static_cast<size_t>(size))
                         : std::string{};
}

std::string ColumnBlob(sqlite3_stmt* statement, int index) {
  const auto* blob = sqlite3_column_blob(statement, index);
  const int size = sqlite3_column_bytes(statement, index);
  return blob != nullptr && size > 0
             ? std::string(static_cast<const char*>(blob),
                           static_cast<size_t>(size))
             : std::string{};
}

}  // namespace

Outbox::Outbox(const std::string& db_path) {
  const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  if (sqlite3_open_v2(db_path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
    const std::string message = db_ != nullptr ? sqlite3_errmsg(db_) : "unknown";
    throw std::runtime_error("outbox: cannot open " + db_path + ": " + message);
  }
  sqlite3_busy_timeout(db_, 5000);
  char* error = nullptr;
  if (sqlite3_exec(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;",
                   nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error != nullptr ? error : "unknown";
    sqlite3_free(error);
    throw std::runtime_error("outbox: pragma failed: " + message);
  }
  if (sqlite3_exec(db_, kSchema, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error != nullptr ? error : "unknown";
    sqlite3_free(error);
    throw std::runtime_error("outbox: schema failed: " + message);
  }
  MigrateLegacyImageIds();
}

void Outbox::MigrateLegacyImageIds() {
  std::lock_guard<std::mutex> lock(mutex_);
  // Collect legacy rows (old format contains ':' — the wire format
  // str(message_id) never does).
  Statement select(db_,
      "SELECT image_id FROM outbox WHERE instr(image_id, ':') > 0");
  std::vector<std::string> legacy;
  while (select.Step(db_)) {
    legacy.push_back(ColumnText(select.get(), 0));
  }
  if (legacy.empty()) return;

  char* error = nullptr;
  if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &error) !=
      SQLITE_OK) {
    const std::string message = error != nullptr ? error : "unknown";
    sqlite3_free(error);
    throw std::runtime_error("outbox: legacy migration begin failed: " +
                             message);
  }
  try {
    int converted = 0;
    int dropped = 0;
    int dead_lettered = 0;
    for (const std::string& old_id : legacy) {
      // "<chat>:<message_id>:<uid>": message_id = between the FIRST and the
      // LAST ':' (unique ids are alphanumeric, but be liberal).
      const std::size_t first = old_id.find(':');
      const std::size_t last = old_id.rfind(':');
      const std::string middle =
          (first != std::string::npos && last != std::string::npos &&
           last > first)
              ? old_id.substr(first + 1, last - first - 1)
              : std::string{};
      bool numeric = !middle.empty();
      for (const char c : middle) {
        if (c < '0' || c > '9') {
          numeric = false;
          break;
        }
      }
      if (numeric) {
        Statement exists(db_, "SELECT 1 FROM outbox WHERE image_id = ?1");
        BindText(exists.get(), 1, middle);
        if (exists.Step(db_)) {
          // A current-format row already exists — it wins; drop the legacy
          // row (both dedupe to the same server key anyway).
          Statement del(db_, "DELETE FROM outbox WHERE image_id = ?1");
          BindText(del.get(), 1, old_id);
          del.Step(db_);
          ++dropped;
        } else {
          Statement upd(db_,
              "UPDATE outbox SET image_id = ?1, updated_at = ?2 "
              "WHERE image_id = ?3");
          BindText(upd.get(), 1, middle);
          BindInt(upd.get(), 2, NowMs());
          BindText(upd.get(), 3, old_id);
          upd.Step(db_);
          ++converted;
        }
      } else {
        // Unparseable: never replay it, never wedge the retry loop —
        // dead-letter for inspection.
        Statement dead(db_,
            "UPDATE outbox SET state = ?1, last_status = ?2, updated_at = ?3 "
            "WHERE image_id = ?4");
        BindInt(dead.get(), 1,
                static_cast<std::int64_t>(OutboxState::kDeadLetter));
        BindText(dead.get(), 2,
                 "legacy image_id unparseable — dead-lettered by migration");
        BindInt(dead.get(), 3, NowMs());
        BindText(dead.get(), 4, old_id);
        dead.Step(db_);
        ++dead_lettered;
      }
    }
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &error) != SQLITE_OK) {
      const std::string message = error != nullptr ? error : "unknown";
      sqlite3_free(error);
      error = nullptr;
      throw std::runtime_error("outbox: legacy migration commit failed: " +
                               message);
    }
    spdlog::info("outbox: legacy image_id migration — {} rewritten, {} "
                 "dropped (new-format row kept), {} dead-lettered",
                 converted, dropped, dead_lettered);
  } catch (...) {
    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

Outbox::~Outbox() {
  if (db_ != nullptr) {
    sqlite3_close_v2(db_);
  }
}

OutboxRecord Outbox::RowToRecord(sqlite3_stmt* statement) const {
  OutboxRecord record;
  record.image_id = ColumnText(statement, 0);
  record.image_data = ColumnBlob(statement, 1);
  record.memo = ColumnText(statement, 2);
  record.owner_id = sqlite3_column_int64(statement, 3);
  record.state = static_cast<OutboxState>(sqlite3_column_int(statement, 4));
  record.attempts = sqlite3_column_int(statement, 5);
  record.next_retry_at_ms = sqlite3_column_int64(statement, 6);
  record.last_status = ColumnText(statement, 7);
  record.created_at_ms = sqlite3_column_int64(statement, 8);
  record.updated_at_ms = sqlite3_column_int64(statement, 9);
  return record;
}

void Outbox::Put(const OutboxRecord& record) {
  std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(db_,
      "INSERT INTO outbox (image_id, image_data, memo, owner_id, state, "
      "attempts, next_retry_at, last_status, created_at, updated_at) "
      "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10) "
      "ON CONFLICT(image_id) DO UPDATE SET image_data=?2, memo=?3, "
      "owner_id=?4, state=?5, attempts=?6, next_retry_at=?7, last_status=?8, "
      "updated_at=?10");
  const std::int64_t now = NowMs();
  BindText(statement.get(), 1, record.image_id);
  BindBlob(statement.get(), 2, record.image_data);
  BindText(statement.get(), 3, record.memo);
  BindInt(statement.get(), 4, record.owner_id);
  BindInt(statement.get(), 5, static_cast<std::int64_t>(record.state));
  BindInt(statement.get(), 6, record.attempts);
  BindInt(statement.get(), 7, record.next_retry_at_ms);
  BindText(statement.get(), 8, record.last_status);
  BindInt(statement.get(), 9, now);   // created_at: now on first insert
  BindInt(statement.get(), 10, now);  // updated_at
  statement.Step(db_);
}

std::optional<OutboxRecord> Outbox::Get(const std::string& image_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(db_,
      "SELECT image_id, image_data, memo, owner_id, state, attempts, "
      "next_retry_at, last_status, created_at, updated_at "
      "FROM outbox WHERE image_id = ?1");
  BindText(statement.get(), 1, image_id);
  if (!statement.Step(db_)) return std::nullopt;
  return RowToRecord(statement.get());
}

namespace {

// Shared body for UPDATE statements: sqlite3_step returns SQLITE_DONE for
// those (not SQLITE_ROW); sqlite3_changes tells us whether a row matched.
bool RunTransition(sqlite3* db, Statement& statement) {
  const int rc = sqlite3_step(statement.get());
  if (rc != SQLITE_DONE) {
    throw std::runtime_error("outbox: update failed: " +
                             std::string(sqlite3_errmsg(db)));
  }
  return sqlite3_changes(db) > 0;
}

}  // namespace

bool Outbox::MarkDone(const std::string& image_id, const std::string& result) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Only a record still awaiting its reply transitions to DONE. A record
  // already DONE stays DONE: a duplicate ImageReply for the same image_id
  // (the server replied twice, or the request was re-sent before the
  // ack timeout and both replies arrived) must NOT report a second DONE
  // — the first reply already logged it. The caller distinguishes "already
  // done" from "unknown" with Get().
  Statement statement(db_,
      "UPDATE outbox SET state = ?1, last_status = ?2, updated_at = ?3 "
      "WHERE image_id = ?4 AND state IN (?5, ?6)");
  BindInt(statement.get(), 1, static_cast<std::int64_t>(OutboxState::kDone));
  BindText(statement.get(), 2, result);
  BindInt(statement.get(), 3, NowMs());
  BindText(statement.get(), 4, image_id);
  BindInt(statement.get(), 5, static_cast<std::int64_t>(OutboxState::kPending));
  BindInt(statement.get(), 6, static_cast<std::int64_t>(OutboxState::kFailed));
  return RunTransition(db_, statement);
}

bool Outbox::MarkRequeued(const std::string& image_id, std::int64_t retry_delay_ms,
                          int max_attempts, const std::string& note) {
  std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(db_,
      "UPDATE outbox "
      "SET attempts = attempts + 1, "
      "    state = CASE WHEN attempts + 1 >= ?1 THEN ?2 ELSE state END, "
      "    next_retry_at = ?3, last_status = ?4, updated_at = ?3 "
      "WHERE image_id = ?5");
  BindInt(statement.get(), 1, max_attempts);
  BindInt(statement.get(), 2, static_cast<std::int64_t>(OutboxState::kDeadLetter));
  BindInt(statement.get(), 3, NowMs() + retry_delay_ms);
  BindText(statement.get(), 4, note);
  BindText(statement.get(), 5, image_id);
  return RunTransition(db_, statement);
}

bool Outbox::MarkFailed(const std::string& image_id, const std::string& error,
                        std::int64_t retry_delay_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(db_,
      "UPDATE outbox SET state = ?1, last_status = ?2, "
      "next_retry_at = ?3, updated_at = ?3 WHERE image_id = ?4");
  BindInt(statement.get(), 1, static_cast<std::int64_t>(OutboxState::kFailed));
  BindText(statement.get(), 2, error);
  BindInt(statement.get(), 3, NowMs() + retry_delay_ms);
  BindText(statement.get(), 4, image_id);
  return RunTransition(db_, statement);
}

bool Outbox::MarkDeadLetter(const std::string& image_id, const std::string& error) {
  std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(db_,
      "UPDATE outbox SET state = ?1, last_status = ?2, updated_at = ?3 "
      "WHERE image_id = ?4");
  BindInt(statement.get(), 1, static_cast<std::int64_t>(OutboxState::kDeadLetter));
  BindText(statement.get(), 2, error);
  BindInt(statement.get(), 3, NowMs());
  BindText(statement.get(), 4, image_id);
  return RunTransition(db_, statement);
}

std::vector<OutboxRecord> Outbox::DueForRetry(std::int64_t now_ms,
                                              std::size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(db_,
      "SELECT image_id, image_data, memo, owner_id, state, attempts, "
      "next_retry_at, last_status, created_at, updated_at "
      "FROM outbox WHERE state IN (?1, ?2) AND next_retry_at <= ?3 "
      "ORDER BY next_retry_at ASC LIMIT ?4");
  BindInt(statement.get(), 1, static_cast<std::int64_t>(OutboxState::kPending));
  BindInt(statement.get(), 2, static_cast<std::int64_t>(OutboxState::kFailed));
  BindInt(statement.get(), 3, now_ms);
  BindInt(statement.get(), 4, static_cast<std::int64_t>(limit));

  std::vector<OutboxRecord> due;
  while (statement.Step(db_)) {
    due.push_back(RowToRecord(statement.get()));
  }
  return due;
}

std::int64_t Outbox::CountByState(OutboxState state) const {
  std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(db_, "SELECT COUNT(*) FROM outbox WHERE state = ?1");
  BindInt(statement.get(), 1, static_cast<std::int64_t>(state));
  if (!statement.Step(db_)) return 0;
  return sqlite3_column_int64(statement.get(), 0);
}

void Outbox::PurgeDoneBefore(std::int64_t cutoff_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(
      db_, "DELETE FROM outbox WHERE state = ?1 AND updated_at <= ?2");
  BindInt(statement.get(), 1, static_cast<std::int64_t>(OutboxState::kDone));
  BindInt(statement.get(), 2, cutoff_ms);
  statement.Step(db_);
}

}  // namespace tg::infra
