#ifndef TG_FEATURES_COMMANDS_DIALOGSCANNER_H_
#define TG_FEATURES_COMMANDS_DIALOGSCANNER_H_

// DialogScanner — dialog enumeration for the control commands (Telethon
// migration Chunk 2; Python handlersm 838-916 / scan_dialogs 1294-1317).
//
// This TDLib build has NO legacy getDialogs(offset,...): enumeration is
//   1. loadChats(chat_list, 100) repeatedly until it answers the 404
//      error — TDLib's documented "all chats loaded" signal (each call
//      loads the next chunk of the list into the local database). The main
//      list first, then the archive list (Python get_dialogs /
//      iter_dialogs with archived=True counts archived dialogs too —
//      without this step the C++ count is short by exactly the archived
//      dialogs);
//   2. getChats(main, dialog_limit) + getChats(archive, dialog_limit),
//      merged and truncated to dialog_limit total (Python's limit is a
//      total across folders).
// Then each dialog id resolves sequentially (one outstanding request):
//   getChat -> type:
//     chatTypePrivate    -> getUser                     (usernames.active_usernames_, first_name)
//     chatTypeBasicGroup -> getBasicGroup               (is_active, upgraded_to_supergroup_id, status)
//                          -> migrated: re-enter at the
//                             upgraded supergroup (Python resolve_active_chat_entity)
//     chatTypeSupergroup -> getSupergroup               (usernames, verification_status
//                                                         scam/fake, restriction_info,
//                                                         is_channel, status)
//                          -> kAudit mode: getSupergroupFullInfo probe
//                             (Python GetFullChannelRequest — a private/dead
//                             channel errors here)
//
// The full result set is delivered in ONE callback (std::vector<ChatView>,
// ~120 bytes per dialog — 1202 dialogs are trivial). The caller renders
// /rsme listings or /rsor audit reports from it.
//
// FloodWait pauses the whole scan via a ResumeTimer and RETRIES THE SAME
// request after retry-after + pad (Python sleeps seconds + 1 and skips the
// dialog; retrying the same step is the closer C++ analogue and never loses
// a dialog mid-scan — every step is an idempotent read).
//
// Threading: Scan() from a command worker; every TDLib reply is handled on
// the Receive thread only by posting the next step to the worker pool. One
// scan at a time (running()); the result callback fires on a worker task.
// Shutdown() stops the scan and the resume timer at the beginning of the
// Application shutdown sequence.

#include <common/ResumeTimer.h>
#include <telegram/TelegramClient.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace tg::app {
struct AppContext;
}  // namespace tg::app

namespace tg::features::commands {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

struct ChatView {
  enum class Kind { kUser, kChannel, kBasicGroup };

  Kind kind = Kind::kChannel;
  std::int64_t bare_id = 0;    // MTProto bare id (the /xm//ym argument form)
  std::int64_t dialog_id = 0;  // TDLib dialog id
  std::string title;           // chat title, or user display name
  std::string username;        // first active username ("" if none)
  std::string first_name;      // users only
  // channel/supergroup:
  bool is_channel = false;     // broadcast channel (not a megagroup)
  bool left = false;           // chatMemberStatusLeft
  bool banned = false;         // chatMemberStatusBanned (forbidden/kicked)
  bool scam = false;           // verification_status.is_scam
  bool fake = false;           // verification_status.is_fake
  bool restricted = false;     // restriction_info.restriction_reason non-empty
  // basic group:
  bool deactivated = false;    // is_active == false
  bool migrated = false;       // upgraded_to_supergroup_id != 0
  // kAudit probe (supergroups only, Python GetFullChannelRequest):
  bool probe_failed = false;
  std::string probe_error;     // "code: message" of the failed probe

  // Python check_status 1319-1350: returns true when the chat is 失效/非法.
  // Users never have findings (scan_dialogs skips them).
  bool HasFinding() const;
  // The reason string for the current finding, in Python's wording.
  std::string FindingReason() const;
  // Python build_link 1352-1359: public link via username, private channel
  // via https://t.me/c/<bare>, else the "no public link" note.
  std::string Link() const;
};

class DialogScanner {
 public:
  enum class Mode { kList, kAudit };

