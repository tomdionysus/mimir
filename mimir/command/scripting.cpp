//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// EVAL, EVALSHA, EVAL_RO, EVALSHA_RO and SCRIPT, as Valkey 8 documents and
// answers them: a script's server.call (or redis.call) runs a command under
// the same hold of the lock, its reply converted to Lua and back by Valkey's
// rules, and an error carries the script's sha and line.

#include "mimir/command/scripting.h"

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <exception>
#include <utility>
#include <vector>

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"
#include "mimir/command/lua_struct.h"
#include "mimir/keyspace/glob.h"
#include "mimir/keyspace/rdb.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

// Debian's lua-cjson, lua-bitop and lua-cmsgpack (ruling 25); cmsgpack ships
// no header.
int luaopen_cjson(lua_State* L);
int luaopen_bit(lua_State* L);
int luaopen_cmsgpack(lua_State* L);
}

namespace mimir::command {

std::string sha1_hex(std::string_view bytes) {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  EVP_Digest(bytes.data(), bytes.size(), digest, &length, EVP_sha1(), nullptr);
  std::string out;
  char hex[3];
  for (unsigned int i = 0; i < length; ++i) {
    std::snprintf(hex, sizeof(hex), "%02x", digest[i]);
    out += hex;
  }
  return out;
}

namespace {

// One script's or function's run, reached from the Lua side through the
// registry: what its errors name (the script's sha or the function's name)
// and the chunk its lines are in ("user_script" or "user_function").
struct Run {
  Call* outer;
  Session session;
  bool read_only;
  std::string name;
  const char* chunk;
};

constexpr const char* run_key = "mimir.run";

Run& current(lua_State* L) {
  lua_getfield(L, LUA_REGISTRYINDEX, run_key);
  auto* run = static_cast<Run*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return *run;
}

// Pushes {err=message}: a server error as Lua holds it.
void push_error(lua_State* L, std::string_view message) {
  lua_newtable(L);
  lua_pushstring(L, "err");
  lua_pushlstring(L, message.data(), message.size());
  lua_settable(L, -3);
}

// Lua 5.1 raises by longjmp, which must cross no live C++ object: a function
// that raises pushes its error inside a scope and raises after it closes.

// One reply, parsed from the bytes Mimir itself wrote, pushed as Lua sees it
// in RESP2: integers as numbers, bulk strings as strings, nulls as false,
// status as {ok=}, errors as {err=}, arrays as tables.
void push_reply(lua_State* L, std::string_view bytes, size_t& at) {
  const size_t end = bytes.find("\r\n", at);
  const char type = bytes[at];
  const std::string_view line = bytes.substr(at + 1, end - at - 1);
  at = end + 2;
  switch (type) {
    case ':':
      lua_pushnumber(L, static_cast<lua_Number>(std::stoll(std::string(line))));
      return;
    case '+':
    case '-':
      lua_newtable(L);
      lua_pushstring(L, type == '+' ? "ok" : "err");
      lua_pushlstring(L, line.data(), line.size());
      lua_settable(L, -3);
      return;
    case '$': {
      const long long n = std::stoll(std::string(line));
      if (n < 0) {
        lua_pushboolean(L, 0);
        return;
      }
      lua_pushlstring(L, bytes.data() + at, static_cast<size_t>(n));
      at += static_cast<size_t>(n) + 2;
      return;
    }
    case '*': {
      const long long n = std::stoll(std::string(line));
      if (n < 0) {
        lua_pushboolean(L, 0);
        return;
      }
      lua_createtable(L, static_cast<int>(n), 0);
      for (long long i = 1; i <= n; ++i) {
        push_reply(L, bytes, at);
        lua_rawseti(L, -2, static_cast<int>(i));
      }
      return;
    }
    default:
      lua_pushboolean(L, 0);
      return;
  }
}

// The value at `index` as a reply, by Valkey's rules.
void write_reply(lua_State* L, int index, resp::Reply& reply) {
  switch (lua_type(L, index)) {
    case LUA_TNUMBER:
      reply.integer(static_cast<int64_t>(lua_tonumber(L, index)));
      return;
    case LUA_TSTRING: {
      size_t n = 0;
      const char* s = lua_tolstring(L, index, &n);
      reply.bulk(std::string_view(s, n));
      return;
    }
    case LUA_TBOOLEAN:
      if (lua_toboolean(L, index))
        reply.integer(1);
      else
        reply.null();
      return;
    case LUA_TTABLE: {
      const int t = index < 0 ? lua_gettop(L) + index + 1 : index;
      lua_getfield(L, t, "err");
      if (lua_isstring(L, -1)) {
        std::string message = lua_tostring(L, -1);
        lua_pop(L, 1);
        if (!message.empty() && message[0] == '-') message.erase(0, 1);
        reply.error(message);
        return;
      }
      lua_pop(L, 1);
      lua_getfield(L, t, "ok");
      if (lua_isstring(L, -1)) {
        reply.simple(lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
      }
      lua_pop(L, 1);
      // An array: up to the first nil.
      int count = 0;
      for (;;) {
        lua_rawgeti(L, t, count + 1);
        const bool nil = lua_isnil(L, -1);
        lua_pop(L, 1);
        if (nil) break;
        ++count;
      }
      reply.array(static_cast<size_t>(count));
      for (int i = 1; i <= count; ++i) {
        lua_rawgeti(L, t, i);
        write_reply(L, -1, reply);
        lua_pop(L, 1);
      }
      return;
    }
    default:
      reply.null();
      return;
  }
}

bool has_flag(std::string_view flags, std::string_view flag) {
  size_t at = 0;
  while (at < flags.size()) {
    size_t end = flags.find(' ', at);
    if (end == std::string_view::npos) end = flags.size();
    if (flags.substr(at, end - at) == flag) return true;
    at = end + 1;
  }
  return false;
}

// Runs one command for the script and pushes its reply; false with the error
// table pushed when the command failed or was refused.
bool call_command(lua_State* L, Run& run) {
  const int argc = lua_gettop(L);
  if (argc == 0) {
    push_error(L, "ERR Please specify at least one argument for this redis lib call");
    return false;
  }
  std::vector<std::string> args;
  for (int i = 1; i <= argc; ++i) {
    if (lua_type(L, i) != LUA_TSTRING && lua_type(L, i) != LUA_TNUMBER) {
      push_error(L, "ERR Lua redis lib command arguments must be strings or integers");
      return false;
    }
    size_t n = 0;
    const char* s = lua_tolstring(L, i, &n);
    args.emplace_back(s, n);
  }
  const Spec* spec = find_spec(args[0]);
  const char* refused = nullptr;
  if (!spec) {
    refused = "ERR Unknown command called from script";
  } else {
    const int count = static_cast<int>(args.size());
    if ((spec->arity > 0 && count != spec->arity) || (spec->arity < 0 && count < -spec->arity)) {
      refused = "ERR Wrong number of args calling command from script";
    } else if (has_flag(spec->flags, "noscript")) {
      refused = "ERR This Redis command is not allowed from script";
    } else if (run.read_only && has_flag(spec->flags, "write")) {
      refused = "ERR Write commands are not allowed from read-only scripts.";
    }
  }
  if (refused) {
    push_error(L, refused);
    return false;
  }
  // The script runs as its caller's user, each command checked as it is called.
  std::optional<std::string> denied = [&]() DANNAN_NO_THREAD_SAFETY_ANALYSIS {
    return run.outer->database.acl_refusal_locked(run.outer->session, args, "lua");
  }();
  if (denied) {
    push_error(L, "ERR ACL failure in script: " + *denied);
    return false;
  }
  resp::Reply reply(resp::Protocol::resp2);
  try {
    // Under the lock the script's own command holds.
    [&]() DANNAN_NO_THREAD_SAFETY_ANALYSIS { run.outer->database.run_locked(run.session, args, reply); }();
  } catch (const std::exception& e) {
    push_error(L, std::string("ERR ") + e.what());
    return false;
  }
  const std::string& bytes = reply.bytes();
  if (!bytes.empty() && bytes[0] == '-') {
    push_error(L, std::string_view(bytes).substr(1, bytes.size() - 3));
    return false;
  }
  size_t at = 0;
  push_reply(L, bytes, at);
  return true;
}

int call_generic(lua_State* L, bool protect) {
  const bool ok = call_command(L, current(L));
  if (!ok && !protect) return lua_error(L);
  return 1;
}

int lua_call_command(lua_State* L) { return call_generic(L, false); }
int lua_pcall_command(lua_State* L) { return call_generic(L, true); }

int lua_error_reply(lua_State* L) {
  std::string message = luaL_checkstring(L, 1);
  if (!message.empty() && message[0] == '-') message.erase(0, 1);
  // An error without an upper-case code gets ERR, as Valkey gives it.
  const size_t space = message.find(' ');
  const std::string code = message.substr(0, space);
  bool upper_code = !code.empty();
  for (char c : code) upper_code = upper_code && (std::isupper(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_');
  if (!upper_code || space == std::string::npos) message = "ERR " + message;
  lua_newtable(L);
  lua_pushstring(L, "err");
  lua_pushstring(L, message.c_str());
  lua_settable(L, -3);
  return 1;
}

int lua_status_reply(lua_State* L) {
  lua_newtable(L);
  lua_pushstring(L, "ok");
  lua_pushvalue(L, 1);
  lua_settable(L, -3);
  return 1;
}

int lua_sha1hex(lua_State* L) {
  size_t n = 0;
  const char* s = luaL_checklstring(L, 1, &n);
  const std::string hex = sha1_hex(std::string_view(s, n));
  lua_pushstring(L, hex.c_str());
  return 1;
}

int lua_noop(lua_State*) { return 0; }
int lua_true(lua_State* L) {
  lua_pushboolean(L, 1);
  return 1;
}

// Scripts here always see RESP2 replies; asking for 3 is accepted and answers
// alike, a difference left for the differential gate to name.
int lua_setresp(lua_State* L) {
  const lua_Number version = luaL_checknumber(L, 1);
  if (version != 2 && version != 3) {
    push_error(L, "ERR RESP version must be 2 or 3.");
    return lua_error(L);
  }
  return 0;
}

int lua_os_clock(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>(std::clock()) / CLOCKS_PER_SEC);
  return 1;
}

// The read-only sandbox, as Valkey 7 and later have it. Valkey marks tables
// read-only in a patched Lua; on Debian's Lua each protected table is a proxy:
// an empty table whose metatable reads through to the real one and refuses
// every write. A script's globals are such a proxy over the real globals,
// which refuses a read of a global that does not exist too. rawset and
// setmetatable are wrapped to refuse a proxy, and the proxies' metatables are
// hidden. Scripts see the proxies only; KEYS and ARGV are written to the real
// globals beneath.
constexpr const char* protected_key = "mimir.protected";
constexpr const char* env_key = "mimir.env";

int readonly_newindex(lua_State* L) {
  luaL_where(L, 1);
  lua_pushstring(L, "Attempt to modify a readonly table");
  lua_concat(L, 2);
  return lua_error(L);
}

int global_index(lua_State* L) {
  lua_pushvalue(L, 2);
  lua_rawget(L, lua_upvalueindex(1));
  if (!lua_isnil(L, -1)) return 1;
  luaL_where(L, 1);
  lua_pushfstring(L, "Script attempted to access nonexistent global variable '%s'", lua_isstring(L, 2) ? lua_tostring(L, 2) : "?");
  lua_concat(L, 2);
  return lua_error(L);
}

bool is_protected(lua_State* L, int index) {
  if (!lua_istable(L, index)) return false;
  lua_getfield(L, LUA_REGISTRYINDEX, protected_key);
  lua_pushvalue(L, index);
  lua_rawget(L, -2);
  const bool found = lua_toboolean(L, -1) != 0;
  lua_pop(L, 2);
  return found;
}

int refuse_readonly(lua_State* L) {
  lua_pushstring(L, "Attempt to modify a readonly table");
  return lua_error(L);
}

int safe_rawset(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checkany(L, 2);
  luaL_checkany(L, 3);
  if (is_protected(L, 1)) return refuse_readonly(L);
  lua_settop(L, 3);
  lua_rawset(L, 1);
  return 1;
}

void unwrap(lua_State* L, int index);

// rawget on a proxy reads the table beneath it, as rawget on Valkey's
// read-only tables does.
int safe_rawget(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_checkany(L, 2);
  lua_settop(L, 2);
  unwrap(L, 1);
  lua_rawget(L, 1);
  return 1;
}

// Replaces the proxy at stack position `index` (positive) with the table
// beneath it, in place: its metatable's __index, or the upvalue of the
// globals' __index function.
void unwrap(lua_State* L, int index) {
  if (!is_protected(L, index)) return;
  const int top = lua_gettop(L);
  lua_getmetatable(L, index);
  lua_getfield(L, -1, "__index");
  if (lua_isfunction(L, -1)) lua_getupvalue(L, -1, 1);
  lua_replace(L, index);
  lua_settop(L, top);
}

// next, pairs and ipairs walk the table beneath a proxy; the script is handed
// the proxy back as the iteration's state, never the table beneath.
int safe_next(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 2);
  unwrap(L, 1);
  if (lua_next(L, 1)) return 2;
  lua_pushnil(L);
  return 1;
}

int safe_pairs(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_pushcfunction(L, safe_next);
  lua_pushvalue(L, 1);
  lua_pushnil(L);
  return 3;
}

int safe_inext(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  const int i = luaL_checkint(L, 2) + 1;
  lua_settop(L, 1);
  unwrap(L, 1);
  lua_pushinteger(L, i);
  lua_rawgeti(L, 1, i);
  return lua_isnil(L, -1) ? 0 : 2;
}

int safe_ipairs(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_pushcfunction(L, safe_inext);
  lua_pushvalue(L, 1);
  lua_pushinteger(L, 0);
  return 3;
}

int safe_setmetatable(lua_State* L) {
  if (is_protected(L, 1)) return refuse_readonly(L);
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
  return lua_gettop(L);
}

// loadstring and load: what they compile runs on the script's globals.
int safe_load(lua_State* L) {
  const int argc = lua_gettop(L);
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  lua_call(L, argc, LUA_MULTRET);
  if (lua_isfunction(L, 1)) {
    lua_getfield(L, LUA_REGISTRYINDEX, env_key);
    lua_setfenv(L, 1);
  }
  return lua_gettop(L);
}

// Replaces the table at the top with a read-only proxy over it; `index`
// gives the proxy's __index when not the table itself. getmetatable on the
// proxy answers a read-only view of its metatable (a table, as Valkey's),
// which cannot be changed to undo the protection.
void make_proxy(lua_State* L, lua_CFunction index = nullptr, bool view = true) {
  const int real = lua_gettop(L);
  lua_newtable(L);  // the proxy
  lua_newtable(L);  // its metatable
  if (index) {
    lua_pushvalue(L, real);
    lua_pushcclosure(L, index, 1);
  } else {
    lua_pushvalue(L, real);
  }
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, readonly_newindex);
  lua_setfield(L, -2, "__newindex");
  if (view) {
    lua_pushvalue(L, -1);
    make_proxy(L, nullptr, false);
  } else {
    lua_pushboolean(L, 0);
  }
  lua_setfield(L, -2, "__metatable");
  lua_setmetatable(L, -2);
  lua_getfield(L, LUA_REGISTRYINDEX, protected_key);
  lua_pushvalue(L, -2);
  lua_pushboolean(L, 1);
  lua_rawset(L, -3);
  lua_pop(L, 1);
  lua_replace(L, real);
}

void wrap_global(lua_State* L, const char* name, lua_CFunction wrapper) {
  lua_getglobal(L, name);
  lua_pushcclosure(L, wrapper, 1);
  lua_setglobal(L, name);
}

// The script's line where an error arose.
int script_line(lua_State* L, std::string_view chunk) {
  lua_Debug ar;
  for (int level = 0; lua_getstack(L, level, &ar); ++level) {
    lua_getinfo(L, "Sl", &ar);
    const std::string_view source = ar.source;
    if (source.size() == chunk.size() + 1 && source[0] == '@' && source.substr(1) == chunk && ar.currentline > 0) return ar.currentline;
  }
  return 0;
}

// The message handler: makes the final error text, the script's sha (or the
// function's name) and line attached.
int on_error(lua_State* L) {
  Run& run = current(L);
  const int line = script_line(L, run.chunk);
  std::string message;
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "err");
    message = lua_isstring(L, -1) ? lua_tostring(L, -1) : "ERR unknown error";
    lua_pop(L, 1);
  } else {
    message = std::string("ERR ") + (lua_isstring(L, 1) ? lua_tostring(L, 1) : "unknown error");
  }
  message += " script: " + run.name + ", on @" + run.chunk + ":" + std::to_string(line) + ".";
  lua_pushstring(L, message.c_str());
  return 1;
}

