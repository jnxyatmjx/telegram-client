#ifndef TG_FEATURES_CHAT_STATE_D1CHATSTATEBACKEND_H_
#define TG_FEATURES_CHAT_STATE_D1CHATSTATEBACKEND_H_

// Cloudflare D1 ChatStateBackend. Mirrors app_client.py D1Database
// (query URL, retries, VALUES chunking) and schedule2db / load_chat_state.
//
// offset_id is converted only here. D1 stores the Telethon/server message
// id (tdlib_mid >> 20). LoadAll shifts it back (<< 20) so ChatStateStore
// and backfill keep comparing TDLib ids. The SQLite backend does not
// convert. A mid that is not divisible by 2^20 is not written; the existing
// D1 offset is kept (the row is skipped when D1 has no row yet).
//
// Upsert throws on failure (ChatStateStore logs and keeps memory). LoadAll
// throws: startup restore treats that like a failed Outbox open and exits.
// Credentials are never logged.

#include <features/chat_state/ChatStateBackend.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tg::features::chat_state {

struct D1Settings {
  std::string account_id;
  std::string api_token;
  std::string database_id;
  std::string api_base = "https://api.cloudflare.com/client/v4";
  int batch_size = 500;
  int max_sql_variables = 90;
  int max_attempts = 3;
  int timeout_seconds = 35;
};

// One POST to the D1 query endpoint. Tests inject this; production uses
// a single reused libcurl easy handle.
struct D1HttpResponse {
  long status = 0;
  std::string body;
  bool transport_error = false;
  std::string error;
  // CURLINFO_NUM_CONNECTS. 0 means the connection was reused.
  long num_connects = -1;
};

class D1HttpTransport {
 public:
  virtual ~D1HttpTransport() = default;
  virtual D1HttpResponse Post(const std::string& url,
                              const std::string& authorization,
                              const std::string& json_body,
                              int timeout_seconds) = 0;
  // Drop the live connection after a transport failure. Default is a no-op.
  virtual void Reset() {}
};

// tdlib_mid >> 20 when mid >= 0 and mid % 2^20 == 0. Otherwise nullopt
// (caller must not write a server id).
std::optional<std::int64_t> TdlibOffsetToServer(std::int64_t tdlib_mid);

// server << 20. nullopt when the shift would overflow or server < 0.
std::optional<std::int64_t> ServerOffsetToTdlib(std::int64_t server_mid);

struct D1Statement {
  std::string sql;
  std::string body;  // {"sql":...,"params":[...]}
  int param_count = 0;
};

// Rows whose offset_id is already the server id. Chunked so each statement
// binds at most max_sql_variables parameters (6 per row) and at most
// batch_size rows.
std::vector<D1Statement> BuildChathisUpserts(
    const std::vector<ChatStateRow>& server_rows, int batch_size,
    int max_sql_variables);

// Throws std::runtime_error on success!=true, a per-result error, or
// malformed JSON. On success, rows have TDLib-scale offset_id, init_date
// as digits, and init_time without a fractional part. null chat_url is "".
std::vector<ChatStateRow> ParseChathisLoadAll(const std::string& body);

class D1ChatStateBackend : public ChatStateBackend {
 public:
  using Sleeper = std::function<void(std::chrono::milliseconds)>;

  D1ChatStateBackend(D1Settings settings,
                     std::unique_ptr<D1HttpTransport> http,
                     Sleeper sleeper = {});

  void Upsert(const std::vector<ChatStateRow>& rows) override;
  std::vector<ChatStateRow> LoadAll() override;

  // Last request bodies, for tests.
  const std::vector<std::string>& request_bodies() const { return bodies_; }

 private:
  std::string Query(const std::string& sql_body);
  std::string QueryUrl() const;

  D1Settings settings_;
  std::unique_ptr<D1HttpTransport> http_;
  Sleeper sleeper_;
  std::mutex mutex_;
  std::vector<std::string> bodies_;
};

// Production constructor. Throws if libcurl was not linked or a required
// setting is empty. `settings.api_token` is not logged.
std::unique_ptr<ChatStateBackend> MakeD1ChatStateBackend(D1Settings settings);

}  // namespace tg::features::chat_state

#endif  // TG_FEATURES_CHAT_STATE_D1CHATSTATEBACKEND_H_
