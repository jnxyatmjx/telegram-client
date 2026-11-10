#include "telegram/TdErrors.h"

#include <charconv>

namespace tg::telegram {
namespace {

constexpr const char* kRetryAfterPrefix = "Too Many Requests: retry after ";

}  // namespace

bool IsFloodWait(const td_api::error& error) { return error.code_ == 429; }

std::optional<std::int64_t> RetryAfterSeconds(const td_api::error& error) {
  const std::string& message = error.message_;
  const size_t prefix_len = std::char_traits<char>::length(kRetryAfterPrefix);
  if (message.size() <= prefix_len ||
      message.compare(0, prefix_len, kRetryAfterPrefix) != 0) {
    return std::nullopt;
  }
  std::int64_t seconds = 0;
  const auto* first = message.data() + prefix_len;
  const auto* last = message.data() + message.size();
  const auto [ptr, ec] = std::from_chars(first, last, seconds, 10);
  if (ec != std::errc() || ptr != last || seconds < 0) return std::nullopt;
  return seconds;
}

std::int64_t RetryAfterOr(const td_api::error& error,
                          std::int64_t fallback_seconds) {
  if (const auto seconds = RetryAfterSeconds(error)) return *seconds;
  return fallback_seconds;
}

}  // namespace tg::telegram
