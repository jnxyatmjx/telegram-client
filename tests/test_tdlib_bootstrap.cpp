// Regression test: a real TDLib client must deliver the initial
// updateAuthorizationState. TDLib only creates the Td actor on the first
// send(), so a client that never sends a request receives no update at all
// and authorization waits forever with state "unknown" (live bug found
// 2026-11-08: a real login timed out after 300 s without any auth state).
// The first update (waitTdlibParameters) is sent before any parameters are
// validated, so this needs no network and no real credentials.
#include <app/Config.h>
#include <telegram/AuthManager.h>
#include <telegram/TelegramClient.h>

#include <td/telegram/Log.h>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>

TEST(TdlibBootstrap, FirstAuthorizationStateArrives) {
  // Keep TDLib's own logging quiet; its default verbosity would flood the
  // test output with per-second receive-loop chatter.
  td::Log::set_verbosity_level(2);

  const std::string base =
      "/tmp/tg_bootstrap_test_" + std::to_string(getpid());
  std::filesystem::create_directories(base + "/db");
  std::filesystem::create_directories(base + "/files");

  tg::telegram::TelegramClient client;
  tg::app::TelegramConfig config;
  config.database_directory = base + "/db";
  config.files_directory = base + "/files";
  // api_id/api_hash/phone deliberately left dummy.
  client.Start(config);

  std::string state;
  for (int i = 0; i < 100; ++i) {
    state = client.auth().StateDescription();
    if (state != "unknown") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  EXPECT_NE(state, "unknown")
      << "no updateAuthorizationState arrived within 10 s";
  client.Stop();

  std::filesystem::remove_all(base);
}
