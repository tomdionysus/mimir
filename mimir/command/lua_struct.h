//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

struct lua_State;

// The `struct` library Valkey's scripts may use (pack, unpack, size), which
// Debian does not package (ruling 25): written here to the behaviour of the
// struct library 0.2 that Valkey bundles. Leaves the library's table on the
// stack, as a Lua library's opener does.
namespace mimir::command {

int open_struct(lua_State* L);

}  // namespace mimir::command
