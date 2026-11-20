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
option(MIMIR_BUILD_CLIENT_TESTS "Build mimir_client_tests: hiredis against a real three-node cluster (ruling 44); needs libhiredis-dev" OFF)
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

include(GNUInstallDirs)

# Dannan, the network layer, at its pinned release. An installed release of
# that minor (libdannan-dev) is used first, but never by a sanitizer build;
# otherwise, unless MIMIR_FETCH_DANNAN is off (the package build), the tag is
# cloned into the build tree from the sibling checkout or a URL, and built
# with this build's flags, so building Mimir never touches ../dannan.
# A string, not a PATH: CMake makes a PATH absolute, and a URL (hosted CI's
# https://github.com/...) then becomes a directory under the source tree.
set(MIMIR_DANNAN_REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../dannan" CACHE STRING "Dannan's repository: a path or a URL")
option(MIMIR_FETCH_DANNAN "Fetch Dannan at its tag when no installed release of its minor is found" ON)
# Not a cache variable: a moved pin must take effect in a build tree that
# already exists.
set(MIMIR_DANNAN_TAG "0.9.2")
# A sanitizer build instruments every stack library it links, so it always
# builds Dannan from source with the same flags, never an installed copy.
set(MIMIR_SANITIZED OFF)
if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=")
  set(MIMIR_SANITIZED ON)
endif()
# Dannan's own switch for it (DANNAN_SANITIZE, from 0.10.0), named from this
# build's flags: address (with UBSan) or thread.
if(CMAKE_CXX_FLAGS MATCHES "-fsanitize=[^ ]*thread")
  set(DANNAN_SANITIZE thread)
elseif(CMAKE_CXX_FLAGS MATCHES "-fsanitize=[^ ]*address")
  set(DANNAN_SANITIZE address)
endif()
if(NOT TARGET dannan::dannan AND NOT MIMIR_SANITIZED)
  if(MIMIR_FETCH_DANNAN)
    find_package(dannan ${MIMIR_DANNAN_TAG} CONFIG QUIET)
  else()
    find_package(dannan ${MIMIR_DANNAN_TAG} CONFIG REQUIRED)
  endif()
endif()
if(NOT TARGET dannan::dannan)
  include(FetchContent)
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

# What a package installs beside /usr/bin/mimir-node: the settings (a
# conffile, /etc/mimir when the prefix is /usr), the systemd unit and the
# manual page.
install(FILES packaging/config.yaml DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/mimir)
install(FILES packaging/mimir.service DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/systemd/system)
install(FILES docs/man/mimir-node.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)

if(MIMIR_BUILD_TESTING)
  find_package(GTest 1.14 CONFIG REQUIRED)
  add_subdirectory(mimir/test)
endif()

# A real client, hiredis, against real mimir-node processes: a test-only
# dependency, never linked into mimir-node (ruling 44).
if(MIMIR_BUILD_CLIENT_TESTS)
  find_package(GTest 1.14 CONFIG REQUIRED)
  pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
  add_subdirectory(mimir/test/client)
endif()

if(MIMIR_BUILD_FUZZING)
  add_subdirectory(mimir/fuzz)
endif()
