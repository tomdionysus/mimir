#
# Mimir - The Distributed Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# One libFuzzer target per parser of untrusted bytes. Clang only.
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "MIMIR_BUILD_FUZZING needs clang")
endif()

# AddressSanitizer cannot start on a kernel with a 39-bit address space (the
# build servers'); there, "fuzzer,undefined".
set(MIMIR_FUZZ_SANITIZERS "fuzzer,address,undefined" CACHE STRING "The -fsanitize= list the fuzz targets build with")

foreach(target resp_request resp_reply rdb_restore)
  add_executable(${target}_fuzz ${target}_fuzz.cpp)
  target_compile_options(${target}_fuzz PRIVATE -fsanitize=${MIMIR_FUZZ_SANITIZERS})
  target_link_options(${target}_fuzz PRIVATE -fsanitize=${MIMIR_FUZZ_SANITIZERS})
  target_link_libraries(${target}_fuzz PRIVATE mimir mimir_warnings)
endforeach()
