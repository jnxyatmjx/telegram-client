#include "telegram/MessageSender.h"

#include <common/Logger.h>
#include <telegram/FloodRetry.h>
#include <telegram/TdErrors.h>

namespace tg::telegram {
namespace {

// Note: count CHARACTERS not UTF-8 code points — the 4096 limit is enforced
// conservatively (multi-byte characters cost more bytes than chars, so a
// 4096--char chunk of CJK text can reach ~12 KiB bytes; Telegram counts
// characters the other way and would accept it — we only ever refuse early).
// The generated replies are mostly ASCII; the slack is harmless.

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t nl = text.find('\n', start);
    if (nl == std::string::npos) {
      lines.push_back(text.substr(start));
      break;
    }
    lines.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  return lines;
}

}  // namespace

std::vector<std::string> MessageSender::SplitForTelegram(
    const std::string& text, std::size_t max_chars) {
  if (max_chars == 0) max_chars = kMaxMessageChars;
  std::vector<std::string> chunks;
  std::string current;
  for (std::string& line : SplitLines(text)) {
    // A single oversized line is hard-split at the limit.
    while (line.size() > max_chars) {
      if (!current.empty()) {
        chunks.push_back(current);
        current.clear();
      }
      chunks.push_back(line.substr(0, max_chars));
      line = line.substr(max_chars);
    }
    if (current.size() + line.size() + 1 > max_chars && !current.empty()) {
      chunks.push_back(current);
      current.clear();
    }
    if (!current.empty()) current.push_back('\n');
    current += line;
  }
  chunks.push_back(current);
  return chunks;
}

MessageSender::MessageSender(ITdSender& sender, FloodRetry* flood_retry)
    : sender_(sender), flood_retry_(flood_retry) {}

void MessageSender::SendMarkdown(std::int64_t chat_id,
                                 const std::string& text) const {
  StartMarkdown(chat_id, MarkdownDelivery{}, text);
}

void MessageSender::SendMarkdownReply(std::int64_t chat_id,
                                      std::int64_t reply_to_message_id,
                                      const std::string& text,
                                      bool link_preview_enabled) const {
  MarkdownDelivery delivery;
  delivery.reply_to_message_id = reply_to_message_id;
  delivery.link_preview_enabled = link_preview_enabled;
  StartMarkdown(chat_id, delivery, text);
}

void MessageSender::StartMarkdown(std::int64_t chat_id,
                                  MarkdownDelivery delivery,
                                  const std::string& text) const {
  auto chunks = std::make_shared<const std::vector<std::string>>(
      SplitForTelegram(text));
  SendChunksInOrder(chat_id, delivery, chunks, 0);
}

void MessageSender::SendChunksInOrder(
    std::int64_t chat_id, MarkdownDelivery delivery,
    std::shared_ptr<const std::vector<std::string>> chunks,
    std::size_t index) const {
  if (index >= chunks->size()) return;
  ParseAndSendChunk(chat_id, delivery, (*chunks)[index], 1,
                    [this, chat_id, delivery, chunks, index] {
                      SendChunksInOrder(chat_id, delivery, chunks, index + 1);
                    });
}

