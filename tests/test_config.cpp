// Config parsing: minimal YAML subset, env expansion, defaults.
#include <app/Config.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>

namespace app = tg::app;

static const char* kSample = R"(
# full example
telegram:
  api_id: 123456
  api_hash: "abc-hash"   # inline comment
  phone: '+8613800000000'
  database_directory: /data/tdlib/db
  files_directory: /data/tdlib/files
  encryption_key: ${TG_TEST_KEY}
  log_updates: true

grpc:
  target: "dns:///media.example.com:443"
  tls: false
  token: ${TG_TEST_TOKEN}
  ca_cert: /etc/ssl/tg-ca.crt
  ack_timeout_ms: 45000
  queue_capacity: 50
  connections: 3
  keepalive_ms: 5000
  keepalive_timeout_ms: 1500
  keepalive_permit_without_calls: false
  max_message_bytes: 8388608
  http2_lookahead_bytes: 262144
  http2_write_buffer_bytes: 65536

features:
  media_thumbnail:
    enabled: false
    max_thumb_bytes: 1048576
    keyme: "${TG_TEST_KEYME}"   # AES-GCM memo seed (secret, env-supplied)
  bot_command:
    enabled: true
    command_bot_user_id: 987654321
    process_command: "/process"
    search_command: "/search"
  auto_search:
    enabled: true
    chats:
      - -1001234567890
      - -987654321
    dedup_ttl_seconds: 300
  commands:
    enabled: true
    control_chat_id: 777
    self_commands: true
    rsme_command: "/rsme"
    rsor_command: "/rsor"
    backfill_commands:
      - "/xm"
      - "/xy"
      - "/ym"
      - "/who"
    dialog_limit: 1202
    list_batch_lines: 45
    rsor_report_limit: 50
  history_backfill:
    enabled: true
    page_limit: 60
    flood_pad_seconds: 1
  media_channels:
    enabled: true
    chats:
      - 1234567890
      - 987654321
    rpc: "search"

chat_state:
  enabled: true
  db_path: /data/chathis.sqlite
  flush_interval_seconds: 30
  active_window_seconds: 45

outbox:
  db_path: /tmp/outbox-test.sqlite

workers: 2
log:
  level: debug
)";

TEST(ParseYaml, NestedMapsScalarsListsComments) {
  const app::YamlNode root = app::ParseYaml(kSample);
  const app::YamlNode* telegram = root.Find("telegram");
  ASSERT_NE(telegram, nullptr);
  ASSERT_NE(telegram->Find("api_id"), nullptr);
  EXPECT_EQ(telegram->Find("api_id")->AsInt64(), std::optional<std::int64_t>(123456));
  EXPECT_EQ(telegram->Find("api_hash")->AsString(), std::optional<std::string>("abc-hash"));
  EXPECT_EQ(telegram->Find("phone")->AsString(),
            std::optional<std::string>("+8613800000000"));  // quotes stripped

  const app::YamlNode* features = root.Find("features");
  ASSERT_NE(features, nullptr);
  const app::YamlNode* auto_search = features->Find("auto_search");
  ASSERT_NE(auto_search, nullptr);
  const app::YamlNode* chats = auto_search->Find("chats");
  ASSERT_NE(chats, nullptr);
  EXPECT_EQ(chats->AsInt64List(),
            (std::vector<std::int64_t>{-1001234567890LL, -987654321LL}));
}

TEST(ParseYaml, RejectsGarbage) {
  EXPECT_THROW(app::ParseYaml("this line has no colon"), std::runtime_error);
  EXPECT_THROW(app::ParseYaml("a:\n  b: 1\n c: 2"), std::runtime_error);
}

TEST(ExpandEnv, ResolvesAndEmptiesMissingVars) {
  setenv("TG_CONFIG_TEST_VAR", "hello", 1);
  EXPECT_EQ(app::Config::ExpandEnv("x${TG_CONFIG_TEST_VAR}y"), "xhelloy");
  EXPECT_EQ(app::Config::ExpandEnv("${TG_CONFIG_TEST_VAR_UNSET_XYZ}"), "");
  unsetenv("TG_CONFIG_TEST_VAR");
}

class ConfigFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = "/tmp/tg_client_config_test_" + std::to_string(getpid()) + ".yaml";
    std::ofstream out(path_);
    out << kSample;
  }
  void TearDown() override { std::remove(path_.c_str()); }
  std::string path_;
};

