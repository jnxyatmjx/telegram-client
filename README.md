# tg-client

C++ Telegram client core (TDLib, native `td::ClientManager`) with a fully
async gRPC media transport (`ImageProcessor::ProcessImages` /
`SearchImages`, bidirectional streaming, `grpc::CompletionQueue`).
Architecture: `~/workspace/tdlib-grpc-architecture.md` (v5).

## Status — chunk 1 (skeleton)

This tree is the **project skeleton only**. It configures and compiles as a
placeholder, but contains **no real implementation yet**.

Done in this chunk:

- Project scaffold: CMakeLists.txt, CMakePresets.json, directory layout per
  architecture v5 §3.7 (`src/app`, `src/telegram`, `src/features/...`,
  `src/transport`, `src/infra`, `src/common`, `tests/`). All `.h/.cpp` files
  exist as guarded placeholders with their contract documented in comments.
- gRPC code generation from `proto/image_stream.proto` (see below).
- CMake configuration verified (including TDLib subdirectory integration,
  without building TDLib).

NOT done yet (later chunks):

- TDLib is cloned into `third_party/td` but **not built**; `Td::TdStatic` is
  wired into the link line but the TDLib build runs in a later chunk
  (20–60 min, `-j1`/`-j2` only on this host).
- No real implementation: config loading, TelegramClient/auth, dispatcher,
  handlers, async streamers, Outbox — all placeholders.
- Tests are placeholder smoke tests proving the harness wires up; the real
  GTest cases (bare-ID conversion, image_id/memo, BotCommandHandler gating,
  MediaExtractor size selection, Outbox state machine, AsyncStreamer vs fake
  server, AutoSearchHandler) land in later chunks.

## Dependencies (apt, Ubuntu 24.04 — no vcpkg, no source builds)

```bash
apt-get update && apt-get install -y \
  build-essential cmake git gperf php-cli ccache pkg-config \
  libssl-dev zlib1g-dev \
  libgrpc++-dev protobuf-compiler protobuf-compiler-grpc \
  libspdlog-dev libsqlite3-dev libgtest-dev
```

Installed versions (recorded 2026-11-08): libgrpc++-dev / protobuf-compiler-grpc
1.51.1, protobuf-compiler 3.21.12, libspdlog-dev 1.12.0, libsqlite3-dev 3.45.1,
libgtest-dev 1.14.0, gperf 3.1, php-cli 8.3, ccache 4.9.1, cmake 3.28.3,
git 2.43.0, libssl-dev 3.0.13, zlib1g-dev 1.3 (all from noble repos; every
requested package name matched, none missing).

Note: Ubuntu's protobuf 3.21 ships no `protobuf-config.cmake`, so the build
uses `find_package(Protobuf)` in module mode (builtin FindProtobuf); gRPC is
still found via `find_package(gRPC CONFIG)`. gRPC/protobuf come from
apt (deviation from v5 §3.7's `vcpkg.json`: apt prebuilt packages were chosen
per the task instructions; source builds are far too slow on this host).

TDLib: shallow clone, not a submodule (this project has no git repo yet):

```bash
git clone --depth 1 https://github.com/tdlib/td third_party/td
```

## Build

The working build directory is `build/` (Release). TDLib is already built
there (2026-11-08) and must NEVER be rebuilt for later code changes — build
only the tg-client and test targets:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target tg-client test_idempotency test_config \
  test_media_extractor test_outbox test_bot_command test_auto_search \
  test_handler_registry test_async_streamer test_e2e_fake_server -j1
```

(Do not run a bare `cmake --build build`: it would also consider the
TDLib targets. Never `rm -rf build`, never rebuild `tdcore`/`tdclient`.)

The `release`/`debug` CMake presets create `build-release`/`build-debug`
instead; they are equivalent but were not the directories used for the
chunk 2/3 builds. The `release-no-td` preset builds the skeleton without
TDLib at all.

## gRPC code generation

Integrated into the CMake build: `protoc --cpp_out` + `--grpc_out
--plugin=protoc-gen-grpc=grpc_cpp_plugin` run as a custom command into
`<build-dir>/generated/` (kept out of the source tree). The proto declares no
package, so generated symbols are global: `ImageProcessor`, `ImageRequest`,
`ImageReply`, `SearchReq`, `SearchRsp`.

Manual equivalent (toolchain verification):

```bash
mkdir -p generated && \
protoc -I proto --cpp_out=generated --grpc_out=generated \
  --plugin=protoc-gen-grpc=$(which grpc_cpp_plugin) proto/image_stream.proto
