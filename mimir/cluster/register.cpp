//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/cluster/register.h"

#include <algorithm>

#include "dannan/identity/crypto.h"
#include "dannan/net/codec.h"
#include "mimir/keyspace/slot.h"

namespace mimir::cluster {

namespace {

inline constexpr uint8_t format = 1;
inline constexpr uint8_t has_payload = 1;
inline constexpr uint8_t has_expiry = 2;
// Valkey's proto-max-bulk-len: no value or reply is larger.
inline constexpr size_t max_bulk = 512ull * 1024 * 1024;

std::span<const uint8_t> bytes_of(std::string_view text) { return {reinterpret_cast<const uint8_t*>(text.data()), text.size()}; }

std::string text_of(std::span<const uint8_t> bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }

}  // namespace

Bytes name_of(uint16_t database, std::string_view key) {
  // Sized once: GCC 14 at -O3 misreads growing a vector just made.
  Bytes name(2 + key.size());
  name[0] = static_cast<uint8_t>(database >> 8);
  name[1] = static_cast<uint8_t>(database);
  std::copy(key.begin(), key.end(), name.begin() + 2);
  return name;
}

std::optional<std::pair<uint16_t, std::string>> parse_name(std::span<const uint8_t> name) {
  if (name.size() < 2) return std::nullopt;
  const auto database = static_cast<uint16_t>((name[0] << 8) | name[1]);
  return std::pair{database, text_of(name.subspan(2))};
}

dannan::agree::Slot shard_of_slot(uint16_t slot, uint32_t shards) { return static_cast<dannan::agree::Slot>((uint64_t{slot} * shards) / keyspace::slot_count); }

dannan::agree::Slot shard_of(std::string_view key, uint32_t shards) { return shard_of_slot(keyspace::key_slot(key), shards); }

const Applied* Record::find(std::string_view id) const noexcept {
  const auto found = std::find_if(applied.begin(), applied.end(), [id](const Applied& op) { return op.id == id; });
  return found == applied.end() ? nullptr : &*found;
}

std::optional<Bytes> encode(const Record& record) {
  if (record.empty()) return std::nullopt;
  dannan::net::Writer out;
  out.u8(format);
  out.u8(static_cast<uint8_t>((record.payload ? has_payload : 0) | (record.expires_at ? has_expiry : 0)));
  if (record.expires_at) out.u64(*record.expires_at);
  if (record.payload) out.bytes(bytes_of(*record.payload));
  out.u32(static_cast<uint32_t>(record.applied.size()));
  for (const auto& op : record.applied) {
    out.bytes(bytes_of(op.id));
    out.u64(op.issued_ms);
    out.bytes(bytes_of(op.reply));
  }
  return out.take();
}

std::optional<Record> decode(std::span<const uint8_t> bytes) {
  try {
    dannan::net::Reader in(bytes);
    if (in.u8() != format) return std::nullopt;
    const uint8_t flags = in.u8();
    if (flags & ~(has_payload | has_expiry)) return std::nullopt;
    Record record;
    if (flags & has_expiry) record.expires_at = in.u64();
    if (flags & has_payload) record.payload = text_of(in.view_bytes(max_bulk));
    const uint32_t count = in.u32();
    if (count > max_applied) return std::nullopt;
    record.applied.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      Applied op;
      op.id = text_of(in.view_bytes(max_operation_id));
      op.issued_ms = in.u64();
      op.reply = text_of(in.view_bytes(max_bulk));
      record.applied.push_back(std::move(op));
    }
    in.finish();
    // An empty record is never a value: encode() gives none for it.
    if (record.empty()) return std::nullopt;
    return record;
  } catch (const dannan::net::DecodeError&) {
    return std::nullopt;
  }
}

std::optional<Record> decode_value(const std::optional<Bytes>& value) {
  if (!value) return Record{};
  return decode(*value);
}

bool note_applied(Record& record, Applied applied) {
  const auto live = [&applied](const Applied& op) { return op.issued_ms + applied_horizon_ms >= applied.issued_ms; };
  if (static_cast<size_t>(std::count_if(record.applied.begin(), record.applied.end(), live)) >= max_applied) return false;
  std::erase_if(record.applied, [&live](const Applied& op) { return !live(op); });
  record.applied.push_back(std::move(applied));
  return true;
}

Bytes summary_of(std::span<const uint8_t> value) {
  const auto digest = dannan::identity::sha256(value);
  return Bytes(digest.bytes.begin(), digest.bytes.end());
}

}  // namespace mimir::cluster
