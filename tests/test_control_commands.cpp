// ControlCommandsHandler (Telethon migration Chunk 2; Python handlersm
// 838-916 / handrestore 1271-1317): command classification, /rsme listing
// rendering, /rsor audit rendering, the two trigger directions (self-
// outgoing + configured bot) and the Handle wiring into scanner/backfill.
#include <app/AppContext.h>
#include <app/Config.h>
#include <app/WorkerPool.h>
#include <features/chat_state/ChatStateStore.h>
#include <features/commands/ControlCommandsHandler.h>
#include <features/commands/DialogScanner.h>
#include <features/history_backfill/HistoryBackfill.h>
#include <features/media_thumbnail/MediaExtractor.h>
#include <features/media_thumbnail/MediaThumbnailHandler.h>
#include <infra/Outbox.h>
#include <telegram/MarkdownV2.h>
#include <telegram/MessageSender.h>
#include <telegram/TelegramClient.h>
#include <transport/AsyncStreamer.h>

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace td_api = td::td_api;

namespace app = tg::app;
namespace chat_state = tg::features::chat_state;
namespace commands = tg::features::commands;
namespace common = tg::common;
namespace history_backfill = tg::features::history_backfill;
namespace media = tg::features::media_thumbnail;
namespace transport = tg::transport;

namespace {

constexpr std::int64_t kControl = 777;      // control chat
constexpr std::int64_t kBotUser = 555;      // configured command bot
constexpr std::int64_t kBare = 1'234'567'890;
constexpr std::int64_t kDialog = -1'001'234'567'890LL;

class FakeTdSender : public tg::telegram::ITdSender {
 public:
  // scanner enumeration programming
  int load_chats_ok_calls = 0;  // default: 404 immediately (empty dialog set)
  std::vector<std::int64_t> chat_list;
  struct ChatSpec {
    std::string title;
    int kind = 0;  // 0 supergroup, 1 basic group, 2 private
    std::int64_t peer_id = 0;
  };
  std::map<std::int64_t, ChatSpec> chats;
  struct SupergroupSpec {
    std::string username;
    bool is_channel = true;
  };
  std::map<std::int64_t, SupergroupSpec> supergroups;
  struct UserSpec {
    std::string first_name;
    std::string username;
  };
  std::map<std::int64_t, UserSpec> users;
  std::map<std::int64_t, std::int64_t> basic_groups_migrated_to;

  // backfill programming
  struct SearchPage {
    std::vector<std::int64_t> message_ids;
    std::int64_t next_from_message_id = 0;
  };
  std::vector<SearchPage> search_pages;
  std::string thumbnail_path;

  // blocking gate for deterministic busy-state races
  bool gate_getChat = false;
  std::atomic<int> gate{0};

  // capture
  std::vector<std::int64_t> sent_chat_ids;
  std::vector<std::string> sent_texts;
  int sent_getSupergroupFullInfo = 0;

