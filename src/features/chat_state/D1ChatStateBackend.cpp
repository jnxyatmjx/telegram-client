#include "features/chat_state/D1ChatStateBackend.h"

#include <common/Logger.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(TG_HAVE_CURL) && TG_HAVE_CURL
#include <curl/curl.h>
#endif

namespace tg::features::chat_state {
namespace {

constexpr std::int64_t kShift = 1LL << 20;
constexpr int kParamsPerRow = 6;

constexpr const char* kUpsertHead =
    "INSERT INTO chathis (chat_id, offset_id, chat_url, init_date, init_time, "
    "updated_at) VALUES ";
constexpr const char* kUpsertTail =
    " ON CONFLICT(chat_id) DO UPDATE SET "
    "offset_id = excluded.offset_id, "
    "chat_url = excluded.chat_url, "
    "init_date = excluded.init_date, "
    "init_time = excluded.init_time, "
    "updated_at = excluded.updated_at";

constexpr const char* kLoadSql =
    "SELECT chat_id, offset_id, chat_url, init_date, init_time FROM chathis";

struct Json {
  enum class Type { kNull, kBool, kInt, kString, kArray, kObject };
  Type type = Type::kNull;
  bool b = false;
  std::int64_t i = 0;
  std::string s;
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;

  const Json* Find(const std::string& key) const {
    for (const auto& [k, v] : o) {
      if (k == key) return &v;
    }
    return nullptr;
  }
};

class Parser {
 public:
  explicit Parser(const std::string& text) : s_(text) {}

  Json Parse() {
    Skip();
    Json value = ParseValue();
    Skip();
    if (i_ != s_.size()) {
      throw std::runtime_error("d1: trailing data in JSON response");
    }
    return value;
  }

 private:
  void Skip() {
    while (i_ < s_.size() &&
           std::isspace(static_cast<unsigned char>(s_[i_]))) {
      ++i_;
    }
  }

  char Peek() const { return i_ < s_.size() ? s_[i_] : '\0'; }

  char Get() {
    if (i_ >= s_.size()) throw std::runtime_error("d1: truncated JSON");
    return s_[i_++];
  }

  Json ParseValue() {
    Skip();
    const char c = Peek();
    if (c == '{') return ParseObject();
    if (c == '[') return ParseArray();
    if (c == '"') return ParseString();
    if (c == 't' || c == 'f') return ParseBool();
    if (c == 'n') return ParseNull();
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      return ParseNumber();
    }
    throw std::runtime_error("d1: invalid JSON value");
  }

  Json ParseObject() {
    Get();
    Json out;
    out.type = Json::Type::kObject;
    Skip();
    if (Peek() == '}') {
      Get();
      return out;
    }
    while (true) {
      Skip();
      Json key = ParseString();
      Skip();
      if (Get() != ':') throw std::runtime_error("d1: expected ':'");
      Json value = ParseValue();
      out.o.emplace_back(std::move(key.s), std::move(value));
      Skip();
      const char c = Get();
      if (c == '}') return out;
      if (c != ',') throw std::runtime_error("d1: expected ',' or '}'");
    }
  }

  Json ParseArray() {
    Get();
    Json out;
    out.type = Json::Type::kArray;
    Skip();
    if (Peek() == ']') {
      Get();
      return out;
    }
    while (true) {
      out.a.push_back(ParseValue());
      Skip();
      const char c = Get();
      if (c == ']') return out;
      if (c != ',') throw std::runtime_error("d1: expected ',' or ']'");
    }
  }