// "8.1.1" as Valkey numbers it: 0x080101.
lua_Number version_number(const std::string& version) {
  unsigned major = 0;
  unsigned minor = 0;
  unsigned patch = 0;
  std::sscanf(version.c_str(), "%u.%u.%u", &major, &minor, &patch);
  return static_cast<lua_Number>((major << 16) | (minor << 8) | patch);
}

// What the server says of itself, on a redis table at the top.
void set_version_fields(lua_State* L, const ScriptingNames& names) {
  lua_pushstring(L, names.redis_version.c_str());
  lua_setfield(L, -2, "REDIS_VERSION");
  lua_pushnumber(L, version_number(names.redis_version));
  lua_setfield(L, -2, "REDIS_VERSION_NUM");
}

void install(lua_State* L, const ScriptingNames& names) {
  for (lua_CFunction open : {luaopen_base, luaopen_table, luaopen_string, luaopen_math}) {
    lua_pushcfunction(L, open);
    lua_call(L, 0, 0);
  }
  lua_settop(L, 0);
  // Valkey's sandbox: no files, no environments, no modules.
  for (const char* gone : {"dofile", "loadfile", "print", "getfenv", "setfenv", "newproxy", "module", "require"}) {
    lua_pushnil(L);
    lua_setglobal(L, gone);
  }
  // The libraries Valkey bundles (ruling 25).
  const std::pair<const char*, lua_CFunction> libraries[] = {
      {"cjson", luaopen_cjson}, {"bit", luaopen_bit}, {"cmsgpack", luaopen_cmsgpack}, {"struct", open_struct}};
  for (const auto& [name, open] : libraries) {
    lua_pushcfunction(L, open);
    lua_call(L, 0, 1);
    lua_setglobal(L, name);
  }
  lua_newtable(L);
  const luaL_Reg functions[] = {
      {"call", lua_call_command}, {"pcall", lua_pcall_command}, {"error_reply", lua_error_reply}, {"status_reply", lua_status_reply}, {"sha1hex", lua_sha1hex},
      {"log", lua_noop},          {"setresp", lua_setresp},     {"set_repl", lua_noop},           {"replicate_commands", lua_true},   {nullptr, nullptr}};
  for (const luaL_Reg* f = functions; f->name; ++f) {
    lua_pushcfunction(L, f->func);
    lua_setfield(L, -2, f->name);
  }
  int level = 0;
  for (const char* name : {"LOG_DEBUG", "LOG_VERBOSE", "LOG_NOTICE", "LOG_WARNING"}) {
    lua_pushnumber(L, level++);
    lua_setfield(L, -2, name);
  }
  level = 0;
  for (const char* name : {"REPL_NONE", "REPL_SLAVE", "REPL_REPLICA", "REPL_AOF", "REPL_ALL"}) {
    lua_pushnumber(L, level++);
    lua_setfield(L, -2, name);
  }
  set_version_fields(L, names);
  lua_pushstring(L, names.server_name.c_str());
  lua_setfield(L, -2, "SERVER_NAME");
  lua_pushstring(L, names.valkey_version.c_str());
  lua_setfield(L, -2, "VALKEY_VERSION");
  lua_pushnumber(L, version_number(names.valkey_version));
  lua_setfield(L, -2, "VALKEY_VERSION_NUM");
  lua_pushvalue(L, -1);
  lua_setglobal(L, "redis");
  lua_setglobal(L, "server");
  // os, with its clock alone.
  lua_newtable(L);
  lua_pushcfunction(L, lua_os_clock);
  lua_setfield(L, -2, "clock");
  lua_setglobal(L, "os");
  lua_pushcfunction(L, on_error);
  lua_setfield(L, LUA_REGISTRYINDEX, "mimir.on_error");
  wrap_global(L, "rawset", safe_rawset);
  wrap_global(L, "rawget", safe_rawget);
  wrap_global(L, "setmetatable", safe_setmetatable);
  wrap_global(L, "loadstring", safe_load);
  wrap_global(L, "load", safe_load);
  const std::pair<const char*, lua_CFunction> walkers[] = {{"next", safe_next}, {"pairs", safe_pairs}, {"ipairs", safe_ipairs}};
  for (const auto& [name, walker] : walkers) {
    lua_pushcfunction(L, walker);
    lua_setglobal(L, name);
  }
  // Every library read-only, the globals last.
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, protected_key);
  for (const char* name : {"string", "table", "math", "coroutine", "os", "cjson", "bit", "cmsgpack", "struct", "redis"}) {
    lua_getglobal(L, name);
    make_proxy(L);
    lua_setglobal(L, name);
  }
  lua_getglobal(L, "redis");
  lua_setglobal(L, "server");
  // Methods on strings read through the proxy, so string's identity holds.
  lua_pushstring(L, "");
  lua_getmetatable(L, -1);
  lua_getglobal(L, "string");
  lua_setfield(L, -2, "__index");
  lua_pop(L, 2);
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  make_proxy(L, global_index);
  lua_pushvalue(L, -1);
  lua_setglobal(L, "_G");
  lua_setfield(L, LUA_REGISTRYINDEX, env_key);
}

