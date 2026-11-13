# Unit tests (chunk 2). The fake-server END-TO-END pipeline test lands in
# chunk 3; test_async_streamer.cpp here is the in-process fake-server test of
# the CQ state machine required by IMPLEMENTATION_TASK.md item 6.
find_package(GTest CONFIG REQUIRED)

set(TG_TEST_SOURCES
  test_idempotency.cpp
  test_config.cpp
  test_memo_cipher.cpp
  test_chat_state.cpp
  test_media_extractor.cpp
  test_outbox.cpp
  test_bot_command.cpp
  test_auto_search.cpp
  test_search_reply.cpp
  test_handler_registry.cpp
  test_async_streamer.cpp
  test_e2e_fake_server.cpp
  test_tdlib_bootstrap.cpp
  test_auth_prompt.cpp
  test_update_logging.cpp
  test_streamer_pool.cpp
  test_message_sender.cpp
  test_chat_state_backend.cpp
  test_history_backfill.cpp
  test_dialog_scan.cpp
  test_control_commands.cpp
  test_media_channels.cpp
  test_flood_retry.cpp
  test_grpc_channel_args.cpp
  test_delete_file.cpp
  test_flow_control.cpp
  test_application_flow_control.cpp)

foreach(src IN LISTS TG_TEST_SOURCES)
  get_filename_component(test_name "${src}" NAME_WE)
  add_executable(${test_name} ${src})
  target_compile_options(${test_name} PRIVATE -Wall -Wextra)
  target_link_libraries(${test_name} PRIVATE tg_core GTest::gtest_main)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# test_search_reply calls td::parse_markdown_v2 (td/telegram/MessageEntity.h).
# tdutils headers are not on Td::TdStatic's public include path.
if(TG_HAVE_TDLIB AND TARGET test_search_reply)
  target_include_directories(test_search_reply PRIVATE
    "${CMAKE_SOURCE_DIR}/third_party/td/tdutils"
    "${CMAKE_BINARY_DIR}/third_party/td/tdutils")
endif()

# The streamer test opens real sockets and waits on reconnects.
set_property(TEST test_async_streamer PROPERTY TIMEOUT 120)
# The e2e test opens real sockets (fake server + two streams).
set_property(TEST test_e2e_fake_server PROPERTY TIMEOUT 120)
# The flow-control test opens a real socket (gated fake echo server).
set_property(TEST test_flow_control PROPERTY TIMEOUT 120)
# Drives Application's real ImageReply callback against a gated fake server.
set_property(TEST test_application_flow_control PROPERTY TIMEOUT 120)
# The bootstrap test creates a real TDLib client and waits for its first
# authorization-state update (no network, no real credentials).
set_property(TEST test_tdlib_bootstrap PROPERTY TIMEOUT 60)
# The auth-prompt test drives the real AuthManager with injected streams.
set_property(TEST test_auth_prompt PROPERTY TIMEOUT 60)
# The update-logging test creates a real TDLib client and captures its logs.
set_property(TEST test_update_logging PROPERTY TIMEOUT 60)
# The pool test opens 3 parallel streams against a fake server.
set_property(TEST test_streamer_pool PROPERTY TIMEOUT 120)
# The flood tests wait on real (zero/one-second) backoff timers.
set_property(TEST test_flood_retry PROPERTY TIMEOUT 60)
