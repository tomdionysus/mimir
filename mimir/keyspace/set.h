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
#include <vector>

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
  // Valkey's set-max-intset-entries, set-max-listpack-entries and -value.
  static constexpr size_t max_intset = 512;
  static constexpr size_t max_listpack = 128;
  static constexpr size_t max_listpack_value = 64;

  enum class Shape : uint8_t { integers, small, large };

  Set() = default;
  Set(const Set& other) { *this = other; }
  Set& operator=(const Set& other);
  Set(Set&&) noexcept = default;
  Set& operator=(Set&&) noexcept = default;

  // Adds `member`; whether it is new.
  bool add(std::string_view member);
  bool contains(std::string_view member) const { return _index.count(member) > 0; }
  // Removes `member`; whether it was present.
  bool erase(std::string_view member);

  size_t size() const noexcept { return _members.size(); }
  bool empty() const noexcept { return _members.empty(); }
  Shape shape() const noexcept { return _shape; }
  // The members in the order Valkey would answer them (above).
  std::vector<std::string_view> ordered() const;

 private:
  std::list<std::string> _members;
  // Keys view the members held in `_members`, whose nodes never move.
  std::unordered_map<std::string_view, std::list<std::string>::iterator> _index;
  Shape _shape = Shape::integers;
};

}  // namespace mimir::keyspace
