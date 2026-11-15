//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>

struct lua_State;

// Scripting, on Debian's Lua 5.1 (ruling 20): one interpreter per database,
// used only under the database's lock, so a script runs whole with no other
// command between its own, as Valkey's do. Scripts are cached by the SHA-1 of
// their body; globals are read-only, as Valkey 7 and later make them.
namespace mimir::command {

class Scripting {
 public:
  Scripting();
  ~Scripting();
  Scripting(const Scripting&) = delete;
  Scripting& operator=(const Scripting&) = delete;

  lua_State* state() noexcept { return _lua; }
  // The body cached under `sha`, or null.
  const std::string* body(const std::string& sha) const;
  // Caches `body`; its SHA-1 in hex.
  std::string load(const std::string& body);
  void flush();

 private:
  lua_State* _lua = nullptr;
  std::map<std::string, std::string> _bodies;
};

std::string sha1_hex(std::string_view bytes);

}  // namespace mimir::command
