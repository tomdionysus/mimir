#
# Mimir - The Distributed Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# The database. Public headers are mimir/include/mimir/*.h, included as
# "mimir/x.h"; everything else is included from the repository root as
# "mimir/<subsystem>/x.h".
add_library(mimir SHARED
  command/args.cpp
  command/connection.cpp
  command/database.cpp
  command/generic.cpp
  command/hashes.cpp
  command/lists.cpp
  command/sets.cpp
  command/strings.cpp
  command/table.cpp
  keyspace/glob.cpp
  keyspace/hash.cpp
  keyspace/keyspace.cpp
  keyspace/set.cpp
  keyspace/slot.cpp
  resp/parser.cpp
  resp/reply.cpp
  server/server.cpp
  version.cpp
)
target_include_directories(mimir
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}>
)
target_link_libraries(mimir
  PUBLIC dannan::dannan Boost::headers OpenSSL::SSL OpenSSL::Crypto Threads::Threads
  PRIVATE mimir_warnings
)
set_target_properties(mimir PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
