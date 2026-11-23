//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mimir/keyspace/limits.h"
#include "mimir/keyspace/scan_order.h"

// A set's members. Valkey answers a small set's members in an order clients
// come to see: a set of at most 512 integers sorted numerically (its intset),
// a set of at most 128 short members in the order they were added (its
// listpack), and anything larger in no particular order. Mimir keeps the same
// three shapes and the same one-way changes between them, so SMEMBERS on a
// small set answers as Valkey's does; a large set answers in the order members
// were added.
namespace mimir::keyspace {

class Set {
 public:
  enum class Shape : uint8_t { integers, small, large };

  Set() = default;
  Set(const Set& other) { *this = other; }
  Set& operator=(const Set& other);
  Set(Set&&) noexcept = default;
  Set& operator=(Set&&) noexcept = default;

  // Adds `member`; whether it is new. The shape changes as Valkey's encoding
  // would under `limits` (the agreed set-max-intset-entries,
  // set-max-listpack-entries and -value).
  bool add(std::string_view member, const Limits& limits = Limits{});
  bool contains(std::string_view member) const { return _index.count(member) > 0; }
  // Removes `member`; whether it was present.
  bool erase(std::string_view member);

  size_t size() const noexcept { return _members.size(); }
  bool empty() const noexcept { return _members.empty(); }
  Shape shape() const noexcept { return _shape; }
  // The shape a payload was dumped in, for the cluster's own records, which
  // keep it whatever the size. Precondition: the members fit it (integers
  // only for `integers`).
  void keep_shape(Shape shape) noexcept { _shape = shape; }
  // The members in the order Valkey would answer them (above).
  std::vector<std::string_view> ordered() const;
  // SSCAN's page of a large set: up to `count` members from `cursor`, and the
  // cursor to go on from (ScanOrder).
  std::pair<std::vector<std::string_view>, uint64_t> page(uint64_t cursor, size_t count);
  // Takes the scan numbers of `before`, the value this one replaces (a write
  // committed by agreement installs a whole new value): an element in both
  // keeps its number, and a new one is numbered on from where `before` had
  // reached, so a scan begun before the write goes on as ruling 30 promises.
  void adopt_numbers(const Set& before);

 private:
  struct Entry {
    std::list<std::string>::iterator member;
    uint64_t number;
  };
  std::list<std::string> _members;
  // Keys view the members held in `_members`, whose nodes never move.
  std::unordered_map<std::string_view, Entry> _index;
  Shape _shape = Shape::integers;
  ScanOrder<std::string_view> _order;
};

}  // namespace mimir::keyspace
