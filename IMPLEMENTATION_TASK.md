# Implementation task for Claude Code (GLM, opus model)

You are Claude Code running on this cloud Linux VM (Ubuntu 24.04, 4 CPU, 7.7 GB RAM,
~150 GB home disk). Your job: implement the C++ Telegram client described in
`~/workspace/tdlib-grpc-architecture.md` (v5, read it fully first) in this directory
(`~/workspace/tg-client/`). The proto file is already here at `proto/image_stream.proto`
(copied verbatim from the user, do not modify it).

## Non-negotiable decisions (from the user, already confirmed)

- Language: C++ only, no Python anywhere in the implementation.
- TDLib is a Telegram client core, not a one-off bridge. Image/video thumbnail sending
  via gRPC is only the first feature; the core must be extensible (handler registry).
- gRPC: fully async, `grpc::CompletionQueue`, no synchronous stub calls anywhere.
- Proto: `ImageProcessor` with `ProcessImages` and `SearchImages`, both bidirectional
  streaming. No invented `MediaService`, no unary calls.
- `memo` in `ImageRequest`: caption only, never JSON, never metadata smuggling.
- Trigger (phase 1, build this now): both RPCs are triggered ONLY by a specific command
  from a specified bot in Telegram, mode (a): the bot itself is the sender of the command
  message. Verify `sender_user_id == command_bot_user_id`. Command names are placeholders
  for now: `/process` and `/search` (configurable). Target message is given by the
  command's reply-to message, or by `chat_id message_id` arguments.
- Trigger (phase 2, implement but DISABLED by default): a user posting an image/video
  message in a specified channel/group automatically triggers `SearchImages` ONLY
  (never `ProcessImages`). Config `features.auto_search.enabled: false` by default.
  Implement `AutoSearchHandler` with a chat allowlist and a (oid,id) dedup set
  (TTL 10 min). Do not enable it.
- `SearchReq.id` / `oid` are `uint32` because the server side (Telethon/MTProto) uses
  bare IDs, not TDLib Bot API dialog IDs. Convert on the client side:
  - private chat (`chat_id > 0`): `oid = chat_id`
  - basic group (`-1000000000000 < chat_id < 0`): `oid = -chat_id`
  - supergroup/channel (`chat_id <= -1000000000000`): `oid = -chat_id - 1000000000000`
  - `id = message_id`
  Range-check both to `0 .. 4294967295` first; out of range = log an error and do NOT
  send, never silently truncate to low 32 bits.
- `ImageRequest.image_id = "<chat_id>:<message_id>:<file_unique_id>"` (TDLib dialog
  `chat_id` here, string, no conversion), `image_data` = thumbnail bytes,
  `owner_id = chat_id` (int64, no conversion).
- Process/thread model (v5 §3): ONE process, ONE binary `tg-client`, ONE TDLib client.
  Threads: Main (1), TDLib Receive (1, only caller of `ClientManager::receive`),
  Worker Pool (4), ProcessImages CQ (1), SearchImages CQ (1), Outbox Retry (1).
  Two streams each own their own `CompletionQueue` and CQ thread. Only the CQ thread
  calls `Write`/`Read`/`Finish` on its stream; workers only `Enqueue`.
- TDLib via native C++ `td::ClientManager` only. Do NOT use the deprecated `td::Client`,
  do NOT use the JSON interface (`Td::TdJson`). Link `Td::TdStatic` only.
- In-process handoff only: Receive thread -> Dispatcher -> Worker -> FileDownloader
  (`downloadFile` + `updateFile` future) -> read `local.path` bytes -> build protobuf
  -> Enqueue to async streamer -> CQ thread writes. SQLite Outbox is retry-only,
  keyed by `image_id`; server dedupes on `image_id`. `SearchReq` is NOT replayed
  from Outbox.

## What to build

In `~/workspace/tg-client/`, scaffold the full project per v5 §3.7 (adjust only where
the code forces a better layout, and note any deviation in README):

- `CMakeLists.txt`, `CMakePresets.json` (Debug/Release), `vcpkg.json` OR apt-based
  dependency handling (prefer apt prebuilt packages on Ubuntu 24.04 where available:
  `libgrpc++-dev`, `protobuf-compiler`, `protobuf-compiler-grpc`, `libspdlog-dev`,
  `libsqlite3-dev`, `libgtest-dev`, `gperf`, `php-cli`, `ccache` — run `apt-get update`
  first if needed; you are root, no sudo needed). Do NOT build gRPC/protobuf from
  source — that is far too slow here. If an apt package is genuinely unavailable,
  say so in README and use the next best prebuilt route.
- `third_party/td/` — TDLib as a git submodule / shallow clone of
  `https://github.com/tdlib/td` (shallow, `--depth 1`). Build only `Td::TdStatic`.
- `proto/image_stream.proto` (already present, do not edit) + generated code via
  `protoc --cpp_out` + `--grpc_out --plugin=protoc-gen-grpc=grpc_cpp_plugin`.
