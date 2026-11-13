#include "features/history_backfill/HistoryBackfill.h"

#include <app/AppContext.h>
#include <common/Idempotency.h>
#include <common/Logger.h>
#include <telegram/MarkdownV2.h>
#include <telegram/TdErrors.h>

#include <chrono>
#include <memory>
#include <utility>

namespace tg::features::history_backfill {
namespace td_api = td::td_api;
namespace {

std::int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string MdCode(const std::string& text) {
  return "`" + telegram::EscapeCodeSpan(text) + "`";
}

bool IsError(const td::ClientManager::Response& response) {
  return response.object == nullptr ||
         response.object->get_id() == td_api::error::ID;
}

const td_api::error* AsError(const td::ClientManager::Response& response) {
  if (response.object == nullptr) return nullptr;
  if (response.object->get_id() != td_api::error::ID) return nullptr;
  return static_cast<const td_api::error*>(response.object.get());
}

std::string ErrorText(const td::ClientManager::Response& response) {
  if (response.object == nullptr) return "<no object>";
  if (const auto* error = AsError(response)) {
    return std::to_string(error->code_) + ": " + error->message_;
  }
  return "<not an error>";
}

}  // namespace

HistoryBackfill::HistoryBackfill(
    telegram::ITdSender& sender, media_thumbnail::MediaThumbnailHandler process,
    chat_state::ChatStateStore& chat_state, telegram::MessageSender messenger,
    HistoryBackfillConfig config)
    : sender_(sender),
      process_(std::move(process)),
      chat_state_(chat_state),
      messenger_(messenger),
      config_(config),
      resume_timer_([this] { PumpResumed(); }) {
  if (config_.page_limit < 1) config_.page_limit = 1;
  if (config_.page_limit > 100) config_.page_limit = 100;  // TDLib search cap
}

HistoryBackfill::~HistoryBackfill() { Shutdown(); }

void HistoryBackfill::Shutdown() {
  stopping_.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.clear();
  }
  resume_timer_.Shutdown();
}

std::size_t HistoryBackfill::active_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return jobs_.size();
}

bool HistoryBackfill::Start(std::int64_t bare_chat_id, app::AppContext& ctx,
                            std::int64_t reply_chat_id) {
  if (!config_.enabled) return false;
  const auto candidates = common::DialogIdCandidates(bare_chat_id);
  if (candidates.empty()) {
    spdlog::error("backfill: {} is not a valid bare chat id — ignored",
                  bare_chat_id);
    messenger_.SendMarkdown(
        reply_chat_id,
        "Backfill refused: " + MdCode(std::to_string(bare_chat_id)) +
            " is not a valid bare chat id\\.");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_.load(std::memory_order_acquire)) return false;
    if (jobs_.count(bare_chat_id) > 0) {
      spdlog::info("backfill: chat {} already running — ignored",
                   bare_chat_id);
      messenger_.SendMarkdown(
          reply_chat_id,
          "Backfill already running for chat " +
              MdCode(std::to_string(bare_chat_id)) + "\\.");
      return false;
    }
    ctx_ = &ctx;  // one AppContext for the process lifetime
    auto job = std::make_shared<Job>();
    job->bare_id = bare_chat_id;
    job->reply_chat_id = reply_chat_id;
    jobs_.emplace(bare_chat_id, job);
  }
  // Resolve bare -> dialog id (probe the peer-type candidates; Telethon's
  // get_entity(int) answered from its session cache, TDLib needs the marked
  // form). Async: the job activates when the first getChat succeeds.
  SendTargetProbe(bare_chat_id, 0, ctx);
  return true;
}

