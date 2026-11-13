// HistoryBackfill (Telethon migration Chunk 2; Python handlerfetchhistory
// 918-963): phase A pages photoAndVideo history BACKWARD from the newest
// message down to the chat's watermark, phase B replays the gap ascending
// through the REALTIME pipeline (extract -> download -> Outbox ->
// ProcessImages) with image_id = str(message_id). FloodWait pauses via the
// resume timer; a known watermark skips already-covered messages.
#include <app/AppContext.h>
#include <app/WorkerPool.h>
#include <common/MemoCipher.h>
#include <features/chat_state/ChatStateStore.h>
#include <features/history_backfill/HistoryBackfill.h>
#include <features/media_thumbnail/MediaExtractor.h>
#include <features/media_thumbnail/MediaThumbnailHandler.h>
#include <infra/Outbox.h>
#include <telegram/MessageSender.h>
#include <telegram/TelegramClient.h>
#include <transport/AsyncStreamer.h>

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace td_api = td::td_api;

namespace app = tg::app;
namespace chat_state = tg::features::chat_state;
namespace common = tg::common;
namespace history_backfill = tg::features::history_backfill;
namespace media = tg::features::media_thumbnail;
namespace transport = tg::transport;

namespace {

constexpr std::int64_t kBare = 1'234'567'890;          // bare channel id
constexpr std::int64_t kDialog = -1'001'234'567'890LL;  // TDLib dialog id
constexpr std::int64_t kControl = 777;                  // control chat

class FakeTdSender : public tg::telegram::ITdSender {
 public:
  // searchChatMessages programming: pages consumed in order.
  struct SearchPage {
    std::vector<std::int64_t> message_ids;  // reverse-chronological
    std::int64_t next_from_message_id = 0;
    bool flood = false;  // answer 429 instead
  };
  std::vector<SearchPage> search_pages;
  int sent_search = 0;
  int sent_getMessage = 0;
  int sent_getChat = 0;
  // getMessage answers (message id -> photo message id echoed).
  std::vector<std::int64_t> message_ids;
  // sendMessage capture (the messenger's replies).
  std::vector<std::int64_t> sent_chat_ids;
  std::vector<std::string> sent_texts;
  // downloadFile answers with this local path.
  std::string thumbnail_path;
  // Mid-flight watermark acks (deterministic, fired inside getMessage).
  chat_state::ChatStateStore* ack_state = nullptr;
  std::int64_t ack_bare_id = 0;
  std::map<std::int64_t, std::int64_t> ack_on_get_message;
  // When false, EVERY getChat candidate probe fails — the "never
  // interacted" peer of UnknownPeerDropsJobWithNotice.
  bool peer_known = true;
  // Title returned by a successful getChat. Notices interpolate it, so
  // tests can put MarkdownV2 reserved characters here.
  std::string peer_title = "Deals";
  // Deterministic phase-B gate: while held, getMessage answers wait for
  // ReleaseGetMessage() — pins a job mid-dispatch so it is still "running"
  // when the test issues a duplicate Start() (5 s timeout as a safety net).
  void HoldGetMessage() {
    std::lock_guard<std::mutex> lock(hold_mutex_);
    hold_get_message_ = true;
  }
  void ReleaseGetMessage() {
    std::lock_guard<std::mutex> lock(hold_mutex_);
    hold_get_message_ = false;
    hold_cv_.notify_all();
  }

  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback) override {
    td::ClientManager::Response response;
    response.request_id = next_id_++;
    switch (request->get_id()) {
      case td_api::getChat::ID: {
        ++sent_getChat;
        const auto chat_id =
            static_cast<const td_api::getChat&>(*request).chat_id_;
        if (peer_known && chat_id == kDialog) {
          auto chat = td_api::make_object<td_api::chat>();
          chat->id_ = chat_id;
          chat->title_ = peer_title;
          chat->type_ = td_api::make_object<td_api::chatTypeSupergroup>(
              kBare, false);
          response.object = std::move(chat);
        } else {
          response.object =
              td_api::make_object<td_api::error>(400, "Chat not found");
        }
        break;
      }
      case td_api::searchChatMessages::ID: {
        ++sent_search;
        const auto& search =
            static_cast<const td_api::searchChatMessages&>(*request);
        last_search_chat_id = search.chat_id_;
        last_search_from = search.from_message_id_;
        last_search_limit = search.limit_;
        last_search_filter_photo_and_video =
            search.filter_ != nullptr &&
            search.filter_->get_id() ==
                td_api::searchMessagesFilterPhotoAndVideo::ID;
        SearchPage page;
        if (!search_pages.empty()) {
          page = search_pages.front();
          search_pages.erase(search_pages.begin());
        }
        if (page.flood) {
          response.object = td_api::make_object<td_api::error>(
              429, "Too Many Requests: retry after 0");
        } else {
          auto found = td_api::make_object<td_api::foundChatMessages>();
          found->total_count_ = static_cast<std::int32_t>(
              page.message_ids.size());
          for (const std::int64_t id : page.message_ids) {
            found->messages_.push_back(MakePhotoMessage(id));
          }
          found->next_from_message_id_ = page.next_from_message_id;
          response.object = std::move(found);
        }
        break;
      }
      case td_api::getMessage::ID: {
        ++sent_getMessage;
        {
          std::unique_lock<std::mutex> lock(hold_mutex_);
          hold_cv_.wait_for(lock, std::chrono::seconds(5),
                            [this] { return !hold_get_message_; });
        }
        const auto message_id =
            static_cast<const td_api::getMessage&>(*request).message_id_;
        // Deterministic mid-flight "server ack": fetching this message
        // advances the watermark the way ImageReply processing would, so the
        // NEXT batch dispatch sees it (the re-read happens after this reply).
        if (ack_state != nullptr) {
          const auto ack = ack_on_get_message.find(message_id);
          if (ack != ack_on_get_message.end()) {
            ack_state->ApplyReply(ack_bare_id, ack->second);
          }
        }
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
        remote->unique_id_ = "u-backfill";
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
    remote->unique_id_ = "u-backfill";
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

  std::int64_t last_search_chat_id = 0;
  std::int64_t last_search_from = -1;
  int last_search_limit = -1;
  bool last_search_filter_photo_and_video = false;

 private:
  std::mutex hold_mutex_;
  std::condition_variable hold_cv_;
  bool hold_get_message_ = false;
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

}  // namespace

class HistoryBackfillTest : public ::testing::Test {
 protected:
  void SetUp() override {
    thumb_path_ =
        "/tmp/tg_client_backfill_thumb_" + std::to_string(getpid()) + ".jpg";
    {
      std::ofstream out(thumb_path_, std::ios::binary);
      out << "\xFF\xD8" "fake-jpeg-thumb";
    }
    outbox_path_ =
        "/tmp/tg_client_backfill_outbox_" + std::to_string(getpid()) +
        ".sqlite";
    std::remove(outbox_path_.c_str());

    config_.media_thumbnail.keyme = "backfill-test-key";
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
  }

  void TearDown() override {
    backfill_.reset();
    // Drain the pool first: queued finish stages use ctx_/outbox_/streamers.
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

  bool WaitForOutboxCount(std::int64_t count) {
    return WaitFor(
        [this, count] { return outbox_->PendingCount() == count; });
  }

  bool WaitForTextContaining(const std::string& needle) {
    return WaitFor([this, &needle] {
      for (const std::string& text : sender_.sent_texts) {
        if (text.find(needle) != std::string::npos) return true;
      }
      return false;
    });
  }

  std::string thumb_path_;
  std::string outbox_path_;
  app::Config config_;
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
  std::unique_ptr<history_backfill::HistoryBackfill> backfill_;
};

TEST_F(HistoryBackfillTest, FullRunReplaysGapThroughRealtimePipeline) {
  chat_state_->ApplyReply(kBare, 100);  // server-confirmed watermark
  sender_.search_pages = {
      FakeTdSender::SearchPage{{300, 200, 150}, 150},
      FakeTdSender::SearchPage{{120}, 0},
  };
  backfill_ = std::make_unique<history_backfill::HistoryBackfill>(
      sender_, *process_handler_, *chat_state_, *messenger_,
      history_backfill::HistoryBackfillConfig{});

  ASSERT_TRUE(backfill_->Start(kBare, *ctx_, kControl));
  // Phase B replays the gap oldest-first: 120, 150, 200, 300 — each through
  // the realtime pipeline, image_id = str(message_id).
  ASSERT_TRUE(WaitForOutboxCount(4));
  ASSERT_NE(outbox_->Get("120"), std::nullopt);
  ASSERT_NE(outbox_->Get("150"), std::nullopt);
  ASSERT_NE(outbox_->Get("200"), std::nullopt);
  ASSERT_NE(outbox_->Get("300"), std::nullopt);
  // Phase A really used the media filter against the dialog.
  EXPECT_EQ(sender_.last_search_filter_photo_and_video, true);
  EXPECT_EQ(sender_.last_search_chat_id, kDialog);
  // The finished notice went to the control chat.
  EXPECT_TRUE(WaitForTextContaining("Backfill finished"));
  bool finished_to_control = false;
  for (std::size_t k = 0; k < sender_.sent_texts.size(); ++k) {
    if (sender_.sent_texts[k].find("Backfill finished") !=
            std::string::npos &&
        sender_.sent_chat_ids[k] == kControl) {
      finished_to_control = true;
    }
  }
  EXPECT_TRUE(finished_to_control);
  EXPECT_EQ(backfill_->active_count(), 0u);
}

TEST_F(HistoryBackfillTest, WatermarkTerminatesCollection) {
  chat_state_->ApplyReply(kBare, 150);  // newer watermark: 150 and below
                                        // are already covered
  sender_.search_pages = {
      FakeTdSender::SearchPage{{300, 200, 150}, 0},  // oldest == watermark
  };
  backfill_ = std::make_unique<history_backfill::HistoryBackfill>(
      sender_, *process_handler_, *chat_state_, *messenger_,
      history_backfill::HistoryBackfillConfig{});

  ASSERT_TRUE(backfill_->Start(kBare, *ctx_, kControl));
  ASSERT_TRUE(WaitForOutboxCount(2));  // only 200 and 300
  EXPECT_NE(outbox_->Get("200"), std::nullopt);
  EXPECT_NE(outbox_->Get("300"), std::nullopt);
  EXPECT_EQ(outbox_->Get("150"), std::nullopt);  // at/below the watermark
  EXPECT_TRUE(WaitForTextContaining("Backfill finished"));
}

TEST_F(HistoryBackfillTest, NothingAboveWatermarkFinishesImmediately) {
  chat_state_->ApplyReply(kBare, 500);
  sender_.search_pages = {
      FakeTdSender::SearchPage{{400, 300}, 0},  // both <= watermark
  };
  backfill_ = std::make_unique<history_backfill::HistoryBackfill>(
      sender_, *process_handler_, *chat_state_, *messenger_,
      history_backfill::HistoryBackfillConfig{});

  ASSERT_TRUE(backfill_->Start(kBare, *ctx_, kControl));
  EXPECT_TRUE(WaitForTextContaining("already up to date"));
  EXPECT_EQ(outbox_->PendingCount(), 0);
  EXPECT_EQ(sender_.sent_getMessage, 0);
}

TEST_F(HistoryBackfillTest, PerBatchWatermarkReReadSkipsCoveredMessages) {
  chat_state_->ApplyReply(kBare, 25);
  // Gap above the watermark: 30, 40, 50 (collected desc, dispatched asc).
  sender_.search_pages = {
      FakeTdSender::SearchPage{{50, 40, 30}, 0},
  };
  backfill_ = std::make_unique<history_backfill::HistoryBackfill>(
      sender_, *process_handler_, *chat_state_, *messenger_,
      history_backfill::HistoryBackfillConfig{true, /* page_limit */ 1,
                                              /* flood_pad_seconds */ 1});

  // Mid-flight ack: while message 30 is being fetched, the "server"
  // confirms the watermark up to 40 (ImageReply processing). The next batch
  // dispatch re-reads the watermark and skips 40 — 30 and 50 still go out.
  sender_.ack_state = chat_state_.get();
  sender_.ack_bare_id = kBare;
  sender_.ack_on_get_message[30] = 40;

  ASSERT_TRUE(backfill_->Start(kBare, *ctx_, kControl));
  ASSERT_TRUE(WaitForOutboxCount(2));
  EXPECT_NE(outbox_->Get("30"), std::nullopt);
  EXPECT_NE(outbox_->Get("50"), std::nullopt);
  EXPECT_EQ(outbox_->Get("40"), std::nullopt);  // covered by the new watermark
  EXPECT_TRUE(WaitForTextContaining("Backfill finished"));
}

TEST_F(HistoryBackfillTest, FloodWaitPausesThenResumes) {
  chat_state_->ApplyReply(kBare, 0);
  sender_.search_pages = {
      FakeTdSender::SearchPage{{}, 0, /* flood */ true},  // first search: 429
      FakeTdSender::SearchPage{{60, 50}, 50},
      FakeTdSender::SearchPage{{40}, 0},
  };
  backfill_ = std::make_unique<history_backfill::HistoryBackfill>(
      sender_, *process_handler_, *chat_state_, *messenger_,
      history_backfill::HistoryBackfillConfig{true, 60,
                                              /* flood_pad_seconds */ 0});

  ASSERT_TRUE(backfill_->Start(kBare, *ctx_, kControl));
  // The job pauses on the 429, then the resume timer (retry after 0 + pad 0)
  // re-opens phase A; both pages then replay through the pipeline.
  ASSERT_TRUE(WaitForOutboxCount(3));
  EXPECT_NE(outbox_->Get("40"), std::nullopt);
  EXPECT_NE(outbox_->Get("50"), std::nullopt);
  EXPECT_NE(outbox_->Get("60"), std::nullopt);
  EXPECT_TRUE(WaitForTextContaining("Backfill finished"));
}

TEST_F(HistoryBackfillTest, UnknownPeerDropsJobWithNotice) {
  // Python checks the peer BEFORE paging (handlerfetchhistory:
  // get_entity(int) aborts the command on error) — the production probe
  // mirrors that, so the fake must report the peer as unknown to reach it.
  sender_.peer_known = false;
  backfill_ = std::make_unique<history_backfill::HistoryBackfill>(
      sender_, *process_handler_, *chat_state_, *messenger_,
      history_backfill::HistoryBackfillConfig{});

  ASSERT_TRUE(backfill_->Start(kBare, *ctx_, kControl));
  EXPECT_TRUE(WaitForTextContaining("not a known peer"));
  EXPECT_EQ(backfill_->active_count(), 0u);
  EXPECT_EQ(sender_.sent_search, 0);  // never searched
}

TEST_F(HistoryBackfillTest, DuplicateJobAndInvalidIdsRefused) {
  chat_state_->ApplyReply(kBare, 100);
  sender_.search_pages = {
      FakeTdSender::SearchPage{{300, 200, 150}, 150},
      FakeTdSender::SearchPage{{120}, 0},
  };
  // The inline fake would run the whole job to completion inside the first
  // Start() — pin phase B on the first getMessage so the job is still in
  // the active map when the duplicate Start() below is checked.
  sender_.HoldGetMessage();
  backfill_ = std::make_unique<history_backfill::HistoryBackfill>(
      sender_, *process_handler_, *chat_state_, *messenger_,
      history_backfill::HistoryBackfillConfig{});

  ASSERT_TRUE(backfill_->Start(kBare, *ctx_, kControl));
  EXPECT_TRUE(WaitForTextContaining("Backfill started"));
  // Same chat while running: refused, with a notice.
  EXPECT_FALSE(backfill_->Start(kBare, *ctx_, kControl));
  EXPECT_TRUE(WaitForTextContaining("already running"));
  // Non-positive ids: refused outright — the only invalid bare ids under
  // the int64 bare-id semantics (Telegram user ids above 2^32 are
  // legitimate /xm arguments now).
  EXPECT_FALSE(backfill_->Start(0, *ctx_, kControl));
  EXPECT_FALSE(backfill_->Start(-5, *ctx_, kControl));
  // 5'000'000'000 is above the uint32 range but a legitimate user id:
  // Start accepts it and the probe drops the job with the unknown-peer
  // notice (Python: get_entity(int) raises and the command just returns).
  EXPECT_TRUE(backfill_->Start(5'000'000'000LL, *ctx_, kControl));
  EXPECT_TRUE(WaitForTextContaining("not a known peer"));

  sender_.ReleaseGetMessage();  // let the pinned batch finish
  ASSERT_TRUE(WaitForOutboxCount(4));
  EXPECT_TRUE(WaitForTextContaining("Backfill finished"));
}

TEST_F(HistoryBackfillTest, NoticesEscapeMarkdownV2InTitleAndTemplate) {
  // The live failure: the title had no '.', but the notice template ended
  // with one, and a title may contain any MarkdownV2 reserved character.
  // Parentheses in the finish reason are reserved too.
  backfill_ = std::make_unique<history_backfill::HistoryBackfill>(
      sender_, *process_handler_, *chat_state_, *messenger_,
      history_backfill::HistoryBackfillConfig{});

  auto run = [this](const std::string& title,
                    const std::string& escaped_title) {
    sender_.peer_title = title;
    sender_.search_pages = {FakeTdSender::SearchPage{{400}, 0}};
    chat_state_->ApplyReply(kBare, 500);  // page is already covered
    if (!backfill_->Start(kBare, *ctx_, kControl)) {
      ADD_FAILURE() << "Start refused for " << title;
      return;
    }
    const std::string started =
        "Backfill started: `1234567890` \"" + escaped_title +
        "\" from watermark `500`\\.";
    const std::string finished =
        "Backfill finished for `1234567890` \"" + escaped_title +
        "\": nothing to backfill \\(already up to date\\) — sent `0`, "
        "skipped `0`\\.";
    EXPECT_TRUE(WaitForTextContaining(started)) << started;
    EXPECT_TRUE(WaitForTextContaining(finished)) << finished;
  };

  run("Liu Pole and Alsia Kople.", "Liu Pole and Alsia Kople\\.");
  run("a-b (c)!", "a\\-b \\(c\\)\\!");
}

TEST_F(HistoryBackfillTest, DisabledRefusesEverything) {
  backfill_ = std::make_unique<history_backfill::HistoryBackfill>(
      sender_, *process_handler_, *chat_state_, *messenger_,
      history_backfill::HistoryBackfillConfig{/* enabled */ false, 60, 1});
  EXPECT_FALSE(backfill_->Start(kBare, *ctx_, kControl));
  EXPECT_FALSE(backfill_->enabled());
}
