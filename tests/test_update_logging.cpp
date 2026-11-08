// telegram.log_updates: when enabled, every incoming TDLib update is logged
// with its type and full content; when disabled, no "telegram: update" line
// appears. Uses a real TelegramClient (dummy config, no network, no real
// credentials) and captures spdlog output through a string sink.
#include <app/Config.h>
#include <telegram/AuthManager.h>
#include <telegram/TelegramClient.h>

#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <td/telegram/Log.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

// Captures everything the program logs via spdlog while alive; restores the
// previous default logger on destruction.
class LogCapture {
 public:
  LogCapture() {
    previous_ = spdlog::default_logger();
    sink_ = std::make_shared<spdlog::sinks::ostream_sink_mt>(stream_);
    logger_ = std::make_shared<spdlog::logger>("capture", sink_);
    logger_->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger_);
  }
  ~LogCapture() { spdlog::set_default_logger(previous_); }

  std::string text() const { return stream_.str(); }

 private:
  std::shared_ptr<spdlog::logger> previous_;
  std::shared_ptr<spdlog::sinks::ostream_sink_mt> sink_;
  std::shared_ptr<spdlog::logger> logger_;
  std::ostringstream stream_;
};

std::string RunClientAndCapture(bool log_updates) {
  td::Log::set_verbosity_level(2);  // keep TDLib's own stderr chatter down
  LogCapture capture;
  const std::string base =
      "/tmp/tg_update_log_test_" + std::to_string(getpid());
  std::filesystem::create_directories(base + "/db");
  std::filesystem::create_directories(base + "/files");

  tg::telegram::TelegramClient client;
  tg::app::TelegramConfig config;
  config.database_directory = base + "/db";
  config.files_directory = base + "/files";
  config.log_updates = log_updates;
  client.Start(config);

  // Wait until at least one update has flowed through HandleUpdate (the
  // first authorization state), then stop.
  for (int i = 0; i < 100; ++i) {
    if (client.auth().StateDescription() != "unknown") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  client.Stop();
  std::filesystem::remove_all(base);
  return capture.text();
}

}  // namespace

TEST(UpdateLogging, EnabledLogsUpdateTypeAndContent) {
  const std::string log = RunClientAndCapture(true);
  EXPECT_NE(log.find("telegram: update"), std::string::npos)
      << "no update line logged with log_updates=true:\n"
      << log;
  EXPECT_NE(log.find("updateAuthorizationState"), std::string::npos)
      << "logged update is missing its type:\n"
      << log;
  EXPECT_NE(log.find("waitTdlibParameters"), std::string::npos)
      << "logged update is missing its content:\n"
      << log;
}

TEST(UpdateLogging, DisabledLogsNoUpdates) {
  const std::string log = RunClientAndCapture(false);
  EXPECT_EQ(log.find("telegram: update"), std::string::npos)
      << "update line logged with log_updates=false:\n"
      << log;
}
