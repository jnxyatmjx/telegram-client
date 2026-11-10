#include "features/chat_state/ChatStateStore.h"

#include <common/Idempotency.h>
#include <common/Logger.h>

#include <charconv>
#include <chrono>
#include <utility>
#include <vector>

namespace tg::features::chat_state {
namespace {

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Python build_chat_info (app_client.py:771-788) URL shapes.
std::string PublicLinkUrl(const std::string& username) {
  std::string url = "https://t.me/";
  url += username;
  url += "/";
  return url;
}

std::string PrivateChannelUrl(std::int64_t link_bare_id) {
  return "https://t.me/c/" + std::to_string(link_bare_id) + "/";
}

std::string BasicGroupUrl(const std::string& title) {
  return ">>Chat>" + title;
}

std::string UserUrl(const std::string& title) {
  return ">>>Nrror>" + title;
}

std::string PlaceholderUrl(std::int64_t bare_cid) {
  return ">>>>Nrror>" + std::to_string(bare_cid);
}

// supergroup.usernames_ -> first active username (Python get_chat_public_link
// walks username then usernames[] the same way).
std::string SupergroupUrl(const td_api::supergroup& supergroup,
                          std::int64_t link_bare_id) {
  if (supergroup.usernames_ != nullptr &&
      !supergroup.usernames_->active_usernames_.empty()) {
    return PublicLinkUrl(supergroup.usernames_->active_usernames_.front());
  }
  return PrivateChannelUrl(link_bare_id);
}

bool IsError(const td::ClientManager::Response& response) {
  return response.object == nullptr ||
         response.object->get_id() == td_api::error::ID;
}

std::string ErrorText(const td::ClientManager::Response& response) {
  if (response.object == nullptr) return "<no object>";
  if (response.object->get_id() == td_api::error::ID) {
    const auto& error = static_cast<const td_api::error&>(*response.object);
    return std::to_string(error.code_) + ": " + error.message_;
  }
  return "<not an error>";
}

std::optional<std::int64_t> ToInt64(const std::string& text) {
  std::int64_t value = 0;
  const auto* first = text.data();
  const auto* last = first + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value, 10);
  if (ec != std::errc() || ptr != last) return std::nullopt;
  return value;
}

// --- minimal JSON flat-object scanner ---------------------------------------
//
// Only what ImageReply.result needs: {"key": value, ...} with value = string
// | number | true | false | null. Nested objects/arrays are skipped balanced.
// No dependencies (spdlog/fmt is not a JSON library; the server contract is
// exactly this flat shape).

void SkipWs(const std::string& s, size_t& i) {
  while (i < s.size() &&
         (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
    ++i;
  }
}

bool ParseString(const std::string& s, size_t& i, std::string* out) {
  if (i >= s.size() || s[i] != '"') return false;
  ++i;
  out->clear();
  while (i < s.size()) {
    const char c = s[i++];
    if (c == '"') return true;
    if (c != '\\') {
      out->push_back(c);
      continue;
    }
    if (i >= s.size()) return false;
    const char esc = s[i++];
    switch (esc) {
      case '"': out->push_back('"'); break;
      case '\\': out->push_back('\\'); break;
      case '/': out->push_back('/'); break;
      case 'b': out->push_back('\b'); break;
      case 'f': out->push_back('\f'); break;
      case 'n': out->push_back('\n'); break;
      case 'r': out->push_back('\r'); break;
      case 't': out->push_back('\t'); break;
      case 'u': {
        if (i + 4 > s.size()) return false;
        unsigned cp = 0;
        for (int k = 0; k < 4; ++k) {
          const char h = s[i++];
          cp <<= 4;
          if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
          else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
          else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
          else return false;
        }
        // Encode as UTF-8 (BMP only; surrogates land in the replacement
        // range — irrelevant for cid/mid keys we care about).
        if (cp < 0x80) {
          out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
          out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
          out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
          out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
          out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        break;
      }
      default:
        return false;
    }
  }
  return false;  // unterminated
}

bool ParseNumber(const std::string& s, size_t& i, std::string* out) {
  const size_t start = i;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
  bool any_digit = false;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    ++i;
    any_digit = true;
  }
  if (i < s.size() && s[i] == '.') {
    ++i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ++i;
      any_digit = true;
    }
  }
  if (!any_digit) return false;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    const size_t save = i;
    ++i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    bool exp_digit = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      ++i;
      exp_digit = true;
    }
    if (!exp_digit) i = save;  // not an exponent after all
  }
  *out = s.substr(start, i - start);
  return true;
}

// Skips a nested object/array (brackets balanced, strings honoured).
bool SkipBalanced(const std::string& s, size_t& i, char open, char close) {
  int depth = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '"') {
      std::string ignored;
      if (!ParseString(s, i, &ignored)) return false;
      continue;
    }
    if (c == open) ++depth;
    if (c == close) {
      --depth;
      if (depth == 0) {
        ++i;
        return true;
      }
    }
    ++i;
  }
  return false;
}