void HistoryBackfill::SendTargetProbe(std::int64_t bare_chat_id,
                                      int candidate_index,
                                      app::AppContext& ctx) {
  const auto candidates = common::DialogIdCandidates(bare_chat_id);
  auto request = td_api::make_object<td_api::getChat>();
  request->chat_id_ = candidates[static_cast<std::size_t>(candidate_index)];
  sender_.Send(
      std::move(request),
      [this, bare_chat_id, candidate_index,
       &ctx](td::ClientManager::Response&& response) {
        if (stopping_.load(std::memory_order_acquire)) return;
        const auto candidates = common::DialogIdCandidates(bare_chat_id);
        if (IsError(response)) {
          if (candidate_index + 1 < static_cast<int>(candidates.size())) {
            SendTargetProbe(bare_chat_id, candidate_index + 1, ctx);
            return;
          }
          spdlog::warn("backfill: chat {} is not a known peer (last error "
                       "{}) — job dropped",
                       bare_chat_id, ErrorText(response));
          auto job = TakeJob(bare_chat_id);
          if (job != nullptr) {
            FinishJob(job, "not a known peer (never interacted or left)");
          }
          return;
        }
        auto chat = td_api::move_object_as<td_api::chat>(
            std::move(response.object));
        if (chat == nullptr) {
          auto job = TakeJob(bare_chat_id);
          if (job != nullptr) FinishJob(job, "getChat returned no chat");
          return;
        }
        auto job = TakeJob(bare_chat_id);
        if (job == nullptr) return;
        job->dialog_id = chat->id_;
        job->title = chat->title_;
        if (!ctx.workers.Post([this, job, &ctx] { Activate(job, ctx); })) {
          spdlog::error("backfill: worker pool rejected activation for chat "
                        "{}", bare_chat_id);
          FinishJob(job, "worker pool rejected the activation");
        }
      });
}

std::shared_ptr<HistoryBackfill::Job> HistoryBackfill::TakeJob(
    std::int64_t bare_chat_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = jobs_.find(bare_chat_id);
  if (it == jobs_.end()) return nullptr;
  return it->second;
}

void HistoryBackfill::Activate(const std::shared_ptr<Job>& job,
                               app::AppContext& ctx) {
  // Worker thread: seed the watermark, then open phase A.
  const auto seed = chat_state_.Get(job->bare_id);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    job->seed_watermark = seed.has_value() ? seed->offset_id : 0;
    if (job->finished) return;
  }
  spdlog::info("backfill: chat {} (dialog {}, \"{}\") from watermark {}",
               job->bare_id, job->dialog_id, job->title, job->seed_watermark);
  messenger_.SendMarkdown(
      job->reply_chat_id,
      "Backfill started: " + MdCode(std::to_string(job->bare_id)) + " \"" +
          telegram::EscapeMarkdownV2(job->title) + "\" from watermark " +
          MdCode(std::to_string(job->seed_watermark)) + "\\.");
  SendSearchPage(job, ctx);
}

void HistoryBackfill::SendSearchPage(const std::shared_ptr<Job>& job,
                                     app::AppContext& ctx) {
  std::int64_t cursor = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_.load(std::memory_order_acquire) || job->finished) return;
    job->busy = true;
    cursor = job->cursor;
  }
  auto request = td_api::make_object<td_api::searchChatMessages>();
  request->chat_id_ = job->dialog_id;
  request->topic_id_ = nullptr;
  request->query_ = "";  // media filter only (Python sends no query either)
  request->sender_id_ = nullptr;
  request->from_message_id_ = cursor;  // 0 = from the newest message
  request->offset_ = 0;                // results start exactly at the cursor
  request->limit_ = config_.page_limit;
  request->filter_ =
      td_api::make_object<td_api::searchMessagesFilterPhotoAndVideo>();

  sender_.Send(std::move(request),
               [this, job, cursor, &ctx](td::ClientManager::Response&& response) {
                 {
                   std::lock_guard<std::mutex> lock(mutex_);
                   job->busy = false;
                 }
                 auto response_ptr =
                     std::make_shared<td::ClientManager::Response>(
                         std::move(response));
                 if (!ctx.workers.Post([this, job, cursor, response_ptr,
                                        &ctx] {
                       HandleSearchPage(job, std::move(*response_ptr), cursor,
                                        ctx);
                     })) {
                   spdlog::error(
                       "backfill: worker pool rejected search page for chat "
                       "{}", job->bare_id);
                   FinishJob(job, "worker pool rejected the search page");
                 }
               });
}