  Json ParseString() {
    if (Get() != '"') throw std::runtime_error("d1: expected string");
    Json out;
    out.type = Json::Type::kString;
    while (true) {
      const char c = Get();
      if (c == '"') return out;
      if (c != '\\') {
        out.s.push_back(c);
        continue;
      }
      const char e = Get();
      switch (e) {
        case '"':
        case '\\':
        case '/':
          out.s.push_back(e);
          break;
        case 'b':
          out.s.push_back('\b');
          break;
        case 'f':
          out.s.push_back('\f');
          break;
        case 'n':
          out.s.push_back('\n');
          break;
        case 'r':
          out.s.push_back('\r');
          break;
        case 't':
          out.s.push_back('\t');
          break;
        case 'u': {
          unsigned cp = 0;
          for (int k = 0; k < 4; ++k) {
            const char h = Get();
            cp <<= 4;
            if (h >= '0' && h <= '9') cp += static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') cp += static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp += static_cast<unsigned>(h - 'A' + 10);
            else throw std::runtime_error("d1: bad \\u escape");
          }
          if (cp < 0x80) {
            out.s.push_back(static_cast<char>(cp));
          } else if (cp < 0x800) {
            out.s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          } else {
            out.s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          }
          break;
        }
        default:
          throw std::runtime_error("d1: bad escape");
      }
    }
  }

  Json ParseBool() {
    Json out;
    out.type = Json::Type::kBool;
    if (s_.compare(i_, 4, "true") == 0) {
      i_ += 4;
      out.b = true;
      return out;
    }
    if (s_.compare(i_, 5, "false") == 0) {
      i_ += 5;
      out.b = false;
      return out;
    }
    throw std::runtime_error("d1: invalid bool");
  }

  Json ParseNull() {
    if (s_.compare(i_, 4, "null") != 0) {
      throw std::runtime_error("d1: invalid null");
    }
    i_ += 4;
    Json out;
    out.type = Json::Type::kNull;
    return out;
  }

  Json ParseNumber() {
    const std::size_t begin = i_;
    if (Peek() == '-') Get();
    if (!std::isdigit(static_cast<unsigned char>(Peek()))) {
      throw std::runtime_error("d1: invalid number");
    }
    while (std::isdigit(static_cast<unsigned char>(Peek()))) Get();
    bool integral = true;
    if (Peek() == '.') {
      integral = false;
      Get();
      while (std::isdigit(static_cast<unsigned char>(Peek()))) Get();
    }
    if (Peek() == 'e' || Peek() == 'E') {
      integral = false;
      Get();
      if (Peek() == '+' || Peek() == '-') Get();
      while (std::isdigit(static_cast<unsigned char>(Peek()))) Get();
    }
    Json out;
    out.s = s_.substr(begin, i_ - begin);
    if (!integral) {
      // Fractional numbers are not used as ids. Keep the text.
      out.type = Json::Type::kString;
      return out;
    }
    std::int64_t value = 0;
    const auto* first = out.s.data();
    const auto* last = first + out.s.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc() || ptr != last) {
      throw std::runtime_error("d1: integer out of range");
    }
    out.type = Json::Type::kInt;
    out.i = value;
    return out;
  }

  const std::string& s_;
  std::size_t i_ = 0;
};

std::string EscapeJson(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 2);
  out.push_back('"');
  for (const unsigned char c : text) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(kHex[c >> 4]);
          out.push_back(kHex[c & 0xF]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
  return out;
}

const std::vector<Json>& AsArray(const Json& value) {
  if (value.type != Json::Type::kArray) {
    throw std::runtime_error("d1: expected JSON array");
  }
  return value.a;
}

std::vector<const Json*> ResultItems(const Json* result) {
  std::vector<const Json*> items;
  if (result == nullptr || result->type == Json::Type::kNull) return items;
  if (result->type == Json::Type::kArray) {
    for (const Json& item : result->a) items.push_back(&item);
    return items;
  }
  items.push_back(result);
  return items;
}

std::string FieldText(const Json& value) {
  if (value.type == Json::Type::kString) return value.s;
  if (value.type == Json::Type::kInt) return std::to_string(value.i);
  if (value.type == Json::Type::kNull) return {};
  throw std::runtime_error("d1: unexpected JSON field type");
}

