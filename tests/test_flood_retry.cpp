// FloodWait handling (Telethon migration chunk 3): telegram/TdErrors 429
// parsing, the shared telegram/FloodRetry scheduler (bounded, retry-after
// respecting re-issues) and the FileDownloader downloadFile flood path —
// a rate-limited download is retried, not dropped, and never re-sent
// before the retry-after passes.
#include <telegram/FileDownloader.h>
#include <telegram/FloodRetry.h>
#include <telegram/TdErrors.h>
#include <telegram/TelegramClient.h>

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace td_api = td::td_api;
namespace telegram = tg::telegram;

namespace {

constexpr auto kWait = std::chrono::seconds(5);

td_api::object_ptr<td_api::Object> FloodError(int retry_after) {
  return td_api::make_object<td_api::error>(
      429, "Too Many Requests: retry after " + std::to_string(retry_after));
}

// --- TdErrors parsing ------------------------------------------------------

TEST(TdErrors, IsFloodWaitMatchesCode429Only) {
  const auto flood = td_api::make_object<td_api::error>(
      429, "Too Many Requests: retry after 42");
  const auto other = td_api::make_object<td_api::error>(
      400, "Too Many Requests: retry after 42");  // wrong code
  EXPECT_TRUE(telegram::IsFloodWait(*flood));
  EXPECT_FALSE(telegram::IsFloodWait(*other));
}

TEST(TdErrors, RetryAfterSecondsParsesTheWireText) {
  const auto flood = td_api::make_object<td_api::error>(
      429, "Too Many Requests: retry after 42");
  EXPECT_EQ(telegram::RetryAfterSeconds(*flood), std::optional<std::int64_t>(42));
  EXPECT_EQ(telegram::RetryAfterOr(*flood, 5), 42);
}

TEST(TdErrors, RetryAfterSecondsRejectsUnparseableText) {
  const auto no_number = td_api::make_object<td_api::error>(429, "Too Many Requests: retry after ");
  const auto junk = td_api::make_object<td_api::error>(429, "Too Many Requests: retry after x1");
  const auto wrong_prefix = td_api::make_object<td_api::error>(429, "slow down please");
  EXPECT_EQ(telegram::RetryAfterSeconds(*no_number), std::nullopt);
  EXPECT_EQ(telegram::RetryAfterSeconds(*junk), std::nullopt);
  EXPECT_EQ(telegram::RetryAfterSeconds(*wrong_prefix), std::nullopt);
  // RetryAfterOr falls back in all three cases.
  EXPECT_EQ(telegram::RetryAfterOr(*no_number, 5), 5);
  EXPECT_EQ(telegram::RetryAfterOr(*junk, 7), 7);
  EXPECT_EQ(telegram::RetryAfterOr(*wrong_prefix, 9), 9);
}

// --- FloodRetry ------------------------------------------------------------

TEST(FloodRetry, RunsTheClosureAfterRetryAfterPlusPad) {
  telegram::FloodRetry scheduler({/*pad_seconds=*/1, /*fallback_seconds=*/5,
                                  /*max_attempts=*/3});
  std::promise<void> fired;
  const auto start = std::chrono::steady_clock::now();
  const bool scheduled = scheduler.Schedule(
      *td_api::move_object_as<td_api::error>(FloodError(0)), 1,
      [&fired] { fired.set_value(); });
  ASSERT_TRUE(scheduled);
  ASSERT_EQ(fired.get_future().wait_for(kWait), std::future_status::ready);
  // pad_seconds=1 on a retry-after of 0: the closure may not run at once.
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count(),
            900);
  EXPECT_LE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count(),
            4000);
  scheduler.Shutdown();
}

