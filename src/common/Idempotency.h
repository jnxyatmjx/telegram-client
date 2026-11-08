#ifndef TG_COMMON_IDEMPOTENCY_H_
#define TG_COMMON_IDEMPOTENCY_H_

// ID conversion + idempotency key helpers (architecture v5 §1.2, §1.3).
//
// The server (Telethon/MTProto) speaks *bare* IDs; TDLib speaks Bot API
// dialog IDs. Conversions here are the single place where that happens, and
// every conversion range-checks: out of uint32 range -> nullopt, log an
// error at the call site and DO NOT send. Never silently truncate to the low
// 32 bits (different chats would collide on the same oid).

#include <cstdint>
#include <optional>
#include <string>

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

// TDLib message_id is int53 but really is the per-chat MTProto sequence
// number, a positive 32-bit value. Out of range -> nullopt.
std::optional<std::uint32_t> ToBareMessageId(std::int64_t message_id);

// image_id = "<chat_id>:<message_id>:<file_unique_id>" — the idempotency +
// correlation key for ProcessImages (server dedupes on it). chat_id here is
// the TDLib dialog id in string form (no conversion; image_id is a string
// and not bound by the uint32 limit). file_unique_id is the TDLib
// remoteFile.unique_id (the Bot API file_unique_id analogue); callers pass
// whatever identity their media provides.
std::string MakeImageId(std::int64_t chat_id, std::int64_t message_id,
                        const std::string& file_unique_id);

}  // namespace tg::common

#endif  // TG_COMMON_IDEMPOTENCY_H_