std::int64_t FieldInt(const Json& value) {
  if (value.type == Json::Type::kInt) return value.i;
  if (value.type == Json::Type::kString) {
    std::int64_t parsed = 0;
    const auto* first = value.s.data();
    const auto* last = first + value.s.size();
    const auto [ptr, ec] = std::from_chars(first, last, parsed);
    if (ec != std::errc() || ptr != last) {
      throw std::runtime_error("d1: non-integer id field");
    }
    return parsed;
  }
  throw std::runtime_error("d1: missing integer field");
}

std::string StripFraction(std::string time_text) {
  const auto dot = time_text.find('.');
  if (dot != std::string::npos) time_text.resize(dot);
  return time_text;
}

int RowsPerChunk(int batch_size, int max_sql_variables) {
  int rows = std::max(1, batch_size);
  if (max_sql_variables > 0) {
    rows = std::max(1, std::min(rows, max_sql_variables / kParamsPerRow));
  }
  return rows;
}

std::string RequestBody(const std::string& sql,
                        const std::vector<std::string>& params_json) {
  std::string body = "{\"sql\":";
  body += EscapeJson(sql);
  body += ",\"params\":[";
  for (std::size_t i = 0; i < params_json.size(); ++i) {
    if (i != 0) body.push_back(',');
    body += params_json[i];
  }
  body += "]}";
  return body;
}

bool RetryStatus(long status) {
  return status == 429 || status == 500 || status == 502 || status == 503 ||
         status == 504;
}

std::chrono::milliseconds Backoff(int attempt) {
  // attempt is 1-based, sleep after that failed attempt: 0.5 * 2^(n-1), cap 8s.
  double seconds = 0.5;
  for (int i = 1; i < attempt; ++i) seconds *= 2.0;
  if (seconds > 8.0) seconds = 8.0;
  return std::chrono::milliseconds(static_cast<int>(seconds * 1000.0));
}

#if defined(TG_HAVE_CURL) && TG_HAVE_CURL

void EnsureCurlGlobal() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::size_t CurlWrite(char* ptr, std::size_t size, std::size_t nmemb,
                      void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

class CurlTransport : public D1HttpTransport {
 public:
  CurlTransport() {
    EnsureCurlGlobal();
    handle_ = curl_easy_init();
    if (handle_ == nullptr) {
      throw std::runtime_error("d1: curl_easy_init failed");
    }
  }
  ~CurlTransport() override {
    if (handle_ != nullptr) curl_easy_cleanup(handle_);
  }

  D1HttpResponse Post(const std::string& url, const std::string& authorization,
                      const std::string& json_body,
                      int timeout_seconds) override {
    D1HttpResponse result;
    if (handle_ == nullptr) {
      result.transport_error = true;
      result.error = "curl handle is null";
      return result;
    }
    std::string body;
    curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle_, CURLOPT_POST, 1L);
    curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(json_body.size()));
    curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, &CurlWrite);
    curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(handle_, CURLOPT_HTTP_VERSION,
                     static_cast<long>(CURL_HTTP_VERSION_2TLS));
    curl_easy_setopt(handle_, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(handle_, CURLOPT_TCP_KEEPIDLE, 20L);
    curl_easy_setopt(handle_, CURLOPT_TCP_KEEPINTVL, 20L);
    curl_easy_setopt(handle_, CURLOPT_MAXAGE_CONN, 60L);
    curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(handle_, CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds));
    curl_easy_setopt(handle_, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, 0L);

    curl_slist* headers = nullptr;
    const std::string auth = "Authorization: " + authorization;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, headers);

    const CURLcode rc = curl_easy_perform(handle_);
    curl_slist_free_all(headers);
    curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, nullptr);
    if (rc != CURLE_OK) {
      result.transport_error = true;
      result.error = curl_easy_strerror(rc);
      return result;
    }
    curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &result.status);
    long connects = 0;
    curl_easy_getinfo(handle_, CURLINFO_NUM_CONNECTS, &connects);
    result.num_connects = connects;
    result.body = std::move(body);
    spdlog::debug("d1: http {} num_connects={} ({})", result.status, connects,
                  connects == 0 ? "connection reused" : "new connection");
    return result;
  }

  void Reset() override {
    if (handle_ != nullptr) curl_easy_cleanup(handle_);
    handle_ = curl_easy_init();
  }

 private:
  CURL* handle_ = nullptr;
};