void HistoryBackfill::HandleSearchPage(const std::shared_ptr<Job>& job,
                                       td::ClientManager::Response&& response,
                                       std::int64_t cursor_used,
                                       app::AppContext& ctx) {
  if (const auto* error = AsError(response)) {
    if (telegram::IsFloodWait(*error)) {
      spdlog::warn("backfill: chat {} search FloodWait ({} s) — pausing",
                   job->bare_id,
                   telegram::RetryAfterSeconds(*error).value_or(-1));
      FloodPause(job, *error);
      return;
    }
    spdlog::error("backfill: chat {} search failed: {} — job dropped",
                  job->bare_id, ErrorText(response));
    FinishJob(job, "search failed: " + ErrorText(response));
    return;
  }
  const auto& found =
      static_cast<const td_api::foundChatMessages&>(*response.object);
  std::int64_t oldest_in_page = 0;
  bool done_collecting = false;
  bool start_dispatch = false;
  {
    // One pool task at a time runs phase A for a job, but PumpResumed
    // (timer thread) reads `collecting` — keep job state behind the mutex.
    std::lock_guard<std::mutex> lock(mutex_);
    if (job->finished) return;
    for (const auto& message : found.messages_) {
      if (message == nullptr) continue;
      oldest_in_page = message->id_;  // reverse-chronological: last = oldest
      if (message->id_ > job->seed_watermark) {
        job->collected_desc.push_back(message->id_);
      }
    }

    // Python ends the loop on an empty batch (app_client.py:952-953); a
    // page reaching the watermark is the same termination — everything
    // beyond is already covered by the server-confirmed watermark.
    const bool reached_watermark =
        oldest_in_page != 0 && oldest_in_page <= job->seed_watermark;
    const bool exhausted = found.next_from_message_id_ == 0 ||
                           oldest_in_page == 0;
    if (reached_watermark || exhausted) {
      done_collecting = true;
      job->collecting = false;
      job->pending_asc.assign(job->collected_desc.rbegin(),
                              job->collected_desc.rend());  // oldest first
      job->total_seen = static_cast<std::int64_t>(job->pending_asc.size());
      job->collected_desc.clear();
      spdlog::info(
          "backfill: chat {} collection done — {} messages above watermark {}",
          job->bare_id, job->total_seen, job->seed_watermark);
      start_dispatch = !job->pending_asc.empty();
    } else {
      job->cursor = found.next_from_message_id_;
    }
  }
  if (!done_collecting) {
    spdlog::debug("backfill: chat {} page at {} collected {} ids",
                  job->bare_id, cursor_used, found.messages_.size());
    SendSearchPage(job, ctx);
    return;
  }
  if (!start_dispatch) {
    FinishJob(job, "nothing to backfill (already up to date)");
    return;
  }
  DispatchNextBatch(job, ctx);
}

void HistoryBackfill::DispatchNextBatch(const std::shared_ptr<Job>& job,
                                        app::AppContext& ctx) {
  std::vector<std::int64_t> batch;
  bool finished_now = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_.load(std::memory_order_acquire) || job->finished) return;
    if (job->flooded) return;  // the resume timer owns this job for now
    // Re-read the CURRENT watermark: replies may have advanced it since the
    // job started (resume-friendliness — never re-download what the server
    // already confirmed).
    if (const auto current = chat_state_.Get(job->bare_id);
        current.has_value()) {
      while (!job->pending_asc.empty() &&
             job->pending_asc.front() <= current->offset_id) {
        job->pending_asc.pop_front();
        ++job->total_skipped;
      }
    }
    if (job->pending_asc.empty()) {
      finished_now = true;  // finish outside the lock (FinishJob locks)
    } else {
      while (!job->pending_asc.empty() &&
             static_cast<int>(batch.size()) < config_.page_limit) {
        batch.push_back(job->pending_asc.front());
        job->pending_asc.pop_front();
      }
      job->batch_in_flight = static_cast<int>(batch.size());
    }
  }
  if (finished_now) {
    FinishJob(job, "done");
    return;
  }
  for (const std::int64_t message_id : batch) {
    auto request = td_api::make_object<td_api::getMessage>();
    request->chat_id_ = job->dialog_id;
    request->message_id_ = message_id;
    sender_.Send(
        std::move(request),
        [this, job, message_id, &ctx](td::ClientManager::Response&& response) {
          auto response_ptr = std::make_shared<td::ClientManager::Response>(
              std::move(response));
          if (!ctx.workers.Post([this, job, response_ptr, &ctx] {
                HandleMessageReply(job, std::move(*response_ptr), ctx);
              })) {
            spdlog::error("backfill: worker pool rejected reply for message "
                          "{} of chat {}", message_id, job->bare_id);
            // A rejected task must not stall the batch counter forever.
            FinishJob(job, "worker pool rejected a message reply");
          }
        });
  }
}

