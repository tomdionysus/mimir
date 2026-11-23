//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/keyspace/list.h"

#include <algorithm>
#include <cstdint>
#include <utility>

#include "mimir/keyspace/rdb.h"

namespace mimir::keyspace {

namespace {

// An empty listpack's header and end.
constexpr size_t empty_listpack = 7;
// What a node's size is estimated to grow by for each element, beyond its
// length; and the size no node of a count limit may pass.
constexpr size_t entry_overhead = 8;
constexpr size_t safety_limit = 8192;
// The node sizes list-max-listpack-size -1 to -5 give.
constexpr size_t node_sizes[] = {4096, 8192, 16384, 32768, 65536};

struct Bounds {
  size_t bytes = SIZE_MAX;
  size_t count = SIZE_MAX;
};

Bounds bounds_of(int64_t fill) {
  Bounds b;
  if (fill >= 0)
    b.count = fill == 0 ? 1 : static_cast<size_t>(fill);
  else
    b.bytes = node_sizes[std::min<int64_t>(-fill - 1, 4)];
  return b;
}

// Whether a node of `bytes` and `count` would be past the limits.
bool exceeds(int64_t fill, size_t bytes, size_t count) {
  const Bounds b = bounds_of(fill);
  if (b.bytes != SIZE_MAX) return bytes > b.bytes;
  if (bytes > safety_limit) return true;
  return count > b.count;
}

// An element that takes a plain node of its own.
bool large(size_t length, int64_t fill) { return fill >= 0 ? length > safety_limit : length > bounds_of(fill).bytes; }

size_t size_of(const std::string& element) { return rdb::listpack_element_size(element); }

bool allows_insert(const List::Node& node, size_t length, int64_t fill) {
  if (node.plain || large(length, fill)) return false;
  return !exceeds(fill, node.bytes + length + entry_overhead, node.count + 1);
}

bool allows_merge(const List::Node& a, const List::Node& b, int64_t fill) {
  if (a.plain || b.plain) return false;
  return !exceeds(fill, a.bytes + b.bytes - empty_listpack, a.count + b.count);
}

}  // namespace

std::pair<size_t, size_t> List::locate(size_t index) const {
  size_t node = 0;
  while (index >= _nodes[node].count) index -= _nodes[node++].count;
  return {node, index};
}

size_t List::first_of(size_t node) const {
  size_t first = 0;
  for (size_t i = 0; i < node; ++i) first += _nodes[i].count;
  return first;
}

size_t List::bytes_of(size_t first, size_t count) const {
  size_t bytes = empty_listpack;
  for (size_t i = 0; i < count; ++i) bytes += size_of(_items[first + i]);
  return bytes;
}

void List::grow(size_t add_bytes, size_t add_count, const Limits& limits) {
  if (_quicklist) return;
  const size_t bytes = bytes_of(0, _items.size());
  if (!exceeds(limits.list_max_listpack_size, bytes + add_bytes, _items.size() + add_count)) return;
  // The whole listpack becomes one node, whatever its size.
  _quicklist = true;
  _nodes.clear();
  if (!_items.empty()) _nodes.push_back(Node{_items.size(), bytes, false});
}

void List::settle(bool shrinking, const Limits& limits) {
  if (!_quicklist || _nodes.size() != 1 || _nodes.front().plain) return;
  Bounds b = bounds_of(limits.list_max_listpack_size);
  if (shrinking) {
    if (b.bytes != SIZE_MAX) b.bytes /= 2;
    if (b.count != SIZE_MAX) b.count /= 2;
  }
  if (_nodes.front().bytes > b.bytes || _items.size() > b.count) return;
  make_listpack();
}

void List::push(bool head, std::string value, const Limits& limits) {
  if (_quicklist) {
    const int64_t fill = limits.list_max_listpack_size;
    if (large(value.size(), fill)) {
      const Node plain{1, value.size(), true};
      if (head)
        _nodes.insert(_nodes.begin(), plain);
      else
        _nodes.push_back(plain);
    } else {
      Node* end = _nodes.empty() ? nullptr : head ? &_nodes.front() : &_nodes.back();
      if (end && allows_insert(*end, value.size(), fill)) {
        ++end->count;
        end->bytes += size_of(value);
      } else {
        const Node made{1, empty_listpack + size_of(value), false};
        if (head)
          _nodes.insert(_nodes.begin(), made);
        else
          _nodes.push_back(made);
      }
    }
  }
  if (head)
    _items.push_front(std::move(value));
  else
    _items.push_back(std::move(value));
}

std::string List::pop(bool head) {
  std::string value = head ? std::move(_items.front()) : std::move(_items.back());
  if (head)
    _items.pop_front();
  else
    _items.pop_back();
  if (_quicklist && !_nodes.empty()) {
    const size_t at = head ? 0 : _nodes.size() - 1;
    Node& node = _nodes[at];
    if (node.plain || node.count == 1) {
      _nodes.erase(_nodes.begin() + static_cast<std::ptrdiff_t>(at));
    } else {
      --node.count;
      node.bytes -= size_of(value);
    }
  }
  return value;
}

void List::erase(size_t index) {
  if (_quicklist) {
    const auto [at, offset] = locate(index);
    Node& node = _nodes[at];
    if (node.plain || node.count == 1) {
      _nodes.erase(_nodes.begin() + static_cast<std::ptrdiff_t>(at));
    } else {
      --node.count;
      node.bytes -= size_of(_items[index]);
    }
  }
  _items.erase(_items.begin() + static_cast<std::ptrdiff_t>(index));
}

void List::erase_range(size_t start, size_t count) {
  start = std::min(start, _items.size());
  count = std::min(count, _items.size() - start);
  if (count == 0) return;
  if (_quicklist) {
    auto [at, offset] = locate(start);
    // The elements' positions as they are: they are removed after the nodes.
    size_t index = start;
    size_t left = count;
    while (left > 0) {
      Node& node = _nodes[at];
      const size_t take = std::min(left, node.count - offset);
      if (take == node.count) {
        _nodes.erase(_nodes.begin() + static_cast<std::ptrdiff_t>(at));
      } else {
        for (size_t i = 0; i < take; ++i) node.bytes -= size_of(_items[index + i]);
        node.count -= take;
        ++at;
      }
      index += take;
      left -= take;
      offset = 0;
    }
  }
  _items.erase(_items.begin() + static_cast<std::ptrdiff_t>(start), _items.begin() + static_cast<std::ptrdiff_t>(start + count));
}

void List::join(size_t at) {
  Node& a = _nodes[at];
  const Node& b = _nodes[at + 1];
  a.count += b.count;
  a.bytes = a.bytes + b.bytes - empty_listpack;
  _nodes.erase(_nodes.begin() + static_cast<std::ptrdiff_t>(at + 1));
}

void List::merge_around(size_t center, const Limits& limits) {
  const int64_t fill = limits.list_max_listpack_size;
  // The two before the centre, then the two after it, then the centre with
  // the one before, then whatever that made with the one after.
  if (center >= 2 && allows_merge(_nodes[center - 1], _nodes[center - 2], fill)) {
    join(center - 2);
    --center;
  }
  if (center + 2 < _nodes.size() && allows_merge(_nodes[center + 1], _nodes[center + 2], fill)) join(center + 1);
  size_t target = center;
  if (center >= 1 && allows_merge(_nodes[center], _nodes[center - 1], fill)) {
    join(center - 1);
    target = center - 1;
  }
  if (target + 1 < _nodes.size() && allows_merge(_nodes[target], _nodes[target + 1], fill)) join(target);
}

void List::insert(size_t pivot, bool after, std::string value, const Limits& limits) {
  const size_t position = pivot + (after ? 1 : 0);
  if (!_quicklist) {
    _items.insert(_items.begin() + static_cast<std::ptrdiff_t>(position), std::move(value));
    return;
  }
  const int64_t fill = limits.list_max_listpack_size;
  const size_t length = value.size();
  const auto [at, offset] = locate(pivot);
  const Node node = _nodes[at];
  const bool full = !allows_insert(node, length, fill);
  const bool at_tail = after && offset == node.count - 1;
  const bool at_head = !after && offset == 0;
  const bool room_next = at_tail && at + 1 < _nodes.size() && allows_insert(_nodes[at + 1], length, fill);
  const bool room_prev = at_head && at > 0 && allows_insert(_nodes[at - 1], length, fill);
  const size_t element_bytes = size_of(value);
  _items.insert(_items.begin() + static_cast<std::ptrdiff_t>(position), std::move(value));
  const auto place = [this](size_t index, Node made) { _nodes.insert(_nodes.begin() + static_cast<std::ptrdiff_t>(index), made); };
  // A node of `count` elements from `first`, packed.
  const auto packed = [this](size_t first, size_t count) { return Node{count, bytes_of(first, count), false}; };
  const size_t first = first_of(at);
  if (large(length, fill)) {
    const Node plain{1, length, true};
    if (node.plain || at_tail || at_head) {
      place(after ? at + 1 : at, plain);
      return;
    }
    // Split at the pivot: its side keeps the pivot, the value between.
    if (after) {
      _nodes[at] = packed(first, offset + 1);
      place(at + 1, plain);
      place(at + 2, packed(first + offset + 2, node.count - offset - 1));
    } else {
      _nodes[at] = packed(first + offset + 1, node.count - offset);
      place(at, plain);
      place(at, packed(first, offset));
    }
    return;
  }
  if (!full) {
    ++_nodes[at].count;
    _nodes[at].bytes += element_bytes;
  } else if (at_tail && room_next) {
    ++_nodes[at + 1].count;
    _nodes[at + 1].bytes += element_bytes;
  } else if (at_head && room_prev) {
    ++_nodes[at - 1].count;
    _nodes[at - 1].bytes += element_bytes;
  } else if (at_tail || at_head) {
    place(after ? at + 1 : at, Node{1, empty_listpack + element_bytes, false});
  } else if (after) {
    // The node splits after the pivot; the value heads the part split off.
    _nodes[at] = packed(first, offset + 1);
    place(at + 1, packed(first + offset + 1, node.count - offset));
    merge_around(at, limits);
  } else {
    // The node splits before the pivot; the value ends the part split off.
    _nodes[at] = packed(first + offset + 1, node.count - offset);
    place(at, packed(first, offset + 1));
    merge_around(at + 1, limits);
  }
}

void List::replace(size_t index, std::string value, const Limits& limits) {
  if (!_quicklist) {
    _items[index] = std::move(value);
    return;
  }
  const int64_t fill = limits.list_max_listpack_size;
  const auto [at, offset] = locate(index);
  Node& node = _nodes[at];
  const bool big = large(value.size(), fill);
  if (!node.plain && !big) {
    node.bytes = node.bytes - size_of(_items[index]) + size_of(value);
    _items[index] = std::move(value);
    return;
  }
  if (node.plain && big) {
    node.bytes = value.size();
    _items[index] = std::move(value);
    return;
  }
  if (node.plain) {
    // A small value for a plain node: inserted after it, the plain node gone.
    insert(index, true, std::move(value), limits);
    erase(index);
    return;
  }
  // A large value in a packed node: split after the element, a plain node
  // for the value after it, then the element itself removed.
  const Node before = node;
  const size_t first = first_of(at);
  _items[index] = std::move(value);
  std::vector<Node> made;
  if (offset + 1 > 1) made.push_back(Node{offset, bytes_of(first, offset), false});
  made.push_back(Node{1, _items[index].size(), true});
  if (offset + 1 < before.count) made.push_back(Node{before.count - offset - 1, bytes_of(first + offset + 1, before.count - offset - 1), false});
  _nodes.erase(_nodes.begin() + static_cast<std::ptrdiff_t>(at));
  _nodes.insert(_nodes.begin() + static_cast<std::ptrdiff_t>(at), made.begin(), made.end());
  // The original node lost its last element: merges around the new one, but
  // not where the original node went with it.
  if (offset > 0) merge_around(at + 1, limits);
}

List List::new_quicklist() {
  List list;
  list._quicklist = true;
  return list;
}

List List::from_nodes(std::vector<std::vector<std::string>> nodes, const std::vector<bool>& plain) {
  List list;
  list._quicklist = true;
  for (size_t n = 0; n < nodes.size(); ++n) {
    if (nodes[n].empty()) continue;
    const size_t first = list._items.size();
    for (auto& element : nodes[n]) list._items.push_back(std::move(element));
    const size_t count = list._items.size() - first;
    const bool is_plain = n < plain.size() && plain[n];
    list._nodes.push_back(Node{count, is_plain ? list._items[first].size() : list.bytes_of(first, count), is_plain});
  }
  return list;
}

List List::listpack(std::deque<std::string> elements) {
  List list;
  list._items = std::move(elements);
  return list;
}

void List::make_listpack() noexcept {
  _quicklist = false;
  _nodes.clear();
}

}  // namespace mimir::keyspace
