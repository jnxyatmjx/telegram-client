# tg-client

C++ Telegram client core (TDLib, native `td::ClientManager`) with a fully
async gRPC media transport (`ImageProcessor::ProcessImages` /
`SearchImages`, bidirectional streaming, `grpc::CompletionQueue`).
Architecture: `~/workspace/tdlib-grpc-architecture.md` (v5).

## Status — chunks 1–3 complete (2026-11-08)

构建任务已全部完成：TDLib 已编好（2026-11-08），tg-client 真实实现已完成，
`ctest` 9/9 通过（见 Test 一节），二进制 `tg-client 0.1.0` 可正常运行
`--help` / `--version`。唯一还没做的是真机联调，需要用户提供服务器地址、
api_id/api_hash、触发 bot 的 user_id 和命令名（见 Open items）。

- TDLib 已编译：`build/third_party/td/libtdcore.a`（132MB）、`libtdclient.a`，
  此后绝不再重编（见 Build 一节铁律）。
- 真实实现：44 个源文件，约 4430 行（登录、接收、派发、下载、两个触发器、
  全异步 gRPC 流、SQLite Outbox、bare-ID 换算）。
- 测试：9 个 GTest 二进制，含假服务器端到端测试，全部通过。

## Dependencies (apt, Ubuntu 24.04 / Debian 11 — no vcpkg, no source builds)

### Build dependencies (compiling from source)

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

TDLib: shallow clone, not a submodule:

```bash
git clone --depth 1 https://github.com/tdlib/td third_party/td
```

#### Debian 11 (bullseye) — build

Yes, it builds and runs on Debian 11. Two things needed changing
(done in `CMakeLists.txt`): the project required CMake 3.20 but
bullseye ships 3.18.4, so the minimum is now 3.18; and bullseye's
grpc 1.30.2 packages ship headers, libraries and `grpc++.pc` but no
`gRPCConfig.cmake`, so gRPC is now found via pkg-config when the CMake
config package is absent (`TG_GRPC_PKGCONFIG=ON` forces that path, e.g.
to test it on a machine that does have the config package). Everything
else the project uses already exists in bullseye's versions: GCC 10.2
(C++17), protobuf 3.12.4, spdlog 1.8.1, SQLite 3.34.1, GTest 1.10.0,
OpenSSL 1.1.1, zlib 1.2.11, gperf 3.1, php-cli 7.4 (TDLib's code
generator uses it), and TDLib itself only needs CMake ≥ 3.10, C++17,
OpenSSL, zlib, gperf and php — all met.

```bash
sudo apt-get update && sudo apt-get install -y \
  build-essential cmake git gperf php-cli ccache pkg-config \
  libssl-dev zlib1g-dev \
  libgrpc++-dev protobuf-compiler protobuf-compiler-grpc \
  libspdlog-dev libsqlite3-dev libgtest-dev libgmock-dev
```

Same package names as Ubuntu 24.04, plus `libgmock-dev`: Debian 11
splits Google Mock into its own package — `libgtest-dev` ships only
gtest, but its `GTestTargets.cmake` still references `libgmock.a`, so
`find_package(GTest)` fails at configure time without it. (Ubuntu
24.04's `libgtest-dev` includes gmock, so it isn't needed there.) The
installed versions are just older (gRPC 1.30.2, protobuf 3.12.4, spdlog 1.8.1, SQLite 3.34.1,
GTest 1.10.0, OpenSSL 1.1.1, CMake 3.18.4, GCC 10.2.1). The
`pkg-config` package is required — it is how gRPC is found on
bullseye.

**The stock `sources.list` no longer works.** Debian 11 LTS ended
2026-08-31, and `security.debian.org` has removed `bullseye-security`
(every URL there now 404s), so `apt` cannot fetch the security updates
(`libssl1.1` deb11u8, php7.4 deb11u12, …) and the install fails with
unmet dependencies. Point apt at the archive instead, and disable the
expired-Release check (the archived Release files are old):

```bash
cat > /etc/apt/sources.list << 'EOF'
deb http://archive.debian.org/debian/ bullseye main
deb http://archive.debian.org/debian/ bullseye-updates main
deb http://archive.debian.org/debian-security/ bullseye-security main
EOF
echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/10-no-check-valid-until
apt-get update
apt --fix-broken install
```

Then run the install command above. (Verified 2026-11-09: all three
archive suites exist — bullseye 11.11, bullseye-updates, and
bullseye-security with a 2026-09-12 snapshot.)

Two caveats:

