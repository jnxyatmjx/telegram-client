// DialogScanner (Telethon migration Chunk 2; Python handlersm 838-916 /
// scan_dialogs 1294-1317): the ChatView finding/link model and the
// loadChats-until-404 -> getChats -> per-dialog resolution chain, including
// the kAudit supergroup probe, migrated basic groups and FloodWait retries.
#include <app/AppContext.h>
#include <app/WorkerPool.h>
#include <features/commands/DialogScanner.h>
#include <infra/Outbox.h>
#include <telegram/TelegramClient.h>
#include <transport/AsyncStreamer.h>

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace td_api = td::td_api;

namespace app = tg::app;
namespace commands = tg::features::commands;
namespace infra = tg::infra;
namespace transport = tg::transport;

namespace {

class FakeTdSender : public tg::telegram::ITdSender {
 public:
  // --- programming --------------------------------------------------------
  int load_chats_ok_calls = 1;  // loadChats answers Ok this often, then 404
  std::string getChats_error;   // non-empty: hard-fail getChats with this
  std::vector<std::int64_t> chat_list;
  struct ChatSpec {
    std::string title;
    int kind = 0;  // 0 supergroup, 1 basic group, 2 private, 3 secret
    std::int64_t peer_id = 0;
  };
  std::map<std::int64_t, ChatSpec> chats;  // dialog id -> spec

  struct SupergroupSpec {
    std::string username;
    bool is_channel = false;
    bool scam = false;
    bool fake = false;
    bool left = false;
    bool banned = false;
    std::string restriction_reason;  // non-empty => restricted
    std::string probe_error;  // non-empty => getSupergroupFullInfo errs
                              // with 400 + this message
  };
  std::map<std::int64_t, SupergroupSpec> supergroups;  // bare id -> spec

  struct UserSpec {
    std::string first_name;
    std::string username;
  };
  std::map<std::int64_t, UserSpec> users;  // user id -> spec

  struct BasicGroupSpec {
    bool is_active = true;
    bool left = false;
    bool banned = false;
    std::int64_t upgraded_to_supergroup_id = 0;
  };
  std::map<std::int64_t, BasicGroupSpec> basic_groups;  // bare id -> spec

  std::int64_t flood_supergroup_id = 0;  // 429 once before the real answer
  int flood_left = 0;

  // --- counters ------------------------------------------------------------
  int sent_loadChats = 0;
  int sent_getChats = 0;
  int sent_getSupergroup = 0;
  int sent_getSupergroupFullInfo = 0;

  td_api::object_ptr<td_api::usernames> MakeUsernames(
      const std::string& username) const {
    return td_api::make_object<td_api::usernames>(
        username.empty() ? std::vector<std::string>{}
                         : std::vector<std::string>{username},
        std::vector<std::string>{}, "", std::vector<std::string>{}, "");
  }

