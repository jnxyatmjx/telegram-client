#ifndef TG_TELEGRAM_TDERRORS_H_
#define TG_TELEGRAM_TDERRORS_H_

// TDLib error classification for the rate-limited calls added in Telethon
// migration Chunk 2 (dialog scans, messages.search, full-info probes).
//
// Telethon raises FloodWaitError with a structured .seconds; TDLib signals
// the same condition as error code 429 with the retry-after embedded in the
// message text. The exact wire format (td/telegram/Global.cpp:178):
//   "Too Many Requests: retry after <seconds>"
// parsed by RetryAfterSeconds below — no structured field exists in this
// TDLib version.

#include <td/telegram/td_api.h>

#include <cstdint>
#include <optional>

namespace tg::telegram {
namespace td_api = td::td_api;

// True for TDLib error 429 (FloodWaitError analogue).
bool IsFloodWait(const td_api::error& error);

// Retry-after seconds from a 429 message ("Too Many Requests: retry after
// 42" -> 42). nullopt when the message does not carry the number (the caller
// falls back to a default backoff).
std::optional<std::int64_t> RetryAfterSeconds(const td_api::error& error);

// Retry-after or `fallback_seconds` when the message carries no number.
std::int64_t RetryAfterOr(const td_api::error& error,
                          std::int64_t fallback_seconds);

}  // namespace tg::telegram

#endif  // TG_TELEGRAM_TDERRORS_H_
