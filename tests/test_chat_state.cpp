// ChatStateStore + ImageReply.result parsing (Telethon migration Chunk 1):
// watermark only increases, {"cid","mid"} JSON contract, on-demand chat_url
// resolution via TDLib getChat/getSupergroup/getBasicGroup, placeholder on
// failure with NO retry storm, single in-flight resolution.
#include <features/chat_state/ChatStateStore.h>

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace td_api = td::td_api;

namespace chat_state = tg::features::chat_state;

namespace {

// Canned TDLib data, as plain values (objects are built fresh per request —
// td_api objects are move-only).
struct ChatInfo {
  std::string title;
  enum class Kind { kSupergroup, kBasicGroup, kPrivate } kind;
  std::int64_t type_id;  // supergroup_id / basic_group_id / user_id
};

struct SupergroupInfo {
  std::vector<std::string> active_usernames;
};

struct BasicGroupInfo {
  bool is_active = true;
  std::int64_t upgraded_to_supergroup_id = 0;
};

// Fake TDLib sender: answers getChat/getSupergroup/getBasicGroup from the
// canned maps; can defer responses (to observe in-flight resolution dedup)
// until Flush().
class FakeTdSender : public tg::telegram::ITdSender {
 public:
  std::map<std::int64_t, ChatInfo> chats;
  std::map<std::int64_t, SupergroupInfo> supergroups;
  std::map<std::int64_t, BasicGroupInfo> basic_groups;
  bool defer = false;
  int sent_getChat = 0;
  int sent_getSupergroup = 0;
  int sent_getBasicGroup = 0;

  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback) override {
    td::ClientManager::Response response;
    response.request_id = next_id_++;
    response.object = Answer(*request);
    const std::int64_t id = static_cast<std::int64_t>(response.request_id);
    if (defer) {
      pending_.emplace_back(std::move(callback), std::move(response));
    } else {
      callback(std::move(response));
    }
    return id;
  }

  void Flush() {
    defer = false;
    auto pending = std::move(pending_);
    pending_.clear();
    for (auto& [callback, response] : pending) {
      callback(std::move(response));
    }
  }

 private:
  static td_api::object_ptr<td_api::error> Err(int code, std::string text) {
    return td_api::make_object<td_api::error>(code, std::move(text));
  }

  td_api::object_ptr<td_api::Object> Answer(td_api::Function& request) {
    switch (request.get_id()) {
      case td_api::getChat::ID: {
        ++sent_getChat;
        const auto chat_id =
            static_cast<const td_api::getChat&>(request).chat_id_;
        const auto it = chats.find(chat_id);
        if (it == chats.end()) return Err(400, "Chat not found");
        const ChatInfo& info = it->second;
        auto chat = td_api::make_object<td_api::chat>();
        chat->id_ = chat_id;
        chat->title_ = info.title;
        switch (info.kind) {
          case ChatInfo::Kind::kSupergroup:
            chat->type_ = td_api::make_object<td_api::chatTypeSupergroup>(
                info.type_id, false);
            break;
          case ChatInfo::Kind::kBasicGroup:
            chat->type_ = td_api::make_object<td_api::chatTypeBasicGroup>(
                info.type_id);
            break;
          case ChatInfo::Kind::kPrivate:
            chat->type_ =
                td_api::make_object<td_api::chatTypePrivate>(info.type_id);
            break;
        }
        return chat;
      }
      case td_api::getSupergroup::ID: {
        ++sent_getSupergroup;
        const auto id =
            static_cast<const td_api::getSupergroup&>(request).supergroup_id_;
        const auto it = supergroups.find(id);
        if (it == supergroups.end()) return Err(400, "Supergroup not found");
        auto supergroup = td_api::make_object<td_api::supergroup>();
        supergroup->id_ = id;
        supergroup->usernames_ = td_api::make_object<td_api::usernames>(
            std::move(it->second.active_usernames), std::vector<std::string>{},
            "", std::vector<std::string>{}, "");
        return supergroup;
      }
      case td_api::getBasicGroup::ID: {
        ++sent_getBasicGroup;
        const auto id =
            static_cast<const td_api::getBasicGroup&>(request).basic_group_id_;
        const auto it = basic_groups.find(id);
        if (it == basic_groups.end()) return Err(400, "Basic group not found");
        return td_api::make_object<td_api::basicGroup>(
            id, 3, td_api::make_object<td_api::chatMemberStatusMember>(),
            it->second.is_active, it->second.upgraded_to_supergroup_id);
      }
      default:
        return Err(400, "unexpected request");
    }
  }