```

Deviation from v5 §1.1: generated code lives under the build dir rather than
an in-tree `generated/` (standard CMake out-of-source practice; the manual
command above still writes an in-tree `generated/` if you run it).

## Configure

Copy `config/config.example.yaml` to `config/config.yaml` and fill in real
values. Secrets (`TG_API_HASH`, `TG_TDLIB_ENCRYPTION_KEY`, `GRPC_TOKEN`,
phone) come from environment variables — never hardcoded, never committed
(`.gitignore` excludes `config/config.yaml`).

## Run

```bash
./build/tg-client --help
./build/tg-client --version   # tg-client 0.1.0
./build/tg-client --config config/config.yaml
```

A live run needs a real config (see Open items below): Telegram
api_id/api_hash, the target gRPC server, and the trigger bot's user_id.

## Test

```bash
ctest --test-dir build/tests --output-on-failure
```

9 test binaries, all green (2026-11-08, 2.59 s total):

| test | covers |
|---|---|
| test_idempotency | image_id formation, memo = caption only |
| test_config | config parsing, env expansion |
| test_media_extractor | photo tier selection (≤320px), video thumbnail-only |
| test_outbox | PENDING -> DONE / retry / dead-letter / replay |
| test_bot_command | only the configured bot's command accepted, arg parsing |
| test_auto_search | allowlisted chat triggers Search only (disabled by default) |
| test_handler_registry | dispatcher registration / dispatch |
| test_async_streamer | CQ state machine vs in-process fake server: round-trip by image_id, bounded-queue backpressure, reconnect after server restart |
| test_e2e_fake_server | **chunk 3 acceptance test**: a local thumbnail file through the real pipeline (MediaThumbnailHandler/SearchHandler -> FileDownloader -> GrpcTransport -> AsyncStreamers -> fake ImageProcessor server); ProcessImages reply round-trips by image_id and the Outbox record goes PENDING -> DONE; SearchImages reply round-trips by (oid, id) and resolves through the correlation table |

Note: the socket-based tests (test_async_streamer, test_e2e_fake_server)
use a Unix domain socket, not TCP loopback. This cloud VM's runtime
intercepts direct TCP connections (`other_tcp` denied), so a 127.0.0.1
loopback client receives the platform's block message instead of the
server's HTTP/2 SETTINGS frame. The streamer logic is transport-agnostic;
production still uses the TCP target from config. To allow TCP loopback
tests, switch `other_tcp` from Deny to Ask in Muse settings ->
Permissions -> Direct network protocols.

## Open items (need the user)

Per IMPLEMENTATION_TASK.md / v5 §7 — a live run is NOT possible yet:

1. gRPC server: address (host:port), TLS, auth (token / mTLS / none).
2. Telegram account: user account api_id/api_hash, phone; which chats.
3. Bot user_id (`command_bot_user_id`) and the real command names
   (`/process`, `/search` are placeholders).
4. Auto-search chat allowlist (phase 2; disabled by default).

## Layout (v5 §3.7)

```
CMakeLists.txt  CMakePresets.json  Dockerfile  README.md
proto/image_stream.proto            (verbatim, do not edit)
src/main.cpp
src/app/            Application, Config, AppContext
src/telegram/       TelegramClient, AuthManager, UpdateDispatcher,
                    UpdateHandler, FileDownloader
src/features/       bot_command/ (BotCommandHandler, CommandParser)
                    auto_search/ (AutoSearchHandler, disabled by default)
                    media_thumbnail/ (MediaThumbnailHandler, MediaExtractor, SearchHandler)
src/transport/      GrpcTransport, AsyncStreamer (template),
                    ProcessImagesClient, SearchImagesClient
src/infra/          Outbox (SQLite), Sha256, FileStore
src/common/         Logger (spdlog), Idempotency (bare-ID conversion)
tests/              GTest unit tests
config/             config.example.yaml
third_party/td/     TDLib shallow clone (commit 4c6377a; built 2026-11-08,
                    libtdcore.a/libtdclient.a — never rebuild, see Build)
```
