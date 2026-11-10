#ifndef TG_FEATURES_HISTORY_BACKFILL_HISTORYBACKFILL_H_
#define TG_FEATURES_HISTORY_BACKFILL_HISTORYBACKFILL_H_

// HistoryBackfill (Telethon migration Chunk 2; Python handlerfetchhistory
// 918-963): command-triggered media-history backfill per chat.
//
// Python paged `get_messages(offset_id=<watermark>, limit=60, reverse=True,
// filter=InputMessagesFilterPhotoVideo)` — oldest-to-newest FROM the
// watermark ONWARD — and fed every batch into the same mss_queue -> gRPC
// pipeline as realtime messages. This engine mirrors that with TDLib:
//
//   Phase A (collect): searchChatMessages(filter=photoAndVideo) pages
//     BACKWARD from the newest message (from_message_id=0, then
//     next_from_message_id; results are reverse-chronological), keeping
//     message ids > the chat's watermark. The phase ends at
//     next_from_message_id == 0 or when a page reaches ids <= the
//     watermark — so only the gap between watermark and newest message is
//     ever collected (8 bytes per id; message objects are re-fetched per
//     id in phase B to keep memory flat even for huge gaps).
//   Phase B (dispatch): collected ids ASCENDING (Python's oldest-first
//     order) in page_limit batches -> getMessage per id (fan-out, TDLib
//     answers from its local database) -> MediaThumbnailHandler::
//     SendProcessImages per message — the EXACT realtime pipeline
//     (extract -> async download -> Outbox -> ProcessImages stream), so
//     image_id = str(message_id) dedupes and the server never sees a
//     duplicate.
//
// Resume/断点续传: the watermark lives in ChatStateStore (persisted by the
// 30 s flush; restored at startup) and is advanced by SERVER replies —
// exactly Python's model (SURVEY §2.4: the watermark is server-driven, not
// download-driven). A restart loses the in-flight job (the command must be
// re-issued, as in Python) but continues from the last server-confirmed
// watermark; each dispatch additionally re-reads the watermark so messages
// the server already processed are skipped without re-downloading.
//
// FloodWait: any 429 in either phase pauses the whole job for
// retry-after + flood_pad_seconds (Telethon sleeps seconds + 1) via a
// ResumeTimer; the job then continues at its cursor.
//
// Threading: Start() from a command worker; TDLib responses arrive on the
// Receive thread and only post the next stage to the worker pool; the
// ResumeTimer fires on its own thread. All job state sits behind one
// mutex; TDLib Send calls happen outside it. Shutdown() stops new work at
// the beginning of the Application shutdown sequence.

#include <common/ResumeTimer.h>
#include <features/chat_state/ChatStateStore.h>
#include <features/media_thumbnail/MediaThumbnailHandler.h>
#include <telegram/MessageSender.h>
#include <telegram/TelegramClient.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace tg::app {
struct AppContext;
}  // namespace tg::app

namespace tg::features::history_backfill {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

struct HistoryBackfillConfig {
  bool enabled = true;
  // Python get_messages(limit=60); TDLib accepts 1..100 for search.
  int page_limit = 60;
  // Telethon: `await asyncio.sleep(e.seconds + 1)` after FloodWait.
  int flood_pad_seconds = 1;
};

class HistoryBackfill {
 public:
  HistoryBackfill(telegram::ITdSender& sender,
                  media_thumbnail::MediaThumbnailHandler process,
                  chat_state::ChatStateStore& chat_state,
                  telegram::MessageSender messenger,
                  HistoryBackfillConfig config);
  ~HistoryBackfill();  // Shutdown()

  HistoryBackfill(const HistoryBackfill&) = delete;
  HistoryBackfill& operator=(const HistoryBackfill&) = delete;

  // Starts a backfill for a BARE chat id (/xm argument). Resolves the bare
  // id to a dialog first (probe getChat over the peer-type candidates —
  // Telethon answered this from its session cache); the reply about
  // progress goes to reply_chat_id (the control chat). Returns false when
  // disabled, stopping, or a job for this chat is already running.
  bool Start(std::int64_t bare_chat_id, app::AppContext& ctx,
             std::int64_t reply_chat_id);

