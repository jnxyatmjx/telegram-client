# Retry / dedup design review — ImageRequest path (ProcessImages)

Date: 2026-11-10 · Scope: analysis only, no code changed.
Baseline: `925a0aa` ("Fix duplicate send / duplicate DONE for one /process").

## Decisions (Hong, 2026-11-12) — these settle §4's open questions

1. **SearchImages: follow the Python contract, NO retry mechanism added for
   now.** The search path stays as-is (write-failed / stream-failed requests
   are dropped; the user re-triggers). The lightweight search retry sketched
   in the 2026-11-12 three-file review is NOT to be implemented. Only the
   ProcessImages path gets the (a2) treatment.
2. **Server-side dedup key is `(cid, image_id)`, NOT `image_id` alone.**
   `image_id` is `str(message_id)` and is only unique within one chat, so
   the server must dedup on `(owner_id, image_id)` together. (The vector
   record already stores both as `cid` / `mid`.)
3. **Flow-control window W = 60** (process path only; search bypasses it).
   With W = 60 the worst legitimate in-flight latency is ≈ 60 × per-image
   time, which the `sent_timeout_ms` safety net (default 600 s) must cover.
4. **The server may be modified, but keep the changes minimal.** Server
   work is limited to the `(cid, image_id)` dedup (in-memory; see §4 Q1).
   The "always reply on failure" and "don't drain the whole queue on a dead
   connection" items are deferred — the system stays correct without them
   (failed images reach DEAD_LETTER via the safety net; a drained queue is
   recovered by the safety-net re-send), just slower to recover.

§4 status after these decisions: Q1 answered (no server dedup today — read
`face_server_tx2.py`; dedup key `(cid, image_id)`, in-memory first); Q2 open
(per-image time T still unmeasured — measure from live reply latency, then
re-check `sent_timeout_ms` against 60 × T); Q3 moot for now (no proto change);
Q4 answered (duplicate replies are harmless client-side — watermark is
idempotent — but duplicate *processing* pollutes Milvus, which is why the
server dedup exists); Q5: dead-letter SENT on permanent errors (as §3.1);
Q6 answered (safety net 600 s is acceptable with W = 60 bounding the queue).

Files read: `src/infra/Outbox.h/.cpp`, `src/app/Application.cpp/.h`,
`src/app/AppContext.h`, `src/app/Config.h/.cpp`,
`src/features/media_thumbnail/MediaThumbnailHandler.cpp`,
`src/transport/AsyncStreamer.h`, `src/transport/StreamerPool.h`,
`src/transport/ProcessImagesClient.h`, `src/common/Idempotency.h`,
`proto/image_stream.proto`, `tests/test_outbox.cpp`.

---

## 0. The system as built

### 0.1 Send path

1. `MediaThumbnailHandler::FinishProcess`
   (`src/features/media_thumbnail/MediaThumbnailHandler.cpp:88`) builds the
   `ImageRequest`, then **Outbox first** (v5 §2.2): `ctx.outbox.Put(record)`
   with `state = kPending` and
   `next_retry_at_ms = now + grpc.ack_timeout_ms` (line 154; default 30 s,
   `src/app/Config.h:42`) — the post-925a0aa ack deadline. Then
   `ctx.process_images.Enqueue(request, 0 ms)` (line 157).
2. `StreamerPool::Enqueue` (`src/transport/StreamerPool.h:94`) deals the
   request round-robin to **one** lane; `AsyncStreamer::Enqueue`
   (`src/transport/AsyncStreamer.h:141`) pushes into the bounded write queue
   (`grpc.queue_capacity`, default 1000). Return value means "accepted into a
   lane's write queue" — **not** "sent".
3. The lane's CQ thread `PumpWrite` (`AsyncStreamer.h:275`) issues one
   `Write` at a time (`write_in_progress_`, single `write_tag_`); the next
   queued request is written only after `OnWriteDone(ok)` (line 291).
4. `OnWriteDone(ok == false)` → `FailStream` → `ClearQueue()` (drops every
   queued-but-unwritten request) → `Finish` → backoff → reconnect.
   `OnFinish` (`AsyncStreamer.h:332`) also calls `ClearQueue()`.
   **No callback reaches Application on write success or failure — the
   streamer is the only component that knows whether a request was written,
   and it tells nobody.**
