#ifndef TG_FEATURES_BOT_COMMAND_BOTCOMMANDHANDLER_H_
#define TG_FEATURES_BOT_COMMAND_BOTCOMMANDHANDLER_H_

// BotCommandHandler (v5 §2.6): the ONLY ProcessImages/SearchImages trigger in
// phase 1. Trigger mode (a) — the bot itself SENDS the command message, so we
// verify sender_user_id == command_bot_user_id.
//
// CanHandle (cheap, Receive thread): updateNewMessage + enabled + right
// sender + messageText starting with a configured command.
// Handle (worker thread): resolve the target message (reply-to, or
// "chat_id message_id" args) and send getMessage — then RETURN. The
// getMessage reply arrives on the Receive thread, which posts the pipeline
// kick-off (MediaThumbnailHandler / SearchHandler, both async) to the pool.
// No stage ever blocks on another message's I/O (2026-11-09).

#include <app/Config.h>
#include <features/media_thumbnail/MediaThumbnailHandler.h>
#include <features/media_thumbnail/SearchHandler.h>
#include <telegram/UpdateHandler.h>

namespace tg::features::bot_command {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

class BotCommandHandler : public telegram::IUpdateHandler {
 public:
  BotCommandHandler(const app::BotCommandConfig& config,
                    media_thumbnail::MediaThumbnailHandler process,
                    media_thumbnail::SearchHandler& search);

  bool CanHandle(
      const td_api::object_ptr<td_api::Update>& update) const override;
  void Handle(td_api::object_ptr<td_api::Update>& update,
              app::AppContext& ctx) override;
  const char* Name() const override { return "BotCommandHandler"; }

  // Exposed for unit tests: what would we do with this text?
  enum class Action { kIgnore, kProcess, kSearch };
  static Action Classify(const std::string& text,
                         const app::BotCommandConfig& config);

 private:
  app::BotCommandConfig config_;
  media_thumbnail::MediaThumbnailHandler process_;
  media_thumbnail::SearchHandler& search_;
};

}  // namespace tg::features::bot_command

#endif  // TG_FEATURES_BOT_COMMAND_BOTCOMMANDHANDLER_H_
