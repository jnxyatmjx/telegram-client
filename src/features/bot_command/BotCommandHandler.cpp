#include "features/bot_command/BotCommandHandler.h"

#include <app/AppContext.h>
#include <common/Logger.h>
#include <features/bot_command/CommandParser.h>

#include <chrono>
#include <optional>
#include <utility>

namespace tg::features::bot_command {
namespace {

std::string NormalizeCommand(std::string command) {
  // "/process@MyBot" -> "process"
  const size_t at = command.find('@');
  if (at != std::string::npos) command = command.substr(0, at);
  while (!command.empty() && command.front() == '/') {
    command.erase(command.begin());
  }
  for (char& c : command) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return command;
}

bool IsSenderUser(const td_api::message& message, std::int64_t expected_user_id) {
  if (message.sender_id_ == nullptr ||
      message.sender_id_->get_id() != td_api::messageSenderUser::ID) {
    return false;
  }
  // Read-only: the message is borrowed, so downcast without moving it out.
  const auto* sender = static_cast<const td_api::messageSenderUser*>(
      message.sender_id_.get());
  return sender->user_id_ == expected_user_id;
}

const std::string* CommandText(const td_api::message& message) {
  if (message.content_ == nullptr ||
      message.content_->get_id() != td_api::messageText::ID) {
    return nullptr;
  }
  const auto* content =
      static_cast<const td_api::messageText*>(message.content_.get());
  return &content->text_->text_;
}

// Reply-to target: (chat_id, message_id); same-chat replies carry chat_id 0.
std::optional<std::pair<std::int64_t, std::int64_t>> ReplyToTarget(
    const td_api::message& message) {
  if (message.reply_to_ == nullptr ||
      message.reply_to_->get_id() != td_api::messageReplyToMessage::ID) {
    return std::nullopt;
  }
  const auto* reply = static_cast<const td_api::messageReplyToMessage*>(
      message.reply_to_.get());
  const std::int64_t chat_id =
      reply->chat_id_ != 0 ? reply->chat_id_ : message.chat_id_;
  return std::make_pair(chat_id, reply->message_id_);
}

}  // namespace

BotCommandHandler::BotCommandHandler(
    const app::BotCommandConfig& config,
    media_thumbnail::MediaThumbnailHandler process,
    media_thumbnail::SearchHandler& search)
    : config_(config),
      process_(std::move(process)),
      search_(search) {
  config_.process_command = NormalizeCommand(config_.process_command);
  config_.search_command = NormalizeCommand(config_.search_command);
}

BotCommandHandler::Action BotCommandHandler::Classify(
    const std::string& text, const app::BotCommandConfig& config) {
  const auto parsed = CommandParser::Parse(text);
  if (!parsed) return Action::kIgnore;
  // Normalize here too: Classify is static and may be called with a config
  // that has not gone through the (normalizing) constructor.
  if (parsed->name == NormalizeCommand(config.process_command)) {
    return Action::kProcess;
  }
  if (parsed->name == NormalizeCommand(config.search_command)) {
    return Action::kSearch;
  }
  return Action::kIgnore;
}

bool BotCommandHandler::CanHandle(
    const td_api::object_ptr<td_api::Update>& update) const {
  if (!config_.enabled) return false;
  if (config_.command_bot_user_id == 0) return false;
  if (update == nullptr || update->get_id() != td_api::updateNewMessage::ID) {
    return false;
  }
  const auto& new_message =
      static_cast<const td_api::updateNewMessage&>(*update);
  const td_api::message* message = new_message.message_.get();
  if (message == nullptr) return false;
  // Trigger mode (a): the configured bot must be the SENDER (v5 §2.6).
  if (!IsSenderUser(*message, config_.command_bot_user_id)) return false;
  const std::string* text = CommandText(*message);
  if (text == nullptr) return false;
  return Classify(*text, config_) != Action::kIgnore;
}

void BotCommandHandler::Handle(td_api::object_ptr<td_api::Update>& update,
                               app::AppContext& ctx) {
  const auto new_message =
      td_api::move_object_as<td_api::updateNewMessage>(std::move(update));
  const td_api::message* message = new_message->message_.get();
  if (message == nullptr) return;

  const std::string* text = CommandText(*message);
  if (text == nullptr) return;

  // Belt and braces: CanHandle already ran, but Handle is also reachable
  // directly (tests, future triggers).
  const Action action = Classify(*text, config_);
  if (action == Action::kIgnore) {
    spdlog::debug("bot_command: unknown command — ignored");
    return;
  }
  if (!IsSenderUser(*message, config_.command_bot_user_id)) {
    spdlog::warn("bot_command: sender {} is not the configured bot {} — ignored",
                 message->chat_id_, config_.command_bot_user_id);
    return;
  }

  // Target resolution (v5 §2.6 step 3): reply-to first, then
  // "chat_id message_id" arguments.
  std::optional<std::pair<std::int64_t, std::int64_t>> target =
      ReplyToTarget(*message);
  if (!target) {
    const auto parsed = CommandParser::Parse(*text);
    if (parsed && parsed->target_chat_id && parsed->target_message_id) {
      target = std::make_pair(*parsed->target_chat_id, *parsed->target_message_id);
    }
  }
  if (!target) {
    spdlog::error(
        "bot_command: {} command has neither reply-to nor chat_id/message_id "
        "args — ignored", config_.process_command);
    return;
  }

  auto request = td_api::make_object<td_api::getMessage>();
  request->chat_id_ = target->first;
  request->message_id_ = target->second;
  auto future = ctx.telegram.SendWithFuture(std::move(request));

  const auto timeout = std::chrono::milliseconds(config_.get_message_timeout_ms);
  if (future.wait_for(timeout) != std::future_status::ready) {
    spdlog::error("bot_command: getMessage({}, {}) timed out", target->first,
                  target->second);
    return;
  }
  auto response = future.get();
  if (response.object == nullptr) {
    spdlog::error("bot_command: getMessage returned no object");
    return;
  }
  if (response.object->get_id() == td_api::error::ID) {
    const auto error = td_api::move_object_as<td_api::error>(response.object);
    spdlog::error("bot_command: getMessage({}, {}) failed: code {}: {}",
                  target->first, target->second, error->code_, error->message_);
    return;
  }
  const auto target_message =
      td_api::move_object_as<td_api::message>(std::move(response.object));

  media_thumbnail::PipelineResult result;
  if (action == Action::kProcess) {
    result = process_.SendProcessImages(*target_message, ctx);
  } else {
    result = search_.SendSearchImages(*target_message, ctx);
  }

  if (result.ok) {
    spdlog::info("bot_command: {} dispatched {}", action == Action::kProcess
                                                    ? "process"
                                                    : "search",
                 result.image_id);
  } else {
    spdlog::error("bot_command: {} failed: {}", result.image_id, result.error);
  }
}

}  // namespace tg::features::bot_command
