#include <features/chat_state/D1ChatStateBackend.h>

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cs = tg::features::chat_state;

namespace {

class ScriptedHttp : public cs::D1HttpTransport {
 public:
  std::vector<cs::D1HttpResponse> script;
  std::vector<std::string> bodies;
  std::vector<std::string> authorizations;
  int resets = 0;
  std::vector<std::chrono::milliseconds> sleeps;

  cs::D1HttpResponse Post(const std::string&, const std::string& authorization,
                          const std::string& json_body, int) override {
    bodies.push_back(json_body);
    authorizations.push_back(authorization);
    if (script.empty()) {
      cs::D1HttpResponse missing;
      missing.transport_error = true;
      missing.error = "no scripted response";
      return missing;
    }
    cs::D1HttpResponse next = std::move(script.front());
    script.erase(script.begin());
    return next;
  }

  void Reset() override { ++resets; }
};

cs::D1HttpResponse Ok(const std::string& body) {
  cs::D1HttpResponse response;
  response.status = 200;
  response.body = body;
  response.num_connects = 0;
  return response;
}

cs::ChatStateRow Row(std::int64_t chat, std::int64_t offset) {
  cs::ChatStateRow row;
  row.chat_id = chat;
  row.offset_id = offset;
  row.chat_url = "https://t.me/name/";
  row.init_date = "20261114";
  row.init_time = "12:00:00";
  row.updated_at = "2026-11-14 12:00:01";
  return row;
}

cs::D1Settings TestSettings() {
  cs::D1Settings settings;
  settings.account_id = "acct";
  settings.api_token = "secret-token";
  settings.database_id = "db";
  settings.batch_size = 500;
  settings.max_sql_variables = 90;
  settings.max_attempts = 3;
  settings.timeout_seconds = 35;
  return settings;
}

}  // namespace

TEST(D1Offset, ShiftsTdlibIdAndSkipsNonDivisible) {
  EXPECT_EQ(cs::TdlibOffsetToServer(31799115776LL), 30326);
  EXPECT_EQ(cs::TdlibOffsetToServer(0), 0);
  EXPECT_FALSE(cs::TdlibOffsetToServer(30326).has_value());
  EXPECT_FALSE(cs::TdlibOffsetToServer(-1048576).has_value());
  EXPECT_EQ(cs::ServerOffsetToTdlib(30326), 31799115776LL);
  EXPECT_FALSE(cs::ServerOffsetToTdlib(-1).has_value());
  EXPECT_FALSE(cs::ServerOffsetToTdlib(
                   (std::numeric_limits<std::int64_t>::max() >> 20) + 1)
                   .has_value());
}

TEST(D1UpsertSql, ChunksAtFifteenRowsOfSixParams) {
  std::vector<cs::ChatStateRow> rows;
  for (int i = 0; i < 16; ++i) rows.push_back(Row(i + 1, i));
  const auto statements = cs::BuildChathisUpserts(rows, 500, 90);
  ASSERT_EQ(statements.size(), 2u);
  EXPECT_EQ(statements[0].param_count, 90);
  EXPECT_EQ(statements[1].param_count, 6);
  EXPECT_NE(statements[0].sql.find(
                "ON CONFLICT(chat_id) DO UPDATE SET offset_id = "
                "excluded.offset_id"),
            std::string::npos);
  EXPECT_NE(statements[0].body.find("\"params\":["), std::string::npos);
  int placeholders = 0;
  for (char c : statements[0].sql) {
    if (c == '?') ++placeholders;
  }
  EXPECT_EQ(placeholders, 90);
}

