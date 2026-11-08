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
};

struct GrpcConfig {
  std::string target;
  bool tls = true;
  std::string token;
  // Optional PEM file with the CA(s) that signed the server certificate
  // (private/self-signed CA). Empty = system default roots.
  std::string ca_cert_path;
  std::int64_t ack_timeout_ms = 30'000;          // app-level ack timeout
  std::int64_t reconnect_backoff_ms = 1'000;     // 1s -> 2s -> ... capped
  std::int64_t max_reconnect_backoff_ms = 30'000;
  std::int64_t enqueue_timeout_ms = 5'000;       // streamer write-queue wait
  std::int64_t connect_timeout_ms = 10'000;
  int queue_capacity = 1'000;                    // bounded write queue (§6)
};

struct MediaThumbnailConfig {
  bool enabled = true;
  std::int64_t max_thumb_bytes = 5 * 1024 * 1024;
  int max_edge_px = 320;  // photo size tier: largest edge <= this
};

struct BotCommandConfig {
  bool enabled = true;
  // Trigger mode (a): the bot itself sends the command message; verify
  // sender_user_id == command_bot_user_id.
  std::int64_t command_bot_user_id = 0;
  std::string process_command = "/process";  // placeholder names (user TBD)
  std::string search_command = "/search";
  std::int64_t get_message_timeout_ms = 10'000;
};

struct AutoSearchConfig {
  bool enabled = false;  // phase-2 trigger; OFF by default
  std::vector<std::int64_t> chats;  // TDLib dialog chat_ids (-100... form)
  std::int64_t dedup_ttl_seconds = 600;
};

struct OutboxConfig {
  std::string db_path = "/data/outbox.sqlite";
  std::int64_t retry_interval_ms = 10'000;   // retry thread sweep period
  std::int64_t retry_backoff_ms = 5'000;     // per-record backoff base
  int max_attempts = 5;                      // then dead-letter
  int64_t purge_done_after_seconds = 24 * 3600;
};

struct Config {
  TelegramConfig telegram;
  GrpcConfig grpc;
  MediaThumbnailConfig media_thumbnail;
  BotCommandConfig bot_command;
  AutoSearchConfig auto_search;
  OutboxConfig outbox;
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
