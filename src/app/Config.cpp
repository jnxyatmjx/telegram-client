#include "app/Config.h"

#include <common/Idempotency.h>
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

// A block sequence entry is "-" or "- " (YAML requires whitespace after
// the dash). "-100123:" is a map key, not a list item.
bool IsBlockSequenceEntry(const std::string& text) {
  if (text.empty() || text.front() != '-') return false;
  return text.size() == 1 ||
         std::isspace(static_cast<unsigned char>(text[1]));
}

// Flow sequence: [a, "b", -100]. nullopt when `value` is not one.
// Throws on an unclosed '[' so a bad list is not silently dropped.
std::optional<std::vector<std::string>> TryParseFlowSequence(
    const std::string& value) {
  if (value.empty() || value.front() != '[') return std::nullopt;
  if (value.back() != ']') {
    throw std::runtime_error("yaml: unclosed flow sequence: " + value);
  }
  std::vector<std::string> items;
  std::string current;
  bool in_single = false;
  bool in_double = false;
  for (size_t i = 1; i + 1 < value.size(); ++i) {
    const char c = value[i];
    if (c == '\'' && !in_double) {
      in_single = !in_single;
      current.push_back(c);
      continue;
    }
    if (c == '"' && !in_single) {
      in_double = !in_double;
      current.push_back(c);
      continue;
    }
    if (c == ',' && !in_single && !in_double) {
      const std::string item = Unquote(Trim(current));
      if (!item.empty()) items.push_back(item);
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  if (in_single || in_double) {
    throw std::runtime_error("yaml: unclosed quote in flow sequence: " + value);
  }
  const std::string item = Unquote(Trim(current));
  if (!item.empty()) items.push_back(item);
  return items;
}

void ParseBlock(const std::vector<YamlLine>& lines, size_t& pos, int indent,
                YamlNode& out) {
  if (pos < lines.size() && IsBlockSequenceEntry(lines[pos].text)) {
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
    } else if (auto items = TryParseFlowSequence(value)) {
      for (auto& item : *items) {
        YamlNode entry;
        entry.scalar = std::move(item);
        child.list.push_back(std::move(entry));
      }
    } else {
      child.scalar = Unquote(value);
    }
    out.map.emplace_back(std::move(key), std::move(child));
  }
}

std::optional<std::int64_t> ToInt64(const std::string& s) {
  if (s.empty()) return std::nullopt;
  std::int64_t v = 0;
  const auto* first = s.data();
  const auto* last = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(first, last, v, 10);
  if (ec != std::errc() || ptr != last) return std::nullopt;
  return v;
}

std::string AsciiLower(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

// auto_search.chats item -> dialog id or public username.
// Accepts -100…, @name, name, https://t.me/name, t.me/name/123,
// and t.me/c/<bare id> (private-channel link -> supergroup dialog id).
AutoSearchChatSpec ParseAutoSearchChat(std::string raw) {
  AutoSearchChatSpec spec;
  spec.raw = raw;
  std::string s = raw;
  std::string lower = AsciiLower(s);
  auto strip_prefix = [&](const char* prefix) {
    const std::string p(prefix);
    if (lower.rfind(p, 0) == 0) {
      s = s.substr(p.size());
      lower = lower.substr(p.size());
      return true;
    }
    return false;
  };
  strip_prefix("https://");
  strip_prefix("http://");
  strip_prefix("t.me/");
  strip_prefix("telegram.me/");
  const auto cut = s.find_first_of("?#");
  if (cut != std::string::npos) s = s.substr(0, cut);
  while (!s.empty() && (s.back() == '/' ||
                        std::isspace(static_cast<unsigned char>(s.back())))) {
    s.pop_back();
  }
  while (!s.empty() && s.front() == '@') s.erase(s.begin());
  if (s.empty()) return spec;

  const std::string path_lower = AsciiLower(s);
  if (path_lower.rfind("c/", 0) == 0) {
    std::string digits = s.substr(2);
    const auto slash = digits.find('/');
    if (slash != std::string::npos) digits = digits.substr(0, slash);
    if (const auto bare = ToInt64(digits); bare && *bare > 0) {
      spec.numeric = true;
      // TDLib channel/supergroup dialog id = -(bare + 10^12).
      spec.dialog_id = -(*bare) - 1'000'000'000'000LL;
      return spec;
    }
  }
  const auto slash = s.find('/');
  if (slash != std::string::npos) s = s.substr(0, slash);
  if (s.empty()) return spec;
  if (const auto id = ToInt64(s)) {
    spec.numeric = true;
    spec.dialog_id = *id;
    return spec;
  }
  spec.username = s;
  return spec;
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

std::vector<std::string> StrList(const YamlNode& node) {
  std::vector<std::string> out;
  out.reserve(node.list.size());
  for (const auto& item : node.list) {
    if (auto v = item.AsString()) out.push_back(*v);
  }
  return out;
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
    cfg.telegram.log_updates = Bool(*t, "log_updates", false);
  }

  if (const YamlNode* g = root.Find("grpc")) {
    cfg.grpc.target = Str(*g, "target", cfg.grpc.target);
    cfg.grpc.tls = Bool(*g, "tls", cfg.grpc.tls);
    cfg.grpc.token = Str(*g, "token", {});
    cfg.grpc.ca_cert_path = Str(*g, "ca_cert", {});
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
    const auto connections = Int(*g, "connections", cfg.grpc.connections);
    cfg.grpc.connections =
        static_cast<int>(connections > 0 ? connections : cfg.grpc.connections);
    cfg.grpc.keepalive_ms = Int(*g, "keepalive_ms", cfg.grpc.keepalive_ms);
    cfg.grpc.keepalive_timeout_ms =
        Int(*g, "keepalive_timeout_ms", cfg.grpc.keepalive_timeout_ms);
    cfg.grpc.keepalive_permit_without_calls = Bool(
        *g, "keepalive_permit_without_calls",
        cfg.grpc.keepalive_permit_without_calls);
    cfg.grpc.max_message_bytes =
        Int(*g, "max_message_bytes", cfg.grpc.max_message_bytes);
    cfg.grpc.http2_lookahead_bytes =
        Int(*g, "http2_lookahead_bytes", cfg.grpc.http2_lookahead_bytes);
    cfg.grpc.http2_write_buffer_bytes = Int(*g, "http2_write_buffer_bytes",
                                            cfg.grpc.http2_write_buffer_bytes);
  }

  if (const YamlNode* f = root.Find("features")) {
    if (const YamlNode* m = f->Find("media_thumbnail")) {
      cfg.media_thumbnail.enabled = Bool(*m, "enabled", cfg.media_thumbnail.enabled);
      cfg.media_thumbnail.max_thumb_bytes =
          Int(*m, "max_thumb_bytes", cfg.media_thumbnail.max_thumb_bytes);
      cfg.media_thumbnail.keyme = Str(*m, "keyme", cfg.media_thumbnail.keyme);
    }
    if (const YamlNode* b = f->Find("bot_command")) {
      cfg.bot_command.enabled = Bool(*b, "enabled", cfg.bot_command.enabled);
      cfg.bot_command.command_bot_user_id =
          Int(*b, "command_bot_user_id", cfg.bot_command.command_bot_user_id);
      cfg.bot_command.process_command = Str(*b, "process_command", cfg.bot_command.process_command);
      cfg.bot_command.search_command = Str(*b, "search_command", cfg.bot_command.search_command);
    }
    if (const YamlNode* a = f->Find("auto_search")) {
      cfg.auto_search.enabled = Bool(*a, "enabled", cfg.auto_search.enabled);
      if (const YamlNode* chats = a->Find("chats")) {
        // Mixed list: integers stay dialog ids; @name / t.me/name are
        // usernames. AsInt64List would silently drop the names.
        // A block list, a flow list `[...]`, or a single scalar all work.
        auto add_chat = [&](const std::string& text) {
          if (text.empty()) return;
          const AutoSearchChatSpec spec = ParseAutoSearchChat(text);
          cfg.auto_search.chat_specs.push_back(spec);
          if (spec.numeric) cfg.auto_search.chats.push_back(spec.dialog_id);
        };
        if (!chats->list.empty()) {
          for (const auto& item : chats->list) {
            if (const auto text = item.AsString()) add_chat(*text);
          }
        } else if (chats->map.empty() && !chats->scalar.empty()) {
          add_chat(chats->scalar);
        }
      }
      if (const YamlNode* links = a->Find("chat_links")) {
        if (!links->list.empty()) {
          spdlog::warn(
              "auto_search: chat_links must be a map of chat -> URL — "
              "list ignored");
        }
        for (const auto& [key, node] : links->map) {
          const auto url = node.AsString();
          if (!url || url->empty()) {
            spdlog::warn(
                "auto_search: chat_links key \"{}\" has an empty URL — skipped",
                key);
            continue;
          }
          if (url->rfind("https://", 0) != 0 && url->rfind("http://", 0) != 0) {
            spdlog::warn(
                "auto_search: chat_links \"{}\" -> \"{}\" is not an http(s) "
                "URL — skipped",
                key, *url);
            continue;
          }
          const AutoSearchChatSpec spec = ParseAutoSearchChat(key);
          if (!spec.numeric && spec.username.empty()) {
            spdlog::warn(
                "auto_search: chat_links key \"{}\" is not a chat id or "
                "username — skipped",
                key);
            continue;
          }
          AutoSearchChatLink link;
          link.raw_key = key;
          link.url = *url;
          link.numeric = spec.numeric;
          link.username = spec.username;
          if (spec.numeric) {
            link.dialog_id = spec.dialog_id;
            if (spec.dialog_id > 0) {
              link.bare_id = spec.dialog_id;
            } else {
              const std::int64_t bare =
                  -spec.dialog_id - common::kSupergroupMark;
              if (bare > 0) link.bare_id = bare;
            }
          }
          cfg.auto_search.chat_links.push_back(std::move(link));
        }
      }
      cfg.auto_search.dedup_ttl_seconds =
          Int(*a, "dedup_ttl_seconds", cfg.auto_search.dedup_ttl_seconds);
      cfg.auto_search.max_inflight = static_cast<int>(
          Int(*a, "max_inflight", cfg.auto_search.max_inflight));
      if (cfg.auto_search.max_inflight < 1) cfg.auto_search.max_inflight = 1;
      if (cfg.auto_search.max_inflight > 16) cfg.auto_search.max_inflight = 16;
    }
    if (const YamlNode* c = f->Find("commands")) {
      cfg.commands.enabled = Bool(*c, "enabled", cfg.commands.enabled);
      cfg.commands.control_chat_id =
          Int(*c, "control_chat_id", cfg.commands.control_chat_id);
      cfg.commands.self_commands =
          Bool(*c, "self_commands", cfg.commands.self_commands);
      cfg.commands.rsme_command =
          Str(*c, "rsme_command", cfg.commands.rsme_command);
      cfg.commands.rsor_command =
          Str(*c, "rsor_command", cfg.commands.rsor_command);
      if (const YamlNode* cmds = c->Find("backfill_commands")) {
        const auto commands = StrList(*cmds);
        if (!commands.empty()) cfg.commands.backfill_commands = commands;
      }
      cfg.commands.dialog_limit =
          static_cast<int>(Int(*c, "dialog_limit", cfg.commands.dialog_limit));
      cfg.commands.list_batch_lines = static_cast<int>(
          Int(*c, "list_batch_lines", cfg.commands.list_batch_lines));
      cfg.commands.rsor_report_limit = static_cast<int>(
          Int(*c, "rsor_report_limit", cfg.commands.rsor_report_limit));
    }
    if (const YamlNode* h = f->Find("history_backfill")) {
      cfg.history_backfill.enabled =
          Bool(*h, "enabled", cfg.history_backfill.enabled);
      cfg.history_backfill.page_limit =
          static_cast<int>(Int(*h, "page_limit", cfg.history_backfill.page_limit));
      cfg.history_backfill.flood_pad_seconds = static_cast<int>(
          Int(*h, "flood_pad_seconds", cfg.history_backfill.flood_pad_seconds));
    }
    if (const YamlNode* mc = f->Find("media_channels")) {
      cfg.media_channels.enabled =
          Bool(*mc, "enabled", cfg.media_channels.enabled);
      if (const YamlNode* chats = mc->Find("chats")) {
        cfg.media_channels.chats = chats->AsInt64List();
      }
      cfg.media_channels.rpc = Str(*mc, "rpc", cfg.media_channels.rpc);
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
    cfg.outbox.send_grace_ms = Int(*o, "send_grace_ms", cfg.outbox.send_grace_ms);
    cfg.outbox.sent_timeout_ms =
        Int(*o, "sent_timeout_ms", cfg.outbox.sent_timeout_ms);
    cfg.outbox.max_in_flight =
        static_cast<int>(Int(*o, "max_in_flight", cfg.outbox.max_in_flight));
  }

  if (const YamlNode* c = root.Find("chat_state")) {
    cfg.chat_state.enabled = Bool(*c, "enabled", cfg.chat_state.enabled);
    cfg.chat_state.db_path = Str(*c, "db_path", cfg.chat_state.db_path);
    cfg.chat_state.flush_interval_seconds = static_cast<int>(
        Int(*c, "flush_interval_seconds", cfg.chat_state.flush_interval_seconds));
    cfg.chat_state.active_window_seconds = static_cast<int>(
        Int(*c, "active_window_seconds", cfg.chat_state.active_window_seconds));
  }

  cfg.worker_count = static_cast<int>(Int(root, "workers", cfg.worker_count));
  if (cfg.worker_count < 1) cfg.worker_count = 1;

  if (const YamlNode* l = root.Find("log")) {
    cfg.log_level = Str(*l, "level", cfg.log_level);
  }

  return cfg;
}

}  // namespace tg::app
