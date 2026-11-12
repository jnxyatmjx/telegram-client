#ifndef TG_TELEGRAM_MESSAGESENDER_H_
#define TG_TELEGRAM_MESSAGESENDER_H_

// MessageSender — the single sendMessage path (Telethon migration Chunk 2,
// SURVEY §7.2-6 "SendText"). Python replies with event.reply(...,
// link_preview=False, parse_mode="markdown") in /rsme 45-line batches;
// TDLib has no auto-split, and Telethon's 45-line batches can exceed the
// 4096-character hard limit (SURVEY §8.5-2) — so this wrapper splits at
// 4096 chars (line boundaries preferred) and disables link previews on
// every chunk.
//
// Markdown rendering (fixed 2026-11-12): TDLib does NOT parse markdown
// from inputMessageText — a formattedText with raw "[text](url)" / "**bold**"
// source is sent as literal plain text. So SendMarkdown first runs each
// chunk through parseTextEntities with textParseModeMarkdown version 2
// (MarkdownV2 — the flavour whose **bold** matches Python/Telethon's
// "markdown" output) and sends the returned formattedText. A chunk whose
// markdown fails to parse (e.g. an unescaped special character in a
// display name) falls back to plain text so the reply still goes through.
// Callers MUST escape MarkdownV2 specials in user-controlled text
// (_ * [ ] ( ) ~ ` > # + - = | { } . !) before handing it to SendMarkdown.
//
// Chunks are sent strictly in order: chunk N+1 is only parsed after chunk
// N's sendMessage has been dispatched, so a multi-chunk reply cannot
// arrive scrambled. (A FloodWait retry re-sends a chunk later, so a
// retried chunk can still land after a later chunk — same as before.)
//
// Fire-and-forget: sends are async, errors are logged with the chat id and
// the first 200 chars of the chunk. No retry for ordinary errors — a failed
// reply is a lost reply (the command can be re-issued), matching the Python
// client. The one exception (Telethon migration chunk 3): a TDLib 429
// FloodWait is retried through the shared telegram::FloodRetry — the
// rejected chunk is re-sent after retry-after + pad, at most max_attempts
// times, then dropped with an error log. A 429 means the message was never
// accepted by Telegram, so re-sending it cannot duplicate.

#include <telegram/TelegramClient.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tg::telegram {

class FloodRetry;

class MessageSender {
 public:
  // Telegram's hard per-message limit in characters.
  static constexpr std::size_t kMaxMessageChars = 4096;

  // `flood_retry` is the shared 429 scheduler (Application passes one);
  // null keeps the pre-chunk-3 behaviour — a 429 is logged and the chunk
  // dropped. Production code MUST pass a FloodRetry.
  explicit MessageSender(ITdSender& sender, FloodRetry* flood_retry = nullptr);

  // Splits `text` into chunks of at most max_chars characters: whole lines
  // are kept together while they fit; a single line longer than the limit
  // is hard-split at the limit. Empty text produces one empty chunk (an
  // intentional empty reply still sends).
  static std::vector<std::string> SplitForTelegram(
      const std::string& text,
      std::size_t max_chars = kMaxMessageChars);

  // Sends `text` as markdown (MarkdownV2, parsed via parseTextEntities)
  // with link previews disabled, in as many messages as the 4096-char
  // split needs, strictly in order. Safe from any thread.
  void SendMarkdown(std::int64_t chat_id, const std::string& text) const;

 private:
  // Sends the chunks of one SendMarkdown call strictly in order: chunk
  // `index` is parsed and dispatched, then the next chunk starts. `chunks`
  // is shared so the chain can outlive the SendMarkdown call.
  void SendChunksInOrder(
      std::int64_t chat_id,
      std::shared_ptr<const std::vector<std::string>> chunks,
      std::size_t index) const;

  // Parses one chunk as MarkdownV2, then sends it (via SendParsedChunk)
  // and invokes `on_done` once the send has been dispatched. A parse
  // failure falls back to the raw chunk as plain text.
  void ParseAndSendChunk(std::int64_t chat_id, const std::string& chunk,
                         int attempt,
                         std::function<void()> on_done) const;

  // Sends one already-parsed chunk (attempt 1 from ParseAndSendChunk; the
  // flood retry path re-parses `chunk` and re-enters with attempt + 1).
  // `on_done` runs once the send has been dispatched.
  void SendParsedChunk(
      std::int64_t chat_id,
      td_api::object_ptr<td_api::formattedText> formatted,
      const std::string& chunk, int attempt,
      std::function<void()> on_done) const;

  ITdSender& sender_;
  FloodRetry* flood_retry_;  // not owned; may be null (see ctor comment)
};

}  // namespace tg::telegram

#endif  // TG_TELEGRAM_MESSAGESENDER_H_