- `src/main.cpp`, `src/app/` (Application, Config, AppContext),
  `src/telegram/` (TelegramClient, AuthManager, UpdateDispatcher, UpdateHandler,
  FileDownloader), `src/features/bot_command/` (BotCommandHandler, CommandParser),
  `src/features/auto_search/` (AutoSearchHandler, disabled by default),
  `src/features/media_thumbnail/` (MediaThumbnailHandler, MediaExtractor),
  `src/transport/` (GrpcTransport, AsyncStreamer template, ProcessImagesClient,
  SearchImagesClient), `src/infra/` (Outbox on SQLite, Sha256, FileStore),
  `src/common/` (Logger on spdlog, Idempotency / bare-ID conversion helpers).
- `config/config.example.yaml` (or equivalent config file — if you use a YAML parser
  that is not available, use a simple `key = value` / JSON config instead and say so;
  do NOT add a heavy new dependency just for YAML).
- `Dockerfile` (multi-stage, optional but nice; only if it does not block the main build).
- `README.md`: how to build, configure, run, and test; what is NOT yet done
  (live Telegram account, live gRPC server — see below).

## Build constraints on this host

- Memory is tight (~650 MB available, 7.7 GB total). Build TDLib with `-j1` or
  `-j2` at most, never `-j4`. Use `ccache` if installed. If a TDLib compile step
  OOMs, retry with `-j1` and/or run `php SplitSource.php` in the TDLib tree first
  (install `php-cli` if missing). Do not keep retrying the same failing command
  unchanged — diagnose, then change one thing.
- First TDLib Release build takes 20–60 min. That is expected. Keep working; do not
  stop early and report "build in progress" as done.
- C++17 (TDLib requirement; C++20 is acceptable, no C++23 features).
- Secrets (api_id, api_hash, phone, TDLib encryption key, gRPC token) ONLY via
  environment variables / config file, never hardcoded, never committed to git.
  There is no git repo obligation; if you init one, add a `.gitignore` that excludes
  config with secrets, build dirs, and the TDLib database.

## Tests (required — do not skip)

Write GTest unit tests in `tests/` and make `ctest` pass. At minimum:

1. Bare-ID conversion: dialog ID -> (oid, peer type) for user / basic group /
   supergroup / channel, plus out-of-range rejection (no truncation).
2. `image_id` formation and `memo` = caption only.
3. BotCommandHandler: only the configured bot's command is accepted; other senders
   ignored; unknown commands ignored; reply-to and `chat_id message_id` argument
   parsing.
4. MediaExtractor size selection (photo sizes: pick the ≤320px-max-edge entry;
   video: use thumbnail only, never the original video).
5. Outbox state machine: PENDING -> DONE on reply, retry on UNAVAILABLE, dead-letter
   on INVALID_ARGUMENT / PERMISSION_DENIED, replay after reconnect by `image_id`.
6. AsyncStreamer with an in-process fake gRPC server (implement a fake
   `ImageProcessor` server in the test): open a bidi stream, send an `ImageRequest`,
   receive the `ImageReply` matched by `image_id`; same for `SearchReq`/`SearchRsp`
   matched by `(oid, id)`; stream break -> reconnect -> replay of unacked requests.
7. AutoSearchHandler (disabled by default): allowlisted chat triggers, non-allowlisted
   chat ignored, non-image message ignored, only `SearchImages` is produced.

End-to-end test (test binary or script, your choice): a fake gRPC server + a local
image file through the pipeline — build an `ImageRequest` from a thumbnail file,
send it over the async `ProcessImages` stream, receive the reply, and assert the
`image_id` round-trips. A live Telegram account and the user's real gRPC server are
NOT available (no api_id/api_hash, no server address yet), so do NOT attempt a live
login or a connection to a real server; the fake-server e2e test is the acceptance
test, and README must state clearly that a live run still needs the user's
api_id/api_hash, target server address/TLS/auth, bot user_id, and command names.

## Acceptance criteria (all must hold before you finish)

- `cmake --build` succeeds in Release (or Debug, say which) with no errors.
  Warnings are acceptable only if listed in README with a reason.
- `ctest` passes (all tests green). Paste the test output summary in your final
  message.
- The binary `tg-client` exists and `--help` / `--version` (or equivalent) works.
- No Python in the implementation. No synchronous gRPC stub calls (`stub->Send...`
  sync form must not appear). No hardcoded secrets. No `memo` JSON smuggling.
- README documents: build steps, config, how to run, how to run tests, and the
  open items that need the user (server address/TLS/auth, Telegram account,
  api_id/api_hash, bot user_id, exact command names, auto-search chat list).

## Working style

- Work autonomously. Do not ask questions; resolve build/test problems yourself
  (the user explicitly said: solve all compile/test problems yourself, do not ask).
- If a dependency cannot be installed or a build step genuinely cannot proceed on
  this host, implement the affected part behind a clear seam (e.g. a stubbed
  `TelegramClient` interface with the real TDLib implementation present but
  build-gated), document exactly what is stubbed and why, and still make the rest
  compile and test green. Do not silently drop a required component.
- Commit nothing, push nothing. Work only in `~/workspace/tg-client/`.
- At the end, report: what was built (file list, short), build result, full test
  results, the fake-server e2e result, and the remaining open items for the user.
