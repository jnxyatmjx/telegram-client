# State persistence & the D1 backend (Telethon migration chunk 2)

Hong's decision of 2026-11-10 13:50 (IMPLEMENTATION_PLAN top item 10): the
chat-state backend ends up on Cloudflare **D1** — the same table the Python
client writes, so both deployments share the watermark state. Until Hong
provides the credentials (Secure Vault), the available default is **local
SQLite** behind a clean interface. Chunk 2 shipped exactly that: the
interface, the SQLite backend, and this design note for the later D1
implementation.

## What persists

The Python client's `chathis` table — one row per bare chat id:

| column       | form                                    | C++ source            |
|--------------|-----------------------------------------|-----------------------|
| `chat_id`    | bare MTProto id (uint32 range, int64)   | `ChatStateStore` key  |
| `offset_id`  | watermark, only increases               | `ApplyReply`          |
| `chat_url`   | `https://t.me/...` or the placeholder   | on-demand resolution  |
| `init_date`  | `YYYYMMDD`, GMT+8                       | `common/Gmt8::Date`   |
| `init_time`  | `HH:MM:SS`, GMT+8                       | `common/Gmt8::Clock`  |
| `updated_at` | `YYYY-MM-DD HH:MM:SS`, GMT+8            | `common/Gmt8::DateTime` |

The timestamp formats must stay byte-compatible with the Python client
(`gmt8_now()` + `strftime`, app_client.py 31-43) or D1 rows written by one
deployment would not parse back in the other (SURVEY §8.5-4).

## The seam

`features/chat_state/ChatStateBackend.h` — the interface behind
`ChatStateStore`:

- `Upsert(row)` — one flush-pass row (Python `schedule2db` upsert shape).
- `LoadAll()` — startup restore (Python `load_chat_state`).

`ChatStateStore` and `Application` do NOT change when D1 arrives: only
another `ChatStateBackend` implementation is swapped in by the wiring.
`SqliteChatStateBackend` is the default while `chat_state.enabled` is set;
it is a separate SQLite file (`chat_state.db_path`, default
`/data/chathis.sqlite`) — not a table inside the Outbox database — so the
D1 swap replaces the whole backend file, and a future migration tool can
copy rows into D1 verbatim.

## Flush model (identical to Python)

- Every `chat_state.flush_interval_seconds` (30 s, Python's `schedule2db`
  cadence) the Application flush thread calls `ChatStateStore::FlushActive`.
- Only entries whose watermark advanced within the last
  `active_window_seconds` (45 s) are written — quiet chats stay out of the
  write path.
- Startup: `Restore()` loads every persisted row into memory BEFORE the
  flush thread starts; `chat_url` is NOT re-resolved in bulk (Python's
  lesson: a resolution storm over many chats is a FloodWait trap) —
  resolution stays on demand, exactly as in the realtime chain.

## D1 client design (to implement when credentials arrive)

Authoritative reference: the Python `D1Database` wrapper (app_client.py
449-648). The C++ client mirrors its REST call:

- **Endpoint**: `POST {D1_API_BASE}/accounts/{account_id}/d1/database/{database_id}/query`
  with `D1_API_BASE` defaulting to `https://api.cloudflare.com/client/v4`.
- **Headers**: `Authorization: Bearer {CLOUDFLARE_API_TOKEN}`,
  `Content-Type: application/json`. Timeout 35 s per request.
- **Retries**: on 429/500/502/503/504 back off exponentially
  (0.5 · 2^(n−1) s, capped at 8 s), at most 3 attempts; 401/403 fail
  immediately (credentials are wrong — retrying cannot help).
- **Parameters**: SQL named parameters (`:name`) are translated to `?`
  placeholders with an ordered bind list.
- **Batch inserts**: expand VALUES per row, then chunk so no statement
  exceeds `D1_MAX_SQL_VARIABLES` (default 90) bound variables; row chunks
  of `D1_BATCH_SIZE` (default 500).
- **Config keys** (env, never git/logs): `CLOUDFLARE_ACCOUNT_ID`,
  `CLOUDFLARE_API_TOKEN`, `D1_DATABASE_ID`, `D1_API_BASE`,
  `D1_BATCH_SIZE`, `D1_MAX_SQL_VARIABLES`, `D1_MAX_ATTEMPTS`, `D1_TIMEOUT`.
- **Transport**: an HTTPS client in the C++ process (chunk 3 scope, like
  the other robustness items). Flushing happens on the Application flush
  thread; the D1 backend must never block a worker or the Receive thread.

Credentials arrive via Hong (Secure Vault). Until then the SQLite backend
keeps the process fully functional — same schema, same flush semantics, so
switching to D1 is a wiring change plus a one-time row copy.