void MessageSender::ParseAndSendChunk(std::int64_t chat_id,
                                      MarkdownDelivery delivery,
                                      const std::string& chunk, int attempt,
                                      std::function<void()> on_done) const {
  // TDLib parses markdown locally (no network), so this is fast — but it
  // is still an async request through the client, hence the callback chain.
  auto parse_request = td_api::make_object<td_api::parseTextEntities>();
  parse_request->text_ = chunk;
  auto mode = td_api::make_object<td_api::textParseModeMarkdown>();
  // Version 2 = MarkdownV2, whose **bold** matches Python/Telethon's
  // "markdown" output. Version 0/1 is the classic flavour (single * for
  // bold) and would silently strip the ** in our /rsme format.
  mode->version_ = 2;
  parse_request->parse_mode_ = std::move(mode);

  sender_.Send(std::move(parse_request),
               [this, chat_id, delivery, chunk, attempt,
                on_done = std::move(on_done)](
                   td::ClientManager::Response&& response) mutable {
                 td_api::object_ptr<td_api::formattedText> formatted;
                 if (response.object != nullptr &&
                     response.object->get_id() == td_api::formattedText::ID) {
                   formatted = td_api::move_object_as<td_api::formattedText>(
                       std::move(response.object));
                 } else {
                   // Parse failed (malformed markdown — e.g. an unescaped
                   // special character in a display name). Fall back to
                   // the raw chunk as plain text so the reply still goes
                   // through; the caller should escape user-controlled
                   // text to avoid hitting this.
                   std::string why = "<no object>";
                   if (response.object != nullptr &&
                       response.object->get_id() == td_api::error::ID) {
                     const auto& error =
                         static_cast<const td_api::error&>(*response.object);
                     why = std::to_string(error.code_) + ": " + error.message_;
                   }
                   spdlog::warn("send_text: markdown parse failed ({}) — "
                                "sending chunk as plain text (head: {:.80})",
                                why, chunk);
                   formatted = td_api::make_object<td_api::formattedText>();
                   formatted->text_ = chunk;
                 }
                 SendParsedChunk(chat_id, delivery, std::move(formatted), chunk,
                                 attempt, std::move(on_done));
               });
}

void MessageSender::SendParsedChunk(
    std::int64_t chat_id, MarkdownDelivery delivery,
    td_api::object_ptr<td_api::formattedText> formatted,
    const std::string& chunk, int attempt,
    std::function<void()> on_done) const {
  auto content = td_api::make_object<td_api::inputMessageText>();
  content->text_ = std::move(formatted);
  auto preview = td_api::make_object<td_api::linkPreviewOptions>();
  // SendMarkdown keeps previews off. Search "No One Find" turns them on.
  preview->is_disabled_ = !delivery.link_preview_enabled;
  content->link_preview_options_ = std::move(preview);
  content->clear_draft_ = false;

  auto request = td_api::make_object<td_api::sendMessage>();
  request->chat_id_ = chat_id;
  request->topic_id_ = nullptr;
  if (delivery.reply_to_message_id > 0) {
    request->reply_to_ = td_api::make_object<td_api::inputMessageReplyToMessage>(
        delivery.reply_to_message_id, nullptr, 0, "");
  }
  request->options_ = nullptr;
  request->reply_markup_ = nullptr;
  request->input_message_content_ = std::move(content);

  sender_.Send(
      std::move(request),
      [this, chat_id, delivery, chunk, attempt](
          td::ClientManager::Response&& response) {
        if (response.object == nullptr) return;
        if (response.object->get_id() == td_api::error::ID) {
          const auto& error =
              static_cast<const td_api::error&>(*response.object);
          if (IsFloodWait(error)) {
            // Rate-limited: re-parse and re-send the SAME chunk after
            // retry-after + pad (it was never accepted), bounded by the
            // shared scheduler's attempt cap. Re-parsing (local, fast)
            // preserves the markdown entities, the reply target, and the
            // preview flag.
            if (flood_retry_ != nullptr &&
                flood_retry_->Schedule(
                    error, attempt,
                    [this, chat_id, delivery, chunk, attempt] {
                      ParseAndSendChunk(chat_id, delivery, chunk, attempt + 1,
                                        [] {});
                    },
                    // Search replies carry reply_to. Control notices do not.
                    delivery.reply_to_message_id > 0 ? 1 : 0)) {
              return;
            }
            spdlog::error("send_text: sendMessage to {} still flood-limited "
                          "after {} attempts — chunk dropped (head: {:.80})",
                          chat_id, attempt, chunk);
            return;
          }
          spdlog::error("send_text: sendMessage to {} failed: {}: {} "
                        "(text head: {:.80})",
                        chat_id, error.code_, error.message_, chunk);
        }
      });
  // The send has been dispatched; the next chunk may start. (A FloodWait
  // retry re-parses and re-sends this chunk later, so it can land after a
  // later chunk — same as the pre-parse behaviour.)
  if (on_done) on_done();
}

}  // namespace tg::telegram
