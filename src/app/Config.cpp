#include "app/Config.h"

#include <common/Logger.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace tg::app {
namespace {

std::string Trim(const std::string& s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  size_t e = s.size();
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// Strips a single matching pair of surrounding quotes.
std::string Unquote(const std::string& s) {
  if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                        (s.front() == '\'' && s.back() == '\''))) {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

// Removes a trailing comment (# preceded by start-of-line or whitespace,
// outside quotes) and surrounding whitespace.
std::string StripComment(const std::string& line) {
  bool in_single = false;
  bool in_double = false;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '\'' && !in_double) in_single = !in_single;
    if (c == '"' && !in_single) in_double = !in_double;
    if (c == '#' && !in_single && !in_double) {
      if (i == 0 || std::isspace(static_cast<unsigned char>(line[i - 1]))) {
        return line.substr(0, i);
      }
    }
  }
  return line;
}

struct YamlLine {
  int indent;
  std::string text;
};

bool IsSpaceOnly(const std::string& line) {
  return std::all_of(line.begin(), line.end(), [](char c) {
    return std::isspace(static_cast<unsigned char>(c));
  });
}

std::vector<YamlLine> ScanLines(const std::string& text) {
  std::vector<YamlLine> lines;
  std::istringstream in(text);
  std::string raw;
  while (std::getline(in, raw)) {
    if (!raw.empty() && raw.back() == '\r') raw.pop_back();
    const std::string stripped = Trim(StripComment(raw));
    if (stripped.empty()) continue;
    int indent = 0;
    while (indent < static_cast<int>(raw.size()) &&
           std::isspace(static_cast<unsigned char>(raw[indent]))) {
      ++indent;
    }
    if (raw.substr(0, raw.find_first_not_of(" \t")).find('\t') !=
        std::string::npos) {
      throw std::runtime_error("yaml: tabs are not allowed for indentation");
    }
    lines.push_back({indent, stripped});
  }
  return lines;
}

void ParseBlock(const std::vector<YamlLine>& lines, size_t& pos, int indent,
                YamlNode& out) {
  if (pos < lines.size() && lines[pos].text.rfind("-", 0) == 0) {
    // sequence
    while (pos < lines.size() && lines[pos].indent == indent &&
           lines[pos].text.rfind("-", 0) == 0) {
      const std::string item = Trim(lines[pos].text.substr(1));
      if (item.empty()) {
        throw std::runtime_error("yaml: empty list item is not supported");
      }
      YamlNode child;
      child.scalar = Unquote(item);
      out.list.push_back(std::move(child));
      ++pos;
    }
    return;
  }

  while (pos < lines.size() && lines[pos].indent == indent) {
    const std::string& text = lines[pos].text;
    const size_t colon = text.find(':');
    if (colon == std::string::npos) {
      throw std::runtime_error("yaml: expected 'key: value' line: " + text);
    }
    std::string key = Unquote(Trim(text.substr(0, colon)));
    if (key.empty()) {
      throw std::runtime_error("yaml: empty key");
    }
    std::string value = Trim(text.substr(colon + 1));
    ++pos;

    YamlNode child;
    if (value.empty()) {
      // Nested block (map or list) or genuinely empty value.
      if (pos < lines.size() && lines[pos].indent > indent) {
        ParseBlock(lines, pos, lines[pos].indent, child);
      }
    } else {
      child.scalar = Unquote(value);
    }
    out.map.emplace_back(std::move(key), std::move(child));
  }
}

std::optional<std::int64_t> ToInt64(const std::string& s) {
  std::int64_t v = 0;
  const auto* first = s.data();
  const auto* last = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(first, last, v, 10);
  if (ec != std::errc() || ptr != last) return std::nullopt;
  return v;
}

std::optional<bool> ToBool(const std::string& s) {
  std::string lower;
  lower.reserve(s.size());
  for (char c : s) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (lower == "true" || lower == "yes" || lower == "on") return true;
  if (lower == "false" || lower == "no" || lower == "off") return false;
  return std::nullopt;
}

}  // namespace

