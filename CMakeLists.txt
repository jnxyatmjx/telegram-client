cmake_minimum_required(VERSION 3.18)

project(tg-client
  VERSION 0.1.0
  DESCRIPTION "Telegram client core (TDLib) with an async gRPC media transport"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

option(TG_ENABLE_TDLIB "Add third_party/td (TDLib) to the build and link Td::TdStatic" ON)
option(TG_BUILD_TESTS "Build unit tests" ON)
option(TG_ENABLE_CCACHE "Use ccache as a compile launcher when available" ON)

find_package(Threads REQUIRED)
# Ubuntu's protobuf 3.21 ships no protobuf-config.cmake: use module mode
# (CMake's builtin FindProtobuf, provides the protobuf::libprotobuf target).
find_package(Protobuf REQUIRED)
# gRPC: prefer the CMake config package (Ubuntu 24.04, Debian 13, ...).
# Debian 11's grpc 1.30.2 packages ship headers, libs and pkg-config files
# but no gRPCConfig.cmake, so fall back to pkg-config there. The link
# target is kept in TG_GRPCPP_TARGET so tg_proto links the same way in
# both cases. TG_GRPC_PKGCONFIG=ON forces the pkg-config path (also used
# to test it on systems that do have the config package).
option(TG_GRPC_PKGCONFIG "Find gRPC via pkg-config instead of the CMake config package" OFF)
if(NOT TG_GRPC_PKGCONFIG)
  find_package(gRPC CONFIG QUIET)
endif()
if(gRPC_FOUND)
  set(TG_GRPCPP_TARGET gRPC::grpc++)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
  set(TG_GRPCPP_TARGET PkgConfig::GRPCPP)
endif()
find_package(spdlog CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
# AES-256-GCM memo encryption (MemoCipher) and RAND_bytes come from OpenSSL.
find_package(OpenSSL REQUIRED)

find_program(PROTOC_EXECUTABLE NAMES protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN_EXECUTABLE NAMES grpc_cpp_plugin REQUIRED)

if(TG_ENABLE_CCACHE)
  find_program(CCACHE_EXECUTABLE ccache)
  if(CCACHE_EXECUTABLE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_EXECUTABLE}")
    message(STATUS "ccache enabled: ${CCACHE_EXECUTABLE}")
  endif()
endif()

# ---------------------------------------------------------------------------
# Generated protobuf / gRPC code.
#
# Generated into <build>/generated (kept out of the source tree). The proto has
# no `package` declaration, so the generated C++ symbols live in the global
# namespace: ImageProcessor, ImageRequest, ImageReply, SearchReq, SearchRsp.
# ---------------------------------------------------------------------------
set(TG_PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(TG_PROTO "${TG_PROTO_DIR}/image_stream.proto")
set(TG_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${TG_GENERATED_DIR}")

set(TG_PROTO_PB_H    "${TG_GENERATED_DIR}/image_stream.pb.h")
set(TG_PROTO_PB_CC   "${TG_GENERATED_DIR}/image_stream.pb.cc")
set(TG_PROTO_GRPC_H  "${TG_GENERATED_DIR}/image_stream.grpc.pb.h")
set(TG_PROTO_GRPC_CC "${TG_GENERATED_DIR}/image_stream.grpc.pb.cc")

add_custom_command(
  OUTPUT "${TG_PROTO_PB_H}" "${TG_PROTO_PB_CC}" "${TG_PROTO_GRPC_H}" "${TG_PROTO_GRPC_CC}"
  COMMAND "${PROTOC_EXECUTABLE}"
          "-I" "${TG_PROTO_DIR}"
          "--cpp_out=${TG_GENERATED_DIR}"
          "--grpc_out=${TG_GENERATED_DIR}"
          "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}"
          "${TG_PROTO}"
  DEPENDS "${TG_PROTO}"
  COMMENT "Generating C++ + gRPC code from proto/image_stream.proto"
  VERBATIM)

add_library(tg_proto STATIC "${TG_PROTO_PB_CC}" "${TG_PROTO_GRPC_CC}")
target_include_directories(tg_proto PUBLIC "${TG_GENERATED_DIR}")
target_link_libraries(tg_proto PUBLIC ${TG_GRPCPP_TARGET} protobuf::libprotobuf)

# ---------------------------------------------------------------------------
# Core library (skeleton: placeholder translation units, filled in by later
# chunks per ~/workspace/tdlib-grpc-architecture.md v5).
# ---------------------------------------------------------------------------
add_library(tg_core STATIC
  src/app/Application.cpp
  src/app/Config.cpp
  src/telegram/TelegramClient.cpp
  src/telegram/AuthManager.cpp
  src/telegram/UpdateDispatcher.cpp
  src/telegram/FileDownloader.cpp
  src/telegram/FloodRetry.cpp
  src/telegram/MessageSender.cpp
  src/telegram/TdErrors.cpp
  src/features/bot_command/BotCommandHandler.cpp
  src/features/bot_command/CommandParser.cpp
  src/features/auto_search/AutoSearchHandler.cpp
  src/features/auto_search/SearchReply.cpp
  src/features/chat_state/ChatStateStore.cpp
  src/features/chat_state/SqliteChatStateBackend.cpp
  src/features/commands/ControlCommandsHandler.cpp
  src/features/commands/DialogScanner.cpp
  src/features/history_backfill/HistoryBackfill.cpp
  src/features/media_channels/MediaChannelHandler.cpp
  src/features/media_thumbnail/MediaThumbnailHandler.cpp
  src/features/media_thumbnail/MediaExtractor.cpp
  src/features/media_thumbnail/SearchHandler.cpp
  src/transport/GrpcTransport.cpp
  src/transport/ProcessImagesClient.cpp
  src/transport/SearchImagesClient.cpp
  src/infra/Outbox.cpp
  src/infra/Sha256.cpp
  src/infra/FileStore.cpp
  src/common/Gmt8.cpp
  src/common/Idempotency.cpp
  src/common/MemoCipher.cpp
  src/common/ResumeTimer.cpp)

target_include_directories(tg_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(tg_core
  PUBLIC tg_proto Threads::Threads spdlog::spdlog SQLite::SQLite3
         OpenSSL::Crypto)
target_compile_options(tg_core PRIVATE -Wall -Wextra)

# TDLib is wired in here but not built by chunk 1. The build happens later;
# TG_ENABLE_TDLIB=OFF configures a skeleton-only build without it.
set(TG_HAVE_TDLIB OFF)
if(TG_ENABLE_TDLIB AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/td/CMakeLists.txt")
  set(TD_ENABLE_JNI OFF CACHE BOOL "" FORCE)
  set(TD_ENABLE_DOTNET OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/td EXCLUDE_FROM_ALL)
  # td_api.h is generated during TDLib's own build; skeleton placeholders do
  # not include it, so tg_core compiles before TDLib is built.
  target_link_libraries(tg_core PUBLIC Td::TdStatic)
  set(TG_HAVE_TDLIB ON)
endif()
message(STATUS "TDLib integration (Td::TdStatic): ${TG_HAVE_TDLIB}")

add_executable(tg-client src/main.cpp)
target_compile_options(tg-client PRIVATE -Wall -Wextra)
target_compile_definitions(tg-client PRIVATE TG_CLIENT_VERSION="0.1.0")
target_link_libraries(tg-client PRIVATE tg_core)

include(CTest)
if(BUILD_TESTING AND TG_BUILD_TESTS)
  add_subdirectory(tests)
endif()
