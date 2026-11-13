# Build notes — tg-client

> **Session log, not current behavior.** Entries below record what was
> true on the date in the heading (test counts, the pre-migration
> `image_id` form, `memo` equal to the caption). Later protocol,
> Outbox and flow-control behavior is in `README.md`
> ([Outbox state](README.md#outbox-state)). Do not treat a dated result
> such as "ctest 9/9" or "image_id = -100…:777:u-m" as what `main` does
> now.



## TDLib: build once, never rebuild (Hong, 2026-11-08 10:18)

TDLib (`tdcore` / `Td::TdStatic`) is built once. After that, later code
changes must NOT trigger a TDLib rebuild — only our own code is recompiled.

- First full build: 2026-11-08, `nohup cmake --build build -j1`,
  log `tdlib-build.log` (in this directory, never /tmp).
- Future builds — build only our targets, never the whole tree:
  `cmake --build build --target tg-client -j1`
  then `ctest --test-dir build` (or build the individual test targets).
- NEVER `rm -rf build`, never `cmake --build build --target tdclient`,
  never clean/rebuild `tdcore`. If the build dir is lost, TDLib must be
  rebuilt from scratch (20–60 min) — avoid that.
- If a VM replacement happens: deps in /usr are wiped (only $HOME
  persists). Reinstall them first (`apt-reinstall.log` has the package
  list), verify `cmake` configures, then build only the tg-client targets.

## Build commands

- Configure: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
- Full build (first time only): `cmake --build build -j1`
- Incremental (normal): `cmake --build build --target tg-client -j1`
- Tests: `ctest --test-dir build --output-on-failure`

## 2026-11-08 chunk 2 finish: AsyncStreamer teardown fix (production code)
- `test_async_streamer` hung in teardown even though the round-trip itself
  worked (2 writes ok, 2 replies ok). Root cause: `AsyncStreamer::Stop()`
  called `cq_.Shutdown()` BEFORE joining the CQ thread, stranding the
  stream with an outstanding Read. The stream was then destroyed without
  its Finish tag, so the cancel never reached the server and the fake
  server hung in `Read()` forever; `server_->Wait()` never returned.
  This violated the file's own teardown contract (Finish must be the last
  op; destroy the stream only after its Finish tag).
- Fix in `src/transport/AsyncStreamer.h` only: `Stop()` now joins the CQ
  thread FIRST (the CQ loop cancels the live call on TIMEOUT via
  `context_->TryCancel()` — issued from the CQ thread, never from Stop(),
  to avoid racing `Connect()` resetting `context_`), lets the stream drain
  through WritesDone -> Finish -> OnFinish, and only then calls
  `cq_.Shutdown()` and resets stream/context.
- Test fix in `tests/test_async_streamer.cpp` only: `StopServer()` now uses
  `server_->Shutdown(deadline +2s)`. A plain Shutdown()/Wait() never
  returns while a bidi stream is open (handler blocked in Read()); the
  deadline force-cancels in-flight calls. This is what "killing the
  server" means for a long-lived stream.
- Temporary `[DBG] fprintf` prints were used to find the hang and have
  all been removed; no debug output remains in the header.
- Result: `ctest --test-dir build/tests` 8/8 passed (2026-11-08 04:21 UTC),
  total 2.48 s. TDLib was NOT rebuilt (libtdcore.a/libtdclient.a still
  02:21 UTC); only tg-client/test targets were built.

## 2026-11-08 chunk 3: fake-server end-to-end test (COMPLETED)
- New test binary tests/test_e2e_fake_server.cpp (registered in
  tests/CMakeLists.txt, TIMEOUT 120). No production code changed.
- Wiring (all real production code): local thumbnail file on disk ->
  MediaThumbnailHandler / SearchHandler -> real FileDownloader (fake
  ITdSender answers downloadFile inline with the local file path) ->
  real GrpcTransport (target unix://..., tls off) + ProcessImagesClient /
  SearchImagesClient -> AsyncStreamers -> in-process fake ImageProcessor
  server on a Unix domain socket. Reply handlers wired exactly like
  Application::WireStreamerCallbacks (Outbox MarkDone; correlation lookup).
- Test 1 ProcessImagesThumbnailFileRoundTrip: photo message (channel
  -1001234567890, msg 777, 320px tier u-m, caption "hello caption") ->
  SendProcessImages ok, image_id "-1001234567890:777:u-m"; reply
  round-trips by image_id with result "ok:<image_id>"; Outbox record is
  DONE; server received image_data == file bytes, memo == caption only,
  owner_id == chat id.
- Test 2 SearchImagesBareIdRoundTrip: same message -> SendSearchImages ok;
  SearchRsp round-trips by (oid=1234567890, id=777); correlation table
  resolves to the original chat/message/image_id; server received the bare
  uint32 ids, never the TDLib dialog id.
- Result: ctest 9/9 passed (2026-11-08 12:25 CST), total 2.59 s.
  Build: Release (CMAKE_BUILD_TYPE=Release), 0 errors, binary
  tg-client 0.1.0, --version/--help work. No Python, no sync stub calls.
- README updated: build section now documents the real build dir (build/,
  tg-client/test targets only, never rebuild TDLib), the test table with
  all 9 binaries, and the unix-socket / other_tcp platform note.

## Async message processing (2026-11-09 21:26-21:50 CST)
- Requirement (Hong): Telegram message processing — receiving AND bot
  commands — must be async; messages must not synchronously block each
  other. Audit found the Receive path was already async, but the pipeline
  blocked: BotCommandHandler waited on getMessage (SendWithFuture +
  wait_for, up to 10s) and both handlers waited on the download future
  (up to 30s) on 4 worker threads, so a few slow downloads could stall
  later messages.
- Refactor (callback/continuation, no blocking waits anywhere):
  * FileDownloader: new DownloadAsync(file_id, callback); pending map is
    now file_id -> vector<callback> (concurrent downloads of the same file
    share one downloadFile request); future Download() kept as a thin
    wrapper for tests; FailAll now invokes pending callbacks with an error.
  * AppContext gained WorkerPool& workers (callbacks post continuations).
  * MediaThumbnailHandler/SearchHandler: Send*Images are async kick-offs
    (extract + DownloadAsync, return immediately); download callback posts
    the finish stage (read bytes -> Outbox -> non-blocking Enqueue, timeout
    0) to the pool. PipelineResult removed.
  * BotCommandHandler: getMessage via Send() callback (no timeout — a
    never-answered getMessage just drops the command); the callback posts
    the pipeline kick-off to the pool. get_message_timeout_ms config removed.
  * ITdSender::SendWithFuture removed (both the interface default and the
    TelegramClient override) — no blocking wait API remains.
  * Shutdown order changed: telegram_.Stop() first (completes all pending
    TDLib callbacks -> download callbacks post finish stages), then
    files_.FailAll, then workers drain, then streamers, then retry thread.
  * New regression test BotCommandTest.SecondCommandCompletesWhileFirstDownloadPending:
    A's download takes 2s (fake delivers the response from a background
    thread), B's is instant; B's request reaches the streamer queue while A
    is still downloading. Verified it fails the old ordering (A would be
    first). FakeTdSender gained per-message targets + slow-download support.
- VM was replaced again before the build: apt deps wiped (only
  libsqlite3-dev survived). Reinstalled (apt-reinstall3.log,
  APT_REINSTALL3_OK rc=0) — same set as chunk 1.
- Verified: build exit 0, 0 own-code warnings, ctest 13/13 (5.16s),
  tg-client --version/--help work. TDLib NOT rebuilt (tg-client targets only).
