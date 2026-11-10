#ifndef TG_FEATURES_CHAT_STATE_CHATSTATESTORE_H_
#define TG_FEATURES_CHAT_STATE_CHATSTATESTORE_H_

// ChatStateStore — per-chat watermark + link state, the C++ analogue of
// app_client.py's `cgid2link` dict (Telethon migration Chunk 1; Hong
// decisions 2/4 of 2026-11-10).
//
// Flow (identical to Python insertIdUpdate -> updateCGIdict, 1243-1253 /
// 1164-1200): every ImageReply.result is the JSON {"cid": <bare chat id>,
// "mid": <message id>}. The store keeps, per bare chat id:
//   offset_id — the watermark; ONLY ever increases (mid > current).
//   chat_url  — "https://t.me/<username>/" style link, resolved on demand.
//   last_seen — when the watermark last advanced ("t_t" analogue; the 30 s
//               flush of active chats to the state database is Chunk 2).
//
// chat_url resolution (the Python lesson at app_client.py:1168-1171): resolve
// ON DEMAND via TDLib (getChat, then getSupergroup/getBasicGroup for
// usernames/upgrades — the searchPublicChat equivalents for id-keyed chats),
// never by scanning dialogs. A chat that cannot be resolved (private, left,
// deleted) gets the placeholder URL ">>>>Nrror><chat_id>" and is never
// retried — a non-empty URL keeps later replies out of the resolve path, so
// no retry storm. While a resolution is in flight, further replies for the
// same chat do NOT start a second one.
//
// In-memory only in Chunk 1: state dies with the process. Persistence (30 s
// flush, startup restore) is Chunk 2 — keep the interface ready for it.
//
// Threading: ApplyReply runs on the gRPC CQ thread, resolution callbacks on
// the TDLib Receive thread; all state is behind one mutex. Callbacks only
// touch memory — no blocking anywhere.

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <telegram/TelegramClient.h>

namespace tg::features::chat_state {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

// One parsed ImageReply.result. `cid` is a bare (MTProto) chat id, `mid` the
// message id the server processed.
struct ParsedReply {
  std::int64_t cid = 0;
  std::int64_t mid = 0;
};

// Parses {"cid": ..., "mid": ...}. Both may be JSON numbers or decimal
// strings; extra fields are ignored. nullopt on invalid JSON or missing /
// non-numeric fields (the caller logs and drops — never crashes).
std::optional<ParsedReply> ParseReplyResult(const std::string& json);

struct ChatEntry {
  std::int64_t offset_id = 0;     // watermark; only increases
  std::string chat_url;           // resolved link or ">>>>Nrror>..." placeholder
  std::int64_t last_seen_ms = 0;  // epoch ms of the last watermark advance
};

class ChatStateStore {
 public:
  explicit ChatStateStore(telegram::ITdSender& sender);

  ChatStateStore(const ChatStateStore&) = delete;
  ChatStateStore& operator=(const ChatStateStore&) = delete;

  // ImageReply.result arrived as {"cid","mid"} (already parsed). Advances the
  // watermark forward-only, creates the entry when new, and kicks off the
  // on-demand chat_url resolution when it is missing. Invalid ids are logged
  // and dropped.
  void ApplyReply(std::int64_t bare_cid, std::int64_t mid);

  std::optional<ChatEntry> Get(std::int64_t bare_cid) const;
  std::size_t size() const;

 private:
  struct State {
    ChatEntry entry;
    bool resolving = false;  // resolution in flight — do not start another
    bool resolved = false;   // done (url set, possibly a placeholder)
  };

  // Resolution chain: getChat over candidate dialog ids
  // (supergroup -> basic group -> user), then per type getSupergroup (for
  // usernames) or getBasicGroup (for the migration upgrade). Every step is
  // async; the callbacks write the result under the mutex and send at most
  // one follow-up request.
  void StartResolve(std::int64_t bare_cid);
  void SendGetChat(std::int64_t bare_cid, int candidate_index);
  void ResolveFromChat(std::int64_t bare_cid, td_api::object_ptr<td_api::chat> chat);
  void SendGetSupergroup(std::int64_t bare_cid, std::int64_t supergroup_id,
                         std::int64_t link_bare_id);
  void SendGetBasicGroup(std::int64_t bare_cid, std::int64_t basic_group_id,
                         std::string title);
  void FinishResolve(std::int64_t bare_cid, std::string url);

  void SetUrl(std::int64_t bare_cid, std::string url);
  void SetPlaceholder(std::int64_t bare_cid);

  telegram::ITdSender& sender_;
  mutable std::mutex mutex_;
  std::unordered_map<std::int64_t, State> chats_;
};

}  // namespace tg::features::chat_state

#endif  // TG_FEATURES_CHAT_STATE_CHATSTATESTORE_H_
