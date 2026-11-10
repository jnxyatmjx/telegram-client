#include "common/Gmt8.h"

#include <charconv>
#include <chrono>

namespace tg::common {
namespace {

// Howard Hinnant's civil_from_days (public domain): days since 1970-01-01
// -> (year, month, day). No timezone tables needed: GMT+8 is a fixed offset
// and Asia/Taipei has had no DST since 1979 (the Python client relies on the
// same fixed-offset assumption via TZ=Asia/Taipei).
void CivilFromDays(std::int64_t days, int* year, unsigned* month,
                   unsigned* day) {
  days += 719'468;
  const std::int64_t era = (days >= 0 ? days : days - 146'096) / 146'097;
  const unsigned doe = static_cast<unsigned>(days - era * 146'097);  // [0, 146096]
  const unsigned yoe =
      (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;         // [0, 399]
  const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);      // [0, 365]
  const unsigned mp = (5 * doy + 2) / 153;                           // [0, 11]
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;                   // [1, 31]
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;                      // [1, 12]
  *year = static_cast<int>(y + (m <= 2 ? 1 : 0));
  *month = m;
  *day = d;
}

std::string Pad2(unsigned value) {
  std::string out = std::to_string(value);
  if (out.size() < 2) out.insert(out.begin(), '0');
  return out;
}

std::string Pad4(int value) {
  std::string out = std::to_string(value);
  while (out.size() < 4) out.insert(out.begin(), '0');
  return out;
}

}  // namespace

std::string Gmt8Date(std::int64_t epoch_ms) {
  const std::int64_t seconds =
      epoch_ms >= 0 ? epoch_ms / 1000 + 8 * 3600
                    : (epoch_ms - 999) / 1000 + 8 * 3600;  // floor for negatives
  const std::int64_t days = seconds >= 0 ? seconds / 86400
                                         : (seconds - 86399) / 86400;
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  CivilFromDays(days, &year, &month, &day);
  return Pad4(year) + Pad2(month) + Pad2(day);
}

std::string Gmt8Clock(std::int64_t epoch_ms) {
  const std::int64_t seconds =
      epoch_ms >= 0 ? epoch_ms / 1000 + 8 * 3600
                    : (epoch_ms - 999) / 1000 + 8 * 3600;
  const std::int64_t secs_of_day =
      seconds >= 0 ? seconds % 86400
                   : ((seconds % 86400) + 86400) % 86400;
  return Pad2(static_cast<unsigned>(secs_of_day / 3600)) + ":" +
         Pad2(static_cast<unsigned>((secs_of_day / 60) % 60)) + ":" +
         Pad2(static_cast<unsigned>(secs_of_day % 60));
}

std::string Gmt8DateTime(std::int64_t epoch_ms) {
  const std::int64_t seconds =
      epoch_ms >= 0 ? epoch_ms / 1000 + 8 * 3600
                    : (epoch_ms - 999) / 1000 + 8 * 3600;
  const std::int64_t days = seconds >= 0 ? seconds / 86400
                                         : (seconds - 86399) / 86400;
  const std::int64_t secs_of_day =
      seconds >= 0 ? seconds % 86400
                   : ((seconds % 86400) + 86400) % 86400;
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  CivilFromDays(days, &year, &month, &day);
  return Pad4(year) + "-" + Pad2(month) + "-" + Pad2(day) + " " +
         Pad2(static_cast<unsigned>(secs_of_day / 3600)) + ":" +
         Pad2(static_cast<unsigned>((secs_of_day / 60) % 60)) + ":" +
         Pad2(static_cast<unsigned>(secs_of_day % 60));
}

std::optional<std::int64_t> ParseGmt8(std::int64_t init_date,
                                      const std::string& init_time) {
  // init_date: exactly 8 digits once zero-padded to the width Python writes
  // (strftime('%Y%m%d')). Values like 20261110 parse directly; wider/narrower
  // or non-digit values are rejected.
  std::string date_text = std::to_string(init_date);
  if (date_text.size() != 8) return std::nullopt;
  int year = 0;
  unsigned month = 0;
  unsigned day = 0;
  const auto read2 = [&date_text](size_t at, unsigned* out) {
    for (int k = 0; k < 2; ++k) {
      const char c = date_text[at + static_cast<size_t>(k)];
      if (c < '0' || c > '9') return false;
      *out = *out * 10 + static_cast<unsigned>(c - '0');
    }
    return true;
  };
  for (int k = 0; k < 4; ++k) {
    const char c = date_text[static_cast<size_t>(k)];
    if (c < '0' || c > '9') return std::nullopt;
    year = year * 10 + (c - '0');
  }
  if (!read2(4, &month) || !read2(6, &day)) return std::nullopt;
  if (month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;

  // init_time: "HH:MM:SS", fractional tail allowed and dropped
  // (str(time).split(".", 1)[0] in Python).
  std::string clock = init_time;
  const size_t dot = clock.find('.');
  if (dot != std::string::npos) clock = clock.substr(0, dot);
  if (clock.size() != 8 || clock[2] != ':' || clock[5] != ':') {
    return std::nullopt;
  }
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  const auto read_clock = [&](size_t at, unsigned* out) {
    for (int k = 0; k < 2; ++k) {
      const char c = clock[at + static_cast<size_t>(k)];
      if (c < '0' || c > '9') return false;
      *out = *out * 10 + static_cast<unsigned>(c - '0');
    }
    return true;
  };
  if (!read_clock(0, &hour) || !read_clock(3, &minute) ||
      !read_clock(6, &second)) {
    return std::nullopt;
  }
  if (hour > 23 || minute > 59 || second > 60) return std::nullopt;  // 60: leap

  // Days from civil (Hinnant) -> epoch seconds at GMT+8.
  const std::int64_t yy = static_cast<std::int64_t>(year) -
                          (month <= 2 ? 1 : 0);
  const std::int64_t era = (yy >= 0 ? yy : yy - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(yy - era * 400);
  const unsigned doy = (153 * (month <= 2 ? month + 9 : month - 3) + 2) / 5 +
                       day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const std::int64_t days = era * 146'097 + static_cast<std::int64_t>(doe) -
                            719'468;
  const std::int64_t seconds =
      days * 86400 + hour * 3600 + minute * 60 + second - 8 * 3600;
  return seconds * 1000;
}

}  // namespace tg::common
