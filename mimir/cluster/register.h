//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dannan/agree/caspaxos.h"

// A key as one of Dannan's agreement registers (stage 3, ruling 26): its name,
// the shard (Dannan's slot) it lives in, and the record its value is.
//
// A shard is a contiguous range of Valkey's 16384 hash slots, so keys sharing a
// hash tag share a shard, as they share a slot in a Valkey cluster. The record
// holds the value in DUMP's form, its absolute expiry, and the operations
// recently applied to it with their replies: a write retried after its accept
// went out finds its own id and answers as the first attempt did, so it is
// applied once (Dannan's agreement review, 2026-11-17).
namespace mimir::cluster {

using Bytes = dannan::agree::Bytes;

// The space Mimir's registers live in, and the capability its nodes advertise.
inline constexpr std::string_view space = "mimir";

// A register's name: the database as two bytes, big-endian, then the key.
Bytes name_of(uint16_t database, std::string_view key);
// The database and key a name holds; none for a name not made by name_of.
std::optional<std::pair<uint16_t, std::string>> parse_name(std::span<const uint8_t> name);

// The shard of `key` among `shards` (at most Dannan's max_slots): its Valkey
// slot, mapped onto the shards in contiguous ranges.
dannan::agree::Slot shard_of(std::string_view key, uint32_t shards);
// The shard a Valkey slot falls in.
dannan::agree::Slot shard_of_slot(uint16_t slot, uint32_t shards);

// One operation applied to a register: its id (node, epoch, sequence, as the
// caller makes it), when it was issued on its node's wall clock, and its reply
// as RESP bytes.
struct Applied {
  std::string id;
  uint64_t issued_ms = 0;
  std::string reply;
  bool operator==(const Applied&) const = default;
};

inline constexpr size_t max_operation_id = 64;
// How long an applied operation is kept, measured against a later operation's
// issue time: longer than any command's deadline and the clocks' disagreement,
// so no retry still running can miss its own id.
inline constexpr uint64_t applied_horizon_ms = 120'000;
// How many applied operations a register keeps at most. A write that would
// need to drop one younger than the horizon is refused instead.
inline constexpr size_t max_applied = 1024;

struct Record {
  // The value as DUMP's payload; none: the key does not exist.
  std::optional<std::string> payload;
  // Unix milliseconds; none: the key never expires.
  std::optional<uint64_t> expires_at;
  // Oldest first.
  std::vector<Applied> applied;
  // How many writes have committed to the key: WATCH's evidence that nothing
  // wrote it between the WATCH and the EXEC.
  uint64_t serial = 0;

  bool empty() const noexcept { return !payload && applied.empty(); }
  // The reply recorded for operation `id`, if it was applied here.
  const Applied* find(std::string_view id) const noexcept;
  bool operator==(const Record&) const = default;
};

// The record's bytes. A record that is empty() is no value at all (none).
std::optional<Bytes> encode(const Record& record);
// The record `bytes` hold; none for bytes that are not one. Untrusted: every
// length is checked against what remains, before anything is allocated.
std::optional<Record> decode(std::span<const uint8_t> bytes);
// The record a register's value holds: an absent value is the empty record;
// none for a value that is not a record.
std::optional<Record> decode_value(const std::optional<Bytes>& value);

// Records `applied` in `record`, after dropping the operations older than the
// horizon before its issue time. False, with the record unchanged, when it
// would hold more than max_applied.
bool note_applied(Record& record, Applied applied);

// The summary Dannan carries for a value (at most max_summary bytes): SHA-256
// of the value's bytes, so two different records never share one.
Bytes summary_of(std::span<const uint8_t> value);

}  // namespace mimir::cluster