  td_api::object_ptr<td_api::usernames> MakeUsernames(
      const std::string& username) const {
    return td_api::make_object<td_api::usernames>(
        username.empty() ? std::vector<std::string>{}
                         : std::vector<std::string>{username},
        std::vector<std::string>{}, "", std::vector<std::string>{}, "");
  }

  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback) override {
    td::ClientManager::Response response;
    response.request_id = next_id_++;
    switch (request->get_id()) {
      case td_api::getChat::ID: {
        const auto chat_id =
            static_cast<const td_api::getChat&>(*request).chat_id_;
        if (gate_getChat) {
          while (gate.load() == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
          }
        }
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
          default:
            chat->type_ = td_api::make_object<td_api::chatTypePrivate>(
                it->second.peer_id);
        }
        response.object = std::move(chat);
        break;
      }
      case td_api::loadChats::ID: {
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
        auto result = td_api::make_object<td_api::chats>();
        result->total_count_ = static_cast<std::int32_t>(chat_list.size());
        result->chat_ids_ = chat_list;
        response.object = std::move(result);
        break;
      }
      case td_api::getSupergroup::ID: {
        const auto supergroup_id =
            static_cast<const td_api::getSupergroup&>(*request)
                .supergroup_id_;
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
        group->status_ = td_api::make_object<td_api::chatMemberStatusMember>();
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
        const auto it = basic_groups_migrated_to.find(group_id);
        if (it == basic_groups_migrated_to.end()) {
          response.object =
              td_api::make_object<td_api::error>(400, "Group not found");
          break;
        }
        auto group = td_api::make_object<td_api::basicGroup>();
        group->id_ = group_id;
        group->is_active_ = true;
        group->status_ = td_api::make_object<td_api::chatMemberStatusMember>();
        group->upgraded_to_supergroup_id_ = it->second;
        response.object = std::move(group);
        break;
      }
      case td_api::getSupergroupFullInfo::ID: {
        ++sent_getSupergroupFullInfo;
        response.object = td_api::make_object<td_api::supergroupFullInfo>();
        break;
      }
      case td_api::searchChatMessages::ID: {
        SearchPage page;
        if (!search_pages.empty()) {
          page = search_pages.front();
          search_pages.erase(search_pages.begin());
        }
        auto found = td_api::make_object<td_api::foundChatMessages>();
        found->total_count_ =
            static_cast<std::int32_t>(page.message_ids.size());
        for (const std::int64_t id : page.message_ids) {
          found->messages_.push_back(MakePhotoMessage(id));
        }
        found->next_from_message_id_ = page.next_from_message_id;
        response.object = std::move(found);
        break;
      }
      case td_api::getMessage::ID: {
        const auto message_id =
            static_cast<const td_api::getMessage&>(*request).message_id_;
        response.object = MakePhotoMessage(message_id);
        break;
      }
      case td_api::sendMessage::ID: {
        const auto& send = static_cast<const td_api::sendMessage&>(*request);
        sent_chat_ids.push_back(send.chat_id_);
        const auto& content = static_cast<const td_api::inputMessageText&>(
            *send.input_message_content_);
        sent_texts.push_back(content.text_->text_);
        response.object = td_api::make_object<td_api::message>();
        break;
      }
      case td_api::downloadFile::ID: {
        auto file = td_api::make_object<td_api::file>();
        file->id_ = 4242;
        auto remote = td_api::make_object<td_api::remoteFile>();
        remote->unique_id_ = "u-ctl";
        file->remote_ = std::move(remote);
        auto local = td_api::make_object<td_api::localFile>();
        local->path_ = thumbnail_path;
        local->is_downloading_completed_ = true;
        file->local_ = std::move(local);
        response.object = std::move(file);
        break;
      }
      default:
        response.object =
            td_api::make_object<td_api::error>(400, "unexpected");
    }
    callback(std::move(response));
    return static_cast<std::int64_t>(response.request_id);
  }

  td_api::object_ptr<td_api::message> MakePhotoMessage(
      std::int64_t message_id) const {
    auto message = td_api::make_object<td_api::message>();
    message->id_ = message_id;
    message->chat_id_ = kDialog;
    auto photo = td_api::make_object<td_api::photo>();
    auto size = td_api::make_object<td_api::photoSize>();
    size->type_ = "m";
    size->width_ = 320;
    size->height_ = 240;
    auto file = td_api::make_object<td_api::file>();
    file->id_ = 4242;
    auto remote = td_api::make_object<td_api::remoteFile>();
    remote->unique_id_ = "u-ctl";
    file->remote_ = std::move(remote);
    auto local = td_api::make_object<td_api::localFile>();
    file->local_ = std::move(local);
    size->photo_ = std::move(file);
    photo->sizes_.push_back(std::move(size));
    auto content = td_api::make_object<td_api::messagePhoto>();
    content->photo_ = std::move(photo);
    message->content_ = std::move(content);
    return message;
  }

 private:
  std::uint64_t next_id_ = 1;
};

bool WaitFor(const std::function<bool()>& condition) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

td_api::object_ptr<td_api::Update> MakeTextUpdate(const std::string& text,
                                                  std::int64_t chat_id,
                                                  bool outgoing,
                                                  std::int64_t sender_user_id =
                                                      0) {
  auto message = td_api::make_object<td_api::message>();
  message->chat_id_ = chat_id;
  message->is_outgoing_ = outgoing;
  if (sender_user_id != 0) {
    auto sender = td_api::make_object<td_api::messageSenderUser>();
    sender->user_id_ = sender_user_id;
    message->sender_id_ = std::move(sender);
  }
  auto content = td_api::make_object<td_api::messageText>();
  auto formatted = td_api::make_object<td_api::formattedText>();
  formatted->text_ = text;
  content->text_ = std::move(formatted);
  message->content_ = std::move(content);
  auto update = td_api::make_object<td_api::updateNewMessage>();
  update->message_ = std::move(message);
  return update;
}