5. Reply: `OnReadDone` → reply handler wired in
   `Application::WireStreamerCallbacks` (`src/app/Application.cpp:177`) →
   `outbox_.MarkDone(reply.image_id(), reply.result())`. Since 925a0aa,
   `Outbox::MarkDone` (`src/infra/Outbox.cpp:284`) transitions only
   `PENDING/FAILED → DONE`; a duplicate reply is logged and ignored.
6. `SetOnReconnected` → `SweepOutbox()` (`Application.cpp:241`); this runs on
   the lane's CQ thread (`AsyncStreamer::OnStart`, line 259).
7. `SetOnStreamFailed` (`Application.cpp:227`): on *permanent* codes
   (`IsPermanentGrpcError`: INVALID_ARGUMENT, PERMISSION_DENIED,
   UNIMPLEMENTED, NOT_FOUND) it dead-letters **all** PENDING/FAILED records
   (`DueForRetry(now + 24h, 10'000)` — i.e. everything, limit 10000).
8. Retry thread (`StartOutboxRetryThread`, `Application.cpp:266`): every
   `outbox.retry_interval_ms` (10 s) calls `SweepOutbox()`
   (`Application.cpp:245`): `DueForRetry(now, 100)` → for each record,
   `Enqueue` and, on success, `MarkRequeued(id, retry_backoff_ms /*5 s*/,
   max_attempts /*5*/, "requeued for send")`.

`Outbox::DueForRetry` (`Outbox.cpp:346`) selects
`state IN (PENDING, FAILED) AND next_retry_at <= now ORDER BY next_retry_at
LIMIT n`. `MarkRequeued` (`Outbox.cpp:304`) does `attempts+1`,
`next_retry_at = now + delay`, and `state = DEAD_LETTER` when
`attempts+1 >= max_attempts`. It has **no source-state restriction** — that
matters for the design below. `Outbox::Put` (`Outbox.cpp:235`) is
INSERT-or-UPDATE keyed on `image_id` and is called from exactly one
production site: `FinishProcess` (verified by grep).

### 0.2 What 925a0aa fixed, and what it left open

The incident: a server took > 10 s (one sweep interval) per image; the old
code left `next_retry_at = 0` at Put, so the sweep re-sent the same request;
the server replied once per send; one `/process` logged several DONEs.
925a0aa (a) defers the first sweep past the ack timeout and (b) makes
`MarkDone` idempotent. That fixes the *single-image, slow-server, first-send*
case. It does not fix the class, and reading the current code shows four
concrete gaps:

- **G1 — the sweep re-arm ignores the ack deadline.** The 30 s deadline is
  applied only in `FinishProcess`. Every *re-send* by `SweepOutbox` re-arms
  with `retry_backoff_ms` (5 s), not `ack_timeout_ms`. With the defaults
  (ack 30 s, sweep 10 s, backoff 5 s, max_attempts 5) an image the server
  has not replied to within ~70 s follows this timeline:
  Put at t=0 (due t+30) → re-send at t≈30 (attempts 1, due t+35) → re-send
  at t≈40 (2) → t≈50 (3) → t≈60 (4) → t≈70: 5th requeue **dead-letters** the
  record, with 5 copies already on the wire. A slow-but-alive server turns
  "duplicate DONEs" into "5× duplicate work, then the record is dropped".
  The reply that finally arrives hits the
  `"reply for unknown image_id"` warn branch (`Application.cpp:195`).
- **G2 — a provably-lost send waits out the whole deadline.** When a stream
  breaks, `FailStream`/`OnFinish` drop queued-but-unwritten requests
  (`ClearQueue`), and the record stays PENDING with a future
  `next_retry_at`. Nothing replays it until the deadline passes — even
  though the client *knows* the request was never written. Same for the
  enqueue-failure path: the comment at `MediaThumbnailHandler.cpp:141`
  acknowledges a queue-full record also waits the full ack timeout.
- **G3 — reconnect inside the ack window recovers nothing.** The reconnect
  sweep calls `DueForRetry(now)`, which excludes records whose deadline is
  in the future — exactly the records whose in-flight stream just died.
  Recovery of a broken connection is delayed by up to `ack_timeout_ms`.
