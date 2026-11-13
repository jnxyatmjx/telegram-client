#include "features/auto_search/SearchReply.h"

#include <common/Logger.h>
#include <telegram/MarkdownV2.h>

#include <cctype>
#include <charconv>
#include <utility>

namespace tg::features::auto_search {
namespace {

// Python rspwork uses Telethon legacy markdown (**bold**). MarkdownV2
// bold is a single asterisk (td::parse_markdown_v2, MessageEntity.cpp).
// The URL's '.' sits inside the link target, where it is not reserved.
constexpr const char* kNoOneFind =
    "\n⚡️\t*No One Find*\t🎯\n"
    "[👉Maybe Here You Like👈](https://t.me/uuuiii3)";

// Python: memo_str = "🎯 ✅🔥 ✅⚡️ ✅⚡️ ✅🔥 ✅🎯 " when decryption yields
// an empty caption.
constexpr const char* kEmptyMemo = "🎯 ✅🔥 ✅⚡️ ✅⚡️ ✅🔥 ✅🎯 ";

constexpr std::size_t kMemoCodePoints = 28;

std::string FirstCodePoints(const std::string& text, std::size_t limit) {
  std::size_t i = 0;
  std::size_t count = 0;
  while (i < text.size() && count < limit) {
    const auto c = static_cast<unsigned char>(text[i]);
    std::size_t len = 1;
    if ((c & 0x80) == 0) {
      len = 1;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
      len = 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
      len = 3;
    } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
      len = 4;
    }
    i += len;
    ++count;
  }
  return text.substr(0, i);
}

std::string EscapeLinkTarget(const std::string& url) {
  // Inside the (...) of a MarkdownV2 link only ')' and '\' are special.
  std::string out;
  out.reserve(url.size());
  for (const char c : url) {
    if (c == ')' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

bool StartsWithHttp(const std::string& url) {
  return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0;
}

class JsonCursor {
 public:
  explicit JsonCursor(const std::string& text) : s_(text) {}

  void SkipWs() {
    while (i_ < s_.size() &&
           std::isspace(static_cast<unsigned char>(s_[i_]))) {
      ++i_;
    }
  }

  bool Consume(char expected) {
    SkipWs();
    if (i_ < s_.size() && s_[i_] == expected) {
      ++i_;
      return true;
    }
    return false;
  }

  std::optional<std::string> ParseString() {
    SkipWs();
    if (i_ >= s_.size() || s_[i_] != '"') return std::nullopt;
    ++i_;
    std::string out;
    while (i_ < s_.size()) {
      const char c = s_[i_++];
      if (c == '"') return out;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (i_ >= s_.size()) return std::nullopt;
      const char esc = s_[i_++];
      switch (esc) {
        case '"':
        case '\\':
        case '/':
          out.push_back(esc);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          if (i_ + 4 > s_.size()) return std::nullopt;
          unsigned cp = 0;
          for (int k = 0; k < 4; ++k) {
            const char h = s_[i_++];
            cp <<= 4;
            if (h >= '0' && h <= '9') {
              cp += static_cast<unsigned>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
              cp += static_cast<unsigned>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
              cp += static_cast<unsigned>(h - 'A' + 10);
            } else {
              return std::nullopt;
            }
          }
          // BMP only. Memo tokens are base64; a \u escape is rare.
          if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
          } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
          }
          break;
        }
        default:
          return std::nullopt;
      }
    }
    return std::nullopt;
  }

  bool SkipValue() {
    SkipWs();
    if (i_ >= s_.size()) return false;
    const char c = s_[i_];
    if (c == '"') return ParseString().has_value();
    if (c == '{') return SkipObject();
    if (c == '[') return SkipArray();
    if (c == 't' || c == 'f' || c == 'n') {
      while (i_ < s_.size() && std::isalpha(static_cast<unsigned char>(s_[i_]))) {
        ++i_;
      }
      return true;
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      return SkipNumber();
    }
    return false;
  }

  bool SkipObject() {
    if (!Consume('{')) return false;
    SkipWs();
    if (Consume('}')) return true;
    while (true) {
      if (!ParseString()) return false;
      if (!Consume(':')) return false;
      if (!SkipValue()) return false;
      SkipWs();
      if (Consume('}')) return true;
      if (!Consume(',')) return false;
    }
  }

  bool SkipArray() {
    if (!Consume('[')) return false;
    SkipWs();
    if (Consume(']')) return true;
    while (true) {
      if (!SkipValue()) return false;
      SkipWs();
      if (Consume(']')) return true;
      if (!Consume(',')) return false;
    }
  }

  bool SkipNumber() {
    SkipWs();
    if (i_ < s_.size() && s_[i_] == '-') ++i_;
    if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) {
      return false;
    }
    while (i_ < s_.size() &&
           (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' ||
            s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-')) {
      ++i_;
    }
    return true;
  }

  std::optional<std::int64_t> ParseInt() {
    SkipWs();
    const bool as_string = i_ < s_.size() && s_[i_] == '"';
    std::string text;
    if (as_string) {
      auto parsed = ParseString();
      if (!parsed) return std::nullopt;
      text = std::move(*parsed);
    } else {
      if (i_ < s_.size() && s_[i_] == '-') {
        text.push_back('-');
        ++i_;
      }
      if (i_ >= s_.size() || !std::isdigit(static_cast<unsigned char>(s_[i_]))) {
        return std::nullopt;
      }
      while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_]))) {
        text.push_back(s_[i_++]);
      }
      // Drop a fractional / exponent tail; ids are integers.
      if (i_ < s_.size() && (s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E')) {
        if (s_[i_] == '.') {
          ++i_;
          while (i_ < s_.size() &&
                 std::isdigit(static_cast<unsigned char>(s_[i_]))) {
            ++i_;
          }
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
          ++i_;
          if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
          while (i_ < s_.size() &&
                 std::isdigit(static_cast<unsigned char>(s_[i_]))) {
            ++i_;
          }
        }
      }
    }
    std::int64_t value = 0;
    const auto* first = text.data();
    const auto* last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc() || ptr != last) return std::nullopt;
    return value;
  }

 private:
  const std::string& s_;
  std::size_t i_ = 0;
};