TEST(FloodRetry, FallsBackWhenTheMessageCarriesNoNumber) {
  // retry-after unparseable -> fallback_seconds=1 (padded with 0): fires
  // quickly, proving the fallback path schedules rather than dropping.
  telegram::FloodRetry scheduler({/*pad_seconds=*/0, /*fallback_seconds=*/1,
                                  /*max_attempts=*/3});
  std::promise<void> fired;
  const auto error = td_api::make_object<td_api::error>(429, "slow down");
  const bool scheduled = scheduler.Schedule(*error, 1, [&fired] {
    fired.set_value();
  });
  ASSERT_TRUE(scheduled);
  EXPECT_EQ(fired.get_future().wait_for(kWait), std::future_status::ready);
  scheduler.Shutdown();
}

TEST(FloodRetry, DropsAttemptsBeyondTheCap) {
  telegram::FloodRetry scheduler({/*pad_seconds=*/0, /*fallback_seconds=*/5,
                                  /*max_attempts=*/3});
  const auto error = td_api::make_object<td_api::error>(
      429, "Too Many Requests: retry after 0");
  EXPECT_TRUE(scheduler.Schedule(*error, 1, [] {}));
  EXPECT_TRUE(scheduler.Schedule(*error, 2, [] {}));
  EXPECT_TRUE(scheduler.Schedule(*error, 3, [] {}));
  EXPECT_FALSE(scheduler.Schedule(*error, 4, [] {}));  // cap
  EXPECT_FALSE(scheduler.Schedule(*error, 99, [] {}));
  scheduler.Shutdown();
}

TEST(FloodRetry, ShutdownDropsPendingRetries) {
  telegram::FloodRetry scheduler({/*pad_seconds=*/0, /*fallback_seconds=*/5,
                                  /*max_attempts=*/3});
  std::atomic<bool> ran{false};
  const auto error = td_api::make_object<td_api::error>(
      429, "Too Many Requests: retry after 30");  // long pause
  ASSERT_TRUE(
      scheduler.Schedule(*error, 1, [&ran] { ran.store(true); }));
  ASSERT_EQ(scheduler.pending_count(), 1u);
  scheduler.Shutdown();
  EXPECT_EQ(scheduler.pending_count(), 0u);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(ran.load());  // dropped, never fired
  // Shutdown is idempotent.
  scheduler.Shutdown();
}

TEST(FloodRetry, ScheduleAfterShutdownReturnsFalse) {
  telegram::FloodRetry scheduler;
  scheduler.Shutdown();
  const auto error = td_api::make_object<td_api::error>(
      429, "Too Many Requests: retry after 0");
  std::atomic<bool> ran{false};
  EXPECT_FALSE(scheduler.Schedule(*error, 1, [&ran] { ran.store(true); }));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(ran.load());
}

TEST(FloodRetry, MultiplePendingRetriesAllRun) {
  telegram::FloodRetry scheduler({/*pad_seconds=*/0, /*fallback_seconds=*/5,
                                  /*max_attempts=*/3});
  std::promise<void> first, second;
  const auto error = td_api::make_object<td_api::error>(
      429, "Too Many Requests: retry after 0");
  ASSERT_TRUE(
      scheduler.Schedule(*error, 1, [&first] { first.set_value(); }));
  ASSERT_TRUE(
      scheduler.Schedule(*error, 1, [&second] { second.set_value(); }));
  EXPECT_EQ(scheduler.pending_count(), 2u);
  EXPECT_EQ(first.get_future().wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(second.get_future().wait_for(kWait), std::future_status::ready);
  scheduler.Shutdown();
}

// --- FileDownloader flood path ----------------------------------------------

// downloadFile-only fake: `flood_left` requests are answered with a 429
// (retry after 0), the rest with a completed file.
class FloodFakeSender : public telegram::ITdSender {
 public:
  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    telegram::ResponseCallback callback) override {
    ++download_requests;
    const std::int32_t file_id =
        static_cast<td_api::downloadFile*>(request.get())->file_id_;
    td::ClientManager::Response response;
    response.request_id = next_id_++;
    if (flood_left > 0) {
      --flood_left;
      response.object = FloodError(0);
    } else {
      auto file = td_api::make_object<td_api::file>();
      file->id_ = file_id;
      auto local = td_api::make_object<td_api::localFile>();
      local->path_ = "/tmp/flood-thumb";
      local->is_downloading_completed_ = true;
      file->local_ = std::move(local);
      response.object = std::move(file);
    }
    callback(std::move(response));
    return static_cast<std::int64_t>(response.request_id);
  }

  int download_requests = 0;
  int flood_left = 0;

 private:
  std::uint64_t next_id_ = 1;
};