// --- YamlNode -------------------------------------------------------------

const YamlNode* YamlNode::Find(const std::string& key) const {
  for (const auto& [k, v] : map) {
    if (k == key) return &v;
  }
  return nullptr;
}

std::optional<std::string> YamlNode::AsString() const {
  if (!scalar.empty()) return scalar;
  if (map.empty() && list.empty()) return std::string{};  // empty value
  return std::nullopt;
}

std::optional<bool> YamlNode::AsBool() const {
  if (auto s = AsString(); s && !s->empty()) return ToBool(*s);
  return std::nullopt;
}

std::optional<std::int64_t> YamlNode::AsInt64() const {
  if (auto s = AsString(); s && !s->empty()) return ToInt64(*s);
  return std::nullopt;
}

std::vector<std::int64_t> YamlNode::AsInt64List() const {
  std::vector<std::int64_t> out;
  out.reserve(list.size());
  for (const auto& item : list) {
    if (auto v = item.AsInt64()) out.push_back(*v);
  }
  return out;
}

YamlNode ParseYaml(const std::string& text) {
  const auto lines = ScanLines(text);
  YamlNode root;
  if (lines.empty()) return root;
  size_t pos = 0;
  ParseBlock(lines, pos, lines.front().indent, root);
  if (pos != lines.size()) {
    throw std::runtime_error("yaml: inconsistent indentation at line: " +
                             lines[pos].text);
  }
  return root;
}

std::string Config::ExpandEnv(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size();) {
    if (value.compare(i, 2, "${") == 0) {
      const size_t close = value.find('}', i + 2);
      if (close != std::string::npos) {
        const std::string var = value.substr(i + 2, close - i - 2);
        const char* env = var.empty() ? nullptr : std::getenv(var.c_str());
        if (env != nullptr) {
          out += env;
        } else {
          spdlog::warn("config: environment variable ${{{}}} is not set; "
                       "using empty value", var);
        }
        i = close + 1;
        continue;
      }
    }
    out += value[i++];
  }
  return out;
}

namespace {

// Helper pair: node lookup + env expansion + default on absence.
std::string Str(const YamlNode& section, const std::string& key,
                const std::string& fallback) {
  const YamlNode* n = section.Find(key);
  if (n == nullptr) return fallback;
  auto v = n->AsString();
  return v ? Config::ExpandEnv(*v) : fallback;
}

std::int64_t Int(const YamlNode& section, const std::string& key,
                 std::int64_t fallback) {
  const YamlNode* n = section.Find(key);
  if (n == nullptr) return fallback;
  auto v = n->AsInt64();
  return v ? *v : fallback;
}

bool Bool(const YamlNode& section, const std::string& key, bool fallback) {
  const YamlNode* n = section.Find(key);
  if (n == nullptr) return fallback;
  auto v = n->AsBool();
  return v ? *v : fallback;
}

}  // namespace