  td_api::object_ptr<td_api::ChatMemberStatus> MakeStatus(bool left,
                                                          bool banned) const {
    if (banned) return td_api::make_object<td_api::chatMemberStatusBanned>();
    if (left) return td_api::make_object<td_api::chatMemberStatusLeft>();
    return td_api::make_object<td_api::chatMemberStatusMember>();
  }

  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback) override {
    td::ClientManager::Response response;
    response.request_id = next_id_++;
    switch (request->get_id()) {
      case td_api::loadChats::ID: {
        ++sent_loadChats;
        if (load_chats_ok_calls > 0) {
          --load_chats_ok_calls;
          response.object = td_api::make_object<td_api::ok>();
        } else {
          response.object =
              td_api::make_object<td_api::error>(404, "Not Found");
        }
        break;
      }
      case td_api::getChats::ID: {
        ++sent_getChats;
        if (!getChats_error.empty()) {
          // Split "code: message" — only the message part goes to error().
          const auto colon = getChats_error.find(": ");
          response.object = td_api::make_object<td_api::error>(
              std::stoi(getChats_error.substr(0, colon)),
              getChats_error.substr(colon + 2));
          break;
        }
        auto result = td_api::make_object<td_api::chats>();
        result->total_count_ = static_cast<std::int32_t>(chat_list.size());
        result->chat_ids_ = chat_list;
        response.object = std::move(result);
        break;
      }
      case td_api::getChat::ID: {
        const auto chat_id =
            static_cast<const td_api::getChat&>(*request).chat_id_;
        const auto it = chats.find(chat_id);
        if (it == chats.end()) {
          response.object =
              td_api::make_object<td_api::error>(400, "Chat not found");
          break;
        }
        auto chat = td_api::make_object<td_api::chat>();
        chat->id_ = chat_id;
        chat->title_ = it->second.title;
        switch (it->second.kind) {
          case 0:
            chat->type_ = td_api::make_object<td_api::chatTypeSupergroup>(
                it->second.peer_id, false);
            break;
          case 1:
            chat->type_ = td_api::make_object<td_api::chatTypeBasicGroup>(
                it->second.peer_id);
            break;
          case 2:
            chat->type_ =
                td_api::make_object<td_api::chatTypePrivate>(
                    it->second.peer_id);
            break;
          default:
            chat->type_ = td_api::make_object<td_api::chatTypeSecret>(
                it->second.peer_id, 0);
        }
        response.object = std::move(chat);
        break;
      }
      case td_api::getSupergroup::ID: {
        const auto supergroup_id =
            static_cast<const td_api::getSupergroup&>(*request)
                .supergroup_id_;
        ++sent_getSupergroup;
        if (supergroup_id == flood_supergroup_id && flood_left > 0) {
          --flood_left;
          response.object = td_api::make_object<td_api::error>(
              429, "Too Many Requests: retry after 0");
          break;
        }
        const auto it = supergroups.find(supergroup_id);
        if (it == supergroups.end()) {
          response.object =
              td_api::make_object<td_api::error>(400, "Supergroup not found");
          break;
        }
        auto group = td_api::make_object<td_api::supergroup>();
        group->id_ = supergroup_id;
        group->usernames_ = MakeUsernames(it->second.username);
        group->is_channel_ = it->second.is_channel;
        group->status_ = MakeStatus(it->second.left, it->second.banned);
        if (it->second.scam || it->second.fake) {
          auto verification = td_api::make_object<td_api::verificationStatus>(
              false, it->second.scam, it->second.fake, 0);
          group->verification_status_ = std::move(verification);
        }
        if (!it->second.restriction_reason.empty()) {
          auto restriction = td_api::make_object<td_api::restrictionInfo>(
              it->second.restriction_reason, false);
          group->restriction_info_ = std::move(restriction);
        }
        response.object = std::move(group);
        break;
      }
      case td_api::getUser::ID: {
        const auto user_id =
            static_cast<const td_api::getUser&>(*request).user_id_;
        const auto it = users.find(user_id);
        if (it == users.end()) {
          response.object =
              td_api::make_object<td_api::error>(400, "User not found");
          break;
        }
        auto user = td_api::make_object<td_api::user>();
        user->id_ = user_id;
        user->first_name_ = it->second.first_name;
        user->usernames_ = MakeUsernames(it->second.username);
        response.object = std::move(user);
        break;
      }
      case td_api::getBasicGroup::ID: {
        const auto group_id =
            static_cast<const td_api::getBasicGroup&>(*request)
                .basic_group_id_;
        const auto it = basic_groups.find(group_id);
        if (it == basic_groups.end()) {
          response.object =
              td_api::make_object<td_api::error>(400, "Group not found");
          break;
        }
        auto group = td_api::make_object<td_api::basicGroup>();
        group->id_ = group_id;
        group->is_active_ = it->second.is_active;
        group->status_ = MakeStatus(it->second.left, it->second.banned);
        group->upgraded_to_supergroup_id_ =
            it->second.upgraded_to_supergroup_id;
        response.object = std::move(group);
        break;
      }
      case td_api::getSupergroupFullInfo::ID: {
        const auto supergroup_id =
            static_cast<const td_api::getSupergroupFullInfo&>(*request)
                .supergroup_id_;
        ++sent_getSupergroupFullInfo;
        const auto it = supergroups.find(supergroup_id);
        if (it != supergroups.end() && !it->second.probe_error.empty()) {
          response.object = td_api::make_object<td_api::error>(
              400, it->second.probe_error);
          break;
        }
        response.object = td_api::make_object<td_api::supergroupFullInfo>();
        break;
      }
      default:
        response.object =
            td_api::make_object<td_api::error>(400, "unexpected");
    }
    callback(std::move(response));
    return static_cast<std::int64_t>(response.request_id);
  }

