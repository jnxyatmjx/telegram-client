#include "features/media_channels/MediaChannelHandler.h"

#include <app/AppContext.h>
#include <common/Idempotency.h>
#include <common/Logger.h>

namespace tg::features::media_channels {

MediaChannelHandler::MediaChannelHandler(
    const app::MediaChannelsConfig& config,
    media_thumbnail::MediaThumbnailHandler process,
    media_thumbnail::SearchHandler& search)
    : config_(config), process_(std::move(process)), search_(search) {}

bool MediaChannelHandler::IsWhitelisted(std::int64_t dialog_chat_id) const {
  const auto bare = common::ToBareChatId(dialog_chat_id);
  if (!bare.has_value()) return false;
  for (const std::int64_t allowed : config_.chats) {
    if (allowed == static_cast<std::int64_t>(bare->oid)) return true;
  }
  return false;
}

bool MediaChannelHandler::CanHandle(
    const td_api::object_ptr<td_api::Update>& update) const {
  // Default config: enabled == false -> never claims.
  if (!config_.enabled || config_.chats.empty()) return false;
  if (update == nullptr || update->get_id() != td_api::updateNewMessage::ID) {
    return false;
  }
  const auto& new_message =
      static_cast<const td_api::updateNewMessage&>(*update);
  const td_api::message* message = new_message.message_.get();
  if (message == nullptr) return false;
  if (!IsWhitelisted(message->chat_id_)) return false;
  // handmedias: photo or video/animation media (the media pipeline's input).
  if (message->content_ == nullptr) return false;
  switch (message->content_->get_id()) {
    case td_api::messagePhoto::ID:
    case td_api::messageVideo::ID:
    case td_api::messageAnimation::ID:
      return true;
    default:
      return false;
  }
}

void MediaChannelHandler::Handle(td_api::object_ptr<td_api::Update>& update,
                                 app::AppContext& ctx) {
  const auto new_message =
      td_api::move_object_as<td_api::updateNewMessage>(std::move(update));
  const td_api::message* message = new_message->message_.get();
  if (message == nullptr) return;

  // Belt and braces: CanHandle already ran, but Handle is also reachable
  // directly (tests, future callers).
  if (!IsWhitelisted(message->chat_id_) || message->content_ == nullptr) {
    return;
  }

  const auto oid = common::ToBareChatId(message->chat_id_);
  if (!oid.has_value()) {
    // v5 §1.3 hard rule 1: loud fail, never truncate.
    spdlog::error("media_channel: chat_id {} does not fit a uint32 bare id — "
                  "NOT processing", message->chat_id_);
    return;
  }

  if (config_.rpc == "search") {
    search_.SendSearchImages(*message, ctx);
    return;
  }
  process_.SendProcessImages(*message, ctx);
}

}  // namespace tg::features::media_channels
