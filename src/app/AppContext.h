#ifndef TG_APP_APPCONTEXT_H_
#define TG_APP_APPCONTEXT_H_

// AppContext (architecture v5 §3.5): one object passed through every
// handler; references owned by Application, which outlives all workers.
// Deviation from v5: `telegram` is the ITdSender& seam (TelegramClient
// implements it) so handlers are unit-testable against a fake sender.

#include <app/Config.h>
#include <infra/Outbox.h>
#include <telegram/FileDownloader.h>
#include <telegram/TelegramClient.h>
#include <transport/ProcessImagesClient.h>
#include <transport/SearchImagesClient.h>
#include <transport/StreamerPool.h>

namespace tg::app {

struct AppContext {
  telegram::ITdSender& telegram;
  telegram::FileDownloader& files;
  // Pools deal requests round-robin across grpc.connections lanes; the
  // handlers only need Enqueue, so they are unchanged by the lane count.
  transport::StreamerPool<ImageRequest, ImageReply>& process_images;
  transport::StreamerPool<SearchReq, SearchRsp>& search_images;
  infra::Outbox& outbox;
  const Config& config;
};

}  // namespace tg::app

#endif  // TG_APP_APPCONTEXT_H_
