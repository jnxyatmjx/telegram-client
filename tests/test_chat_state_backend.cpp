// State persistence (Telethon migration Chunk 2; Python chathis /
// schedule2db / load_chat_state): GMT+8 field formatting + parsing, the
// SQLite `chathis` backend, and the FlushActive -> Restore round-trip
// through ChatStateStore.
#include <common/Gmt8.h>
#include <features/chat_state/ChatStateStore.h>
#include <features/chat_state/SqliteChatStateBackend.h>
#include <telegram/TelegramClient.h>

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace td_api = td::td_api;

namespace chat_state = tg::features::chat_state;
namespace common = tg::common;

namespace {

class FakeTdSender : public tg::telegram::ITdSender {
 public:
  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback) override {
    td::ClientManager::Response response;
    response.request_id = next_id_++;
    if (request->get_id() == td_api::getChat::ID) {
      ++sent_getChat;
      const auto chat_id =
          static_cast<const td_api::getChat&>(*request).chat_id_;
      if (chat_id != kSgDialog) {
        response.object = td_api::make_object<td_api::error>(400, "no chat");
      } else {
        auto chat = td_api::make_object<td_api::chat>();
        chat->id_ = chat_id;
        chat->title_ = "Deals";
        chat->type_ = td_api::make_object<td_api::chatTypeSupergroup>(
            kBare, false);
        response.object = std::move(chat);
      }
    } else if (request->get_id() == td_api::getSupergroup::ID) {
      auto supergroup = td_api::make_object<td_api::supergroup>();
      supergroup->id_ = kBare;
      supergroup->usernames_ = td_api::make_object<td_api::usernames>(
          std::vector<std::string>{"dealchannel"}, std::vector<std::string>{},
          "", std::vector<std::string>{}, "");
      response.object = std::move(supergroup);
    } else {
      response.object = td_api::make_object<td_api::error>(400, "unexpected");
    }
    callback(std::move(response));
    return static_cast<std::int64_t>(response.request_id);
  }

  int sent_getChat = 0;

  static constexpr std::int64_t kBare = 1'234'567'890;
  static constexpr std::int64_t kSgDialog = -1'001'234'567'890LL;

 private:
  std::uint64_t next_id_ = 1;
};

std::string TempDbPath(const char* tag) {
  return "/tmp/tg_client_chathis_" + std::string(tag) + "_" +
         std::to_string(getpid()) + ".sqlite";
}

// FlushActive compares its `now_ms` argument against the store's
// system-clock last_seen — the tests use the real clock too.
std::int64_t SystemNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

// --- Gmt8 -----------------------------------------------------------------------

TEST(Gmt8, FormatsFixedOffsetFields) {
  // 2026-01-01 03:04:05 GMT+8 == 2025-12-31T19:04:05Z
  const std::int64_t epoch_ms = 1'767'207'845'000LL;
  EXPECT_EQ(common::Gmt8Date(epoch_ms), "20260101");
  EXPECT_EQ(common::Gmt8Clock(epoch_ms), "03:04:05");
  EXPECT_EQ(common::Gmt8DateTime(epoch_ms), "2026-01-01 03:04:05");
}

TEST(Gmt8, DateRollsForwardAtUtcMidnight) {
  // 2025-12-31 20:00:00 UTC is already 2026-01-01 04:00:00 in GMT+8.
  const std::int64_t epoch_ms = 1'767'211'200'000LL;
  EXPECT_EQ(common::Gmt8Date(epoch_ms), "20260101");
  EXPECT_EQ(common::Gmt8Clock(epoch_ms), "04:00:00");
}

TEST(Gmt8, ClockSameUtcTimeDifferentDay) {
  // 2026-01-02 03:04:05 GMT+8 — one day later than the FormatsFixedOffset
  // case, same clock string.
  const std::int64_t epoch_ms = 1'767'207'845'000LL + 86'400'000LL;
  EXPECT_EQ(common::Gmt8Date(epoch_ms), "20260102");
  EXPECT_EQ(common::Gmt8Clock(epoch_ms), "03:04:05");
}

TEST(Gmt8, ParseGmt8RoundTrip) {
  const auto parsed = common::ParseGmt8(20260101, "03:04:05");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, 1'767'207'845'000LL);
}

TEST(Gmt8, ParseGmt8DropsFractionalTail) {
  // Python str(time) formatting: "03:04:05.123456" — the fraction is cut.
  const auto parsed = common::ParseGmt8(20260101, "03:04:05.123456");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, 1'767'207'845'000LL);
}

