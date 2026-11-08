#include "common/Idempotency.h"

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

}  // namespace tg::common
