#ifndef TG_FEATURES_COMMANDS_CONTROLCOMMANDSHANDLER_H_
#define TG_FEATURES_COMMANDS_CONTROLCOMMANDSHANDLER_H_

// ControlCommandsHandler (Telethon migration Chunk 2; Python handlersm
// 838-916, handlerfetchhistory 918-963, handrestore 1271-1317): the control-
// chat command surface.
//
//   /rsme   — dialog listing (channels/groups then users, list_batch_lines
//             per reply message, 4096-char split by MessageSender). The
//             listing hands out BARE chat ids as /xm//xy//ym//who arguments.
//             Python's auto-LeaveChannelRequest for restricted private
//             channels is deliberately NOT migrated (destructive; Hong to
//             confirm) — restricted channels just get the ⚠️ line.
//   /rsor   — 失效/非法 dialog audit (scan_dialogs/check_status): probes
//             supergroups with getSupergroupFullInfo, reports scam/fake/
//             left/banned/deactivated/migrated/private, max rsor_report_limit
//             findings. Reports ONLY — no auto-leave.
//   /xm /xy /ym /who + args — history backfill (handlerfetchhistory): the
//             first numeric argument is the BARE chat id; resolution and
//             watermark handling live in HistoryBackfill.
//
// Trigger directions (both configured): the Python model — an OUTGOING
// message (self_commands) in the control chat — and the existing bot model —
// a message from command_bot_user_id in the control chat. Everything else is
// ignored. Commands are also accepted with an "@bot" suffix and any case
// (CommandParser normalization).
//
// Threading: CanHandle on the Receive thread (cheap checks only); Handle
// runs on a worker task and drives the async DialogScanner /
// HistoryBackfill / reply sends. All sends go through MessageSender
// (markdown, 4096-char split).

#include <app/Config.h>
#include <features/commands/DialogScanner.h>
#include <features/history_backfill/HistoryBackfill.h>
#include <telegram/MessageSender.h>
#include <telegram/UpdateHandler.h>

#include <string>
#include <vector>

namespace tg::features::commands {
namespace td_api = td::td_api;  // shorthand for td::td_api::*

class ControlCommandsHandler : public telegram::IUpdateHandler {
 public:
  ControlCommandsHandler(const app::CommandsConfig& config,
                         const app::BotCommandConfig& bot_config,
                         telegram::MessageSender messenger,
                         DialogScanner& scanner,
                         history_backfill::HistoryBackfill& backfill);

  bool CanHandle(
      const td_api::object_ptr<td_api::Update>& update) const override;
  void Handle(td_api::object_ptr<td_api::Update>& update,
              app::AppContext& ctx) override;
  const char* Name() const override { return "ControlCommandsHandler"; }

  // Exposed for unit tests.
  enum class Action { kIgnore, kRsme, kRsor, kBackfill };
  static Action Classify(const std::string& name,
                         const app::CommandsConfig& config);
  // /rsme listing: user batches first, then channel/group batches, up to
  // batch_lines lines per message (Python handlersm 905-916).
  static std::vector<std::string> RenderRsmeBatches(
      const std::vector<ChatView>& views, int batch_lines);
  // /rsor report: prologue-free body ("未发现..." / "共发现 N 个...").
  static std::string RenderRsor(const std::vector<ChatView>& views,
                                int report_limit);
  // handlersm 861: strip whitespace and [\[\]\*_\-|\`] from titles for the
  // /xm argument form.
  static std::string SanitizeTitle(const std::string& title);

 private:
  bool TriggeredByControl(const td_api::message& message) const;

  app::CommandsConfig config_;
  std::int64_t bot_user_id_;  // from bot_command config (0 = no bot trigger)
  telegram::MessageSender messenger_;
  DialogScanner& scanner_;
  history_backfill::HistoryBackfill& backfill_;
};

}  // namespace tg::features::commands

#endif  // TG_FEATURES_COMMANDS_CONTROLCOMMANDSHANDLER_H_