TEST(Gmt8, ParseGmt8RejectsGarbage) {
  EXPECT_FALSE(common::ParseGmt8(20260101, "").has_value());
  EXPECT_FALSE(common::ParseGmt8(20260101, "not-a-time").has_value());
  EXPECT_FALSE(common::ParseGmt8(20260101, "25:00:00").has_value());
  EXPECT_FALSE(common::ParseGmt8(0, "03:04:05").has_value());
  EXPECT_FALSE(common::ParseGmt8(-5, "03:04:05").has_value());
  EXPECT_FALSE(common::ParseGmt8(20261332, "03:04:05").has_value());
}

// --- SqliteChatStateBackend -----------------------------------------------------

TEST(SqliteChatStateBackend, UpsertThenLoadAll) {
  const std::string path = TempDbPath("upsert");
  std::remove(path.c_str());
  auto backend = chat_state::MakeSqliteChatStateBackend(path);

  std::vector<chat_state::ChatStateRow> rows;
  chat_state::ChatStateRow a;
  a.chat_id = 42;
  a.offset_id = 100;
  a.chat_url = "https://t.me/dealchannel/";
  a.init_date = "20260101";
  a.init_time = "03:04:05";
  a.updated_at = "2026-01-01 03:04:05";
  rows.push_back(a);
  chat_state::ChatStateRow b;
  b.chat_id = 77;
  b.offset_id = 5;
  b.chat_url = ">>>>Nrror>77";  // placeholder persists like a real link
  b.init_date = "20251231";
  b.init_time = "23:59:59";
  b.updated_at = "2025-12-31 23:59:59";
  rows.push_back(b);
  backend->Upsert(rows);

  const auto loaded = backend->LoadAll();
  ASSERT_EQ(loaded.size(), 2u);
  bool saw_a = false;
  bool saw_b = false;
  for (const auto& row : loaded) {
    if (row.chat_id == 42) {
      saw_a = true;
      EXPECT_EQ(row.offset_id, 100);
      EXPECT_EQ(row.chat_url, "https://t.me/dealchannel/");
      EXPECT_EQ(row.init_date, "20260101");
      EXPECT_EQ(row.init_time, "03:04:05");
      EXPECT_EQ(row.updated_at, "2026-01-01 03:04:05");
    } else if (row.chat_id == 77) {
      saw_b = true;
      EXPECT_EQ(row.offset_id, 5);
      EXPECT_EQ(row.chat_url, ">>>>Nrror>77");
    }
  }
  EXPECT_TRUE(saw_a);
  EXPECT_TRUE(saw_b);
  std::remove(path.c_str());
}

TEST(SqliteChatStateBackend, UpsertSameChatUpdatesWatermarkOnly) {
  const std::string path = TempDbPath("conflict");
  std::remove(path.c_str());
  auto backend = chat_state::MakeSqliteChatStateBackend(path);

  chat_state::ChatStateRow row;
  row.chat_id = 42;
  row.offset_id = 100;
  row.chat_url = "https://t.me/dealchannel/";
  row.init_date = "20260101";
  row.init_time = "03:04:05";
  row.updated_at = "2026-01-01 03:04:05";
  std::vector<chat_state::ChatStateRow> rows{row};
  backend->Upsert(rows);

  row.offset_id = 250;  // forward only — same shape, new watermark
  row.updated_at = "2026-01-01 03:04:35";
  rows = {row};
  backend->Upsert(rows);

  const auto loaded = backend->LoadAll();
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0].chat_id, 42);
  EXPECT_EQ(loaded[0].offset_id, 250);
  EXPECT_EQ(loaded[0].chat_url, "https://t.me/dealchannel/");
  EXPECT_EQ(loaded[0].updated_at, "2026-01-01 03:04:35");
  std::remove(path.c_str());
}

TEST(SqliteChatStateBackend, RowsSurviveReopen) {
  const std::string path = TempDbPath("reopen");
  std::remove(path.c_str());
  {
    auto backend = chat_state::MakeSqliteChatStateBackend(path);
    chat_state::ChatStateRow row;
    row.chat_id = 9;
    row.offset_id = 1234;
    row.chat_url = "";
    row.init_date = "20260101";
    row.init_time = "00:00:00";
    row.updated_at = "2026-01-01 00:00:30";
    std::vector<chat_state::ChatStateRow> rows{row};
    backend->Upsert(rows);
  }
  auto reopened = chat_state::MakeSqliteChatStateBackend(path);
  const auto loaded = reopened->LoadAll();
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0].chat_id, 9);
  EXPECT_EQ(loaded[0].offset_id, 1234);
  std::remove(path.c_str());
}

