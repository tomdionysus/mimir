//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "mimir/keyspace/limits.h"

// A list's elements, and its encoding as Valkey 8.1.1 holds it: a listpack,
// or a quicklist of nodes, each a listpack of a few elements or a plain node
// of one large element. A quicklist's nodes follow the history of its writes
// (where a push lands, where an insert splits a node), so OBJECT ENCODING and
// DUMP's bytes answer as Valkey's whatever the history (the maintainer,
// 2026-11-23: exact, sticky encodings). The rules are Valkey's quicklist's,
// learnt from its documented behaviour, its source's description and probes,
// and written here anew.
namespace mimir::keyspace {

class List {
 public:
  struct Node {
    size_t count = 0;
    // A packed node's listpack bytes; a plain node's element's length.
    size_t bytes = 0;
    bool plain = false;
  };
  using const_iterator = std::deque<std::string>::const_iterator;

  List() = default;
  // A listpack of these elements.
  List(std::initializer_list<std::string> elements) : _items(elements) {}

  size_t size() const noexcept { return _items.size(); }
  bool empty() const noexcept { return _items.empty(); }
  const std::string& operator[](size_t index) const { return _items[index]; }
  const std::string& front() const { return _items.front(); }
  const std::string& back() const { return _items.back(); }
  const_iterator begin() const noexcept { return _items.begin(); }
  const_iterator end() const noexcept { return _items.end(); }
  bool operator==(const List& other) const { return _items == other._items; }

  bool quicklist() const noexcept { return _quicklist; }
  // A quicklist's nodes, in order; none for a listpack.
  const std::vector<Node>& nodes() const noexcept { return _nodes; }

  // Before elements are added (a push, LINSERT, LSET, LMOVE's destination):
  // a listpack whose bytes and count with them would pass the agreed
  // list-max-listpack-size becomes a quicklist of one node holding it whole.
  void grow(size_t add_bytes, size_t add_count, const Limits& limits);
  // After elements are removed (`shrinking`: a pop, LREM, LTRIM, LSET), or
  // when a list is made whole (SORT STORE, a client's RESTORE): a quicklist of
  // a single packed node within the limit (half of it when shrinking) becomes
  // a listpack.
  void settle(bool shrinking, const Limits& limits);

  // An element at the head or the tail: into the end node if it has room,
  // else a node of its own.
  void push(bool head, std::string value, const Limits& limits);
  // The head's or the tail's element, removed: an emptied node goes.
  std::string pop(bool head);
  // The element at `index` removed (LREM), as pop.
  void erase(size_t index);
  // `count` elements from `start` removed (LTRIM), node by node, no merging.
  void erase_range(size_t start, size_t count);
  // LINSERT: `value` before or after the element at `pivot`.
  void insert(size_t pivot, bool after, std::string value, const Limits& limits);
  // LSET: the element at `index` becomes `value`.
  void replace(size_t index, std::string value, const Limits& limits);

  // A quicklist made empty, to be filled at the tail (SORT STORE's result).
  static List new_quicklist();
  // A quicklist of the nodes a payload held, as given: each element list,
  // plain or packed (a client's RESTORE settles it after; the cluster's own
  // records keep it, or make it a listpack when it was one).
  static List from_nodes(std::vector<std::vector<std::string>> nodes, const std::vector<bool>& plain);
  // A listpack of these elements.
  static List listpack(std::deque<std::string> elements);
  // This list as a listpack, whatever it was (a record that held one).
  void make_listpack() noexcept;

 private:
  // The node holding `index`, and its offset there.
  std::pair<size_t, size_t> locate(size_t index) const;
  // The index of `node`'s first element.
  size_t first_of(size_t node) const;
  // A packed node's bytes from its elements.
  size_t bytes_of(size_t first, size_t count) const;
  void merge_around(size_t center, const Limits& limits);
  // Joins node `at` with node `at + 1`.
  void join(size_t at);

  std::deque<std::string> _items;
  bool _quicklist = false;
  std::vector<Node> _nodes;
};

}  // namespace mimir::keyspace