struct Hit {
  std::string memo_token;
  bool memo_present = false;
  std::int64_t cid = 0;
  std::int64_t mid = 0;
  bool have_cid = false;
  bool have_mid = false;
};

// Parses a top-level object. nullopt = not a JSON object (No One Find).
// data_present false = missing data (also No One Find, per the approved plan).
struct ParsedResult {
  bool data_present = false;
  std::vector<Hit> hits;
};

std::optional<ParsedResult> ParseResult(const std::string& result) {
  JsonCursor cursor(result);
  if (!cursor.Consume('{')) return std::nullopt;
  ParsedResult parsed;
  cursor.SkipWs();
  if (cursor.Consume('}')) return parsed;
  while (true) {
    auto key = cursor.ParseString();
    if (!key || !cursor.Consume(':')) return std::nullopt;
    if (*key == "data") {
      parsed.data_present = true;
      if (!cursor.Consume('[')) return std::nullopt;
      cursor.SkipWs();
      if (!cursor.Consume(']')) {
        while (true) {
          if (!cursor.Consume('{')) return std::nullopt;
          Hit hit;
          cursor.SkipWs();
          if (!cursor.Consume('}')) {
            while (true) {
              auto field = cursor.ParseString();
              if (!field || !cursor.Consume(':')) return std::nullopt;
              if (*field == "memo") {
                auto memo = cursor.ParseString();
                if (!memo) return std::nullopt;
                hit.memo_token = std::move(*memo);
                hit.memo_present = true;
              } else if (*field == "cid") {
                auto cid = cursor.ParseInt();
                if (!cid) return std::nullopt;
                hit.cid = *cid;
                hit.have_cid = true;
              } else if (*field == "mid") {
                auto mid = cursor.ParseInt();
                if (!mid) return std::nullopt;
                hit.mid = *mid;
                hit.have_mid = true;
              } else if (!cursor.SkipValue()) {
                return std::nullopt;
              }
              cursor.SkipWs();
              if (cursor.Consume('}')) break;
              if (!cursor.Consume(',')) return std::nullopt;
            }
          }
          if (hit.have_cid && hit.have_mid) parsed.hits.push_back(std::move(hit));
          cursor.SkipWs();
          if (cursor.Consume(']')) break;
          if (!cursor.Consume(',')) return std::nullopt;
        }
      }
    } else if (!cursor.SkipValue()) {
      return std::nullopt;
    }
    cursor.SkipWs();
    if (cursor.Consume('}')) return parsed;
    if (!cursor.Consume(',')) return std::nullopt;
  }
}