TEST(D1Response, SuccessErrorsAndDateForms) {
  const char* ok = R"({
    "success": true,
    "result": [{
      "success": true,
      "results": [{
        "chat_id": 42,
        "offset_id": 30326,
        "chat_url": null,
        "init_date": 20261114,
        "init_time": "01:02:03.456"
      }, {
        "chat_id": "7",
        "offset_id": "1",
        "chat_url": "https://t.me/a/",
        "init_date": "20260101",
        "init_time": "00:00:00"
      }]
    }]
  })";
  const auto rows = cs::ParseChathisLoadAll(ok);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].chat_id, 42);
  EXPECT_EQ(rows[0].offset_id, 30326LL << 20);
  EXPECT_TRUE(rows[0].chat_url.empty());
  EXPECT_EQ(rows[0].init_date, "20261114");
  EXPECT_EQ(rows[0].init_time, "01:02:03");
  EXPECT_EQ(rows[1].chat_id, 7);
  EXPECT_EQ(rows[1].offset_id, 1LL << 20);
  EXPECT_EQ(rows[1].chat_url, "https://t.me/a/");
  EXPECT_EQ(rows[1].init_date, "20260101");

  EXPECT_THROW(cs::ParseChathisLoadAll(
                   R"({"success":false,"errors":[{"message":"nope"}]})"),
               std::runtime_error);
  EXPECT_THROW(
      cs::ParseChathisLoadAll(
          R"({"success":true,"result":[{"success":false,"error":"bad sql"}]})"),
      std::runtime_error);
}

TEST(D1Backend, UpsertConvertsOffsetAndRetriesTransient) {
  auto http = std::make_unique<ScriptedHttp>();
  ScriptedHttp* raw = http.get();
  // Lookup of the non-TDLib row, then the upsert (500, then success).
  raw->script.push_back(
      Ok(R"({"success":true,"result":[{"success":true,"results":[]}]})"));
  raw->script.push_back(cs::D1HttpResponse{500, "busy", false, "", 1});
  raw->script.push_back(Ok(R"({"success":true,"result":[{"success":true}]})"));
  std::vector<std::chrono::milliseconds> slept;
  cs::D1ChatStateBackend backend(
      TestSettings(), std::move(http),
      [&](std::chrono::milliseconds delay) { slept.push_back(delay); });
  backend.Upsert({Row(9, 31799115776LL), Row(10, 30326)});
  ASSERT_EQ(raw->bodies.size(), 3u);
  EXPECT_EQ(raw->authorizations.front(), "Bearer secret-token");
  EXPECT_NE(raw->bodies[0].find("SELECT chat_id, offset_id"), std::string::npos);
  EXPECT_NE(raw->bodies[1].find("30326"), std::string::npos);
  EXPECT_EQ(raw->bodies[1].find("31799115776"), std::string::npos);
  EXPECT_EQ(raw->bodies[1], raw->bodies[2]);
  ASSERT_EQ(slept.size(), 1u);
  EXPECT_EQ(slept[0].count(), 500);
  EXPECT_EQ(raw->resets, 0);
}

TEST(D1Backend, AuthErrorDoesNotRetry) {
  auto http = std::make_unique<ScriptedHttp>();
  ScriptedHttp* raw = http.get();
  raw->script.push_back(cs::D1HttpResponse{401, "no", false, "", 1});
  int sleeps = 0;
  cs::D1ChatStateBackend backend(
      TestSettings(), std::move(http),
      [&](std::chrono::milliseconds) { ++sleeps; });
  EXPECT_THROW(backend.LoadAll(), std::runtime_error);
  EXPECT_EQ(raw->bodies.size(), 1u);
  EXPECT_EQ(sleeps, 0);
}

TEST(D1Backend, KeepsExistingOffsetWhenMidIsNotTdlib) {
  auto http = std::make_unique<ScriptedHttp>();
  ScriptedHttp* raw = http.get();
  raw->script.push_back(Ok(
      R"({"success":true,"result":[{"success":true,"results":[{"chat_id":10,"offset_id":99}]}]})"));
  raw->script.push_back(Ok(R"({"success":true,"result":[{"success":true}]})"));
  cs::D1ChatStateBackend backend(TestSettings(), std::move(http),
                                 [](std::chrono::milliseconds) {});
  backend.Upsert({Row(10, 30326)});
  ASSERT_EQ(raw->bodies.size(), 2u);
  EXPECT_NE(raw->bodies[0].find("SELECT chat_id, offset_id"), std::string::npos);
  EXPECT_NE(raw->bodies[1].find("99"), std::string::npos);
  EXPECT_EQ(raw->bodies[1].find("30326"), std::string::npos);
}
