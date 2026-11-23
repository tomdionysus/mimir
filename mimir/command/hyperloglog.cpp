//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The HyperLogLog commands, as Valkey 8 documents them, over the shared
// representation (keyspace/hll.h). Registers only ever grow, so PFADD and
// PFMERGE are joins: they merge in the cluster without agreement.

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"
#include "mimir/keyspace/hll.h"

namespace mimir::command::hyperloglog {

namespace {

namespace hll = keyspace::hll;

constexpr const char* not_hll = "WRONGTYPE Key is not a valid HyperLogLog string value.";
constexpr const char* corrupt = "INVALIDOBJ Corrupted HLL object detected";

// The value at `key` as a HyperLogLog, or null when absent.
std::string* hll_value(keyspace::Keyspace& keys, std::string_view key) {
  std::string* value = string_value(keys, key);
  if (value && !hll::looks_valid(*value)) throw CommandError(not_hll);
  return value;
}

// The agreed hll-sparse-max-bytes. Called under the database's lock, as every
// handler is.
size_t sparse_limit(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS { return call.database.limits_locked().hll_sparse_max_bytes; }

hll::Registers registers_of(const std::string& value) {
  auto regs = hll::decode(value);
  if (!regs) throw CommandError(corrupt);
  return *regs;
}

}  // namespace

void pfadd(Call& call) {
  std::string* value = hll_value(call.keys, call.args[1]);
  bool changed = false;
  if (!value) {
    value = &std::get<std::string>(call.keys.put(call.args[1], keyspace::Entry{hll::make_empty(), std::nullopt}).value);
    changed = true;
  }
  hll::Registers regs = registers_of(*value);
  bool grew = false;
  for (size_t i = 2; i < call.args.size(); ++i) {
    auto [index, count] = hll::position(call.args[i]);
    if (regs[index] < count) {
      regs[index] = count;
      grew = true;
    }
  }
  if (grew) {
    // A dense value stays dense, as Valkey's never goes back; a sparse one
    // past the agreed hll-sparse-max-bytes becomes dense.
    *value = hll::encode(regs, hll::is_dense(*value), sparse_limit(call));
    changed = true;
  }
  if (changed) call.notify('$', "pfadd", call.args[1]);
  call.reply.integer(changed ? 1 : 0);
}

void pfcount(Call& call) {
  if (call.args.size() == 2) {
    std::string* value = hll_value(call.keys, call.args[1]);
    if (!value) {
      call.reply.integer(0);
      return;
    }
    if (auto cached = hll::cached(*value)) {
      call.reply.integer(static_cast<int64_t>(*cached));
      return;
    }
    const uint64_t count = hll::estimate(registers_of(*value));
    // The cache is written back, as Valkey writes it.
    hll::store_cache(*value, count);
    call.reply.integer(static_cast<int64_t>(count));
    return;
  }
  hll::Registers merged{};
  for (size_t i = 1; i < call.args.size(); ++i) {
    std::string* value = hll_value(call.keys, call.args[i]);
    if (!value) continue;
    const hll::Registers regs = registers_of(*value);
    for (size_t r = 0; r < hll::registers; ++r) merged[r] = std::max(merged[r], regs[r]);
  }
  call.reply.integer(static_cast<int64_t>(hll::estimate(merged)));
}

void pfmerge(Call& call) {
  hll::Registers merged{};
  bool dense = false;
  for (size_t i = 1; i < call.args.size(); ++i) {
    std::string* value = hll_value(call.keys, call.args[i]);
    if (!value) continue;
    dense = dense || hll::is_dense(*value);
    const hll::Registers regs = registers_of(*value);
    for (size_t r = 0; r < hll::registers; ++r) merged[r] = std::max(merged[r], regs[r]);
  }
  keyspace::Entry* existing = call.keys.find(call.args[1]);
  const std::optional<uint64_t> expires_at = existing ? existing->expires_at : std::nullopt;
  call.keys.put(call.args[1], keyspace::Entry{hll::encode(merged, dense, sparse_limit(call)), expires_at});
  call.notify('$', "pfadd", call.args[1]);
  call.reply.simple("OK");
}

namespace {

// A sparse value's opcodes as PFDEBUG DECODE names them: "Z:n" a long run of
// zeros, "z:n" a short one, "v:value,n" a run of one value; none for a body
// that is not sparse opcodes.
std::optional<std::string> decode_sparse(std::string_view value) {
  std::string out;
  for (size_t at = hll::header_size; at < value.size(); ++at) {
    const auto byte = static_cast<uint8_t>(value[at]);
    if (!out.empty()) out.push_back(' ');
    if ((byte & 0xc0) == 0x00) {
      out += "z:" + std::to_string((byte & 0x3f) + 1);
    } else if ((byte & 0xc0) == 0x40) {
      if (++at >= value.size()) return std::nullopt;
      out += "Z:" + std::to_string((((byte & 0x3f) << 8) | static_cast<uint8_t>(value[at])) + 1);
    } else {
      out += "v:" + std::to_string(((byte >> 2) & 0x1f) + 1) + "," + std::to_string((byte & 0x03) + 1);
    }
  }
  return out;
}

// `value` made dense, its cached cardinality kept as Valkey keeps it.
void make_dense(std::string& value) {
  std::string dense = hll::encode(registers_of(value), true);
  std::copy(value.begin() + 8, value.begin() + hll::header_size, dense.begin() + 8);
  value = std::move(dense);
}

}  // namespace

// PFDEBUG GETREG|DECODE|ENCODING|TODENSE key: the key is checked first, then
// the subcommand, as Valkey checks them.
void pfdebug(Call& call) {
  std::string* value = hll_value(call.keys, call.args[2]);
  if (!value) throw CommandError("ERR The specified key does not exist");
  const std::string sub = lower(call.args[1]);
  if (sub == "getreg") {
    if (!hll::is_dense(*value)) make_dense(*value);
    const hll::Registers regs = registers_of(*value);
    call.reply.array(hll::registers);
    for (const uint8_t r : regs) call.reply.integer(r);
  } else if (sub == "decode") {
    if (hll::is_dense(*value)) throw CommandError("ERR HLL encoding is not sparse");
    const auto decoded = decode_sparse(*value);
    if (!decoded) throw CommandError(corrupt);
    call.reply.simple(*decoded);
  } else if (sub == "encoding") {
    call.reply.simple(hll::is_dense(*value) ? "dense" : "sparse");
  } else if (sub == "todense") {
    const bool converted = !hll::is_dense(*value);
    if (converted) make_dense(*value);
    call.reply.integer(converted ? 1 : 0);
  } else {
    throw CommandError("ERR Unknown PFDEBUG subcommand '" + call.args[1] + "'");
  }
}

}  // namespace mimir::command::hyperloglog