- **G4 — the wire truth is unobservable.** `Enqueue == true` is not
  "sent", and "sent" is not recorded anywhere. All timing decisions
  downstream are guesses about an event the client actually observed
  (the `kWrite` tag) but discarded.

G4 is the root cause; G1–G3 are its symptoms.

---

## 1. Why raising the timeout is limited

The timeout (`grpc.ack_timeout_ms`) answers one question: *"how long may the
server legitimately take to reply?"* It is used to answer a different one:
*"should I send this again?"* Three structural failures:

### 1.1 Batch load: the timeout rides a moving target and creates a feedback loop

The server's reply latency under a batch of N images is not a constant — it
is queue position × per-image time. Concretely (assuming the server processes
serially at T seconds/image — unknown, see §4):

- All N requests land within one sweep; the last image's reply takes
  ≈ N·T. No *fixed* timeout covers both the first image (T) and the last
  (N·T): a timeout ≥ N·T makes single-image failure recovery N·T slow;
  a timeout < N·T re-sends every image that is still queued.
- Each re-send is *new work* for the same saturated server: the queue grows
  by the re-sends, which pushes later images past the timeout, which causes
  more re-sends — the retry storm. With 925a0aa the storm is bounded only by
  `max_attempts = 5`, i.e. it ends by **dead-lettering** (G1 timeline): for a
  batch whose tail latency exceeds ~70 s, the timeout does not merely cause
  duplicates, it silently drops records (DEAD_LETTER records are never
  replayed; `DueForRetry` filters them out).
- The 5 s sweep re-arm (G1) makes it worse than "one re-send per
  ack_timeout": once a record is re-queued by the sweep it is re-sent on
  *every* 10 s sweep, so the amplification factor is `max_attempts`, not 1.

### 1.2 High latency: the timeout cannot distinguish lost from slow

A lost request and a slow reply are indistinguishable to a timer, and they
need opposite responses: re-send now vs. keep waiting. On a healthy gRPC
stream, loss is not a timer event at all — the client learns it from
`OnWriteDone(ok == false)` or from the stream dying (`FailStream`/`OnFinish`),
both of which fire immediately. Setting the timeout *low* to catch losses
faster just manufactures duplicates of slow replies; setting it *high* makes
real losses (connection drop with the request still in the write queue — G2/G3)
recover at timeout speed. The information that would resolve the ambiguity
exists one layer down (`AsyncStreamer`) and is thrown away.

### 1.3 The value is a guess, and the trade is duplicates vs. recovery speed

Any fixed value trades two failure modes with no correct setting:

- too low → duplicate server work per slow image (the original incident) and,
  since 925a0aa, dead-lettering of slow records (G1);
- too high → slow recovery after a genuine loss (G2/G3) and a bigger
  "unknown fate" window after every stream break.

Plus the deadline is currently *inconsistent by construction*: 30 s for the
first send (`FinishProcess`), 5 s for every re-send (`SweepOutbox` →
`MarkRequeued`). Whatever value is chosen, two code paths disagree about it.

**Conclusion:** the timeout is the wrong control variable. It should be
demoted to a rarely-hit safety net, and the *primary* re-send trigger should
be the events the transport actually reports: write failure, stream failure,
reconnect. That is design (a).

---

## 2. Alternatives

### (a) In-flight tracking — never re-send what was written to a healthy stream

**Idea.** A record leaves the retry set the moment its request is *written*
(`kWrite` tag, `ok == true`) and returns to it only on: write failure, stream
failure, reconnect-without-reply, or a long safety timeout. The sweep then
only ever re-sends requests whose fate is genuinely unknown or known-bad.

**Two variants:**

- **(a1) In-memory in-flight set.** `std::unordered_set<std::string>` in
  `Application` (or a small tracker); populated by a new
  "written" callback, cleared/reconciled on stream failure. No schema change.
  Weaknesses: it is wiped by restart, which is precisely the failure the
  Outbox exists to survive (`Outbox.h:4-8`). After a restart the client has
  no idea which PENDING records were written pre-crash; either it replays
  them all (duplicate work, and G2-style behavior returns) or it needs the
  information in the DB anyway. It also introduces a second synchronization
  domain (pool × lanes × threads) beside the Outbox mutex, for state that is
  a strict subset of what the record already stores.