void set_array(lua_State* L, const char* name, const std::vector<std::string>& items) {
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  lua_pushstring(L, name);
  lua_createtable(L, static_cast<int>(items.size()), 0);
  for (size_t i = 0; i < items.size(); ++i) {
    lua_pushlstring(L, items[i].data(), items[i].size());
    lua_rawseti(L, -2, static_cast<int>(i + 1));
  }
  lua_rawset(L, -3);
  lua_pop(L, 1);
}

void run_script(Call& call, const std::string& sha, const std::string& body, size_t numkeys_at, bool read_only) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  auto numkeys = parse_integer(call.args[numkeys_at]);
  if (!numkeys) throw CommandError(not_integer);
  if (*numkeys < 0) throw CommandError("ERR Number of keys can't be negative");
  if (static_cast<uint64_t>(*numkeys) > call.args.size() - numkeys_at - 1) throw CommandError("ERR Number of keys can't be greater than number of args");
  Scripting& scripting = call.database.scripting_locked();
  lua_State* L = scripting.state();
  const std::string function = "f_" + sha;
  lua_getfield(L, LUA_REGISTRYINDEX, function.c_str());
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    if (luaL_loadbuffer(L, body.data(), body.size(), "@user_script") != 0) {
      std::string message = lua_tostring(L, -1);
      lua_pop(L, 1);
      throw CommandError("ERR Error compiling script (new function): " + message);
    }
    lua_getfield(L, LUA_REGISTRYINDEX, env_key);
    lua_setfenv(L, -2);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, function.c_str());
  }
  const size_t first = numkeys_at + 1;
  const size_t split = first + static_cast<size_t>(*numkeys);
  set_array(L, "KEYS",
            std::vector<std::string>(call.args.begin() + static_cast<std::ptrdiff_t>(first), call.args.begin() + static_cast<std::ptrdiff_t>(split)));
  set_array(L, "ARGV", std::vector<std::string>(call.args.begin() + static_cast<std::ptrdiff_t>(split), call.args.end()));
  Run run{&call, Session{}, read_only, sha, "user_script"};
  run.session.id = call.session.id;
  run.session.user = call.session.user;
  run.session.authenticated = run.session.welcomed = true;
  run.session.db = call.session.db;
  run.session.scripted = true;
  lua_pushlightuserdata(L, &run);
  lua_setfield(L, LUA_REGISTRYINDEX, run_key);
  // The handler under the function on the stack.
  lua_getfield(L, LUA_REGISTRYINDEX, "mimir.on_error");
  lua_insert(L, -2);
  const int handler = lua_gettop(L) - 1;
  const int status = lua_pcall(L, 0, 1, handler);
  if (status != 0) {
    std::string message = lua_isstring(L, -1) ? lua_tostring(L, -1) : "ERR script failed";
    lua_settop(L, 0);
    throw CommandError(message);
  }
  write_reply(L, -1, call.reply);
  lua_settop(L, 0);
}

