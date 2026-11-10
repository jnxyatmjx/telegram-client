#include "telegram/MessageSender.h"

#include <common/Logger.h>

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

MessageSender::MessageSender(ITdSender& sender) : sender_(sender) {}

void MessageSender::SendMarkdown(std::int64_t chat_id,
                                 const std::string& text) const {
  for (const std::string& chunk : SplitForTelegram(text)) {
    auto content = td_api::make_object<td_api::inputMessageText>();
    auto formatted = td_api::make_object<td_api::formattedText>();
    formatted->text_ = chunk;
    content->text_ = std::move(formatted);
    auto preview = td_api::make_object<td_api::linkPreviewOptions>();
    preview->is_disabled_ = true;  // Python link_preview=False everywhere
    content->link_preview_options_ = std::move(preview);
    content->clear_draft_ = false;

    auto request = td_api::make_object<td_api::sendMessage>();
    request->chat_id_ = chat_id;
    request->topic_id_ = nullptr;
    request->reply_to_ = nullptr;
    request->options_ = nullptr;
    request->reply_markup_ = nullptr;
    request->input_message_content_ = std::move(content);

    const std::string head = chunk.substr(0, 200);
    sender_.Send(std::move(request),
                 [chat_id, head](td::ClientManager::Response&& response) {
                   if (response.object == nullptr) return;
                   if (response.object->get_id() == td_api::error::ID) {
                     const auto& error = static_cast<const td_api::error&>(
                         *response.object);
                     spdlog::error(
                         "send_text: sendMessage to {} failed: {}: {} "
                         "(text head: {:.80})", chat_id, error.code_,
                         error.message_, head);
                   }
                 });
  }
}

}  // namespace tg::telegram