- **(a2) New `OutboxState::kSent` (recommended).** The write outcome is
  persisted on the record. The DB stays the single source of truth; sweep
  logic stays SQL-driven and unit-testable in `tests/test_outbox.cpp`
  without a live streamer; restart semantics become explicit; multi-lane
  needs no extra bookkeeping (replies ride the same lane their request was
  dealt to, and each lane's CQ thread serializes its write tag before any
  reply tag, so `MarkSent` precedes `MarkDone` for a given id).

Detailed sketch of (a2) is §3.

**Pros (a2):** removes the duplicate-send class entirely on the healthy path;
recovery on loss becomes event-driven (fast) instead of timer-driven;
`attempts` starts meaning "un-acked re-sends" so `max_attempts` dead-lettering
becomes a meaningful backstop instead of a slow-server kill switch (fixes G1);
no proto change.
**Cons / needs care:** write-ok means "gRPC accepted the message into the
transport", not "the server read it" — a stream can still die between write
and reply, so the safety net and at-least-once replay after stream death stay
necessary, and **server-side dedup by image_id remains the correctness
backstop** (see §4, Q1 — the live test suggests it may not exist today).
Needs a new callback surface on `AsyncStreamer` (it is generic over
`Request/Reply`, so the callback must carry the `Request` and let
`Application` extract `image_id()`); `ClearQueue` must report dropped
requests (care: don't invoke callbacks while holding `queue_mutex_` — swap
the queue out first); any `switch` over `OutboxState` elsewhere must learn
`kSent` (grep found none outside `Outbox.cpp`/`Application.cpp` comparisons).

### (b) Adaptive ack timeout from observed reply latency

**Idea.** Track reply latency (e.g. EMA/p95 over the last N replies) in
`Application::WireStreamerCallbacks`: on a successful `MarkDone`, latency ≈
`now − record.updated_at` (updated_at is written at Put/MarkRequeued time,
i.e. ≈ last send time — needs one extra `Get`, or `MarkDone` extended to
return the record). Set the effective deadline to
`clamp(observed × k, floor, ceiling)`.

**Changes:** `Application.cpp` (latency bookkeeping + recompute),
`Config.h/.cpp` (factor/floor/ceiling), `Outbox` untouched (deadline is a
parameter). Mildly awkward today because the *sweep* path hardcodes
`retry_backoff_ms` (G1) — it would have to read the adaptive value too.

**Pros:** no proto change; tracks a genuinely slow server; fewer duplicates
during sustained-load periods than any fixed value.
**Cons:** it is still a timer answering the wrong question — it cannot tell
lost from slow (§1.2), it *chases* the batch-load curve instead of shaping it
(§1.1: during ramp-up every record is re-sent at least once, because the
observed latency lags the queue growth), adds moving parts and a warm-up
window in which behavior equals today's, and the feedback loop (re-sends
raise latency, raised latency raises timeout) is the thing being feared,
now with extra state. Hard to unit-test deterministically.
**Verdict:** strictly better than a fixed timeout, but it optimizes the same
broken mechanism. If kept at all, its right job is to *size the safety net*
of design (a) (e.g. `sent_timeout = max(floor, observed_p99 × 10)`), not to
be the primary mechanism.

### (c) Bounded in-flight window / flow control for batches

**Idea.** Cap concurrent sent-but-unreplied requests at W (e.g. 4–8, total or
per lane). `FinishProcess` and `SweepOutbox` gate the `Enqueue` on the
window; when full, the record stays PENDING and the retry sweep picks it up
later. The server queue then never grows beyond W, per-image latency stays
roughly flat, and any timeout becomes a valid estimate.