bool SkipValue(const std::string& s, size_t& i, std::string* scalar_out) {
  SkipWs(s, i);
  if (i >= s.size()) return false;
  const char c = s[i];
  if (c == '"') return ParseString(s, i, scalar_out);
  if (c == '{') return SkipBalanced(s, i, '{', '}');
  if (c == '[') return SkipBalanced(s, i, '[', ']');
  if (s.compare(i, 4, "true") == 0) { i += 4; *scalar_out = "true"; return true; }
  if (s.compare(i, 5, "false") == 0) { i += 5; *scalar_out = "false"; return true; }
  if (s.compare(i, 4, "null") == 0) { i += 4; *scalar_out = "null"; return true; }
  return ParseNumber(s, i, scalar_out);
}

}  // namespace

// --- ParseReplyResult -------------------------------------------------------

std::optional<ParsedReply> ParseReplyResult(const std::string& json) {
  size_t i = 0;
  SkipWs(json, i);
  if (i >= json.size() || json[i] != '{') return std::nullopt;
  ++i;
  std::vector<std::pair<std::string, std::string>> fields;
  SkipWs(json, i);
  if (i < json.size() && json[i] == '}') {
    return std::nullopt;  // empty object: no cid/mid
  }
  while (true) {
    SkipWs(json, i);
    std::string key;
    if (!ParseString(json, i, &key)) return std::nullopt;
    SkipWs(json, i);
    if (i >= json.size() || json[i] != ':') return std::nullopt;
    ++i;
    std::string value;
    if (!SkipValue(json, i, &value)) return std::nullopt;
    fields.emplace_back(std::move(key), std::move(value));
    SkipWs(json, i);
    if (i >= json.size()) return std::nullopt;
    if (json[i] == ',') {
      ++i;
      continue;
    }
    if (json[i] == '}') break;
    return std::nullopt;
  }
  ++i;
  SkipWs(json, i);
  if (i != json.size()) return std::nullopt;  // trailing garbage

  ParsedReply reply;
  bool have_cid = false;
  bool have_mid = false;
  for (const auto& [key, value] : fields) {
    if (key == "cid") {
      if (auto v = ToInt64(value)) {
        reply.cid = *v;
        have_cid = true;
      }
    } else if (key == "mid") {
      if (auto v = ToInt64(value)) {
        reply.mid = *v;
        have_mid = true;
      }
    }
  }
  if (!have_cid || !have_mid) return std::nullopt;
  return reply;
}

// --- ChatStateStore ---------------------------------------------------------

ChatStateStore::ChatStateStore(telegram::ITdSender& sender) : sender_(sender) {}

void ChatStateStore::ApplyReply(std::int64_t bare_cid, std::int64_t mid) {
  // Bare chat ids are uint32; anything else means the server contract broke.
  if (bare_cid <= 0 ||
      static_cast<std::uint64_t>(bare_cid) > tg::common::kUint32Max) {
    spdlog::error("chat_state: reply cid {} is not a valid bare chat id — dropped",
                  bare_cid);
    return;
  }
  if (mid <= 0) {
    spdlog::error("chat_state: reply mid {} is not a positive message id — dropped",
                  mid);
    return;
  }
  bool need_resolve = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    State& state = chats_[bare_cid];
    // Watermark only increases (app_client.py:1196-1198); last_seen follows
    // the same advance (Python refreshes t_t on the same branch).
    if (mid > state.entry.offset_id) {
      state.entry.offset_id = mid;
      state.entry.last_seen_ms = NowMs();
    }
    if (state.entry.chat_url.empty() && !state.resolving && !state.resolved) {
      state.resolving = true;
      need_resolve = true;
    }
  }
  if (need_resolve) StartResolve(bare_cid);
}

std::optional<ChatEntry> ChatStateStore::Get(std::int64_t bare_cid) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = chats_.find(bare_cid);
  if (it == chats_.end()) return std::nullopt;
  return it->second.entry;
}

std::size_t ChatStateStore::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return chats_.size();
}

// Candidate dialog ids for a bare id, tried in this order: supergroup/channel
// (the media-chat case), basic group, user. Telethon's get_entity(int) picks
// from its session cache; TDLib needs the dialog-id form per peer type.
std::int64_t CandidateDialogId(std::int64_t bare_cid, int index) {
  switch (index) {
    case 0:
      return -bare_cid - tg::common::kSupergroupMark;
    case 1:
      return -bare_cid;
    default:
      return bare_cid;
  }
}

void ChatStateStore::StartResolve(std::int64_t bare_cid) {
  spdlog::info("chat_state: resolving chat_url for bare chat id {}", bare_cid);
  SendGetChat(bare_cid, 0);
}

