#
# Mimir - The Distributed Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

add_executable(mimir_tests
  command_bitmaps_test.cpp
  command_blocking_test.cpp
  command_generic_test.cpp
  command_hashes_test.cpp
  command_hyperloglog_test.cpp
  command_lists_test.cpp
  command_pubsub_test.cpp
  command_sets_test.cpp
  command_streams_test.cpp
  command_strings_test.cpp
  command_transactions_test.cpp
  command_zsets_test.cpp
  differential_test.cpp
  keyspace_keyspace_test.cpp
  keyspace_slot_test.cpp
  resp_parser_test.cpp
  resp_reply_test.cpp
  server_test.cpp
  version_test.cpp
)
target_link_libraries(mimir_tests PRIVATE mimir GTest::gtest_main mimir_warnings)
# Dannan's fixtures write only inside Mimir's build tree.
target_compile_definitions(mimir_tests PRIVATE DANNAN_TEST_TEMP_ROOT="${PROJECT_BINARY_DIR}/test-tmp")
