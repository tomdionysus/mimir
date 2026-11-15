//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The generic key commands, as Valkey 8 documents them.

#include <charconv>

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"
#include "mimir/keyspace/glob.h"

namespace mimir::command::generic {

namespace {

// EXPIRE and its three siblings: `seconds` and `relative` say which.
void expire_generic(Call& call, bool seconds, bool relative, std::string_view command) {
  const std::string& key = call.args[1];
  const int64_t value = integer_argument(call.args[2]);
  bool nx = false, xx = false, gt = false, lt = false;
  for (size_t i = 3; i < call.args.size(); ++i) {
    const std::string& option = call.args[i];
    if (iequals(option, "NX"))
      nx = true;
    else if (iequals(option, "XX"))
      xx = true;
    else if (iequals(option, "GT"))
      gt = true;
    else if (iequals(option, "LT"))
      lt = true;
    else
      throw CommandError("ERR Unsupported option " + option);
  }
  if (nx && (xx || gt || lt)) throw CommandError("ERR NX and XX, GT or LT options at the same time are not compatible");
  if (gt && lt) throw CommandError("ERR GT and LT options at the same time are not compatible");
  const uint64_t at = absolute_expiry(value, seconds, relative, call.keys.now_ms(), command, true);

  keyspace::Entry* entry = call.keys.find(key);
  if (!entry) {
    call.reply.integer(0);
    return;
  }
  const std::optional<uint64_t> current = entry->expires_at;
  // No expiry is an infinite one: GT never beats it, LT always does.
  if ((nx && current) || (xx && !current) || (gt && (!current || at <= *current)) || (lt && current && at >= *current)) {
    call.reply.integer(0);
    return;
  }
  if (at <= call.keys.now_ms()) {
    call.keys.erase(key);
  } else {
    keyspace::Entry updated = std::move(*entry);
    updated.expires_at = at;
    call.keys.put(key, std::move(updated));
  }
  call.reply.integer(1);
}

// TTL and its siblings: -2 no key, -1 no expiry.
void ttl_generic(Call& call, bool ms, bool absolute) {
  keyspace::Entry* entry = call.keys.find(call.args[1]);
  if (!entry) {
    call.reply.integer(-2);
    return;
  }
  if (!entry->expires_at) {
    call.reply.integer(-1);
    return;
  }
  const uint64_t at = *entry->expires_at;
  if (absolute) {
    call.reply.integer(static_cast<int64_t>(ms ? at : at / 1000));
    return;
  }
  const uint64_t now = call.keys.now_ms();
  const int64_t left = at > now ? static_cast<int64_t>(at - now) : 0;
  call.reply.integer(ms ? left : (left + 500) / 1000);
}

std::optional<keyspace::Type> type_named(std::string_view name) {
  for (keyspace::Type type : {keyspace::Type::string, keyspace::Type::list, keyspace::Type::hash, keyspace::Type::set, keyspace::Type::zset}) {
    if (iequals(name, keyspace::type_name(type))) return type;
  }
  return std::nullopt;
}

}  // namespace

void del(Call& call) {
  int64_t removed = 0;
  for (size_t i = 1; i < call.args.size(); ++i) removed += call.keys.erase(call.args[i]) ? 1 : 0;
  call.reply.integer(removed);
}

void exists(Call& call) {
  int64_t found = 0;
  for (size_t i = 1; i < call.args.size(); ++i) found += call.keys.find(call.args[i]) ? 1 : 0;
  call.reply.integer(found);
}

void type(Call& call) {
  keyspace::Entry* entry = call.keys.find(call.args[1]);
  call.reply.simple(entry ? keyspace::type_name(entry->type()) : "none");
}

void expire(Call& call) { expire_generic(call, true, true, "expire"); }
void pexpire(Call& call) { expire_generic(call, false, true, "pexpire"); }
void expireat(Call& call) { expire_generic(call, true, false, "expireat"); }
void pexpireat(Call& call) { expire_generic(call, false, false, "pexpireat"); }
void ttl(Call& call) { ttl_generic(call, false, false); }
void pttl(Call& call) { ttl_generic(call, true, false); }
void expiretime(Call& call) { ttl_generic(call, false, true); }
void pexpiretime(Call& call) { ttl_generic(call, true, true); }

void persist(Call& call) {
  keyspace::Entry* entry = call.keys.find(call.args[1]);
  if (!entry || !entry->expires_at) {
    call.reply.integer(0);
    return;
  }
  keyspace::Entry updated = std::move(*entry);
  updated.expires_at.reset();
  call.keys.put(call.args[1], std::move(updated));
  call.reply.integer(1);
}

void rename(Call& call) {
  const std::string& from = call.args[1];
  const std::string& to = call.args[2];
  if (!call.keys.find(from)) throw CommandError("ERR no such key");
  if (from != to) call.keys.rename(from, to);
  call.reply.simple("OK");
}

void renamenx(Call& call) {
  const std::string& from = call.args[1];
  const std::string& to = call.args[2];
  if (!call.keys.find(from)) throw CommandError("ERR no such key");
  if (from == to || call.keys.find(to)) {
    call.reply.integer(0);
    return;
  }
  call.keys.rename(from, to);
  call.reply.integer(1);
}

void keys(Call& call) {
  const std::string& pattern = call.args[1];
  std::vector<std::string> matched;
  for (auto& key : call.keys.keys()) {
    if (keyspace::glob_match(pattern, key)) matched.push_back(std::move(key));
  }
  call.reply.array(matched.size());
  for (const auto& key : matched) call.reply.bulk(key);
}

void scan(Call& call) {
  uint64_t cursor = 0;
  {
    const std::string& text = call.args[1];
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), cursor);
    if (text.empty() || error != std::errc() || end != text.data() + text.size()) throw CommandError("ERR invalid cursor");
  }
  std::optional<std::string> pattern;
  size_t count = 10;
  std::optional<keyspace::Type> type;
  for (size_t i = 2; i < call.args.size(); ++i) {
    const std::string& option = call.args[i];
    const bool has_next = i + 1 < call.args.size();
    if (iequals(option, "MATCH") && has_next) {
      pattern = call.args[++i];
    } else if (iequals(option, "COUNT") && has_next) {
      const int64_t n = integer_argument(call.args[++i]);
      if (n < 1) throw CommandError(syntax_error);
      count = static_cast<size_t>(n);
    } else if (iequals(option, "TYPE") && has_next) {
      const std::string& name = call.args[++i];
      type = type_named(name);
      if (!type) throw CommandError("ERR unknown type name '" + name + "'");
    } else {
      throw CommandError(syntax_error);
    }
  }
  auto page = call.keys.scan(cursor, count, type);
  call.reply.array(2);
  call.reply.bulk(std::to_string(page.cursor));
  size_t kept = 0;
  for (const auto& key : page.keys) kept += !pattern || keyspace::glob_match(*pattern, key);
  call.reply.array(kept);
  for (const auto& key : page.keys) {
    if (!pattern || keyspace::glob_match(*pattern, key)) call.reply.bulk(key);
  }
}

