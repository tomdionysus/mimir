//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The set commands, as Valkey 8 documents them. A set that becomes empty is
// removed, and a store that makes an empty set removes its destination.

#include <charconv>
#include <climits>

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"
#include "mimir/keyspace/glob.h"

namespace mimir::command::sets {

namespace {

using keyspace::Set;

Set* set_value(keyspace::Keyspace& keys, std::string_view key) {
  keyspace::Entry* entry = keys.find(key);
  if (!entry) return nullptr;
  if (entry->type() != keyspace::Type::set) throw CommandError(wrong_type);
  return &std::get<Set>(entry->value);
}

Set& set_or_create(keyspace::Keyspace& keys, std::string_view key) {
  if (Set* set = set_value(keys, key)) return *set;
  return std::get<Set>(keys.put(key, keyspace::Entry{Set{}, std::nullopt}).value);
}

void reply_set(resp::Reply& reply, const Set& set) {
  auto members = set.ordered();
  reply.set(members.size());
  for (auto member : members) reply.bulk(member);
}

// The sets named from `first` on (every one type-checked); a missing key is
// an empty set.
std::vector<const Set*> sets_of(Call& call, size_t first, size_t last) {
  std::vector<const Set*> out;
  static const Set empty;
  for (size_t i = first; i < last; ++i) {
    const Set* set = set_value(call.keys, call.args[i]);
    out.push_back(set ? set : &empty);
  }
  return out;
}

enum class Op { inter, onion, diff };

Set compute(const std::vector<const Set*>& sets, Op op) {
  Set result;
  if (sets.empty()) return result;
  if (op == Op::onion) {
    for (const Set* set : sets)
      for (auto member : set->ordered()) result.add(member);
    return result;
  }
  if (op == Op::inter) {
    const Set* smallest = sets.front();
    for (const Set* set : sets)
      if (set->size() < smallest->size()) smallest = set;
    for (auto member : smallest->ordered()) {
      bool everywhere = true;
      for (const Set* set : sets) everywhere = everywhere && set->contains(member);
      if (everywhere) result.add(member);
    }
    return result;
  }
  for (auto member : sets.front()->ordered()) {
    bool elsewhere = false;
    for (size_t i = 1; i < sets.size() && !elsewhere; ++i) elsewhere = sets[i]->contains(member);
    if (!elsewhere) result.add(member);
  }
  return result;
}

void combine(Call& call, Op op) {
  Set result = compute(sets_of(call, 1, call.args.size()), op);
  reply_set(call.reply, result);
}

void combine_store(Call& call, Op op) {
  Set result = compute(sets_of(call, 2, call.args.size()), op);
  const std::string& destination = call.args[1];
  call.reply.integer(static_cast<int64_t>(result.size()));
  if (result.empty()) {
    if (call.keys.erase(destination)) call.notify('g', "del", destination);
  } else {
    call.keys.put(destination, keyspace::Entry{std::move(result), std::nullopt});
    call.notify('s', op == Op::inter ? "sinterstore" : op == Op::onion ? "sunionstore" : "sdiffstore", destination);
  }
}

// After members were taken from `key`: the event, then an emptied key's.
void removed_from(Call& call, std::string_view event, const std::string& key, const Set& set) {
  call.notify('s', event, key);
  if (!set.empty()) return;
  call.keys.erase(key);
  call.notify('g', "del", key);
}

}  // namespace

void sadd(Call& call) {
  Set& set = set_or_create(call.keys, call.args[1]);
  int64_t added = 0;
  for (size_t i = 2; i < call.args.size(); ++i) added += set.add(call.args[i]) ? 1 : 0;
  if (added) call.notify('s', "sadd", call.args[1]);
  call.reply.integer(added);
}

void srem(Call& call) {
  Set* set = set_value(call.keys, call.args[1]);
  if (!set) {
    call.reply.integer(0);
    return;
  }
  int64_t removed = 0;
  for (size_t i = 2; i < call.args.size(); ++i) removed += set->erase(call.args[i]) ? 1 : 0;
  if (removed) removed_from(call, "srem", call.args[1], *set);
  call.reply.integer(removed);
}

void smembers(Call& call) {
  Set* set = set_value(call.keys, call.args[1]);
  if (set)
    reply_set(call.reply, *set);
  else
    call.reply.set(0);
}

void sismember(Call& call) {
  Set* set = set_value(call.keys, call.args[1]);
  call.reply.integer(set && set->contains(call.args[2]) ? 1 : 0);
}

void smismember(Call& call) {
  Set* set = set_value(call.keys, call.args[1]);
  call.reply.array(call.args.size() - 2);
  for (size_t i = 2; i < call.args.size(); ++i) call.reply.integer(set && set->contains(call.args[i]) ? 1 : 0);
}

void scard(Call& call) {
  Set* set = set_value(call.keys, call.args[1]);
  call.reply.integer(set ? static_cast<int64_t>(set->size()) : 0);
}

void spop(Call& call) {
  if (call.args.size() > 3) throw CommandError(syntax_error);
  std::optional<uint64_t> count;
  if (call.args.size() == 3) {
    auto parsed = parse_integer(call.args[2]);
    if (!parsed || *parsed < 0) throw CommandError("ERR value is out of range, must be positive");
    count = static_cast<uint64_t>(*parsed);
  }
  Set* set = set_value(call.keys, call.args[1]);
  if (!set) {
    if (count)
      call.reply.set(0);
    else
      call.reply.null();
    return;
  }
  auto members = set->ordered();
  std::vector<std::string> taken;
  const size_t n = count ? static_cast<size_t>(std::min<uint64_t>(*count, members.size())) : 1;
  for (size_t i = 0; i < n; ++i) {
    const size_t j = i + static_cast<size_t>(call.database.random() % (members.size() - i));
    std::swap(members[i], members[j]);
    taken.emplace_back(members[i]);
  }
  for (const auto& member : taken) set->erase(member);
  if (!count) {
    call.reply.bulk(taken.front());
  } else {
    call.reply.set(taken.size());
    for (const auto& member : taken) call.reply.bulk(member);
  }
  if (!taken.empty()) removed_from(call, "spop", call.args[1], *set);
}

void srandmember(Call& call) {
  if (call.args.size() > 3) throw CommandError(syntax_error);
  Set* set = nullptr;
  if (call.args.size() == 2) {
    set = set_value(call.keys, call.args[1]);
    if (!set) {
      call.reply.null();
      return;
    }
    auto members = set->ordered();
    call.reply.bulk(members[call.database.random() % members.size()]);
    return;
  }
  const int64_t count = integer_argument(call.args[2]);
  if (count == LLONG_MIN) throw CommandError("ERR value is out of range");
  set = set_value(call.keys, call.args[1]);
  if (!set || count == 0) {
    call.reply.array(0);
    return;
  }
  auto members = set->ordered();
  if (count < 0) {
    const uint64_t n = 0 - static_cast<uint64_t>(count);
    call.reply.array(static_cast<size_t>(n));
    for (uint64_t i = 0; i < n; ++i) call.reply.bulk(members[call.database.random() % members.size()]);
    return;
  }
  const size_t n = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(count), members.size()));
  if (n < members.size()) {
    for (size_t i = 0; i < n; ++i) {
      const size_t j = i + static_cast<size_t>(call.database.random() % (members.size() - i));
      std::swap(members[i], members[j]);
    }
  }
  call.reply.array(n);
  for (size_t i = 0; i < n; ++i) call.reply.bulk(members[i]);
}

