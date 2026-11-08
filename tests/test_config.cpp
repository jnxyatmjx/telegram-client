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

grpc:
  target: "dns:///media.example.com:443"
  tls: false
  token: ${TG_TEST_TOKEN}
  ca_cert: /etc/ssl/tg-ca.crt
  ack_timeout_ms: 45000
  queue_capacity: 50

features:
  media_thumbnail:
    enabled: false
    max_thumb_bytes: 1048576
    max_edge_px: 256
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

  const app::Config config = app::Config::FromFile(path_);

  EXPECT_EQ(config.telegram.api_id, 123456);
  EXPECT_EQ(config.telegram.api_hash, "abc-hash");
  EXPECT_EQ(config.telegram.encryption_key, "secret-key");
  EXPECT_EQ(config.grpc.target, "dns:///media.example.com:443");
  EXPECT_FALSE(config.grpc.tls);
  EXPECT_EQ(config.grpc.token, "grpc-token");
  EXPECT_EQ(config.grpc.ca_cert_path, "/etc/ssl/tg-ca.crt");
  EXPECT_EQ(config.grpc.ack_timeout_ms, 45000);
  EXPECT_EQ(config.grpc.queue_capacity, 50);
  EXPECT_FALSE(config.media_thumbnail.enabled);
  EXPECT_EQ(config.media_thumbnail.max_edge_px, 256);
  EXPECT_EQ(config.bot_command.command_bot_user_id, 987654321);
  EXPECT_TRUE(config.auto_search.enabled);
  EXPECT_EQ(config.auto_search.chats.size(), 2u);
  EXPECT_EQ(config.outbox.db_path, "/tmp/outbox-test.sqlite");
  EXPECT_EQ(config.worker_count, 2);
  EXPECT_EQ(config.log_level, "debug");

  unsetenv("TG_TEST_KEY");
  unsetenv("TG_TEST_TOKEN");
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