void dbsize(Call& call) { call.reply.integer(static_cast<int64_t>(call.keys.size())); }

void flushall(Call& call) {
  if (call.args.size() > 2 || (call.args.size() == 2 && !iequals(call.args[1], "ASYNC") && !iequals(call.args[1], "SYNC"))) {
    throw CommandError(syntax_error);
  }
  call.keys.clear();
  call.reply.simple("OK");
}

void randomkey(Call& call) {
  auto key = call.keys.random_key(call.database.random());
  if (key)
    call.reply.bulk(*key);
  else
    call.reply.null();
}

void touch(Call& call) { exists(call); }

void copy(Call& call) {
  const std::string& from = call.args[1];
  const std::string& to = call.args[2];
  bool replace = false;
  for (size_t i = 3; i < call.args.size(); ++i) {
    if (iequals(call.args[i], "REPLACE")) {
      replace = true;
    } else if (iequals(call.args[i], "DB") && i + 1 < call.args.size()) {
      if (integer_argument(call.args[++i]) != 0) throw CommandError("ERR DB index is out of range");
    } else {
      throw CommandError(syntax_error);
    }
  }
  if (from == to) throw CommandError("ERR source and destination objects are the same");
  keyspace::Entry* source = call.keys.find(from);
  if (!source || (!replace && call.keys.find(to))) {
    call.reply.integer(0);
    return;
  }
  keyspace::Entry copied = *source;
  call.keys.put(to, std::move(copied));
  call.reply.integer(1);
}

}  // namespace mimir::command::generic
