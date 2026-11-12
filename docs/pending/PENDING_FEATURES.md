# Pending features — Telethon migration follow-ups

Recorded 2026-11-11 per Hong. These two features are **not yet implemented**;
this file is the record so they are not lost. Implement them only when Hong
says to (retry redesign needs his choice on the design; D1 needs his
Cloudflare credentials).

## 1. Retry redesign (in-flight / kSent)

- **Design doc:** `retry-design.md` (26 KB, written 2026-11-10 by Claude Code
  CLI analysis; no code changed).
- **Status:** design written, **awaiting Hong's decision**. Do not implement
  until he chooses.
- **Problem it solves:** the `925a0aa` ack-timeout fix is incomplete. A
  freshly-sent Outbox record is Put with `next_retry_at = now + ack_timeout`
  (30 s), but every re-send re-arms with `retry_backoff_ms` (5 s), so a
  server slower than ~70 s/image gets 5 duplicate sends and then the record
  DEAD-LETTERS (and the late reply hits "reply for unknown image_id").
  Related gaps: a provably-lost send (stream break, queue dropped) still
  waits out the full ack deadline; reconnect inside the ack window recovers
  nothing; write success is unobservable (`Enqueue == true` is not "sent").
- **Recommended design (a2):** persisted `OutboxState::kSent` with
  write-outcome callbacks — `MarkSent` on write-ok, `MarkFailed` on
  write-lost, safety-net `sent_timeout_ms` (default 10 min), startup
  migrates `SENT → PENDING`. Alternatives in the doc: (b) adaptive timeout,
  (c) bounded in-flight window, (d) server-ACK proto change (not
  recommended).
- **Open questions (from the design doc):** does the server actually dedup
  by `image_id` (live test suggests maybe not)? Server per-image time under
  batch load? Is a proto change acceptable? Is the duplicate-reply
  watermark advance idempotent? Permanent-error handling for SENT?
  Safety-net latency acceptability?

## 2. D1 hookup (Cloudflare D1 state persistence)

- **Design doc:** `state-persistence-and-d1.md`
  (written in Chunk 2) + `IMPLEMENTATION_PLAN.md` item 10.
- **Status:** SQLite backend is the working temporary implementation
  (Chunk 2). D1 is the final target. **Needs Hong's Cloudflare
  credentials** (`CLOUDFLARE_ACCOUNT_ID`, `CLOUDFLARE_API_TOKEN`,
  `D1_DATABASE_ID`) via the Secure Vault — never in chat or files.
- **What to build:** `D1Client` behind the same `ChatStateBackend`
  interface, using the D1 REST API: `POST
  {D1_API_BASE}/accounts/{account_id}/d1/database/{database_id}/query`
  (default base `https://api.cloudflare.com/client/v4`), `Authorization:
  Bearer {CLOUDFLARE_API_TOKEN}`, timeout 35 s, retry 429/500/502/503/504
  with exponential backoff (0.5·2^(n-1), cap 8 s), max 3 attempts, 401/403
  fail immediately; named params `:name` → `?` placeholders; batch insert
  chunked by `D1_MAX_SQL_VARIABLES` (default 90), `D1_BATCH_SIZE` default
  500. Reference: Python `D1Database` (`app_client.py:449-648`) and the
  Cloudflare D1 docs (llms.txt, OpenAPI spec).
- **Note:** D1 stores chat state (watermarks / `chathis`), never image
  bytes — see the memory-only constraint below.

## Design constraint — image / thumbnail data (corrected 2026-11-11)

Hong clarified 2026-11-11 12:36 CST (after the first version of this section
was written): **TDLib's internal operations are out of scope; the only thing
that matters is the client itself deliberately downloading/saving a file to
local disk. Saving image bytes in the Outbox SQLite `image_data` BLOB is
acceptable and stays as the retry byte source.**

What this means for the two features above (and the client generally):

- The retry redesign **keeps** the Outbox `image_data` BLOB — it is the
  retry byte source, including retry-after-restart. Do **not** remove it
  as part of the retry redesign. (The earlier version of this section said
  the opposite; that was wrong and was corrected here on 2026-11-11.)
- D1 stores chat state only (watermarks, chat URLs, timestamps). It must
  never receive image bytes.
- The client itself never writes an image file. A Telegram message carries
  only file metadata, so TDLib's `downloadFile` is the only way to fetch
  bytes, and it always writes to `telegram.files_directory`.
- **TDLib downloads off persistent disk:** point `files_directory` at a
  tmpfs mount (e.g. `/dev/shm/tdlib-files`), so the download lands in RAM
  and vanishes on reboot. Documented in `config/config.example.yaml`,
  pushed as `c9c6da7`. Hong applies this on his WSL and Debian machines by
  changing `files_directory` in his own `config.yaml` — no code change.

### Current state (no conflict)

- **Outbox persists image bytes today, by design.** `OutboxRecord.image_data`
  is stored as a `BLOB` in the local SQLite outbox
  (`src/infra/Outbox.cpp`: `image_data BLOB NOT NULL`, `Put` binds the
  blob). That is how retry-after-restart works today, and Hong confirmed
  it is fine.
- **TDLib `downloadFile` writes to `telegram.files_directory`** — handled
  by the tmpfs config above, and since `9775eb9` also by code: after the
  bytes are stored in the Outbox BLOB (or the SearchReq is enqueued),
  `deleteFile` removes the local cached file (reference-counted per
  file_id). Tmpfs remains an optional extra, not a requirement.

## Done (for reference — not pending)

- Migration Chunk 1 (protocol alignment) — remote `a939e0b`.
- Migration Chunk 2 (state persistence, backfill, commands) — remote
  `da529f0`.
- Migration Chunk 3 (gRPC channel args, FloodWait, docs) — remote
  `9707ed9`.
- Duplicate-DONE fix — remote `925a0aa`.
- owner_id int64 fix — in `da529f0`.
- deleteFile cleanup (delete local cached file after download + BLOB
  store, reference-counted) — remote `9775eb9`, ctest 24/24. Pre-existing
  files in `files_directory` from before this change are not auto-cleared.
