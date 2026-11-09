#include "features/auto_search/AutoSearchHandler.h"

#include <app/AppContext.h>
#include <common/Idempotency.h>
#include <common/Logger.h>

namespace tg::features::auto_search {

AutoSearchHandler::AutoSearchHandler(const app::AutoSearchConfig& config,
                                     media_thumbnail::SearchHandler& search)
    : config_(config), search_(search) {}

bool AutoSearchHandler::IsMediaMessage(const td_api::message& message) {
  if (message.content_ == nullptr) return false;
  switch (message.content_->get_id()) {
    case td_api::messagePhoto::ID:
    case td_api::messageVideo::ID:
    case td_api::messageAnimation::ID:
      return true;
    default:
      return false;
  }
}

bool AutoSearchHandler::IsAllowlisted(std::int64_t chat_id) const {
  for (const std::int64_t allowed : config_.chats) {
    if (allowed == chat_id) return true;
  }
  return false;
}

bool AutoSearchHandler::RecentlySearched(std::uint32_t oid,
                                         std::uint32_t id) const {
  const auto key = media_thumbnail::SearchCorrelationTable::Key(oid, id);
  const auto ttl = std::chrono::seconds(config_.dedup_ttl_seconds);
  std::lock_guard<std::mutex> lock(mutex_);
  // Opportunistic sweep of expired entries.
  const auto now = std::chrono::steady_clock::now();
  for (auto it = recent_.begin(); it != recent_.end();) {
    if (now - it->second > ttl) {
      it = recent_.erase(it);
    } else {
      ++it;
    }
  }
  return recent_.count(key) > 0;
}

bool AutoSearchHandler::CanHandle(
    const td_api::object_ptr<td_api::Update>& update) const {
  // Default config: enabled == false -> never claims (phase 2, v5 §2.7).
  if (!config_.enabled) return false;
  if (config_.chats.empty()) return false;
  if (update == nullptr || update->get_id() != td_api::updateNewMessage::ID) {
    return false;
  }
  const auto& new_message =
      static_cast<const td_api::updateNewMessage&>(*update);
  const td_api::message* message = new_message.message_.get();
  if (message == nullptr) return false;
  // chat_id allowlist only; sender intentionally unchecked (§2.7 step 2).
  if (!IsAllowlisted(message->chat_id_)) return false;
  return IsMediaMessage(*message);
}

void AutoSearchHandler::Handle(td_api::object_ptr<td_api::Update>& update,
                               app::AppContext& ctx) {
  const auto new_message =
      td_api::move_object_as<td_api::updateNewMessage>(std::move(update));
  const td_api::message* message = new_message->message_.get();
  if (message == nullptr) return;

  // Belt and braces: the dispatcher only calls Handle for claimed updates,
  // but Handle is also reachable directly — re-check the claim conditions
  // (allowlisted chat + media message) before doing any work.
  if (!IsAllowlisted(message->chat_id_) || !IsMediaMessage(*message)) {
    return;
  }

  const auto oid = common::ToBareChatId(message->chat_id_);
  const auto bare_id = common::ToBareMessageId(message->id_);
  if (!oid || !bare_id) {
    // v5 §1.3 hard rule 1: loud fail, never truncate.
    spdlog::error(
        "auto_search: chat_id {} / message_id {} do not fit uint32 bare ids "
        "— NOT searching", message->chat_id_, message->id_);
    return;
  }

  if (RecentlySearched(oid->oid, *bare_id)) {
    spdlog::info("auto_search: (oid={}, id={}) already searched within TTL — "
                 "skipped", oid->oid, *bare_id);
    return;
  }

  {
    const auto key = media_thumbnail::SearchCorrelationTable::Key(oid->oid, *bare_id);
    std::lock_guard<std::mutex> lock(mutex_);
    recent_[key] = std::chrono::steady_clock::now();
  }

  // SearchImages ONLY (never ProcessImages, v5 §2.7 step 5). Async
  // kick-off: returns immediately once the download starts; the outcome
  // is logged by the pipeline's finish stage.
  search_.SendSearchImages(*message, ctx);
}

}  // namespace tg::features::auto_search