TEST_F(ConfigFileTest, FromFileMapsEverything) {
  setenv("TG_TEST_KEY", "secret-key", 1);
  setenv("TG_TEST_TOKEN", "grpc-token", 1);
  setenv("TG_TEST_KEYME", "memo-seed-secret", 1);

  const app::Config config = app::Config::FromFile(path_);

  EXPECT_EQ(config.telegram.api_id, 123456);
  EXPECT_EQ(config.telegram.api_hash, "abc-hash");
  EXPECT_EQ(config.telegram.encryption_key, "secret-key");
  EXPECT_TRUE(config.telegram.log_updates);
  EXPECT_EQ(config.grpc.target, "dns:///media.example.com:443");
  EXPECT_FALSE(config.grpc.tls);
  EXPECT_EQ(config.grpc.token, "grpc-token");
  EXPECT_EQ(config.grpc.ca_cert_path, "/etc/ssl/tg-ca.crt");
  EXPECT_EQ(config.grpc.ack_timeout_ms, 45000);
  EXPECT_EQ(config.grpc.queue_capacity, 50);
  EXPECT_EQ(config.grpc.connections, 3);

  // Chunk 3: gRPC channel args (keepalive / 64 MiB cap / HTTP-2 window).
  EXPECT_EQ(config.grpc.keepalive_ms, 5000);
  EXPECT_EQ(config.grpc.keepalive_timeout_ms, 1500);
  EXPECT_FALSE(config.grpc.keepalive_permit_without_calls);
  EXPECT_EQ(config.grpc.max_message_bytes, 8388608);  // 8 MiB override
  EXPECT_EQ(config.grpc.http2_lookahead_bytes, 262144);
  EXPECT_EQ(config.grpc.http2_write_buffer_bytes, 65536);
  EXPECT_FALSE(config.media_thumbnail.enabled);
  EXPECT_EQ(config.media_thumbnail.keyme, "memo-seed-secret");
  EXPECT_EQ(config.bot_command.command_bot_user_id, 987654321);
  EXPECT_TRUE(config.auto_search.enabled);
  EXPECT_EQ(config.auto_search.chats.size(), 2u);

  // Chunk 2: command surface.
  EXPECT_TRUE(config.commands.enabled);
  EXPECT_EQ(config.commands.control_chat_id, 777);
  EXPECT_TRUE(config.commands.self_commands);
  EXPECT_EQ(config.commands.rsme_command, "/rsme");
  EXPECT_EQ(config.commands.rsor_command, "/rsor");
  EXPECT_EQ(config.commands.backfill_commands.size(), 4u);
  EXPECT_EQ(config.commands.backfill_commands[0], "/xm");
  EXPECT_EQ(config.commands.backfill_commands[3], "/who");
  EXPECT_EQ(config.commands.dialog_limit, 1202);
  EXPECT_EQ(config.commands.list_batch_lines, 45);
  EXPECT_EQ(config.commands.rsor_report_limit, 50);

  // Chunk 2: history backfill.
  EXPECT_TRUE(config.history_backfill.enabled);
  EXPECT_EQ(config.history_backfill.page_limit, 60);
  EXPECT_EQ(config.history_backfill.flood_pad_seconds, 1);

  // Chunk 2: whitelisted media channels (bare chat ids).
  EXPECT_TRUE(config.media_channels.enabled);
  EXPECT_EQ(config.media_channels.chats.size(), 2u);
  EXPECT_EQ(config.media_channels.chats[0], 1234567890);
  EXPECT_EQ(config.media_channels.rpc, "search");

  // Chunk 2: state persistence (Python chathis).
  EXPECT_TRUE(config.chat_state.enabled);
  EXPECT_EQ(config.chat_state.db_path, "/data/chathis.sqlite");
  EXPECT_EQ(config.chat_state.flush_interval_seconds, 30);
  EXPECT_EQ(config.chat_state.active_window_seconds, 45);

  EXPECT_EQ(config.outbox.db_path, "/tmp/outbox-test.sqlite");
  EXPECT_EQ(config.worker_count, 2);
  EXPECT_EQ(config.log_level, "debug");

  unsetenv("TG_TEST_KEY");
  unsetenv("TG_TEST_TOKEN");
  unsetenv("TG_TEST_KEYME");
}