void ChatStateStore::SendGetChat(std::int64_t bare_cid, int candidate_index) {
  auto request = td_api::make_object<td_api::getChat>();
  request->chat_id_ = CandidateDialogId(bare_cid, candidate_index);
  sender_.Send(
      std::move(request),
      [this, bare_cid, candidate_index](td::ClientManager::Response&& response) {
        if (IsError(response)) {
          if (candidate_index + 1 < 3) {
            SendGetChat(bare_cid, candidate_index + 1);
          } else {
            spdlog::warn(
                "chat_state: getChat failed for bare chat id {} (all peer "
                "types tried, last error {}) — placeholder",
                bare_cid, ErrorText(response));
            SetPlaceholder(bare_cid);
          }
          return;
        }
        ResolveFromChat(
            bare_cid,
            td_api::move_object_as<td_api::chat>(std::move(response.object)));
      });
}

void ChatStateStore::ResolveFromChat(
    std::int64_t bare_cid, td_api::object_ptr<td_api::chat> chat) {
  if (chat == nullptr || chat->type_ == nullptr) {
    SetPlaceholder(bare_cid);
    return;
  }
  switch (chat->type_->get_id()) {
    case td_api::chatTypeSupergroup::ID: {
      const auto& type =
          static_cast<const td_api::chatTypeSupergroup&>(*chat->type_);
      // Supergroup ids ARE bare MTProto ids — the t.me/c link uses them.
      SendGetSupergroup(bare_cid, type.supergroup_id_, type.supergroup_id_);
      return;
    }
    case td_api::chatTypeBasicGroup::ID: {
      const auto& type =
          static_cast<const td_api::chatTypeBasicGroup&>(*chat->type_);
      // Mirrors Python resolve_active_chat_entity: a migrated basic group is
      // replaced by its (new) supergroup for URL purposes, still stored
      // under the old bare id key.
      SendGetBasicGroup(bare_cid, type.basic_group_id_, chat->title_);
      return;
    }
    default:
      // Private chat / secret chat — Python build_chat_info's User branch.
      SetUrl(bare_cid, UserUrl(chat->title_));
      return;
  }
}

void ChatStateStore::SendGetSupergroup(std::int64_t bare_cid,
                                       std::int64_t supergroup_id,
                                       std::int64_t link_bare_id) {
  auto request = td_api::make_object<td_api::getSupergroup>();
  request->supergroup_id_ = supergroup_id;
  sender_.Send(
      std::move(request),
      [this, bare_cid, link_bare_id](td::ClientManager::Response&& response) {
        if (IsError(response)) {
          // getChat succeeded, so the chat exists: fall back to the private
          // c/ link form (a channel without a public username in Python).
          spdlog::warn("chat_state: getSupergroup failed for bare chat id {}: "
                       "{} — using private link form",
                       bare_cid, ErrorText(response));
          SetUrl(bare_cid, PrivateChannelUrl(link_bare_id));
          return;
        }
        SetUrl(bare_cid, SupergroupUrl(
            static_cast<const td_api::supergroup&>(*response.object),
            link_bare_id));
      });
}

void ChatStateStore::SendGetBasicGroup(std::int64_t bare_cid,
                                       std::int64_t basic_group_id,
                                       std::string title) {
  auto request = td_api::make_object<td_api::getBasicGroup>();
  request->basic_group_id_ = basic_group_id;
  sender_.Send(
      std::move(request),
      [this, bare_cid, basic_group_id, title = std::move(title)](
          td::ClientManager::Response&& response) mutable {
        if (IsError(response)) {
          spdlog::warn("chat_state: getBasicGroup failed for bare chat id {}: "
                       "{} — placeholder",
                       bare_cid, ErrorText(response));
          SetPlaceholder(bare_cid);
          return;
        }
        const auto& group =
            static_cast<const td_api::basicGroup&>(*response.object);
        if (group.upgraded_to_supergroup_id_ != 0) {
          spdlog::info("chat_state: basic group {} upgraded to supergroup {} "
                       "(bare chat id {})", basic_group_id,
                       group.upgraded_to_supergroup_id_, bare_cid);
          SendGetSupergroup(bare_cid, group.upgraded_to_supergroup_id_,
                            group.upgraded_to_supergroup_id_);
        } else {
          SetUrl(bare_cid, BasicGroupUrl(title));
        }
      });
}

void ChatStateStore::SetUrl(std::int64_t bare_cid, std::string url) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    State& state = chats_[bare_cid];  // created by ApplyReply; keep offset
    state.entry.chat_url = url;
    state.resolving = false;
    state.resolved = true;
  }
  spdlog::info("chat_state: bare chat id {} url = {}", bare_cid, url);
}

void ChatStateStore::SetPlaceholder(std::int64_t bare_cid) {
  SetUrl(bare_cid, PlaceholderUrl(bare_cid));
}

}  // namespace tg::features::chat_state
