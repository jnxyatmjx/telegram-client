# tg-client

C++ Telegram client core (TDLib, native `td::ClientManager`) with a fully
async gRPC media transport (`ImageProcessor::ProcessImages` /
`SearchImages`, bidirectional streaming, `grpc::CompletionQueue`).
Architecture: `~/workspace/tdlib-grpc-architecture.md` (v5).

## Status — build complete (2026-11-08); Telethon migration chunks 1–3 complete (2026-11-10)

构建任务已全部完成：TDLib 已编好（2026-11-08），tg-client 真实实现已完成，
二进制 `tg-client 0.1.0` 可正常运行 `--help` / `--version`。唯一还没做的是
真机联调，需要用户提供服务器地址、api_id/api_hash、触发 bot 的 user_id 和
命令名（见 Open items）。

- TDLib 已编译：`build/third_party/td/libtdcore.a`（132MB）、`libtdclient.a`，
  此后绝不再重编（见 Build 一节铁律）。
- 真实实现：登录、接收、派发、下载、两个触发器、全异步 gRPC 流、
  SQLite Outbox、bare-ID 换算。
- 测试：26 个 GTest 二进制（`tests/CMakeLists.txt`；见 Test 一节）。
- **Telethon 迁移**：chunk 1（线上协议对齐：memo AES-256-GCM、
  `image_id = str(message_id)`、bare owner_id、`ImageReply.result`
  JSON、整图发送）已合入（commit a939e0b）；chunk 2（状态持久化、
  控制命令面 `/rsme` `/rsor` `/xm` 族、历史回填引擎、媒体频道白名单、
  Outbox 旧 id 迁移）已合入（`da529f0`）；chunk 3（gRPC channel args
  健壮性 + 统一 FloodWait 处理 + 扩展文档）已合入（`9707ed9`，
  见 §Extending）。其后的 Outbox `kSent` 重试（`42c75b0`）和
  ProcessImages 流控窗口（`outbox.max_in_flight`；槽位在 `MarkDone`
  成功时释放，`4cec0e3`）也在当前 `main` 上。见
  [Outbox state](#outbox-state)。

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
This section is the short version. Where that doc and the code disagree,
the code wins; this README follows the code.

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
  writes it to the server. The handler writes the SQLite Outbox before
  that enqueue, so the bytes survive a crash. The streamer's happy path
  sends the in-memory request; the retry thread reads the Outbox only
  for rows that are still due. See [Outbox state](#outbox-state).
- **At-least-once delivery.** Every `ImageRequest` carries
  `image_id = str(message_id)` and `owner_id` (the bare chat id). The
  outbox primary key is `image_id` alone, so two chats that share a
  message id share one row; `MakeWireImageId` accepts that collision.
  A row still in `kSent` is not replayed on reconnect. Startup recovery
  and the sent-timeout safety net do re-send, and
  `Outbox::RecoverSentRecords` names server dedup of `(cid, image_id)`
  — the same pair as `(owner_id, image_id)` — as the backstop.
  Chat-state watermarks are per bare chat id: `ImageReply.result`
  `{"cid","mid"}` only moves that chat's `offset_id` forward. The old
  composite `"<chat>:<msg>:<uid>"` form (`MakeImageId`) survives only
  inside the in-memory SearchReq correlation table, keyed by `(oid, id)`.
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
  (`ToBareChatId64`: channel `-chat_id - 1e12`, basic group `-chat_id`,
  user `chat_id`) with no uint32 cap — user ids above 2^32 are valid.
  An id that is not a dialog id is a loud error and is not sent.
  `SearchReq.oid` / `SearchReq.id` are the uint32 bare ids, range-checked
  to `0..4294967295` (out of range is a loud error, never truncated),
  and `SearchReq` is not replayed from the Outbox because its correlation
  table `(oid, id) -> message` lives only in memory. `ImageReply.result`
  is the server contract `{"cid": <bare id>, "mid": <msg id>}`; it is
  parsed into `ChatStateStore` (per-chat watermark, on-demand chat-URL
  resolution; persisted when `chat_state.enabled`) and a malformed result
  is logged and dropped, never crashing the pipeline.
- **Failure handling.** `FileDownloader` has no download timer: a
  download that never completes simply never resolves. A TDLib 429 on
  `downloadFile` is retried through `FloodRetry` (up to 3 re-issues),
  then the download fails; that path does not dead-letter the Outbox.
  A full ProcessImages write queue does not block the worker (`Enqueue`
  timeout 0): the record goes to `kFailed`, due immediately, for the
  retry thread. A broken gRPC stream reconnects with backoff
  (1s → 2s → 4s … cap 30s). Reconnect replays only `kPending` /
  `kFailed` rows whose `next_retry_at` has passed; a `kSent` row stays
  sent until its reply, its safety deadline, or a permanent error.
  `INVALID_ARGUMENT`, `PERMISSION_DENIED`, `UNIMPLEMENTED` and
  `NOT_FOUND` dead-letter `kPending`, `kFailed` and `kSent` immediately.

### Outbox state

`outbox.state` is `tg::infra::OutboxState` (`src/infra/Outbox.h`),
stored as an INTEGER. Names and values:

| value | name | meaning |
|---|---|---|
| 0 | `kPending` | In the queue, not yet accepted by a stream write, or re-armed for another send. `DueForRetry` returns it once `next_retry_at` has passed. |
| 4 | `kSent` | The request was written to a stream (`MarkSent`) and is waiting for `ImageReply`. Invisible to `DueForRetry`. `next_retry_at` is the safety deadline. |
| 1 | `kDone` | `MarkDone` recorded the reply. Eligible for purge after `outbox.purge_done_after_seconds`. |
| 2 | `kFailed` | The send did not leave the client (`MarkFailed`: queue full, or the write was lost while the row was still `kPending`). Still inside the retry window: `DueForRetry` picks it up when `next_retry_at` passes. |
| 3 | `kDeadLetter` | No automatic send. `DueForRetry` and `SentPastDeadline` both skip it. |

There is no other state. The usual path is `kPending` → `kSent` →
`kDone`. A lost write goes to `kFailed` and is sent again; once
`MarkRequeued` has incremented `attempts` to `outbox.max_attempts`
(default 5) the row becomes `kDeadLetter`.

Transitions, as the SQL actually filters them:

- **`Put`** inserts or replaces on `image_id`. `FinishProcess` stores
  `kPending` with `next_retry_at = now + outbox.send_grace_ms` (default
  5 s) and `attempts` 0. Re-processing an id that is still `kSent`
  releases that flow-control slot first, then overwrites the row.
- **`MarkSent(id, sent_until_ms)`** — `kPending` or `kFailed` →
  `kSent`, `next_retry_at = sent_until_ms` (`now + outbox.sent_timeout_ms`,
  default 600 s). Returns false (no change) for an id that is already
  `kSent`, `kDone`, `kDeadLetter`, or unknown. A false write-ok
  releases the flow-control slot; a true one keeps it.
- **`MarkDone(id, result)`** — `kPending`, `kFailed` or `kSent` →
  `kDone`, `last_status = result`, `updated_at = now`. Returns false
  for `kDone`, `kDeadLetter`, or an unknown id. **A true result
  releases one ProcessImages flow-control slot, before the
  `{"cid","mid"}` parse, so a non-JSON `result` still frees the
  window.** A false result releases nothing — a second reply must
  not under-count the window. The duplicate-`kDone` case is logged as
  already done; any other false result is logged as an unknown
  `image_id`. When `result` parses as `{"cid","mid"}`, the watermark
  update still runs, including after a duplicate reply.
- **`MarkFailed(id, error, delay_ms)`** — only `kPending` → `kFailed`,
  `next_retry_at = now + delay_ms`. The call sites pass `0`, so the
  row is due immediately. A write-lost callback always releases the
  slot, then calls this; a lost duplicate of a row that is already
  `kSent`, `kDone` or `kDeadLetter` does not move it.
- **`MarkRequeued(id, delay_ms, max_attempts, note)`** — any existing
  row. `attempts` becomes `attempts + 1`. If that value is `>=
  max_attempts` the state is `kDeadLetter`, otherwise `kPending`, and
  `next_retry_at = now + delay_ms`. The retry sweep calls it after a
  successful re-enqueue (`retry_backoff_ms`, default 5 s, note
  `"requeued for send"`). The safety net calls it for a `kSent` row
  past its deadline (note `"ack timeout"`) and, on success, releases
  the slot. A window-blocked row is not `MarkRequeued`, so sitting
  behind the cap does not burn attempts.
- **`MarkDeadLetter(id, error)`** — any existing row → `kDeadLetter`.
  A permanent gRPC status selects `kPending` / `kFailed` with
  `DueForRetry(now + 24h, 10000)` and `kSent` with
  `SentPastDeadline(now + 24h, 10000)`, then dead-letters each.
  Queued `kPending` / `kFailed` copies release the slot later, via
  write-lost. Each dead-lettered `kSent` row releases one slot in
  this callback.
- **Startup** (`RecoverSentRecords`, grace = `send_grace_ms`): every
  `kSent` row becomes `kPending` with `next_retry_at = now + grace`.
  Shutdown does not clear `kSent`; the next open does. If no `kSent`
  rows are left, the update changes nothing.
- **`PurgeDoneBefore(cutoff)`** deletes rows with `state = kDone` AND
  `updated_at <= cutoff`. Nothing else is deleted. The retry thread
  calls it once per sweep with
  `cutoff = now - outbox.purge_done_after_seconds * 1000` (default
  86400 s, so a day after `MarkDone`). `0` makes the cutoff `now`, so
  every `kDone` row goes on the next sweep. `kPending`, `kSent`,
  `kFailed` and `kDeadLetter` stay until some other transition moves
  them.

`SweepOutbox` (retry thread every `outbox.retry_interval_ms`, default
10 s, and on every lane reconnect, including SearchImages) does two
passes, each capped at 100 rows:

1. `DueForRetry`: `kPending` and `kFailed` with `next_retry_at <= now`.
   Each one `TryAcquire`s a flow-control slot, then `Enqueue`s. Success
   → `MarkRequeued`. The window is full → leave the row alone. Enqueue
   fails → release the slot, leave the row due.
2. `SentPastDeadline`: `kSent` with `next_retry_at <= now` →
   `MarkRequeued` and release the slot. That is the only timer that
   puts a written request back into the retry set.

Flow control (`src/app/FlowControl.h`, `outbox.max_in_flight`, default
60; `0` disables it) counts ProcessImages requests from `TryAcquire`
until their fate is known: copies sitting in a lane queue and rows
already `kSent`. SearchImages never takes a slot. `Release` decrements
unconditionally (a rare double release can make the counter negative;
it does not clamp at zero, so a real release cannot be lost). Slots
come back on a successful `MarkDone`, a write-lost, a failed enqueue
after acquire, a false `MarkSent`, a safety-net requeue, a permanent
error's `kSent` dead-letter, or `FinishProcess` replacing a `kSent`
row. A full window leaves the new row `kPending` until `send_grace_ms`,
then a later sweep sends it.

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
  ResumeTimer threads (3: FloodRetry, HistoryBackfill, DialogScanner)
  Chat-state flush thread (1, only when chat_state.enabled)
  TDLib internal threads (N, owned by TDLib)
```

| Thread | Count | Does | Must never do |
|---|---|---|---|
| Main | 1 | Parse config, assemble `Application`, start threads, wait for SIGINT/SIGTERM, stop threads in order, join | Call `ClientManager::receive()`, call gRPC `Write` |
| TDLib Receive | 1 | The only caller of `ClientManager::receive(10.0)`; routes responses to their callbacks by `request_id`, hands updates (`request_id == 0`) to `UpdateDispatcher` | Download, read disk, call gRPC, block for more than a few ms |
| Worker Pool | 4 | Handler stages (short, never blocking): extract tier, start download, finish stage (read file, hash, build `ImageRequest`, write Outbox, enqueue to streamer) | Call `receive()`; call gRPC `Write`/`Read` directly (only `Enqueue`); block on a TDLib response or a download |
| ProcessImages CQ | grpc.connections | One per lane: owns that lane's `ProcessImages` CompletionQueue and `ClientAsyncReaderWriter`; loops `cq.Next()`, handles Write done, Read replies, Finish, reconnect | Call TDLib `send`/`receive` (use thread-safe `TelegramClient::Send`) |
| SearchImages CQ | grpc.connections | Same as above, for the `SearchImages` stream, its own CQ and stub per lane | Same; the two streams are isolated, one breaking does not affect the other |
| Outbox Retry | 1 | Every `outbox.retry_interval_ms` (10 s): `SweepOutbox` (due `kPending`/`kFailed`, plus `kSent` past `sent_timeout_ms`), then `PurgeDoneBefore` | Call gRPC `Write` directly (only `Enqueue`) |
| ResumeTimer | 3 | One thread each for `FloodRetry`, `HistoryBackfill` and `DialogScanner`; idle until a FloodWait deadline | Block, or do file I/O, in the fire callback |
| Chat-state flush | 0 or 1 | When `chat_state.enabled`: every `flush_interval_seconds`, `FlushActive`; a final full flush on shutdown | Run when the feature is off |
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
  non-blocking attempt — a full queue `MarkFailed`s the record, due
  immediately, for the retry thread). `FileDownloader` has no timer: a download that never
  completes simply never resolves, and no thread is blocked waiting for
  it. There is no blocking wait API at all — `ITdSender::SendWithFuture`
  was removed, and `FileDownloader::Download` (future) exists only for
  tests. Regression test:
  `BotCommandTest.SecondCommandCompletesWhileFirstDownloadPending`
  (message A's download takes 2 s, B's is instant; B's request reaches
  the streamer queue while A is still downloading).
- **Downloaded files are deleted from TDLib's cache (deleteFile,
  2026-11-11).** Every `FileDownloader::DownloadAsync` call counts one
  consumer (shared file_ids share one download); the consumer calls
  `Release` only AFTER the bytes are stored (Outbox BLOB for ProcessImages,
  enqueued `SearchReq` for SearchImages), and at zero claims a
  fire-and-forget `td_api::deleteFile` removes the cached file from
  `telegram.files_directory` — best-effort, errors logged not fatal.
  No delete on download failure (file may not exist / be partial) and no
  delete during shutdown (`FailAll` clears the claims). Regression tests:
  `tests/test_delete_file.cpp`.
- **Async gRPC state machine (one per stream).**
  `CREATE` (PrepareAsync + StartCall) → `CONNECTING`/`RUNNING`
  (issue a Read immediately; Write when the queue is non-empty and no
  Write is in flight) → `WRITE_DONE` (next Write) / `READ_DONE`
  (handle the reply by `image_id`/`(oid, id)`, issue the next Read) →
  `FINISH` (server closed the stream, error, or shutdown) →
  `RECONNECT` (backoff 1s → 2s → 4s … cap 30s, then CREATE again).
  Each stream gets a fresh `ClientContext` with auth metadata attached
  once per stream, never per message. There is no per-message deadline.
  Replays are the Outbox sweep: write-ok moves the row to `kSent` until
  `outbox.sent_timeout_ms`; `kPending` / `kFailed` are sent again when
  `next_retry_at` passes. `grpc.ack_timeout_ms` is parsed and unused.
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
- **Shutdown order** (`Application::Run`). (1) `HistoryBackfill` and
  `DialogScanner` refuse new TDLib sends; `FloodRetry::Shutdown` drops
  pending 429 re-sends. (2) `TelegramClient::Stop()` completes pending
  TDLib callbacks (their download callbacks post finish stages).
  (3) `FileDownloader::FailAll` for anything still pending.
  (4) Drain the worker pool (10 s). Unfinished Outbox rows stay on
  disk. (5) Both streamers: cancel, `WritesDone`, `Finish`, then shut
  the CQ and join the CQ thread. (6) Join the Outbox retry thread.
  (7) Join the chat-state flush thread and, when it is enabled, flush
  every entry (not only the 45 s window).

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
Threads besides the per-lane CQ pair: Main (1) + TDLib Receive (1) +
Worker Pool (4) + Outbox Retry (1) + three `ResumeTimer` threads, so
**10 + 2 × connections**, plus the chat-state flush thread when
`chat_state.enabled`. Each lane also holds 2 stubs and 2 long-lived
streams. TDLib's own threads are extra and not in that count.

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
  Outbox before it was enqueued. On reconnect, `OnReconnected` fires
  `SweepOutbox`, which re-enqueues `kPending` / `kFailed` rows whose
  retry time has come (under the flow-control window) and requeues
  `kSent` rows past `sent_timeout_ms`. A `kSent` row still inside its
  deadline is not sent again. A later safety-net or restart replay is
  at-least-once; the backstop named in `RecoverSentRecords` is server
  dedup of `(cid, image_id)`. A dead lane delays ProcessImages traffic,
  it does not drop it.
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

**Reconnect side effects.** `OnReconnected` fires once per lane (both
RPCs), and each firing sweeps the Outbox. The sweep only takes rows
that are already due, so several lanes reconnecting at once do not
re-send a `kSent` row that is still inside its deadline. A safety-net
requeue can send a written request again; that replay is at-least-once.
Permanent gRPC errors (`INVALID_ARGUMENT`, `PERMISSION_DENIED`,
`UNIMPLEMENTED`, `NOT_FOUND`) are different from a dead connection:
they dead-letter `kPending`, `kFailed` and `kSent` rows (up to 10000
of each, anything due within 24 h) instead of retrying, because
retrying cannot help. Each dead-lettered `kSent` row releases its
flow-control slot in that callback.

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
  Wires the streamer callbacks: `ImageReply` → `OnProcessImageReply`
  (`MarkDone` by `image_id`, and `FlowControl::Release` only when that
  returns true) plus the `{"cid","mid"}` watermark; write-ok →
  `MarkSent` (false → release the slot); write-lost → release the slot
  and `MarkFailed`; permanent gRPC status → `MarkDeadLetter`;
  `SearchRsp` → correlation-table lookup, then one Telegram reply to
  that message (hit list, or "No One Find"); stream (re)connected →
  `SweepOutbox`. Owns the shutdown order above.
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
  stage to the pool. Since chunk 3 a `downloadFile` FloodWait (TDLib 429)
  is retried through the shared `telegram/FloodRetry` (bounded attempts;
  while a retry is pending, later downloads of the same file ride along —
  there is never a second request before retry-after passes; without a
  scheduler the old fail-fast behaviour stays).
  **Cache cleanup (deleteFile, 2026-11-11):** every `DownloadAsync` call
  counts one consumer; `Release(file_id)` drops the claim, and at zero the
  cached local file is removed from TDLib's cache with a fire-and-forget
  `td_api::deleteFile` (best-effort — an error is logged, never fatal) so
  downloads do not accumulate in `telegram.files_directory`. Handlers call
  `Release` only after the bytes are stored (Outbox BLOB for ProcessImages /
  enqueued `SearchReq` for SearchImages); shared file_ids (forwarded photo,
  same message twice) delete exactly once, at the last `Release`. A failed
  download deletes nothing; shutdown (`FailAll`) deletes nothing.
  `Download(file_id)` (future) is a thin wrapper for
  tests; its timeout is enforced by the caller via `wait_for`.
- `features/bot_command/BotCommandHandler` + `CommandParser` — Mode (a)
  trigger. Verifies the sender is the configured bot
  (`sender_user_id == command_bot_user_id`), parses the command and its
  target (reply-to or `chat_id message_id` arguments), resolves the
  target message with `getMessage`, then dispatches `/process` →
  `ProcessImages` or `/search` → `SearchImages`. Command names are
  placeholders until the user confirms the real ones.
- `features/auto_search/AutoSearchHandler` — Disabled by default.
  Fires when a photo, video, animation, `image/*` document, or
  `video/mp4` document is posted in an allowlisted chat, including a
  forward of one of those. `auto_search.chats` accepts TDLib dialog ids
  (`-100…`) and public usernames (`@name`, `name`, `t.me/name`,
  `https://t.me/name`); `t.me/c/<bare>` becomes the `-100…` dialog id.
  Quoted and unquoted `@name` both parse. After authorization,
  usernames are resolved with `searchPublicChat` on a worker (never the
  receive thread); FloodWait goes through `FloodRetry`. Each entry is
  logged as `auto_search: @name -> chat_id <id> (title "...", type ...)`
  and the effective id list is logged once resolution finishes. A chat
  that is not a channel/supergroup is not added; not-found and
  not-a-member are warnings (not a member still keeps the id, but
  updates will not arrive). Verifies only `chat_id`, not the sender.
  Produces `SearchImages` only, never `ProcessImages`. Dedups by
  `(oid, id)` with a 10-minute TTL. `SearchReply` turns the `SearchRsp`
  into one MarkdownV2 reply to that message: numbered `[memo](url)`
  hits (memo decrypted with `KEYME`, 28 code points, preview off) or
  the fixed "No One Find" line (preview on). The send uses the
  correlation entry's TDLib chat id, never the uint32 `oid`.
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
  chat id) → Outbox `kPending` (`next_retry_at` = now +
  `send_grace_ms`) → flow-control `TryAcquire` → enqueue to the
  ProcessImages stream. A full window leaves the row `kPending` for a
  later sweep. Fails fast before downloading when the chat id is not a
  valid bare id or `KEYME` is unconfigured; an encryption failure sends
  nothing. Enqueue failure releases the slot and `MarkFailed`s the row
  (due now) for the retry thread. After the Outbox `Put` (bytes safe in the BLOB) — and on every
  early-return path — the handler drops its claim on the local file
  (`FileDownloader::Release` → `deleteFile` at the last consumer); the
  retry replays from the BLOB, never from the file.
- `features/media_thumbnail/SearchHandler` — The `/search` chain. Same
  extraction/download path, but builds a `SearchReq` with the bare-ID
  conversion and range checks, registers `(oid, id)` in the correlation
  table, and enqueues to the SearchImages stream. Never writes to the
  Outbox. The matching `SearchRsp` is rendered by `auto_search/SearchReply`
  and sent as a reply to the correlated message. After the enqueue
  attempt (a dropped `SearchReq` is recovered by re-triggering, not by
  re-reading the file) — and on every
  early-return path — the handler drops its claim on the local file
  (`FileDownloader::Release` → `deleteFile` at the last consumer).
- `transport/GrpcTransport` — A pool of `grpc.connections` Channels
  (one per lane, one TCP/HTTP2 connection each, all to the same target;
  1 by default). TLS or insecure from config. Every channel is built with
  `MakeChannelArgs` (chunk 3): the per-lane subchannel-pool flag plus the
  configurable keepalive / 64 MiB message caps / HTTP-2 window args (see
  Configure → gRPC channel args). `MakeContext()` returns
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
- `infra/Outbox` — SQLite persistence in WAL mode, primary key
  `image_id`. `Put`, `Get`, `MarkSent`, `MarkDone`, `MarkFailed`,
  `MarkRequeued`, `MarkDeadLetter`, `DueForRetry`, `SentPastDeadline`,
  `PurgeDoneBefore`. State values and transitions:
  [Outbox state](#outbox-state). One connection behind one mutex,
  shared by workers, CQ threads and the retry thread. On open it runs
  the legacy-id migration (chunk 2): rows whose image_id contains `:`
  (the Telethon client's `<chat>:<message_id>:<uid>` keys) are rewritten
  to `str(message_id)`; a collision with an existing current-format row
  drops the legacy row (the local key is the message-id string); a
  non-numeric middle segment is dead-lettered for inspection — never
  replayed, never wedging the retry loop. Then `RecoverSentRecords`
  turns leftover `kSent` rows into `kPending`. Both passes are
  idempotent on every reopen.
- `infra/Sha256`, `infra/FileStore` — SHA-256 hex (log only) and
  `ReadFileBytes` with a max-bytes cap.
- `common/Idempotency` — `MakeWireImageId` (`str(message_id)`, the wire
  image_id since chunk 1), `MakeImageId`
  (`<chat_id>:<message_id>:<file_unique_id>` — internal SearchReq
  correlation only), `DialogIdCandidates` (bare → TDLib probe candidates
  supergroup → basic group → user) and the bare-ID conversions. Two
  widths, since chunk 2: `ToBareChatId` / `ToBareMessageId` for the
  proto-uint32 search fields (uint32 range checks that fail loudly
  instead of truncating) and `ToBareChatId64` for `ImageRequest.owner_id`
  (proto int64, NO uint32 cap — Telegram now allocates user ids above
  2^32, e.g. 7974301646; capping owner_id would refuse those chats
  entirely).
- `common/MemoCipher` — AES-256-GCM memo encryption, byte-compatible
  with the Python server (`base64(nonce[12] ‖ ct ‖ tag[16])`, key =
  SHA256(KEYME), 12 random nonce bytes per message). `Clean` applies
  the Python caption-cleaning regex (URLs, t.me links, Unicode
  whitespace, `[]*_ -`). `EncryptWithNonce` exists for the fixed-nonce
  cross-language test vectors only. Uses OpenSSL `EVP` (linked as
  `OpenSSL::Crypto`).
- `features/chat_state/ChatStateStore` — Per-bare-chat-id state fed by
  `ImageReply.result` (`{"cid","mid"}` JSON, parsed by
  `ParseReplyResult`): watermark `offset_id` that only increases, a
  `chat_url` resolved on demand over TDLib (getChat candidates
  supergroup → basic group → user, then getSupergroup usernames /
  getBasicGroup upgrade; failures land on the `>>>>Nrror><id>`
  placeholder and are never retried), and `last_seen`. Since chunk 2
  the store flushes through a `ChatStateBackend` (below): every
  `chat_state.flush_interval_seconds` the Application flush thread
  writes entries active within `active_window_seconds`, and startup
  restores the table before the flush thread starts.
- `features/chat_state/ChatStateBackend` + `SqliteChatStateBackend` —
  The storage seam behind `ChatStateStore` (chunk 2). The interface
  (`Upsert` / `LoadAll`) exists so the planned Cloudflare D1 backend —
  the Python client's backend, sharing the watermark table — can be
  swapped in without touching the store or the wiring. The default
  implementation is local SQLite with the exact Python `chathis` schema
  (column names, order, `ON CONFLICT(chat_id)` upsert —
  app_client.py schedule2db), a separate database file from the Outbox.
  Timestamps are GMT+8 civil strings in the Python formats
  (`common/Gmt8`), so D1 rows written by either deployment parse back
  in both. Full D1 design: `docs/pending/state-persistence-and-d1.md`.
- `features/commands/ControlCommandsHandler` — The control-chat command
  surface (chunk 2; Python `handlersm` / `handlerfetchhistory` /
  `handrestore`). `/rsme` lists dialogs (channels/groups then users,
  `list_batch_lines` per reply message); the listing hands out BARE
  chat ids as `/xm`-family arguments. `/rsor` audits stale/illegal
  dialogs (scam/fake/left/banned/deactivated/migrated/private reports
  from `getSupergroupFullInfo`) — reports only, NEVER auto-leaves
  (Python's auto-LeaveChannelRequest is deliberately not migrated;
  destructive, Hong to confirm). `/xm` `/xy` `/ym` `/who <bare_chat_id>`
  start a history backfill. Triggers follow the Python model: an
  OUTGOING message in the control chat (`self_commands`) and/or the
  configured bot sending there; incoming media from self is ignored.
- `features/commands/DialogScanner` — Dialog enumeration for the
  command surface (chunk 2). This TDLib build has no legacy
  `getDialogs(offset, …)`: `loadChats` is called repeatedly until the
  documented 404 "all chats loaded" signal, then `getChats` returns the
  ordered list; each dialog is then resolved sequentially (getChat →
  getUser / getBasicGroup / getSupergroup, migrated groups re-entered
  at their supergroup — Python `resolve_active_chat_entity`), one
  outstanding request at a time; FloodWaits pause via `ResumeTimer`.
- `features/history_backfill/HistoryBackfill` — The engine behind
  `/xm` & co (chunk 2; Python `handlerfetchhistory`). Phase A pages
  `searchChatMessages` (photoAndVideo filter) BACKWARD from the newest
  message down to the chat's persisted watermark (8 bytes per id —
  only the gap is collected, message objects are re-fetched per id in
  phase B to keep memory flat). Phase B replays the gap ASCENDING
  through the realtime pipeline (`getMessage` →
  `MediaThumbnailHandler::SendProcessImages`), so `image_id =
  str(message_id)` dedupes server-side and the watermark re-read per
  batch skips what the server already confirmed (resume-friendliness).
  FloodWait (429 + retry-after, `telegram/TdErrors`) pauses the job for
  `retry_after + flood_pad_seconds` via `common/ResumeTimer`. Unknown
  peers are dropped with a notice BEFORE any search (Python checks
  `get_entity` first); a duplicate Start for a running chat is refused
  with a notice; each dispatch re-reads the CURRENT watermark so a
  restart never re-downloads covered messages.
- `features/media_channels/MediaChannelHandler` — Whitelist chats whose
  media goes through the pipeline automatically (chunk 2; Python
  `handmedias` main chain). OFF by default — the realtime ImageReply
  chain already drives the pipeline. Chats are BARE ids (same space as
  `chathis.chat_id`); `rpc` selects ProcessImages (default) or
  SearchImages. First-claimant-wins with auto_search when both match a
  chat: registration order decides.
- `telegram/MessageSender` — The single sendMessage path (chunk 2;
  Python `SendText`). Splits at the 4096-character hard limit
  (line boundaries preferred — Telethon's 45-line batches can exceed
  the limit and TDLib has no auto-split), disables link previews on
  every chunk, sends markdown fire-and-forget (a failed reply is a
  lost reply; the command can be re-issued — matching Python). Since
  chunk 3 each chunk that comes back 429 is re-sent once the retry-after
  passes via the shared `telegram/FloodRetry` (bounded; a 429 was
  rejected, never applied, so re-sending cannot duplicate).
- `telegram/TdErrors` — TDLib 429 classification (chunk 2): TDLib embeds
  retry-after in the error MESSAGE text
  (`"Too Many Requests: retry after <seconds>"` — no structured
  field in this TDLib version); `RetryAfterSeconds` parses it,
  `IsFloodWait` matches code 429, `RetryAfterOr` adds the fallback.
  The single place error text is parsed — call sites never do it
  themselves.
- `telegram/FloodRetry` — The shared FloodWait scheduler (chunk 3).
  One instance owned by `Application`, wired into `FileDownloader`,
  `MessageSender` and `BotCommandHandler`. `Schedule(error, attempt,
  retry)` runs `retry` on its timer thread after retry-after + pad
  (pad = `history_backfill.flood_pad_seconds`), up to
  `max_attempts` (3) re-issues, then the caller gives up and logs.
  TDLib requests are move-only and cannot be cloned, so the retry
  closure rebuilds its request explicitly — parsing is centralized in
  `TdErrors`, scheduling here; there is deliberately no generic
  send-retry decorator. `Shutdown()` drops pending retries (called
  first in Application teardown, before `telegram_.Stop()`). Feature
  jobs that pause WHOLE tasks (`HistoryBackfill`, `DialogScanner`)
  keep their own `ResumeTimer` pause instead.
- `common/Gmt8` — Fixed-offset GMT+8 civil time (chunk 2), the analogue
  of Python's `gmt8_now()` + strftime: `Gmt8Date` (`YYYYMMDD`),
  `Gmt8Clock` (`HH:MM:SS`), `Gmt8DateTime` (`YYYY-MM-DD HH:MM:SS`) —
  the exact `chathis` timestamp formats (SURVEY §8.5-4).
- `common/ResumeTimer` — One-slot delay scheduler (chunk 2) for
  FloodWait backoff: Telethon does `await asyncio.sleep(e.seconds + 1)`;
  the C++ side has no event loop and no thread may block on I/O, so the
  pause is a deadline and this thread fires when it passes. Used by
  `HistoryBackfill`, `DialogScanner` and, since chunk 3, internally by
  `telegram/FloodRetry`.
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
unset, `SendProcessImages` refuses before the download and nothing is
written to the Outbox. If encryption fails after the download, the
finish stage logs and returns before `Put` — again nothing is queued,
and the retry thread has no row to re-encounter.

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

### gRPC channel args (chunk 3)

Every lane channel is built through `GrpcTransport::MakeChannelArgs`
(`src/transport/GrpcTransport.cpp`) with the per-lane subchannel-pool
flag (what keeps N lanes on N TCP connections) plus the following
configurable args — each `0`/absent keeps the gRPC default, so the
defaults below are what a fresh `config.yaml` gets:

| key | default | channel arg | why |
|---|---|---|---|
| `grpc.keepalive_ms` | 60000 | `GRPC_ARG_KEEPALIVE_TIME_MS` | ping an idle lane every 60 s so NAT/LB doesn't drop the long-lived streams. If the server answers `too_many_pings`, RAISE this — never lower it |
| `grpc.keepalive_timeout_ms` | 20000 | `GRPC_ARG_KEEPALIVE_TIMEOUT_MS` | a ping unanswered this long marks the connection dead (reconnect takes over) |
| `grpc.keepalive_permit_without_calls` | true | `GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS` | keep the lane TCP healthy even with no call in flight |
| `grpc.max_message_bytes` | 67108864 (64 MiB) | `GRPC_ARG_MAX_SEND_MESSAGE_LENGTH` + `GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH` | matches the Python client's `max_send_message_length=64 MiB`; full images must fit in one message |
| `grpc.http2_lookahead_bytes` | 1048576 (1 MiB) | `GRPC_ARG_HTTP2_STREAM_LOOKAHEAD_BYTES` | how far ahead gRPC requests stream data (gRPC default 64 KiB) — larger = fewer flow-control stalls on big image sends |
| `grpc.http2_write_buffer_bytes` | 524288 (512 KiB) | `GRPC_ARG_HTTP2_WRITE_BUFFER_SIZE` | per-call write buffer, smooths large sends |

`grpc.ack_timeout_ms` is NOT one of these. It is still parsed (default
30000) so an old `config.yaml` loads, and nothing in the send path
reads it. Deadlines live on the outbox, below.

### Outbox

Keys under `outbox:` (`src/app/Config.h`; the example file comments
them). State names are in [Outbox state](#outbox-state).

| key | default | effect |
|---|---|---|
| `outbox.db_path` | `/data/outbox.sqlite` | WAL database. Primary key is `image_id` |
| `outbox.retry_interval_ms` | 10000 | Retry-thread period: sweep, then purge |
| `outbox.retry_backoff_ms` | 5000 | `next_retry_at` delay applied by `MarkRequeued` |
| `outbox.max_attempts` | 5 | `MarkRequeued` dead-letters when the incremented `attempts` reaches this |
| `outbox.send_grace_ms` | 5000 | `Put` deadline, and the grace that turns leftover `kSent` rows into `kPending` on open |
| `outbox.sent_timeout_ms` | 600000 | Safety deadline stored by `MarkSent` (`kSent` → requeue) |
| `outbox.max_in_flight` | 60 | ProcessImages flow-control window. `0` disables it. SearchImages ignores it |
| `outbox.purge_done_after_seconds` | 86400 | Each sweep deletes `kDone` rows whose `updated_at` is at least this old. `0` deletes every `kDone` row on the next sweep. Other states are kept |

### Feature toggles (Telethon migration chunk 2)

Four new config blocks (all documented inline in
`config/config.example.yaml`; defaults keep today's behaviour — nothing
auto-forwards, no commands answered until `features.commands.control_chat_id`
is set):

- `features.commands` — the control-chat command surface. Set
  `control_chat_id` (TDLib dialog id of the control conversation; 0 =
  commands off) and optionally `self_commands: true` so the account itself
  typing `/rsme` / `/rsor` / `/xm <bare_chat_id>` triggers them (the Python
  model). `dialog_limit`, `list_batch_lines`, `rsor_report_limit` tune the
  listing/audit sizes.
- `features.history_backfill` — the `/xm` engine. `page_limit: 60` (TDLib
  caps 100), `flood_pad_seconds: 1` (Telethon sleeps `e.seconds + 1`).
- `features.media_channels` — whitelist auto-forward (Python `handmedias`),
  OFF by default; `chats` are BARE ids, `rpc` picks `process` (Python main
  chain) or `search`.
- `chat_state` — watermark persistence: `enabled: false` until you provide
  a `db_path` (default `/data/chathis.sqlite`); flushes every
  `flush_interval_seconds` (30 s), entries active within
  `active_window_seconds` (45 s), schema and GMT+8 fields byte-compatible
  with the Python `chathis` table. The D1 backend arrives later behind the
  same interface (`docs/pending/state-persistence-and-d1.md`).

## Extending the client — how to add a new feature

This is the recipe for the three most common extensions, with the real
file paths. §4 of `~/workspace/tdlib-grpc-architecture.md` is the
original Chinese write-up; follow the paths in this section when they
differ. Existing features to copy from:
a small handler is `features/media_channels/MediaChannelHandler`, a
command is `/rsme` in `features/commands/ControlCommandsHandler`, an RPC
is `SearchImages`.

### Ground rules (every scenario)

- **Threads — never block.** Handler and callback code runs on the
  Receive thread or a worker thread. Never call
  `ClientManager::receive()` (only `TelegramClient`'s Receive thread
  does), never call gRPC `Write` directly (only a streamer's CQ thread
  does — everyone else `Enqueue`s), never wait on a TDLib response or a
  download. Send the request, return, and continue in the response
  callback — and that callback must only post the next stage to
  `AppContext::workers`. There is no blocking-wait API anywhere by
  design.
- **TDLib access** goes only through `ITdSender::Send`
  (`src/telegram/TelegramClient.h`). Never create your own
  `td::ClientManager`; never send a request a core path already owns
  (sendMessage → `MessageSender`, downloadFile → `FileDownloader`) —
  add there, don't bypass.
- **FloodWait (TDLib 429).** Parse with `telegram/TdErrors.h`
  (`IsFloodWait`, `RetryAfterSeconds`/`RetryAfterOr`) — never match the
  error text yourself. For one-shot requests (a sendMessage, a
  getMessage, a downloadFile) schedule bounded re-issues through the
  shared `telegram/FloodRetry` instance — `Application` owns it and
  passes the pointer to constructors (see `FileDownloader`,
  `MessageSender`, `BotCommandHandler`). TDLib requests are move-only
  and can't be cloned, so each retry closure rebuilds its request
  explicitly; there is deliberately no generic send-retry decorator. A
  long-running scan job instead pauses the WHOLE job via
  `common/ResumeTimer` (copy `HistoryBackfill`). A 429 means the request
  was rejected — re-sending it can never duplicate anything, even for
  sendMessage.
- **Secrets** are `${ENV}` placeholders in config only (`KEYME`,
  `GRPC_TOKEN`, `TG_API_HASH`, …) — never a default value, never git,
  never logs.
- **Never rebuild TDLib.** Build only your targets:
  `cmake --build build --target test_<feature> -j1`; run tests from
  `build/tests/` (never top-level `build/` — TDLib's never-built
  `run_all_tests` lives there). TDLib's `TlObject.h` prints
  `-Wunused-parameter` noise on every compile — filter it with
  `| grep -v third_party` before counting your own warnings.

### Scenario 1 — a new update handler (react to a class of Telegram updates)

1. Create `src/features/<feature>/<Feature>Handler.{h,cpp}` implementing
   `telegram::IUpdateHandler` (`src/telegram/UpdateHandler.h`):
   `CanHandle()` claims only your update type(s). Registration order is
   first-claimant-wins — that is how `media_channels` and `auto_search`
   resolve a chat claimed by both.
2. Take shared services from `AppContext` (`src/app/AppContext.h`):
   `workers`, `telegram`, `files`, `process_images`, `search_images`,
   `outbox`, `config`. Handlers never reach into `Application`.
3. Register in `src/app/Application.cpp`: construct the handler with its
   dependencies (including `&flood_retry_` if it sends rate-limited
   TDLib calls) and `Register` it on the dispatcher next to the others.
4. Config: struct + defaults in `src/app/Config.h` (default OFF unless
   the Python twin was always-on), parse in `src/app/Config.cpp`, comment
   into `config/config.example.yaml`, assert in `tests/test_config.cpp`.
5. Test `tests/test_<feature>.cpp` with a fake `ITdSender` (a minimal one
   is `FloodFakeSender` in `tests/test_flood_retry.cpp`; handler-level
   fixtures are in `tests/test_bot_command.cpp`). Add the file to
   `tests/CMakeLists.txt` (the foreach picks it up), build
   `--target test_<feature>`, run `ctest -R test_<feature>` from
   `build/tests/`. Set a `TIMEOUT` test property if it waits on real
   timers/sockets.

### Scenario 2 — a new control command (e.g. `/ping`)

1. Add the command name to `CommandsConfig` (`src/app/Config.h`, e.g.
   `ping_command: "/ping"` with a default), parse it
   (`src/app/Config.cpp`), document it (`config/config.example.yaml`),
   assert it (`tests/test_config.cpp`).
2. Classify it where `/rsme` & co are classified in
   `src/features/commands/ControlCommandsHandler.cpp`; the trigger
   directions (self-commands / configured bot) already exist there.
3. Reply through `telegram/MessageSender::SendMarkdown` — never a raw
   `sendMessage` (that path owns the 4096 split, link-preview disabling
   and FloodWait retry).
4. Enumeration or lookup needs go through `TelegramClient::Send` with
   one outstanding request at a time and a `ResumeTimer` FloodWait pause
   — copy `DialogScanner`. `ChatStateStore`'s placeholder URL resolution
   is the deliberate counter-example: it never retries, to avoid flood
   storms — keep that in mind when you add retries to bulk work.
5. Test in `tests/test_control_commands.cpp` (fixtures and fake sender
   already exist there).

### Scenario 3 — a new gRPC RPC

1. New `proto/<service>.proto` — keep `proto/image_stream.proto`
   verbatim (it is the server's contract). Copy the existing `protoc`
   custom command in `CMakeLists.txt` for codegen.
2. `src/transport/<X>Client.h` — a stub owner that only produces the
   `StartFn` its streamer uses; copy `SearchImagesClient`. Reply handling
   lives in `Application`, never in the client wrapper.
3. Instantiate `transport/AsyncStreamer` (template — use it, don't
   modify it), one per lane, exposed through `GrpcTransport` next to the
   existing pools. Reuse each lane's existing Channel — never create
   your own channel, or you lose the one-TCP-per-lane guarantee (see
   `GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL` in `MakeChannelArgs`). New
   streams inherit the lane channel args (keepalive, 64 MiB caps,
   window) automatically.
4. Decide replay semantics up front: at-least-once ⇒ `Outbox.Put`
   BEFORE `Enqueue`, write-ok ⇒ `MarkSent`, ack ⇒ `MarkDone` and
   release the flow-control slot only when `MarkDone` returns true
   (the ProcessImages pattern; see [Outbox state](#outbox-state)); no
   replay ⇒ say so in the header and accept loss on a dead lane (the
   SearchImages pattern — its correlation table is memory-only, and it
   does not take a flow-control slot).
5. Test against the in-process fake server: `tests/test_async_streamer.cpp`
   for the CQ state machine, `tests/test_e2e_fake_server.cpp` for the
   full pipeline (both use a Unix domain socket — see the note in
   Test).

### Adding a config item (all scenarios)

`src/app/Config.h` (struct field + default) → `src/app/Config.cpp`
(parse) → `config/config.example.yaml` (commented doc, Chinese) →
`tests/test_config.cpp` (sample-file assertion + defaults assertion).
Keys are immutable once shipped — grep `src/app/Config.cpp` first so a
new key doesn't collide with an existing name, and never rename one
(configs in the field would silently fall back to defaults).

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

26 test binaries, registered in `tests/CMakeLists.txt`:

| test | covers |
|---|---|
| test_idempotency | image_id formation (`MakeImageId`/`MakeWireImageId`), bare-ID conversions |
| test_config | config parsing, env expansion (`keyme` included) |
| test_memo_cipher | chunk 1: Python-compatible caption cleaning (case by case), cross-language AES-GCM vectors in BOTH directions (Python ciphertext decrypted here; fixed-nonce C++ ciphertext byte-equal to Python's), round-trips incl. every base64 padding length, tamper/wrong-key rejection, a DISABLED live token dump for re-running the C++→Python check |
| test_chat_state | chunk 1: `{"cid","mid"}` parsing, watermark only increases, supergroup/basic/user URL forms, migrated group follows its supergroup, placeholder on failure with NO retry storm, single in-flight resolution |
| test_media_extractor | photo largest-tier (full image), tier tie-break, file-less tier skip, video/animation thumbnail-only, document `image/*`/`video/*`/reject |
| test_outbox | `kPending` → `kDone` / retry / `kDeadLetter` / replay; `MarkSent` leaves the retry set until the deadline; `MarkDone` on `kSent`; reopen turns `kSent` into `kPending`; `MarkFailed` only from `kPending`; `PurgeDoneBefore` deletes old `kDone` rows only; legacy-id migration on reopen: numeric-middle `<chat>:<msg>:<uid>` rewritten to `str(message_id)` and kept pending, collision keeps the current-format row, converted rows dedupe among themselves, unparseable rows dead-letter, migration idempotent on every reopen |
| test_bot_command | only the configured bot's command accepted, arg parsing; wire request checks: `image_id = str(message_id)`, `memo` = KEYME-encrypted cleaned caption (decrypt-verified), `owner_id` = bare id; `SecondCommandCompletesWhileFirstDownloadPending` proves async message processing (B completes while A's download is still pending) |
| test_auto_search | allowlisted chat triggers Search only (disabled by default) |
| test_handler_registry | dispatcher registration / dispatch |
| test_async_streamer | CQ state machine vs in-process fake server: round-trip by image_id, bounded-queue backpressure, reconnect after server restart |
| test_e2e_fake_server | **chunk 3 acceptance test**: a local thumbnail file through the real pipeline (MediaThumbnailHandler/SearchHandler -> FileDownloader -> GrpcTransport -> AsyncStreamers -> fake ImageProcessor server); ProcessImages reply round-trips by `str(message_id)`, the memo arrives as KEYME ciphertext (decrypt-verified) and `owner_id` as the bare id, the Outbox record goes PENDING -> DONE; SearchImages reply round-trips by (oid, id) and resolves through the correlation table |
| test_tdlib_bootstrap | real TDLib client delivers the first authorization state (regression test for the missing bootstrap request) |
| test_auth_prompt | terminal prompts for the login code and 2FA password (env override, empty input fails fast) |
| test_update_logging | `telegram.log_updates` logs every update with type + content, silent when off |
| test_streamer_pool | 3-lane pool against a fake server: one stream per lane, 9 requests dealt 3 per lane, every reply round-trips |
| test_message_sender | chunk 2: 4096-char split (line boundaries kept intact, hard-split oversized lines, empty text), multi-message sends, errors logged not thrown; chunk 3: a 429'd chunk is retried until it lands (no duplicates), the cap drops it after initial + 3 re-issues, split chunks retry independently (flooded chunk doesn't block the others), no retry without a scheduler |
| test_chat_state_backend | chunk 2: `common/Gmt8` formats (fixed offset, UTC-midnight date roll, parse round-trip/garbage), SQLite backend upsert/load/reopen, flush→restore round trip (url + watermark), 45 s active window skips quiet chats, flushed fields match the Python `chathis` shape |
| test_history_backfill | chunk 2: gap replay through the realtime pipeline (oldest first, `str(message_id)`), watermark terminates collection, per-batch watermark re-read skips covered messages, FloodWait pauses and the resume timer continues, unknown peer dropped with a notice BEFORE any search, duplicate Start refused while a job is active, invalid bare ids refused, disabled engine refuses everything |
| test_dialog_scan | chunk 2: `loadChats`/`getChats` enumeration for all chat kinds, supergroup full-info audit findings, migrated groups listed as their supergroup, FloodWait retries the same step, enumeration failure aborts, unnamed/broken chats skipped, second scan refused while running; view-model wording/links match Python |
| test_control_commands | chunk 2: command classification, `/rsme` batching (users first, one line per dialog, empty input), `/rsor` clean + findings reports (capped), trigger directions (self-commands / bot), backfill start through the pipeline + refusals |
| test_media_channels | chunk 2: disabled by default never claims, only whitelisted chats and only media claimed, `rpc: process` runs the realtime pipeline, `rpc: search` queues a search request |
| test_flood_retry | chunk 3: `TdErrors` 429/retry-after parsing (code match, wire-text parse, unparseable → `RetryAfterOr` fallback); `FloodRetry` (fires after retry-after + pad, fallback delay when the text carries no number, attempt cap drops, Shutdown drops pending, Schedule after Shutdown refused, multiple pending all run); FileDownloader flood path (same-file retry lands once, cap gives up and fails the future, ride-along callbacks fail with the give-up, null scheduler keeps the old fail-fast, no double send while a retry is pending) |
| test_grpc_channel_args | chunk 3: `MakeChannelArgs` — keepalive trio, 64 MiB send/receive caps, HTTP/2 lookahead + write buffer, per-lane subchannel-pool flag; `0` keeps the gRPC default for that arg; config values reach the raw `grpc_channel_args` (read back via `c_channel_args()` — gRPC 1.51 has no getter API) |
| test_delete_file | deleteFile cache cleanup: one consumer downloads → `Release` sends `deleteFile` exactly once with the right file_id (and only after the bytes are delivered); shared file_id with the download in flight → both callbacks from ONE request, first `Release` deletes nothing, second deletes exactly once; failed download deletes nothing and the downloader stays usable; `Release` of an unknown file_id / double `Release` is a no-op; a `deleteFile` error response is swallowed (best-effort) |
| test_flow_control | ProcessImages window: `TryAcquire` → enqueue → write-ok `MarkSent` (slot stays held) → `ImageReply` `MarkDone` → `Release`; the send past `max_in_flight` stays queued until one reply frees a slot |
| test_application_flow_control | The same window through `Application`'s real `ImageReply` callback: a successful `MarkDone` releases the slot; a duplicate reply, a `kDeadLetter` row, and an unknown id do not |

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
5. Cloudflare D1 credentials (Hong, via Secure Vault): `CLOUDFLARE_ACCOUNT_ID`,
   `CLOUDFLARE_API_TOKEN`, `D1_DATABASE_ID` — for the chat-state backend swap
   (SQLite default works today; design in `docs/pending/state-persistence-and-d1.md`).
6. Restricted private channels: Python auto-left them during `/rsme`
   (LeaveChannelRequest — destructive); the C++ port only marks them ⚠️.
   Confirm whether to migrate the auto-leave or keep report-only.

## Layout (v5 §3.7)

```
CMakeLists.txt  CMakePresets.json  Dockerfile  README.md
proto/image_stream.proto            (verbatim, do not edit)
src/main.cpp
src/app/            Application, Config, AppContext, FlowControl
src/telegram/       TelegramClient, AuthManager, UpdateDispatcher,
                    UpdateHandler, FileDownloader, MessageSender (chunk 2),
                    TdErrors (chunk 2: 429/retry-after parsing),
                    FloodRetry (chunk 3: shared bounded 429 scheduler)
src/features/       bot_command/ (BotCommandHandler, CommandParser)
                    auto_search/ (AutoSearchHandler, disabled by default)
                    chat_state/ (ChatStateStore: {"cid","mid"} watermarks,
                    chat-URL resolution; ChatStateBackend +
                    SqliteChatStateBackend — chunk 2 persistence)
                    commands/ (ControlCommandsHandler, DialogScanner —
                    chunk 2: /rsme, /rsor, /xm family)
                    history_backfill/ (HistoryBackfill — chunk 2 engine
                    behind /xm & co)
                    media_channels/ (MediaChannelHandler — chunk 2
                    whitelist auto-forward, disabled by default)
                    media_thumbnail/ (MediaThumbnailHandler, MediaExtractor, SearchHandler)
src/transport/      GrpcTransport, AsyncStreamer (template),
                    StreamerPool (template), ProcessImagesClient,
                    SearchImagesClient
src/infra/          Outbox (SQLite + legacy-id migration, chunk 2),
                    Sha256, FileStore
src/common/         Logger (spdlog), Idempotency (bare-ID conversion,
                    MakeImageId/MakeWireImageId), MemoCipher (AES-256-GCM
                    memo, chunk 1), Gmt8 (chunk 2), ResumeTimer (chunk 2)
docs/pending/       state-persistence-and-d1.md (chunk 2, D1 design),
                    retry-design.md, PENDING_FEATURES.md (pending features)
tests/              GTest unit tests
config/             config.example.yaml
third_party/td/     TDLib shallow clone (commit 4c6377a; built 2026-11-08,
                    libtdcore.a/libtdclient.a — never rebuild, see Build)
```
