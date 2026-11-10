// Outbox state machine: PENDING -> DONE on reply, retry on failure,
// dead-letter on permanent errors / attempt exhaustion, persistence across
// reopen, purge.
#include <infra/Outbox.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <string>

namespace infra = tg::infra;

namespace {

std::string TempDbPath() {
  return "/tmp/tg_client_outbox_test_" + std::to_string(getpid()) + ".sqlite";
}

infra::OutboxRecord MakeRecord(const std::string& image_id) {
  infra::OutboxRecord record;
  record.image_id = image_id;
  record.image_data = "\x01\x02thumb-bytes";
  record.memo = "caption text";  // caption only, never JSON (v5 §1.2)
  record.owner_id = -1'001'234'567'890LL;
  record.state = infra::OutboxState::kPending;
  return record;
}

}  // namespace

class OutboxTest : public ::testing::Test {
 protected:
  void SetUp() override {
    path_ = TempDbPath();
    std::remove(path_.c_str());
    outbox_ = std::make_unique<infra::Outbox>(path_);
  }
  void TearDown() override {
    outbox_.reset();
    std::remove(path_.c_str());
  }
  std::string path_;
  std::unique_ptr<infra::Outbox> outbox_;
};

TEST_F(OutboxTest, PutThenGetRoundTrip) {
  outbox_->Put(MakeRecord("c1:m1:uid"));
  const auto record = outbox_->Get("c1:m1:uid");
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->state, infra::OutboxState::kPending);
  EXPECT_EQ(record->attempts, 0);
  EXPECT_EQ(record->image_data, "\x01\x02thumb-bytes");
  EXPECT_EQ(record->memo, "caption text");
  EXPECT_EQ(record->owner_id, -1'001'234'567'890LL);
}

TEST_F(OutboxTest, DoneOnReply) {
  outbox_->Put(MakeRecord("c1:m1:uid"));
  EXPECT_TRUE(outbox_->MarkDone("c1:m1:uid", "processed ok"));
  const auto record = outbox_->Get("c1:m1:uid");
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->state, infra::OutboxState::kDone);
  EXPECT_EQ(record->last_status, "processed ok");
  EXPECT_EQ(outbox_->PendingCount(), 0);
}

TEST_F(OutboxTest, UnknownIdTransitionsFail) {
  EXPECT_FALSE(outbox_->MarkDone("nope", "x"));
  EXPECT_FALSE(outbox_->MarkFailed("nope", "x", 1000));
  EXPECT_FALSE(outbox_->MarkDeadLetter("nope", "x"));
  EXPECT_FALSE(outbox_->MarkRequeued("nope", 1000, 3, "x"));
}

TEST_F(OutboxTest, RequeuedBumpsAttemptsAndDefers) {
  outbox_->Put(MakeRecord("c1:m1:uid"));
  EXPECT_TRUE(outbox_->MarkRequeued("c1:m1:uid", 5000, 3, "requeued"));
  auto record = outbox_->Get("c1:m1:uid");
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->state, infra::OutboxState::kPending);
  EXPECT_EQ(record->attempts, 1);
  EXPECT_GT(record->next_retry_at_ms, infra::OutboxRecord{}.next_retry_at_ms);

  // Not yet due (next_retry_at is in the future).
  const auto now = record->next_retry_at_ms - 1;
  EXPECT_TRUE(outbox_->DueForRetry(now, 10).empty());
  EXPECT_EQ(outbox_->DueForRetry(now + 6000, 10).size(), 1u);
}

TEST_F(OutboxTest, AttemptExhaustionDeadLetters) {
  outbox_->Put(MakeRecord("c1:m1:uid"));
  outbox_->MarkRequeued("c1:m1:uid", 0, 3, "retry 1");  // attempts 1
  outbox_->MarkRequeued("c1:m1:uid", 0, 3, "retry 2");  // attempts 2
  auto record = outbox_->Get("c1:m1:uid");
  EXPECT_EQ(record->state, infra::OutboxState::kPending);
  outbox_->MarkRequeued("c1:m1:uid", 0, 3, "retry 3");  // attempts 3 -> dead
  record = outbox_->Get("c1:m1:uid");
  EXPECT_EQ(record->state, infra::OutboxState::kDeadLetter);
  EXPECT_TRUE(outbox_->DueForRetry(9999999999999LL, 10).empty());
}

