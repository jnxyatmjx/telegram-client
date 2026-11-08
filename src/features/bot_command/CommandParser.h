#ifndef TG_FEATURES_BOT_COMMAND_COMMANDPARSER_H_
#define TG_FEATURES_BOT_COMMAND_COMMANDPARSER_H_

// CommandParser (v5 §2.6): "/command arg1 arg2" -> name + args.
//  - leading '/' required; command name lowercased; "@botname" suffix on the
//    command token is stripped (Telegram clients may append the target bot).
//  - target form 2 (besides reply-to): first two args are exactly
//    "<chat_id> <message_id>" as int64 decimals.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tg::features::bot_command {

struct ParsedCommand {
  std::string name;                    // lowercased, no '/', no '@bot'
  std::vector<std::string> args;
  std::optional<std::int64_t> target_chat_id;     // set when args parse as
  std::optional<std::int64_t> target_message_id;  // "chat_id message_id"
};

class CommandParser {
 public:
  // nullopt when text is not a command (no leading '/', empty name).
  static std::optional<ParsedCommand> Parse(const std::string& text);

  // "chat_id message_id" -> pair; nullopt on anything else.
  static std::optional<std::pair<std::int64_t, std::int64_t>> ParseTargetArgs(
      const std::vector<std::string>& args);

  // Strips a "@botname" suffix from a command token.
  static std::string StripBotSuffix(const std::string& command_token);
};

}  // namespace tg::features::bot_command

#endif  // TG_FEATURES_BOT_COMMAND_COMMANDPARSER_H_