void HistoryBackfill::HandleMessageReply(
    const std::shared_ptr<Job>& job, td::ClientManager::Response&& response,
    app::AppContext& ctx) {
  bool batch_done = false;
  bool flooded_now = false;
  if (const auto* error = AsError(response)) {
    if (telegram::IsFloodWait(*error)) {
      spdlog::warn("backfill: chat {} getMessage FloodWait ({} s) — pausing "
                   "after the in-flight batch drains", job->bare_id,
                   telegram::RetryAfterSeconds(*error).value_or(-1));
      FloodPause(job, *error);
    } else {
      // Message deleted / inaccessible: skip it — the watermark logic does
      // not depend on every id succeeding.
      spdlog::warn("backfill: chat {} getMessage failed: {} — skipped",
                   job->bare_id, ErrorText(response));
    }
    std::lock_guard<std::mutex> lock(mutex_);
    batch_done = --job->batch_in_flight <= 0;
    flooded_now = job->flooded;
  } else {
    auto message = td_api::move_object_as<td_api::message>(
        std::move(response.object));
    // The realtime pipeline, verbatim: extract + async download + Outbox +
    // ProcessImages. image_id = str(message_id) dedupes server-side. Runs
    // outside the mutex (it touches the downloader).
    if (message != nullptr) {
      process_.SendProcessImages(*message, ctx);
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (message != nullptr) ++job->total_sent;
      batch_done = --job->batch_in_flight <= 0;
      flooded_now = job->flooded;
      if (job->total_sent > 0 && job->total_sent % 100 == 0) {
        spdlog::info("backfill: chat {} progress — sent {}, pending {}",
                     job->bare_id, job->total_sent, job->pending_asc.size());
      }
    }
  }
  // A flooded job waits for the resume timer instead of the next batch;
  // the timer dispatches it once the pause is over.
  if (batch_done && !flooded_now) DispatchNextBatch(job, ctx);
}

void HistoryBackfill::FloodPause(const std::shared_ptr<Job>& job,
                                 const td_api::error& error) {
  const std::int64_t seconds = telegram::RetryAfterOr(error, 5);
  const std::int64_t resume_at =
      NowMs() + (seconds + config_.flood_pad_seconds) * 1000;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (job->finished) return;
    job->flooded = true;
    job->resume_at_ms = resume_at;
  }
  ArmForEarliestResume();
}

void HistoryBackfill::ArmForEarliestResume() {
  std::int64_t earliest = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [_, job] : jobs_) {
      if (job->flooded && (earliest == 0 || job->resume_at_ms < earliest)) {
        earliest = job->resume_at_ms;
      }
    }
  }
  if (earliest == 0) return;
  const std::int64_t now = NowMs();
  resume_timer_.Arm(
      std::chrono::milliseconds(earliest > now ? earliest - now : 0));
}

void HistoryBackfill::PumpResumed() {
  // ResumeTimer thread. Job state under the mutex; Sends inside
  // SendSearchPage/DispatchNextBatch happen outside it (Send is
  // thread-safe anyway).
  std::vector<std::shared_ptr<Job>> ready;
  app::AppContext* ctx = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::int64_t now = NowMs();
    for (auto& [_, job] : jobs_) {
      if (job->flooded && job->resume_at_ms <= now) {
        job->flooded = false;
        ready.push_back(job);
      }
    }
    ctx = ctx_;
  }
  if (ctx == nullptr) return;
  for (const auto& job : ready) {
    if (job->finished) continue;
    spdlog::info("backfill: chat {} flood pause over — resuming",
                 job->bare_id);
    if (job->collecting) {
      SendSearchPage(job, *ctx);
    } else {
      DispatchNextBatch(job, *ctx);
    }
  }
  ArmForEarliestResume();
}

void HistoryBackfill::FinishJob(const std::shared_ptr<Job>& job,
                                const std::string& reason) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (job->finished) return;
    job->finished = true;
    jobs_.erase(job->bare_id);
  }
  spdlog::info("backfill: chat {} finished ({}) — collected {}, sent {}, "
               "skipped {}",
               job->bare_id, reason, job->total_seen, job->total_sent,
               job->total_skipped);
  messenger_.SendMarkdown(
      job->reply_chat_id,
      "Backfill finished for " + MdCode(std::to_string(job->bare_id)) +
          " \"" + telegram::EscapeMarkdownV2(job->title) + "\": " +
          telegram::EscapeMarkdownV2(reason) + " — sent " +
          MdCode(std::to_string(job->total_sent)) + ", skipped " +
          MdCode(std::to_string(job->total_skipped)) + "\\.");
}

}  // namespace tg::features::history_backfill
