// MessageSender (Telethon migration Chunk 2, SURVEY §7.2-6 "SendText"):
// 4096-char split (whole lines kept together, oversized lines hard-split) and
// the markdown sendMessage path with link previews disabled.
#include <telegram/MessageSender.h>
#include <telegram/FloodRetry.h>
#include <telegram/TelegramClient.h>

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace td_api = td::td_api;

namespace message_sender = tg::telegram;

namespace {

class FakeTdSender : public tg::telegram::ITdSender {
 public:
  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback) override {
    td::ClientManager::Response response;
    response.request_id = next_id_++;
    if (request->get_id() == td_api::parseTextEntities::ID) {
      // Simulate a successful MarkdownV2 parse that returns the text
      // unchanged (no entity stripping in this fake — the flow, not
      // TDLib's parser, is what these tests cover).
      const auto& parse =
          static_cast<const td_api::parseTextEntities&>(*request);
      auto formatted = td_api::make_object<td_api::formattedText>();
      formatted->text_ = parse.text_;
      response.object = std::move(formatted);
    } else if (request->get_id() == td_api::sendMessage::ID) {
      const auto& send = static_cast<const td_api::sendMessage&>(*request);
      sent_chat_ids.push_back(send.chat_id_);
      EXPECT_TRUE(send.input_message_content_ != nullptr);
      EXPECT_TRUE(send.input_message_content_->get_id() ==
                  td_api::inputMessageText::ID);
      const auto& content = static_cast<const td_api::inputMessageText&>(
          *send.input_message_content_);
      sent_texts.push_back(content.text_->text_);
      sent_link_previews_disabled.push_back(
          content.link_preview_options_ != nullptr &&
          content.link_preview_options_->is_disabled_);
      response.object = td_api::make_object<td_api::message>();
    } else {
      response.object = td_api::make_object<td_api::error>(400, "unexpected");
    }
    callback(std::move(response));
    return static_cast<std::int64_t>(response.request_id);
  }

  std::vector<std::int64_t> sent_chat_ids;
  std::vector<std::string> sent_texts;
  std::vector<bool> sent_link_previews_disabled;

 private:
  std::uint64_t next_id_ = 1;
};

}  // namespace

// --- SplitForTelegram ---------------------------------------------------------

TEST(SplitForTelegram, SplitsAtLineBoundariesKeepingLinesIntact) {
  // 17 chars at limit 12: the third line does not fit — the split happens
  // BETWEEN lines, no line is broken.
  const auto chunks = message_sender::MessageSender::SplitForTelegram(
      "alpha\nbeta\ngamma", 12);
  ASSERT_EQ(chunks.size(), 2u);
  EXPECT_EQ(chunks[0], "alpha\nbeta");
  EXPECT_EQ(chunks[1], "gamma");
  std::string rejoined;
  for (std::size_t k = 0; k < chunks.size(); ++k) {
    if (k > 0) rejoined += "\n";
    rejoined += chunks[k];
  }
  EXPECT_EQ(rejoined, "alpha\nbeta\ngamma");
}

TEST(SplitForTelegram, SplitsAtLineBoundaryWhenOverLimit) {
  const auto chunks =
      message_sender::MessageSender::SplitForTelegram("aaa\nbbb\nccc", 7);
  ASSERT_EQ(chunks.size(), 2u);
  EXPECT_EQ(chunks[0], "aaa\nbbb");
  EXPECT_EQ(chunks[1], "ccc");
}

TEST(SplitForTelegram, HardSplitsOversizedLine) {
  // An 11-char line at limit 4: hard-split into 4+4+3 — content exact.
  const auto chunks =
      message_sender::MessageSender::SplitForTelegram("aaabbbcccdd", 4);
  ASSERT_EQ(chunks.size(), 3u);
  EXPECT_EQ(chunks[0], "aaab");
  EXPECT_EQ(chunks[1], "bbcc");
  EXPECT_EQ(chunks[2], "cdd");
  std::string rejoined;
  for (const std::string& chunk : chunks) rejoined += chunk;
  EXPECT_EQ(rejoined, "aaabbbcccdd");
}