// --- FlushActive / Restore round-trip -------------------------------------------

TEST(ChatStatePersistence, FlushRestoreRoundTripKeepsUrlAndWatermark) {
  const std::string path = TempDbPath("roundtrip");
  std::remove(path.c_str());
  FakeTdSender sender;
  std::shared_ptr<chat_state::ChatStateBackend> backend =
      chat_state::MakeSqliteChatStateBackend(path);

  chat_state::ChatStateStore store(sender, backend);
  store.ApplyReply(FakeTdSender::kBare, 555);  // resolves the URL via TDLib
  const std::int64_t now_ms = SystemNowMs();
  EXPECT_EQ(store.FlushActive(now_ms, 45), 1u);

  // A second store over the same backend restores the persisted row; the
  // restored non-empty URL counts as resolved (no TDLib traffic ever again
  // for it) and the watermark continues from 555.
  chat_state::ChatStateStore restored_store(sender, backend);
  EXPECT_EQ(restored_store.Restore(), 1u);
  const auto entry = restored_store.Get(FakeTdSender::kBare);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->offset_id, 555);
  EXPECT_EQ(entry->chat_url, "https://t.me/dealchannel/");
  restored_store.ApplyReply(FakeTdSender::kBare, 600);
  EXPECT_EQ(restored_store.Get(FakeTdSender::kBare)->offset_id, 600);
  EXPECT_EQ(sender.sent_getChat, 1);  // only the first store resolved

  std::remove(path.c_str());
}

TEST(ChatStatePersistence, FlushActiveWindowSkipsInactiveChats) {
  const std::string path = TempDbPath("window");
  std::remove(path.c_str());
  FakeTdSender sender;
  std::shared_ptr<chat_state::ChatStateBackend> backend =
      chat_state::MakeSqliteChatStateBackend(path);
  chat_state::ChatStateStore store(sender, backend);

  store.ApplyReply(11, 100);  // last_seen = system now
  const std::int64_t now_ms = SystemNowMs();
  EXPECT_EQ(store.FlushActive(now_ms, 45), 1u);     // within window
  EXPECT_EQ(store.FlushActive(now_ms + 100'000, 45), 0u);  // now inactive

  const auto loaded = backend->LoadAll();
  ASSERT_EQ(loaded.size(), 1u);
  EXPECT_EQ(loaded[0].chat_id, 11);
  std::remove(path.c_str());
}

TEST(ChatStatePersistence, FlushActiveFieldsMatchPythonShape) {
  const std::string path = TempDbPath("shape");
  std::remove(path.c_str());
  FakeTdSender sender;
  std::shared_ptr<chat_state::ChatStateBackend> backend =
      chat_state::MakeSqliteChatStateBackend(path);
  chat_state::ChatStateStore store(sender, backend);
  store.ApplyReply(FakeTdSender::kBare, 77);
  const auto before = store.Get(FakeTdSender::kBare);
  ASSERT_TRUE(before.has_value());
  // Python schedule2db: init_date/init_time from the watermark advance
  // (t_t = last_seen), updated_at from the flush `now`; both GMT+8 fixed
  // offset. The exact GMT+8 conversion is pinned by the Gmt8 tests above;
  // here we pin the SHAPES and the source of each field.
  const std::int64_t now_ms = SystemNowMs();
  ASSERT_EQ(store.FlushActive(now_ms, 45), 1u);

  const auto loaded = backend->LoadAll();
  ASSERT_EQ(loaded.size(), 1u);
  ASSERT_EQ(loaded[0].init_date.size(), 8u);
  EXPECT_EQ(loaded[0].init_date, common::Gmt8Date(before->last_seen_ms));
  ASSERT_EQ(loaded[0].init_time.size(), 8u);
  EXPECT_EQ(loaded[0].init_time, common::Gmt8Clock(before->last_seen_ms));
  EXPECT_EQ(loaded[0].updated_at, common::Gmt8DateTime(now_ms));
  EXPECT_EQ(loaded[0].offset_id, 77);
  EXPECT_EQ(loaded[0].chat_url, "https://t.me/dealchannel/");
  std::remove(path.c_str());
}

TEST(ChatStatePersistence, MemoryOnlyStoreFlushesNothing) {
  FakeTdSender sender;
  chat_state::ChatStateStore store(sender);  // no backend
  store.ApplyReply(5, 10);
  EXPECT_EQ(store.FlushActive(1'000, 45), 0u);
  EXPECT_FALSE(store.persistent());
}
