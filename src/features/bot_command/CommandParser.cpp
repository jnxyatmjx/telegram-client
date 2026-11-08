#include "features/bot_command/CommandParser.h"

#include <cctype>

namespace tg::features::bot_command {
namespace {

std::string ToLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::vector<std::string> Tokenize(const std::string& text) {
  std::vector<std::string> tokens;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() &&
           std::isspace(static_cast<unsigned char>(text[i]))) {
      ++i;
    }
    const size_t start = i;
    while (i < text.size() &&
           !std::isspace(static_cast<unsigned char>(text[i]))) {
      ++i;
    }
    if (i > start) tokens.push_back(text.substr(start, i - start));
  }
  return tokens;
}

bool ParseInt64(const std::string& token, std::int64_t* out) {
  if (token.empty()) return false;
  std::int64_t value = 0;
  int sign = 1;
  size_t i = 0;
  if (token[0] == '-') {
    sign = -1;
    i = 1;
  } else if (token[0] == '+') {
    i = 1;
  }
  if (i >= token.size()) return false;
  for (; i < token.size(); ++i) {
    const char c = token[i];
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    const int digit = c - '0';
    if (value > (9223372036854775807LL - digit) / 10) return false;  // overflow
    value = value * 10 + digit;
  }
  *out = sign * value;
  return true;
}

}  // namespace

std::string CommandParser::StripBotSuffix(const std::string& command_token) {
  const size_t at = command_token.find('@');
  if (at == std::string::npos) return command_token;
  return command_token.substr(0, at);
}

std::optional<ParsedCommand> CommandParser::Parse(const std::string& text) {
  const auto tokens = Tokenize(text);
  if (tokens.empty() || tokens[0].empty() || tokens[0][0] != '/') {
    return std::nullopt;
  }

  ParsedCommand parsed;
  parsed.name = ToLower(StripBotSuffix(tokens[0].substr(1)));
  if (parsed.name.empty()) return std::nullopt;
  parsed.args.assign(tokens.begin() + 1, tokens.end());

  if (auto target = ParseTargetArgs(parsed.args)) {
    parsed.target_chat_id = target->first;
    parsed.target_message_id = target->second;
  }
  return parsed;
}

std::optional<std::pair<std::int64_t, std::int64_t>>
CommandParser::ParseTargetArgs(const std::vector<std::string>& args) {
  if (args.size() != 2) return std::nullopt;
  std::int64_t chat_id = 0;
  std::int64_t message_id = 0;
  if (!ParseInt64(args[0], &chat_id) || !ParseInt64(args[1], &message_id)) {
    return std::nullopt;
  }
  return std::make_pair(chat_id, message_id);
}

}  // namespace tg::features::bot_command
