// MessageSender (Telethon migration Chunk 2, SURVEY §7.2-6 "SendText"):
// 4096-char split (whole lines kept together, oversized lines hard-split) and
// the markdown sendMessage path with link previews disabled.
#include <telegram/MessageSender.h>
#include <telegram/TelegramClient.h>

#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

#include <string>
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
    if (request->get_id() == td_api::sendMessage::ID) {
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
  // no retry.
  FakeTdSender sender;
  const message_sender::MessageSender messenger(sender);
  EXPECT_NO_THROW(messenger.SendMarkdown(7, "x"));
  EXPECT_EQ(sender.sent_texts.size(), 1u);
}