  struct Config {
    // Python get_dialogs(limit=1202).
    int dialog_limit = 1202;
    // Telethon: sleep e.seconds + 1 after FloodWait.
    int flood_pad_seconds = 1;
  };

  DialogScanner(telegram::ITdSender& sender, Config config);
  ~DialogScanner();  // Shutdown()

  DialogScanner(const DialogScanner&) = delete;
  DialogScanner& operator=(const DialogScanner&) = delete;

  // Starts one scan; false when disabled-by-stop, already running, or
  // shutting down. `on_done` fires once, on a worker task, with every
  // resolved dialog (empty vector = enumeration failed).
  bool Scan(app::AppContext& ctx, Mode mode,
            std::function<void(std::vector<ChatView>)> on_done);

  bool running() const;

  void Shutdown();

 private:
  // Enumeration (steps 1-2) and the per-chat resolution loop (step 3). Every
  // Send* posts its continuation to the worker pool; per-chat state rides in
  // `current_` so a FloodWait retry closure only needs the peer id.
  void LoadMoreChats(app::AppContext& ctx);
  void FetchChatList(app::AppContext& ctx);
  // Step 2b: the archive list's chats, merged with the main list's ids.
  void FetchArchiveChatList(app::AppContext& ctx,
                            std::vector<std::int64_t> main_ids);
  void ResolveNext(app::AppContext& ctx);
  void SendGetChat(app::AppContext& ctx, std::int64_t chat_id);
  void SendGetUser(app::AppContext& ctx, std::int64_t user_id);
  void SendGetBasicGroup(app::AppContext& ctx, std::int64_t group_id);
  void SendGetSupergroup(app::AppContext& ctx, std::int64_t supergroup_id);
  void SendProbeSupergroup(app::AppContext& ctx);
  void HandleEnumeratedChat(td_api::object_ptr<td_api::chat> chat,
                            app::AppContext& ctx);
  void ContinueBasicGroup(td_api::object_ptr<td_api::basicGroup> group,
                          app::AppContext& ctx);
  void ContinueSupergroup(td_api::object_ptr<td_api::supergroup> group,
                          app::AppContext& ctx);
  void ContinueUser(td_api::object_ptr<td_api::user> user,
                    app::AppContext& ctx);
  // Python resolve_active_chat_entity for migrated basic groups: re-enter
  // enumeration at the upgraded supergroup's dialog.
  void ContinueMigrated(std::int64_t supergroup_bare_id,
                        app::AppContext& ctx);

  // Records the fully resolved view and moves to the next enumerated chat.
  void Push(ChatView view, app::AppContext& ctx);
  void Advance(app::AppContext& ctx);
  void Finish(app::AppContext& ctx, bool ok);
  // Pauses the scan and retries the SAME step (captured in `retry`) after
  // retry-after + pad. Callable from any thread.
  void FloodPause(app::AppContext& ctx, const td_api::error& error,
                  std::function<void()> retry);
  // ResumeTimer thread: re-post the captured retry step.
  void OnResumeTimer();

  telegram::ITdSender& sender_;
  Config config_;

  mutable std::mutex mutex_;
  bool running_ = false;
  Mode mode_ = Mode::kList;
  std::function<void(std::vector<ChatView>)> on_done_;
  bool loading_archive_ = false;  // LoadMoreChats phase: main, then archive
  std::vector<std::int64_t> chat_ids_;  // enumeration result
  std::size_t next_chat_ = 0;           // resolution cursor
  std::vector<ChatView> views_;
  ChatView current_;                    // view being resolved (chain is
                                        // strictly sequential)
  app::AppContext* ctx_ = nullptr;      // the scan's AppContext (process-lifetime)
  std::function<void()> retry_step_;    // FloodPause retry closure
  std::atomic<bool> stopping_{false};
  common::ResumeTimer resume_timer_;
};

}  // namespace tg::features::commands

#endif  // TG_FEATURES_COMMANDS_DIALOGSCANNER_H_