// FUNCTION's libraries. A library's code runs once, at FUNCTION LOAD, on
// globals that hold only a narrow redis table (register_function, log, its
// levels, the version): Valkey's load-time sandbox, in which even `type` and
// `error` are missing. Once loaded, the library's globals become the runtime
// ones, so a helper it defined sees what its functions see.
constexpr const char* load_redis_key = "mimir.load_redis";
constexpr const char* loading_key = "mimir.loading";
constexpr const char* pending_key = "mimir.pending";

constexpr const char* function_flags[] = {"no-writes", "allow-oom", "allow-stale", "no-cluster", "allow-cross-slot-keys"};

bool valid_name(std::string_view name) {
  if (name.empty()) return false;
  for (char c : name) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
  }
  return true;
}

// What one FUNCTION LOAD has registered so far; the callbacks wait in the
// registry's pending table under their names.
struct LoadState {
  std::vector<Scripting::Function> functions;
  // An error FUNCTION LOAD answers as it is, not as a registration error.
  const char* direct_error = nullptr;
};

// server.register_function(name, callback), or with one table of named
// arguments (function_name, callback, description, flags). Raises only after
// its C++ objects are gone.
int register_function(lua_State* L) {
  const char* failure = nullptr;
  bool direct = false;
  lua_getfield(L, LUA_REGISTRYINDEX, loading_key);
  auto* load = static_cast<LoadState*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  // A library may keep register_function in a local and call it later.
  if (!load) {
    push_error(L, "ERR server.register_function can only be called on FUNCTION LOAD command");
    return lua_error(L);
  }
  const int argc = lua_gettop(L);
  // The four arguments' stack slots.
  int name = 0;
  int callback = 0;
  int description = 0;
  int flags = 0;
  if (argc == 1) {
    if (!lua_istable(L, 1)) {
      failure = "ERR calling server.register_function with a single argument is only applicable to Lua table (representing named arguments).";
    } else {
      for (int i = 0; i < 4; ++i) lua_pushnil(L);
      name = 2;
      callback = 3;
      description = 4;
      flags = 5;
      lua_pushnil(L);
      while (!failure && lua_next(L, 1)) {
        const char* key = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "";
        const std::string_view k = key;
        int slot = 0;
        if (k == "function_name") {
          if (!lua_isstring(L, -1)) failure = "ERR function_name argument given to server.register_function must be a string";
          slot = name;
        } else if (k == "callback") {
          if (!lua_isfunction(L, -1)) failure = "ERR callback argument given to server.register_function must be a function";
          slot = callback;
        } else if (k == "description") {
          if (!lua_isstring(L, -1)) failure = "ERR description argument given to server.register_function must be a string";
          slot = description;
        } else if (k == "flags") {
          if (!lua_istable(L, -1)) failure = "ERR flags argument to server.register_function must be a table representing function flags";
          slot = flags;
        } else {
          failure = "ERR unknown argument given to server.register_function";
        }
        if (failure) break;
        lua_replace(L, slot);
      }
      if (!failure && lua_isnil(L, name)) failure = "ERR server.register_function must get a function name argument";
      if (!failure && lua_isnil(L, callback)) failure = "ERR server.register_function must get a callback argument";
    }
  } else if (argc == 2) {
    name = 1;
    callback = 2;
    if (!lua_isstring(L, 1)) {
      failure = "ERR first argument to server.register_function must be a string";
    } else if (!lua_isfunction(L, 2)) {
      failure = "ERR second argument to server.register_function must be a function";
    }
  } else {
    failure = "ERR wrong number of arguments to server.register_function";
  }
  // The flags, each one Valkey knows.
  int flag_count = 0;
  if (!failure && flags && !lua_isnil(L, flags)) {
    lua_pushnil(L);
    while (!failure && lua_next(L, flags)) {
      bool known = false;
      if (lua_type(L, -1) == LUA_TSTRING) {
        for (const char* flag : function_flags) known = known || std::string_view(lua_tostring(L, -1)) == flag;
      }
      if (!known) failure = "ERR unknown flag given";
      lua_pop(L, 1);
      ++flag_count;
    }
  }
  if (!failure) {
    size_t length = 0;
    const char* text = lua_tolstring(L, name, &length);
    const std::string_view function_name(text, length);
    if (!valid_name(function_name)) {
      failure = "ERR Function names can only contain letters, numbers, or underscores(_) and must be at least one character long";
      direct = true;
    }
    for (const auto& known : load->functions) {
      if (!failure && known.name == function_name) {
        failure = "ERR Function already exists in the library";
        direct = true;
      }
    }
  }
  if (!failure) {
    {
      Scripting::Function function;
      function.name = lua_tostring(L, name);
      if (description && !lua_isnil(L, description)) function.description = lua_tostring(L, description);
      if (flags && !lua_isnil(L, flags)) {
        for (int i = 1; i <= flag_count; ++i) {
          lua_rawgeti(L, flags, i);
          if (lua_isstring(L, -1)) function.flags.emplace_back(lua_tostring(L, -1));
          lua_pop(L, 1);
        }
      }
      load->functions.push_back(std::move(function));
    }
    lua_getfield(L, LUA_REGISTRYINDEX, pending_key);
    lua_pushvalue(L, name);
    lua_pushvalue(L, callback);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    return 0;
  }
  if (direct) load->direct_error = failure;
  push_error(L, failure);
  return lua_error(L);
}