TEST_F(OutboxTest, FailedIsRetriedLater) {
  outbox_->Put(MakeRecord("c1:m1:uid"));
  EXPECT_TRUE(outbox_->MarkFailed("c1:m1:uid", "enqueue failed", 1000));
  const auto record = outbox_->Get("c1:m1:uid");
  EXPECT_EQ(record->state, infra::OutboxState::kFailed);
  EXPECT_EQ(record->last_status, "enqueue failed");
  // FAILED records come back once next_retry_at passes.
  EXPECT_EQ(outbox_->DueForRetry(9999999999999LL, 10).size(), 1u);
}

TEST_F(OutboxTest, DeadLetterOnPermanentError) {
  outbox_->Put(MakeRecord("c1:m1:uid"));
  EXPECT_TRUE(outbox_->MarkDeadLetter("c1:m1:uid", "INVALID_ARGUMENT: bad proto"));
  const auto record = outbox_->Get("c1:m1:uid");
  EXPECT_EQ(record->state, infra::OutboxState::kDeadLetter);
  EXPECT_TRUE(outbox_->DueForRetry(9999999999999LL, 10).empty());
}

TEST_F(OutboxTest, ReputReArmsForReplay) {
  // Replay after reconnect: same image_id re-enqueued by the sweep; server
  // dedupes on image_id, so INSERT-or-update on the same key is safe.
  outbox_->Put(MakeRecord("c1:m1:uid"));
  outbox_->MarkDone("c1:m1:uid", "done");
  auto record = MakeRecord("c1:m1:uid");
  record.attempts = 2;
  outbox_->Put(record);
  const auto fetched = outbox_->Get("c1:m1:uid");
  ASSERT_TRUE(fetched.has_value());
  EXPECT_EQ(fetched->state, infra::OutboxState::kPending);
  EXPECT_EQ(fetched->attempts, 2);
}

TEST_F(OutboxTest, PurgeRemovesOnlyOldDoneRecords) {
  outbox_->Put(MakeRecord("done:old"));
  outbox_->Put(MakeRecord("done:new"));
  outbox_->Put(MakeRecord("pending:one"));
  outbox_->MarkDone("done:old", "ok");
  outbox_->MarkDone("done:new", "ok");

  // Cutoff far in the future removes ALL done records but not pending.
  outbox_->PurgeDoneBefore(9999999999999LL);
  EXPECT_FALSE(outbox_->Get("done:old").has_value());
  EXPECT_FALSE(outbox_->Get("done:new").has_value());
  EXPECT_TRUE(outbox_->Get("pending:one").has_value());
}

TEST_F(OutboxTest, PersistsAcrossReopen) {
  // A REAL legacy id ("<chat>:<message_id>:<uid>" — the middle segment was
  // always numeric): the reopen below runs MigrateLegacyImageIds, which
  // rewrites the row to the wire format "456" and keeps it pending. A
  // non-numeric middle would be dead-lettered instead (see
  // OutboxLegacyMigration.UnparseableLegacyRowDeadLetters).
  outbox_->Put(MakeRecord("-100123:456:uid"));
  outbox_->MarkRequeued("-100123:456:uid", 12345, 5, "held");
  outbox_.reset();

  infra::Outbox reopened(path_);
  const auto record = reopened.Get("456");
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->state, infra::OutboxState::kPending);
  EXPECT_EQ(record->attempts, 1);
  EXPECT_EQ(record->last_status, "held");
}

TEST_F(OutboxTest, CountByState) {
  outbox_->Put(MakeRecord("a"));
  outbox_->Put(MakeRecord("b"));
  outbox_->Put(MakeRecord("c"));
  outbox_->MarkDone("a", "ok");
  outbox_->MarkDeadLetter("b", "bad");
  EXPECT_EQ(outbox_->CountByState(infra::OutboxState::kPending), 1);
  EXPECT_EQ(outbox_->CountByState(infra::OutboxState::kDone), 1);
  EXPECT_EQ(outbox_->CountByState(infra::OutboxState::kDeadLetter), 1);
}

TEST_F(OutboxTest, OpensInvalidPathThrows) {
  EXPECT_THROW(infra::Outbox("/nonexistent-dir-xyz/outbox.sqlite"),
               std::runtime_error);
}

// --- legacy image_id migration (Telethon migration Chunk 2) ---------------------
//
// The Telethon client's outbox keyed rows "<chat>:<message_id>:<uid>"; the
// C++ wire format is str(message_id). On open, rows carrying ':' are
// rewritten to the message id, dropped on collision with an existing
// current-format row, or dead-lettered when unparseable — idempotently.

namespace {

std::string MigrationDbPath() {
  return "/tmp/tg_client_outbox_migration_" + std::to_string(getpid()) +
         ".sqlite";
}

infra::OutboxRecord MakeRecordWith(const std::string& image_id,
                                   const std::string& data) {
  infra::OutboxRecord record = MakeRecord(image_id);
  record.image_data = data;
  return record;
}

}  // namespace