TEST(SplitForTelegram, OversizedLineFlushesPendingChunkFirst) {
  // "ok\n" + a 6-char line at limit 4: the pending chunk goes out before the
  // hard split begins.
  const auto chunks =
      message_sender::MessageSender::SplitForTelegram("ok\nxxyyzz", 4);
  ASSERT_EQ(chunks.size(), 3u);
  EXPECT_EQ(chunks[0], "ok");
  EXPECT_EQ(chunks[1], "xxyy");
  EXPECT_EQ(chunks[2], "zz");
}

TEST(SplitForTelegram, EmptyTextProducesOneEmptyChunk) {
  const auto chunks = message_sender::MessageSender::SplitForTelegram("", 100);
  ASSERT_EQ(chunks.size(), 1u);
  EXPECT_EQ(chunks[0], "");
}

TEST(SplitForTelegram, DefaultLimitIs4096) {
  std::string big;
  for (int i = 0; i < 500; ++i) big += "0123456789\n";  // 11 * 500 chars
  const auto chunks = message_sender::MessageSender::SplitForTelegram(big);
  for (const std::string& chunk : chunks) {
    EXPECT_LE(chunk.size(), message_sender::MessageSender::kMaxMessageChars);
  }
  EXPECT_GT(chunks.size(), 1u);
}

TEST(SplitForTelegram, EveryChunkRespectsCustomLimit) {
  std::string big;
  for (int i = 0; i < 40; ++i) big += "line\n";
  const auto chunks =
      message_sender::MessageSender::SplitForTelegram(big, 10);
  ASSERT_FALSE(chunks.empty());
  for (const std::string& chunk : chunks) {
    EXPECT_LE(chunk.size(), 10u);
  }
  // Round-trip: chunks joined with the boundary newlines reproduce the
  // original text (the trailing "\n" lives inside the last chunk).
  std::string rejoined;
  for (std::size_t k = 0; k < chunks.size(); ++k) {
    if (k > 0) rejoined += "\n";
    rejoined += chunks[k];
  }
  EXPECT_EQ(rejoined, big);
}

// --- SendMarkdown -------------------------------------------------------------

TEST(SendMarkdown, SingleChunkSendsOneMessage) {
  FakeTdSender sender;
  const message_sender::MessageSender messenger(sender);
  messenger.SendMarkdown(-1'001'234'567'890LL, "hello `world`");
  ASSERT_EQ(sender.sent_texts.size(), 1u);
  EXPECT_EQ(sender.sent_texts[0], "hello `world`");
  EXPECT_EQ(sender.sent_chat_ids[0], -1'001'234'567'890LL);
  EXPECT_TRUE(sender.sent_link_previews_disabled[0]);
}

TEST(SendMarkdown, LongTextSplitsIntoMultipleMessages) {
  FakeTdSender sender;
  const message_sender::MessageSender messenger(sender);
  std::string text;
  for (int i = 0; i < 600; ++i) text += "line-of-text-0123456789\n";
  messenger.SendMarkdown(42, text);
  EXPECT_GT(sender.sent_texts.size(), 1u);
  for (const std::string& chunk : sender.sent_texts) {
    EXPECT_LE(chunk.size(), message_sender::MessageSender::kMaxMessageChars);
  }
  // Rejoined (modulo the final newline) the chunks carry the whole text.
  std::size_t total = 0;
  for (const std::string& chunk : sender.sent_texts) total += chunk.size();
  EXPECT_GE(total, text.size() - sender.sent_texts.size());
  for (const bool disabled : sender.sent_link_previews_disabled) {
    EXPECT_TRUE(disabled);  // Python link_preview=False everywhere
  }
}

TEST(SendMarkdown, ErrorsAreLoggedNotThrown) {
  // The reply arrives as an error object: fire-and-forget — no exception,
  // no retry (ordinary errors are terminal; the command can be re-issued).
  FakeTdSender sender;
  const message_sender::MessageSender messenger(sender);
  EXPECT_NO_THROW(messenger.SendMarkdown(7, "x"));
  EXPECT_EQ(sender.sent_texts.size(), 1u);
}