SearchReplyMessage NoMatch() {
  SearchReplyMessage message;
  message.text = kNoOneFind;
  message.link_preview_enabled = true;
  return message;
}

}  // namespace

std::string SearchResultLink(std::int64_t bare_cid, std::int64_t mid,
                             const std::optional<std::string>& chat_url) {
  if (chat_url.has_value() && StartsWithHttp(*chat_url)) {
    return *chat_url + std::to_string(mid);
  }
  return "https://t.me/c/" + std::to_string(bare_cid) + "/" +
         std::to_string(mid);
}

SearchReplyMessage FormatSearchReply(
    const std::string& result,
    const std::function<std::optional<std::string>(const std::string&)>&
        decrypt,
    const std::function<std::optional<std::string>(std::int64_t)>& chat_url) {
  const auto parsed = ParseResult(result);
  if (!parsed.has_value() || !parsed->data_present || parsed->hits.empty()) {
    return NoMatch();
  }
  std::string body;
  for (std::size_t n = 0; n < parsed->hits.size(); ++n) {
    const Hit& hit = parsed->hits[n];
    std::string memo;
    if (!hit.memo_present) {
      memo = kEmptyMemo;
    } else if (decrypt) {
      auto plain = decrypt(hit.memo_token);
      if (!plain.has_value()) {
        // Python aborts the whole reply. We keep the other hits and
        // substitute the same placeholder an empty caption uses.
        spdlog::warn("search: memo decrypt failed for cid {} mid {} — "
                     "using placeholder",
                     hit.cid, hit.mid);
        memo = kEmptyMemo;
      } else {
        memo = FirstCodePoints(*plain, kMemoCodePoints);
        if (memo.empty()) memo = kEmptyMemo;
      }
    } else {
      memo = kEmptyMemo;
    }
    if (!body.empty()) body.push_back('\n');
    // MarkdownV2 bold is *text*, not **text** (that pair is an empty bold).
    body += "*";
    body += std::to_string(n + 1);
    body += "*\\. [";
    body += telegram::EscapeMarkdownV2(memo);
    body += "](";
    body += EscapeLinkTarget(SearchResultLink(hit.cid, hit.mid, chat_url
                                                                    ? chat_url(hit.cid)
                                                                    : std::nullopt));
    body += ")";
  }
  body.push_back('\n');
  SearchReplyMessage message;
  message.text = std::move(body);
  message.link_preview_enabled = false;
  return message;
}

void DeliverSearchReply(
    telegram::MessageSender& messenger,
    const media_thumbnail::SearchCorrelationTable::Entry& target,
    const std::string& result,
    const std::function<std::optional<std::string>(const std::string&)>&
        decrypt,
    const std::function<std::optional<std::string>(std::int64_t)>& chat_url) {
  const SearchReplyMessage message =
      FormatSearchReply(result, decrypt, chat_url);
  // target.chat_id is the TDLib dialog id stored at send time. Never the
  // uint32 oid, and never the result's cid (that cid is the *matched*
  // chat, used only inside the link).
  messenger.SendMarkdownReply(target.chat_id, target.message_id, message.text,
                              message.link_preview_enabled);
}

}  // namespace tg::features::auto_search
