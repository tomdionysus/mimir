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
#include "mimir/keyspace/rdb.h"

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
    // A time already past expires the key now, and says so.
    call.keys.erase(key);
    call.notify('x', "expired", key);
  } else {
    keyspace::Entry updated = std::move(*entry);
    updated.expires_at = at;
    call.keys.put(key, std::move(updated));
    call.notify('g', "expire", key);
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
  for (keyspace::Type type :
       {keyspace::Type::string, keyspace::Type::list, keyspace::Type::hash, keyspace::Type::set, keyspace::Type::zset, keyspace::Type::stream}) {
    if (iequals(name, keyspace::type_name(type))) return type;
  }
  return std::nullopt;
}

}  // namespace

void del(Call& call) {
  int64_t removed = 0;
  for (size_t i = 1; i < call.args.size(); ++i) {
    if (!call.keys.erase(call.args[i])) continue;
    ++removed;
    call.notify('g', "del", call.args[i]);
  }
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
  call.notify('g', "persist", call.args[1]);
  call.reply.integer(1);
}

namespace {
void rename_notified(Call& call, const std::string& from, const std::string& to) {
  call.keys.rename(from, to);
  call.notify('g', "rename_from", from);
  call.notify('g', "rename_to", to);
}
}  // namespace

void rename(Call& call) {
  const std::string& from = call.args[1];
  const std::string& to = call.args[2];
  if (!call.keys.find(from)) throw CommandError("ERR no such key");
  if (from != to) rename_notified(call, from, to);
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
  rename_notified(call, from, to);
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

void flushall(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  if (call.args.size() > 2 || (call.args.size() == 2 && !iequals(call.args[1], "ASYNC") && !iequals(call.args[1], "SYNC"))) {
    throw CommandError(syntax_error);
  }
  for (size_t i = 0; i < call.database.databases(); ++i) call.database.db_locked(i).clear();
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

void copy(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  const std::string& from = call.args[1];
  const std::string& to = call.args[2];
  bool replace = false;
  size_t target = call.session.db;
  for (size_t i = 3; i < call.args.size(); ++i) {
    if (iequals(call.args[i], "REPLACE")) {
      replace = true;
    } else if (iequals(call.args[i], "DB") && i + 1 < call.args.size()) {
      const int64_t db = integer_argument(call.args[++i]);
      if (db < 0 || static_cast<size_t>(db) >= call.database.databases()) throw CommandError("ERR DB index is out of range");
      target = static_cast<size_t>(db);
    } else {
      throw CommandError(syntax_error);
    }
  }
  if (from == to && target == call.session.db) throw CommandError("ERR source and destination objects are the same");
  keyspace::Keyspace& into = call.database.db_locked(target);
  keyspace::Entry* source = call.keys.find(from);
  if (!source || (!replace && into.find(to))) {
    call.reply.integer(0);
    return;
  }
  keyspace::Entry copied = *source;
  into.put(to, std::move(copied));
  call.database.notify_locked(target, 'g', "copy_to", to);
  call.reply.integer(1);
}

void move(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  const int64_t db = integer_argument(call.args[2]);
  if (db < 0 || static_cast<size_t>(db) >= call.database.databases()) throw CommandError("ERR DB index is out of range");
  if (static_cast<size_t>(db) == call.session.db) throw CommandError("ERR source and destination objects are the same");
  keyspace::Keyspace& into = call.database.db_locked(static_cast<size_t>(db));
  keyspace::Entry* source = call.keys.find(call.args[1]);
  if (!source || into.find(call.args[1])) {
    call.reply.integer(0);
    return;
  }
  // The key keeps its value and its expiry.
  into.put(call.args[1], std::move(*source));
  call.keys.erase(call.args[1]);
  call.notify('g', "move_from", call.args[1]);
  call.database.notify_locked(static_cast<size_t>(db), 'g', "move_to", call.args[1]);
  call.reply.integer(1);
}

void dump(Call& call) {
  keyspace::Entry* entry = call.keys.find(call.args[1]);
  if (!entry) {
    call.reply.null();
    return;
  }
  call.reply.bulk(keyspace::rdb::dump(*entry));
}

void restore(Call& call) {
  const std::string& key = call.args[1];
  bool replace = false;
  bool absttl = false;
  bool idle = false;
  bool freq = false;
  for (size_t j = 4; j < call.args.size(); ++j) {
    const size_t additional = call.args.size() - j - 1;
    if (iequals(call.args[j], "REPLACE")) {
      replace = true;
    } else if (iequals(call.args[j], "ABSTTL")) {
      absttl = true;
    } else if (iequals(call.args[j], "IDLETIME") && additional >= 1 && !freq) {
      if (integer_argument(call.args[++j]) < 0) throw CommandError("ERR Invalid IDLETIME value, must be >= 0");
      idle = true;
    } else if (iequals(call.args[j], "FREQ") && additional >= 1 && !idle) {
      const int64_t f = integer_argument(call.args[++j]);
      if (f < 0 || f > 255) throw CommandError("ERR Invalid FREQ value, must be >= 0 and <= 255");
      freq = true;
    } else {
      throw CommandError(syntax_error);
    }
  }
  if (!replace && call.keys.find(key)) throw CommandError("BUSYKEY Target key name already exists.");
  const int64_t ttl = integer_argument(call.args[2]);
  if (ttl < 0) throw CommandError("ERR Invalid TTL value, must be >= 0");
  auto restored = keyspace::rdb::restore(call.args[3]);
  if (auto* refusal = std::get_if<keyspace::rdb::Refusal>(&restored)) {
    throw CommandError(*refusal == keyspace::rdb::Refusal::checksum ? "ERR DUMP payload version or checksum are wrong" : "ERR Bad data format");
  }
  keyspace::Entry entry = std::move(std::get<keyspace::Entry>(restored));
  if (ttl > 0) {
    const uint64_t now = call.keys.now_ms();
    const uint64_t at = absttl ? static_cast<uint64_t>(ttl) : now + static_cast<uint64_t>(ttl);
    // Already past: nothing is made, and a key replaced is gone.
    if (at <= now) {
      if (replace && call.keys.erase(key)) call.notify('g', "del", key);
      call.reply.simple("OK");
      return;
    }
    entry.expires_at = at;
  }
  if (replace) call.keys.erase(key);
  call.keys.put(key, std::move(entry));
  call.notify('g', "restore", key);
  call.reply.simple("OK");
}

// MIGRATE host port key|"" destination-db timeout [COPY] [REPLACE]
// [AUTH password | AUTH2 username password] [KEYS key...], checked as
// Valkey checks it. The keys are read here, under the lock; the talk with
// the target is the connection's, off the lock (ruling 31), and the command
// ends in Database::finish_migrate.
void migrate(Call& call) {
  MigrateJob job;
  job.host = call.args[1];
  job.port = call.args[2];
  size_t first_key = 3;
  size_t keys = 1;
  for (size_t j = 6; j < call.args.size(); ++j) {
    const size_t more = call.args.size() - 1 - j;
    if (iequals(call.args[j], "COPY")) {
      job.copy = true;
    } else if (iequals(call.args[j], "REPLACE")) {
      job.replace = true;
    } else if (iequals(call.args[j], "AUTH")) {
      if (more < 1) throw CommandError(syntax_error);
      job.password = call.args[++j];
    } else if (iequals(call.args[j], "AUTH2")) {
      if (more < 2) throw CommandError(syntax_error);
      job.username = call.args[++j];
      job.password = call.args[++j];
    } else if (iequals(call.args[j], "KEYS")) {
      if (!call.args[3].empty()) throw CommandError("ERR When using MIGRATE KEYS option, the key argument must be set to the empty string");
      first_key = j + 1;
      keys = call.args.size() - j - 1;
      break;
    } else {
      throw CommandError(syntax_error);
    }
  }
  job.timeout_ms = integer_argument(call.args[5]);
  job.target_db = integer_argument(call.args[4]);
  if (job.timeout_ms <= 0) job.timeout_ms = 1000;
  job.source_db = call.db;
  const uint64_t now = call.keys.now_ms();
  for (size_t i = first_key; i < first_key + keys; ++i) {
    keyspace::Entry* entry = call.keys.find(call.args[i]);
    if (!entry) continue;
    MigrateJob::Item item{call.args[i], keyspace::rdb::dump(*entry), 0};
    // A life that ends now still goes, with one millisecond, as Valkey's.
    if (entry->expires_at) item.ttl_ms = std::max<int64_t>(1, static_cast<int64_t>(*entry->expires_at) - static_cast<int64_t>(now));
    job.items.push_back(std::move(item));
  }
  // None of them here: nothing to move, which is no error.
  if (job.items.empty()) {
    call.reply.simple("NOKEY");
    return;
  }
  call.migrate = std::move(job);
}

void swapdb(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  auto a = parse_integer(call.args[1]);
  if (!a) throw CommandError("ERR invalid first DB index");
  auto b = parse_integer(call.args[2]);
  if (!b) throw CommandError("ERR invalid second DB index");
  const auto in_range = [&](int64_t i) { return i >= 0 && static_cast<size_t>(i) < call.database.databases(); };
  if (!in_range(*a) || !in_range(*b)) throw CommandError("ERR DB index is out of range");
  call.database.swap_locked(static_cast<size_t>(*a), static_cast<size_t>(*b));
  call.reply.simple("OK");
}

void flushdb(Call& call) {
  if (call.args.size() > 2 || (call.args.size() == 2 && !iequals(call.args[1], "ASYNC") && !iequals(call.args[1], "SYNC"))) {
    throw CommandError(syntax_error);
  }
  call.keys.clear();
  call.reply.simple("OK");
}

}  // namespace mimir::command::generic
