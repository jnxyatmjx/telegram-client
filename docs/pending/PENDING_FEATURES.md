# Pending features — Telethon migration follow-ups

Recorded 2026-11-11 per Hong. The retry redesign below has since landed.
**D1 is still not implemented** — it needs Hong's Cloudflare credentials.
Do not describe the D1 client as current behavior.

## 1. Retry redesign (in-flight / kSent) — done

Shipped on `main` (`42c75b0`, flow-control slot release on a successful
`ImageReply` in `4cec0e3`). Current behavior is
[Outbox state](../../README.md#outbox-state) in the README. The design
write-up is `retry-design.md`: its opening decisions are Hong's
2026-11-12 choices; **§0 is the pre-change baseline at `925a0aa`, not
what the client does now.**

What the code does, relative to that write-up:

- **(a2) is in.** `OutboxState::kSent = 4`, `MarkSent` on write-ok,
  `MarkFailed` on write-lost (from `kPending` only), safety-net
  `outbox.sent_timeout_ms` (default 600000), startup
  `kSent → kPending` after `send_grace_ms`.
- **(c) is in too**, not deferred: `outbox.max_in_flight` (default 60,
  `0` disables it) on the ProcessImages path only. `TryAcquire` before
  enqueue; a full window leaves the row `kPending` and does not burn
  `attempts`. `Release` runs when `MarkDone` returns true, on
  write-lost, on a false `MarkSent`, on a failed enqueue after
  acquire, on a safety-net requeue, when a permanent error dead-letters
  a `kSent` row, and when `FinishProcess` replaces a row that is still
  `kSent`. A false `MarkDone` (already `kDone`, `kDeadLetter`, or an
  unknown id) releases nothing.
- **(b) and (d) are not implemented.** `grpc.ack_timeout_ms` is still
  parsed and is not read. No `ImageAck` proto change.
- The Outbox `image_data` BLOB stayed. It is the retry byte source.

## 2. D1 hookup (Cloudflare D1 state persistence)

- **Design doc:** `state-persistence-and-d1.md` (written in Chunk 2).
  `IMPLEMENTATION_PLAN.md` is not in this repository.
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

- The shipped retry redesign kept the Outbox `image_data` BLOB — it is
  the retry byte source, including retry-after-restart. (The earlier
  version of this section said to remove it; that was wrong and was
  corrected here on 2026-11-11.)
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

- Retry redesign (a2) plus the flow-control window — `42c75b0`,
  slot release on successful `MarkDone` — `4cec0e3`. See §1 above.
- Migration Chunk 1 (protocol alignment) — remote `a939e0b`.
- Migration Chunk 2 (state persistence, backfill, commands) — remote
  `da529f0`.
- Migration Chunk 3 (gRPC channel args, FloodWait, docs) — remote
  `9707ed9`.
- Duplicate-DONE fix — remote `925a0aa`.
- owner_id int64 fix — in `da529f0`.
- deleteFile cleanup (delete local cached file after download + BLOB
  store, reference-counted) — remote `9775eb9`, ctest 24/24 at that
  commit. The registered suite is now 26 binaries. Pre-existing
  files in `files_directory` from before this change are not auto-cleared.