commands::ChatView MakeChannelView(const std::string& title,
                                   const std::string& username,
                                   std::int64_t bare) {
  commands::ChatView view;
  view.kind = commands::ChatView::Kind::kChannel;
  view.title = title;
  view.username = username;
  view.bare_id = bare;
  return view;
}

commands::ChatView MakeUserView(const std::string& first_name,
                                const std::string& username,
                                std::int64_t bare) {
  commands::ChatView view;
  view.kind = commands::ChatView::Kind::kUser;
  view.title = first_name;
  view.first_name = first_name;
  view.username = username;
  view.bare_id = bare;
  return view;
}

bool HasTextContaining(const FakeTdSender& sender, const std::string& needle) {
  for (const std::string& text : sender.sent_texts) {
    if (text.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

// --- statics ---------------------------------------------------------------------

TEST(ControlCommandStatics, ClassifyMatchesConfiguredCommands) {
  app::CommandsConfig config;  // defaults: /rsme /rsor /xm /xy /ym /who
  EXPECT_EQ(commands::ControlCommandsHandler::Classify("/rsme", config),
            commands::ControlCommandsHandler::Action::kRsme);
  EXPECT_EQ(commands::ControlCommandsHandler::Classify("/RSME@MyBot", config),
            commands::ControlCommandsHandler::Action::kRsme);
  EXPECT_EQ(commands::ControlCommandsHandler::Classify("/rsor", config),
            commands::ControlCommandsHandler::Action::kRsor);
  EXPECT_EQ(commands::ControlCommandsHandler::Classify("/xm", config),
            commands::ControlCommandsHandler::Action::kBackfill);
  EXPECT_EQ(commands::ControlCommandsHandler::Classify("/who", config),
            commands::ControlCommandsHandler::Action::kBackfill);
  EXPECT_EQ(commands::ControlCommandsHandler::Classify("/nope", config),
            commands::ControlCommandsHandler::Action::kIgnore);
  EXPECT_EQ(commands::ControlCommandsHandler::Classify("hello", config),
            commands::ControlCommandsHandler::Action::kIgnore);

  app::CommandsConfig custom;
  custom.rsme_command = "/list";
  custom.rsor_command = "/audit";
  custom.backfill_commands = {"/fetch"};
  EXPECT_EQ(commands::ControlCommandsHandler::Classify("/list", custom),
            commands::ControlCommandsHandler::Action::kRsme);
  EXPECT_EQ(commands::ControlCommandsHandler::Classify("/audit@Bot", custom),
            commands::ControlCommandsHandler::Action::kRsor);
  EXPECT_EQ(commands::ControlCommandsHandler::Classify("/fetch", custom),
            commands::ControlCommandsHandler::Action::kBackfill);
  EXPECT_EQ(commands::ControlCommandsHandler::Classify("/rsme", custom),
            commands::ControlCommandsHandler::Action::kIgnore);
}

TEST(ControlCommandStatics, SanitizeTitleStripsMarkdownAndWhitespace) {
  EXPECT_EQ(commands::ControlCommandsHandler::SanitizeTitle(
                "Big* Deals [2026]_ -sale| `x` y"),
            "BigDeals2026salexy");
  EXPECT_EQ(commands::ControlCommandsHandler::SanitizeTitle("plain"), "plain");
  EXPECT_EQ(commands::ControlCommandsHandler::SanitizeTitle(""), "");
}

TEST(ControlCommandStatics, RenderRsmeBatchesUsersFirstThenChats) {
  std::vector<commands::ChatView> views;
  views.push_back(MakeChannelView("Deals & Steals", "deals", 100));
  views.push_back(MakeUserView("Alice", "alice_h", 200));
  views.push_back(MakeUserView("Bob", "", 300));
  views.push_back(MakeChannelView("Priv Channel", "", 400));

  const auto batches =
      commands::ControlCommandsHandler::RenderRsmeBatches(views, 2);
  // 4 lines / 2 per batch = 2 messages, USERS first.
  ASSERT_EQ(batches.size(), 2u);
  EXPECT_EQ(batches[0],
            "1\\. [Alice](https://t.me/alice_h)\t`/ym 200 Alice`\t\n\n"
            "2\\. **🎯Bob🎯**\t`/who 300`\t\n\n");
  EXPECT_EQ(batches[1],
            "1\\. [👉👉👈](https://t.me/deals)\t`/xm 100 Deals&Steals`\t\n\n"
            "2\\. **⚠️Priv Channel⚠️**\t`/who 400 Priv Channel`\t\n\n");
}

TEST(MarkdownV2Escape, TitlesAndCodeSpans) {
  // Outside a code span every reserved character is escaped, including
  // the ones that show up in chat titles and in the client's own notices.
  EXPECT_EQ(tg::telegram::EscapeMarkdownV2("Liu Pole and Alsia Kople."),
            "Liu Pole and Alsia Kople\\.");
  EXPECT_EQ(tg::telegram::EscapeMarkdownV2("a-b (c)!"), "a\\-b \\(c\\)\\!");
  EXPECT_EQ(tg::telegram::EscapeMarkdownV2("a\\b`c_d"), "a\\\\b\\`c\\_d");
  // Inside a code span only backtick and backslash are special.
  EXPECT_EQ(tg::telegram::EscapeCodeSpan("Liu Pole and Alsia Kople."),
            "Liu Pole and Alsia Kople.");
  EXPECT_EQ(tg::telegram::EscapeCodeSpan("a-b (c)!"), "a-b (c)!");
  EXPECT_EQ(tg::telegram::EscapeCodeSpan("a\\b`c_d"), "a\\\\b\\`c_d");
}

TEST(ControlCommandStatics, RenderRsmeBatchesEscapesMarkdownV2Specials) {
  // A display name with MarkdownV2 specials (_ * …) must be escaped in
  // the formatting context (link text / bold) so MessageSender's
  // MarkdownV2 parse succeeds; inside a `code span` only ` and \ are
  // special, so the other specials stay literal there.
  std::vector<commands::ChatView> views;
  views.push_back(MakeUserView("Cool_Channel*Star", "cool", 200));
  views.push_back(MakeChannelView("Best*Deals", "", 400));

  const auto batches =
      commands::ControlCommandsHandler::RenderRsmeBatches(views, 10);
  // Users and chats batch separately (users first), so two batches.
  ASSERT_EQ(batches.size(), 2u);
  EXPECT_EQ(batches[0],
            "1\\. [Cool\\_Channel\\*Star](https://t.me/cool)\t`/ym 200 "
            "Cool_Channel*Star`\t\n\n");
  EXPECT_EQ(batches[1],
            "1\\. **⚠️Best\\*Deals⚠️**\t`/who 400 Best*Deals`\t\n\n");
}

TEST(ControlCommandStatics, RenderRsmeBatchesOneLinePerMessage) {
  std::vector<commands::ChatView> views;
  views.push_back(MakeUserView("Alice", "alice_h", 200));
  views.push_back(MakeChannelView("Deals", "deals", 100));
  const auto batches =
      commands::ControlCommandsHandler::RenderRsmeBatches(views, 1);
  ASSERT_EQ(batches.size(), 2u);
  EXPECT_EQ(batches[0],
            "1\\. [Alice](https://t.me/alice_h)\t`/ym 200 Alice`\t\n\n");
  EXPECT_EQ(batches[1],
            "1\\. [👉👉👈](https://t.me/deals)\t`/xm 100 Deals`\t\n\n");
}

TEST(ControlCommandStatics, RenderRsmeBatchesEmptyInput) {
  const auto batches = commands::ControlCommandsHandler::RenderRsmeBatches(
      {}, app::CommandsConfig{}.list_batch_lines);
  EXPECT_TRUE(batches.empty());
}

TEST(ControlCommandStatics, RenderRsorCleanReport) {
  std::vector<commands::ChatView> views;
  views.push_back(MakeChannelView("Deals", "deals", 100));
  EXPECT_EQ(commands::ControlCommandsHandler::RenderRsor(views, 50),
            "未发现失效或非法的频道/群组。");
  EXPECT_EQ(commands::ControlCommandsHandler::RenderRsor({}, 50),
            "未发现失效或非法的频道/群组。");
}

TEST(ControlCommandStatics, RenderRsorFindingsReport) {
  std::vector<commands::ChatView> views;
  views.push_back(MakeChannelView("Deals", "deals", 100));
  views.push_back(MakeChannelView("Dead", "", 222));
  views[1].banned = true;

  const std::string report =
      commands::ControlCommandsHandler::RenderRsor(views, 50);
  EXPECT_EQ(report,
            "共发现 1 个失效/非法对话：\n\n"
            "• Dead [私有]\n"
            "  https://t\\.me/c/222\n"
            "  原因：已被封禁/删除（Forbidden）");
}

TEST(ControlCommandStatics, RenderRsorCapsFindingCount) {
  std::vector<commands::ChatView> views;
  for (int k = 0; k < 3; ++k) {
    views.push_back(MakeChannelView("C" + std::to_string(k), "", 100 + k));
    views.back().banned = true;
  }
  const std::string report =
      commands::ControlCommandsHandler::RenderRsor(views, 2);
  EXPECT_NE(report.find("共发现 3 个失效/非法对话："), std::string::npos);
  EXPECT_NE(report.find("C0"), std::string::npos);
  EXPECT_NE(report.find("C1"), std::string::npos);
  EXPECT_EQ(report.find("C2"), std::string::npos);  // capped at 2 bullets
}

// --- wiring ------------------------------------------------------------------

class ControlCommandsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    thumb_path_ =
        "/tmp/tg_client_ctl_thumb_" + std::to_string(getpid()) + ".jpg";
    {
      std::ofstream out(thumb_path_, std::ios::binary);
      out << "\xFF\xD8" "fake-jpeg-thumb";
    }
    outbox_path_ =
        "/tmp/tg_client_ctl_outbox_" + std::to_string(getpid()) + ".sqlite";
    std::remove(outbox_path_.c_str());

    config_.media_thumbnail.keyme = "ctl-test-key";
    sender_.thumbnail_path = thumb_path_;
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
    outbox_ = std::make_unique<tg::infra::Outbox>(outbox_path_);
    ctx_ = std::make_unique<app::AppContext>(app::AppContext{
        *workers_, sender_, *files_, *process_streamer_, *search_streamer_,
        *outbox_, config_});
    chat_state_ = std::make_unique<chat_state::ChatStateStore>(sender_);
    process_handler_ = std::make_unique<media::MediaThumbnailHandler>(
        media::MediaExtractor(config_.media_thumbnail), sender_, *files_,
        common::MemoCipher(config_.media_thumbnail.keyme));
    messenger_ = std::make_unique<tg::telegram::MessageSender>(sender_);
    scanner_ = std::make_unique<commands::DialogScanner>(
        sender_, commands::DialogScanner::Config{});
    backfill_ = std::make_unique<history_backfill::HistoryBackfill>(
        sender_, *process_handler_, *chat_state_, *messenger_,
        history_backfill::HistoryBackfillConfig{});

    commands_config_.control_chat_id = kControl;
    commands_config_.self_commands = true;
    bot_config_.command_bot_user_id = kBotUser;
    handler_ = std::make_unique<commands::ControlCommandsHandler>(
        commands_config_, bot_config_, *messenger_, *scanner_, *backfill_);
  }

  void TearDown() override {
    handler_.reset();
    backfill_.reset();
    scanner_.reset();
    workers_.reset();
    ctx_.reset();
    outbox_.reset();
    search_streamer_.reset();
    process_streamer_.reset();
    messenger_.reset();
    process_handler_.reset();
    chat_state_.reset();
    files_.reset();
    std::remove(thumb_path_.c_str());
    std::remove(outbox_path_.c_str());
  }

  bool WaitForText(const std::string& needle) {
    return WaitFor([this, &needle] {
      return HasTextContaining(sender_, needle);
    });
  }

  std::string thumb_path_;
  std::string outbox_path_;
  app::Config config_;
  app::CommandsConfig commands_config_;
  app::BotCommandConfig bot_config_;
  FakeTdSender sender_;
  std::unique_ptr<tg::telegram::FileDownloader> files_;
  std::unique_ptr<app::WorkerPool> workers_;
  std::unique_ptr<transport::StreamerPool<ImageRequest, ImageReply>>
      process_streamer_;
  std::unique_ptr<transport::StreamerPool<SearchReq, SearchRsp>>
      search_streamer_;
  std::unique_ptr<tg::infra::Outbox> outbox_;
  std::unique_ptr<app::AppContext> ctx_;
  std::unique_ptr<chat_state::ChatStateStore> chat_state_;
  std::unique_ptr<media::MediaThumbnailHandler> process_handler_;
  std::unique_ptr<tg::telegram::MessageSender> messenger_;
  std::unique_ptr<commands::DialogScanner> scanner_;
  std::unique_ptr<history_backfill::HistoryBackfill> backfill_;
  std::unique_ptr<commands::ControlCommandsHandler> handler_;
};

TEST_F(ControlCommandsTest, CanHandleTriggerDirections) {
  // The Python model: the account itself commands in the control chat.
  auto self = MakeTextUpdate("/xm 100", kControl, true);
  EXPECT_TRUE(handler_->CanHandle(self));

  // Incoming from a random user: not a configured direction.
  auto stranger = MakeTextUpdate("/xm 100", kControl, false, 999);
  EXPECT_FALSE(handler_->CanHandle(stranger));

  // Incoming from the configured command bot: the existing bot model.
  auto bot = MakeTextUpdate("/xm 100", kControl, false, kBotUser);
  EXPECT_TRUE(handler_->CanHandle(bot));

  // Any chat other than the control chat: never.
  auto other_chat = MakeTextUpdate("/xm 100", 42, true);
  EXPECT_FALSE(handler_->CanHandle(other_chat));

  // Non-command and non-text content: never.
  auto not_command = MakeTextUpdate("hello world", kControl, true);
  EXPECT_FALSE(handler_->CanHandle(not_command));

  auto photo_update = MakeTextUpdate("", kControl, true);
  // Replace the content with messagePhoto: CanHandle must refuse.
  {
    auto photo_message = sender_.MakePhotoMessage(1);
    static_cast<td_api::updateNewMessage&>(*photo_update).message_->content_ =
        std::move(photo_message->content_);
  }
  EXPECT_FALSE(handler_->CanHandle(photo_update));

  // Unknown command is not ours.
  auto unknown = MakeTextUpdate("/status", kControl, true);
  EXPECT_FALSE(handler_->CanHandle(unknown));

  // Case and @bot suffix do not matter.
  auto styled = MakeTextUpdate("/RSME@MyBot", kControl, true);
  EXPECT_TRUE(handler_->CanHandle(styled));

  // Unconfigured direction when self_commands is off: only the bot remains.
  commands_config_.self_commands = false;
  commands::ControlCommandsHandler bot_only(commands_config_, bot_config_,
                                            *messenger_, *scanner_, *backfill_);
  auto self_again = MakeTextUpdate("/xm 100", kControl, true);
  EXPECT_FALSE(bot_only.CanHandle(self_again));
  auto bot_again = MakeTextUpdate("/xm 100", kControl, false, kBotUser);
  EXPECT_TRUE(bot_only.CanHandle(bot_again));

  // Disabled handler never fires.
  commands_config_.enabled = false;
  commands::ControlCommandsHandler disabled(commands_config_, bot_config_,
                                            *messenger_, *scanner_, *backfill_);
  EXPECT_FALSE(disabled.CanHandle(self_again));
}

TEST_F(ControlCommandsTest, HandleBackfillStartsJobThroughPipeline) {
  sender_.chats[kDialog] = {"Deals", 0, kBare};
  sender_.supergroups[kBare] = {"dealchannel", true};
  sender_.search_pages = {
      FakeTdSender::SearchPage{{300, 200, 150}, 150},
      FakeTdSender::SearchPage{{120}, 0},
  };
  auto update = MakeTextUpdate("/xm 1234567890", kControl, true);
  handler_->Handle(update, *ctx_);

  // The backfill runs the realtime pipeline: all four ids land in the
  // outbox with image_id = str(message_id).
  ASSERT_TRUE(WaitFor([this] { return outbox_->PendingCount() == 4; }));
  EXPECT_NE(outbox_->Get("120"), std::nullopt);
  EXPECT_NE(outbox_->Get("300"), std::nullopt);
  EXPECT_TRUE(WaitForText("Backfill started"));
  EXPECT_TRUE(WaitForText("Backfill finished"));
  // Replies went to the control chat.
  bool all_to_control = true;
  for (const std::int64_t chat_id : sender_.sent_chat_ids) {
    if (chat_id != kControl) all_to_control = false;
  }
  EXPECT_TRUE(all_to_control);
}

TEST_F(ControlCommandsTest, HandleBackfillRefusals) {
  // Disabled backfill feature: a dedicated disabled instance.
  {
    auto disabled = std::make_unique<history_backfill::HistoryBackfill>(
        sender_, *process_handler_, *chat_state_, *messenger_,
        history_backfill::HistoryBackfillConfig{false, 60, 1});
    commands::ControlCommandsHandler disabled_handler(
        commands_config_, bot_config_, *messenger_, *scanner_, *disabled);
    auto update = MakeTextUpdate("/xm 100", kControl, true);
    disabled_handler.Handle(update, *ctx_);
    EXPECT_TRUE(WaitForText("回填功能未启用"));
  }

  // Missing argument.
  auto no_args = MakeTextUpdate("/xm", kControl, true);
  handler_->Handle(no_args, *ctx_);
  EXPECT_TRUE(WaitForText("用法：/xm <裸 chat id\\>"));

  // Unparseable argument.
  auto bad_arg = MakeTextUpdate("/xm abc", kControl, true);
  handler_->Handle(bad_arg, *ctx_);
  EXPECT_TRUE(WaitForText("无法解析 chat id `abc`"));

  // A bare id above uint32 is VALID (int64; Telegram user ids above 2^32
  // exist, e.g. 7974301646): Start accepts it and the job proceeds to the
  // peer probe, which fails here because the fake knows no such chat.
  auto huge = MakeTextUpdate("/xm 5000000000", kControl, true);
  handler_->Handle(huge, *ctx_);
  EXPECT_TRUE(WaitForText("not a known peer"));

  // A non-positive bare id has no dialog candidates: Start refuses it.
  auto invalid = MakeTextUpdate("/xm -5", kControl, true);
  handler_->Handle(invalid, *ctx_);
  EXPECT_TRUE(WaitForText("not a valid bare chat id"));
}

TEST_F(ControlCommandsTest, HandleRsmeEmptyDialogSet) {
  auto update = MakeTextUpdate("/rsme", kControl, true);
  handler_->Handle(update, *ctx_);
  EXPECT_TRUE(WaitForText("没有发现对话。"));
}

TEST_F(ControlCommandsTest, HandleRsorEmptyAuditSet) {
  auto update = MakeTextUpdate("/rsor", kControl, true);
  handler_->Handle(update, *ctx_);
  EXPECT_TRUE(WaitForText("开始扫描已加入的频道/群组…"));
  EXPECT_TRUE(WaitForText("未发现失效或非法的频道/群组。"));
  EXPECT_EQ(sender_.sent_getSupergroupFullInfo, 0);  // nothing to probe
}

TEST_F(ControlCommandsTest, SecondScanCommandWhileRunningGetsBusyReply) {
  // One dialog, gated at getChat: the first scan stays running until we
  // open the gate — the second command must hit the busy path.
  const std::int64_t user_id = 42;
  sender_.chat_list = {user_id};
  sender_.chats[user_id] = {"Alice", 2, user_id};
  sender_.users[user_id] = {"Alice", "alice_h"};
  sender_.gate_getChat = true;

  auto first = MakeTextUpdate("/rsme", kControl, true);
  handler_->Handle(first, *ctx_);  // scan starts, blocks inside getChat

  auto second = MakeTextUpdate("/rsor", kControl, true);
  handler_->Handle(second, *ctx_);
  EXPECT_TRUE(WaitForText("扫描进行中，请稍候。"));

  sender_.gate.store(1);  // release the first scan
  EXPECT_TRUE(WaitForText("[Alice](https://t.me/alice_h)"));
  EXPECT_TRUE(WaitFor([this] { return !scanner_->running(); }));
}
