//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mimir/keyspace/scan_order.h"

// A sorted set: members ordered by score, then by their bytes for equal
// scores, as Valkey orders them. Each member's score is found in constant
// time; the order is an order-statistic treap, so a member's rank, the member
// at a rank and the count below a score or a name are each O(log n) expected,
// which ZRANK, ZRANGE by rank and ZCOUNT need.
namespace mimir::keyspace {

class SortedSet {
 public:
  struct Item {
    double score;
    std::string member;
  };

  // Every special member is defined where Node is complete.
  SortedSet();
  SortedSet(const SortedSet& other);
  SortedSet& operator=(const SortedSet& other);
  // Defined where Node is complete.
  SortedSet(SortedSet&&) noexcept;
  SortedSet& operator=(SortedSet&&) noexcept;
  ~SortedSet();

  // Sets `member`'s score; whether it is new.
  bool set(std::string_view member, double score);
  std::optional<double> score(std::string_view member) const;
  // Removes `member`; whether it was present.
  bool erase(std::string_view member);

  size_t size() const noexcept { return _scores.size(); }
  bool empty() const noexcept { return _scores.empty(); }

  // `member`'s rank from the lowest, or none.
  std::optional<size_t> rank(std::string_view member) const;
  // The item at `rank`. Precondition: rank < size().
  const Item& at(size_t rank) const;
  // How many items order before (score, member); with `inclusive`, also those
  // equal to it.
  size_t count_below(double score, std::string_view member, bool inclusive) const;
  // How many items have a score below `score` (or at most, with `inclusive`).
  size_t count_below_score(double score, bool inclusive) const;

  // ZSCAN's page of a large sorted set: up to `count` items (member and
  // score) from `cursor`, and the cursor to go on from (ScanOrder).
  std::pair<std::vector<Item>, uint64_t> page(uint64_t cursor, size_t count);
  // Takes the scan numbers of `before`, the value this one replaces (a write
  // committed by agreement installs a whole new value): an element in both
  // keeps its number, and a new one is numbered on from where `before` had
  // reached, so a scan begun before the write goes on as ruling 30 promises.
  void adopt_numbers(const SortedSet& before);

 private:
  struct Entry {
    double score;
    uint64_t number;
  };
  struct Node;
  static size_t size_of(const Node* node) noexcept;
  static void fix(Node* node) noexcept;
  static std::unique_ptr<Node> merge(std::unique_ptr<Node> left, std::unique_ptr<Node> right);
  // Splits into items ordering before (score, member) and the rest.
  static void split(std::unique_ptr<Node> node, double score, std::string_view member, std::unique_ptr<Node>& left, std::unique_ptr<Node>& right);
  static std::unique_ptr<Node> copy(const Node* node);
  void insert(double score, std::string_view member);
  void remove(double score, std::string_view member);

  std::unique_ptr<Node> _root;
  std::unordered_map<std::string, Entry> _scores;
  uint64_t _seed = 0x9e3779b97f4a7c15ull;
  // Handles are the members, as `_scores` holds them: its nodes never move.
  ScanOrder<const std::string*> _order;
};

}  // namespace mimir::keyspace