 private:
  std::uint64_t next_id_ = 1;
};

// Dialog ids: supergroup chats are -100<bare>, basic groups -<bare>, private
// chats the bare user id.
constexpr std::int64_t SgDialog(std::int64_t bare) { return -1'000'000'000'000LL - bare; }

bool WaitFor(const std::function<bool()>& condition) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

}  // namespace

class DialogScanTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const std::string outbox_path =
        "/tmp/tg_client_dialog_scan_outbox_" + std::to_string(getpid()) +
        ".sqlite";
    std::remove(outbox_path.c_str());
    outbox_ = std::make_unique<infra::Outbox>(outbox_path);
    files_ = std::make_unique<tg::telegram::FileDownloader>(sender_);
    workers_ = std::make_unique<app::WorkerPool>(4u);
    process_streamer_ = std::make_unique<
        transport::StreamerPool<ImageRequest, ImageReply>>(
        std::vector<transport::ProcessImagesStreamer::Options>{
            transport::ProcessImagesStreamer::Options{"ProcessImages"}});
    search_streamer_ = std::make_unique<
        transport::StreamerPool<SearchReq, SearchRsp>>(
        std::vector<transport::SearchImagesStreamer::Options>{
            transport::SearchImagesStreamer::Options{"SearchImages"}});
    ctx_ = std::make_unique<app::AppContext>(app::AppContext{
        *workers_, sender_, *files_, *process_streamer_, *search_streamer_,
        *outbox_, config_});
  }

  void TearDown() override {
    scanner_.reset();
    ctx_.reset();
    workers_.reset();
    search_streamer_.reset();
    process_streamer_.reset();
    files_.reset();
    outbox_.reset();
  }

  void MakeScanner(commands::DialogScanner::Config config) {
    scanner_ = std::make_unique<commands::DialogScanner>(sender_, config);
  }

  // Runs one scan to completion; returns the delivered views (empty on
  // abort). Fails the test if the scan does not finish in time.
  std::vector<commands::ChatView> ScanAndWait(commands::DialogScanner::Mode mode) {
    std::mutex mutex;
    std::vector<commands::ChatView> delivered;
    bool done = false;
    const bool started =
        scanner_->Scan(*ctx_, mode, [&](std::vector<commands::ChatView> views) {
          std::lock_guard<std::mutex> lock(mutex);
          delivered = std::move(views);
          done = true;
        });
    EXPECT_TRUE(started);
    EXPECT_TRUE(WaitFor([&] {
      std::lock_guard<std::mutex> lock(mutex);
      return done;
    }));
    std::lock_guard<std::mutex> lock(mutex);
    return delivered;
  }

  std::string outbox_path_;
  app::Config config_;
  FakeTdSender sender_;
  std::unique_ptr<tg::telegram::FileDownloader> files_;
  std::unique_ptr<app::WorkerPool> workers_;
  std::unique_ptr<transport::StreamerPool<ImageRequest, ImageReply>>
      process_streamer_;
  std::unique_ptr<transport::StreamerPool<SearchReq, SearchRsp>>
      search_streamer_;
  std::unique_ptr<infra::Outbox> outbox_;
  std::unique_ptr<app::AppContext> ctx_;
  std::unique_ptr<commands::DialogScanner> scanner_;
};

// --- ChatView finding/link model -----------------------------------------------