void smove(Call& call) {
  const std::string& source = call.args[1];
  const std::string& destination = call.args[2];
  const std::string& member = call.args[3];
  Set* from = set_value(call.keys, source);
  set_value(call.keys, destination);
  if (!from) {
    call.reply.integer(0);
    return;
  }
  if (source == destination) {
    call.reply.integer(from->contains(member) ? 1 : 0);
    return;
  }
  if (!from->erase(member)) {
    call.reply.integer(0);
    return;
  }
  removed_from(call, "srem", source, *from);
  if (set_or_create(call.keys, destination).add(member)) call.notify('s', "sadd", destination);
  call.reply.integer(1);
}

void sinter(Call& call) { combine(call, Op::inter); }
void sunion(Call& call) { combine(call, Op::onion); }
void sdiff(Call& call) { combine(call, Op::diff); }
void sinterstore(Call& call) { combine_store(call, Op::inter); }
void sunionstore(Call& call) { combine_store(call, Op::onion); }
void sdiffstore(Call& call) { combine_store(call, Op::diff); }

void sintercard(Call& call) {
  auto numkeys = parse_integer(call.args[1]);
  if (!numkeys || *numkeys <= 0) throw CommandError("ERR numkeys should be greater than 0");
  if (static_cast<uint64_t>(*numkeys) > call.args.size() - 2) throw CommandError("ERR Number of keys can't be greater than number of args");
  const size_t end = 2 + static_cast<size_t>(*numkeys);
  uint64_t limit = 0;
  for (size_t i = end; i < call.args.size(); ++i) {
    if (iequals(call.args[i], "LIMIT") && i + 1 < call.args.size()) {
      auto parsed = parse_integer(call.args[++i]);
      if (!parsed) throw CommandError(not_integer);
      if (*parsed < 0) throw CommandError("ERR LIMIT can't be negative");
      limit = static_cast<uint64_t>(*parsed);
    } else {
      throw CommandError(syntax_error);
    }
  }
  Set result = compute(sets_of(call, 2, end), Op::inter);
  uint64_t size = result.size();
  if (limit > 0 && size > limit) size = limit;
  call.reply.integer(static_cast<int64_t>(size));
}

void sscan(Call& call) {
  {
    const std::string& text = call.args[2];
    uint64_t cursor = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), cursor);
    if (text.empty() || error != std::errc() || end != text.data() + text.size()) throw CommandError("ERR invalid cursor");
  }
  std::optional<std::string> pattern;
  for (size_t i = 3; i < call.args.size(); ++i) {
    const bool has_next = i + 1 < call.args.size();
    if (iequals(call.args[i], "MATCH") && has_next) {
      pattern = call.args[++i];
    } else if (iequals(call.args[i], "COUNT") && has_next) {
      if (integer_argument(call.args[++i]) < 1) throw CommandError(syntax_error);
    } else {
      throw CommandError(syntax_error);
    }
  }
  Set* set = set_value(call.keys, call.args[1]);
  std::vector<std::string_view> kept;
  if (set)
    for (auto member : set->ordered())
      if (!pattern || keyspace::glob_match(*pattern, member)) kept.push_back(member);
  // The whole set in one page, as Valkey answers for a small one.
  call.reply.array(2);
  call.reply.bulk("0");
  call.reply.array(kept.size());
  for (auto member : kept) call.reply.bulk(member);
}

}  // namespace mimir::command::sets
