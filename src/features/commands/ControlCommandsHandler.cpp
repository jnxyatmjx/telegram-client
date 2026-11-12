#include "features/commands/ControlCommandsHandler.h"

#include <app/AppContext.h>
#include <common/Logger.h>
#include <features/bot_command/CommandParser.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace tg::features::commands {
namespace {

std::string NormalizeCommand(std::string command) {
  // "/rsme@MyBot" -> "rsme" (same normalization as BotCommandHandler).
  const size_t at = command.find('@');
  if (at != std::string::npos) command = command.substr(0, at);
  while (!command.empty() && command.front() == '/') {
    command.erase(command.begin());
  }
  for (char& c : command) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return command;
}

bool IsSenderUser(const td_api::message& message,
                  std::int64_t expected_user_id) {
  if (message.sender_id_ == nullptr ||
      message.sender_id_->get_id() != td_api::messageSenderUser::ID) {
    return false;
  }
  const auto* sender = static_cast<const td_api::messageSenderUser*>(
      message.sender_id_.get());
  return sender->user_id_ == expected_user_id;
}

// "123" / "-456" -> int64; nullopt otherwise. The /xm-style backfill
// argument is ONE bare id (handlerfetchhistory: int(es_[0])).
std::optional<std::int64_t> ParseBareId(const std::string& token) {
  if (token.empty()) return std::nullopt;
  std::size_t i = 0;
  bool negative = false;
  if (token[0] == '-') {
    negative = true;
    i = 1;
  } else if (token[0] == '+') {
    i = 1;
  }
  if (i >= token.size()) return std::nullopt;
  std::int64_t value = 0;
  const std::int64_t max =
      std::numeric_limits<std::int64_t>::max();
  for (; i < token.size(); ++i) {
    const char c = token[i];
    if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    const int digit = c - '0';
    if (value > (max - digit) / 10) return std::nullopt;  // overflow
    value = value * 10 + digit;
  }
  return negative ? -value : value;
}

}  // namespace

ControlCommandsHandler::ControlCommandsHandler(
    const app::CommandsConfig& config, const app::BotCommandConfig& bot_config,
    telegram::MessageSender messenger, DialogScanner& scanner,
    history_backfill::HistoryBackfill& backfill)
    : config_(config),
      bot_user_id_(bot_config.command_bot_user_id),
      messenger_(messenger),
      scanner_(scanner),
      backfill_(backfill) {}

// --- Testable rendering -----------------------------------------------------

std::string ControlCommandsHandler::SanitizeTitle(const std::string& title) {
  // handlersm 861: re.sub(r'[\s*]+|[\[\]\*_\-\|\`]+', '', title).
  std::string out;
  out.reserve(title.size());
  for (const char c : title) {
    switch (c) {
      case '*':
      case '[':
      case ']':
      case '_':
      case '-':
      case '|':
      case '`':
        continue;
      default:
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        out.push_back(c);
    }
  }
  return out;
}

