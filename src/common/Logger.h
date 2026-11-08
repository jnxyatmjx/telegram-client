#ifndef TG_COMMON_LOGGER_H_
#define TG_COMMON_LOGGER_H_

// Logging: everything goes through spdlog's default logger. Call
// common::Logger::Init(level) once from main() before anything else logs.
//
// Privacy rule (architecture v5 §6): log identifiers only — chat_id,
// message_id, image_id, file paths. Never log media bytes or tokens.
//
// header-only on purpose: no .cpp in this directory except Idempotency.

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <string>

namespace tg::common {

class Logger {
 public:
  Logger() = delete;
  Logger(const Logger&) = delete;
  Logger& operator=(const Logger&) = delete;

  // `level`: trace|debug|info|warning|error|critical|off (bad values -> info).
  static void Init(const std::string& level) {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("tg", sink);
    // [tid] makes the thread table (v5 §3.2) visible in the logs.
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%t] %-8l %v");
    spdlog::set_default_logger(logger);

    auto lvl = spdlog::level::from_str(level);
    if (lvl == spdlog::level::off && level != "off") {
      lvl = spdlog::level::info;  // from_str returns off for unknown names
    }
    spdlog::set_level(lvl);
    spdlog::flush_on(spdlog::level::warn);
  }
};

}  // namespace tg::common

#endif  // TG_COMMON_LOGGER_H_
