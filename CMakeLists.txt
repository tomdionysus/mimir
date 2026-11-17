#
# Mimir - The Distributed Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

cmake_minimum_required(VERSION 3.24)

project(mimir VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Executables at the top of the build tree: ./build/mimir_tests.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(MIMIR_BUILD_TESTING "Build mimir_tests (GoogleTest)" OFF)
option(MIMIR_BUILD_FUZZING "Build the libFuzzer targets in mimir/fuzz; needs a clang that ships libFuzzer" OFF)
option(MIMIR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

find_package(Boost 1.83 CONFIG REQUIRED COMPONENTS headers)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto SSL)
find_package(Threads REQUIRED)
# Scripting's interpreter: Debian's Lua 5.1 (ruling 20), the version Valkey's
# scripts are written for.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LUA51 REQUIRED IMPORTED_TARGET lua5.1)
# The libraries Valkey bundles for scripts (ruling 25): Debian's cjson and
# bitop through pkg-config; cmsgpack ships only its runtime library, linked
# by name.
pkg_check_modules(LUA51_CJSON REQUIRED IMPORTED_TARGET lua5.1-cjson)
pkg_check_modules(LUA51_BITOP REQUIRED IMPORTED_TARGET lua5.1-bitop)
find_library(MIMIR_LUA51_CMSGPACK NAMES lua5.1-cmsgpack liblua5.1-cmsgpack.so.0 REQUIRED)

# Dannan, the network layer, at its tag, cloned from the sibling checkout into
# the build tree, so building Mimir never touches ../dannan.
# A string, not a PATH: CMake makes a PATH absolute, and a URL (hosted CI's
# https://github.com/...) then becomes a directory under the source tree.
set(MIMIR_DANNAN_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../dannan" CACHE STRING "Dannan's repository: a path or a URL")
# Not a cache variable: a moved pin must take effect in a build tree that
# already exists.
set(MIMIR_DANNAN_TAG "0.9.1")
include(FetchContent)
if(NOT TARGET dannan::dannan)
  FetchContent_Declare(dannan GIT_REPOSITORY "${MIMIR_DANNAN_REPOSITORY}" GIT_TAG "${MIMIR_DANNAN_TAG}")
  set(DANNAN_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(dannan)
endif()

add_library(mimir_warnings INTERFACE)
target_compile_options(mimir_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_options(mimir_warnings INTERFACE -Wthread-safety)
endif()
if(MIMIR_WARNINGS_AS_ERRORS)
  target_compile_options(mimir_warnings INTERFACE -Werror)
endif()

configure_file(mimir/include/mimir/version.h.in ${PROJECT_BINARY_DIR}/generated/mimir/version.h @ONLY)

add_subdirectory(mimir)
add_subdirectory(tools)

if(MIMIR_BUILD_TESTING)
  find_package(GTest 1.14 CONFIG REQUIRED)
  add_subdirectory(mimir/test)
endif()

if(MIMIR_BUILD_FUZZING)
  add_subdirectory(mimir/fuzz)
endif()
