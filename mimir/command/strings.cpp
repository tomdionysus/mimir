//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The string commands, as Valkey 8 documents them.

#include <algorithm>
#include <climits>
#include <cmath>
#include <vector>

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"

namespace mimir::command {

// Valkey's proto-max-bulk-len: the largest string SETRANGE and APPEND may make.
inline constexpr uint64_t max_string = 512u * 1024 * 1024;

std::string* string_value(keyspace::Keyspace& keys, std::string_view key) {
  keyspace::Entry* entry = keys.find(key);
  if (!entry) return nullptr;
  if (entry->type() != keyspace::Type::string) throw CommandError(wrong_type);
  return &std::get<std::string>(entry->value);
}

uint64_t absolute_expiry(int64_t value, bool seconds, bool relative, uint64_t now_ms, std::string_view command, bool allow_non_positive) {
  const std::string invalid = "ERR invalid expire time in '" + std::string(command) + "' command";
  if (!allow_non_positive && value <= 0) throw CommandError(invalid);
  int64_t ms = value;
  if (seconds) {
    if (ms > LLONG_MAX / 1000 || ms < LLONG_MIN / 1000) throw CommandError(invalid);
    ms *= 1000;
  }
  if (relative) {
    if (ms > 0 && static_cast<uint64_t>(ms) > static_cast<uint64_t>(LLONG_MAX) - now_ms) throw CommandError(invalid);
    ms += static_cast<int64_t>(now_ms);
  }
  // A time before the epoch has passed as surely as one just gone.
  return ms < 0 ? 0 : static_cast<uint64_t>(ms);
}

namespace strings {

namespace {

// SET's and GETEX's options, as Valkey parses them: each at most once, one
// expiry of the four or KEEPTTL (or PERSIST for GETEX), NX or XX.
struct Options {
  bool nx = false;
  bool xx = false;
  bool get = false;
  bool keepttl = false;
  bool persist = false;
  std::optional<uint64_t> expires_at;
};

Options parse_options(Call& call, size_t from, bool for_set, std::string_view command) {
  Options options;
  bool have_expiry = false;
  const auto& args = call.args;
  for (size_t i = from; i < args.size(); ++i) {
    const std::string& option = args[i];
    const bool has_next = i + 1 < args.size();
    if (for_set && iequals(option, "NX") && !options.xx) {
      options.nx = true;
    } else if (for_set && iequals(option, "XX") && !options.nx) {
      options.xx = true;
    } else if (for_set && iequals(option, "GET")) {
      options.get = true;
    } else if (for_set && iequals(option, "KEEPTTL") && !have_expiry && !options.persist) {
      options.keepttl = true;
    } else if (!for_set && iequals(option, "PERSIST") && !have_expiry) {
      options.persist = true;
    } else if ((iequals(option, "EX") || iequals(option, "PX") || iequals(option, "EXAT") || iequals(option, "PXAT")) && !have_expiry && !options.keepttl &&
               !options.persist && has_next) {
      have_expiry = true;
      const int64_t value = integer_argument(args[++i]);
      const bool seconds = iequals(option, "EX") || iequals(option, "EXAT");
      const bool relative = iequals(option, "EX") || iequals(option, "PX");
      options.expires_at = absolute_expiry(value, seconds, relative, call.keys.now_ms(), command, false);
    } else {
      throw CommandError(syntax_error);
    }
  }
  return options;
}

void incr_by(Call& call, int64_t by) {
  const std::string& key = call.args[1];
  keyspace::Entry* entry = call.keys.find(key);
  int64_t current = 0;
  if (entry) {
    if (entry->type() != keyspace::Type::string) throw CommandError(wrong_type);
    auto parsed = parse_integer(std::get<std::string>(entry->value));
    if (!parsed) throw CommandError(not_integer);
    current = *parsed;
  }
  if ((by < 0 && current < 0 && by < LLONG_MIN - current) || (by > 0 && current > 0 && by > LLONG_MAX - current)) {
    throw CommandError("ERR increment or decrement would overflow");
  }
  const int64_t next = current + by;
  if (entry) {
    std::get<std::string>(entry->value) = format_integer(next);
  } else {
    call.keys.put(key, keyspace::Entry{format_integer(next), std::nullopt});
  }
  call.notify('$', "incrby", key);
  call.reply.integer(next);
}

// The range [start, end] of a string of `length` bytes, Valkey's way; none when empty.
std::optional<std::pair<size_t, size_t>> string_range(int64_t start, int64_t end, size_t length) {
  const int64_t len = static_cast<int64_t>(length);
  if (start < 0 && end < 0 && start > end) return std::nullopt;
  if (start < 0) start = len + start;
  if (end < 0) end = len + end;
  if (start < 0) start = 0;
  if (end < 0) end = 0;
  if (end >= len) end = len - 1;
  if (len == 0 || start > end) return std::nullopt;
  return std::make_pair(static_cast<size_t>(start), static_cast<size_t>(end));
}

}  // namespace

void get(Call& call) {
  std::string* value = string_value(call.keys, call.args[1]);
  if (value)
    call.reply.bulk(*value);
  else
    call.reply.null();
}

Class classify_set(const std::vector<std::string>& args) {
  for (size_t i = 3; i < args.size(); ++i) {
    if (iequals(args[i], "NX") || iequals(args[i], "XX") || iequals(args[i], "GET")) return Class::agreement;
  }
  return Class::merge;
}

void set(Call& call) {
  const std::string& key = call.args[1];
  Options options = parse_options(call, 3, true, "set");
  keyspace::Entry* existing = call.keys.find(key);
  std::optional<std::string> old;
  if (options.get && existing) {
    if (existing->type() != keyspace::Type::string) throw CommandError(wrong_type);
    old = std::get<std::string>(existing->value);
  }
  const bool write = !((options.nx && existing) || (options.xx && !existing));
  if (write) {
    std::optional<uint64_t> expires_at = options.expires_at;
    if (options.keepttl && existing) expires_at = existing->expires_at;
    if (expires_at && *expires_at <= call.keys.now_ms()) {
      // Already past: the value is set and gone at once, as Valkey does.
      call.keys.erase(key);
      call.notify('$', "set", key);
      call.notify('g', "del", key);
    } else {
      call.keys.put(key, keyspace::Entry{call.args[2], expires_at});
      call.notify('$', "set", key);
      if (options.expires_at) call.notify('g', "expire", key);
    }
  }
  if (options.get) {
    if (old)
      call.reply.bulk(*old);
    else
      call.reply.null();
  } else if (write) {
    call.reply.simple("OK");
  } else {
    call.reply.null();
  }
}

void setnx(Call& call) {
  if (call.keys.find(call.args[1])) {
    call.reply.integer(0);
    return;
  }
  call.keys.put(call.args[1], keyspace::Entry{call.args[2], std::nullopt});
  call.notify('$', "set", call.args[1]);
  call.reply.integer(1);
}

namespace {
void set_with_expiry(Call& call, bool seconds, std::string_view command) {
  const int64_t value = integer_argument(call.args[2]);
  const uint64_t at = absolute_expiry(value, seconds, true, call.keys.now_ms(), command, false);
  call.keys.put(call.args[1], keyspace::Entry{call.args[3], at});
  call.notify('$', "set", call.args[1]);
  call.notify('g', "expire", call.args[1]);
  call.reply.simple("OK");
}
}  // namespace

void setex(Call& call) { set_with_expiry(call, true, "setex"); }
void psetex(Call& call) { set_with_expiry(call, false, "psetex"); }

void getset(Call& call) {
  std::string* value = string_value(call.keys, call.args[1]);
  if (value)
    call.reply.bulk(*value);
  else
    call.reply.null();
  call.keys.put(call.args[1], keyspace::Entry{call.args[2], std::nullopt});
  call.notify('$', "set", call.args[1]);
}

void getdel(Call& call) {
  std::string* value = string_value(call.keys, call.args[1]);
  if (!value) {
    call.reply.null();
    return;
  }
  call.reply.bulk(*value);
  call.keys.erase(call.args[1]);
  call.notify('g', "del", call.args[1]);
}

void getex(Call& call) {
  const std::string& key = call.args[1];
  Options options = parse_options(call, 2, false, "getex");
  std::string* value = string_value(call.keys, key);
  if (!value) {
    call.reply.null();
    return;
  }
  call.reply.bulk(*value);
  keyspace::Entry* entry = call.keys.find(key);
  if (options.expires_at) {
    if (*options.expires_at <= call.keys.now_ms()) {
      call.keys.erase(key);
      call.notify('g', "del", key);
    } else {
      keyspace::Entry updated = std::move(*entry);
      updated.expires_at = options.expires_at;
      call.keys.put(key, std::move(updated));
      call.notify('g', "expire", key);
    }
  } else if (options.persist && entry->expires_at) {
    keyspace::Entry updated = std::move(*entry);
    updated.expires_at.reset();
    call.keys.put(key, std::move(updated));
    call.notify('g', "persist", key);
  }
}

void mget(Call& call) {
  call.reply.array(call.args.size() - 1);
  for (size_t i = 1; i < call.args.size(); ++i) {
    keyspace::Entry* entry = call.keys.find(call.args[i]);
    if (entry && entry->type() == keyspace::Type::string)
      call.reply.bulk(std::get<std::string>(entry->value));
    else
      call.reply.null();
  }
}

void mset(Call& call) {
  if (call.args.size() % 2 == 0) throw CommandError("ERR wrong number of arguments for 'mset' command");
  for (size_t i = 1; i < call.args.size(); i += 2) {
    call.keys.put(call.args[i], keyspace::Entry{call.args[i + 1], std::nullopt});
    call.notify('$', "set", call.args[i]);
  }
  call.reply.simple("OK");
}

void msetnx(Call& call) {
  if (call.args.size() % 2 == 0) throw CommandError("ERR wrong number of arguments for 'msetnx' command");
  for (size_t i = 1; i < call.args.size(); i += 2) {
    if (call.keys.find(call.args[i])) {
      call.reply.integer(0);
      return;
    }
  }
  for (size_t i = 1; i < call.args.size(); i += 2) {
    call.keys.put(call.args[i], keyspace::Entry{call.args[i + 1], std::nullopt});
    call.notify('$', "set", call.args[i]);
  }
  call.reply.integer(1);
}

void incr(Call& call) { incr_by(call, 1); }
void decr(Call& call) { incr_by(call, -1); }
void incrby(Call& call) { incr_by(call, integer_argument(call.args[2])); }

void decrby(Call& call) {
  const int64_t by = integer_argument(call.args[2]);
  if (by == LLONG_MIN) throw CommandError("ERR decrement would overflow");
  incr_by(call, -by);
}

void incrbyfloat(Call& call) {
  const std::string& key = call.args[1];
  keyspace::Entry* entry = call.keys.find(key);
  long double current = 0;
  if (entry) {
    if (entry->type() != keyspace::Type::string) throw CommandError(wrong_type);
    auto parsed = parse_long_double(std::get<std::string>(entry->value));
    if (!parsed) throw CommandError(not_float);
    current = *parsed;
  }
  auto by = parse_long_double(call.args[2]);
  if (!by) throw CommandError(not_float);
  const long double next = current + *by;
  if (std::isnan(next) || std::isinf(next)) throw CommandError("ERR increment would produce NaN or Infinity");
  std::string text = format_long_double(next);
  call.reply.bulk(text);
  if (entry)
    std::get<std::string>(entry->value) = std::move(text);
  else
    call.keys.put(key, keyspace::Entry{std::move(text), std::nullopt});
  call.notify('$', "incrbyfloat", key);
}

void append(Call& call) {
  const std::string& key = call.args[1];
  std::string* value = string_value(call.keys, key);
  if (!value) {
    call.keys.put(key, keyspace::Entry{call.args[2], std::nullopt});
    call.notify('$', "append", key);
    call.reply.integer(static_cast<int64_t>(call.args[2].size()));
    return;
  }
  if (value->size() + call.args[2].size() > max_string) throw CommandError("ERR string exceeds maximum allowed size (proto-max-bulk-len)");
  value->append(call.args[2]);
  call.notify('$', "append", key);
  call.reply.integer(static_cast<int64_t>(value->size()));
}

void strlen(Call& call) {
  std::string* value = string_value(call.keys, call.args[1]);
  call.reply.integer(value ? static_cast<int64_t>(value->size()) : 0);
}

void getrange(Call& call) {
  const int64_t start = integer_argument(call.args[2]);
  const int64_t end = integer_argument(call.args[3]);
  std::string* value = string_value(call.keys, call.args[1]);
  if (!value) {
    call.reply.bulk("");
    return;
  }
  auto range = string_range(start, end, value->size());
  if (!range)
    call.reply.bulk("");
  else
    call.reply.bulk(std::string_view(*value).substr(range->first, range->second - range->first + 1));
}

void setrange(Call& call) {
  const std::string& key = call.args[1];
  const int64_t offset = integer_argument(call.args[2]);
  if (offset < 0) throw CommandError("ERR offset is out of range");
  const std::string& piece = call.args[3];
  std::string* value = string_value(call.keys, key);
  if (!value) {
    if (piece.empty()) {
      call.reply.integer(0);
      return;
    }
    if (static_cast<uint64_t>(offset) + piece.size() > max_string) throw CommandError("ERR string exceeds maximum allowed size (proto-max-bulk-len)");
    std::string made(static_cast<size_t>(offset), '\0');
    made.append(piece);
    call.reply.integer(static_cast<int64_t>(made.size()));
    call.keys.put(key, keyspace::Entry{std::move(made), std::nullopt});
    call.notify('$', "setrange", key);
    return;
  }
  if (piece.empty()) {
    call.reply.integer(static_cast<int64_t>(value->size()));
    return;
  }
  if (static_cast<uint64_t>(offset) + piece.size() > max_string) throw CommandError("ERR string exceeds maximum allowed size (proto-max-bulk-len)");
  const size_t at = static_cast<size_t>(offset);
  if (value->size() < at + piece.size()) value->resize(at + piece.size(), '\0');
  value->replace(at, piece.size(), piece);
  call.notify('$', "setrange", key);
  call.reply.integer(static_cast<int64_t>(value->size()));
}

// LCS: the longest common subsequence of two strings, by the table of
// prefixes' LCS lengths; IDX walks the table back from the end, as Valkey
// does, so the ranges it names (and their order) are Valkey's.
void lcs(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  keyspace::Entry* ea = call.keys.find(call.args[1]);
  keyspace::Entry* eb = call.keys.find(call.args[2]);
  if ((ea && ea->type() != keyspace::Type::string) || (eb && eb->type() != keyspace::Type::string)) {
    throw CommandError("ERR The specified keys must contain string values");
  }
  const std::string a = ea ? std::get<std::string>(ea->value) : std::string();
  const std::string b = eb ? std::get<std::string>(eb->value) : std::string();
  bool get_len = false, get_idx = false, with_match_len = false;
  int64_t min_match_len = 0;
  for (size_t j = 3; j < call.args.size(); ++j) {
    const bool more = j + 1 < call.args.size();
    if (iequals(call.args[j], "IDX")) {
      get_idx = true;
    } else if (iequals(call.args[j], "LEN")) {
      get_len = true;
    } else if (iequals(call.args[j], "WITHMATCHLEN")) {
      with_match_len = true;
    } else if (iequals(call.args[j], "MINMATCHLEN") && more) {
      min_match_len = std::max<int64_t>(0, integer_argument(call.args[++j]));
    } else {
      throw CommandError(syntax_error);
    }
  }
  if (get_idx && get_len) throw CommandError("ERR If you want both the length and indexes, please just use IDX.");
  const uint64_t alen = a.size();
  const uint64_t blen = b.size();
  // The table is transient memory, bounded as Valkey bounds it.
  const uint64_t cells = (alen + 1) * (blen + 1);
  const auto& settings = call.database.config_locked();
  const uint64_t limit = std::stoull(settings.at("proto-max-bulk-len"));
  if (cells * sizeof(uint32_t) > limit) throw CommandError("ERR Insufficient memory, transient memory for LCS exceeds proto-max-bulk-len");
  std::vector<uint32_t> table(cells, 0);
  const auto at = [&](uint64_t i, uint64_t j) -> uint32_t& { return table[j + i * (blen + 1)]; };
  for (uint64_t i = 1; i <= alen; ++i) {
    for (uint64_t j = 1; j <= blen; ++j) {
      at(i, j) = a[i - 1] == b[j - 1] ? at(i - 1, j - 1) + 1 : std::max(at(i - 1, j), at(i, j - 1));
    }
  }
  uint32_t idx = at(alen, blen);
  const bool compute = get_idx || !get_len;
  std::string result(compute ? idx : 0, '\0');
  struct Range {
    uint64_t a_start, a_end, b_start, b_end, len;
  };
  std::vector<Range> ranges;
  uint64_t a_start = alen, a_end = 0, b_start = 0, b_end = 0;
  uint64_t i = alen, j = blen;
  while (compute && i > 0 && j > 0) {
    bool emit = false;
    if (a[i - 1] == b[j - 1]) {
      result[idx - 1] = a[i - 1];
      if (a_start == alen) {
        a_start = a_end = i - 1;
        b_start = b_end = j - 1;
      } else if (a_start == i && b_start == j) {
        --a_start;
        --b_start;
      } else {
        emit = true;
      }
      if (a_start == 0 || b_start == 0) emit = true;
      --idx;
      --i;
      --j;
    } else {
      if (at(i - 1, j) > at(i, j - 1))
        --i;
      else
        --j;
      if (a_start != alen) emit = true;
    }
    const uint64_t match_len = a_end - a_start + 1;
    if (emit) {
      if (min_match_len == 0 || match_len >= static_cast<uint64_t>(min_match_len)) ranges.push_back(Range{a_start, a_end, b_start, b_end, match_len});
      a_start = alen;
    }
  }
  if (get_idx) {
    call.reply.map(2);
    call.reply.bulk("matches");
    call.reply.array(ranges.size());
    for (const auto& r : ranges) {
      call.reply.array(with_match_len ? 3 : 2);
      call.reply.array(2);
      call.reply.integer(static_cast<int64_t>(r.a_start));
      call.reply.integer(static_cast<int64_t>(r.a_end));
      call.reply.array(2);
      call.reply.integer(static_cast<int64_t>(r.b_start));
      call.reply.integer(static_cast<int64_t>(r.b_end));
      if (with_match_len) call.reply.integer(static_cast<int64_t>(r.len));
    }
    call.reply.bulk("len");
    call.reply.integer(at(alen, blen));
  } else if (get_len) {
    call.reply.integer(idx);
  } else {
    call.reply.bulk(result);
  }
}

}  // namespace strings
}  // namespace mimir::command
