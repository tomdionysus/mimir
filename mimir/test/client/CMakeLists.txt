#
# Mimir - The Distributed Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# Its own binary, so mimir_tests stays self-contained: these start processes
# and need hiredis.
add_executable(mimir_client_tests client_test.cpp)
add_dependencies(mimir_client_tests mimir-node)
# Dannan for its test fixtures' headers; libutil for ChildProcess's pseudo-terminal.
target_link_libraries(mimir_client_tests PRIVATE dannan::dannan PkgConfig::HIREDIS GTest::gtest_main mimir_warnings)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(mimir_client_tests PRIVATE util)
endif()
target_include_directories(mimir_client_tests PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_definitions(mimir_client_tests PRIVATE DANNAN_TEST_TEMP_ROOT="${PROJECT_BINARY_DIR}/test-tmp" MIMIR_NODE_BINARY="$<TARGET_FILE:mimir-node>")
