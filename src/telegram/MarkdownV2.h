#ifndef TG_TELEGRAM_MARKDOWNV2_H_
#define TG_TELEGRAM_MARKDOWNV2_H_

// Escaping for text handed to MessageSender::SendMarkdown.
//
// SendMarkdown parses with textParseModeMarkdown version 2. A reserved
// character that is not escaped makes Telegram answer 400 and the chunk
// falls back to plain text.
//
// Outside a code span, escape every one of
// _ * [ ] ( ) ~ ` > # + - = | { } . ! and backslash.
// Inside a code span the content is literal: only backtick and backslash
// are special.

#include <string>

namespace tg::telegram {

inline std::string EscapeMarkdownV2(const std::string& text) {
  std::string out;
  out.reserve(text.size() * 2);
  for (const char c : text) {
    switch (c) {
      case '_':
      case '*':
      case '[':
      case ']':
      case '(':
      case ')':
      case '~':
      case '`':
      case '>':
      case '#':
      case '+':
      case '-':
      case '=':
      case '|':
      case '{':
      case '}':
      case '.':
      case '!':
      case '\\':
        out.push_back('\\');
        break;
      default:
        break;
    }
    out.push_back(c);
  }
  return out;
}

inline std::string EscapeCodeSpan(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (const char c : text) {
    if (c == '`' || c == '\\') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

}  // namespace tg::telegram

#endif  // TG_TELEGRAM_MARKDOWNV2_H_
