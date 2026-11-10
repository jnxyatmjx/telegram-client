#include "common/Idempotency.h"

#include <limits>

namespace tg::common {

std::optional<BareChatId> ToBareChatId(std::int64_t chat_id) {
  if (chat_id > 0) {
    const auto value = static_cast<std::uint64_t>(chat_id);
    if (value > kUint32Max) {
      return std::nullopt;
    }
    return BareChatId{static_cast<std::uint32_t>(value), PeerType::kUser};
  }
  if (chat_id == 0) {
    return std::nullopt;  // 0 is not a Telegram dialog id
  }

  // Negative: compute the magnitude in unsigned arithmetic so that
  // chat_id == INT64_MIN cannot overflow.
  const std::uint64_t magnitude =
      static_cast<std::uint64_t>(~static_cast<std::uint64_t>(chat_id)) + 1ULL;

  if (magnitude < static_cast<std::uint64_t>(kSupergroupMark)) {
    // basic group
    if (magnitude > kUint32Max) {
      return std::nullopt;
    }
    return BareChatId{static_cast<std::uint32_t>(magnitude),
                      PeerType::kBasicGroup};
  }

  // supergroup / channel: bare = -chat_id - 1'000'000'000'000
  const std::uint64_t bare = magnitude - static_cast<std::uint64_t>(kSupergroupMark);
  if (bare == 0ULL || bare > kUint32Max) {
    return std::nullopt;
  }
  return BareChatId{static_cast<std::uint32_t>(bare), PeerType::kSupergroup};
}

std::optional<std::int64_t> ToBareChatId64(std::int64_t chat_id) {
  if (chat_id > 0) {
    return chat_id;  // user: bare id is the user id itself (int64, no cap)
  }
  if (chat_id == 0) {
    return std::nullopt;  // 0 is not a Telegram dialog id
  }

  // Negative: compute the magnitude in unsigned arithmetic so that
  // chat_id == INT64_MIN cannot overflow.
  const std::uint64_t magnitude =
      static_cast<std::uint64_t>(~static_cast<std::uint64_t>(chat_id)) + 1ULL;

  if (magnitude < static_cast<std::uint64_t>(kSupergroupMark)) {
    // basic group: bare = -chat_id (fits int64: magnitude < 1e12)
    return static_cast<std::int64_t>(magnitude);
  }

  // supergroup / channel: bare = -chat_id - 1'000'000'000'000
  const std::uint64_t bare = magnitude - static_cast<std::uint64_t>(kSupergroupMark);
  if (bare == 0ULL) {
    return std::nullopt;  // bare 0 is not a real chat
  }
  // The largest possible bare value is for chat_id == INT64_MIN:
  // 2^63 - 1e12 = 9223371036854775808 < INT64_MAX, so the cast is safe;
  // the check below is defensive only.
  if (bare > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::int64_t>(bare);
}

std::optional<std::uint32_t> ToBareMessageId(std::int64_t message_id) {
  if (message_id <= 0) {
    return std::nullopt;  // MTProto message ids are positive
  }
  if (static_cast<std::uint64_t>(message_id) > kUint32Max) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(message_id);
}

std::string MakeImageId(std::int64_t chat_id, std::int64_t message_id,
                        const std::string& file_unique_id) {
  std::string id = std::to_string(chat_id);
  id.push_back(':');
  id += std::to_string(message_id);
  id.push_back(':');
  id += file_unique_id;
  return id;
}

std::vector<std::int64_t> DialogIdCandidates(std::int64_t bare_chat_id) {
  // Guard the range so the negations below cannot overflow; an out-of-range
  // "bare" id simply has no candidates and callers report chatNotFound-like
  // behaviour instead of UB. Bare ids are int64 (user ids above 2^32
  // exist); only non-positive ids have no candidates.
  if (bare_chat_id <= 0) {
    return {};
  }
  return {-bare_chat_id - kSupergroupMark, -bare_chat_id, bare_chat_id};
}

}  // namespace tg::common
