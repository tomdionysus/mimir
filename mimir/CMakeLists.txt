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
  command/acl.cpp
  command/acl_commands.cpp
  command/args.cpp
  command/bitmaps.cpp
  command/connection.cpp
  command/database.cpp
  command/generic.cpp
  command/geo.cpp
  command/hashes.cpp
  command/hyperloglog.cpp
  command/lists.cpp
  command/lua_struct.cpp
  command/pubsub.cpp
  command/pubsub_commands.cpp
  command/scripting.cpp
  command/server_commands.cpp
  command/sort.cpp
  command/sets.cpp
  command/stream_groups.cpp
  command/streams.cpp
  command/strings.cpp
  command/table.cpp
  command/transactions.cpp
  command/zsets.cpp
  keyspace/geo.cpp
  keyspace/glob.cpp
  keyspace/hash.cpp
  keyspace/hll.cpp
  keyspace/keyspace.cpp
  keyspace/rdb.cpp
  keyspace/set.cpp
  keyspace/slot.cpp
  keyspace/stream.cpp
  keyspace/zset.cpp
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
  PRIVATE mimir_warnings PkgConfig::LUA51 PkgConfig::LUA51_CJSON PkgConfig::LUA51_BITOP ${MIMIR_LUA51_CMSGPACK}
)
set_target_properties(mimir PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR})
