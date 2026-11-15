//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

// The order HSCAN, SSCAN and ZSCAN page a large collection in (ruling 30):
// each element is numbered as it is added, and a cursor is the number to go on
// from, so an element present for the whole scan is returned exactly once,
// whatever is added or removed meanwhile. The index from number to element is
// built the first time the collection is paged and kept from then on, so a
// collection never scanned carries only its numbers. Not thread-safe: its
// collection's.
namespace mimir::keyspace {

// A hash or sorted set of at most this many entries is answered in one page,
// as Valkey answers one it holds as a listpack (its *-max-listpack-entries).
inline constexpr size_t scan_whole = 128;

template <typename Handle>
class ScanOrder {
 public:
  ScanOrder() = default;
  // A copy numbers on from where the original had reached; its collection's
  // handles are its own, so the index is built again when it is paged.
  ScanOrder(const ScanOrder& other) : _next(other._next) {}
  ScanOrder& operator=(const ScanOrder& other) {
    _next = other._next;
    _by_number.clear();
    _built = false;
    return *this;
  }
  ScanOrder(ScanOrder&&) noexcept = default;
  ScanOrder& operator=(ScanOrder&&) noexcept = default;

  // The number for an element being added.
  uint64_t next() noexcept { return _next++; }
  void added(uint64_t number, Handle handle) {
    if (_built) _by_number.emplace(number, handle);
  }
  void removed(uint64_t number) {
    if (_built) _by_number.erase(number);
  }
  // Every number from zero again (a collection copied element by element).
  void restart() noexcept {
    _next = 0;
    _by_number.clear();
    _built = false;
  }

  // Up to `count` elements from `cursor` (zero: the start), and the cursor to
  // go on from (zero: the end). `each(visit)` calls visit(number, handle) for
  // every element, to build the index the first time.
  template <typename Each>
  std::pair<std::vector<Handle>, uint64_t> page(uint64_t cursor, size_t count, Each each) {
    if (!_built) {
      each([this](uint64_t number, Handle handle) { _by_number.emplace(number, handle); });
      _built = true;
    }
    std::vector<Handle> taken;
    // A cursor is the number to go on from plus one, so no cursor but the
    // first and the last is zero.
    auto it = _by_number.lower_bound(cursor == 0 ? 0 : cursor - 1);
    for (; it != _by_number.end() && taken.size() < count; ++it) taken.push_back(it->second);
    return {std::move(taken), it == _by_number.end() ? 0 : it->first + 1};
  }

 private:
  uint64_t _next = 0;
  std::map<uint64_t, Handle> _by_number;
  bool _built = false;
};

}  // namespace mimir::keyspace