namespace {
// MarkdownV2 escaping (MessageSender parses with textParseModeMarkdown
// version 2). In a formatting context (bold text, link text) every one of
// these must be escaped or the parse fails / misfires:
//   _ * [ ] ( ) ~ ` > # + - = | { } . !
std::string EscapeMarkdownV2(const std::string& text) {
  std::string out;
  out.reserve(text.size() * 2);
  for (const char c : text) {
    switch (c) {
      case '_':
      case '*':
      case '[':
      case ']':
      case '(':
      case ')':
      case '~':
      case '`':
      case '>':
      case '#':
      case '+':
      case '-':
      case '=':
      case '|':
      case '{':
      case '}':
      case '.':
      case '!':
        out.push_back('\\');
        break;
      default:
        break;
    }
    out.push_back(c);
  }
  return out;
}

// Inside a `code span` the content is literal — only a backtick can close
// the span early, so only it needs escaping.
std::string EscapeCodeSpan(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (const char c : text) {
    if (c == '`') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

}  // namespace

std::vector<std::string> ControlCommandsHandler::RenderRsmeBatches(
    const std::vector<ChatView>& views, int batch_lines) {
  if (batch_lines < 1) batch_lines = 1;
  // Python numbering: channels/groups count i (from 1), users count j
  // (from 1), independently. Users reply first (handlersm 905-916).
  std::vector<std::string> user_lines;
  std::vector<std::string> chat_lines;
  int i = 1;
  int j = 1;
  for (const ChatView& view : views) {
    if (view.kind == ChatView::Kind::kUser) {
      if (!view.username.empty()) {
        // handlersm 881/883: [{first_name}](https://t.me/{username}) + /ym.
        // The display name is user-controlled: escape it for the link text
        // (MarkdownV2) and for the code span (backtick only).
        // MarkdownV2: the '.' after the list number is a reserved
        // character and must be escaped (otherwise the whole chunk fails
        // to parse). The URL's '.' is fine — the link parser reads it
        // literally.
        user_lines.push_back(std::to_string(j) + "\\. [" +
                             EscapeMarkdownV2(view.first_name) +
                             "](https://t.me/" + view.username + ")\t`/ym " +
                             std::to_string(view.bare_id) + " " +
                             EscapeCodeSpan(view.first_name) + "`\t\n\n");
      } else {
        // handlersm 885: **🎯name🎯** + /who.
        user_lines.push_back(std::to_string(j) + "\\. **🎯" +
                             EscapeMarkdownV2(view.title) +
                             "🎯**\t`/who " + std::to_string(view.bare_id) +
                             "`\t\n\n");
      }
      ++j;
      continue;
    }
    if (!view.username.empty()) {
      // handlersm 862: [👉👉👈](https://t.me/{username}) + /xm with the
      // sanitized title.
      chat_lines.push_back(std::to_string(i) + "\\. [👉👉👈](https://t.me/" +
                           view.username + ")\t`/xm " +
                           std::to_string(view.bare_id) + " " +
                           SanitizeTitle(view.title) + "`\t\n\n");
    } else {
      // handlersm 874/894: private channel or basic group — the ⚠️ line
      // (restricted-channel auto-leave NOT migrated: destructive).
      // Title is user-controlled: escape it for the bold text and for the
      // code span (backtick only).
      chat_lines.push_back(std::to_string(i) + "\\. **⚠️" +
                           EscapeMarkdownV2(view.title) +
                           "⚠️**\t`/who " + std::to_string(view.bare_id) +
                           " " + EscapeCodeSpan(view.title) + "`\t\n\n");
    }
    ++i;
  }

  // Batch: list_batch_lines lines per reply message; user batches first.
  std::vector<std::string> batches;
  auto emit = [&batches, batch_lines](std::vector<std::string>& lines) {
    for (std::size_t start = 0; start < lines.size();
         start += static_cast<std::size_t>(batch_lines)) {
      const std::size_t end = std::min(
          lines.size(), start + static_cast<std::size_t>(batch_lines));
      std::string batch;
      for (std::size_t k = start; k < end; ++k) batch += lines[k];
      batches.push_back(std::move(batch));
    }
    lines.clear();
  };
  emit(user_lines);
  emit(chat_lines);
  return batches;
}

std::string ControlCommandsHandler::RenderRsor(
    const std::vector<ChatView>& views, int report_limit) {
  std::vector<const ChatView*> findings;
  for (const ChatView& view : views) {
    if (view.HasFinding()) findings.push_back(&view);
  }
  if (findings.empty()) return "未发现失效或非法的频道/群组。";
  // Telegram single-message length limit: Python truncates to 50 findings
  // (handrestore 1289).
  if (report_limit < 0) report_limit = 0;
  const std::size_t shown =
      std::min(findings.size(), static_cast<std::size_t>(report_limit));
  std::string body;
  for (std::size_t k = 0; k < shown; ++k) {
    const ChatView& view = *findings[k];
    const std::string uname =
        view.username.empty() ? "私有" : "@" + view.username;
    // Plain-text report, but it is sent through SendMarkdown (MarkdownV2
    // parse) — escape the user-controlled title / name / reason so a
    // special character cannot misfire the parse. The link is left
    // unescaped (a bare URL: if its '.' ever trips the parse, the
    // plain-text fallback sends the whole report verbatim, which is
    // exactly what this report is).
    body += "• " + EscapeMarkdownV2(view.title) + " [" +
            EscapeMarkdownV2(uname) + "]\n  " + view.Link() +
            "\n  原因：" + EscapeMarkdownV2(view.FindingReason());
    if (k + 1 < shown) body += "\n\n";
  }
  return "共发现 " + std::to_string(findings.size()) +
         " 个失效/非法对话：\n\n" + body;
}

// --- Triggering ---------------------------------------------------------------

ControlCommandsHandler::Action ControlCommandsHandler::Classify(
    const std::string& name, const app::CommandsConfig& config) {
  const std::string normalized = NormalizeCommand(name);
  if (normalized == NormalizeCommand(config.rsme_command)) {
    return Action::kRsme;
  }
  if (normalized == NormalizeCommand(config.rsor_command)) {
    return Action::kRsor;
  }
  for (const std::string& backfill : config.backfill_commands) {
    if (normalized == NormalizeCommand(backfill)) return Action::kBackfill;
  }
  return Action::kIgnore;
}

bool ControlCommandsHandler::TriggeredByControl(
    const td_api::message& message) const {
  // Python model: the account itself sends the command in the control chat.
  if (config_.self_commands && message.is_outgoing_) return true;
  // Existing bot model: the configured command bot sends it there.
  if (bot_user_id_ != 0 && IsSenderUser(message, bot_user_id_)) return true;
  return false;
}

bool ControlCommandsHandler::CanHandle(
    const td_api::object_ptr<td_api::Update>& update) const {
  if (!config_.enabled) return false;
  if (config_.control_chat_id == 0) return false;
  if (update == nullptr || update->get_id() != td_api::updateNewMessage::ID) {
    return false;
  }
  const auto& new_message =
      static_cast<const td_api::updateNewMessage&>(*update);
  const td_api::message* message = new_message.message_.get();
  if (message == nullptr) return false;
  if (message->chat_id_ != config_.control_chat_id) return false;
  if (message->content_ == nullptr ||
      message->content_->get_id() != td_api::messageText::ID) {
    return false;
  }
  const auto& text = static_cast<const td_api::messageText*>(
                         message->content_.get())
                         ->text_->text_;
  const auto parsed = bot_command::CommandParser::Parse(text);
  if (!parsed) return false;
  if (Classify(parsed->name, config_) == Action::kIgnore) return false;
  return TriggeredByControl(*message);
}

void ControlCommandsHandler::Handle(td_api::object_ptr<td_api::Update>& update,
                                    app::AppContext& ctx) {
  const auto new_message =
      td_api::move_object_as<td_api::updateNewMessage>(std::move(update));
  const td_api::message* message = new_message->message_.get();
  if (message == nullptr) return;

  // Belt and braces: CanHandle already ran, but Handle is reachable directly
  // (tests, future callers).
  if (message->chat_id_ != config_.control_chat_id ||
      message->content_ == nullptr ||
      message->content_->get_id() != td_api::messageText::ID) {
    return;
  }
  const std::string text = static_cast<const td_api::messageText*>(
                               message->content_.get())
                               ->text_->text_;
  const auto parsed = bot_command::CommandParser::Parse(text);
  if (!parsed) return;
  const Action action = Classify(parsed->name, config_);
  if (action == Action::kIgnore) return;
  if (!TriggeredByControl(*message)) {
    spdlog::warn("control_cmd: {} in the control chat from an unconfigured "
                 "direction — ignored", parsed->name);
    return;
  }
  const std::int64_t reply_chat_id = message->chat_id_;

  if (action == Action::kBackfill) {
    if (!backfill_.enabled()) {
      messenger_.SendMarkdown(
          reply_chat_id, "回填功能未启用（features.history_backfill.enabled）。");
      return;
    }
    if (parsed->args.empty()) {
      messenger_.SendMarkdown(reply_chat_id,
                              "用法：" + config_.backfill_commands.front() +
                                  " <裸 chat id>");
      return;
    }
    const auto bare = ParseBareId(parsed->args.front());
    if (!bare.has_value()) {
      messenger_.SendMarkdown(reply_chat_id,
                              "无法解析 chat id `" + parsed->args.front() +
                                  "` — 用法：" +
                                  config_.backfill_commands.front() +
                                  " <裸 chat id>");
      return;
    }
    if (!backfill_.Start(*bare, ctx, reply_chat_id)) {
      // Start() already replied for invalid ids and duplicate jobs; the
      // remaining refusal reasons are "disabled" / "stopping".
      spdlog::info("control_cmd: backfill for {} not started (disabled or "
                   "shutting down)", *bare);
    }
    return;
  }

  if (scanner_.running()) {
    messenger_.SendMarkdown(reply_chat_id, "扫描进行中，请稍候。");
    return;
  }
  if (action == Action::kRsme) {
    const bool started = scanner_.Scan(
        ctx, DialogScanner::Mode::kList,
        [this, reply_chat_id, &ctx](std::vector<ChatView> views) {
          std::vector<std::string> batches =
              RenderRsmeBatches(views, config_.list_batch_lines);
          if (batches.empty()) {
            messenger_.SendMarkdown(reply_chat_id, "没有发现对话。");
            return;
          }
          for (std::string& batch : batches) {
            messenger_.SendMarkdown(reply_chat_id, batch);
          }
        });
    if (!started) {
      // Raced with another command between the running() check and Scan.
      messenger_.SendMarkdown(reply_chat_id, "扫描进行中，请稍候。");
    }
    return;
  }
  // kRsor: prologue, then the audit report (handrestore 1272/1286/1292).
  messenger_.SendMarkdown(reply_chat_id, "开始扫描已加入的频道/群组…");
  const bool started = scanner_.Scan(
      ctx, DialogScanner::Mode::kAudit,
      [this, reply_chat_id](std::vector<ChatView> views) {
        messenger_.SendMarkdown(
            reply_chat_id,
            RenderRsor(views, config_.rsor_report_limit));
      });
  if (!started) {
    messenger_.SendMarkdown(reply_chat_id, "扫描进行中，请稍候。");
  }
}

}  // namespace tg::features::commands