- **Presets don't work with stock CMake 3.18.** `CMakePresets.json`
  needs CMake ≥ 3.20, so `cmake --preset release` fails on bullseye.
  Use the direct commands instead (`cmake -S . -B build
  -DCMAKE_BUILD_TYPE=Release`, then `cmake --build build --target
  tg-client -j1`, as in [Build](#build) below). To use the presets,
  install a newer CMake, e.g. from Kitware's apt repository
  (https://apt.kitware.com).
- **The gRPC API used here predates 1.30**, so the older library is
  fine: `CreateChannel`, `SslCredentials`, `ClientAsyncReaderWriter`,
  `CompletionQueue::AsyncNext`, `ClientContext::TryCancel`,
  `Server::Shutdown` and the generated `PrepareAsync*` stubs all exist
  in 1.30.2. The full TDLib build on Debian 11 itself has not been run
  here (it takes 20–60 min even on a fast host); the compatibility
  claim rests on the version/API check above, plus the pkg-config
  fallback being verified end-to-end (configure + build + 13/13
  tests) on this host.

### Runtime dependencies (running the built binary on another machine)

Only needed on a machine that runs the prebuilt `tg-client` without
compiling it — no compiler, no CMake, no `-dev` packages. Ubuntu 24.04:

```bash
sudo apt-get update && sudo apt-get install -y \
  libgrpc++1.51t64 libprotobuf32t64 libspdlog1.12 \
  libsqlite3-0 libssl3t64 zlib1g
```

What each package provides, and what comes along automatically:

- `libgrpc++1.51t64` — `libgrpc++.so.1.51` and `libgrpc.so.29`. Also
  pulls in `libgrpc29t64`, `libprotobuf32t64`, and the whole Abseil set
  (`libabsl20220623t64`: all 41 `libabsl_*.so.20220623` files, including
  `libabsl_synchronization`), plus `libcares2` and `libre2-10`.
- `libprotobuf32t64` — `libprotobuf.so.32` (also a dependency of
  `libgrpc++1.51t64`; listed here so the intent is explicit).
- `libspdlog1.12` — `libspdlog.so.1.12`; also pulls in `libfmt9`
  (`libfmt.so.9`).
- `libsqlite3-0` — `libsqlite3.so.0`.
- `libssl3t64` — `libssl.so.3`.
- `zlib1g` — `libz.so.1`.
- `libc6`, `libgcc-s1`, `libstdc++6` — the C/C++ runtime every Ubuntu
  install already has; nothing to install.

Watch out for the `t64` suffix: Ubuntu 24.04 renamed these libraries
in the 64-bit `time_t` transition, so the plain names (`libgrpc++1`,
`libprotobuf32`, `libssl3`) **do not exist** on 24.04 — `apt` answers
`Unable to locate package`. Use the `t64` names above.

Verify after installing: `ldd ./tg-client` must show no `not found`
lines. If a line still says `not found`, the package providing that
`.so` is missing — look it up with `apt-file search <name>` or
`dpkg -S <name>` on a machine that has it.

#### Debian 13 (trixie) — runtime

Same command shape, different spdlog generation (trixie ships spdlog
1.15 and Abseil 20240722):

```bash
sudo apt-get update && sudo apt-get install -y \
  libgrpc++1.51t64 libprotobuf32t64 libspdlog1.15 \
  libsqlite3-0 libssl3t64 zlib1g
```

`libgrpc++1.51t64` again pulls the Abseil set automatically
(`libabsl20240722` on trixie); `libspdlog1.15` pulls `libfmt10`.

> **Warning: this does NOT make the Ubuntu 24.04-built binary run on
> Debian 13.** The loader needs exact SONAMEs, and two libraries
> changed SONAME between the two distros:
>
> | library | Ubuntu 24.04 binary needs | Debian 13 provides |
> |---|---|---|
> | Abseil | `libabsl_*.so.20220623` | `libabsl_*.so.20240722` |
> | spdlog | `libspdlog.so.1.12` (+ `libfmt.so.9`) | `libspdlog.so.1.15` (+ `libfmt.so.10`) |
>
> Everything else matches (gRPC 1.51.1, protobuf 3.21.12, OpenSSL 3,
> SQLite 3, zlib), but those two mismatches are enough: on Debian 13
> `ldd` will report the absl/spdlog lines as `not found` and the
> binary will not start. To run on Debian 13, either:
>
> - **Rebuild on Debian 13** — the build-dependency command above
>   works unchanged there (same `-dev` package names), and the
>   resulting binary links against trixie's libraries; or
> - **Bundle the Ubuntu `.so` files** next to the binary with
>   `rpath=$ORIGIN` (the 41 absl libs, `libspdlog.so.1.12`,
>   `libfmt.so.9`); or
> - **Run the Ubuntu 24.04 Docker image** (see `Dockerfile`).

#### Debian 11 (bullseye) — runtime

```bash
sudo apt-get update && sudo apt-get install -y \
  libgrpc++1 libspdlog1 libsqlite3-0 libssl1.1 zlib1g
```

What each package provides, and what comes along automatically:

- `libgrpc++1` (1.30.2) — `libgrpc++.so.1.30`. Also pulls in
  `libgrpc10` (`libgrpc.so.10`), `libabsl20200923` (the Abseil set,
  `libabsl_*.so.20200923`) and `libprotobuf23`
  (`libprotobuf.so.23`).
- `libspdlog1` (1.8.1) — `libspdlog.so.1.8`; also pulls in `libfmt7`
  (`libfmt.so.7`).
- `libsqlite3-0` — `libsqlite3.so.0`.
- `libssl1.1` — `libssl.so.1.1` (OpenSSL 1.1.1, not 3.x).
- `zlib1g` — `libz.so.1`.
- `libc6`, `libgcc-s1`, `libstdc++6` — base system libraries; nothing
  to install.

> **Warning: a binary built on Debian 11 will NOT run on Ubuntu 24.04
> or Debian 13, and vice versa.** Every shared library above has a
> different SONAME there (gRPC 1.30 vs 1.51, protobuf 23 vs 32,
> spdlog 1.8 vs 1.12/1.15, OpenSSL 1.1 vs 3, Abseil 20200923 vs
> 20220623/20240722). Rebuild on the target distro — the
> build-dependency command above is the same package names everywhere,
> only the versions differ.

## Build

构建目录是 `build/`，类型为 Release。日常开发只有两步：配置一次，之后改代码
只需重新编译。**铁律：TDLib 已经编好（2026-11-08），以后任何代码改动都只编
tg-client 和测试目标，绝不重编 TDLib，也不删除 `build/`。** TDLib 编一次要
20–60 分钟，删掉就得重编。

### 第 1 步：配置（configure，只需做一次）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

各参数含义：

- `cmake`：CMake 构建工具，负责生成 Makefile。
- `-S .`：源码目录。`.` 表示当前目录（项目根目录，即顶层 CMakeLists.txt
  所在处）。换成别的路径就是换项目位置。
- `-B build`：构建输出目录。所有中间文件和二进制都生成在这里，源码目录保持
  干净。这叫 out-of-source 构建。以后所有 `cmake --build` 命令里的 `build`
  指的都是这个目录。
- `-D<变量>=<值>`：设置一个 CMake 缓存变量（cache variable），配置结果会记
  在 `build/CMakeCache.txt` 里，下次不用重复写。
- `CMAKE_BUILD_TYPE=Release`：构建类型。`Release` 表示优化编译、不带调试
  符号、速度最快，是我们平时用的；`Debug` 表示带调试符号、不优化、方便
  gdb 调试但跑得慢；`RelWithDebInfo` 是优化 + 带调试信息；`MinSizeRel` 是
  体积最小。这个项目如果不写它，CMakeLists.txt 会默认设为 Release。

什么时候需要重新跑配置：改了 CMakeLists.txt、CMakePresets.json 或下面
“CMake 选项”里的开关时；或者刚 clone 下来、第一次建 `build/` 时。只改
`.cpp` / `.h` 代码不需要重新配置，直接第 2 步。

### 第 2 步：编译（build，改代码后每次都跑）

```bash
cmake --build build --target tg-client test_idempotency test_config \
  test_media_extractor test_outbox test_bot_command test_auto_search \
  test_handler_registry test_async_streamer test_e2e_fake_server -j1
```

各参数含义：

- `cmake --build build`：用第 1 步生成的 `build/` 目录里的 Makefile 开始编译。
- `--target <名字>`：只编指定的目标（target），可以写多个，用空格分开。
  `tg-client` 是主程序；`test_*` 是 9 个测试程序。不写 `--target` 就是编
  全部目标——**不要这么做**，全部目标里包含 TDLib 自己的 `tdcore` /
  `tdclient` / `run_all_tests`，会触发重编 TDLib。
- `-j1`：并行编译任务数。`1` 表示一次只编一个文件。TDLib 编译非常吃内存，
  这台云主机只有 7.7GB 内存，`-j` 给大了会内存不足被杀掉，所以编 TDLib
  只能 `-j1` / `-j2`。只编我们自己的 tg-client 代码时，`-j2` / `-j4` 也
  可以，但为保险起见平时都用 `-j1`。
- 成功标志：最后出现 `[100%] Built target tg-client`，且没有任何 `error:`。
  二进制在 `build/tg-client`，跑 `./build/tg-client --version` 应输出
  `tg-client 0.1.0`。

只编某一个测试时，不用写全部目标，例如：

```bash
cmake --build build --target test_outbox -j1
```

### 预设（presets，另一种等价写法）

`CMakePresets.json` 里定义了几套预设，`cmake --preset <名字>` 等价于上面
第 1 步的命令，只是构建目录不同：

- `release`：`CMAKE_BUILD_TYPE=Release`，目录是 `build-release`。
- `debug`：`CMAKE_BUILD_TYPE=Debug`，目录是 `build-debug`。
- `release-no-td`：在 release 基础上加 `TG_ENABLE_TDLIB=OFF`，不含 TDLib，
  只编骨架，用于快速检查。

用法示例：

```bash
cmake --preset release
cmake --build build-release --target tg-client -j1
```

注意：chunk 2/3 实际用的目录是 `build/`（第 1 步的命令），不是
`build-release`。两套写法等价，但不要混用——每个构建目录有自己的缓存，
换目录等于重新配置。

### CMake 选项（`-D` 开关，配置时用）

- `TG_ENABLE_TDLIB`（默认 `ON`）：是否把 `third_party/td`（TDLib）加进构建
  并链接 `Td::TdStatic`。设为 `OFF` 时编出的是不含 TDLib 的骨架
  （`release-no-td` 预设就是这么做的）。正常使用不要动它。
- `TG_BUILD_TESTS`（默认 `ON`）：是否编译 `tests/` 下的测试程序。设为
  `OFF` 可跳过测试，只编主程序。
- `TG_ENABLE_CCACHE`（默认 `ON`）：如果装了 ccache，就用它缓存编译结果，
  重复编译会快很多。没装也不影响。
- `CMAKE_EXPORT_COMPILE_COMMANDS`（预设里为 `ON`）：生成
  `build/compile_commands.json`，给编辑器 / clangd 做代码跳转用，不影响编译。

### TDLib 首次编译（只有全新 clone 时需要，以后绝不重复）

```bash
cmake --build build --target tdclient -j1
```

- `--target tdclient`：只编 TDLib 的客户端库这一个目标（它会连带编完
  `tdcore` 等依赖）。
- 耗时 20–60 分钟，期间不要中断；日志建议重定向到文件慢慢看，例如
  `> tdlib-build.log 2>&1`。编完后 `build/third_party/td/` 下会出现
  `libtdcore.a`（约 132MB）和 `libtdclient.a`。从此只编上面第 2 步的目标。

### 虚拟机被换掉之后

云主机更换后，apt 装的依赖包可能被清空（只有 home 目录保留），而
`build/` 下已编好的 `.a` / `.o` 文件还在。恢复顺序：

1. 先重装依赖（见 Dependencies 一节，完整包名清单在
   `apt-reinstall.log` 里）。
2. 再跑第 1 步重新配置，第 2 步只编 tg-client 和测试目标。
3. **不要**因为配置报错就删 `build/` 或重编 TDLib，先检查是不是依赖没装全。

### 常见错误

- 不带 `--target` 直接 `cmake --build build`：会连 TDLib 一起编，白白花
  20–60 分钟。
- `rm -rf build`：删掉已编好的 TDLib，下次必须从头编 20–60 分钟。
- 给 TDLib 用 `-j4` 以上：内存不足，编译进程会被系统杀掉。
- 在 `build/` 里跑 `ctest`：顶层还包含 TDLib 自己的 `run_all_tests`，
  它从没编过，会显示 Not Run。测试一律在 `build/tests/` 里跑
  （见 Test 一节）。

## Architecture

Design doc: `~/workspace/tdlib-grpc-architecture.md` (v5, settled 2026-11-07).
This section is the short version for the README; the design doc is
authoritative when the two disagree.

### Design

- **One client core, features on top.** The program is a Telegram client
  first and a media pipeline second. Sending image/video thumbnails over
  gRPC is only the first feature; more features attach the same way.
  Language is C++ only, no Python.
- **Layers.** `app` assembles and owns the lifecycle. `telegram` owns the
  TDLib client, auth state machine, update dispatch and file downloads.
  `features` holds one directory per feature, each an `IUpdateHandler`
  registered in the dispatcher. `transport` owns the gRPC channel, the
  async streamers and the stub clients. `infra` owns persistence
  (SQLite Outbox) and file helpers. `common` owns logging and idempotency.
  The core knows nothing about features; features get shared services
  through `AppContext` (see Modules).
- **In-process handoff, no JSON, no network, no queue.** Receive thread
  gets `updateNewMessage` and dispatches only. A worker thread extracts
  the media, calls `downloadFile`, waits for `updateFile`, reads the
  thumbnail bytes from `local.path`, builds an `ImageRequest` and pushes
  it to the streamer's bounded write queue. The CompletionQueue thread
  writes it to the server. The SQLite Outbox is retry-only, not the
  main path.
- **At-least-once delivery, server dedup.** Every `ImageRequest` carries
  `image_id = str(message_id)` (Telethon-migration chunk 1: the
  message-level id the Python server uses; cross-chat collisions are
  accepted, same as the Python client). On reconnect the Outbox replays
  unacked records by `image_id` and the server deduplicates on that key.
  The old composite `"<chat>:<msg>:<uid>"` form (`MakeImageId`) survives
  only inside the in-memory SearchReq correlation table.
- **Triggers.** Nothing is sent automatically on receiving an image.
  `ProcessImages` and `SearchImages` fire only on a command from a
  specified bot in Telegram (mode (a): the bot itself is the sender,
  verified by `sender_user_id == command_bot_user_id`). Phase 2 adds
  auto-search: a user posting an image in a specified channel/group
  triggers `SearchImages` only, never `ProcessImages`; disabled by
  default.
- **Proto.** `proto/image_stream.proto` is used verbatim (no package,
  both RPCs bidirectional streaming). Since the Telethon-migration
  chunk 1 the wire protocol matches the Python server byte-for-byte:
  `memo` is `base64(nonce[12] ‖ AES-256-GCM ciphertext ‖ tag[16])` of the
  *cleaned* caption (key = SHA256(`KEYME`), the same scheme as the
  Python `aes_gcm_encrypt`; the caption is cleaned with the Python regex
  first — URLs, t.me links, whitespace and `[]*_ -` removed). Plaintext
  is never sent: an unconfigured `KEYME` or a failed encryption means
  the message is not sent at all. `owner_id` is the bare MTProto chat id
  (same conversion as `SearchReq.oid`: channel `-chat_id - 1e12`,
  basic group `-chat_id`, user `chat_id`), range-checked to
  `0..4294967295` — out of range is a loud error, never truncated.
  `SearchReq` uses the same uint32 bare MTProto IDs for `oid`/`id` and
  is not replayed from the Outbox because its correlation table
  `(oid, id) -> message` lives only in memory. `ImageReply.result` is
  the server contract `{"cid": <bare id>, "mid": <msg id>}`; it is
  parsed into the in-memory `ChatStateStore` (per-chat watermark,
  on-demand chat-URL resolution) and a malformed result is logged and
  dropped, never crashing the pipeline.
- **Failure handling.** `downloadFile` timeout/failure retries 3 times
  with backoff, then dead-letter. A full write queue blocks the worker
  with a timeout and leaves the record PENDING for the retry thread.
  A broken gRPC stream reconnects with backoff (1s → 2s → 4s … cap 30s)
  and replays unacked Outbox records. `INVALID_ARGUMENT` /
  `PERMISSION_DENIED` from the server dead-letters immediately without
  retry.

### Process and thread model

- **One process, one binary `tg-client`, one TDLib client.** No separate
  TDLib process, no IPC, no shared memory, no local socket, no broker
  between TDLib and gRPC. They share C++ objects and thread-safe queues
  in the same process.

```
tg-client (1 process)
  Main thread (1)
  TDLib Receive thread (1)
  Worker Pool (4)
  ProcessImages CQ threads (grpc.connections, 1 by default)
  SearchImages CQ threads (grpc.connections, 1 by default)
  Outbox Retry thread (1)
  TDLib internal threads (N, owned by TDLib)
```

| Thread | Count | Does | Must never do |
|---|---|---|---|
| Main | 1 | Parse config, assemble `Application`, start threads, wait for SIGINT/SIGTERM, stop threads in order, join | Call `ClientManager::receive()`, call gRPC `Write` |
| TDLib Receive | 1 | The only caller of `ClientManager::receive(10.0)`; routes responses to their callbacks by `request_id`, hands updates (`request_id == 0`) to `UpdateDispatcher` | Download, read disk, call gRPC, block for more than a few ms |
| Worker Pool | 4 | Handler stages (short, never blocking): extract tier, start download, finish stage (read file, hash, build `ImageRequest`, write Outbox, enqueue to streamer) | Call `receive()`; call gRPC `Write`/`Read` directly (only `Enqueue`); block on a TDLib response or a download |
| ProcessImages CQ | grpc.connections | One per lane: owns that lane's `ProcessImages` CompletionQueue and `ClientAsyncReaderWriter`; loops `cq.Next()`, handles Write done, Read replies, Finish, reconnect | Call TDLib `send`/`receive` (use thread-safe `TelegramClient::Send`) |
| SearchImages CQ | grpc.connections | Same as above, for the `SearchImages` stream, its own CQ and stub per lane | Same; the two streams are isolated, one breaking does not affect the other |
| Outbox Retry | 1 | Every 10 s, re-enqueues PENDING/FAILED Outbox records whose retry time has come | Call gRPC `Write` directly (only `Enqueue`) |
| TDLib internal | N | Actors, network, crypto, local database, created by TDLib | Not created, joined or driven by us |

- **Async message processing — no message blocks another (2026-11-09).**
  Receiving and bot commands are both fully async: a message's slow
  getMessage or download must never occupy a thread another message
  needs. The pipeline is a chain of short stages, each posted to the
  worker pool, with callbacks in between — no stage ever waits:
  `BotCommandHandler` sends `getMessage` and returns; its reply callback
  (Receive thread) only posts the pipeline kick-off to the pool.
  `MediaThumbnailHandler` / `SearchHandler` extract the media tier and
  call `FileDownloader::DownloadAsync`, then return; the download
  callback (Receive thread) only posts the finish stage, which reads the
  file, builds the request, writes the Outbox and enqueues (one
  non-blocking attempt — a full queue leaves the record PENDING for the
  retry thread). `FileDownloader` has no timer: a download that never
  completes simply never resolves, and no thread is blocked waiting for
  it. There is no blocking wait API at all — `ITdSender::SendWithFuture`
  was removed, and `FileDownloader::Download` (future) exists only for
  tests. Regression test:
  `BotCommandTest.SecondCommandCompletesWhileFirstDownloadPending`
  (message A's download takes 2 s, B's is instant; B's request reaches
  the streamer queue while A is still downloading).
- **Async gRPC state machine (one per stream).**
  `CREATE` (PrepareAsync + StartCall) → `CONNECTING`/`RUNNING`
  (issue a Read immediately; Write when the queue is non-empty and no
  Write is in flight) → `WRITE_DONE` (next Write) / `READ_DONE`
  (handle the reply by `image_id`/`(oid, id)`, issue the next Read) →
  `FINISH` (server closed the stream, error, or shutdown) →
  `RECONNECT` (backoff 1s → 2s → 4s … cap 30s, then CREATE again).
  Each stream gets a fresh `ClientContext` with auth metadata attached
  once per stream, never per message. There is no per-message deadline;
  the app-level ack timeout (30 s per `image_id`) decides replays.
- **Synchronization.** `receive()` needs no lock because only one thread
  calls it. `send()` is thread-safe in TDLib; `request_id` comes from an
  atomic counter starting at 1. The `FileDownloader` map is guarded by
  one mutex. Each streamer write queue is one mutex + condition
  variable; only the CQ thread calls `Write`/`Read`/`Finish`, and only
  one Write is in flight per stream. The Outbox uses one SQLite
  connection in WAL mode behind one mutex. The search correlation table
  is guarded by one mutex.
- **Teardown contract.** `Finish()` must be the last operation on a
  stream, and the stream object may only be destroyed once its Finish
  tag has been delivered. `AsyncStreamer::Stop()` therefore lets the CQ
  thread drain the stream (cancel → WritesDone → Finish → OnFinish)
  and joins it before shutting the CQ down. Shutting the CQ down first
  strands the stream with an outstanding Read and the server hangs in
  `Read()` forever.
- **Shutdown order.** (1) Stop dispatching new updates. (2) Drain the
  worker pool with a timeout; unfinished work stays in the Outbox.
  (3) Both streamers: `WritesDone()`, wait for `Finish()`, shut the CQ,
  join the CQ thread. (4) `TelegramClient::Stop()`: `close()`, wait for
  the Receive thread. (5) Flush the Outbox, join the retry thread, exit.

### Parallel gRPC connections (lanes)

`grpc.connections` (default 1, range 1–16) controls how many parallel
connections the client opens to the gRPC server. This section explains
the whole mechanism: what a lane is, why one connection is not enough,
how traffic is spread, and exactly what breaks — and what recovers —
when a connection dies.

#### What a lane is

One lane = **one Channel + one ProcessImages stream + one SearchImages
stream**. A Channel is one TCP/HTTP2 connection to the server target, so
`grpc.connections: 3` means 3 independent TCP connections, 3
ProcessImages streams and 3 SearchImages streams:

```
grpc.connections = 3

lane 0:  Channel ── ProcessImages stream ──┐
              └─ SearchImages stream ──┤
lane 1:  Channel ── ProcessImages stream ──┼── gRPC server (one target)
              └─ SearchImages stream ──┤
lane 2:  Channel ── ProcessImages stream ──┘
              └─ SearchImages stream ──┘
```

Everything is built per lane: `GrpcTransport` creates all N channels
eagerly at startup (same credentials for every lane — TLS or insecure,
plus the optional private CA from `grpc.ca_cert`). Each channel is built
with `CreateCustomChannel` and `GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL = 1`:
plain `CreateChannel()` is not enough, because gRPC C-core shares
subchannels (TCP connections) across channels with the same target and
channel args through the global subchannel pool, so N plain channels
collapse onto a single TCP connection — the "configured 3 lanes, only 1
connection" symptom. The per-lane local subchannel pool is what makes
each lane a genuinely separate TCP connection. Each lane gets its
own pair of stubs (`ProcessImagesClient` / `SearchImagesClient`,
constructed with that lane's channel) and its own pair of
`AsyncStreamer`s, each with its own CompletionQueue and its own CQ
thread. `StreamerPool` (one per RPC) owns the N streamers for that RPC.
Application threads are therefore **7 + 2 × connections**: Main (1) +
TDLib Receive (1) + Worker Pool (4) + Outbox Retry (1) + 2 CQ threads
per lane. Each lane also holds 2 stubs and 2 long-lived streams.

#### Why one connection is not enough

A single gRPC Channel is **one** HTTP/2 connection. It multiplexes many
streams, but they all share one TCP connection: one connection's
bandwidth, one TCP-level failure domain, and one connection's congestion
state. Two streams on one channel is normal gRPC, but it is not
connection-level parallelism — and a dead channel takes all of its
streams down with it. N lanes means N independent TCP connections: the
traffic actually spreads across connections, and one dead connection
only ever costs 1/N of the capacity.

#### How requests are spread

`StreamerPool::Enqueue` deals requests **round-robin** across lanes
with an atomic counter — one lane per call, no queue-depth or latency
checks, no least-loaded routing. This is deliberate: the two RPCs have
very different traffic shapes, and round-robin is the simplest scheme
that spreads both evenly.

- **ProcessImages** may run for a long time or sit idle for a long
  time. Its streams are long-lived bidi streams that just stay open
  while idle; there is no per-message setup cost, so idleness costs
  nothing.
- **SearchImages** may spike in short bursts (many searches at once).
  A burst spreads across lanes, so no single stream or queue absorbs
  the whole spike.

Replies do not need to come back in lane order, or in send order at
all: ProcessImages replies are matched by `image_id` (Outbox
`MarkDone`), SearchImages replies by `(oid, id)` (correlation table
lookup). Nothing in the pipeline assumes ordering, so dealing requests
across lanes is safe.

Each streamer has its own **bounded write queue** (`grpc.queue_capacity`,
default 1000). `Enqueue` waits up to `grpc.enqueue_timeout_ms`
(default 5 s) for a free slot in the chosen lane's queue, then fails
and the caller treats it as "try again later" — that is the
backpressure mechanism (§6). A full queue on one lane does not block
the other lanes.

A SearchImages burst cannot block ProcessImages: the two RPCs use
separate streams, separate queues, and separate CQ threads on every
lane. The only resource they share inside a lane is that lane's TCP
connection bandwidth.

#### What happens when a connection dies

There are two failure levels, and they behave differently:

**Channel failure (one lane's connection dies).** Both streams on that
lane break together — they share the TCP connection, so a dead channel
takes both down at once. The other lanes are completely unaffected and
keep serving both RPCs. Each broken streamer then reconnects
**independently**, with its own backoff timer (1 s → 2 s → 4 s … cap
30 s) and its own attempt counter; one lane's reconnect does not wait
for or affect the other's.

**Stream failure (one RPC breaks, the connection is fine).** Only that
one stream on that one lane is affected. The other RPC on the same
lane keeps running — the two streams are isolated from each other.

**Queued requests on a dead lane.** When a stream fails,
`AsyncStreamer::FailStream` clears that streamer's queue. What that
means depends on the RPC:

- **ProcessImages: not lost.** Every request was recorded in the
  Outbox before it was enqueued. On reconnect, the streamer's
  `OnReconnected` handler fires `SweepOutbox`, which re-enqueues the
  unacknowledged records by `image_id`; the server deduplicates on
  that key (at-least-once delivery). So a dead lane delays ProcessImages
  traffic, it does not drop it.
- **SearchImages: lost.** Search requests are never written to the
  Outbox (by design — search is idempotent/read-only, and the
  `(oid, id)` correlation lives only in memory). A search that was
  queued or in flight on a dying lane simply never returns; re-send
  the command to run it again.

**New requests during an outage.** Round-robin keeps dealing 1/N of
new requests to the dead lane. They wait in that lane's bounded queue
until its stream is back up and the queue is pumped — they are delayed,
not rejected. (If the stream fails again while they are queued, the
rule above applies: ProcessImages is Outbox-covered, SearchImages is
lost.)

**Reconnect side effects.** `OnReconnected` fires once per lane, and
each firing sweeps the Outbox. The sweep is idempotent (it only picks
up records that are still unacknowledged), so several lanes
reconnecting at once is harmless — the same record is never sent twice
in a harmful way, and the server dedupes by `image_id` anyway.
Permanent gRPC errors (`INVALID_ARGUMENT`, `PERMISSION_DENIED`,
`UNIMPLEMENTED`, `NOT_FOUND`) are different from a dead connection:
they dead-letter all pending Outbox records instead of retrying them,
because retrying cannot help.

#### What this design deliberately does NOT do

- **No per-RPC channel pools.** One lane carries both RPCs; there is
  no separate `grpc.process_connections` / `grpc.search_connections`.
  This was a deliberate choice (kept after review on 2026-11-08): the
  two RPCs share a lane's connection, and lane-level independence
  (other lanes keep working) is considered enough isolation. Splitting
  them would double the connection count and the configuration surface
  for a failure mode that is already contained to one lane.
- **No load-aware routing.** Distribution is pure round-robin. The
  pool does not look at queue depth, in-flight counts, or latency, and
  it does not skip a lane whose stream is down — a request dealt to a
  down lane waits for that lane to recover (see above).
- **No dynamic scaling.** Lanes are fixed at startup from
  `grpc.connections`. `StreamerPool::AddLane` after `Start()` is
  ignored (with a warning); there is no API to add or remove lanes
  while running. Restart the client to change the lane count.
- **One N for both RPCs.** The same lane count applies to
  ProcessImages and SearchImages; they cannot be tuned independently.

#### Observability and verification

- Startup logs the lane count twice: `grpc: N channel(s) created for
  <target> (TLS|insecure)` from `GrpcTransport`, and
  `application: starting — workers=… outbox=… grpc target=… lanes=N`.
- Every stream is named per lane in the logs (`ProcessImages#0`,
  `SearchImages#2`), so a failure and its reconnect are attributable:
  `ProcessImages#0: stream failed: …`, `ProcessImages#0: reconnecting
  in 2000 ms (attempt 2)`.
- `tests/test_streamer_pool.cpp` proves the mechanism against an
  in-process fake server: a 3-lane pool opens exactly one stream per
  lane (3 connections), 9 requests are dealt 3 per lane, and every
  reply round-trips by `image_id`.

#### Tuning

- **N = 1** (the default) is enough for low or steady traffic, and it
  is the previous behavior — upgrading the binary without setting the
  option changes nothing.
- **Raise N** when SearchImages bursts or sustained ProcessImages
  throughput saturate a single connection, or when you want a dead
  connection to cost less than all of your capacity (a dead lane costs
  1/N).
- **Cost per lane:** 2 CQ threads, 2 stubs, 2 long-lived streams, and
  one more TCP connection the server must accept. The cap of 16 exists
  because beyond that the extra parallelism buys nothing for
  thumbnail-sized traffic.
- To change N: set `grpc.connections: N` in `config.yaml`, rebuild
  only if the binary predates this feature, and restart — the lane
  count is read once at startup.

### Third-party components

All libraries come from Ubuntu 24.04 (`noble`) apt packages, except
TDLib itself, which is a shallow git clone. No vcpkg, no source builds
of libraries.

| Component | Version | Purpose | How it enters the build |
|---|---|---|---|
| TDLib | commit `4c6377a` (shallow clone) | Telegram client core: MTProto, auth, updates, file download | `git clone --depth 1 https://github.com/tdlib/td third_party/td`, `add_subdirectory`, link only `Td::TdStatic` (never `Td::TdJson`) |
| gRPC | 1.51.1 (`libgrpc++-dev`) | Fully-async C++ gRPC: `CompletionQueue`, bidirectional streams | apt, `find_package(gRPC CONFIG)` |
| Protocol Buffers | 3.21.12 (`protobuf-compiler`) | `image_stream.proto` code generation (`--cpp_out`, `--grpc_out`) | apt, `find_package(Protobuf)` in module mode (no `protobuf-config.cmake` in 3.21) |
| spdlog | 1.12.0 (`libspdlog-dev`) | Logging | apt |
| SQLite3 | 3.45.1 (`libsqlite3-dev`) | Outbox persistence, WAL mode | apt |
| GoogleTest | 1.14.0 (`libgtest-dev`) | Unit and end-to-end tests | apt, `find_package(GTest CONFIG)` |
| OpenSSL | 3.0.13 (`libssl-dev`) | TLS for gRPC, crypto for TDLib, AES-256-GCM for `MemoCipher` (`OpenSSL::Crypto`) | apt |
| zlib | 1.3 (`zlib1g-dev`) | Compression for TDLib/gRPC | apt |
| gperf | 3.1 | TDLib TL code generation | apt |
| php-cli | 8.3 | TDLib `SplitSource.php` build step | apt |
| ccache | 4.9.1 | Compile cache for incremental builds | apt, enabled by `TG_ENABLE_CCACHE` |
| CMake | 3.28.3 | Build system, presets | apt |
| build-essential, git, pkg-config | — | Compiler toolchain, source checkout, `pkg-config` lookups | apt |

### Modules

- `app/Application` — Assembles everything and owns the lifecycle.
  Wires the streamer callbacks: `ImageReply` → Outbox `MarkDone` by
  `image_id`; `SearchRsp` → correlation-table lookup; stream
  (re)connected → Outbox sweep (replay unacked records). Owns the
  shutdown order.
- `app/Config` — Loads `config.yaml`, expands `${ENV}` placeholders.
  Secrets (`api_id`/`api_hash`, TDLib encryption key, gRPC token, phone)
  come only from the environment, never from code or git.
- `app/AppContext` — The shared-services object passed to every handler:
  `workers`, `telegram`, `files`, `process_images`, `search_images`,
  `outbox`, `config`. Owned by `Application`, outlives all workers.
  `workers` is how async callbacks post their next stage to the pool.
- `telegram/TelegramClient` — Owns the `td::ClientManager` and the
  single client id. Runs the Receive thread, the only caller of
  `receive()`. `Send()` is callable from any thread and routes each
  response to its callback by `request_id` (async only — no
  `SendWithFuture`). `Start()` sends a bootstrap `getOption("version")`
  request: TDLib only creates the Td actor (which emits the initial
  `updateAuthorizationState`) on the first `send()` for a client id, so
  without it no update ever arrives and login waits forever.
- `telegram/AuthManager` — The login state machine: `setTdlibParameters`,
  phone code, 2FA password (from env providers), ready/closed states.
- `telegram/UpdateDispatcher` — Routes each `td_api::Update` to the
  registered `IUpdateHandler`s by `CanHandle()`; each handler runs on a
  worker thread.
- `telegram/FileDownloader` — Async: `DownloadAsync(file_id, on_done)`
  registers the callback and returns immediately. It sends `downloadFile`
  (synchronous) and completes on the response or on the matching
  `updateFile` when `local.is_downloading_completed` is set; concurrent
  downloads of the same file share one request and every callback fires.
  The callback runs on the Receive thread and must only post the next
  stage to the pool. `Download(file_id)` (future) is a thin wrapper for
  tests; its timeout is enforced by the caller via `wait_for`.
- `features/bot_command/BotCommandHandler` + `CommandParser` — Mode (a)
  trigger. Verifies the sender is the configured bot
  (`sender_user_id == command_bot_user_id`), parses the command and its
  target (reply-to or `chat_id message_id` arguments), resolves the
  target message with `getMessage`, then dispatches `/process` →
  `ProcessImages` or `/search` → `SearchImages`. Command names are
  placeholders until the user confirms the real ones.
- `features/auto_search/AutoSearchHandler` — Phase 2, disabled by
  default. Fires when a user posts an image/video in an allowlisted
  chat (`auto_search.chats`); verifies only `chat_id`, not the sender
  (channel posts may have no sender). Produces `SearchImages` only,
  never `ProcessImages`. Dedups by `(oid, id)` with a 10-minute TTL.
- `features/media_thumbnail/MediaExtractor` — Pure tier selection
  (Telethon-migration chunk 1 semantics). Photo: the LARGEST tier — the
  full-resolution original, no size cap (ties by larger max edge;
  file-less tiers are skipped). Video/animation: the thumbnail only,
  never the original file. Document: by MIME type — `image/*` sends the
  document file itself, `video/*` its thumbnail, anything else is
  rejected with a reason.
- `features/media_thumbnail/MediaThumbnailHandler` — The `/process`
  chain: extract → download → read bytes → SHA-256 (log only) → build
  `ImageRequest` (`image_id = str(message_id)`, media bytes, `memo` =
  AES-256-GCM ciphertext of the cleaned caption, `owner_id` = bare
  chat id) → Outbox PENDING → enqueue to the ProcessImages stream.
  Fails fast before downloading when the chat id does not fit the bare
  uint32 range or `KEYME` is unconfigured; an encryption failure sends
  nothing. Enqueue failure leaves the record PENDING for the retry
  thread.
- `features/media_thumbnail/SearchHandler` — The `/search` chain. Same
  extraction/download path, but builds a `SearchReq` with the bare-ID
  conversion and range checks, registers `(oid, id)` in the correlation
  table, and enqueues to the SearchImages stream. Never writes to the
  Outbox.
- `transport/GrpcTransport` — A pool of `grpc.connections` Channels
  (one per lane, one TCP/HTTP2 connection each, all to the same target;
  1 by default). TLS or insecure from config. `MakeContext()` returns
  a fresh `ClientContext` with auth metadata already applied.
- `transport/StreamerPool` — N parallel connections ("lanes") for one
  RPC: one `AsyncStreamer` per lane, requests dealt round-robin, reply
  handlers installed on every lane. Replies are matched by id, so lane
  order does not matter.
- `transport/AsyncStreamer` — Template for one fully-async bidi stream:
  its own CompletionQueue and CQ thread, a bounded write queue, reconnect
  with backoff, and the teardown contract above. Only the CQ thread
  calls `Write`/`Read`/`Finish`; workers only `Enqueue`.
- `transport/ProcessImagesClient`, `transport/SearchImagesClient` —
  Thin owners of the `ImageProcessor::Stub` halves. Each produces the
  `StartFn` (`PrepareAsync*`) its streamer uses. Reply handling lives
  in `Application`, not here.
- `infra/Outbox` — SQLite persistence in WAL mode: `Put`, `Get`,
  `MarkDone`, `MarkDeadLetter`, `DueForRetry`. One connection behind
  one mutex, shared by workers and the retry thread.
- `infra/Sha256`, `infra/FileStore` — SHA-256 hex (log only) and
  `ReadFileBytes` with a max-bytes cap.
- `common/Idempotency` — `MakeWireImageId` (`str(message_id)`, the wire
  image_id since chunk 1), `MakeImageId`
  (`<chat_id>:<message_id>:<file_unique_id>` — internal SearchReq
  correlation only) and the bare-ID conversions `ToBareChatId` /
  `ToBareMessageId` with range checks that fail loudly instead of
  truncating.
- `common/MemoCipher` — AES-256-GCM memo encryption, byte-compatible
  with the Python server (`base64(nonce[12] ‖ ct ‖ tag[16])`, key =
  SHA256(KEYME), 12 random nonce bytes per message). `Clean` applies
  the Python caption-cleaning regex (URLs, t.me links, Unicode
  whitespace, `[]*_ -`). `EncryptWithNonce` exists for the fixed-nonce
  cross-language test vectors only. Uses OpenSSL `EVP` (linked as
  `OpenSSL::Crypto`).
- `features/chat_state/ChatStateStore` — In-memory per-bare-chat-id
  state fed by `ImageReply.result` (`{"cid","mid"}` JSON, parsed by
  `ParseReplyResult`): watermark `offset_id` that only increases, a
  `chat_url` resolved on demand over TDLib (getChat candidates
  supergroup → basic group → user, then getSupergroup usernames /
  getBasicGroup upgrade; failures land on the `>>>>Nrror><id>`
  placeholder and are never retried), and `last_seen`. Memory-only in
  chunk 1; persistence is chunk 2.
- `common/Logger` — spdlog setup (level from config).

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
`KEYME`, phone) come from environment variables — never hardcoded, never
committed (`.gitignore` excludes `config/config.yaml`).

### Secrets — what each one does

**`telegram.encryption_key` (`TG_TDLIB_ENCRYPTION_KEY`) — a lock on the
local database.** TDLib keeps a local database on disk
(`database_directory`: chats, messages, contacts cache). This key
encrypts that database at rest: TDLib decrypts it on start and encrypts
it on exit. In code it is passed to `setTdlibParameters` as
`database_encryption_key` (`src/telegram/AuthManager.cpp`).

- It is **not** the Telegram account password, **not** `api_hash`, and
  it plays no role in encrypting network traffic (MTProto has its own
  encryption).
- Its job is at-rest protection: if someone copies the database files,
  they cannot read the contents without this key.
- If the key is changed or lost, the existing local database cannot be
  opened; TDLib has to rebuild it and re-sync state. Treat it as a
  secret on the same level as `api_id` / `api_hash`.

**`grpc.token` (`GRPC_TOKEN`) — an access card for the gRPC server.**
This is the credential the client shows the server to prove it is
allowed to use the `ImageProcessor` service. When a stream is created
(`GrpcTransport::MakeContext`), a non-empty token is attached to the
`ClientContext` as one metadata entry:
`authorization: Bearer <token>`. The server validates it and rejects
unauthenticated clients (`UNAUTHENTICATED` / `PERMISSION_DENIED`).

- It is a different layer from TLS: `tls: true` encrypts the transport
  (prevents eavesdropping); the token authenticates identity (prevents
  unauthorized use). TLS alone lets anyone who connects call the
  service; a token alone without TLS travels in cleartext and can be
  intercepted.
- If the server requires no authentication, leave the token empty and
  the client sends no such metadata.

**`media_thumbnail.keyme` (`KEYME`) — the memo cipher seed.** Since the
Telethon-migration chunk 1 the `memo` field on the wire is not the
caption text but `base64(nonce ‖ ciphertext ‖ tag)`, AES-256-GCM with
`key = SHA256(KEYME)` — byte-compatible with the Python server's
`aes_gcm_encrypt`/`aes_gcm_decrypt` (both directions verified with
computed test vectors; see `tests/test_memo_cipher.cpp` and
`mig-chunk1.log`). Before encryption the caption is cleaned with the
same regex the Python client applies (URLs, `t.me` links, Unicode
whitespace, `[]*_ -` all removed). The server must hold the same
`KEYME` to decrypt the memo. Treat it as a secret on the level of
`api_hash`: env only, never code, never git, never logs. If it is
unset or encryption fails, the message is NOT sent (fail loud) —
the record stays in the Outbox and the retry thread re-encounters the
error each pass.

### Private CA (grpc.ca_cert)

If the gRPC server uses a private or self-signed CA, set `grpc.ca_cert`
to the PEM file containing that CA, e.g.
`ca_cert: "/home/hkm/ca.crt"`. The file is read at startup and passed
to gRPC as the root certs; an unreadable file fails startup loudly
instead of failing later with a certificate verify error. Empty (the
default) means the system default roots. This does not help if the
server certificate has no SAN matching the target: connecting by IP
requires an IP SAN for that address, otherwise connect by the DNS name
in the certificate.

### Parallel connections (grpc.connections)

`grpc.connections: N` (default 1, range 1–16) opens N parallel gRPC
connections ("lanes") to the server instead of one; each lane is one
Channel (one TCP connection) with its own ProcessImages + SearchImages
stream, and requests are dealt round-robin across lanes. Set it when
SearchImages bursts or sustained ProcessImages throughput saturate a
single connection, or when you want one dead connection to cost only
1/N of capacity. The full mechanism — what a lane is, how traffic is
spread, and exactly what breaks and recovers when a connection dies —
is documented under Architecture → "Parallel gRPC connections (lanes)".

## Run

```bash
./build/tg-client --help
./build/tg-client --version   # tg-client 0.1.0
./build/tg-client --config config/config.yaml
```

A live run needs a real config (see Open items below): Telegram
api_id/api_hash, the target gRPC server, and the trigger bot's user_id.

First login is interactive, as in TDLib's own example
(`td_example.cpp`): start the program, read the SMS code Telegram
sends, and type it at the `Enter authentication code:` prompt (then
the 2FA password at `Enter authentication password:`, if your account
has one). After that the session is stored in `database_directory`
and later runs log in without a code. For non-interactive use,
`TG_TELEGRAM_CODE` and `TG_TELEGRAM_PASSWORD` may be set instead;
the environment takes precedence over the prompt.

To see everything Telegram sends, set `telegram.log_updates: true`
in the config: every incoming TDLib update is then logged at info
level as `telegram: update <type> {<fields>}` — update type and full
content. It is very verbose (Telegram sends a firehose, especially
on startup), so it defaults to off.

Stop with Ctrl+C (SIGINT) or `kill <pid>` (SIGTERM): the program
drains in-flight work, finishes the gRPC streams, closes the TDLib
session and exits after printing `application: stopped cleanly`.
Never `kill -9` — it skips that shutdown. TDLib's own logging is
silenced to warnings/errors at startup, so the console shows only the
program's own log lines.

## Test

```bash
ctest --test-dir build/tests --output-on-failure
```

15 test binaries, all green (2026-11-10, ~5.3 s total):

| test | covers |
|---|---|
| test_idempotency | image_id formation (`MakeImageId`/`MakeWireImageId`), bare-ID conversions |
| test_config | config parsing, env expansion (`keyme` included) |
| test_memo_cipher | chunk 1: Python-compatible caption cleaning (case by case), cross-language AES-GCM vectors in BOTH directions (Python ciphertext decrypted here; fixed-nonce C++ ciphertext byte-equal to Python's), round-trips incl. every base64 padding length, tamper/wrong-key rejection, a DISABLED live token dump for re-running the C++→Python check |
| test_chat_state | chunk 1: `{"cid","mid"}` parsing, watermark only increases, supergroup/basic/user URL forms, migrated group follows its supergroup, placeholder on failure with NO retry storm, single in-flight resolution |
| test_media_extractor | photo largest-tier (full image), tier tie-break, file-less tier skip, video/animation thumbnail-only, document `image/*`/`video/*`/reject |
| test_outbox | PENDING -> DONE / retry / dead-letter / replay |
| test_bot_command | only the configured bot's command accepted, arg parsing; wire request checks: `image_id = str(message_id)`, `memo` = KEYME-encrypted cleaned caption (decrypt-verified), `owner_id` = bare id; `SecondCommandCompletesWhileFirstDownloadPending` proves async message processing (B completes while A's download is still pending) |
| test_auto_search | allowlisted chat triggers Search only (disabled by default) |
| test_handler_registry | dispatcher registration / dispatch |
| test_async_streamer | CQ state machine vs in-process fake server: round-trip by image_id, bounded-queue backpressure, reconnect after server restart |
| test_e2e_fake_server | **chunk 3 acceptance test**: a local thumbnail file through the real pipeline (MediaThumbnailHandler/SearchHandler -> FileDownloader -> GrpcTransport -> AsyncStreamers -> fake ImageProcessor server); ProcessImages reply round-trips by `str(message_id)`, the memo arrives as KEYME ciphertext (decrypt-verified) and `owner_id` as the bare id, the Outbox record goes PENDING -> DONE; SearchImages reply round-trips by (oid, id) and resolves through the correlation table |
| test_tdlib_bootstrap | real TDLib client delivers the first authorization state (regression test for the missing bootstrap request) |
| test_auth_prompt | terminal prompts for the login code and 2FA password (env override, empty input fails fast) |
| test_update_logging | `telegram.log_updates` logs every update with type + content, silent when off |
| test_streamer_pool | 3-lane pool against a fake server: one stream per lane, 9 requests dealt 3 per lane, every reply round-trips |

Note: the socket-based tests (test_async_streamer, test_e2e_fake_server,
test_streamer_pool)
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
                    chat_state/ (ChatStateStore: {"cid","mid"} watermarks,
                    chat-URL resolution — chunk 1, in-memory)
                    media_thumbnail/ (MediaThumbnailHandler, MediaExtractor, SearchHandler)
src/transport/      GrpcTransport, AsyncStreamer (template),
                    StreamerPool (template), ProcessImagesClient,
                    SearchImagesClient
src/infra/          Outbox (SQLite), Sha256, FileStore
src/common/         Logger (spdlog), Idempotency (bare-ID conversion,
                    MakeImageId/MakeWireImageId), MemoCipher (AES-256-GCM
                    memo, chunk 1)
tests/              GTest unit tests
config/             config.example.yaml
third_party/td/     TDLib shallow clone (commit 4c6377a; built 2026-11-08,
                    libtdcore.a/libtdclient.a — never rebuild, see Build)
```