Config Config::FromFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("config: cannot open file: " + path +
                             " (copy config/config.example.yaml and fill in "
                             "real values; secrets via ${ENV})");
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const YamlNode root = ParseYaml(buffer.str());

  Config cfg;

  if (const YamlNode* t = root.Find("telegram")) {
    cfg.telegram.use_test_dc = Bool(*t, "use_test_dc", false);
    cfg.telegram.api_id = static_cast<std::int32_t>(
        Int(*t, "api_id", cfg.telegram.api_id));
    cfg.telegram.api_hash = Str(*t, "api_hash", {});
    cfg.telegram.phone = Str(*t, "phone", {});
    cfg.telegram.database_directory = Str(*t, "database_directory", {});
    cfg.telegram.files_directory = Str(*t, "files_directory", {});
    cfg.telegram.encryption_key = Str(*t, "encryption_key", {});
  }

  if (const YamlNode* g = root.Find("grpc")) {
    cfg.grpc.target = Str(*g, "target", cfg.grpc.target);
    cfg.grpc.tls = Bool(*g, "tls", cfg.grpc.tls);
    cfg.grpc.token = Str(*g, "token", {});
    cfg.grpc.ack_timeout_ms = Int(*g, "ack_timeout_ms", cfg.grpc.ack_timeout_ms);
    cfg.grpc.reconnect_backoff_ms =
        Int(*g, "reconnect_backoff_ms", cfg.grpc.reconnect_backoff_ms);
    cfg.grpc.max_reconnect_backoff_ms =
        Int(*g, "max_reconnect_backoff_ms", cfg.grpc.max_reconnect_backoff_ms);
    cfg.grpc.enqueue_timeout_ms =
        Int(*g, "enqueue_timeout_ms", cfg.grpc.enqueue_timeout_ms);
    cfg.grpc.connect_timeout_ms =
        Int(*g, "connect_timeout_ms", cfg.grpc.connect_timeout_ms);
    const auto capacity = Int(*g, "queue_capacity", cfg.grpc.queue_capacity);
    cfg.grpc.queue_capacity =
        static_cast<int>(capacity > 0 ? capacity : cfg.grpc.queue_capacity);
  }

  if (const YamlNode* f = root.Find("features")) {
    if (const YamlNode* m = f->Find("media_thumbnail")) {
      cfg.media_thumbnail.enabled = Bool(*m, "enabled", cfg.media_thumbnail.enabled);
      cfg.media_thumbnail.max_thumb_bytes =
          Int(*m, "max_thumb_bytes", cfg.media_thumbnail.max_thumb_bytes);
      const auto edge = Int(*m, "max_edge_px", cfg.media_thumbnail.max_edge_px);
      cfg.media_thumbnail.max_edge_px = static_cast<int>(edge);
    }
    if (const YamlNode* b = f->Find("bot_command")) {
      cfg.bot_command.enabled = Bool(*b, "enabled", cfg.bot_command.enabled);
      cfg.bot_command.command_bot_user_id =
          Int(*b, "command_bot_user_id", cfg.bot_command.command_bot_user_id);
      cfg.bot_command.process_command = Str(*b, "process_command", cfg.bot_command.process_command);
      cfg.bot_command.search_command = Str(*b, "search_command", cfg.bot_command.search_command);
      cfg.bot_command.get_message_timeout_ms =
          Int(*b, "get_message_timeout_ms", cfg.bot_command.get_message_timeout_ms);
    }
    if (const YamlNode* a = f->Find("auto_search")) {
      cfg.auto_search.enabled = Bool(*a, "enabled", cfg.auto_search.enabled);
      if (const YamlNode* chats = a->Find("chats")) {
        cfg.auto_search.chats = chats->AsInt64List();
      }
      cfg.auto_search.dedup_ttl_seconds =
          Int(*a, "dedup_ttl_seconds", cfg.auto_search.dedup_ttl_seconds);
    }
  }

  if (const YamlNode* o = root.Find("outbox")) {
    cfg.outbox.db_path = Str(*o, "db_path", cfg.outbox.db_path);
    cfg.outbox.retry_interval_ms = Int(*o, "retry_interval_ms", cfg.outbox.retry_interval_ms);
    cfg.outbox.retry_backoff_ms = Int(*o, "retry_backoff_ms", cfg.outbox.retry_backoff_ms);
    const auto attempts = Int(*o, "max_attempts", cfg.outbox.max_attempts);
    cfg.outbox.max_attempts = static_cast<int>(attempts);
    cfg.outbox.purge_done_after_seconds =
        Int(*o, "purge_done_after_seconds", cfg.outbox.purge_done_after_seconds);
  }

  cfg.worker_count = static_cast<int>(Int(root, "workers", cfg.worker_count));
  if (cfg.worker_count < 1) cfg.worker_count = 1;

  if (const YamlNode* l = root.Find("log")) {
    cfg.log_level = Str(*l, "level", cfg.log_level);
  }

  return cfg;
}

}  // namespace tg::app
