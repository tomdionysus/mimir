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

#include <cctype>
#include <cstdio>
#include <ctime>
#include <exception>
#include <vector>

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
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

// One script's run, reached from the Lua side through the registry.
struct Run {
  Call* outer;
  Session session;
  bool read_only;
  std::string sha;
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

// Globals are read-only, as Valkey 7 and later make them; reading one that
// is not there is refused too.
int readonly_newindex(lua_State* L) {
  luaL_where(L, 1);
  lua_pushstring(L, "Attempt to modify a readonly table");
  lua_concat(L, 2);
  return lua_error(L);
}

int missing_global(lua_State* L) {
  luaL_where(L, 1);
  lua_pushfstring(L, "Script attempted to access nonexistent global variable '%s'", luaL_optstring(L, 2, "?"));
  lua_concat(L, 2);
  return lua_error(L);
}

// The script's line where an error arose.
int script_line(lua_State* L) {
  lua_Debug ar;
  for (int level = 0; lua_getstack(L, level, &ar); ++level) {
    lua_getinfo(L, "Sl", &ar);
    if (std::string_view(ar.source) == "@user_script" && ar.currentline > 0) return ar.currentline;
  }
  return 0;
}

// The message handler: makes the final error text, sha and line attached.
int on_error(lua_State* L) {
  Run& run = current(L);
  const int line = script_line(L);
  std::string message;
  if (lua_istable(L, 1)) {
    lua_getfield(L, 1, "err");
    message = lua_isstring(L, -1) ? lua_tostring(L, -1) : "ERR unknown error";
    lua_pop(L, 1);
  } else {
    message = std::string("ERR ") + (lua_isstring(L, 1) ? lua_tostring(L, 1) : "unknown error");
  }
  message += " script: " + run.sha + ", on @user_script:" + std::to_string(line) + ".";
  lua_pushstring(L, message.c_str());
  return 1;
}

void install(lua_State* L) {
  for (lua_CFunction open : {luaopen_base, luaopen_table, luaopen_string, luaopen_math}) {
    lua_pushcfunction(L, open);
    lua_call(L, 0, 0);
  }
  lua_settop(L, 0);
  // Valkey's sandbox: no files.
  for (const char* gone : {"dofile", "loadfile", "print"}) {
    lua_pushnil(L);
    lua_setglobal(L, gone);
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
  // Read-only globals.
  lua_pushvalue(L, LUA_GLOBALSINDEX);
  lua_newtable(L);
  lua_pushcfunction(L, readonly_newindex);
  lua_setfield(L, -2, "__newindex");
  lua_pushcfunction(L, missing_global);
  lua_setfield(L, -2, "__index");
  lua_setmetatable(L, -2);
  lua_pop(L, 1);
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
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, function.c_str());
  }
  const size_t first = numkeys_at + 1;
  const size_t split = first + static_cast<size_t>(*numkeys);
  set_array(L, "KEYS",
            std::vector<std::string>(call.args.begin() + static_cast<std::ptrdiff_t>(first), call.args.begin() + static_cast<std::ptrdiff_t>(split)));
  set_array(L, "ARGV", std::vector<std::string>(call.args.begin() + static_cast<std::ptrdiff_t>(split), call.args.end()));
  Run run{&call, Session{}, read_only, sha};
  run.session.id = call.session.id;
  run.session.db = call.session.db;
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

}  // namespace

Scripting::Scripting() {
  _lua = luaL_newstate();
  install(_lua);
}

Scripting::~Scripting() {
  if (_lua) lua_close(_lua);
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
  install(_lua);
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
  } else {
    throw CommandError("ERR unknown subcommand '" + call.args[1] + "'. Try SCRIPT HELP.");
  }
}

}  // namespace scripts
}  // namespace mimir::command