#endif  // TG_HAVE_CURL

}  // namespace

std::optional<std::int64_t> TdlibOffsetToServer(std::int64_t tdlib_mid) {
  if (tdlib_mid < 0) return std::nullopt;
  if (tdlib_mid % kShift != 0) return std::nullopt;
  return tdlib_mid >> 20;
}

std::optional<std::int64_t> ServerOffsetToTdlib(std::int64_t server_mid) {
  if (server_mid < 0) return std::nullopt;
  if (server_mid > (std::numeric_limits<std::int64_t>::max() >> 20)) {
    return std::nullopt;
  }
  return server_mid << 20;
}

std::vector<D1Statement> BuildChathisUpserts(
    const std::vector<ChatStateRow>& server_rows, int batch_size,
    int max_sql_variables) {
  std::vector<D1Statement> out;
  if (server_rows.empty()) return out;
  const int rows_per = RowsPerChunk(batch_size, max_sql_variables);
  for (std::size_t offset = 0; offset < server_rows.size();
       offset += static_cast<std::size_t>(rows_per)) {
    const std::size_t end = std::min(
        server_rows.size(), offset + static_cast<std::size_t>(rows_per));
    D1Statement statement;
    statement.sql = kUpsertHead;
    std::vector<std::string> params;
    for (std::size_t i = offset; i < end; ++i) {
      if (i != offset) statement.sql += ", ";
      statement.sql += "(?, ?, ?, ?, ?, ?)";
      const ChatStateRow& row = server_rows[i];
      params.push_back(std::to_string(row.chat_id));
      params.push_back(std::to_string(row.offset_id));
      params.push_back(EscapeJson(row.chat_url));
      params.push_back(EscapeJson(row.init_date));
      params.push_back(EscapeJson(row.init_time));
      params.push_back(EscapeJson(row.updated_at));
    }
    statement.sql += kUpsertTail;
    statement.param_count = static_cast<int>(params.size());
    statement.body = RequestBody(statement.sql, params);
    out.push_back(std::move(statement));
  }
  return out;
}

namespace {

std::string Describe(const Json* node) {
  if (node == nullptr) return {};
  if (node->type == Json::Type::kString) return node->s;
  if (node->type == Json::Type::kInt) return std::to_string(node->i);
  return "error";
}

}  // namespace