TEST(OutboxLegacyMigration, RewritesLegacyIdsToMessageIds) {
  const std::string path = MigrationDbPath();
  std::remove(path.c_str());
  {
    infra::Outbox writer(path);  // writes rows as-is (migration not run yet)
    writer.Put(MakeRecordWith("c1:120:uidA", "payload-a"));
    writer.Put(MakeRecordWith("c1:150:uidB", "payload-b"));
  }
  // The upgrade: this open runs MigrateLegacyImageIds.
  infra::Outbox migrated(path);
  EXPECT_EQ(migrated.PendingCount(), 2);
  const auto a = migrated.Get("120");
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->image_data, "payload-a");
  const auto b = migrated.Get("150");
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->image_data, "payload-b");
  EXPECT_EQ(migrated.Get("c1:120:uidA"), std::nullopt);
  std::remove(path.c_str());
}

TEST(OutboxLegacyMigration, CollisionKeepsCurrentFormatRow) {
  const std::string path = MigrationDbPath();
  std::remove(path.c_str());
  {
    infra::Outbox writer(path);
    writer.Put(MakeRecordWith("120", "current-row"));
    writer.Put(MakeRecordWith("c1:120:uidX", "legacy-row"));
  }
  infra::Outbox migrated(path);
  EXPECT_EQ(migrated.PendingCount(), 1);  // legacy row deleted, current kept
  const auto kept = migrated.Get("120");
  ASSERT_TRUE(kept.has_value());
  EXPECT_EQ(kept->image_data, "current-row");
  EXPECT_EQ(migrated.Get("c1:120:uidX"), std::nullopt);
  std::remove(path.c_str());
}

TEST(OutboxLegacyMigration, ConvertedLegacyRowsDedupeAmongThemselves) {
  const std::string path = MigrationDbPath();
  std::remove(path.c_str());
  {
    infra::Outbox writer(path);
    writer.Put(MakeRecordWith("c1:120:uidA", "first"));
    writer.Put(MakeRecordWith("c2:120:uidB", "second"));  // same message id
  }
  infra::Outbox migrated(path);
  EXPECT_EQ(migrated.PendingCount(), 1);
  const auto kept = migrated.Get("120");
  ASSERT_TRUE(kept.has_value());
  EXPECT_EQ(kept->image_data, "first");
  std::remove(path.c_str());
}

TEST(OutboxLegacyMigration, UnparseableLegacyRowDeadLetters) {
  const std::string path = MigrationDbPath();
  std::remove(path.c_str());
  {
    infra::Outbox writer(path);
    writer.Put(MakeRecordWith("c1:abc:uid", "junk-middle"));
    writer.Put(MakeRecordWith("no-colon-id", "not legacy"));
    writer.Put(MakeRecordWith("c1:120", "two segments"));
  }
  infra::Outbox migrated(path);
  // Non-digit middle and missing middle: dead-lettered for inspection.
  const auto junk = migrated.Get("c1:abc:uid");
  ASSERT_TRUE(junk.has_value());
  EXPECT_EQ(junk->state, infra::OutboxState::kDeadLetter);
  EXPECT_NE(junk->last_status.find("legacy image_id unparseable"),
            std::string::npos);
  const auto two = migrated.Get("c1:120");
  ASSERT_TRUE(two.has_value());
  EXPECT_EQ(two->state, infra::OutboxState::kDeadLetter);
  // A row without any ':' is NOT legacy — untouched, still pending.
  const auto clean = migrated.Get("no-colon-id");
  ASSERT_TRUE(clean.has_value());
  EXPECT_EQ(clean->state, infra::OutboxState::kPending);
  std::remove(path.c_str());
}

TEST(OutboxLegacyMigration, MigrationIsIdempotentOnReopen) {
  const std::string path = MigrationDbPath();
  std::remove(path.c_str());
  {
    infra::Outbox writer(path);
    writer.Put(MakeRecordWith("c1:120:uidA", "payload-a"));
    writer.Put(MakeRecordWith("c1:abc:uid", "junk"));
  }
  infra::Outbox first(path);
  ASSERT_EQ(first.PendingCount(), 1);          // converted
  ASSERT_TRUE(first.Get("c1:abc:uid").has_value());  // dead-lettered row kept
  {
    infra::Outbox second(path);  // second open: nothing left to migrate
    EXPECT_EQ(second.PendingCount(), 1);
  }
  EXPECT_TRUE(first.Get("120").has_value());
  std::remove(path.c_str());
}
