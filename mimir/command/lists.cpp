//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The list commands that do not block, as Valkey 8 documents them. A list that
// becomes empty is removed, so no key ever holds an empty list.

#include <climits>
#include <cmath>

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"

namespace mimir::command::lists {

namespace {

using keyspace::List;

List* list_value(keyspace::Keyspace& keys, std::string_view key) {
  keyspace::Entry* entry = keys.find(key);
  if (!entry) return nullptr;
  if (entry->type() != keyspace::Type::list) throw CommandError(wrong_type);
  return &std::get<List>(entry->value);
}

List& list_or_create(keyspace::Keyspace& keys, std::string_view key) {
  if (List* list = list_value(keys, key)) return *list;
  return std::get<List>(keys.put(key, keyspace::Entry{List{}, std::nullopt}).value);
}

// An emptied list's key goes, and says so.
void drop_if_empty(Call& call, std::string_view key, const List& list) {
  if (!list.empty()) return;
  call.keys.erase(key);
  call.notify('g', "del", key);
}

// A pop's event, then the emptied key's.
void popped(Call& call, std::string_view key, const List& list, bool left) {
  call.notify('l', left ? "lpop" : "rpop", key);
  drop_if_empty(call, key, list);
}

// A blocking command's source list. Serving a woken wait, only the ready key
// counts and a value of another type is no list (the wait goes on); otherwise
// another type is WRONGTYPE, as for any list command.
List* blocking_source(Call& call, const std::string& key) {
  if (!call.ready_key) return list_value(call.keys, key);
  if (key != *call.ready_key) return nullptr;
  keyspace::Entry* entry = call.keys.find(key);
  if (!entry || entry->type() != keyspace::Type::list) return nullptr;
  return &std::get<List>(entry->value);
}

void park(Call& call, std::vector<std::string> keys, double seconds, bool null_bulk) {
  call.blocked = true;
  call.block_keys = std::move(keys);
  call.block_seconds = seconds;
  call.block_null_bulk = null_bulk;
}

// LEFT or RIGHT, as true for left.
bool side(const std::string& word) {
  if (iequals(word, "LEFT")) return true;
  if (iequals(word, "RIGHT")) return false;
  throw CommandError(syntax_error);
}

// An index from either end, made absolute; none when out of range.
std::optional<size_t> absolute(int64_t index, size_t size) {
  const int64_t len = static_cast<int64_t>(size);
  if (index < 0) index += len;
  if (index < 0 || index >= len) return std::nullopt;
  return static_cast<size_t>(index);
}

// LRANGE's and LTRIM's [start, stop], clamped; none when empty.
std::optional<std::pair<size_t, size_t>> range(int64_t start, int64_t stop, size_t size) {
  const int64_t len = static_cast<int64_t>(size);
  if (start < 0) start += len;
  if (stop < 0) stop += len;
  if (start < 0) start = 0;
  if (start > stop || start >= len) return std::nullopt;
  if (stop >= len) stop = len - 1;
  return std::make_pair(static_cast<size_t>(start), static_cast<size_t>(stop));
}

void push(Call& call, bool left, bool only_if_exists) {
  const std::string& key = call.args[1];
  List* list = list_value(call.keys, key);
  if (!list) {
    if (only_if_exists) {
      call.reply.integer(0);
      return;
    }
    list = &list_or_create(call.keys, key);
  }
  for (size_t i = 2; i < call.args.size(); ++i) {
    if (left)
      list->push_front(call.args[i]);
    else
      list->push_back(call.args[i]);
  }
  call.notify('l', left ? "lpush" : "rpush", key);
  call.reply.integer(static_cast<int64_t>(list->size()));
}

std::string take(List& list, bool left) {
  std::string out = std::move(left ? list.front() : list.back());
  if (left)
    list.pop_front();
  else
    list.pop_back();
  return out;
}

void pop(Call& call, bool left) {
  const std::string& key = call.args[1];
  if (call.args.size() > 3) throw CommandError("ERR wrong number of arguments for '" + std::string(left ? "lpop" : "rpop") + "' command");
  std::optional<int64_t> count;
  if (call.args.size() == 3) {
    auto parsed = parse_integer(call.args[2]);
    if (!parsed || *parsed < 0) throw CommandError("ERR value is out of range, must be positive");
    count = *parsed;
  }
  List* list = list_value(call.keys, key);
  if (!list) {
    if (count)
      call.reply.null_array();
    else
      call.reply.null();
    return;
  }
  if (!count) {
    call.reply.bulk(take(*list, left));
  } else {
    const size_t n = std::min(static_cast<size_t>(*count), list->size());
    call.reply.array(n);
    for (size_t i = 0; i < n; ++i) call.reply.bulk(take(*list, left));
    if (n == 0) return;
  }
  popped(call, key, *list, left);
}

// LMOVE's work, shared with RPOPLPUSH and their blocking forms; whether
// anything moved.
bool move(Call& call, bool from_left, bool to_left, bool blocking = false) {
  const std::string& source = call.args[1];
  const std::string& destination = call.args[2];
  List* from = blocking ? blocking_source(call, source) : list_value(call.keys, source);
  if (!from) {
    if (!blocking) call.reply.null();
    return false;
  }
  // The destination's type is checked before anything moves.
  list_value(call.keys, destination);
  std::string element = take(*from, from_left);
  call.reply.bulk(element);
  // The push is announced before the pop, and an emptied source after both,
  // in Valkey's order.
  List& to = source == destination ? *from : list_or_create(call.keys, destination);
  if (to_left)
    to.push_front(std::move(element));
  else
    to.push_back(std::move(element));
  call.notify('l', to_left ? "lpush" : "rpush", destination);
  popped(call, source, *from, from_left);
  return true;
}

// BLPOP and BRPOP: the first of the keys holding something, else wait.
void blocking_pop(Call& call, bool left) {
  const double seconds = block_timeout(call.args.back());
  std::vector<std::string> keys(call.args.begin() + 1, call.args.end() - 1);
  for (const auto& key : keys) {
    List* list = blocking_source(call, key);
    if (!list) continue;
    call.reply.array(2);
    call.reply.bulk(key);
    call.reply.bulk(take(*list, left));
    popped(call, key, *list, left);
    return;
  }
  park(call, std::move(keys), seconds, false);
}

// LMPOP and BLMPOP: numkeys at `numkeys_at`, the keys, LEFT or RIGHT, COUNT.
void multi_pop(Call& call, size_t numkeys_at, std::optional<double> seconds) {
  auto numkeys = parse_integer(call.args[numkeys_at]);
  if (!numkeys || *numkeys <= 0) throw CommandError("ERR numkeys should be greater than 0");
  // The keys, then LEFT or RIGHT, then COUNT n at most.
  if (static_cast<uint64_t>(*numkeys) > call.args.size() - numkeys_at - 2) throw CommandError(syntax_error);
  const size_t first = numkeys_at + 1;
  const size_t where_at = first + static_cast<size_t>(*numkeys);
  const bool left = side(call.args[where_at]);
  int64_t count = 1;
  bool have_count = false;
  for (size_t i = where_at + 1; i < call.args.size(); ++i) {
    if (iequals(call.args[i], "COUNT") && i + 1 < call.args.size() && !have_count) {
      auto parsed = parse_integer(call.args[++i]);
      if (!parsed || *parsed <= 0) throw CommandError("ERR count should be greater than 0");
      count = *parsed;
      have_count = true;
    } else {
      throw CommandError(syntax_error);
    }
  }
  for (size_t k = first; k < where_at; ++k) {
    List* list = seconds ? blocking_source(call, call.args[k]) : list_value(call.keys, call.args[k]);
    if (!list) continue;
    const size_t n = std::min(static_cast<size_t>(count), list->size());
    call.reply.array(2);
    call.reply.bulk(call.args[k]);
    call.reply.array(n);
    for (size_t i = 0; i < n; ++i) call.reply.bulk(take(*list, left));
    popped(call, call.args[k], *list, left);
    return;
  }
  if (seconds)
    park(call, std::vector<std::string>(call.args.begin() + static_cast<std::ptrdiff_t>(first), call.args.begin() + static_cast<std::ptrdiff_t>(where_at)),
         *seconds, false);
  else
    call.reply.null_array();
}

}  // namespace

void lpush(Call& call) { push(call, true, false); }
void rpush(Call& call) { push(call, false, false); }
void lpushx(Call& call) { push(call, true, true); }
void rpushx(Call& call) { push(call, false, true); }
void lpop(Call& call) { pop(call, true); }
void rpop(Call& call) { pop(call, false); }

void llen(Call& call) {
  List* list = list_value(call.keys, call.args[1]);
  call.reply.integer(list ? static_cast<int64_t>(list->size()) : 0);
}

void lrange(Call& call) {
  const int64_t start = integer_argument(call.args[2]);
  const int64_t stop = integer_argument(call.args[3]);
  List* list = list_value(call.keys, call.args[1]);
  auto span = list ? range(start, stop, list->size()) : std::nullopt;
  if (!span) {
    call.reply.array(0);
    return;
  }
  call.reply.array(span->second - span->first + 1);
  for (size_t i = span->first; i <= span->second; ++i) call.reply.bulk((*list)[i]);
}

void lindex(Call& call) {
  List* list = list_value(call.keys, call.args[1]);
  if (!list) {
    call.reply.null();
    return;
  }
  auto at = absolute(integer_argument(call.args[2]), list->size());
  if (at)
    call.reply.bulk((*list)[*at]);
  else
    call.reply.null();
}

void lset(Call& call) {
  const int64_t index = integer_argument(call.args[2]);
  List* list = list_value(call.keys, call.args[1]);
  if (!list) throw CommandError("ERR no such key");
  auto at = absolute(index, list->size());
  if (!at) throw CommandError("ERR index out of range");
  (*list)[*at] = call.args[3];
  call.notify('l', "lset", call.args[1]);
  call.reply.simple("OK");
}

void linsert(Call& call) {
  bool after;
  if (iequals(call.args[2], "AFTER"))
    after = true;
  else if (iequals(call.args[2], "BEFORE"))
    after = false;
  else
    throw CommandError(syntax_error);
  List* list = list_value(call.keys, call.args[1]);
  if (!list) {
    call.reply.integer(0);
    return;
  }
  for (auto it = list->begin(); it != list->end(); ++it) {
    if (*it == call.args[3]) {
      list->insert(after ? std::next(it) : it, call.args[4]);
      call.notify('l', "linsert", call.args[1]);
      call.reply.integer(static_cast<int64_t>(list->size()));
      return;
    }
  }
  call.reply.integer(-1);
}

void lrem(Call& call) {
  const int64_t count = integer_argument(call.args[2]);
  const std::string& element = call.args[3];
  List* list = list_value(call.keys, call.args[1]);
  if (!list) {
    call.reply.integer(0);
    return;
  }
  // At most |count| matches, from the head for a positive count and from the
  // tail for a negative one; every match for zero.
  const uint64_t limit = count == 0 ? UINT64_MAX : (count < 0 ? 0 - static_cast<uint64_t>(count) : static_cast<uint64_t>(count));
  uint64_t removed = 0;
  if (count >= 0) {
    for (auto it = list->begin(); it != list->end() && removed < limit;) {
      if (*it == element) {
        it = list->erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
  } else {
    for (size_t i = list->size(); i > 0 && removed < limit; --i) {
      if ((*list)[i - 1] == element) {
        list->erase(list->begin() + static_cast<std::ptrdiff_t>(i - 1));
        ++removed;
      }
    }
  }
  call.reply.integer(static_cast<int64_t>(removed));
  if (removed == 0) return;
  call.notify('l', "lrem", call.args[1]);
  drop_if_empty(call, call.args[1], *list);
}

void ltrim(Call& call) {
  const int64_t start = integer_argument(call.args[2]);
  const int64_t stop = integer_argument(call.args[3]);
  List* list = list_value(call.keys, call.args[1]);
  if (list) {
    auto span = range(start, stop, list->size());
    if (!span) {
      list->clear();
    } else {
      list->erase(list->begin() + static_cast<std::ptrdiff_t>(span->second + 1), list->end());
      list->erase(list->begin(), list->begin() + static_cast<std::ptrdiff_t>(span->first));
    }
    call.notify('l', "ltrim", call.args[1]);
    drop_if_empty(call, call.args[1], *list);
  }
  call.reply.simple("OK");
}

void lpos(Call& call) {
  int64_t rank = 1;
  std::optional<int64_t> count;
  int64_t maxlen = 0;
  for (size_t i = 3; i < call.args.size(); ++i) {
    const std::string& option = call.args[i];
    const bool has_next = i + 1 < call.args.size();
    if (iequals(option, "RANK") && has_next) {
      rank = integer_argument(call.args[++i]);
      if (rank == 0) {
        throw CommandError(
            "ERR RANK can't be zero: use 1 to start from the first match, 2 from the second ... or use negative to start from the end of the list");
      }
      if (rank == LLONG_MIN) throw CommandError("ERR value is out of range");
    } else if (iequals(option, "COUNT") && has_next) {
      count = integer_argument(call.args[++i]);
      if (*count < 0) throw CommandError("ERR COUNT can't be negative");
    } else if (iequals(option, "MAXLEN") && has_next) {
      maxlen = integer_argument(call.args[++i]);
      if (maxlen < 0) throw CommandError("ERR MAXLEN can't be negative");
    } else {
      throw CommandError(syntax_error);
    }
  }
  List* list = list_value(call.keys, call.args[1]);
  std::vector<int64_t> found;
  if (list) {
    const size_t size = list->size();
    const bool forward = rank > 0;
    uint64_t skip = (forward ? static_cast<uint64_t>(rank) : 0 - static_cast<uint64_t>(rank)) - 1;
    const uint64_t want = count ? (*count == 0 ? UINT64_MAX : static_cast<uint64_t>(*count)) : 1;
    const uint64_t compare = maxlen == 0 ? UINT64_MAX : static_cast<uint64_t>(maxlen);
    for (uint64_t looked = 0; looked < size && looked < compare && found.size() < want; ++looked) {
      const size_t at = forward ? static_cast<size_t>(looked) : size - 1 - static_cast<size_t>(looked);
      if ((*list)[at] != call.args[2]) continue;
      if (skip > 0) {
        --skip;
        continue;
      }
      found.push_back(static_cast<int64_t>(at));
    }
  }
  if (!count) {
    if (found.empty())
      call.reply.null();
    else
      call.reply.integer(found.front());
    return;
  }
  call.reply.array(found.size());
  for (int64_t at : found) call.reply.integer(at);
}

void lmove(Call& call) {
  const bool from_left = side(call.args[3]);
  const bool to_left = side(call.args[4]);
  move(call, from_left, to_left);
}

void rpoplpush(Call& call) { move(call, false, true); }

void lmpop(Call& call) { multi_pop(call, 1, std::nullopt); }

void blpop(Call& call) { blocking_pop(call, true); }
void brpop(Call& call) { blocking_pop(call, false); }

void blmove(Call& call) {
  const bool from_left = side(call.args[3]);
  const bool to_left = side(call.args[4]);
  const double seconds = block_timeout(call.args[5]);
  if (!move(call, from_left, to_left, true)) park(call, {call.args[1]}, seconds, true);
}

void brpoplpush(Call& call) {
  const double seconds = block_timeout(call.args[3]);
  if (!move(call, false, true, true)) park(call, {call.args[1]}, seconds, true);
}

void blmpop(Call& call) { multi_pop(call, 2, block_timeout(call.args[1])); }

}  // namespace mimir::command::lists

namespace mimir::command {

double block_timeout(std::string_view text) {
  auto value = parse_long_double(text);
  if (!value) throw CommandError("ERR timeout is not a float or out of range");
  if (*value < 0) throw CommandError("ERR timeout is negative");
  // An infinite or too large a timeout is out of range (the differential gate's finding).
  if (std::isinf(*value) || *value > static_cast<long double>(LLONG_MAX) / 1000) throw CommandError("ERR timeout is out of range");
  return static_cast<double>(*value);
}

}  // namespace mimir::command
