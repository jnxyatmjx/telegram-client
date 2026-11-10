#ifndef TG_COMMON_GMT8_H_
#define TG_COMMON_GMT8_H_

// Fixed-offset Asia/Taipei civil time (GMT+8, no DST) — the C++ analogue of
// Python's gmt8_now() + strftime calls around app_client.py:31-43 and
// schedule2db (1209-1241). The Python client stores chat state timestamps as
// three separate GMT+8 columns; the formats here must stay byte-compatible
// or the D1 rows written by the Python client would not parse back (SURVEY
// §8.5-4):
//   init_date  = t_t.strftime('%Y%m%d')             -> Gmt8Date()
//   init_time  = t_t.strftime('%H:%M:%S')           -> Gmt8Clock()
//   updated_at = now_gmt8.strftime('%Y-%m-%d %H:%M:%S') -> Gmt8DateTime()

#include <cstdint>
#include <optional>
#include <string>

namespace tg::common {

// epoch ms -> "YYYYMMDD" in GMT+8.
std::string Gmt8Date(std::int64_t epoch_ms);

// epoch ms -> "HH:MM:SS" in GMT+8.
std::string Gmt8Clock(std::int64_t epoch_ms);

// epoch ms -> "YYYY-MM-DD HH:MM:SS" in GMT+8.
std::string Gmt8DateTime(std::int64_t epoch_ms);

// Inverse of the pair above (Python load_chat_state, app_client.py:797-798:
// strptime(f"{int(init_date)}{time_text}", "%Y%m%d%H:%M:%S")). Accepts
// init_date as digits with or without zero padding ("20261110", "9990110")
// and init_time as "HH:MM:SS" with an optional fractional tail ("07:01:02.5"
// -> the ".5" is dropped, exactly like Python's str(time).split(".", 1)).
// nullopt on anything else.
std::optional<std::int64_t> ParseGmt8(std::int64_t init_date,
                                      const std::string& init_time);

}  // namespace tg::common

#endif  // TG_COMMON_GMT8_H_