TEST(ChatViewModel, HasFindingAndReasonsMatchPythonWording) {
  commands::ChatView banned;
  banned.kind = commands::ChatView::Kind::kChannel;
  banned.banned = true;
  EXPECT_TRUE(banned.HasFinding());
  EXPECT_EQ(banned.FindingReason(), "已被封禁/删除（Forbidden）");

  commands::ChatView deactivated;
  deactivated.kind = commands::ChatView::Kind::kBasicGroup;
  deactivated.deactivated = true;
  EXPECT_TRUE(deactivated.HasFinding());
  EXPECT_EQ(deactivated.FindingReason(), "基础群已停用（deactivated）");

  commands::ChatView migrated;
  migrated.kind = commands::ChatView::Kind::kBasicGroup;
  migrated.migrated = true;
  EXPECT_TRUE(migrated.HasFinding());
  EXPECT_EQ(migrated.FindingReason(), "已迁移为超级群（migrated_to）");

  commands::ChatView group_left;
  group_left.kind = commands::ChatView::Kind::kBasicGroup;
  group_left.left = true;
  EXPECT_TRUE(group_left.HasFinding());
  EXPECT_EQ(group_left.FindingReason(), "已退出（left）");

  commands::ChatView scam;
  scam.kind = commands::ChatView::Kind::kChannel;
  scam.scam = true;
  EXPECT_TRUE(scam.HasFinding());
  EXPECT_EQ(scam.FindingReason(), "被标记为诈骗（scam）");

  commands::ChatView fake;
  fake.kind = commands::ChatView::Kind::kChannel;
  fake.fake = true;
  EXPECT_TRUE(fake.HasFinding());
  EXPECT_EQ(fake.FindingReason(), "被标记为仿冒（fake）");

  commands::ChatView channel_left;
  channel_left.kind = commands::ChatView::Kind::kChannel;
  channel_left.left = true;
  EXPECT_TRUE(channel_left.HasFinding());
  EXPECT_EQ(channel_left.FindingReason(), "已退出（left）");

  commands::ChatView probe_private;
  probe_private.kind = commands::ChatView::Kind::kChannel;
  probe_private.probe_failed = true;
  probe_private.probe_error = "400: CHANNEL_PRIVATE";
  EXPECT_TRUE(probe_private.HasFinding());
  EXPECT_EQ(probe_private.FindingReason(),
            "频道私有/已失效（ChannelPrivateError）");

  commands::ChatView probe_other;
  probe_other.kind = commands::ChatView::Kind::kChannel;
  probe_other.probe_failed = true;
  probe_other.probe_error = "500: boom";
  EXPECT_TRUE(probe_other.HasFinding());
  EXPECT_EQ(probe_other.FindingReason(), "检测出错：500: boom");

  commands::ChatView healthy;
  healthy.kind = commands::ChatView::Kind::kChannel;
  healthy.username = "deals";
  EXPECT_FALSE(healthy.HasFinding());
  EXPECT_EQ(healthy.FindingReason(), "");
}

TEST(ChatViewModel, LinkMatchesPythonBuildLink) {
  commands::ChatView with_username;
  with_username.kind = commands::ChatView::Kind::kChannel;
  with_username.username = "deals";
  EXPECT_EQ(with_username.Link(), "https://t.me/deals");

  commands::ChatView private_channel;
  private_channel.kind = commands::ChatView::Kind::kChannel;
  private_channel.bare_id = 1'234'567'890;
  EXPECT_EQ(private_channel.Link(), "https://t.me/c/1234567890");

  commands::ChatView private_group;
  private_group.kind = commands::ChatView::Kind::kBasicGroup;
  EXPECT_EQ(private_group.Link(), "（私有群，无公开链接）");

  commands::ChatView user;
  user.kind = commands::ChatView::Kind::kUser;
  user.username = "someone";
  EXPECT_EQ(user.Link(), "https://t.me/someone");
}

// --- DialogScanner flow ----------------------------------------------------

