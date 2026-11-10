#include "features/chat_state/SqliteChatStateBackend.h"

#include <stdexcept>
#include <utility>

namespace tg::features::chat_state {
namespace {

constexpr const char* kSchema = R"(
CREATE TABLE IF NOT EXISTS chathis (
  chat_id    INTEGER PRIMARY KEY,
  offset_id  INTEGER NOT NULL,
  chat_url   TEXT    NOT NULL,
  init_date  TEXT    NOT NULL,
  init_time  TEXT    NOT NULL,
  updated_at TEXT    NOT NULL
);
)";

// Python schedule2db's upsert (app_client.py:1229-1237), ?N-bound.
constexpr const char* kUpsert = R"(
INSERT INTO chathis (chat_id, offset_id, chat_url, init_date, init_time, updated_at)
VALUES (?1, ?2, ?3, ?4, ?5, ?6)
ON CONFLICT(chat_id) DO UPDATE SET
    offset_id = excluded.offset_id,
    chat_url = excluded.chat_url,
    init_date = excluded.init_date,
    init_time = excluded.init_time,
    updated_at = excluded.updated_at
)";

std::string ErrorOf(sqlite3* db) { return sqlite3_errmsg(db); }

class Statement {
 public:
  Statement(sqlite3* db, const char* sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK) {
      throw std::runtime_error("chathis: prepare failed: " +
                               std::string(sqlite3_errmsg(db)));
    }
  }
  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  sqlite3_stmt* get() const { return statement_; }

  // SQLITE_ROW -> true, SQLITE_DONE -> false, else throws.
  bool Step(sqlite3* db) const {
    const int rc = sqlite3_step(statement_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw std::runtime_error("chathis: step failed: " + ErrorOf(db));
  }

 private:
  sqlite3_stmt* statement_ = nullptr;
};

}  // namespace

std::unique_ptr<ChatStateBackend> MakeSqliteChatStateBackend(
    const std::string& db_path) {
  return std::make_unique<SqliteChatStateBackend>(db_path);
}

SqliteChatStateBackend::SqliteChatStateBackend(const std::string& db_path) {
  const int flags =
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  if (sqlite3_open_v2(db_path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
    const std::string message =
        db_ != nullptr ? ErrorOf(db_) : "unknown";
    throw std::runtime_error("chathis: cannot open " + db_path + ": " +
                             message);
  }
  sqlite3_busy_timeout(db_, 5000);
  char* error = nullptr;
  if (sqlite3_exec(db_, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;",
                   nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error != nullptr ? error : "unknown";
    sqlite3_free(error);
    throw std::runtime_error("chathis: pragma failed: " + message);
  }
  if (sqlite3_exec(db_, kSchema, nullptr, nullptr, &error) != SQLITE_OK) {
    const std::string message = error != nullptr ? error : "unknown";
    sqlite3_free(error);
    throw std::runtime_error("chathis: schema failed: " + message);
  }
}

SqliteChatStateBackend::~SqliteChatStateBackend() {
  if (db_ != nullptr) {
    sqlite3_close_v2(db_);
  }
}

void SqliteChatStateBackend::Upsert(const std::vector<ChatStateRow>& rows) {
  if (rows.empty()) return;
  std::lock_guard<std::mutex> lock(mutex_);
  char* error = nullptr;
  if (sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &error) !=
      SQLITE_OK) {
    const std::string message = error != nullptr ? error : "unknown";
    sqlite3_free(error);
    throw std::runtime_error("chathis: begin failed: " + message);
  }
  try {
    Statement statement(db_, kUpsert);
    for (const ChatStateRow& row : rows) {
      sqlite3_bind_int64(statement.get(), 1, row.chat_id);
      sqlite3_bind_int64(statement.get(), 2, row.offset_id);
      sqlite3_bind_text(statement.get(), 3, row.chat_url.data(),
                        static_cast<int>(row.chat_url.size()),
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(statement.get(), 4, row.init_date.data(),
                        static_cast<int>(row.init_date.size()),
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(statement.get(), 5, row.init_time.data(),
                        static_cast<int>(row.init_time.size()),
                        SQLITE_TRANSIENT);
      sqlite3_bind_text(statement.get(), 6, row.updated_at.data(),
                        static_cast<int>(row.updated_at.size()),
                        SQLITE_TRANSIENT);
      statement.Step(db_);
      sqlite3_reset(statement.get());
      sqlite3_clear_bindings(statement.get());
    }
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &error) != SQLITE_OK) {
      const std::string message = error != nullptr ? error : "unknown";
      sqlite3_free(error);
      error = nullptr;
      throw std::runtime_error("chathis: commit failed: " + message);
    }
  } catch (...) {
    sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}

std::vector<ChatStateRow> SqliteChatStateBackend::LoadAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  Statement statement(db_,
      "SELECT chat_id, offset_id, chat_url, init_date, init_time, updated_at "
      "FROM chathis");
  std::vector<ChatStateRow> rows;
  while (statement.Step(db_)) {
    ChatStateRow row;
    row.chat_id = sqlite3_column_int64(statement.get(), 0);
    row.offset_id = sqlite3_column_int64(statement.get(), 1);
    const auto* url = sqlite3_column_text(statement.get(), 2);
    if (url != nullptr) {
      row.chat_url = reinterpret_cast<const char*>(url);
    }
    const auto* date = sqlite3_column_text(statement.get(), 3);
    if (date != nullptr) {
      row.init_date = reinterpret_cast<const char*>(date);
    }
    const auto* time = sqlite3_column_text(statement.get(), 4);
    if (time != nullptr) {
      row.init_time = reinterpret_cast<const char*>(time);
    }
    const auto* updated = sqlite3_column_text(statement.get(), 5);
    if (updated != nullptr) {
      row.updated_at = reinterpret_cast<const char*>(updated);
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace tg::features::chat_state
