# Chunk 2 — RESUME instructions (written 2026-11-08 06:58 CST, after VM replacement)

> **Obsolete.** Chunk 2 finished and later chunks landed on `main`.
> The "still missing" file list and the partial TDLib build described
> below are not the tree today. Do not follow these steps. Current
> build and Outbox behavior: `README.md`.



You are resuming chunk 2 of the TDLib + gRPC C++ build. The previous run died at
2026-11-07 21:45 UTC on a GLM connection error (ECONNREFUSED, transient — the
endpoint answers again now). The VM was replaced again at ~22:53 UTC; the process
is gone, but everything on disk survived. Do NOT restart from scratch.

## Verified on-disk state — check with `ls` before you claim anything else

- Real implementation files written so far (only these): `src/common/Logger.h`,
  `src/common/Idempotency.h`, `src/common/Idempotency.cpp`,
  `src/app/Config.h`, `src/app/Config.cpp`, `src/app/WorkerPool.h`,
  `src/telegram/UpdateHandler.h`. Total ~800 lines.
- Still MISSING (do not claim they exist): `src/main.cpp`, `src/telegram/`
  TelegramClient / AuthManager / UpdateDispatcher / FileDownloader,
  `src/transport/*`, `src/features/bot_command/*`, `src/features/auto_search/*`,
  `src/features/media_thumbnail/*`, `src/infra/*`, and all `tests/*.cpp`
  (only `tests/CMakeLists.txt` exists). The chunk-1 placeholders were deleted
  during chunk 2 — that is expected, do not restore placeholders.
- TDLib build in `build/` is PARTIAL: `libtdapi.a` / `libtdmtproto.a` built
  21:41–21:42 UTC, `tdcore` objects still compiling when killed. Resume it,
  do NOT clean it. Build with `-j1` only (7.7 GB RAM, ~1 GB free).
- No `tg-client` binary exists yet in `build/` (only the old placeholder one
  in `build-no-td/` — ignore it).

## Rules for this run

- Work ONLY in `/home/hatch/workspace/tg-client` — use absolute paths everywhere.
  Your cwd drifted into `third_party/td/td/generate/scheme` last time and a
  cleanup `rm` misfired because of it. `cd` to the project root first and stay there.
- Follow `IMPLEMENTATION_TASK.md` and `~/workspace/tdlib-grpc-architecture.md` (v5).
  C++ only, fully async gRPC, `memo` = caption only, bot-command trigger mode (a),
  auto-search phase 2 implemented but disabled by default, bare-ID conversion with
  range checks, one process / one binary / one TDLib client.
- Finish chunk 2: write the remaining sources, resume the TDLib build, compile
  `tg-client`, run `ctest`, fix failures yourself. Write each file to disk as you
  go. Do not keep retrying a failing command unchanged — diagnose, change one thing.
- STOP when chunk 2 is done (binary builds, ctest passes) or genuinely blocked.
  Do not start chunk 3. End with a short summary: what built, test results,
  what is still missing (live Telegram account, live gRPC server, bot user_id,
  real command names — those need the user).