TEST_F(DialogScanTest, ListScanResolvesAllChatKinds) {
  const std::int64_t channel_bare = 2'100'000'001;
  const std::int64_t group_bare = 3'100'000'002;
  const std::int64_t user_id = 4'100'000'003;
  sender_.load_chats_ok_calls = 1;
  sender_.chat_list = {SgDialog(channel_bare), -group_bare, user_id};
  sender_.chats[SgDialog(channel_bare)] = {"Deals", 0, channel_bare};
  sender_.chats[-group_bare] = {"Group", 1, group_bare};
  sender_.chats[user_id] = {"Alice", 2, user_id};
  sender_.supergroups[channel_bare] = {"dealchannel", true, false, false,
                                       false, false, "", ""};
  sender_.basic_groups[group_bare] = {true, false, false, 0};
  sender_.users[user_id] = {"Alice", "alice_h"};
  MakeScanner(commands::DialogScanner::Config{});

  const auto views = ScanAndWait(commands::DialogScanner::Mode::kList);
  ASSERT_EQ(views.size(), 3u);

  // Enumeration order preserved.
  ASSERT_EQ(views[0].kind, commands::ChatView::Kind::kChannel);
  EXPECT_EQ(views[0].bare_id, channel_bare);
  EXPECT_EQ(views[0].title, "Deals");
  EXPECT_EQ(views[0].username, "dealchannel");
  EXPECT_TRUE(views[0].is_channel);

  ASSERT_EQ(views[1].kind, commands::ChatView::Kind::kBasicGroup);
  EXPECT_EQ(views[1].bare_id, group_bare);
  EXPECT_FALSE(views[1].deactivated);
  EXPECT_FALSE(views[1].HasFinding());

  ASSERT_EQ(views[2].kind, commands::ChatView::Kind::kUser);
  EXPECT_EQ(views[2].bare_id, user_id);
  EXPECT_EQ(views[2].first_name, "Alice");
  EXPECT_EQ(views[2].username, "alice_h");

  // kList never probes.
  EXPECT_EQ(sender_.sent_getSupergroupFullInfo, 0);
  EXPECT_FALSE(scanner_->running());
}

TEST_F(DialogScanTest, AuditProbesSupergroupsAndReportsFindings) {
  const std::int64_t ok_bare = 2'100'000'001;
  const std::int64_t dead_bare = 2'100'000'002;
  sender_.load_chats_ok_calls = 1;
  sender_.chat_list = {SgDialog(ok_bare), SgDialog(dead_bare)};
  sender_.chats[SgDialog(ok_bare)] = {"Deals", 0, ok_bare};
  sender_.chats[SgDialog(dead_bare)] = {"Dead", 0, dead_bare};
  sender_.supergroups[ok_bare] = {"dealchannel", true, false, false, false,
                                  false, "", ""};
  sender_.supergroups[dead_bare] = {"deadchannel", true, false, false, false,
                                    false, "", "CHANNEL_PRIVATE"};
  MakeScanner(commands::DialogScanner::Config{});

  const auto views = ScanAndWait(commands::DialogScanner::Mode::kAudit);
  ASSERT_EQ(views.size(), 2u);
  EXPECT_EQ(sender_.sent_getSupergroupFullInfo, 2);  // one probe per channel

  EXPECT_FALSE(views[0].HasFinding());
  ASSERT_TRUE(views[1].HasFinding());
  EXPECT_TRUE(views[1].probe_failed);
  EXPECT_EQ(views[1].probe_error, "400: CHANNEL_PRIVATE");
  EXPECT_EQ(views[1].FindingReason(),
            "频道私有/已失效（ChannelPrivateError）");
}

TEST_F(DialogScanTest, MigratedGroupListsAsItsSupergroup) {
  const std::int64_t old_group_bare = 3'100'000'002;
  const std::int64_t new_channel_bare = 2'100'000'009;
  sender_.load_chats_ok_calls = 1;
  sender_.chat_list = {-old_group_bare};  // only the OLD dialog is enumerated
  sender_.chats[-old_group_bare] = {"Old Group", 1, old_group_bare};
  sender_.basic_groups[old_group_bare] = {true, false, false, new_channel_bare};
  // The migrated re-entry resolves the NEW dialog id.
  sender_.chats[SgDialog(new_channel_bare)] = {"New Channel", 0,
                                               new_channel_bare};
  sender_.supergroups[new_channel_bare] = {"newchannel", true, false, false,
                                           false, false, "", ""};
  MakeScanner(commands::DialogScanner::Config{});

  const auto views = ScanAndWait(commands::DialogScanner::Mode::kList);
  ASSERT_EQ(views.size(), 1u);
  EXPECT_EQ(views[0].kind, commands::ChatView::Kind::kChannel);
  EXPECT_EQ(views[0].bare_id, new_channel_bare);
  EXPECT_EQ(views[0].title, "New Channel");
  EXPECT_EQ(views[0].username, "newchannel");
  EXPECT_EQ(sender_.sent_getSupergroup, 1);  // the new supergroup, not the old
}