// The load-time redis table, read-only, refusing what it does not hold.
void install_load_redis(lua_State* L, const ScriptingNames& names) {
  lua_newtable(L);
  lua_pushcfunction(L, register_function);
  lua_setfield(L, -2, "register_function");
  lua_pushcfunction(L, lua_noop);
  lua_setfield(L, -2, "log");
  int level = 0;
  for (const char* name : {"LOG_DEBUG", "LOG_VERBOSE", "LOG_NOTICE", "LOG_WARNING"}) {
    lua_pushnumber(L, level++);
    lua_setfield(L, -2, name);
  }
  set_version_fields(L, names);
  make_proxy(L, global_index);
  lua_setfield(L, LUA_REGISTRYINDEX, load_redis_key);
}

// Removes a library's functions from the interpreter and the index.
void forget_functions(Scripting& scripting, const Scripting::Library& library) {
  lua_State* L = scripting.functions_state();
  for (const auto& function : library.functions) {
    scripting.function_index().erase(function.name);
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, ("fn_" + function.name).c_str());
  }
}

// FUNCTION LOAD [REPLACE] code: the library's name, or a refusal that
// changes nothing.
std::string load_library(Scripting& scripting, const std::vector<std::string>& args) {
  bool replace = false;
  for (size_t i = 2; i + 1 < args.size(); ++i) {
    if (!iequals(args[i], "REPLACE")) throw CommandError("ERR Unknown option given: " + args[i]);
    replace = true;
  }
  const std::string& code = args.back();
  if (code.rfind("#!", 0) != 0) throw CommandError("ERR Missing library metadata");
  const size_t newline = code.find('\n');
  if (newline == std::string::npos) throw CommandError("ERR Invalid library metadata");
  // "#!<engine> name=<name>": the engine up to the first space (empty if
  // none), then values with any spaces between. Checked in Valkey's order:
  // the values, the name given, the engine, the name's characters.
  const std::string shebang = code.substr(2, newline - 2);
  const size_t engine_end = std::min(shebang.find(' '), shebang.size());
  const std::string engine = shebang.substr(0, engine_end);
  std::optional<std::string> name;
  for (size_t at = engine_end; at < shebang.size();) {
    const size_t end = std::min(shebang.find(' ', at), shebang.size());
    if (end > at) {
      const std::string value = shebang.substr(at, end - at);
      if (value.rfind("name=", 0) != 0) throw CommandError("ERR Invalid metadata value given: " + value);
      if (name) throw CommandError("ERR Invalid metadata value, name argument was given multiple times");
      name = value.substr(5);
    }
    at = end + 1;
  }
  if (!name) throw CommandError("ERR Library name was not given");
  if (!iequals(engine, "lua")) throw CommandError("ERR Engine '" + engine + "' not found");
  if (!valid_name(*name)) throw CommandError("ERR Library names can only contain letters, numbers, or underscores(_) and must be at least one character long");
  auto existing = scripting.libraries().find(*name);
  if (existing != scripting.libraries().end() && !replace) throw CommandError("ERR Library '" + *name + "' already exists");

  lua_State* L = scripting.functions_state();
  lua_settop(L, 0);
  // The shebang's line stays, empty, so the library's lines count from it.
  const std::string body = code.substr(newline);
  if (luaL_loadbuffer(L, body.data(), body.size(), "@user_function") != 0) {
    std::string message = lua_tostring(L, -1);
    lua_settop(L, 0);
    throw CommandError("ERR Error compiling function: " + message);
  }
  // [1] the chunk, [2] its globals: redis and server, the load-time table.
  lua_newtable(L);
  lua_getfield(L, LUA_REGISTRYINDEX, load_redis_key);
  lua_pushvalue(L, -1);
  lua_setfield(L, -3, "redis");
  lua_setfield(L, -2, "server");
  make_proxy(L, global_index);
  lua_pushvalue(L, 2);
  lua_setfenv(L, 1);
  LoadState load;
  lua_pushlightuserdata(L, &load);
  lua_setfield(L, LUA_REGISTRYINDEX, loading_key);
  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, pending_key);
  lua_pushvalue(L, 1);
  const int status = lua_pcall(L, 0, 0, 0);
  lua_pushnil(L);
  lua_setfield(L, LUA_REGISTRYINDEX, loading_key);
  std::string refused;
  if (status != 0) {
    if (load.direct_error) {
      refused = load.direct_error;
    } else if (lua_istable(L, -1)) {
      lua_getfield(L, -1, "err");
      refused = std::string("ERR Error registering functions: ") + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "ERR unknown error");
    } else {
      refused = std::string("ERR Error registering functions: ERR ") + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "unknown error");
    }
  } else if (load.functions.empty()) {
    refused = "ERR No functions registered";
  } else {
    for (const auto& function : load.functions) {
      auto owner = scripting.function_index().find(function.name);
      if (refused.empty() && owner != scripting.function_index().end() && owner->second != *name) refused = "ERR Function " + function.name + " already exists";
    }
  }
  if (!refused.empty()) {
    lua_settop(L, 0);
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, pending_key);
    throw CommandError(refused);
  }
  // Committed: the old library's functions go, the new ones come in, and the
  // library's globals become the runtime ones.
  if (existing != scripting.libraries().end()) forget_functions(scripting, existing->second);
  lua_getfield(L, LUA_REGISTRYINDEX, pending_key);
  for (const auto& function : load.functions) {
    lua_getfield(L, -1, function.name.c_str());
    lua_setfield(L, LUA_REGISTRYINDEX, ("fn_" + function.name).c_str());
    scripting.function_index()[function.name] = *name;
  }
  lua_pop(L, 1);
  lua_pushnil(L);
  lua_setfield(L, LUA_REGISTRYINDEX, pending_key);
  lua_getmetatable(L, 2);
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  lua_pushcclosure(L, global_index, 1);
  lua_setfield(L, -2, "__index");
  lua_settop(L, 0);
  scripting.libraries()[*name] = Scripting::Library{*name, code, std::move(load.functions)};
  return *name;
}

