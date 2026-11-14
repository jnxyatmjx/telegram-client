#ifndef TG_APP_CONFIG_H_
#define TG_APP_CONFIG_H_

// Configuration (architecture v5 §5.1).
//
// File format: a small YAML subset — nested maps by indentation, scalars,
// flat lists (`- item`), `key: value` on one line, `#` comments, quoted or
// bare scalars. Values of the form ${VAR} are expanded from the environment
// (unset -> empty string, warning logged). This avoids pulling a full YAML
// dependency; the example file config/config.example.yaml is valid for it.
// The parser is exercised by tests/test_config.cpp.

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace tg::app {

struct TelegramConfig {
  bool use_test_dc = false;
  std::int32_t api_id = 0;
  std::string api_hash;
  std::string phone;  // user account (not a bot token)
  std::string database_directory;
  std::string files_directory;
  std::string encryption_key;
  // Log every incoming TDLib update (type + full content) at info level.
  // Very verbose (Telegram sends a firehose, especially on startup);
  // off by default.
  bool log_updates = false;
};

struct GrpcConfig {
  std::string target;
  bool tls = true;
  std::string token;
  // Optional PEM file with the CA(s) that signed the server certificate
  // (private/self-signed CA). Empty = system default roots.
  std::string ca_cert_path;
  // App-level ack timeout. SUPERSEDED for the ProcessImages send path by
  // outbox.send_grace_ms / outbox.sent_timeout_ms (retry design §3.1:
  // the timeout is demoted to a safety net driven by write outcomes) —
  // no code reads it anymore. Kept parsed so existing config files load
  // unchanged; do not reuse it for new send-path decisions.
  std::int64_t ack_timeout_ms = 30'000;
  std::int64_t reconnect_backoff_ms = 1'000;     // 1s -> 2s -> ... capped
  std::int64_t max_reconnect_backoff_ms = 30'000;
  std::int64_t enqueue_timeout_ms = 5'000;       // streamer write-queue wait
  std::int64_t connect_timeout_ms = 10'000;
  int queue_capacity = 1'000;                    // bounded write queue (§6)
  // Parallel gRPC connections ("lanes"): each lane is its own Channel with
  // its own ProcessImages + SearchImages stream, and requests are dealt
  // round-robin across lanes. 1 = previous single-connection behavior.
  int connections = 1;
  //
  // Channel args (Telethon migration chunk 3). Applied to every lane's
  // Channel by GrpcTransport::MakeChannelArgs. A value of 0 means "leave
  // gRPC's default" for the *_ms / *_bytes knobs below.
  //
  // HTTP/2 keepalive: ping the server every keepalive_ms; if no ping ack
  // arrives within keepalive_timeout_ms the connection is considered dead
  // and the streamers reconnect — a dead peer is detected while the bidi
  // streams sit idle instead of on the next send. 60 s is deliberately
  // conservative: a server with a stricter minimum ping interval answers
  // with GOAWAY "too_many_pings", so raise this (not lower it) if that
  // shows up in the logs.
  std::int64_t keepalive_ms = 60'000;
  std::int64_t keepalive_timeout_ms = 20'000;
  // Allow keepalive pings while no call is outstanding (e.g. during a
  // reconnect gap): keeps half-dead connections from looking healthy.
  bool keepalive_permit_without_calls = true;
  // Max gRPC message length, BOTH directions, bytes. The Python client sets
  // max_send_message_length=64MiB for the same reason: chunk 1 sends the
  // FULL image (photo largest tier) as ImageRequest.image_data, and full
  // photos must fit in one message.
  std::int64_t max_message_bytes = 64LL * 1024 * 1024;  // 64 MiB
  // HTTP/2 flow-control read-ahead per stream (gRPC default 64 KiB): larger
  // values help throughput on high-latency links carrying multi-MB images.
  std::int64_t http2_lookahead_bytes = 1'048'576;  // 1 MiB
  // Per-stream HTTP/2 write buffer bound (gRPC default 64 KiB): sized so a
  // multi-MB image write is buffered without stalling the write loop.
  std::int64_t http2_write_buffer_bytes = 524'288;  // 512 KiB
};

struct MediaThumbnailConfig {
  bool enabled = true;
  std::int64_t max_thumb_bytes = 5 * 1024 * 1024;
  // AES-GCM memo seed (Python KEYME): SHA256(keyme) is the AES-256 key. A
  // secret — configure as ${KEYME} from the environment, never commit it.
  // Empty = memo encryption is not configured; ProcessImages sends are
  // refused loudly (the server expects ciphertext).
  std::string keyme;
};

struct BotCommandConfig {
  bool enabled = true;
  // Trigger mode (a): the bot itself sends the command message; verify
  // sender_user_id == command_bot_user_id.
  std::int64_t command_bot_user_id = 0;
  std::string process_command = "/process";  // placeholder names (user TBD)
  std::string search_command = "/search";
};

// One auto_search.chats entry. Numeric tokens are TDLib dialog ids
// (-100… int64). Anything else is a public username (@name, name, or a
// t.me/name link) resolved with searchPublicChat after authorization.
// t.me/c/<bare id> is stored as the supergroup/channel dialog id.
struct AutoSearchChatSpec {
  std::string raw;
  bool numeric = false;
  std::int64_t dialog_id = 0;  // set when numeric
  std::string username;        // without '@'; empty when numeric or invalid
};

// One features.auto_search.chat_links entry. The key names a chat (TDLib
// dialog id, bare id, or @name / t.me/name). `url` is the link base used
// for search-result hits in that chat (public t.me URL or a private invite).
struct AutoSearchChatLink {
  std::string raw_key;
  bool numeric = false;
  std::int64_t dialog_id = 0;  // the number as written, when numeric
  std::int64_t bare_id = 0;    // positive bare id when it can be derived
  std::string username;        // without '@'; empty when numeric
  std::string url;
};

struct AutoSearchConfig {
  bool enabled = false;  // phase-2 trigger; OFF by default
  // Numeric dialog ids only, in list order. Existing callers and tests
  // that assign this directly keep working; username entries live in
  // chat_specs and are resolved later.
  std::vector<std::int64_t> chats;
  // Every list entry, numeric and username, in config order.
  std::vector<AutoSearchChatSpec> chat_specs;
  // Per-chat link bases. Numeric keys are indexed by bare id and by the
  // number as written; @name keys are matched when a public username is known.
  std::vector<AutoSearchChatLink> chat_links;
  std::int64_t dedup_ttl_seconds = 600;
  // How many SearchImages streams to keep open (at least grpc.connections).
  // Independent of outbox.max_in_flight, which gates ProcessImages only.
  int max_inflight = 8;
};

struct OutboxConfig {
  std::string db_path = "/data/outbox.sqlite";
  std::int64_t retry_interval_ms = 10'000;   // retry thread sweep period
  std::int64_t retry_backoff_ms = 5'000;     // per-record backoff base
  int max_attempts = 5;                      // then dead-letter
  int64_t purge_done_after_seconds = 24 * 3600;
  // Grace between Put (PENDING) and the write outcome becoming known
  // (retry design §3.1): write-ok -> SENT with the sent_timeout deadline,
  // write-lost -> FAILED due-now. Also the deadline carried by records
  // recovered at startup (SENT -> PENDING) and the delay before a
  // window-blocked record is first retried. Supersedes
  // grpc.ack_timeout_ms for the send path.
  std::int64_t send_grace_ms = 5'000;
  // Safety net (retry design §3.1): a request written but unreplied for
  // this long is requeued (attempts++; dead-letter at max_attempts). Must
  // exceed the worst legitimate in-flight latency — roughly the
  // flow-control window times the per-image processing time (§4 Q2).
  std::int64_t sent_timeout_ms = 600'000;
  // Flow-control window (retry design §2(c)): max sent-but-unreplied
  // ProcessImages requests (copies in lane write queues included).
  // SearchImages bypasses it. 0 = unlimited.
  int max_in_flight = 60;
};

// State persistence (Telethon migration Chunk 2; Python chathis /
// schedule2db 1209-1241). Local SQLite backend with the Python `chathis`
// schema; D1 comes later behind the same ChatStateBackend interface.
struct ChatStateConfig {
  // False = memory-only (Chunk 1 behaviour, nothing restored at startup).
  bool enabled = false;
  std::string db_path = "/data/chathis.sqlite";
  int flush_interval_seconds = 30;   // Python schedule.every(30)
  int active_window_seconds = 45;    // flush only chats active within 45 s
};

// Control-chat command surface (/rsme //rsor //xm ...; Python handlersm /
// handrestore / handlerfetchhistory).
struct CommandsConfig {
  bool enabled = true;
  // TDLib dialog id of the control conversation (where commands are typed).
  // 0 = the whole command surface is off.
  std::int64_t control_chat_id = 0;
  // Trigger directions: outgoing messages in the control chat (the Python
  // model) and/or messages from bot_command.command_bot_user_id there (the
  // existing bot model).
  bool self_commands = true;
  std::string rsme_command = "/rsme";
  std::string rsor_command = "/rsor";
  // Numeric-arg commands that trigger history backfill (bare-id argument).
  std::vector<std::string> backfill_commands = {"/xm", "/xy", "/ym", "/who"};
  int dialog_limit = 1202;         // Python get_dialogs(limit=1202)
  int list_batch_lines = 45;       // handlersm 45-line reply batches
  int rsor_report_limit = 50;      // handrestore 50-finding truncation
};

// History backfill engine (Python handlerfetchhistory 918-963).
struct HistoryBackfillConfig {
  bool enabled = true;
  int page_limit = 60;        // Python get_messages(limit=60); TDLib caps 100
  int flood_pad_seconds = 1;  // Telethon: sleep e.seconds + 1
};

// Whitelist chats whose media is forwarded automatically (Python handmedias
// 1255-1269). Off by default: the realtime ImageReply chain already drives
// the pipeline; this is an opt-in extra (e.g. chats the server never
// acknowledges).
struct MediaChannelsConfig {
  bool enabled = false;
  std::vector<std::int64_t> chats;  // BARE ids (chathis.chat_id space)
  std::string rpc = "process";      // "process" | "search"
};

struct Config {
  TelegramConfig telegram;
  GrpcConfig grpc;
  MediaThumbnailConfig media_thumbnail;
  BotCommandConfig bot_command;
  AutoSearchConfig auto_search;
  OutboxConfig outbox;
  ChatStateConfig chat_state;
  CommandsConfig commands;
  HistoryBackfillConfig history_backfill;
  MediaChannelsConfig media_channels;
  int worker_count = 4;
  std::string log_level = "info";

  // Parses the file at `path`; throws std::runtime_error with a readable
  // message on I/O or syntax problems.
  static Config FromFile(const std::string& path);
  // Expands ${VAR} from the environment.
  static std::string ExpandEnv(const std::string& value);
};

// --- Minimal YAML-subset parser (also used directly by tests) -------------

struct YamlNode {
  std::string scalar;                                  // scalar form
  std::vector<std::pair<std::string, YamlNode>> map;   // mapping form
  std::vector<YamlNode> list;                          // sequence form

  bool IsScalar() const { return !scalar.empty() || (map.empty() && list.empty()); }
  const YamlNode* Find(const std::string& key) const;
  std::optional<std::string> AsString() const;
  std::optional<bool> AsBool() const;
  std::optional<std::int64_t> AsInt64() const;
  std::vector<std::int64_t> AsInt64List() const;
};

// Throws std::runtime_error on malformed input.
YamlNode ParseYaml(const std::string& text);

}  // namespace tg::app

#endif  // TG_APP_CONFIG_H_
