#ifndef TG_FEATURES_MEDIA_CHANNELS_MEDIACHANNELHANDLER_H_
#define TG_FEATURES_MEDIA_CHANNELS_MEDIACHANNELHANDLER_H_

// MediaChannelHandler (Telethon migration Chunk 2; Python handmedias
// 1255-1269): whitelist chats whose media goes through the ProcessImages
// pipeline AUTOMATICALLY — the Python main chain (is_target_media_chat ->
// mss_queue.put([message])). DISABLED BY DEFAULT: with realtime ImageReply
// watermarks driving the state store, auto-forwarding every whitelisted
// message is an opt-in behaviour (features.media_channels.enabled: false).
//
// Config chats are BARE MTProto ids — the same id space as chathis.chat_id
// and the /rsme listing — matched against ToBareChatId(message.chat_id_).
//
// rpc selects the pipeline: "process" (ProcessImages, the Python main
// chain) or "search" (SearchImages). Default "process".
//
// First-claimant-wins: the dispatcher consults handlers in registration
// order. If auto_search is also enabled for the same chat, WHICHEVER of the
// two handlers is registered first claims the update — register deliberately.
//
// Sender intentionally unchecked (channel posts may have no sender; same as
// AutoSearchHandler). Dedup is NOT needed: ProcessImages dedupes server-side
// on image_id = str(message_id).

#include <app/Config.h>
#include <features/media_thumbnail/MediaThumbnailHandler.h>
#include <features/media_thumbnail/SearchHandler.h>
#include <telegram/UpdateHandler.h>

#include <cstdint>

namespace tg::features::media_channels {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

class MediaChannelHandler : public telegram::IUpdateHandler {
 public:
  MediaChannelHandler(const app::MediaChannelsConfig& config,
                      media_thumbnail::MediaThumbnailHandler process,
                      media_thumbnail::SearchHandler& search);

  bool CanHandle(
      const td_api::object_ptr<td_api::Update>& update) const override;
  void Handle(td_api::object_ptr<td_api::Update>& update,
              app::AppContext& ctx) override;
  const char* Name() const override { return "MediaChannelHandler"; }

  // Exposed for unit tests: is this chat one of the configured bare ids?
  bool IsWhitelisted(std::int64_t dialog_chat_id) const;

 private:
  app::MediaChannelsConfig config_;
  media_thumbnail::MediaThumbnailHandler process_;
  media_thumbnail::SearchHandler& search_;
};

}  // namespace tg::features::media_channels

#endif  // TG_FEATURES_MEDIA_CHANNELS_MEDIACHANNELHANDLER_H_
