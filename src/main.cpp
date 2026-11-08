// tg-client entry point (v5 §3.1): ONE process, ONE binary, ONE TDLib client.
//
//   tg-client [--config <path>]
//
// Config path resolution: --config, else $TG_CLIENT_CONFIG, else
// ./config/config.yaml. Secrets (api_id/api_hash/phone/encryption key/gRPC
// token) come from the environment via ${VAR} entries in the config file —
// never hardcoded (IMPLEMENTATION_TASK.md).

#include <app/Application.h>
#include <app/Config.h>
#include <common/Logger.h>

#include <td/telegram/Log.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

using namespace tg;  // app::*, common::* below are tg::app, tg::common

namespace {

constexpr const char* kVersion = "tg-client 0.1.0";
constexpr const char* kHelp =
    "tg-client - Telegram client core (TDLib) with an async gRPC media transport\n"
    "\n"
    "Usage:\n"
    "  tg-client [options]\n"
    "\n"
    "Options:\n"
    "  --config <path>  Config file (default: $TG_CLIENT_CONFIG or ./config/config.yaml)\n"
    "  --help, -h       Show this help\n"
    "  --version        Print version\n"
    "\n"
    "User-account login inputs: TG_TELEGRAM_CODE, TG_TELEGRAM_PASSWORD.\n"
    "Secrets in the config file use ${VAR} environment expansion.\n";

extern "C" void HandleSignal(int) {
  // Async-signal-safe: relaxed atomic store only (see Application.h).
  app::RequestGlobalShutdown();
}

std::string DefaultConfigPath() {
  const char* env = std::getenv("TG_CLIENT_CONFIG");
  if (env != nullptr && env[0] != '\0') return env;
  return "config/config.yaml";
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string config_path = DefaultConfigPath();

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--version") {
      std::cout << kVersion << '\n';
      return 0;
    }
    if (arg == "--help" || arg == "-h") {
      std::cout << kHelp;
      return 0;
    }
    if (arg == "--config" && i + 1 < argc) {
      config_path = argv[++i];
      continue;
    }
    std::cerr << "tg-client: unknown argument: " << arg << " (try --help)\n";
    return 2;
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  // TDLib writes its own logs straight to stderr at its own default
  // verbosity, independent of our spdlog level — that is the per-second
  // "Begin/End to wait for updates" chatter. Warnings and errors only;
  // our own spdlog lines still carry the auth state and progress.
  td::Log::set_verbosity_level(2);

  app::Config config;
  try {
    config = app::Config::FromFile(config_path);
  } catch (const std::exception& e) {
    std::cerr << "tg-client: " << e.what() << '\n';
    return 1;
  }
  common::Logger::Init(config.log_level);

  try {
    app::Application application(std::move(config));
    return application.Run();
  } catch (const std::exception& e) {
    spdlog::critical("tg-client: fatal: {}", e.what());
    return 1;
  }
}