  std::size_t active_count() const;
  bool enabled() const { return config_.enabled; }

  // Refuses new work and stops the resume timer. Call once at the start of
  // the Application shutdown sequence; in-flight posted tasks drain out.
  void Shutdown();

 private:
  struct Job {
    std::int64_t bare_id = 0;
    std::int64_t dialog_id = 0;  // TDLib dialog id (resolved by the probe)
    std::int64_t reply_chat_id = 0;
    std::string title;  // chat title, from the probe's getChat
    std::int64_t seed_watermark = 0;  // watermark read at Start
    bool finished = false;  // FinishJob ran; the job object may outlive its
                            // map entry in pending task captures
    // Phase A:
    std::int64_t cursor = 0;         // next_from_message_id (0 = newest)
    std::vector<std::int64_t> collected_desc;  // ids as paged (newest first)
    bool collecting = true;
    // Phase B:
    std::deque<std::int64_t> pending_asc;  // oldest-first dispatch backlog
    int batch_in_flight = 0;
    // Flood pause (steady_clock ms):
    bool flooded = false;
    std::int64_t resume_at_ms = 0;
    std::int64_t total_seen = 0;   // ids collected above the seed
    std::int64_t total_sent = 0;   // messages handed to the pipeline
    std::int64_t total_skipped = 0;  // below the (advanced) watermark
    bool busy = false;             // a phase-A request is in flight
  };

  // Probes the peer-type candidates for the bare id with getChat (Receive
  // thread): on success fills dialog_id/title and posts Activate; on
  // all-candidates-failed finishes the job with an explanation.
  void SendTargetProbe(std::int64_t bare_chat_id, int candidate_index,
                       app::AppContext& ctx);
  // Removes (and returns) the job for bare_chat_id from the map — for
  // paths that hand the job over to FinishJob.
  std::shared_ptr<Job> TakeJob(std::int64_t bare_chat_id);
  // Worker thread: reads the seed watermark, reports the start, opens
  // phase A.
  void Activate(const std::shared_ptr<Job>& job, app::AppContext& ctx);

  // Worker-thread stages.
  void HandleSearchPage(const std::shared_ptr<Job>& job,
                        td::ClientManager::Response&& response,
                        std::int64_t cursor_used, app::AppContext& ctx);
  void HandleMessageReply(const std::shared_ptr<Job>& job,
                          td::ClientManager::Response&& response,
                          app::AppContext& ctx);

  void SendSearchPage(const std::shared_ptr<Job>& job, app::AppContext& ctx);
  void DispatchNextBatch(const std::shared_ptr<Job>& job,
                         app::AppContext& ctx);
  void FinishJob(const std::shared_ptr<Job>& job, const std::string& reason);

  void FloodPause(const std::shared_ptr<Job>& job,
                  const td_api::error& error);
  // Arms resume_timer_ at the earliest pending flood resume, if any.
  void ArmForEarliestResume();
  // Timer thread: resume every job whose flood pause is over.
  void PumpResumed();

  telegram::ITdSender& sender_;
  media_thumbnail::MediaThumbnailHandler process_;
  chat_state::ChatStateStore& chat_state_;
  telegram::MessageSender messenger_;
  HistoryBackfillConfig config_;

  mutable std::mutex mutex_;
  std::map<std::int64_t, std::shared_ptr<Job>> jobs_;  // bare_id -> job
  // The one AppContext, captured on the first Start. Plain pointer:
  // accessed under mutex_ (Start sets it, PumpResumed reads it); the
  // Application guarantees ctx outlives this object.
  app::AppContext* ctx_ = nullptr;
  std::atomic<bool> stopping_{false};
  common::ResumeTimer resume_timer_;
};

}  // namespace tg::features::history_backfill

#endif  // TG_FEATURES_HISTORY_BACKFILL_HISTORYBACKFILL_H_