std::vector<ChatStateRow> ParseChathisLoadAll(const std::string& body) {
  Json root = Parser(body).Parse();
  if (root.type != Json::Type::kObject) {
    throw std::runtime_error("d1: response is not an object");
  }
  const Json* success = root.Find("success");
  if (success == nullptr || success->type != Json::Type::kBool || !success->b) {
    throw std::runtime_error("d1: API error: " +
                             Describe(root.Find("errors")));
  }
  const Json* result = root.Find("result");
  for (const Json* item : ResultItems(result)) {
    if (item->type != Json::Type::kObject) continue;
    const Json* item_ok = item->Find("success");
    if (item_ok != nullptr && item_ok->type == Json::Type::kBool &&
        !item_ok->b) {
      const Json* err = item->Find("error");
      if (err == nullptr) err = item->Find("errors");
      throw std::runtime_error("d1: query error: " + Describe(err));
    }
  }
  const Json* first = nullptr;
  const auto items = ResultItems(result);
  if (!items.empty()) first = items.front();
  std::vector<ChatStateRow> rows;
  if (first == nullptr || first->type != Json::Type::kObject) return rows;
  const Json* results = first->Find("results");
  if (results == nullptr || results->type == Json::Type::kNull) return rows;
  for (const Json& item : AsArray(*results)) {
    if (item.type != Json::Type::kObject) {
      throw std::runtime_error("d1: result row is not an object");
    }
    const Json* chat_id = item.Find("chat_id");
    const Json* offset = item.Find("offset_id");
    if (chat_id == nullptr || offset == nullptr) {
      throw std::runtime_error("d1: row missing chat_id or offset_id");
    }
    ChatStateRow row;
    row.chat_id = FieldInt(*chat_id);
    const std::int64_t server = FieldInt(*offset);
    const auto tdlib = ServerOffsetToTdlib(server);
    if (!tdlib.has_value()) {
      spdlog::warn(
          "d1: chat {} offset_id {} does not fit a TDLib id (<< 20) — row "
          "skipped",
          row.chat_id, server);
      continue;
    }
    row.offset_id = *tdlib;
    if (const Json* url = item.Find("chat_url")) {
      if (url->type == Json::Type::kNull) {
        row.chat_url.clear();
      } else {
        row.chat_url = FieldText(*url);
      }
    }
    if (const Json* date = item.Find("init_date")) {
      row.init_date = FieldText(*date);
    }
    if (const Json* time = item.Find("init_time")) {
      row.init_time = StripFraction(FieldText(*time));
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

D1ChatStateBackend::D1ChatStateBackend(D1Settings settings,
                                       std::unique_ptr<D1HttpTransport> http,
                                       Sleeper sleeper)
    : settings_(std::move(settings)),
      http_(std::move(http)),
      sleeper_(std::move(sleeper)) {
  if (http_ == nullptr) {
    throw std::runtime_error("d1: HTTP transport is null");
  }
  if (settings_.account_id.empty() || settings_.api_token.empty() ||
      settings_.database_id.empty()) {
    throw std::runtime_error(
        "d1: missing CLOUDFLARE_ACCOUNT_ID, CLOUDFLARE_API_TOKEN, or "
        "D1_DATABASE_ID");
  }
  if (settings_.api_base.empty()) {
    settings_.api_base = "https://api.cloudflare.com/client/v4";
  }
  while (!settings_.api_base.empty() && settings_.api_base.back() == '/') {
    settings_.api_base.pop_back();
  }
  if (settings_.batch_size < 1) settings_.batch_size = 500;
  if (settings_.max_sql_variables < 1) settings_.max_sql_variables = 90;
  if (settings_.max_attempts < 1) settings_.max_attempts = 3;
  if (settings_.timeout_seconds < 1) settings_.timeout_seconds = 35;
  if (!sleeper_) {
    sleeper_ = [](std::chrono::milliseconds delay) {
      std::this_thread::sleep_for(delay);
    };
  }
}

std::string D1ChatStateBackend::QueryUrl() const {
  return settings_.api_base + "/accounts/" + settings_.account_id +
         "/d1/database/" + settings_.database_id + "/query";
}

std::string D1ChatStateBackend::Query(const std::string& sql_body) {
  std::lock_guard<std::mutex> lock(mutex_);
  bodies_.push_back(sql_body);
  const std::string url = QueryUrl();
  const std::string auth = "Bearer " + settings_.api_token;
  std::string last_error;
  for (int attempt = 1; attempt <= settings_.max_attempts; ++attempt) {
    D1HttpResponse response =
        http_->Post(url, auth, sql_body, settings_.timeout_seconds);
    if (response.transport_error) {
      last_error = response.error.empty() ? "network error" : response.error;
      http_->Reset();
      if (attempt >= settings_.max_attempts) {
        throw std::runtime_error("d1: request failed: " + last_error);
      }
      spdlog::warn("d1: request retry {}/{}: {}", attempt,
                   settings_.max_attempts, last_error);
      sleeper_(Backoff(attempt));
      continue;
    }
    if (response.status == 401 || response.status == 403) {
      throw std::runtime_error("d1: auth error: HTTP " +
                               std::to_string(response.status));
    }
    if (RetryStatus(response.status)) {
      last_error = "HTTP " + std::to_string(response.status);
      if (attempt >= settings_.max_attempts) {
        throw std::runtime_error("d1: transient retries exhausted: " +
                                 last_error);
      }
      spdlog::warn("d1: transient HTTP {}, retry {}/{}", response.status,
                   attempt, settings_.max_attempts);
      sleeper_(Backoff(attempt));
      continue;
    }
    if (response.status >= 400) {
      throw std::runtime_error("d1: HTTP error: " +
                               std::to_string(response.status));
    }
    return response.body;
  }
  throw std::runtime_error("d1: request failed: " + last_error);
}

void D1ChatStateBackend::Upsert(const std::vector<ChatStateRow>& rows) {
  if (rows.empty()) return;
  std::vector<ChatStateRow> ready;
  std::vector<ChatStateRow> keep;
  ready.reserve(rows.size());
  for (const ChatStateRow& row : rows) {
    const auto server = TdlibOffsetToServer(row.offset_id);
    if (!server.has_value()) {
      spdlog::warn(
          "d1: chat {} offset_id {} is not a TDLib id (not divisible by "
          "2^20) — keeping the D1 offset",
          row.chat_id, row.offset_id);
      keep.push_back(row);
      continue;
    }
    ChatStateRow copy = row;
    copy.offset_id = *server;
    ready.push_back(std::move(copy));
  }
  if (!keep.empty()) {
    std::unordered_map<std::int64_t, std::int64_t> existing;
    const int cap = std::max(1, settings_.max_sql_variables);
    for (std::size_t offset = 0; offset < keep.size();
         offset += static_cast<std::size_t>(cap)) {
      const std::size_t end =
          std::min(keep.size(), offset + static_cast<std::size_t>(cap));
      std::string sql =
          "SELECT chat_id, offset_id FROM chathis WHERE chat_id IN (";
      std::vector<std::string> chunk_params;
      for (std::size_t i = offset; i < end; ++i) {
        if (i != offset) sql += ", ";
        sql += "?";
        chunk_params.push_back(std::to_string(keep[i].chat_id));
      }
      sql += ")";
      const std::string body = Query(RequestBody(sql, chunk_params));
      Json root = Parser(body).Parse();
      const Json* success = root.Find("success");
      if (success == nullptr || success->type != Json::Type::kBool ||
          !success->b) {
        throw std::runtime_error("d1: API error reading existing offsets");
      }
      const auto items = ResultItems(root.Find("result"));
      if (items.empty() || items.front()->type != Json::Type::kObject) continue;
      const Json* results = items.front()->Find("results");
      if (results == nullptr || results->type != Json::Type::kArray) continue;
      for (const Json& item : results->a) {
        if (item.type != Json::Type::kObject) continue;
        const Json* id = item.Find("chat_id");
        const Json* off = item.Find("offset_id");
        if (id == nullptr || off == nullptr) continue;
        existing[FieldInt(*id)] = FieldInt(*off);
      }
    }
    for (ChatStateRow row : keep) {
      const auto it = existing.find(row.chat_id);
      if (it == existing.end()) {
        spdlog::warn(
            "d1: chat {} has no stored offset and the new mid is not a "
            "TDLib id — row not written",
            row.chat_id);
        continue;
      }
      row.offset_id = it->second;
      ready.push_back(std::move(row));
    }
  }
  const auto statements = BuildChathisUpserts(
      ready, settings_.batch_size, settings_.max_sql_variables);
  for (const D1Statement& statement : statements) {
    const std::string body = Query(statement.body);
    ParseChathisLoadAll(body);  // success / per-result checks; rows ignored
  }
}

std::vector<ChatStateRow> D1ChatStateBackend::LoadAll() {
  const std::string body = Query(RequestBody(kLoadSql, {}));
  return ParseChathisLoadAll(body);
}

std::unique_ptr<ChatStateBackend> MakeD1ChatStateBackend(D1Settings settings) {
#if defined(TG_HAVE_CURL) && TG_HAVE_CURL
  return std::make_unique<D1ChatStateBackend>(
      std::move(settings), std::make_unique<CurlTransport>());
#else
  (void)settings;
  throw std::runtime_error(
      "chat_state.backend is d1, but this build was compiled without "
      "libcurl. Install libcurl and reconfigure, or set chat_state.backend "
      "to sqlite.");
#endif
}

}  // namespace tg::features::chat_state