**Changes:** a counter (release points: `MarkDone` success, write-lost,
safety-net requeue, stream failure) in `Application` or a tiny
`FlowControl` class; `FinishProcess` (`MediaThumbnailHandler.cpp:157`) and
`SweepOutbox` (`Application.cpp:253`) check it before Enqueue.
**Important interaction:** with 925a0aa a window-blocked record waits the
full `ack_timeout_ms` (Put sets the deadline before Enqueue is attempted) —
flow control *requires* the Put/deadline split described in §3 (deadline
applied on write, not on Put), otherwise the window silently serializes
batches at 30 s/image. `StreamerPool::Enqueue` also deals to a *single*
round-robin lane and fails if *that* lane is full (`StreamerPool.h:91-99`) —
with `connections > 1` a full lane next to an empty lane reads as
backpressure; care needed when W spans lanes.

**Pros:** attacks the batch root cause (§1.1) instead of detecting it;
bounds memory too (today a burst can hold 1000 images in lane queues plus
their blobs in SQLite); composes cleanly with (a).
**Cons:** a lost reply must release its window slot or the pipeline wedges —
so it depends on the same event-driven release points as (a) plus the safety
net; throughput floor if the server dies (bounded by W × safety timeout);
one more tunable.
**Verdict:** right mechanism, second priority — do it after (a), as a thin
counter on top of the same events, not in the same chunk.

### (d) Server-ACK protocol change (brief note)

Add `ImageAck {string image_id; bool accepted}` to `image_stream.proto`,
sent by the server when it *receives* (not finishes) a request; client
transitions PENDING → SENT on ack. This makes "received" an explicit wire
event instead of inferring it from the write tag, and would keep working if
the client ever moved to a transport without write-tag visibility.

Why not now: (1) it costs a coordinated proto + Python-server + C++-client
change for information the client already has locally (the `kWrite` tag);
(2) an ack proves the gRPC handler got the message, not that processing is
durable — the duplicate-*processing* problem is unchanged, so server-side
dedup is still required, which is the actual safety property; (3) the batch
overload problem is not touched. Revisit only if the server grows an
internal queue invisible to the client, or if (a)'s write-tag semantics
prove insufficient in practice.

---

## 3. Recommendation: (a2) — persisted `kSent` with write-outcome callbacks

One focused chunk; no proto change. The timeout is demoted to a safety net.

### 3.1 State machine

```
Put (FinishProcess)        state = PENDING, next_retry_at = now + send_grace (small, ~5 s)
write-ok (new callback)    PENDING|FAILED -> SENT      (MarkSent; next_retry_at = now + sent_timeout)
write-lost / queue-dropped (new callback) PENDING -> FAILED, due now   (MarkFailed(id, why, 0))
ImageReply                 PENDING|FAILED|SENT -> DONE  (MarkDone: add SENT to the IN-list)
safety deadline fires      SENT -> PENDING due now+backoff, attempts++ (reuse MarkRequeued)
stream failure, permanent  PENDING|FAILED|SENT -> DEAD_LETTER (decision, §4 Q5)
startup (Outbox ctor)      SENT -> PENDING, next_retry_at = now + reconnect grace
```

### 3.2 Changes per file

**`src/infra/Outbox.h` / `.cpp`**
- Add `kSent = 4` to `OutboxState` (state is stored as INTEGER; no schema
  migration needed — old DBs only hold 0..3).
- New `bool MarkSent(const std::string& image_id, std::int64_t sent_until_ms)`:
  `UPDATE ... SET state=SENT, next_retry_at=?, updated_at=? WHERE image_id=?
  AND state IN (PENDING, FAILED)`. Returns false (harmless no-op) for
  already-SENT (duplicate write-ok), DONE (reply raced ahead), unknown id.
- `MarkDone`: `WHERE ... state IN (PENDING, FAILED, SENT)`. The existing
  duplicate-reply guard (`Outbox.cpp:284-301`, test
  `SecondMarkDoneIsNotADone`) keeps working unchanged.
- New `std::vector<OutboxRecord> SentPastDeadline(std::int64_t now_ms,
  std::size_t limit)`: `state = SENT AND next_retry_at <= now` — the safety
  sweep's input. Reuses the existing `outbox_retry_idx (state, next_retry_at)`
  index.
- `DueForRetry`: **unchanged** — `SENT` records are invisible to the normal
  sweep by construction, which is the whole point.
