#ifndef TG_APP_APPCONTEXT_H_
#define TG_APP_APPCONTEXT_H_

// AppContext (architecture v5 §3.5): one object passed through every
// handler; references owned by Application, which outlives all workers.
// Deviation from v5: `telegram` is the ITdSender& seam (TelegramClient
// implements it) so handlers are unit-testable against a fake sender.
//
// Async rule (2026-11-09): message processing is fully async — no worker
// thread ever blocks on a TDLib response or a download. A handler stage
// that needs an async result (getMessage, downloadFile) registers a
// callback and returns; the callback only POSTS the next stage to
// `workers` (it runs on the TDLib Receive thread, so it must be cheap and
// must never block). That is why the pool is in the context.

#include <app/Config.h>
#include <app/FlowControl.h>
#include <app/WorkerPool.h>
#include <infra/Outbox.h>
#include <telegram/FileDownloader.h>
#include <telegram/TelegramClient.h>
#include <transport/ProcessImagesClient.h>
#include <transport/SearchImagesClient.h>
#include <transport/StreamerPool.h>

namespace tg::app {

struct AppContext {
  // Where continuations run: async callbacks post their next stage here.
  WorkerPool& workers;
  telegram::ITdSender& telegram;
  telegram::FileDownloader& files;
  // Pools deal requests round-robin across grpc.connections lanes; the
  // handlers only need Enqueue, so they are unchanged by the lane count.
  transport::StreamerPool<ImageRequest, ImageReply>& process_images;
  transport::StreamerPool<SearchReq, SearchRsp>& search_images;
  infra::Outbox& outbox;
  const Config& config;
  // Flow-control window for the process path (retry design §2(c)):
  // FinishProcess gates its Enqueue on it. Nullable: a context built
  // without one (tests) means the window is bypassed.
  FlowControl* flow_control = nullptr;
};

}  // namespace tg::app

#endif  // TG_APP_APPCONTEXT_H_
