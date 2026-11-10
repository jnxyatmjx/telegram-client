#ifndef TG_COMMON_IDEMPOTENCY_H_
#define TG_COMMON_IDEMPOTENCY_H_

// ID conversion + idempotency key helpers (architecture v5 §1.2, §1.3).
//
// The server (Telethon/MTProto) speaks *bare* IDs; TDLib speaks Bot API
// dialog IDs. Conversions here are the single place where that happens.
//
// Two bare-id widths exist, and callers must pick the right one:
//   - SearchReq.oid / SearchReq.id are uint32 in the proto, so the search
//     path uses ToBareChatId / ToBareMessageId and range-checks to
//     0..UINT32_MAX (out of range -> nullopt, log an error at the call site
//     and DO NOT send; never silently truncate to the low 32 bits, or
//     different chats would collide on the same oid).
//   - ImageRequest.owner_id is int64 in the proto and Python sends it as an
//     arbitrary-precision int, so the ProcessImages path uses
//     ToBareChatId64 (no uint32 cap). Telegram has started allocating user
//     ids above 2^32 (e.g. a bot at 7974301646), and capping owner_id at
//     uint32 would refuse to send those chats entirely. The same applies to
//     the ImageReply {"cid"} the server echoes back (ChatStateStore keys
//     bare ids as int64) and to DialogIdCandidates (the reverse direction).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tg::common {

enum class PeerType : std::int8_t {
  kUser = 0,          // Bot API dialog id > 0
  kBasicGroup = 1,    // -1'000'000'000'000 < chat_id < 0
  kSupergroup = 2,    // chat_id <= -1'000'000'000'000 (supergroup or channel)
};

struct BareChatId {
  std::uint32_t oid;
  PeerType type;
};

inline constexpr std::int64_t kSupergroupMark = 1'000'000'000'000LL;
inline constexpr std::uint64_t kUint32Max = 4'294'967'295ULL;

// TDLib Bot API dialog id -> MTProto bare id for SearchReq.oid.
//   chat_id > 0                     -> oid = chat_id          (user)
//   -1e12 < chat_id < 0             -> oid = -chat_id         (basic group)
//   chat_id <= -1e12                -> oid = -chat_id - 1e12  (supergroup/channel)
// 0 is not a dialog id; anything whose bare value falls outside
// 0..UINT32_MAX yields nullopt (loud fail upstream).
std::optional<BareChatId> ToBareChatId(std::int64_t chat_id);

// TDLib Bot API dialog id -> MTProto bare id as int64, for
// ImageRequest.owner_id (int64 in the proto) and anywhere else the bare id
// is not a proto uint32 field. Same unmarking as ToBareChatId, but no
// uint32 cap: Telegram user ids above 2^32 exist (e.g. 7974301646) and must
// still send. 0 is not a dialog id; a supergroup/channel whose bare value
// is 0 yields nullopt.
std::optional<std::int64_t> ToBareChatId64(std::int64_t chat_id);

// TDLib message_id is int53 but really is the per-chat MTProto sequence
// number, a positive 32-bit value. Out of range -> nullopt.
std::optional<std::uint32_t> ToBareMessageId(std::int64_t message_id);

// image_id = "<chat_id>:<message_id>:<file_unique_id>" — INTERNAL identity
// used by the SearchReq correlation table bookkeeping (search requests carry
// no image_id on the wire; this is the log/correlation form). file_unique_id
// is the TDLib remoteFile.unique_id (the Bot API file_unique_id analogue).
//
// NOTE (Telethon migration, Chunk 1): ImageRequest.image_id on the wire is
// NOT this anymore — see MakeWireImageId below.
std::string MakeImageId(std::int64_t chat_id, std::int64_t message_id,
                        const std::string& file_unique_id);

// ImageRequest.image_id on the wire: str(message_id) — the decimal message
// id string, exactly what the Python client sends (app_client.py:1388). The
// server dedupes on it at MESSAGE level: the same Telegram message always
// maps to the same key, so a retry/replay is deduped server-side, while two
// different messages never collide (MTProto message ids are per-chat, so two
// chats CAN carry the same number — Python accepted that and so do we; the
// server's key space is the message id string, not a per-chat tuple).
inline std::string MakeWireImageId(std::int64_t message_id) {
  return std::to_string(message_id);
}

// The reverse direction of ToBareChatId for bare ids arriving OUTSIDE
// TDLib (Telethon migration Chunk 2): /xm <id> arguments and the
// ImageReply {"cid"} are bare MTProto ids, but td_api getChat/getMessage
// need the marked dialog-id form. A bare id alone does not say which peer
// type it is (Telethon answered this from its session cache), so callers
// probe getChat over these candidates in order: supergroup/channel first
// (the media-chat case), then basic group, then user. Bare ids are int64
// (user ids above 2^32 exist); only non-positive ids have no candidates.
std::vector<std::int64_t> DialogIdCandidates(std::int64_t bare_chat_id);

}  // namespace tg::common

#endif  // TG_COMMON_IDEMPOTENCY_H_