TEST_F(ConfigFileTest, MissingFileThrows) {
  EXPECT_THROW(app::Config::FromFile("/tmp/does-not-exist-xyz.yaml"),
               std::runtime_error);
}

TEST(ConfigDefaults, AutoSearchDisabledByDefault) {
  const app::YamlNode root = app::ParseYaml("");  // empty config
  (void)root;
  // Config::FromFile needs a real file; use FromFile on an empty temp file.
  const std::string path = "/tmp/tg_client_config_empty_" + std::to_string(getpid()) + ".yaml";
  { std::ofstream out(path); out << ""; }
  const app::Config config = app::Config::FromFile(path);
  std::remove(path.c_str());

  EXPECT_FALSE(config.auto_search.enabled);  // phase 2: off by default (v5 §2.7)
  EXPECT_TRUE(config.bot_command.enabled);
  EXPECT_TRUE(config.media_thumbnail.enabled);
  EXPECT_EQ(config.bot_command.process_command, "/process");
  EXPECT_EQ(config.bot_command.search_command, "/search");
  EXPECT_TRUE(config.grpc.tls);
}

TEST(ConfigDefaults, Chunk2Sections) {
  const std::string path = "/tmp/tg_client_config_empty2_" +
                           std::to_string(getpid()) + ".yaml";
  { std::ofstream out(path); out << ""; }
  const app::Config config = app::Config::FromFile(path);
  std::remove(path.c_str());

  // State persistence: OFF by default (opt-in like Python's chathis path).
  EXPECT_FALSE(config.chat_state.enabled);
  EXPECT_EQ(config.chat_state.db_path, "/data/chathis.sqlite");
  EXPECT_EQ(config.chat_state.flush_interval_seconds, 30);
  EXPECT_EQ(config.chat_state.active_window_seconds, 45);

  // Command surface: on, self-commands trigger (the Python model), no
  // control chat configured yet.
  EXPECT_TRUE(config.commands.enabled);
  EXPECT_EQ(config.commands.control_chat_id, 0);
  EXPECT_TRUE(config.commands.self_commands);
  EXPECT_EQ(config.commands.rsme_command, "/rsme");
  EXPECT_EQ(config.commands.rsor_command, "/rsor");
  ASSERT_EQ(config.commands.backfill_commands.size(), 4u);
  EXPECT_EQ(config.commands.backfill_commands[0], "/xm");
  EXPECT_EQ(config.commands.backfill_commands[1], "/xy");
  EXPECT_EQ(config.commands.backfill_commands[2], "/ym");
  EXPECT_EQ(config.commands.backfill_commands[3], "/who");
  EXPECT_EQ(config.commands.dialog_limit, 1202);
  EXPECT_EQ(config.commands.list_batch_lines, 45);
  EXPECT_EQ(config.commands.rsor_report_limit, 50);

  // History backfill: on, TDLib-capped page size.
  EXPECT_TRUE(config.history_backfill.enabled);
  EXPECT_EQ(config.history_backfill.page_limit, 60);
  EXPECT_EQ(config.history_backfill.flood_pad_seconds, 1);

  // Whitelisted media channels: opt-in, ProcessImages pipeline.
  EXPECT_FALSE(config.media_channels.enabled);
  EXPECT_TRUE(config.media_channels.chats.empty());
  EXPECT_EQ(config.media_channels.rpc, "process");

  // Chunk 3: gRPC channel args default to keepalive on, 64 MiB messages,
  // larger HTTP/2 window + write buffer.
  EXPECT_EQ(config.grpc.keepalive_ms, 60000);
  EXPECT_EQ(config.grpc.keepalive_timeout_ms, 20000);
  EXPECT_TRUE(config.grpc.keepalive_permit_without_calls);
  EXPECT_EQ(config.grpc.max_message_bytes, 64LL * 1024 * 1024);
  EXPECT_EQ(config.grpc.http2_lookahead_bytes, 1LL * 1024 * 1024);
  EXPECT_EQ(config.grpc.http2_write_buffer_bytes, 512LL * 1024);
}

