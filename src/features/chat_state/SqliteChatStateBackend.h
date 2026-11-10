#ifndef TG_FEATURES_CHAT_STATE_SQLITECHATSTATEBACKEND_H_
#define TG_FEATURES_CHAT_STATE_SQLITECHATSTATEBACKEND_H_

// Local-SQLite ChatStateBackend — the available default until Hong provides
// D1 credentials (IMPLEMENTATION_PLAN top item 10, 2026-11-10 13:50).
//
// Same sqlite3 dependency and WAL/fullmutex pattern as infra/Outbox, but a
// SEPARATE database file: the state table is unrelated to the Outbox and the
// D1 swap replaces the whole backend (file), not a table inside another
// database. Schema is the Python `chathis` table (SqliteDatabase init_tables
// 104-114 + the upsert columns of schedule2db 1229-1237), so a future
// migration tool can copy rows into D1 verbatim.

#include <features/chat_state/ChatStateBackend.h>

#include <sqlite3.h>

#include <memory>
#include <mutex>
#include <string>

namespace tg::features::chat_state {

// Returns a SQLite backend for `db_path` (created if missing). Throws
// std::runtime_error when the database cannot be opened or the schema
// cannot be created.
std::unique_ptr<ChatStateBackend> MakeSqliteChatStateBackend(
    const std::string& db_path);

class SqliteChatStateBackend : public ChatStateBackend {
 public:
  explicit SqliteChatStateBackend(const std::string& db_path);
  ~SqliteChatStateBackend() override;

  void Upsert(const std::vector<ChatStateRow>& rows) override;
  std::vector<ChatStateRow> LoadAll() override;

 private:
  std::mutex mutex_;  // one connection, WAL mode (v5 §3.3)
  sqlite3* db_ = nullptr;
};

}  // namespace tg::features::chat_state

#endif  // TG_FEATURES_CHAT_STATE_SQLITECHATSTATEBACKEND_H_
