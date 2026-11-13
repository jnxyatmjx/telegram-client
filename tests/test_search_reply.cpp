// SearchRsp -> one Telegram reply (app_bot_client.rspwork / rspParse).
#include <features/auto_search/SearchReply.h>
#include <features/media_thumbnail/SearchHandler.h>
#include <telegram/MessageSender.h>

#include <td/telegram/MessageEntity.h>
#include <td/telegram/td_api.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace td_api = td::td_api;
namespace search = tg::features::auto_search;

namespace {

std::optional<std::string> Plain(const std::string& token) { return token; }

std::optional<std::string> NoUrl(std::int64_t) { return std::nullopt; }

const char* kPlaceholder = "🎯 ✅🔥 ✅⚡️ ✅⚡️ ✅🔥 ✅🎯 ";

std::string Repeat(const std::string& unit, int n) {
  std::string out;
  for (int i = 0; i < n; ++i) out += unit;
  return out;
}

// parse_markdown_v2 offsets are UTF-16 code units, not bytes.
std::string SliceUtf16(const std::string& utf8, int32_t offset, int32_t length) {
  std::string out;
  int32_t units = 0;
  for (std::size_t i = 0; i < utf8.size() && units < offset + length;) {
    const auto c = static_cast<unsigned char>(utf8[i]);
    std::size_t len = 1;
    int32_t span = 1;
    if ((c & 0x80) == 0) {
      len = 1;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
      len = 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
      len = 3;
    } else if ((c & 0xF8) == 0xF0 && i + 3 < utf8.size()) {
      len = 4;
      span = 2;  // surrogate pair
    }
    if (units >= offset) out.append(utf8, i, len);
    units += span;
    i += len;
  }
  return out;
}

void ExpectParses(const search::SearchReplyMessage& message,
                  const std::string& plain,
                  const std::vector<std::pair<std::string, std::string>>&
                      bold_and_url) {
  td::string text = message.text;
  auto parsed = td::parse_markdown_v2(text);
  if (parsed.is_error()) {
    const auto err = parsed.error().message();
    ADD_FAILURE() << std::string(err.data(), err.size()) << "\n" << message.text;
    return;
  }
  EXPECT_EQ(text, plain);
  std::vector<std::string> bolds;
  std::vector<std::pair<std::string, std::string>> urls;
  for (const auto& entity : parsed.ok()) {
    if (entity.type == td::MessageEntity::Type::Bold) {
      bolds.push_back(SliceUtf16(text, entity.offset, entity.length));
      EXPECT_GT(entity.length, 0);
    } else if (entity.type == td::MessageEntity::Type::TextUrl) {
      urls.emplace_back(SliceUtf16(text, entity.offset, entity.length),
                        entity.argument);
    }
  }
  ASSERT_EQ(bolds.size(), bold_and_url.size());
  ASSERT_EQ(urls.size(), bold_and_url.size());
  for (std::size_t i = 0; i < bold_and_url.size(); ++i) {
    EXPECT_EQ(bolds[i], bold_and_url[i].first);
    EXPECT_EQ(urls[i].second, bold_and_url[i].second);
  }
}

class FakeTdSender : public tg::telegram::ITdSender {
 public:
  std::int64_t Send(td_api::object_ptr<td_api::Function>&& request,
                    tg::telegram::ResponseCallback callback) override {
    td::ClientManager::Response response;
    response.request_id = next_id_++;
    if (request->get_id() == td_api::parseTextEntities::ID) {
      const auto& parse =
          static_cast<const td_api::parseTextEntities&>(*request);
      auto formatted = td_api::make_object<td_api::formattedText>();
      formatted->text_ = parse.text_;
      response.object = std::move(formatted);
    } else if (request->get_id() == td_api::sendMessage::ID) {
      const auto& send = static_cast<const td_api::sendMessage&>(*request);
      chat_id = send.chat_id_;
      const auto& content = static_cast<const td_api::inputMessageText&>(
          *send.input_message_content_);
      text = content.text_->text_;
      preview_disabled = content.link_preview_options_ != nullptr &&
                         content.link_preview_options_->is_disabled_;
      reply_to = 0;
      if (send.reply_to_ != nullptr &&
          send.reply_to_->get_id() == td_api::inputMessageReplyToMessage::ID) {
        reply_to = static_cast<const td_api::inputMessageReplyToMessage&>(
                       *send.reply_to_)
                       .message_id_;
      }
      response.object = td_api::make_object<td_api::message>();
    } else {
      response.object = td_api::make_object<td_api::error>(400, "unexpected");
    }
    callback(std::move(response));
    return static_cast<std::int64_t>(response.request_id);
  }

