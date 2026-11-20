//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

// Scripting, on Debian's Lua 5.1 (ruling 20): two interpreters, one for EVAL's
// scripts and one for FUNCTION's libraries, as Valkey keeps them, so flushing
// one leaves the other. Both are used only under the database's lock, so a
// script or a function runs whole with no other command between its own.
// Scripts are cached by the SHA-1 of their body; libraries by their name.
namespace mimir::command {

// What a script reads of the server: redis.REDIS_VERSION and its kin.
struct ScriptingNames {
  std::string server_name = "mimir";
  std::string valkey_version = "8.1.1";
  std::string redis_version = "7.2.4";
};

class Scripting {
 public:
  struct Function {
    std::string name;
    std::optional<std::string> description;
    std::vector<std::string> flags;
    bool has_flag(std::string_view flag) const;
  };
  struct Library {
    std::string name;
    std::string code;
    std::vector<Function> functions;
  };

  explicit Scripting(ScriptingNames names = {});
  ~Scripting();
  Scripting(const Scripting&) = delete;
  Scripting& operator=(const Scripting&) = delete;

  const ScriptingNames& names() const noexcept { return _names; }

  // EVAL's interpreter and cache.
  lua_State* state() noexcept { return _lua; }
  // The body cached under `sha`, or null.
  const std::string* body(const std::string& sha) const;
  // Caches `body`; its SHA-1 in hex.
  std::string load(const std::string& body);
  void flush();

  // FUNCTION's interpreter and libraries. A loaded function's callback is in
  // the interpreter's registry under "fn_" and its name.
  lua_State* functions_state() noexcept { return _functions_lua; }
  std::map<std::string, Library>& libraries() noexcept { return _libraries; }
  // Each function's library, by the function's name.
  std::map<std::string, std::string>& function_index() noexcept { return _function_index; }
  // The function named `name` and its library, or null.
  const Function* find_function(const std::string& name) const;
  void flush_functions();
  // Every library's code, by name: FUNCTION DUMP's libraries, and in a
  // cluster the libraries' record (ruling 39).
  std::vector<std::string> library_codes() const;
  // The libraries replaced by those `codes` define, all or none (the error
  // when one does not load).
  std::optional<std::string> replace_libraries(const std::vector<std::string>& codes);

 private:
  ScriptingNames _names;
  lua_State* _lua = nullptr;
  std::map<std::string, std::string> _bodies;
  lua_State* _functions_lua = nullptr;
  std::map<std::string, Library> _libraries;
  std::map<std::string, std::string> _function_index;
};

std::string sha1_hex(std::string_view bytes);

}  // namespace mimir::command
