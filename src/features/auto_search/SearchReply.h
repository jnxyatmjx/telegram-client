#ifndef TG_FEATURES_AUTO_SEARCH_SEARCHREPLY_H_
#define TG_FEATURES_AUTO_SEARCH_SEARCHREPLY_H_

// Turns a SearchImages SearchRsp.result into the one Telegram message
// app_bot_client.py sends as a reply to the image that was searched.
//
// result is either Milvus JSON {"data":[{"memo","cid","mid"}, ...]} or a
// non-JSON status string ("Search N No Faces", "Image Empty N", "None",
// an exception blob). Non-JSON, missing `data`, and an empty `data` array
// become the fixed "No One Find" line with the link preview ON. A non-empty
// `data` array becomes one MarkdownV2 message, link preview OFF.
//
// memo is AES-GCM ciphertext (MemoCipher / KEYME). Decrypt failure of one
// hit is logged and that hit uses the emoji placeholder, so the other hits
// still go out. Python drops the whole reply on the first decrypt error;
// keeping the rest is the deliberate difference.
//
// The reply is sent to the correlation entry's TDLib chat_id, reply_to the
// entry's message_id. The uint32 oid is never a sendMessage chat_id.
// Each hit's link uses that hit's `cid` (not the request oid). The base is
// resolved before formatting (chat_links, then chathis, then a public
// username). This header's formatter does not call TDLib itself.

#include <features/media_thumbnail/SearchHandler.h>
#include <telegram/MessageSender.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace tg::features::auto_search {

// Collapse trailing slashes to exactly one, so the mid is one path segment.
std::string NormalizeLinkBase(std::string url);

// http(s) chat_url is joined with the decimal message id (Python
// build_message_link), with exactly one '/' before it. A TDLib message id
// (mid >= 2^20 and mid % 2^20 == 0) is shifted down (mid >> 20) first;
// any other mid is used as-is. This applies to every link kind. Anything
// else — missing, empty, or a placeholder such as ">>>>Nrror>…" —
// becomes https://t.me/c/{bare_cid}/{message id}.
std::string SearchResultLink(std::int64_t bare_cid, std::int64_t mid,
                             const std::optional<std::string>& chat_url);

// Distinct hit cids in `result`, in first-seen order. Empty when the
// result is not a non-empty hit list (the "No One Find" cases).
std::vector<std::int64_t> SearchReplyCids(const std::string& result);

struct SearchReplyMessage {
  std::string text;
  // True only for the "No One Find" line (Python link_preview=True).
  bool link_preview_enabled = false;
};

// `decrypt` returns the plaintext memo, or nullopt on failure.
// `chat_url` returns an http(s) prefix for that bare cid, or nullopt.
SearchReplyMessage FormatSearchReply(
    const std::string& result,
    const std::function<std::optional<std::string>(const std::string&)>&
        decrypt,
    const std::function<std::optional<std::string>(std::int64_t)>& chat_url);

// Formats `result` and sends it with messenger.SendMarkdownReply to
// `target.chat_id` / `target.message_id`.
void DeliverSearchReply(
    telegram::MessageSender& messenger,
    const media_thumbnail::SearchCorrelationTable::Entry& target,
    const std::string& result,
    const std::function<std::optional<std::string>(const std::string&)>&
        decrypt,
    const std::function<std::optional<std::string>(std::int64_t)>& chat_url);

}  // namespace tg::features::auto_search

#endif  // TG_FEATURES_AUTO_SEARCH_SEARCHREPLY_H_
