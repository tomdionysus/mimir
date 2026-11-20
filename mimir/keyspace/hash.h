//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mimir/keyspace/scan_order.h"

// A hash's fields and values, kept in the order the fields were first set: the
// order HGETALL, HKEYS and HVALS return, as Valkey returns a small hash's. A
// field is found by its name in constant time; removing one keeps the order of
// the rest.
namespace mimir::keyspace {

class Hash {
 public:
  using Item = std::pair<std::string, std::string>;
  using const_iterator = std::list<Item>::const_iterator;

  Hash() = default;
  Hash(const Hash& other) { *this = other; }
  Hash& operator=(const Hash& other);
  Hash(Hash&&) noexcept = default;
  Hash& operator=(Hash&&) noexcept = default;

  // Sets `field`; whether it is new. A field already present keeps its place.
  bool set(std::string_view field, std::string value);
  // The value of `field`, or null.
  const std::string* get(std::string_view field) const;
  std::string* get(std::string_view field);
  // Removes `field`; whether it was present.
  bool erase(std::string_view field);

  size_t size() const noexcept { return _items.size(); }
  bool empty() const noexcept { return _items.empty(); }
  const_iterator begin() const noexcept { return _items.begin(); }
  const_iterator end() const noexcept { return _items.end(); }

  // HSCAN's page of a large hash: up to `count` items from `cursor`, and the
  // cursor to go on from (ScanOrder).
  std::pair<std::vector<const_iterator>, uint64_t> page(uint64_t cursor, size_t count);
  // Takes the scan numbers of `before`, the value this one replaces (a write
  // committed by agreement installs a whole new value): an element in both
  // keeps its number, and a new one is numbered on from where `before` had
  // reached, so a scan begun before the write goes on as ruling 30 promises.
  void adopt_numbers(const Hash& before);

 private:
  struct Entry {
    std::list<Item>::iterator item;
    uint64_t number;
  };
  std::list<Item> _items;
  // Keys view the field names held in `_items`, whose nodes never move.
  std::unordered_map<std::string_view, Entry> _index;
  ScanOrder<const_iterator> _order;
};

}  // namespace mimir::keyspace