  std::uint64_t next_id_ = 1;
  std::vector<std::pair<tg::telegram::ResponseCallback,
                        td::ClientManager::Response>>
      pending_;
};

}  // namespace

// --- ParseReplyResult --------------------------------------------------------

TEST(ParseReplyResult, NumericFields) {
  const auto r = chat_state::ParseReplyResult(R"({"cid":1234567890,"mid":777})");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->cid, 1'234'567'890);
  EXPECT_EQ(r->mid, 777);
}

TEST(ParseReplyResult, StringFieldsAndExtraKeysIgnored) {
  const auto r = chat_state::ParseReplyResult(
      R"({ "memo" : "x", "mid" : "42", "cid" : "1234567890", "extra": [1,2] })");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->cid, 1'234'567'890);
  EXPECT_EQ(r->mid, 42);
}

TEST(ParseReplyResult, RejectsGarbage) {
  EXPECT_FALSE(chat_state::ParseReplyResult("").has_value());
  EXPECT_FALSE(chat_state::ParseReplyResult("not json").has_value());
  EXPECT_FALSE(chat_state::ParseReplyResult("[1,2]").has_value());
  EXPECT_FALSE(chat_state::ParseReplyResult("{}").has_value());
  // NB: raw strings need a delimiter here — the JSON's own `)"` would end a
  // plain R"(...)" literal early.
  EXPECT_FALSE(
      chat_state::ParseReplyResult(R"json({"cid":1})json").has_value());
  EXPECT_FALSE(
      chat_state::ParseReplyResult(R"json({"mid":2})json").has_value());
  EXPECT_FALSE(chat_state::ParseReplyResult(R"json({"cid":"x","mid":2})json")
                   .has_value());
  EXPECT_FALSE(chat_state::ParseReplyResult(
                   R"json({"cid":1,"mid":2})trailing)json")
                   .has_value());
  EXPECT_FALSE(
      chat_state::ParseReplyResult(R"json({"cid":1,"mid":2)json").has_value());
}

// --- Watermarks --------------------------------------------------------------

class ChatStateTest : public ::testing::Test {
 protected:
  void SetUp() override { store_ = std::make_unique<chat_state::ChatStateStore>(sender_); }

  FakeTdSender sender_;
  std::unique_ptr<chat_state::ChatStateStore> store_;
};