- `MarkRequeued`: unchanged; it already transitions any source state and
  bumps attempts + dead-letters at `max_attempts`, which is exactly the
  safety-net semantic ("re-sent N times, still no reply → give up").
- Constructor: one-time `UPDATE outbox SET state=PENDING,
  next_retry_at=now+grace WHERE state=SENT` (restart recovery: the previous
  process wrote requests whose replies may or may not have been processed;
  replay after a grace period, dedup as backstop). Same style as
  `MigrateLegacyImageIds` (`Outbox.cpp:121`).

**`src/transport/AsyncStreamer.h`**
- New optional callbacks (generic, so they carry the `Request`):
  `using WrittenHandler = std::function<void(const Request&)>;`
  `SetOnWritten(...)` / `SetOnWriteLost(...)`.
- `OnWriteDone(ok)` (line 291): capture `const Request written =
  *pending_write_;` *before* `pending_write_.reset()`. `ok == true` → fire
  `on_written_(written)` then `PumpWrite()`; `ok == false` → fire
  `on_write_lost_(written)` then `FailStream(...)`. Ordering is safe: the
  callback runs on the CQ thread, after the write outcome is known, before
  any reply for that message can be delivered on this stream.
- `ClearQueue` (line 385): swap the queue out under the lock, then fire
  `on_write_lost_` for each dropped request *outside* the lock (the callback
  touches SQLite, not the streamer; avoids holding `queue_mutex_` across
  user code and any re-entrancy into `Enqueue`).

**`src/transport/StreamerPool.h`**
- Forward `SetOnWritten` / `SetOnWriteLost` to all lanes, same pattern as
  `SetReplyHandler` (`StreamerPool.h:101`). Only the process pool gets them
  wired; SearchImages has no Outbox and simply never sets them.

**`src/app/Application.cpp`**
- `WireStreamerCallbacks`: `process_pool_.SetOnWritten([&](const
  ImageRequest& r){ outbox_.MarkSent(r.image_id(), NowMs() +
  config_.outbox.sent_timeout_ms); });` and `SetOnWriteLost` →
  `outbox_.MarkFailed(r.image_id(), "write lost", 0)` (due immediately).
  Both run on a lane's CQ thread — same threading model as the existing
  reply handler and the reconnect-driven `SweepOutbox`; the Outbox is
  FULLMUTEX + internally locked (`Outbox.cpp:100`), and CQ-thread SQLite
  work is already established practice here.
- `SweepOutbox` (`Application.cpp:245`): after the existing due sweep, a
  safety pass: `for (r : outbox_.SentPastDeadline(now, kSweepBatch))
  outbox_.MarkRequeued(r.image_id, retry_backoff_ms, max_attempts, "ack
  timeout");` — they become PENDING due-now and the next sweep re-sends
  them. This is also where attempts now *only* mean "re-sends that never
  got a reply".
- `SetOnStreamFailed` (`Application.cpp:227`): include SENT records in the
  permanent-error sweep (decision point, §4 Q5) — if the server closed the
  stream with INVALID_ARGUMENT after reading the request, replay cannot
  succeed.
- `SetOnReconnected` → `SweepOutbox()`: unchanged, but now meaningful for
  write-lost records (they are FAILED/due-now and get replayed on the
  reconnect sweep immediately — fixes G3) while genuinely-sent records are
  *not* blindly replayed (fixes the duplicate class).

**`src/features/media_thumbnail/MediaThumbnailHandler.cpp`**
- `FinishProcess` (line 148-163): `record.next_retry_at_ms = now_ms + send_grace`
  (a few seconds, enough for the write to complete; not `ack_timeout_ms`) —
  the deadline now means "grace until the write outcome is known", and the
  authoritative deadline is set by `MarkSent`. This also fixes the
  enqueue-failure wart: on `Enqueue == false` call
  `ctx.outbox.MarkFailed(image_id, "queue full", 0)` so the sweep retries
  immediately instead of after the full grace.
- The Put→write-ok race (a sweep between Put and MarkSent seeing the record
  due) is closed by the grace window: sweeps are 10 s periodic, and the
  reconnect sweep can only fire after a reconnect that happened *after* Put,
  by which time the write outcome is known (or the request was lost →
  write-lost already re-armed it).

