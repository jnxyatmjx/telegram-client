#ifndef TG_FEATURES_CHAT_STATE_CHATSTATEBACKEND_H_
#define TG_FEATURES_CHAT_STATE_CHATSTATEBACKEND_H_

// ChatStateBackend — the storage seam behind ChatStateStore (Telethon
// migration Chunk 2, IMPLEMENTATION_PLAN item 10 / Hong decision of
// 2026-11-10 13:50).
//
// The end state is Cloudflare D1 (the Python client's backend, so the two
// deployments share the watermark table). D1 needs credentials from Hong
// (Secure Vault) and an HTTPS client, so the AVAILABLE default here is local
// SQLite with the exact `chathis` schema the Python client uses — column
// names, order, and the ON CONFLICT(chat_id) upsert are byte-compatible
// (app_client.py schedule2db 1229-1237). Swapping in D1Client later means
// implementing this interface, not touching ChatStateStore or Application;
// the full D1 design lives in docs/pending/state-persistence-and-d1.md.
//
// All timestamps are GMT+8 civil strings in the Python formats (common/Gmt8.h)
// so D1 rows written by either deployment parse back in both.

#include <cstdint>
#include <string>
#include <vector>

namespace tg::features::chat_state {

// One row of the `chathis` table (Python ChatInfo -> D1 row).
struct ChatStateRow {
  std::int64_t chat_id = 0;   // bare MTProto chat id (positive)
  std::int64_t offset_id = 0; // watermark (server-confirmed max message id)
  std::string chat_url;       // t.me link or the ">>>>Nrror><id>" placeholder
  std::string init_date;      // "YYYYMMDD" of the last watermark advance
  std::string init_time;      // "HH:MM:SS" of the last watermark advance
  std::string updated_at;     // "YYYY-MM-DD HH:MM:SS" of the flush itself
};

class ChatStateBackend {
 public:
  virtual ~ChatStateBackend() = default;

  ChatStateBackend(const ChatStateBackend&) = delete;
  ChatStateBackend& operator=(const ChatStateBackend&) = delete;

  // Upsert every row (Python schedule2db: INSERT ... ON CONFLICT(chat_id)
  // DO UPDATE SET offset_id/chat_url/init_date/init_time/updated_at).
  // Throws std::runtime_error on storage failure; the caller logs and keeps
  // going (the in-memory state stays authoritative).
  virtual void Upsert(const std::vector<ChatStateRow>& rows) = 0;

  // Full table load for startup restore (Python load_chat_state: "select
  // chat_id,offset_id,init_date,init_time from chathis"). Throws
  // std::runtime_error on storage failure — restore is startup-critical.
  virtual std::vector<ChatStateRow> LoadAll() = 0;

 protected:
  ChatStateBackend() = default;
};

}  // namespace tg::features::chat_state

#endif  // TG_FEATURES_CHAT_STATE_CHATSTATEBACKEND_H_
