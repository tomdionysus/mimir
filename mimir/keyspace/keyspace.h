//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "dannan/contract/time_source.h"

// One node's keys and their values: what a shard holds in memory. Keys are
// kept in the order of their position, which is their hash slot then a 64-bit
// hash of the key, so a SCAN cursor is a position: every key present for the
// whole of a scan is returned, whatever is inserted or removed meanwhile, and
// a slot's keys are contiguous for the day a slot moves between shards.
//
// A key's expiry is an absolute wall-clock time in Unix milliseconds, as Valkey
// keeps it, so replicas that hold the same expiry agree on when it passes. An
// expired key is removed when it is found (lazy expiry) and by a bounded step
// at the speculative class (active expiry). Not thread-safe: its owner
// serialises access.
namespace mimir::keyspace {

enum class Type : uint8_t { string, list, hash, set, zset };

// "string", "list", "hash", "set", "zset": TYPE's words.
std::string_view type_name(Type) noexcept;

using List = std::deque<std::string>;

struct Entry {
  // In Type's order: the variant's index is the type.
  std::variant<std::string, List> value;
  // Unix milliseconds; none: the key never expires.
  std::optional<uint64_t> expires_at;

  Type type() const noexcept { return static_cast<Type>(value.index()); }
};

// A key's place in the keyspace: (slot << 48) | (FNV-1a of the key >> 16).
uint64_t position_of(std::string_view key) noexcept;

struct ScanPage {
  std::vector<std::string> keys;
  // Where the next page starts; zero: the scan is complete.
  uint64_t cursor = 0;
};

class Keyspace {
 public:
  explicit Keyspace(const dannan::contract::WallClock& wall);

  // The live entry for `key`, or null. An expired entry found here is removed.
  Entry* find(std::string_view key);
  // Whether `key` is live, without removing anything expired.
  bool contains(std::string_view key) const;
  // Sets `key` to `entry`, replacing what was there; returns the entry stored.
  Entry& put(std::string_view key, Entry entry);
  // Removes `key`; whether a live entry was removed.
  bool erase(std::string_view key);
  // Moves `from`'s entry to `to`, replacing what `to` held. Precondition: `from` is live.
  void rename(std::string_view from, std::string_view to);
  void clear() noexcept;

  // Live and not yet removed expired keys alike: DBSIZE's answer is approximate
  // in the same way as Valkey's.
  size_t size() const noexcept { return _keys.size(); }
  size_t expiring() const noexcept { return _expiring; }
  uint64_t now_ms() const noexcept { return _wall.unix_ms(); }

  // Up to `count` keys from `cursor` on, of `type` when given, live only. The
  // page may be shorter than `count` and still not the last.
  ScanPage scan(uint64_t cursor, size_t count, std::optional<Type> type = std::nullopt);
  // Every live key: KEYS. The caller filters.
  std::vector<std::string> keys();
  // A live key chosen at random, or none: RANDOMKEY. `seed` picks it.
  std::optional<std::string> random_key(uint64_t seed);

  // Active expiry: examines at most `budget` keys that carry an expiry, from
  // where the last step stopped, and removes those whose time has passed.
  // Returns how many it removed.
  size_t expire_step(size_t budget);

 private:
  using Key = std::pair<uint64_t, std::string>;
  struct Compare {
    using is_transparent = void;
    bool operator()(const Key& a, const Key& b) const noexcept { return a < b; }
    bool operator()(const Key& a, const std::pair<uint64_t, std::string_view>& b) const noexcept {
      return a.first != b.first ? a.first < b.first : std::string_view(a.second) < b.second;
    }
    bool operator()(const std::pair<uint64_t, std::string_view>& a, const Key& b) const noexcept {
      return a.first != b.first ? a.first < b.first : a.second < std::string_view(b.second);
    }
  };
  using Map = std::map<Key, Entry, Compare>;

  bool expired(const Entry& entry) const noexcept { return entry.expires_at && *entry.expires_at <= _wall.unix_ms(); }
  void forget(Map::iterator it);

  const dannan::contract::WallClock& _wall;
  Map _keys;
  size_t _expiring = 0;
  // Where active expiry resumes.
  uint64_t _expire_cursor = 0;
};

}  // namespace mimir::keyspace