  std::int64_t chat_id = 0;
  std::int64_t reply_to = 0;
  std::string text;
  bool preview_disabled = true;

 private:
  std::uint64_t next_id_ = 1;
};

}  // namespace

TEST(SearchResultLink, HttpUrlIsConcatenatedPlaceholderFallsBack) {
  EXPECT_EQ(search::SearchResultLink(42, 7, std::string("https://t.me/name/")),
            "https://t.me/name/7");
  EXPECT_EQ(search::SearchResultLink(42, 7, std::string("http://example.com/c/")),
            "http://example.com/c/7");
  EXPECT_EQ(search::SearchResultLink(42, 7, std::string(">>>>Nrror>42")),
            "https://t.me/c/42/7");
  EXPECT_EQ(search::SearchResultLink(42, 7, std::string(">>Chat>title")),
            "https://t.me/c/42/7");
  EXPECT_EQ(search::SearchResultLink(42, 7, std::nullopt),
            "https://t.me/c/42/7");
}

TEST(FormatSearchReply, EscapesMemoAndNumberAndUsesBareLink) {
  const std::string result =
      R"({"code":0,"data":[{"memo":"tok","cid":3519704595,"mid":88,"distance":0.91}]})";
  const auto message = search::FormatSearchReply(
      result,
      [](const std::string&) {
        return std::optional<std::string>("a-b (c)!");
      },
      NoUrl);
  EXPECT_FALSE(message.link_preview_enabled);
  EXPECT_EQ(message.text,
            "*1*\\. [a\\-b \\(c\\)\\!](https://t.me/c/3519704595/88)\n");
}

TEST(FormatSearchReply, EmptyMemoUsesPlaceholderAndHttpChatUrl) {
  const std::string result =
      R"({"data":[{"memo":"","cid":"9","mid":"3"}]})";
  const auto message = search::FormatSearchReply(
      result, Plain,
      [](std::int64_t) {
        return std::optional<std::string>("https://t.me/publicxme/");
      });
  EXPECT_FALSE(message.link_preview_enabled);
  EXPECT_NE(message.text.find(kPlaceholder), std::string::npos);
  EXPECT_NE(message.text.find("](https://t.me/publicxme/3)"), std::string::npos);
}

TEST(FormatSearchReply, TruncatesTo28CodePointsCjkAndEmoji) {
  const std::string cjk = Repeat("中", 30);
  const std::string emoji = Repeat("🎯", 29);
  const auto cjk_message = search::FormatSearchReply(
      R"({"data":[{"memo":"x","cid":1,"mid":2}]})",
      [&](const std::string&) { return std::optional<std::string>(cjk); },
      NoUrl);
  const auto emoji_message = search::FormatSearchReply(
      R"({"data":[{"memo":"x","cid":1,"mid":2}]})",
      [&](const std::string&) { return std::optional<std::string>(emoji); },
      NoUrl);
  EXPECT_NE(cjk_message.text.find(Repeat("中", 28)), std::string::npos);
  EXPECT_EQ(cjk_message.text.find(Repeat("中", 29)), std::string::npos);
  EXPECT_NE(emoji_message.text.find(Repeat("🎯", 28)), std::string::npos);
  EXPECT_EQ(emoji_message.text.find(Repeat("🎯", 29)), std::string::npos);
}

TEST(FormatSearchReply, DecryptFailureUsesPlaceholder) {
  const auto message = search::FormatSearchReply(
      R"({"data":[{"memo":"bad","cid":1,"mid":2}]})",
      [](const std::string&) { return std::optional<std::string>{}; }, NoUrl);
  EXPECT_FALSE(message.link_preview_enabled);
  EXPECT_NE(message.text.find(kPlaceholder), std::string::npos);
  EXPECT_NE(message.text.find("https://t.me/c/1/2"), std::string::npos);
}