// parseTextEntities answers with an error: the chunk still goes out as
// plain text (the raw markdown source) instead of being dropped.
class ParseFailingTdSender : public tg::telegram::ITdSender {
 public:
  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback) override {
    td::ClientManager::Response response;
    response.request_id = next_id_++;
    if (request->get_id() == td_api::parseTextEntities::ID) {
      response.object = td_api::make_object<td_api::error>(
          400, "Can't find end of the entity starting at byte offset 3");
    } else if (request->get_id() == td_api::sendMessage::ID) {
      const auto& send = static_cast<const td_api::sendMessage&>(*request);
      const auto& content = static_cast<const td_api::inputMessageText&>(
          *send.input_message_content_);
      sent_texts.push_back(content.text_->text_);
      response.object = td_api::make_object<td_api::message>();
    } else {
      response.object = td_api::make_object<td_api::error>(400, "unexpected");
    }
    callback(std::move(response));
    return static_cast<std::int64_t>(response.request_id);
  }

  std::vector<std::string> sent_texts;

 private:
  std::uint64_t next_id_ = 1;
};

TEST(SendMarkdown, ParseFailureFallsBackToPlainText) {
  ParseFailingTdSender sender;
  const message_sender::MessageSender messenger(sender);
  messenger.SendMarkdown(7, "a *broken markdown");
  ASSERT_EQ(sender.sent_texts.size(), 1u);
  EXPECT_EQ(sender.sent_texts[0], "a *broken markdown");
}

// The parse result is what gets sent, not the raw markdown source: the
// fake's parse strips the backticks (as real TDLib would for a code span).
class StrippingParseTdSender : public tg::telegram::ITdSender {
 public:
  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback) override {
    td::ClientManager::Response response;
    response.request_id = next_id_++;
    if (request->get_id() == td_api::parseTextEntities::ID) {
      const auto& parse =
          static_cast<const td_api::parseTextEntities&>(*request);
      std::string stripped;
      for (const char c : parse.text_) {
        if (c != '`') stripped.push_back(c);
      }
      auto formatted = td_api::make_object<td_api::formattedText>();
      formatted->text_ = stripped;
      response.object = std::move(formatted);
    } else if (request->get_id() == td_api::sendMessage::ID) {
      const auto& send = static_cast<const td_api::sendMessage&>(*request);
      const auto& content = static_cast<const td_api::inputMessageText&>(
          *send.input_message_content_);
      sent_texts.push_back(content.text_->text_);
      response.object = td_api::make_object<td_api::message>();
    } else {
      response.object = td_api::make_object<td_api::error>(400, "unexpected");
    }
    callback(std::move(response));
    return static_cast<std::int64_t>(response.request_id);
  }

  std::vector<std::string> sent_texts;

 private:
  std::uint64_t next_id_ = 1;
};

TEST(SendMarkdown, ParsedTextIsWhatGetsSent) {
  StrippingParseTdSender sender;
  const message_sender::MessageSender messenger(sender);
  messenger.SendMarkdown(7, "hello `world`");
  ASSERT_EQ(sender.sent_texts.size(), 1u);
  EXPECT_EQ(sender.sent_texts[0], "hello world");
}

// --- FloodWait retry (Telethon migration chunk 3) ------------------------------

// sendMessage fake that answers the first `flood_left` sends with a TDLib
// 429 ("Too Many Requests: retry after 0") and the rest with a message.
class FloodFakeTdSender : public tg::telegram::ITdSender {
 public:
  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback) override {
    td::ClientManager::Response response;
    response.request_id = next_id_++;
    if (request->get_id() == td_api::parseTextEntities::ID) {
      // Parse succeeds (text unchanged); parse requests are NOT counted
      // in send_requests — only sendMessage deliveries are.
      const auto& parse =
          static_cast<const td_api::parseTextEntities&>(*request);
      auto formatted = td_api::make_object<td_api::formattedText>();
      formatted->text_ = parse.text_;
      response.object = std::move(formatted);
    } else if (request->get_id() == td_api::sendMessage::ID) {
      ++send_requests;
      const auto& send = static_cast<const td_api::sendMessage&>(*request);
      sent_chat_ids.push_back(send.chat_id_);
      const auto& content = static_cast<const td_api::inputMessageText&>(
          *send.input_message_content_);
      sent_texts.push_back(content.text_->text_);
      if (flood_left > 0) {
        --flood_left;
        response.object = td_api::make_object<td_api::error>(
            429, "Too Many Requests: retry after 0");
      } else {
        response.object = td_api::make_object<td_api::message>();
      }
    } else {
      response.object = td_api::make_object<td_api::error>(400, "unexpected");
    }
    callback(std::move(response));
    return static_cast<std::int64_t>(response.request_id);
  }

  int send_requests = 0;
  int flood_left = 0;
  std::vector<std::int64_t> sent_chat_ids;  // every sendMessage, flood or not
  std::vector<std::string> sent_texts;      // every sendMessage, flood or not

 private:
  std::uint64_t next_id_ = 1;
};

