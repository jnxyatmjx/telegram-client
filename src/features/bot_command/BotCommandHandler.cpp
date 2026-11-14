#include "features/bot_command/BotCommandHandler.h"

#include <app/AppContext.h>
#include <common/Logger.h>
#include <features/bot_command/CommandParser.h>
#include <telegram/FloodRetry.h>
#include <telegram/TdErrors.h>

#include <memory>
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
    media_thumbnail::SearchHandler& search,
    telegram::FloodRetry* flood_retry)
    : config_(config),
      process_(std::move(process)),
      search_(search),
      flood_retry_(flood_retry) {
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

  // Async: send getMessage and return immediately — no worker thread waits
  // for the reply. The callback (Receive thread) only posts the pipeline
  // kick-off to the pool; the pipeline itself is async too, so one
  // command's slow getMessage/download can never block another message.
  // No timeout on the reply: a getMessage that never answers just means
  // this command is dropped (the bot can re-issue it); TDLib answers or
  // errors in practice.
  LookupTargetMessage(action, target->first, target->second, ctx, 1);
}

void BotCommandHandler::LookupTargetMessage(Action action,
                                            std::int64_t chat_id,
                                            std::int64_t message_id,
                                            app::AppContext& ctx,
                                            int attempt) {
  auto request = td_api::make_object<td_api::getMessage>();
  request->chat_id_ = chat_id;
  request->message_id_ = message_id;

  ctx.telegram.Send(
      std::move(request),
      [this, action, chat_id, message_id, attempt,
       &ctx](td::ClientManager::Response&& response) {
        if (response.object == nullptr) {
          spdlog::error("bot_command: getMessage({}, {}) returned no object",
                        chat_id, message_id);
          return;
        }
        if (response.object->get_id() == td_api::error::ID) {
          const auto error =
              td_api::move_object_as<td_api::error>(response.object);
          if (telegram::IsFloodWait(*error)) {
            // Rate-limited: re-issue the SAME getMessage after retry-after
            // + pad (it was refused, not applied). Bounded by the shared
            // scheduler's attempt cap; beyond it the command is dropped —
            // the bot can re-issue.
            if (flood_retry_ != nullptr &&
                flood_retry_->Schedule(
                    *error, attempt, [this, action, chat_id, message_id,
                                      attempt, &ctx] {
                      LookupTargetMessage(action, chat_id, message_id, ctx,
                                          attempt + 1);
                    })) {
              return;
            }
            spdlog::error(
                "bot_command: getMessage({}, {}) still flood-limited after "
                "{} attempts — command dropped", chat_id, message_id,
                attempt);
            return;
          }
          spdlog::error("bot_command: getMessage({}, {}) failed: code {}: {}",
                        chat_id, message_id, error->code_, error->message_);
          return;
        }
        // The message is move-only; ride it into the posted task via a
        // shared_ptr (the task must stay copyable for std::function).
        auto target_message = std::make_shared<
            td_api::object_ptr<td_api::message>>(
            td_api::move_object_as<td_api::message>(
                std::move(response.object)));
        const auto priority = action == Action::kProcess
                                  ? app::TaskPriority::kNormal
                                  : app::TaskPriority::kSearch;
        if (!ctx.workers.Post([this, action, target_message, &ctx] {
              if (action == Action::kProcess) {
                process_.SendProcessImages(**target_message, ctx);
              } else {
                search_.SendSearchImages(**target_message, ctx);
              }
            },
                             priority)) {
          spdlog::error("bot_command: worker pool rejected {} for ({}, {})",
                        action == Action::kProcess ? "process" : "search",
                        chat_id, message_id);
        }
      });
}

}  // namespace tg::features::bot_command
