// Bare-ID conversion, image_id formation, SHA-256 vectors.
#include <common/Idempotency.h>
#include <infra/Sha256.h>

#include <gtest/gtest.h>
#include <limits>

namespace common = tg::common;
using common::PeerType;

TEST(BareId, UserDialogId) {
  const auto id = common::ToBareChatId(123456789);
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->oid, 123456789u);
  EXPECT_EQ(id->type, PeerType::kUser);
}

TEST(BareId, BasicGroupDialogId) {
  const auto id = common::ToBareChatId(-123456);
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->oid, 123456u);
  EXPECT_EQ(id->type, PeerType::kBasicGroup);
}

TEST(BareId, SupergroupDialogId) {
  // Bot API: -(1'000'000'000'000 + 456) -> bare 456
  const auto id = common::ToBareChatId(-1'000'000'000'456LL);
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->oid, 456u);
  EXPECT_EQ(id->type, PeerType::kSupergroup);
}

TEST(BareId, ChannelDialogId) {
  const auto id = common::ToBareChatId(-1'000'429'637'385LL);
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->oid, 429637385u);
  EXPECT_EQ(id->type, PeerType::kSupergroup);
}

TEST(BareId, ZeroIsNotADialog) {
  EXPECT_FALSE(common::ToBareChatId(0).has_value());
}

TEST(BareId, SupergroupMarkAloneHasBareZero) {
  // chat_id == -1e12 would map to bare 0, which is not a valid id.
  EXPECT_FALSE(common::ToBareChatId(-1'000'000'000'000LL).has_value());
}

TEST(BareId, UserOutOfRangeIsRejectedNotTruncated) {
  // 2^32 would silently truncate to 0 — must be rejected.
  EXPECT_FALSE(common::ToBareChatId(4'294'967'296LL).has_value());
}

TEST(BareId, BasicGroupMagnitudeOutOfRangeIsRejected) {
  // -2^32 -> bare 2^32 > uint32 max.
  EXPECT_FALSE(common::ToBareChatId(-4'294'967'296LL).has_value());
}

TEST(BareId, SupergroupBareOutOfRangeIsRejected) {
  // bare = 4'296'373'850 > uint32 max -> reject, never truncate.
  EXPECT_FALSE(common::ToBareChatId(-10'004'296'373'850LL).has_value());
}

TEST(BareId, Int64MinDoesNotOverflow) {
  const auto id = common::ToBareChatId(std::numeric_limits<std::int64_t>::min());
  EXPECT_FALSE(id.has_value());  // magnitude >> uint32 max
}

// ToBareChatId64: same unmarking as ToBareChatId, but the bare id is
// int64 — Telegram user ids above 2^32 exist and must still send as
// ImageRequest.owner_id (int64 in the proto).
TEST(BareId64, UserIdAboveUint32) {
  // Real case: a bot at 7974301646 (live test 2026-11-10) was refused by
  // the uint32-capped conversion; owner_id is int64 and must accept it.
  const auto id = common::ToBareChatId64(7'974'301'646LL);
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(*id, 7'974'301'646LL);
}

TEST(BareId64, SupergroupBareAboveUint32) {
  // chat_id = -(1e12 + 5'000'000'000) -> bare 5'000'000'000.
  const auto id = common::ToBareChatId64(-1'005'000'000'000LL);
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(*id, 5'000'000'000LL);
}

TEST(BareId64, BasicGroupDialogId) {
  const auto id = common::ToBareChatId64(-123456);
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(*id, 123456);
}

TEST(BareId64, ZeroIsNotADialog) {
  EXPECT_FALSE(common::ToBareChatId64(0).has_value());
}

TEST(BareId64, SupergroupMarkAloneHasBareZero) {
  EXPECT_FALSE(common::ToBareChatId64(-1'000'000'000'000LL).has_value());
}

TEST(BareId64, Int64MinDoesNotOverflow) {
  // chat_id == INT64_MIN -> bare = 2^63 - 1e12 = 9223371036854775808,
  // which still fits in int64 (INT64_MAX is 9223372036854775807), so it
  // round-trips without overflow.
  const auto id =
      common::ToBareChatId64(std::numeric_limits<std::int64_t>::min());
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(*id, 9'223'371'036'854'775'808LL);
}

TEST(DialogCandidates, BareIdAboveUint32StillHasCandidates) {
  const auto candidates = common::DialogIdCandidates(7'974'301'646LL);
  ASSERT_EQ(candidates.size(), 3u);
  EXPECT_EQ(candidates[2], 7'974'301'646LL);  // user form is the bare id
}

TEST(DialogCandidates, NonPositiveHasNone) {
  EXPECT_TRUE(common::DialogIdCandidates(0).empty());
  EXPECT_TRUE(common::DialogIdCandidates(-5).empty());
}

TEST(BareMessageId, AcceptsPositiveUint32Range) {
  EXPECT_EQ(common::ToBareMessageId(12345), std::optional<std::uint32_t>(12345));
  EXPECT_EQ(common::ToBareMessageId(4'294'967'295LL),
            std::optional<std::uint32_t>(4'294'967'295u));
}

TEST(BareMessageId, RejectsOutOfRange) {
  EXPECT_FALSE(common::ToBareMessageId(0).has_value());
  EXPECT_FALSE(common::ToBareMessageId(-5).has_value());
  EXPECT_FALSE(common::ToBareMessageId(4'294'967'296LL).has_value());
}

TEST(ImageId, FormationUsesDialogIdVerbatim) {
  // chat_id stays the TDLib dialog id in string form (v5 §1.2).
  EXPECT_EQ(common::MakeImageId(-1'001'234'567'890LL, 12345, "AgADxxxx"),
            "-1001234567890:12345:AgADxxxx");
  EXPECT_EQ(common::MakeImageId(42, 7, ""), "42:7:");
}

TEST(Sha256, KnownVectors) {
  EXPECT_EQ(tg::infra::Sha256::Hex(""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(tg::infra::Sha256::Hex("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(
      tg::infra::Sha256::Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
      "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, ChunkedUpdateMatchesOneShot) {
  const std::string data(1000, 'x');
  tg::infra::Sha256 incremental;
  for (size_t i = 0; i < data.size(); i += 37) {
    incremental.Update(data.data() + i, std::min(size_t(37), data.size() - i));
  }
  EXPECT_EQ(incremental.FinishHex(), tg::infra::Sha256::Hex(data));
}
