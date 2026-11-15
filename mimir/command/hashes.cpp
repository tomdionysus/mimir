//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The hash commands, as Valkey 8 documents them. A hash that becomes empty is
// removed.

#include <charconv>
#include <climits>
#include <cmath>

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"
#include "mimir/keyspace/glob.h"

namespace mimir::command::hashes {

namespace {

using keyspace::Hash;

Hash* hash_value(keyspace::Keyspace& keys, std::string_view key) {
  keyspace::Entry* entry = keys.find(key);
  if (!entry) return nullptr;
  if (entry->type() != keyspace::Type::hash) throw CommandError(wrong_type);
  return &std::get<Hash>(entry->value);
}

Hash& hash_or_create(keyspace::Keyspace& keys, std::string_view key) {
  if (Hash* hash = hash_value(keys, key)) return *hash;
  return std::get<Hash>(keys.put(key, keyspace::Entry{Hash{}, std::nullopt}).value);
}

void set_pairs(Call& call, std::string_view name, bool count_new) {
  if (call.args.size() % 2 != 0) throw CommandError("ERR wrong number of arguments for '" + std::string(name) + "' command");
  Hash& hash = hash_or_create(call.keys, call.args[1]);
  int64_t created = 0;
  for (size_t i = 2; i < call.args.size(); i += 2) created += hash.set(call.args[i], call.args[i + 1]) ? 1 : 0;
  call.notify('h', "hset", call.args[1]);
  if (count_new)
    call.reply.integer(created);
  else
    call.reply.simple("OK");
}

}  // namespace

void hset(Call& call) { set_pairs(call, "hset", true); }
void hmset(Call& call) { set_pairs(call, "hmset", false); }

void hsetnx(Call& call) {
  Hash& hash = hash_or_create(call.keys, call.args[1]);
  if (hash.get(call.args[2])) {
    call.reply.integer(0);
    return;
  }
  hash.set(call.args[2], call.args[3]);
  call.notify('h', "hset", call.args[1]);
  call.reply.integer(1);
}

void hget(Call& call) {
  Hash* hash = hash_value(call.keys, call.args[1]);
  const std::string* value = hash ? hash->get(call.args[2]) : nullptr;
  if (value)
    call.reply.bulk(*value);
  else
    call.reply.null();
}

void hmget(Call& call) {
  Hash* hash = hash_value(call.keys, call.args[1]);
  call.reply.array(call.args.size() - 2);
  for (size_t i = 2; i < call.args.size(); ++i) {
    const std::string* value = hash ? hash->get(call.args[i]) : nullptr;
    if (value)
      call.reply.bulk(*value);
    else
      call.reply.null();
  }
}

void hdel(Call& call) {
  Hash* hash = hash_value(call.keys, call.args[1]);
  if (!hash) {
    call.reply.integer(0);
    return;
  }
  int64_t removed = 0;
  for (size_t i = 2; i < call.args.size(); ++i) removed += hash->erase(call.args[i]) ? 1 : 0;
  if (removed) {
    call.notify('h', "hdel", call.args[1]);
    if (hash->empty()) {
      call.keys.erase(call.args[1]);
      call.notify('g', "del", call.args[1]);
    }
  }
  call.reply.integer(removed);
}

void hlen(Call& call) {
  Hash* hash = hash_value(call.keys, call.args[1]);
  call.reply.integer(hash ? static_cast<int64_t>(hash->size()) : 0);
}

void hstrlen(Call& call) {
  Hash* hash = hash_value(call.keys, call.args[1]);
  const std::string* value = hash ? hash->get(call.args[2]) : nullptr;
  call.reply.integer(value ? static_cast<int64_t>(value->size()) : 0);
}

void hexists(Call& call) {
  Hash* hash = hash_value(call.keys, call.args[1]);
  call.reply.integer(hash && hash->get(call.args[2]) ? 1 : 0);
}

void hkeys(Call& call) {
  Hash* hash = hash_value(call.keys, call.args[1]);
  call.reply.array(hash ? hash->size() : 0);
  if (hash)
    for (const auto& [field, value] : *hash) call.reply.bulk(field);
}

void hvals(Call& call) {
  Hash* hash = hash_value(call.keys, call.args[1]);
  call.reply.array(hash ? hash->size() : 0);
  if (hash)
    for (const auto& [field, value] : *hash) call.reply.bulk(value);
}

void hgetall(Call& call) {
  Hash* hash = hash_value(call.keys, call.args[1]);
  call.reply.map(hash ? hash->size() : 0);
  if (!hash) return;
  for (const auto& [field, value] : *hash) {
    call.reply.bulk(field);
    call.reply.bulk(value);
  }
}

void hincrby(Call& call) {
  const int64_t by = integer_argument(call.args[3]);
  Hash& hash = hash_or_create(call.keys, call.args[1]);
  int64_t current = 0;
  if (const std::string* value = hash.get(call.args[2])) {
    auto parsed = parse_integer(*value);
    if (!parsed) throw CommandError("ERR hash value is not an integer");
    current = *parsed;
  }
  if ((by < 0 && current < 0 && by < LLONG_MIN - current) || (by > 0 && current > 0 && by > LLONG_MAX - current)) {
    throw CommandError("ERR increment or decrement would overflow");
  }
  hash.set(call.args[2], format_integer(current + by));
  call.notify('h', "hincrby", call.args[1]);
  call.reply.integer(current + by);
}

void hincrbyfloat(Call& call) {
  auto by = parse_long_double(call.args[3]);
  if (!by) throw CommandError(not_float);
  Hash* existing = hash_value(call.keys, call.args[1]);
  long double current = 0;
  if (existing) {
    if (const std::string* value = existing->get(call.args[2])) {
      auto parsed = parse_long_double(*value);
      if (!parsed) throw CommandError("ERR hash value is not a float");
      current = *parsed;
    }
  }
  const long double next = current + *by;
  if (std::isnan(next) || std::isinf(next)) throw CommandError("ERR increment would produce NaN or Infinity");
  std::string text = format_long_double(next);
  call.reply.bulk(text);
  hash_or_create(call.keys, call.args[1]).set(call.args[2], std::move(text));
  call.notify('h', "hincrbyfloat", call.args[1]);
}

void hrandfield(Call& call) {
  if (call.args.size() == 2) {
    Hash* hash = hash_value(call.keys, call.args[1]);
    if (!hash) {
      call.reply.null();
      return;
    }
    auto it = hash->begin();
    std::advance(it, static_cast<std::ptrdiff_t>(call.database.random() % hash->size()));
    call.reply.bulk(it->first);
    return;
  }
  const int64_t count = integer_argument(call.args[2]);
  bool with_values = false;
  if (call.args.size() == 4) {
    if (!iequals(call.args[3], "WITHVALUES")) throw CommandError(syntax_error);
    with_values = true;
  } else if (call.args.size() > 4) {
    throw CommandError(syntax_error);
  }
  if (with_values && (count < -(LLONG_MAX / 2) || count > LLONG_MAX / 2)) throw CommandError("ERR value is out of range");
  Hash* hash = hash_value(call.keys, call.args[1]);
  if (!hash || count == 0) {
    call.reply.array(0);
    return;
  }
  std::vector<const Hash::Item*> items;
  items.reserve(hash->size());
  for (const auto& item : *hash) items.push_back(&item);
  std::vector<const Hash::Item*> chosen;
  if (count < 0) {
    // Repeats allowed: |count| independent picks.
    const uint64_t n = 0 - static_cast<uint64_t>(count);
    chosen.reserve(static_cast<size_t>(std::min<uint64_t>(n, 1u << 20)));
    for (uint64_t i = 0; i < n; ++i) chosen.push_back(items[call.database.random() % items.size()]);
  } else if (static_cast<uint64_t>(count) >= items.size()) {
    chosen = items;
  } else {
    // Distinct: a partial shuffle.
    for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
      const size_t j = i + static_cast<size_t>(call.database.random() % (items.size() - i));
      std::swap(items[i], items[j]);
      chosen.push_back(items[i]);
    }
  }
  const bool resp3 = call.reply.protocol() == resp::Protocol::resp3;
  if (with_values && resp3) {
    call.reply.array(chosen.size());
    for (const auto* item : chosen) {
      call.reply.array(2);
      call.reply.bulk(item->first);
      call.reply.bulk(item->second);
    }
    return;
  }
  call.reply.array(with_values ? chosen.size() * 2 : chosen.size());
  for (const auto* item : chosen) {
    call.reply.bulk(item->first);
    if (with_values) call.reply.bulk(item->second);
  }
}

