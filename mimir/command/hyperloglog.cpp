//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The HyperLogLog commands, as Valkey 8 documents them, over the shared
// representation (keyspace/hll.h). Registers only ever grow, so PFADD and
// PFMERGE are joins: they merge in the cluster without agreement.

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
    // A dense value stays dense, as Valkey's never goes back.
    *value = hll::encode(regs, hll::is_dense(*value));
    changed = true;
  }
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
  call.keys.put(call.args[1], keyspace::Entry{hll::encode(merged, dense), expires_at});
  call.reply.simple("OK");
}

}  // namespace mimir::command::hyperloglog
