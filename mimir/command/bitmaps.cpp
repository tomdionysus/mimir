//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The bitmap commands, as Valkey 8 documents them: a bitmap is a string, bit
// 0 the most significant bit of its first byte, and a string grows with zero
// bytes to hold a bit set past its end.

#include <bit>
#include <climits>

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"

namespace mimir::command::bitmaps {

namespace {

// Valkey's proto-max-bulk-len, in bits: 512 MiB of string.
constexpr uint64_t max_bits = 512ull * 1024 * 1024 * 8;

uint64_t bit_offset(std::string_view text) {
  auto value = parse_integer(text);
  if (!value || *value < 0 || static_cast<uint64_t>(*value) >= max_bits) throw CommandError("ERR bit offset is not an integer or out of range");
  return static_cast<uint64_t>(*value);
}

bool get_bit(const std::string& bytes, uint64_t offset) {
  const uint64_t byte = offset / 8;
  if (byte >= bytes.size()) return false;
  return (static_cast<unsigned char>(bytes[byte]) >> (7 - offset % 8)) & 1;
}

void set_bit(std::string& bytes, uint64_t offset, bool on) {
  const uint64_t byte = offset / 8;
  if (byte >= bytes.size()) bytes.resize(byte + 1, '\0');
  auto& b = reinterpret_cast<unsigned char&>(bytes[byte]);
  const unsigned char mask = static_cast<unsigned char>(1u << (7 - offset % 8));
  b = static_cast<unsigned char>(on ? (b | mask) : (b & ~mask));
}

std::string& string_or_create(keyspace::Keyspace& keys, std::string_view key) {
  if (std::string* value = string_value(keys, key)) return *value;
  return std::get<std::string>(keys.put(key, keyspace::Entry{std::string(), std::nullopt}).value);
}

// BITCOUNT's and BITPOS's [start, end] in bytes or bits, clamped to `units`
// units; none when empty.
std::optional<std::pair<int64_t, int64_t>> unit_range(int64_t start, int64_t end, int64_t units) {
  if (start < 0) start += units;
  if (end < 0) end += units;
  if (start < 0) start = 0;
  if (end < 0) end = 0;
  if (end >= units) end = units - 1;
  if (start > end || units == 0) return std::nullopt;
  return std::make_pair(start, end);
}

// BYTE or BIT, as true for bit.
bool unit_is_bit(const std::string& word) {
  if (iequals(word, "BIT")) return true;
  if (iequals(word, "BYTE")) return false;
  throw CommandError(syntax_error);
}

// BITFIELD's types and offsets.
struct FieldType {
  bool is_signed;
  unsigned bits;
};

FieldType field_type(const std::string& text) {
  if (text.size() >= 2 && (text[0] == 'i' || text[0] == 'u' || text[0] == 'I' || text[0] == 'U')) {
    auto bits = parse_integer(std::string_view(text).substr(1));
    const bool is_signed = text[0] == 'i' || text[0] == 'I';
    if (bits && *bits >= 1 && ((is_signed && *bits <= 64) || (!is_signed && *bits <= 63))) return FieldType{is_signed, static_cast<unsigned>(*bits)};
  }
  throw CommandError("ERR Invalid bitfield type. Use something like i16 u8. Note that u64 is not supported but i64 is.");
}

uint64_t field_offset(const std::string& text, const FieldType& type) {
  std::string_view digits = text;
  bool multiply = false;
  if (!digits.empty() && digits[0] == '#') {
    multiply = true;
    digits.remove_prefix(1);
  }
  auto value = parse_integer(digits);
  if (!value || *value < 0) throw CommandError("ERR bit offset is not an integer or out of range");
  uint64_t offset = static_cast<uint64_t>(*value);
  if (multiply) {
    if (offset > max_bits / type.bits) throw CommandError("ERR bit offset is not an integer or out of range");
    offset *= type.bits;
  }
  if (offset + type.bits > max_bits) throw CommandError("ERR bit offset is not an integer or out of range");
  return offset;
}

uint64_t read_field(const std::string& bytes, uint64_t offset, unsigned bits) {
  uint64_t value = 0;
  for (unsigned i = 0; i < bits; ++i) value = (value << 1) | (get_bit(bytes, offset + i) ? 1u : 0u);
  return value;
}

void write_field(std::string& bytes, uint64_t offset, unsigned bits, uint64_t value) {
  for (unsigned i = 0; i < bits; ++i) set_bit(bytes, offset + i, (value >> (bits - 1 - i)) & 1);
}

int64_t as_signed(uint64_t raw, unsigned bits) {
  if (bits == 64) return static_cast<int64_t>(raw);
  const uint64_t sign = 1ull << (bits - 1);
  return (raw & sign) ? static_cast<int64_t>(raw | ~((1ull << bits) - 1)) : static_cast<int64_t>(raw);
}

enum class Overflow { wrap, sat, fail };

// A value in range for the type after an increment, by the overflow rule;
// none under FAIL when it overflows.
std::optional<int64_t> overflowed(const FieldType& type, int64_t current, int64_t by, Overflow rule) {
  const uint64_t mask = type.bits == 64 ? ~0ull : (1ull << type.bits) - 1;
  // The wrapped result: the low bits of the two's-complement sum.
  const uint64_t wrapped = (static_cast<uint64_t>(current) + static_cast<uint64_t>(by)) & mask;
  int64_t sum = 0;
  const bool overflow64 = __builtin_add_overflow(current, by, &sum);
  if (type.is_signed) {
    const int64_t max = type.bits == 64 ? INT64_MAX : static_cast<int64_t>((1ull << (type.bits - 1)) - 1);
    const int64_t min = type.bits == 64 ? INT64_MIN : -static_cast<int64_t>(1ull << (type.bits - 1));
    const bool above = overflow64 ? by > 0 : sum > max;
    const bool below = overflow64 ? by < 0 : sum < min;
    if (!above && !below) return sum;
    if (rule == Overflow::fail) return std::nullopt;
    if (rule == Overflow::sat) return above ? max : min;
    return as_signed(wrapped, type.bits);
  }
  // Unsigned fields are at most 63 bits, so their maximum is an int64.
  const int64_t max = static_cast<int64_t>(mask);
  const bool above = overflow64 ? by > 0 : sum > max;
  const bool below = overflow64 ? by < 0 : sum < 0;
  if (!above && !below) return sum;
  if (rule == Overflow::fail) return std::nullopt;
  if (rule == Overflow::sat) return above ? max : 0;
  return static_cast<int64_t>(wrapped);
}

void bitfield_generic(Call& call, bool read_only) {
  struct Op {
    enum Kind { get, set, incrby } kind;
    FieldType type;
    uint64_t offset;
    int64_t value;
    Overflow overflow;
  };
  std::vector<Op> ops;
  Overflow overflow = Overflow::wrap;
  bool writes = false;
  for (size_t i = 2; i < call.args.size(); ++i) {
    const std::string& word = call.args[i];
    const size_t left = call.args.size() - i - 1;
    if (iequals(word, "GET") && left >= 2) {
      FieldType type = field_type(call.args[i + 1]);
      ops.push_back(Op{Op::get, type, field_offset(call.args[i + 2], type), 0, overflow});
      i += 2;
    } else if (!read_only && (iequals(word, "SET") || iequals(word, "INCRBY")) && left >= 3) {
      FieldType type = field_type(call.args[i + 1]);
      const uint64_t offset = field_offset(call.args[i + 2], type);
      const int64_t value = integer_argument(call.args[i + 3]);
      ops.push_back(Op{iequals(word, "SET") ? Op::set : Op::incrby, type, offset, value, overflow});
      writes = true;
      i += 3;
    } else if (!read_only && iequals(word, "OVERFLOW") && left >= 1) {
      const std::string& how = call.args[++i];
      if (iequals(how, "WRAP"))
        overflow = Overflow::wrap;
      else if (iequals(how, "SAT"))
        overflow = Overflow::sat;
      else if (iequals(how, "FAIL"))
        overflow = Overflow::fail;
      else
        throw CommandError("ERR Invalid OVERFLOW type specified");
    } else if (read_only && (iequals(word, "SET") || iequals(word, "INCRBY") || iequals(word, "OVERFLOW"))) {
      throw CommandError("ERR BITFIELD_RO only supports the GET subcommand");
    } else {
      throw CommandError(syntax_error);
    }
  }
  std::string* existing = string_value(call.keys, call.args[1]);
  std::string empty;
  std::string& bytes = writes ? string_or_create(call.keys, call.args[1]) : (existing ? *existing : empty);
  call.reply.array(ops.size());
  for (const Op& op : ops) {
    const uint64_t raw = read_field(bytes, op.offset, op.type.bits);
    const int64_t current = op.type.is_signed ? as_signed(raw, op.type.bits) : static_cast<int64_t>(raw);
    if (op.kind == Op::get) {
      call.reply.integer(current);
      continue;
    }
    std::optional<int64_t> next;
    if (op.kind == Op::set) {
      // SET writes the value as the type holds it, by the overflow rule; it
      // replies with the old value.
      next = overflowed(op.type, 0, op.value, op.overflow);
      if (!next) {
        call.reply.null();
        continue;
      }
      write_field(bytes, op.offset, op.type.bits, static_cast<uint64_t>(*next));
      call.reply.integer(current);
    } else {
      next = overflowed(op.type, current, op.value, op.overflow);
      if (!next) {
        call.reply.null();
        continue;
      }
      write_field(bytes, op.offset, op.type.bits, static_cast<uint64_t>(*next));
      call.reply.integer(*next);
    }
  }
}

}  // namespace

void setbit(Call& call) {
  const uint64_t offset = bit_offset(call.args[2]);
  const std::string& bit = call.args[3];
  if (bit != "0" && bit != "1") throw CommandError("ERR bit is not an integer or out of range");
  std::string& bytes = string_or_create(call.keys, call.args[1]);
  call.reply.integer(get_bit(bytes, offset) ? 1 : 0);
  set_bit(bytes, offset, bit == "1");
}

void getbit(Call& call) {
  const uint64_t offset = bit_offset(call.args[2]);
  std::string* bytes = string_value(call.keys, call.args[1]);
  call.reply.integer(bytes && get_bit(*bytes, offset) ? 1 : 0);
}

void bitcount(Call& call) {
  if (call.args.size() > 5) throw CommandError(syntax_error);
  std::optional<int64_t> start, end;
  bool bits = false;
  // Valkey 8 takes a start alone, with the end at -1 (the differential gate's finding).
  if (call.args.size() >= 3) {
    start = integer_argument(call.args[2]);
    end = call.args.size() >= 4 ? integer_argument(call.args[3]) : -1;
    if (call.args.size() == 5) bits = unit_is_bit(call.args[4]);
  }
  std::string* bytes = string_value(call.keys, call.args[1]);
  if (!bytes) {
    call.reply.integer(0);
    return;
  }
  const int64_t units = static_cast<int64_t>(bytes->size()) * (bits ? 8 : 1);
  auto span = start ? unit_range(*start, *end, units) : unit_range(0, -1, units);
  if (!span) {
    call.reply.integer(0);
    return;
  }
  int64_t count = 0;
  if (bits) {
    for (int64_t b = span->first; b <= span->second; ++b) count += get_bit(*bytes, static_cast<uint64_t>(b)) ? 1 : 0;
  } else {
    for (int64_t b = span->first; b <= span->second; ++b) count += std::popcount(static_cast<unsigned char>((*bytes)[static_cast<size_t>(b)]));
  }
  call.reply.integer(count);
}

void bitpos(Call& call) {
  const std::string& which = call.args[2];
  if (which != "0" && which != "1") throw CommandError("ERR The bit argument must be 1 or 0.");
  const bool want = which == "1";
  if (call.args.size() > 6) throw CommandError(syntax_error);
  int64_t start = 0, end = -1;
  bool end_given = false;
  bool bits = false;
  if (call.args.size() >= 4) start = integer_argument(call.args[3]);
  if (call.args.size() >= 5) {
    end = integer_argument(call.args[4]);
    end_given = true;
  }
  if (call.args.size() == 6) bits = unit_is_bit(call.args[5]);
  std::string* bytes = string_value(call.keys, call.args[1]);
  if (!bytes) {
    call.reply.integer(want ? -1 : 0);
    return;
  }
  const int64_t units = static_cast<int64_t>(bytes->size()) * (bits ? 8 : 1);
  auto span = unit_range(start, end, units);
  if (!span) {
    call.reply.integer(-1);
    return;
  }
  const int64_t first_bit = bits ? span->first : span->first * 8;
  const int64_t last_bit = bits ? span->second : span->second * 8 + 7;
  for (int64_t b = first_bit; b <= last_bit; ++b) {
    if (get_bit(*bytes, static_cast<uint64_t>(b)) == want) {
      call.reply.integer(b);
      return;
    }
  }
  // Looking for a clear bit with no end given: the string reads as followed
  // by zeros, so the first one past it.
  if (!want && !end_given) {
    call.reply.integer(last_bit + 1);
    return;
  }
  call.reply.integer(-1);
}

void bitop(Call& call) {
  const std::string op = upper(call.args[1]);
  if (op != "AND" && op != "OR" && op != "XOR" && op != "NOT") throw CommandError(syntax_error);
  if (op == "NOT" && call.args.size() != 4) throw CommandError("ERR BITOP NOT must be called with a single source key.");
  std::vector<const std::string*> sources;
  size_t longest = 0;
  static const std::string missing;
  for (size_t i = 3; i < call.args.size(); ++i) {
    const std::string* value = string_value(call.keys, call.args[i]);
    sources.push_back(value ? value : &missing);
    longest = std::max(longest, sources.back()->size());
  }
  std::string result(longest, '\0');
  for (size_t byte = 0; byte < longest; ++byte) {
    const auto at = [&](size_t s) -> unsigned char { return byte < sources[s]->size() ? static_cast<unsigned char>((*sources[s])[byte]) : 0; };
    unsigned char value = at(0);
    if (op == "NOT") {
      value = static_cast<unsigned char>(~value);
    } else {
      for (size_t s = 1; s < sources.size(); ++s) {
        if (op == "AND") value = static_cast<unsigned char>(value & at(s));
        if (op == "OR") value = static_cast<unsigned char>(value | at(s));
        if (op == "XOR") value = static_cast<unsigned char>(value ^ at(s));
      }
    }
    result[byte] = static_cast<char>(value);
  }
  call.reply.integer(static_cast<int64_t>(result.size()));
  if (result.empty())
    call.keys.erase(call.args[2]);
  else
    call.keys.put(call.args[2], keyspace::Entry{std::move(result), std::nullopt});
}

void bitfield(Call& call) { bitfield_generic(call, false); }
void bitfield_ro(Call& call) { bitfield_generic(call, true); }

}  // namespace mimir::command::bitmaps