void push_strings(lua_State* L, std::vector<std::string>::const_iterator begin, std::vector<std::string>::const_iterator end) {
  lua_createtable(L, static_cast<int>(end - begin), 0);
  int i = 0;
  for (auto at = begin; at != end; ++at) {
    lua_pushlstring(L, at->data(), at->size());
    lua_rawseti(L, -2, ++i);
  }
}

// FCALL and FCALL_RO: the function, given its keys and arguments.
void run_function(Call& call, bool read_only_command) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  Scripting& scripting = call.database.scripting_locked();
  const std::string& name = call.args[1];
  const Scripting::Function* function = scripting.find_function(name);
  if (!function) throw CommandError("ERR Function not found");
  auto numkeys = parse_integer(call.args[2]);
  if (!numkeys) throw CommandError("ERR Bad number of keys provided");
  if (*numkeys < 0) throw CommandError("ERR Number of keys can't be negative");
  if (static_cast<uint64_t>(*numkeys) > call.args.size() - 3) throw CommandError("ERR Number of keys can't be greater than number of args");
  const bool no_writes = function->has_flag("no-writes");
  if (read_only_command && !no_writes) throw CommandError("ERR Can not execute a script with write flag using *_ro command.");
  lua_State* L = scripting.functions_state();
  lua_settop(L, 0);
  Run run{&call, Session{}, no_writes, name, "user_function"};
  run.session.id = call.session.id;
  run.session.user = call.session.user;
  run.session.authenticated = run.session.welcomed = true;
  run.session.db = call.session.db;
  run.session.scripted = true;
  lua_pushlightuserdata(L, &run);
  lua_setfield(L, LUA_REGISTRYINDEX, run_key);
  lua_getfield(L, LUA_REGISTRYINDEX, "mimir.on_error");
  lua_getfield(L, LUA_REGISTRYINDEX, ("fn_" + name).c_str());
  const auto split = call.args.begin() + 3 + static_cast<std::ptrdiff_t>(*numkeys);
  push_strings(L, call.args.begin() + 3, split);
  push_strings(L, split, call.args.end());
  if (lua_pcall(L, 2, 1, 1) != 0) {
    std::string message = lua_isstring(L, -1) ? lua_tostring(L, -1) : "ERR function failed";
    lua_settop(L, 0);
    throw CommandError(message);
  }
  write_reply(L, -1, call.reply);
  lua_settop(L, 0);
}

