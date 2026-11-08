# Unit tests (chunk 2). The fake-server END-TO-END pipeline test lands in
# chunk 3; test_async_streamer.cpp here is the in-process fake-server test of
# the CQ state machine required by IMPLEMENTATION_TASK.md item 6.
find_package(GTest CONFIG REQUIRED)

set(TG_TEST_SOURCES
  test_idempotency.cpp
  test_config.cpp
  test_media_extractor.cpp
  test_outbox.cpp
  test_bot_command.cpp
  test_auto_search.cpp
  test_handler_registry.cpp
  test_async_streamer.cpp
  test_e2e_fake_server.cpp)

foreach(src IN LISTS TG_TEST_SOURCES)
  get_filename_component(test_name "${src}" NAME_WE)
  add_executable(${test_name} ${src})
  target_compile_options(${test_name} PRIVATE -Wall -Wextra)
  target_link_libraries(${test_name} PRIVATE tg_core GTest::gtest_main)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# The streamer test opens real sockets and waits on reconnects.
set_property(TEST test_async_streamer PROPERTY TIMEOUT 120)
# The e2e test opens real sockets (fake server + two streams).
set_property(TEST test_e2e_fake_server PROPERTY TIMEOUT 120)