TEST_F(DialogScanTest, FloodWaitRetriesTheSameStep) {
  const std::int64_t channel_bare = 2'100'000'001;
  sender_.load_chats_ok_calls = 1;
  sender_.chat_list = {SgDialog(channel_bare)};
  sender_.chats[SgDialog(channel_bare)] = {"Deals", 0, channel_bare};
  sender_.supergroups[channel_bare] = {"dealchannel", true, false, false,
                                       false, false, "", ""};
  sender_.flood_supergroup_id = channel_bare;
  sender_.flood_left = 1;  // first getSupergroup: 429, then the real answer
  MakeScanner(commands::DialogScanner::Config{1202, /* flood_pad_seconds */ 0});

  const auto views = ScanAndWait(commands::DialogScanner::Mode::kList);
  ASSERT_EQ(views.size(), 1u);
  EXPECT_EQ(views[0].username, "dealchannel");
  EXPECT_EQ(sender_.sent_getSupergroup, 2);  // flood + retry of the same step
}

TEST_F(DialogScanTest, EnumerationFailureAbortsScanWithEmptyResult) {
  sender_.load_chats_ok_calls = 0;  // loadChats 404 right away: all loaded
  sender_.getChats_error = "500: internal";  // …then getChats hard-fails
  MakeScanner(commands::DialogScanner::Config{});

  const auto views = ScanAndWait(commands::DialogScanner::Mode::kList);
  EXPECT_TRUE(views.empty());
  EXPECT_FALSE(scanner_->running());
}

TEST_F(DialogScanTest, UnnamedAndBrokenChatsAreSkipped) {
  const std::int64_t channel_bare = 2'100'000'001;
  sender_.load_chats_ok_calls = 1;
  sender_.chat_list = {
      9'999,                   // getChat fails: unknown dialog -> skipped
      SgDialog(channel_bare),  // healthy channel
      -777'777,                // unnamed private chat -> skipped
  };
  sender_.chats[SgDialog(channel_bare)] = {"Deals", 0, channel_bare};
  sender_.chats[-777'777] = {"", 2, 777'777};  // banned/deleted user
  sender_.supergroups[channel_bare] = {"dealchannel", true, false, false,
                                       false, false, "", ""};
  MakeScanner(commands::DialogScanner::Config{});

  const auto views = ScanAndWait(commands::DialogScanner::Mode::kList);
  ASSERT_EQ(views.size(), 1u);
  EXPECT_EQ(views[0].bare_id, channel_bare);
}

TEST_F(DialogScanTest, SecondScanRefusedWhileRunning) {
  // A dialog list that resolves slowly: getChat answers only after the gate.
  class GatedSender : public FakeTdSender {
   public:
    std::atomic<int> gate{0};  // getChat waits until gate > 0
    std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                      tg::telegram::ResponseCallback callback) override {
      if (request->get_id() == td_api::getChat::ID) {
        while (gate.load() == 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
      }
      return FakeTdSender::Send(std::move(request), std::move(callback));
    }
  };
  GatedSender gated;
  commands::DialogScanner scanner(gated, commands::DialogScanner::Config{});
  const std::int64_t channel_bare = 2'100'000'001;
  gated.load_chats_ok_calls = 1;
  gated.chat_list = {SgDialog(channel_bare)};
  gated.chats[SgDialog(channel_bare)] = {"Deals", 0, channel_bare};
  gated.supergroups[channel_bare] = {"dealchannel", true, false, false, false,
                                     false, "", ""};

  bool done = false;
  ASSERT_TRUE(scanner.Scan(*ctx_, commands::DialogScanner::Mode::kList,
                           [&](std::vector<commands::ChatView>) { done = true; }));
  // Enumeration is underway; a second concurrent scan must be refused.
  EXPECT_FALSE(scanner.Scan(*ctx_, commands::DialogScanner::Mode::kAudit,
                            [](std::vector<commands::ChatView>) {}));
  gated.gate.store(1);
  EXPECT_TRUE(WaitFor([&] { return done; }));
  // After completion, scanning works again.
  bool second_done = false;
  ASSERT_TRUE(scanner.Scan(*ctx_, commands::DialogScanner::Mode::kList,
                           [&](std::vector<commands::ChatView>) { second_done = true; }));
  EXPECT_TRUE(WaitFor([&] { return second_done; }));
}