TEST(ConfigAutoSearch, MixedChatListParsesIdsAndUsernames) {
  const std::string path =
      "/tmp/tg_client_config_autosearch_" + std::to_string(getpid()) + ".yaml";
  {
    std::ofstream out(path);
    out << R"(
features:
  auto_search:
    enabled: true
    chats:
      - "@publicxme"
      - @yykisschat
      - https://t.me/yakio16366
      - t.me/c/3822626821
      - -1003822626821
    dedup_ttl_seconds: 600
)";
  }
  const app::Config config = app::Config::FromFile(path);
  std::remove(path.c_str());

  ASSERT_EQ(config.auto_search.chat_specs.size(), 5u);
  EXPECT_FALSE(config.auto_search.chat_specs[0].numeric);
  EXPECT_EQ(config.auto_search.chat_specs[0].username, "publicxme");
  EXPECT_EQ(config.auto_search.chat_specs[1].username, "yykisschat");
  EXPECT_EQ(config.auto_search.chat_specs[2].username, "yakio16366");
  EXPECT_TRUE(config.auto_search.chat_specs[3].numeric);
  EXPECT_EQ(config.auto_search.chat_specs[3].dialog_id, -1003822626821LL);
  EXPECT_TRUE(config.auto_search.chat_specs[4].numeric);
  EXPECT_EQ(config.auto_search.chat_specs[4].dialog_id, -1003822626821LL);
  ASSERT_EQ(config.auto_search.chats.size(), 2u);
  EXPECT_EQ(config.auto_search.chats[0], -1003822626821LL);
}

TEST(ConfigAutoSearch, FlowListAndChatLinks) {
  const std::string path =
      "/tmp/tg_client_config_autosearch_flow_" + std::to_string(getpid()) +
      ".yaml";
  {
    std::ofstream out(path);
    out << R"(
features:
  auto_search:
    enabled: true
    chats: ["@publicxme", yykisschat, -1003822626821]  # inline comment
    chat_links:
      -1003822626821: "https://t.me/somegroup/"
      4242: https://t.me/baregroup//
      "@privategroup": "https://t.me/+AbCdEf"
)";
  }
  const app::Config config = app::Config::FromFile(path);
  std::remove(path.c_str());

  ASSERT_EQ(config.auto_search.chat_specs.size(), 3u);
  EXPECT_EQ(config.auto_search.chat_specs[0].username, "publicxme");
  EXPECT_EQ(config.auto_search.chat_specs[1].username, "yykisschat");
  EXPECT_TRUE(config.auto_search.chat_specs[2].numeric);
  EXPECT_EQ(config.auto_search.chat_specs[2].dialog_id, -1003822626821LL);
  ASSERT_EQ(config.auto_search.chats.size(), 1u);
  EXPECT_EQ(config.auto_search.chats[0], -1003822626821LL);

  ASSERT_EQ(config.auto_search.chat_links.size(), 3u);
  EXPECT_TRUE(config.auto_search.chat_links[0].numeric);
  EXPECT_EQ(config.auto_search.chat_links[0].dialog_id, -1003822626821LL);
  EXPECT_EQ(config.auto_search.chat_links[0].bare_id, 3822626821LL);
  EXPECT_EQ(config.auto_search.chat_links[0].url, "https://t.me/somegroup/");
  EXPECT_EQ(config.auto_search.chat_links[1].bare_id, 4242);
  EXPECT_EQ(config.auto_search.chat_links[1].url, "https://t.me/baregroup//");
  EXPECT_FALSE(config.auto_search.chat_links[2].numeric);
  EXPECT_EQ(config.auto_search.chat_links[2].username, "privategroup");
  EXPECT_EQ(config.auto_search.chat_links[2].url, "https://t.me/+AbCdEf");
}

TEST(ConfigAutoSearch, SingleFlowIdIsNotDropped) {
  const std::string path =
      "/tmp/tg_client_config_autosearch_one_" + std::to_string(getpid()) +
      ".yaml";
  {
    std::ofstream out(path);
    out << R"(
features:
  auto_search:
    enabled: true
    chats: [-1003822626821]
)";
  }
  const app::Config config = app::Config::FromFile(path);
  std::remove(path.c_str());
  ASSERT_EQ(config.auto_search.chat_specs.size(), 1u);
  EXPECT_EQ(config.auto_search.chat_specs[0].dialog_id, -1003822626821LL);
  ASSERT_EQ(config.auto_search.chats.size(), 1u);
}