TEST_F(ChatStateTest, WatermarkOnlyIncreases) {
  store_->ApplyReply(1'234'567'890, 100);
  store_->ApplyReply(1'234'567'890, 50);   // older message: no regression
  store_->ApplyReply(1'234'567'890, 200);  // newer: advances
  const auto entry = store_->Get(1'234'567'890);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->offset_id, 200);
}

TEST_F(ChatStateTest, ReplyCreatesUnknownChat) {
  EXPECT_FALSE(store_->Get(42).has_value());
  store_->ApplyReply(42, 7);
  const auto entry = store_->Get(42);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->offset_id, 7);
  EXPECT_EQ(store_->size(), 1u);
}

TEST_F(ChatStateTest, InvalidIdsAreDropped) {
  store_->ApplyReply(0, 5);                    // 0 is not a bare id
  store_->ApplyReply(-100, 5);                 // negative
  store_->ApplyReply(5'000'000'000LL, 5);      // > uint32
  store_->ApplyReply(1'234'567'890, 0);        // mid <= 0
  store_->ApplyReply(1'234'567'890, -3);
  EXPECT_EQ(store_->size(), 0u);
}

// --- chat_url resolution -----------------------------------------------------

constexpr std::int64_t kBare = 1'234'567'890;       // channel bare id
constexpr std::int64_t kSgDialog = -1'001'234'567'890LL;

TEST_F(ChatStateTest, SupergroupWithUsernameGetsPublicLink) {
  sender_.chats[kSgDialog] = {"Deals", ChatInfo::Kind::kSupergroup, kBare};
  sender_.supergroups[kBare] = {{"dealchannel", "alt"}};

  store_->ApplyReply(kBare, 10);
  const auto entry = store_->Get(kBare);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->chat_url, "https://t.me/dealchannel/");  // primary username
}

TEST_F(ChatStateTest, SupergroupWithoutUsernameGetsPrivateLink) {
  sender_.chats[kSgDialog] = {"Deals", ChatInfo::Kind::kSupergroup, kBare};
  sender_.supergroups[kBare] = {};

  store_->ApplyReply(kBare, 10);
  const auto entry = store_->Get(kBare);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->chat_url, "https://t.me/c/1234567890/");
}

TEST_F(ChatStateTest, GetSupergroupFailureFallsBackToPrivateLink) {
  sender_.chats[kSgDialog] = {"Deals", ChatInfo::Kind::kSupergroup, kBare};
  // no canned supergroup -> getSupergroup errors

  store_->ApplyReply(kBare, 10);
  const auto entry = store_->Get(kBare);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->chat_url, "https://t.me/c/1234567890/");
}

TEST_F(ChatStateTest, BasicGroupGetsTitleForm) {
  sender_.chats[-500] = {"Old Group", ChatInfo::Kind::kBasicGroup, 77};
  sender_.basic_groups[77] = {true, 0};

  store_->ApplyReply(500, 10);
  const auto entry = store_->Get(500);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->chat_url, ">>Chat>Old Group");
}

TEST_F(ChatStateTest, MigratedBasicGroupFollowsSupergroup) {
  // Basic group 77 upgraded to supergroup 888 (a different bare id: the link
  // uses the NEW id, the state stays keyed by the OLD bare id 500).
  sender_.chats[-500] = {"Old Group", ChatInfo::Kind::kBasicGroup, 77};
  sender_.basic_groups[77] = {false, 888};
  sender_.supergroups[888] = {{"newplace"}};

  store_->ApplyReply(500, 10);
  const auto entry = store_->Get(500);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->chat_url, "https://t.me/newplace/");
  EXPECT_EQ(entry->offset_id, 10);  // watermark kept through resolution
}

TEST_F(ChatStateTest, PrivateChatGetsNrrorTitleForm) {
  sender_.chats[99] = {"Some User", ChatInfo::Kind::kPrivate, 99};

  store_->ApplyReply(99, 10);
  const auto entry = store_->Get(99);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->chat_url, ">>>Nrror>Some User");
}

TEST_F(ChatStateTest, UnresolvableChatGetsPlaceholderWithoutRetryStorm) {
  // Nothing canned: all three peer types fail.
  store_->ApplyReply(kBare, 10);
  const auto entry = store_->Get(kBare);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->chat_url, ">>>>Nrror>1234567890");
  EXPECT_EQ(entry->offset_id, 10);
  EXPECT_EQ(sender_.sent_getChat, 3);  // supergroup, basic group, user

  // Further replies must NOT re-resolve (Python's FloodWait-storm lesson).
  store_->ApplyReply(kBare, 20);
  EXPECT_EQ(sender_.sent_getChat, 3);
  EXPECT_EQ(store_->Get(kBare)->offset_id, 20);  // watermark still advances
}

TEST_F(ChatStateTest, SingleInFlightResolution) {
  sender_.defer = true;
  sender_.chats[kSgDialog] = {"Deals", ChatInfo::Kind::kSupergroup, kBare};
  sender_.supergroups[kBare] = {{"dealchannel"}};

  store_->ApplyReply(kBare, 10);
  store_->ApplyReply(kBare, 20);  // resolution still in flight: no second one
  EXPECT_EQ(sender_.sent_getChat, 1);

  sender_.Flush();
  EXPECT_EQ(store_->Get(kBare)->chat_url, "https://t.me/dealchannel/");
  EXPECT_EQ(store_->Get(kBare)->offset_id, 20);

  // After resolution no further TDLib traffic.
  store_->ApplyReply(kBare, 30);
  EXPECT_EQ(sender_.sent_getChat, 1);
  EXPECT_EQ(store_->Get(kBare)->offset_id, 30);
}