void make_functions_state(lua_State*& L, const ScriptingNames& names) {
  L = luaL_newstate();
  install(L, names);
  install_load_redis(L, names);
}

}  // namespace

bool Scripting::Function::has_flag(std::string_view flag) const {
  for (const auto& held : flags) {
    if (held == flag) return true;
  }
  return false;
}

Scripting::Scripting(ScriptingNames names) : _names(std::move(names)) {
  _lua = luaL_newstate();
  install(_lua, _names);
  make_functions_state(_functions_lua, _names);
}

Scripting::~Scripting() {
  if (_lua) lua_close(_lua);
  if (_functions_lua) lua_close(_functions_lua);
}

const Scripting::Function* Scripting::find_function(const std::string& name) const {
  auto owner = _function_index.find(name);
  if (owner == _function_index.end()) return nullptr;
  for (const auto& function : _libraries.at(owner->second).functions) {
    if (function.name == name) return &function;
  }
  return nullptr;
}

void Scripting::flush_functions() {
  lua_close(_functions_lua);
  _libraries.clear();
  _function_index.clear();
  make_functions_state(_functions_lua, _names);
}

std::vector<std::string> Scripting::library_codes() const {
  std::vector<std::string> codes;
  for (const auto& [name, library] : _libraries) codes.push_back(library.code);
  return codes;
}

std::optional<std::string> Scripting::replace_libraries(const std::vector<std::string>& codes) {
  // Each loads into a fresh interpreter first, so codes that do not load
  // change nothing.
  {
    Scripting trial(_names);
    try {
      for (const auto& code : codes) load_library(trial, {"FUNCTION", "LOAD", code});
    } catch (const CommandError& e) {
      return std::string(e.what());
    }
  }
  flush_functions();
  for (const auto& code : codes) load_library(*this, {"FUNCTION", "LOAD", code});
  return std::nullopt;
}

const std::string* Scripting::body(const std::string& sha) const {
  auto found = _bodies.find(sha);
  return found == _bodies.end() ? nullptr : &found->second;
}

std::string Scripting::load(const std::string& text) {
  std::string sha = sha1_hex(text);
  _bodies.emplace(sha, text);
  return sha;
}

void Scripting::flush() {
  lua_close(_lua);
  _bodies.clear();
  _lua = luaL_newstate();
  install(_lua, _names);
}

namespace scripts {

void eval(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  const std::string sha = call.database.scripting_locked().load(call.args[1]);
  run_script(call, sha, call.args[1], 2, false);
}

void eval_ro(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  const std::string sha = call.database.scripting_locked().load(call.args[1]);
  run_script(call, sha, call.args[1], 2, true);
}

namespace {
void evalsha_generic(Call& call, bool read_only) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  const std::string sha = lower(call.args[1]);
  const std::string* body = call.database.scripting_locked().body(sha);
  if (!body) throw CommandError("NOSCRIPT No matching script.");
  const std::string copy = *body;
  run_script(call, sha, copy, 2, read_only);
}
}  // namespace

void evalsha(Call& call) { evalsha_generic(call, false); }
void evalsha_ro(Call& call) { evalsha_generic(call, true); }

void script(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  if (help::reply_help(call, "script")) return;
  const std::string sub = lower(call.args[1]);
  Scripting& scripting = call.database.scripting_locked();
  if (sub == "load") {
    if (call.args.size() != 3) throw CommandError("ERR wrong number of arguments for 'script|load' command");
    // A script that does not compile is refused at load, as Valkey refuses it.
    lua_State* L = scripting.state();
    if (luaL_loadbuffer(L, call.args[2].data(), call.args[2].size(), "@user_script") != 0) {
      std::string message = lua_tostring(L, -1);
      lua_settop(L, 0);
      throw CommandError("ERR Error compiling script (new function): " + message);
    }
    lua_settop(L, 0);
    call.reply.bulk(scripting.load(call.args[2]));
  } else if (sub == "exists") {
    if (call.args.size() < 3) throw CommandError("ERR wrong number of arguments for 'script|exists' command");
    call.reply.array(call.args.size() - 2);
    for (size_t i = 2; i < call.args.size(); ++i) call.reply.integer(scripting.body(lower(call.args[i])) ? 1 : 0);
  } else if (sub == "flush") {
    if (call.args.size() > 3 || (call.args.size() == 3 && !iequals(call.args[2], "ASYNC") && !iequals(call.args[2], "SYNC"))) {
      throw CommandError("ERR SCRIPT FLUSH only support SYNC|ASYNC option");
    }
    scripting.flush();
    call.reply.simple("OK");
  } else if (sub == "kill") {
    throw CommandError("NOTBUSY No scripts in execution right now.");
  } else if (sub == "show") {
    if (call.args.size() != 3) throw CommandError("ERR wrong number of arguments for 'script|show' command");
    const std::string* body = scripting.body(lower(call.args[2]));
    if (!body) throw CommandError("NOSCRIPT No matching script.");
    call.reply.bulk(*body);
  } else if (sub == "debug") {
    if (call.args.size() != 3) throw CommandError("ERR wrong number of arguments for 'script|debug' command");
    if (iequals(call.args[2], "NO")) {
      call.reply.simple("OK");
    } else if (iequals(call.args[2], "YES") || iequals(call.args[2], "SYNC")) {
      // Valkey's Lua debugger steps a script interactively on its connection;
      // Mimir has none.
      throw CommandError("ERR Mimir has no Lua debugger: SCRIPT DEBUG takes only NO");
    } else {
      throw CommandError("ERR Use SCRIPT DEBUG YES/SYNC/NO");
    }
  } else {
    throw CommandError("ERR unknown subcommand '" + call.args[1] + "'. Try SCRIPT HELP.");
  }
}

