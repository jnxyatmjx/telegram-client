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
