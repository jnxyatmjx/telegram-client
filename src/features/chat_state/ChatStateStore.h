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
// Persistence (Telethon migration Chunk 2, Python schedule2db 1209-1241 /
// load_chat_state 790-800): every 30 s a flush pass writes the entries whose
// watermark advanced within the last 45 s to the backend (local SQLite with
// the Python `chathis` schema; D1 comes later behind the same interface).
// On startup Restore() loads the table back. A restored chat_url — including
// a placeholder — counts as resolved: the no-retry-storm lesson holds across
// restarts. The flush timer thread lives in Application (like the Outbox
// retry thread); the store only provides FlushActive().
//
// Threading: ApplyReply runs on the gRPC CQ thread, resolution callbacks on
// the TDLib Receive thread, FlushActive/Restore on the Application flush
// thread; all state is behind one mutex. The backend call in FlushActive
// happens OUTSIDE the mutex — SQLite I/O must not stall ApplyReply.

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <features/chat_state/ChatStateBackend.h>
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
  // `backend` may be null: the store then runs memory-only (Chunk 1
  // behaviour; chat_state.enabled=false in config).
  explicit ChatStateStore(telegram::ITdSender& sender,
                          std::shared_ptr<ChatStateBackend> backend = nullptr);

  ChatStateStore(const ChatStateStore&) = delete;
  ChatStateStore& operator=(const ChatStateStore&) = delete;

  // ImageReply.result arrived as {"cid","mid"} (already parsed). Advances the
  // watermark forward-only, creates the entry when new, and kicks off the
  // on-demand chat_url resolution when it is missing. Invalid ids are logged
  // and dropped.
  void ApplyReply(std::int64_t bare_cid, std::int64_t mid);

  std::optional<ChatEntry> Get(std::int64_t bare_cid) const;
  std::size_t size() const;

  // Startup restore (Python load_chat_state): loads every persisted row into
  // memory, parsing init_date/init_time back into last_seen. Must run AFTER
  // authorization (before the flush thread starts). Rows whose chat_url is
  // non-empty — placeholder or real — come back as resolved=true and are
  // never re-resolved; empty URLs resolve on demand at the first reply.
  // Returns the number of entries restored. Throws std::runtime_error on a
  // storage failure (startup-critical, same as opening the Outbox).
  std::size_t Restore();

  // One flush pass (Python schedule2db body): rows for every entry whose
  // watermark advanced within active_window_seconds of now, with the GMT+8
  // init_date/init_time (from the watermark advance) and updated_at (from
  // `now_ms`). No-op without a backend. Returns the number of rows written;
  // a backend failure is logged and returns 0 (the in-memory state stays
  // authoritative; the next pass retries).
  std::size_t FlushActive(std::int64_t now_ms, int active_window_seconds);

  bool persistent() const { return backend_ != nullptr; }

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
  // Null = memory-only (chat_state.enabled=false).
  std::shared_ptr<ChatStateBackend> backend_;
  mutable std::mutex mutex_;
  std::unordered_map<std::int64_t, State> chats_;
};

}  // namespace tg::features::chat_state

#endif  // TG_FEATURES_CHAT_STATE_CHATSTATESTORE_H_