void fcall(Call& call) { run_function(call, false); }
void fcall_ro(Call& call) { run_function(call, true); }

namespace {

// One library as FUNCTION LIST answers it: a map, its functions maps too.
void write_library(resp::Reply& reply, const Scripting::Library& library, bool with_code) {
  reply.map(with_code ? 4 : 3);
  reply.bulk("library_name");
  reply.bulk(library.name);
  reply.bulk("engine");
  reply.bulk("LUA");
  reply.bulk("functions");
  reply.array(library.functions.size());
  for (const auto& function : library.functions) {
    reply.map(3);
    reply.bulk("name");
    reply.bulk(function.name);
    reply.bulk("description");
    if (function.description)
      reply.bulk(*function.description);
    else
      reply.null();
    reply.bulk("flags");
    reply.set(function.flags.size());
    for (const auto& flag : function.flags) reply.simple(flag);
  }
  if (with_code) {
    reply.bulk("library_code");
    reply.bulk(library.code);
  }
}

}  // namespace

void function(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  if (help::reply_help(call, "function")) return;
  const std::string sub = lower(call.args[1]);
  Scripting& scripting = call.database.scripting_locked();
  if (sub == "load") {
    if (call.args.size() < 3) throw CommandError("ERR wrong number of arguments for 'function|load' command");
    call.reply.bulk(load_library(scripting, call.args));
  } else if (sub == "delete") {
    if (call.args.size() != 3) throw CommandError("ERR wrong number of arguments for 'function|delete' command");
    auto found = scripting.libraries().find(call.args[2]);
    if (found == scripting.libraries().end()) throw CommandError("ERR Library not found");
    forget_functions(scripting, found->second);
    scripting.libraries().erase(found);
    call.reply.simple("OK");
  } else if (sub == "flush") {
    if (call.args.size() > 3 || (call.args.size() == 3 && !iequals(call.args[2], "ASYNC") && !iequals(call.args[2], "SYNC"))) {
      throw CommandError("ERR FUNCTION FLUSH only supports SYNC|ASYNC option");
    }
    scripting.flush_functions();
    call.reply.simple("OK");
  } else if (sub == "list") {
    bool with_code = false;
    std::optional<std::string> pattern;
    for (size_t i = 2; i < call.args.size(); ++i) {
      if (iequals(call.args[i], "WITHCODE")) {
        with_code = true;
      } else if (iequals(call.args[i], "LIBRARYNAME")) {
        if (i + 1 >= call.args.size()) throw CommandError("ERR library name argument was not given");
        pattern = call.args[++i];
      } else {
        throw CommandError("ERR Unknown argument " + call.args[i]);
      }
    }
    std::vector<const Scripting::Library*> listed;
    for (const auto& [name, library] : scripting.libraries()) {
      if (!pattern || keyspace::glob_match(*pattern, name, true)) listed.push_back(&library);
    }
    call.reply.array(listed.size());
    for (const auto* library : listed) write_library(call.reply, *library, with_code);
  } else if (sub == "stats") {
    if (call.args.size() != 2) throw CommandError("ERR wrong number of arguments for 'function|stats' command");
    call.reply.map(2);
    call.reply.bulk("running_script");
    call.reply.null();
    call.reply.bulk("engines");
    call.reply.map(1);
    call.reply.bulk("LUA");
    call.reply.map(2);
    call.reply.bulk("libraries_count");
    call.reply.integer(static_cast<int64_t>(scripting.libraries().size()));
    call.reply.bulk("functions_count");
    call.reply.integer(static_cast<int64_t>(scripting.function_index().size()));
  } else if (sub == "dump") {
    if (call.args.size() != 2) throw CommandError("ERR wrong number of arguments for 'function|dump' command");
    call.reply.bulk(keyspace::rdb::dump_functions(scripting.library_codes()));
  } else if (sub == "restore") {
    if (call.args.size() < 3 || call.args.size() > 4) throw CommandError("ERR wrong number of arguments for 'function|restore' command");
    std::string policy = "append";
    if (call.args.size() == 4) {
      policy = lower(call.args[3]);
      if (policy != "flush" && policy != "append" && policy != "replace") {
        throw CommandError("ERR Wrong restore policy given, value should be either FLUSH, APPEND or REPLACE.");
      }
    }
    auto read = keyspace::rdb::restore_functions(call.args[2]);
    if (auto* refusal = std::get_if<keyspace::rdb::Refusal>(&read)) {
      throw CommandError(*refusal == keyspace::rdb::Refusal::checksum ? "ERR DUMP payload version or checksum are wrong" : "ERR Bad data format");
    }
    const auto& codes = std::get<std::vector<std::string>>(read);
    // Every library loads into a fresh interpreter first, so a payload that
    // does not load, or would conflict, changes nothing.
    Scripting trial(scripting.names());
    for (const auto& code : codes) load_library(trial, {"FUNCTION", "LOAD", code});
    if (policy != "flush") {
      for (const auto& [name, library] : trial.libraries()) {
        if (policy == "append" && scripting.libraries().count(name)) throw CommandError("ERR Library " + name + " already exists");
      }
      for (const auto& [function, owner] : trial.function_index()) {
        auto held = scripting.function_index().find(function);
        // Under REPLACE a function of a library being replaced does not conflict.
        if (held != scripting.function_index().end() && !(policy == "replace" && trial.libraries().count(held->second))) {
          throw CommandError("ERR Function " + function + " already exists");
        }
      }
    }
    if (policy == "flush") scripting.flush_functions();
    for (const auto& code : codes) load_library(scripting, {"FUNCTION", "LOAD", "REPLACE", code});
    call.reply.simple("OK");
  } else if (sub == "kill") {
    throw CommandError("NOTBUSY No scripts in execution right now.");
  } else {
    throw CommandError("ERR unknown subcommand '" + call.args[1] + "'. Try FUNCTION HELP.");
  }
}

}  // namespace scripts
}  // namespace mimir::command