TEST(FileDownloaderFlood, RetriesTheSameDownloadAfterFloodWait) {
  FloodFakeSender sender;
  telegram::FloodRetry scheduler({/*pad_seconds=*/0, /*fallback_seconds=*/5,
                                  /*max_attempts=*/3});
  telegram::FileDownloader files(sender, &scheduler);
  sender.flood_left = 1;  // first downloadFile: 429, second: the file

  auto future = files.Download(4242);
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(future.get(), "/tmp/flood-thumb");
  EXPECT_EQ(sender.download_requests, 2);  // initial + one flood retry
  scheduler.Shutdown();
}

TEST(FileDownloaderFlood, GivesUpAfterTheAttemptCap) {
  FloodFakeSender sender;
  telegram::FloodRetry scheduler({/*pad_seconds=*/0, /*fallback_seconds=*/5,
                                  /*max_attempts=*/3});
  telegram::FileDownloader files(sender, &scheduler);
  sender.flood_left = 100;  // always rate-limited

  auto future = files.Download(4242);
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_THROW(future.get(), std::runtime_error);
  // Initial attempt + 3 re-issues, then the cap refuses a 4th re-issue.
  EXPECT_EQ(sender.download_requests, 4);
  EXPECT_EQ(files.pending_count(), 0u);  // callbacks were failed, not held
  scheduler.Shutdown();
}

TEST(FileDownloaderFlood, RideAlongCallbacksFailWithTheGiveUp) {
  FloodFakeSender sender;
  telegram::FloodRetry scheduler({/*pad_seconds=*/0, /*fallback_seconds=*/5,
                                  /*max_attempts=*/3});
  telegram::FileDownloader files(sender, &scheduler);
  sender.flood_left = 100;

  auto a = files.Download(4242);
  auto b = files.Download(4242);  // rides along on the same request
  ASSERT_EQ(a.wait_for(kWait), std::future_status::ready);
  ASSERT_EQ(b.wait_for(kWait), std::future_status::ready);
  EXPECT_THROW(a.get(), std::runtime_error);
  EXPECT_THROW(b.get(), std::runtime_error);
  EXPECT_EQ(sender.download_requests, 4);  // NOT doubled by the second caller
  scheduler.Shutdown();
}

TEST(FileDownloaderFlood, NullSchedulerKeepsTheOldFailFast) {
  FloodFakeSender sender;
  telegram::FileDownloader files(sender);  // no FloodRetry wired
  sender.flood_left = 1;

  auto future = files.Download(4242);
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_THROW(future.get(), std::runtime_error);
  EXPECT_EQ(sender.download_requests, 1);  // no retry
}

TEST(FileDownloaderFlood, FloodDoesNotDoubleSendForTheSameFile) {
  // While a flood retry is pending, a new DownloadAsync only rides along —
  // no second downloadFile before the retry-after has passed.
  FloodFakeSender sender;
  telegram::FloodRetry scheduler({/*pad_seconds=*/1, /*fallback_seconds=*/5,
                                  /*max_attempts=*/3});
  telegram::FileDownloader files(sender, &scheduler);
  sender.flood_left = 1;

  auto future = files.Download(4242);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  auto rider = files.Download(4242);
  // Still within the 1 s pad: only the original request exists so far.
  EXPECT_LE(sender.download_requests, 1);
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  ASSERT_EQ(rider.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(sender.download_requests, 2);
  scheduler.Shutdown();
}

}  // namespace