void hscan(Call& call) {
  uint64_t cursor = 0;
  {
    const std::string& text = call.args[2];
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), cursor);
    if (text.empty() || error != std::errc() || end != text.data() + text.size()) throw CommandError("ERR invalid cursor");
  }
  std::optional<std::string> pattern;
  bool novalues = false;
  int64_t count = 10;
  for (size_t i = 3; i < call.args.size(); ++i) {
    const std::string& option = call.args[i];
    const bool has_next = i + 1 < call.args.size();
    if (iequals(option, "MATCH") && has_next) {
      pattern = call.args[++i];
    } else if (iequals(option, "COUNT") && has_next) {
      count = integer_argument(call.args[++i]);
      if (count < 1) throw CommandError(syntax_error);
    } else if (iequals(option, "NOVALUES")) {
      novalues = true;
    } else {
      throw CommandError(syntax_error);
    }
  }
  Hash* hash = hash_value(call.keys, call.args[1]);
  std::vector<const Hash::Item*> kept;
  uint64_t next = 0;
  if (hash && hash->size() > keyspace::scan_whole) {
    // A large hash a page at a time (ruling 30), COUNT a hint as Valkey's is.
    auto [items, after] = hash->page(cursor, static_cast<size_t>(count));
    next = after;
    for (auto it : items)
      if (!pattern || keyspace::glob_match(*pattern, it->first)) kept.push_back(&*it);
  } else if (hash) {
    // The whole hash in one page, as Valkey answers for a small one.
    for (const auto& item : *hash)
      if (!pattern || keyspace::glob_match(*pattern, item.first)) kept.push_back(&item);
  }
  call.reply.array(2);
  call.reply.bulk(std::to_string(next));
  call.reply.array(novalues ? kept.size() : kept.size() * 2);
  for (const auto* item : kept) {
    call.reply.bulk(item->first);
    if (!novalues) call.reply.bulk(item->second);
  }
}

}  // namespace mimir::command::hashes
