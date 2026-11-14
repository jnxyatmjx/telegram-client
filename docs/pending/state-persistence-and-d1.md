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
| `chat_id`    | bare MTProto id (int64; user ids above 2^32 are valid) | `ChatStateStore` key  |
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

`ChatStateStore` does not change when the backend changes. `Application`
only chooses sqlite or d1 from `chat_state.backend`.
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

## D1 backend (implemented)

`features/chat_state/D1ChatStateBackend` is the Cloudflare client. Select
it with `chat_state.backend: d1` (`sqlite` remains the default, so existing
deployments do not change). A build without libcurl refuses `d1` at
startup with an explicit error.

Credentials come from the environment and are never logged. The same names
may be set under `chat_state` (`d1_account_id`, `d1_api_token`,
`d1_database_id`, `d1_api_base`, `d1_batch_size`, `d1_max_sql_variables`,
`d1_max_attempts`, `d1_timeout_seconds`) and override the environment.

| env | default |
| --- | --- |
| `CLOUDFLARE_ACCOUNT_ID` | required |
| `CLOUDFLARE_API_TOKEN` | required |
| `D1_DATABASE_ID` | required |
| `D1_API_BASE` | `https://api.cloudflare.com/client/v4` |
| `D1_BATCH_SIZE` | 500 |
| `D1_MAX_SQL_VARIABLES` | 90 (15 rows × 6 columns) |
| `D1_MAX_ATTEMPTS` | 3 |
| `D1_TIMEOUT` | 35 seconds |

`POST {D1_API_BASE}/accounts/{id}/d1/database/{id}/query` with
`Authorization: Bearer …` and `{"sql","params"}`. `success != true` and a
per-result `success: false` are errors. 401/403 fail immediately. 429 and
5xx, and connection failures, retry with backoff `min(8s, 0.5·2^(n-1))`.
The SQL is the Python upsert:

```sql
INSERT INTO chathis (chat_id, offset_id, chat_url, init_date, init_time, updated_at)
VALUES (?, ?, ?, ?, ?, ?)
ON CONFLICT(chat_id) DO UPDATE SET
    offset_id = excluded.offset_id,
    chat_url = excluded.chat_url,
    init_date = excluded.init_date,
    init_time = excluded.init_time,
    updated_at = excluded.updated_at
```

`LoadAll` is `SELECT chat_id, offset_id, chat_url, init_date, init_time FROM chathis`.
`init_date` may be an integer or a digit string. `init_time` may carry a
fractional second, which is stripped. A null `chat_url` becomes empty.

### offset_id scale

Python stores the Telethon/server message id. The C++ store and backfill
compare TDLib ids (`server_id << 20`). Only the D1 backend converts:

- Write: `offset_id = tdlib_mid >> 20` when `tdlib_mid % 2^20 == 0`.
  Otherwise the row's offset is not changed. If D1 already has the chat,
  that stored server id is written back with the other columns. If it does
  not, the row is skipped and a warning is logged.
- Read: `offset_id << 20`, unless the shift would overflow (the row is
  skipped). SQLite still stores the TDLib id unchanged.

### Keep-alive

One libcurl easy handle is owned by the backend and reused for every
request (flush thread and startup restore; a mutex guards it anyway).
HTTP/2 is requested (`CURL_HTTP_VERSION_2TLS`, falling back to 1.1
keep-alive). TCP keepalive idle and interval are 20s, `CURLOPT_MAXAGE_CONN`
is 60s, connect timeout is 10s, total timeout is `D1_TIMEOUT`. A
connection-level error destroys and recreates the handle before the retry.
`CURLINFO_NUM_CONNECTS == 0` is logged at debug as a reused connection.

### Failures

`Upsert` throws `std::runtime_error`. `ChatStateStore::FlushActive` logs
that and leaves memory authoritative. `LoadAll` throws as well: startup
restore does not catch it, so a D1 outage during boot exits the process
the same way a corrupt SQLite file does. Neither path runs on a worker or
the TDLib receive thread.