TEST(FormatSearchReply, NoMatchCasesTurnPreviewOn) {
  const char* cases[] = {
      "",
      "None",
      "Search 1 No Faces",
      "Image Empty 1",
      "Search boom",
      R"({"code":0})",
      R"({"data":[]})",
      R"({"data":[{"distance":0.1}]})",
  };
  for (const char* result : cases) {
    const auto message = search::FormatSearchReply(result, Plain, NoUrl);
    EXPECT_TRUE(message.link_preview_enabled) << result;
    EXPECT_NE(message.text.find("*No One Find*"), std::string::npos) << result;
    EXPECT_NE(message.text.find("https://t.me/uuuiii3"), std::string::npos)
        << result;
  }
}

TEST(DeliverSearchReply, RepliesToTheOriginalMessageNotTheOid) {
  FakeTdSender sender;
  tg::telegram::MessageSender messenger(sender);
  tg::features::media_thumbnail::SearchCorrelationTable::Entry target;
  // TDLib dialog id. The uint32 oid would be 1234567890; the result cid
  // is a different chat. Neither may be the sendMessage chat_id.
  target.chat_id = -1001234567890LL;
  target.message_id = 555;
  target.image_id = "555";
  const std::string result =
      R"({"data":[{"memo":"tok","cid":999,"mid":42}]})";
  search::DeliverSearchReply(
      messenger, target, result,
      [](const std::string&) { return std::optional<std::string>("hi"); },
      NoUrl);
  EXPECT_EQ(sender.chat_id, -1001234567890LL);
  EXPECT_EQ(sender.reply_to, 555);
  EXPECT_TRUE(sender.preview_disabled);
  EXPECT_NE(sender.text.find("*1*\\. [hi](https://t.me/c/999/42)"),
            std::string::npos);
  EXPECT_EQ(sender.text.find("-1001234567890"), std::string::npos);
}

TEST(FormatSearchReply, MarkdownV2ParsesBoldAndTextUrl) {
  // Hits: single-asterisk bold, escaped . - ( ) !, URL '.' left literal.
  const auto hits = search::FormatSearchReply(
      R"({"data":[{"memo":"tok","cid":3519704595,"mid":88},{"memo":"b","cid":1,"mid":2}]})",
      [](const std::string& token) -> std::optional<std::string> {
        if (token == "tok") return std::string("a-b (c)!.!");
        return std::string("hi\\x");
      },
      [](std::int64_t cid) -> std::optional<std::string> {
        if (cid == 1) return std::string("https://ex.ample/a)b\\");
        return std::nullopt;
      });
  ExpectParses(hits,
               "1. a-b (c)!.!\n2. hi\\x\n",
               {{"1", "https://t.me/c/3519704595/88"},
                {"2", "https://ex.ample/a)b\\2"}});
  // The second link label is the memo; the URL ate the escaped ')' and '\'.
  td::string hit_text = hits.text;
  auto hit_entities = td::parse_markdown_v2(hit_text);
  ASSERT_TRUE(hit_entities.is_ok());
  bool saw_tricky_label = false;
  for (const auto& entity : hit_entities.ok()) {
    if (entity.type == td::MessageEntity::Type::TextUrl &&
        entity.argument == "https://ex.ample/a)b\\2") {
      EXPECT_EQ(SliceUtf16(hit_text, entity.offset, entity.length), "hi\\x");
      saw_tricky_label = true;
    }
  }
  EXPECT_TRUE(saw_tricky_label);

  const auto none = search::FormatSearchReply("Search 1 No Faces", Plain, NoUrl);
  ExpectParses(none,
               "\n⚡️\tNo One Find\t🎯\n👉Maybe Here You Like👈",
               {{"No One Find", "https://t.me/uuuiii3"}});
  const auto empty = search::FormatSearchReply(R"({"data":[]})", Plain, NoUrl);
  ExpectParses(empty,
               "\n⚡️\tNo One Find\t🎯\n👉Maybe Here You Like👈",
               {{"No One Find", "https://t.me/uuuiii3"}});
  const auto missing = search::FormatSearchReply("None", Plain, NoUrl);
  ExpectParses(missing,
               "\n⚡️\tNo One Find\t🎯\n👉Maybe Here You Like👈",
               {{"No One Find", "https://t.me/uuuiii3"}});
}