**`src/app/Config.h` / `Config.cpp`**
- `OutboxConfig`: add `send_grace_ms` (default ~5'000) and
  `sent_timeout_ms` (safety net; default 600'000 — must exceed the worst
  legitimate processing time; see §4 Q2). Parse both in `Config::FromFile`
  alongside the existing outbox keys (`Config.cpp` outbox section).
  `grpc.ack_timeout_ms` becomes obsolete for this path — keep the key or
  remove it; do not keep both alive.

**`tests/test_outbox.cpp`** — new cases mirroring the existing style:
`MarkSent` then `DueForRetry` empty; `SentPastDeadline` returns only past-
deadline SENT rows; `MarkDone` on SENT succeeds (and second call fails);
`MarkSent` on DONE returns false; reopen transitions SENT → PENDING with
future deadline; `MarkRequeued` from SENT dead-letters at exhaustion.

### 3.3 Event-by-event behavior after the change

- **Normal path:** Put → write-ok → SENT (invisible to sweeps) → reply →
  DONE. Zero duplicate sends, whatever the server's latency.
- **Enqueue fails / request dropped from a breaking stream's queue:**
  write-lost → FAILED due-now → replayed by the next sweep or the reconnect
  sweep (seconds, not `ack_timeout`) — fixes G2/G3.
- **Stream dies after write-ok, before reply:** fate unknown. The record
  stays SENT until the safety deadline (`sent_timeout_ms`), then one
  re-send (at-least-once). This is the residual duplicate window; it is
  inherent (the client cannot know) and is exactly what server dedup must
  cover. Optional later extension: `AsyncStreamer` tracks the ids written
  on the *current* stream and reports them at `FailStream`, letting
  Application re-arm only those records right after reconnect — faster
  recovery, deliberately deferred to keep this chunk small (§4 Q6).
- **Restart:** ctor migrates SENT → PENDING(+grace); replay after grace;
  dedup backstop.
- **Safety deadline fires:** SENT → PENDING, attempts++, next sweep sends.
  After `max_attempts` un-acked re-sends → DEAD_LETTER, surfaced in
  `last_status`. Worst case for a truly stuck id:
  ~`max_attempts × sent_timeout_ms` (≈ 50 min at defaults) instead of
  today's ~70 s kill of slow-but-alive records.
- **Permanent gRPC error:** unchanged dead-letter semantics, extended to
  SENT (§4 Q5).

### 3.4 What this does *not* fix

Duplicate *processing* when a genuinely-delivered request is replayed
(safety net, restart, reconnect-after-write) — that is server-side dedup by
`image_id` (`MakeWireImageId`, `src/common/Idempotency.h:81`), and the live
test ("reply once per send") suggests it may not exist (§4 Q1). Batch
arrival shaping is design (c), a deliberate follow-up.

---

## 4. Open questions

1. **Does the server dedup by `image_id` today?** The code asserts it
   (`Outbox.h:7-8`, `Idempotency.h:74-80`), but the observed live test — a
   reply for *every* re-send — suggests it does not (or only post-hoc).
   If there is no dedup, at-least-once replay on reconnect is *already*
   double-processing today, independent of any client change; this should
   be verified (or fixed server-side) before relying on it.
2. **What is the server's per-image processing time, solo and under batch
   load (queue depth, serial or parallel)?** Needed to size
   `sent_timeout_ms` (safety net) and, later, the window W in design (c).
3. **Is a proto change acceptable later** (`ImageAck`, §2d) if the server
   grows an internal queue whose depth the client cannot see?
4. **Are duplicate `ImageReply`s harmful downstream?** Today each reply
   calls `chat_state_.ApplyReply(parsed->cid, parsed->mid)`
   (`Application.cpp:199-209`) even for already-DONE ids — is the watermark
   advance idempotent under a repeated (cid, mid)?
5. **Should a permanent stream failure (INVALID_ARGUMENT family)
   dead-letter SENT records too**, or leave them to the safety net?
6. **Is the safety-net latency acceptable** (up to `sent_timeout_ms` before
   replaying a written-but-unreplied request after a stream death), or
   should the per-stream written-id tracking (§3.3 optional extension) be
   pulled into the first chunk?