// Polls until the retry loop settles: the closures run on the FloodRetry
// timer thread, so arrival is asynchronous in these tests.
bool WaitFor(int& value, int expected) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (value >= expected) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return value >= expected;
}

TEST(SendMarkdownFlood, RetriesTheRejectedChunkUntilItSends) {
  FloodFakeTdSender sender;
  tg::telegram::FloodRetry scheduler(
      {/*pad_seconds=*/0, /*fallback_seconds=*/5, /*max_attempts=*/3});
  const message_sender::MessageSender messenger(sender, &scheduler);
  sender.flood_left = 2;  // two refusals, third attempt succeeds

  messenger.SendMarkdown(7, "once");
  // Rejected twice, then delivered — no duplicates.
  ASSERT_TRUE(WaitFor(sender.send_requests, 3));
  EXPECT_EQ(sender.flood_left, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_EQ(sender.send_requests, 3);
  scheduler.Shutdown();
}

TEST(SendMarkdownFlood, DropsTheChunkAfterTheAttemptCap) {
  FloodFakeTdSender sender;
  tg::telegram::FloodRetry scheduler(
      {/*pad_seconds=*/0, /*fallback_seconds=*/5, /*max_attempts=*/3});
  const message_sender::MessageSender messenger(sender, &scheduler);
  sender.flood_left = 100;  // always rate-limited

  messenger.SendMarkdown(7, "never lands");
  // Initial attempt + 3 re-issues, then the cap refuses any further one.
  ASSERT_TRUE(WaitFor(sender.send_requests, 4));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_EQ(sender.send_requests, 4);
  scheduler.Shutdown();
}

TEST(SendMarkdownFlood, EverySplitChunkIsRetriedIndependently) {
  FloodFakeTdSender sender;
  tg::telegram::FloodRetry scheduler(
      {/*pad_seconds=*/0, /*fallback_seconds=*/5, /*max_attempts=*/3});
  const message_sender::MessageSender messenger(sender, &scheduler);
  sender.flood_left = 1;  // the first chunk is refused once

  // Three ~3000-char lines: two never fit into one 4096-char chunk, so the
  // text really splits into exactly three messages.
  const std::string a(3000, 'a');
  const std::string b(3000, 'b');
  const std::string c(3000, 'c');
  messenger.SendMarkdown(7, a + "\n" + b + "\n" + c);

  // The initial loop is synchronous: all three chunks went out at once —
  // the flooded one did not block or swallow the other two.
  ASSERT_EQ(sender.send_requests, 3);
  // The refused chunk is retried on its own and lands exactly once.
  ASSERT_TRUE(WaitFor(sender.send_requests, 4));
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_EQ(sender.send_requests, 4);
  EXPECT_EQ(sender.sent_chat_ids.size(), 4u);
  std::vector<std::string> texts = sender.sent_texts;
  std::sort(texts.begin(), texts.end());
  EXPECT_EQ(texts, (std::vector<std::string>{a, a, b, c}));  // one retry, no dup
  scheduler.Shutdown();
}

TEST(SendMarkdownFlood, NoFloodRetryWithoutAScheduler) {
  FloodFakeTdSender sender;
  const message_sender::MessageSender messenger(sender);  // null FloodRetry
  sender.flood_left = 1;

  messenger.SendMarkdown(7, "x");
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_EQ(sender.send_requests, 1);  // pre-chunk-3 behaviour: fail fast
}
