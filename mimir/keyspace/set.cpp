//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/keyspace/set.h"

#include <algorithm>
#include <charconv>
#include <optional>

namespace mimir::keyspace {

namespace {

// Whether Valkey would hold `member` in an intset: a 64-bit integer written in
// its canonical form (no '+', no leading zeros, not "-0").
std::optional<int64_t> as_integer(std::string_view member) {
  if (member.empty() || member.size() > 20) return std::nullopt;
  int64_t value = 0;
  auto [end, error] = std::from_chars(member.data(), member.data() + member.size(), value);
  if (error != std::errc() || end != member.data() + member.size()) return std::nullopt;
  // Canonical only: what reads back is what was written.
  if (std::to_string(value) != member) return std::nullopt;
  return value;
}

}  // namespace

Set& Set::operator=(const Set& other) {
  if (this == &other) return *this;
  _index.clear();
  _members = other._members;
  _shape = other._shape;
  _order.restart();
  _index.reserve(_members.size());
  for (auto it = _members.begin(); it != _members.end(); ++it) _index.emplace(std::string_view(*it), Entry{it, _order.next()});
  return *this;
}

bool Set::add(std::string_view member, const Limits& limits) {
  if (_index.count(member)) return false;
  // The one-way changes of shape, as Valkey's encodings change, by the
  // agreed limits.
  if (_shape == Shape::integers && (!as_integer(member) || _members.size() + 1 > limits.set_max_intset_entries)) {
    bool fits_small = _members.size() + 1 <= limits.set_max_listpack_entries && member.size() <= limits.set_max_listpack_value;
    _shape = fits_small ? Shape::small : Shape::large;
    // The members keep the intset's sorted order, and what is added later comes
    // after them, as Valkey's conversion copies them (the differential gate's finding).
    _members.sort([](const std::string& a, const std::string& b) { return *as_integer(a) < *as_integer(b); });
  } else if (_shape == Shape::small && (_members.size() + 1 > limits.set_max_listpack_entries || member.size() > limits.set_max_listpack_value)) {
    _shape = Shape::large;
  }
  _members.emplace_back(member);
  auto it = std::prev(_members.end());
  const uint64_t number = _order.next();
  _index.emplace(std::string_view(*it), Entry{it, number});
  _order.added(number, std::string_view(*it));
  return true;
}

bool Set::erase(std::string_view member) {
  auto found = _index.find(member);
  if (found == _index.end()) return false;
  const Entry entry = found->second;
  _order.removed(entry.number);
  _index.erase(found);
  _members.erase(entry.member);
  return true;
}

std::pair<std::vector<std::string_view>, uint64_t> Set::page(uint64_t cursor, size_t count) {
  return _order.page(cursor, count, [this](auto visit) {
    for (const auto& [name, entry] : _index) visit(entry.number, std::string_view(*entry.member));
  });
}

std::vector<std::string_view> Set::ordered() const {
  std::vector<std::string_view> out;
  out.reserve(_members.size());
  for (const auto& member : _members) out.emplace_back(member);
  if (_shape == Shape::integers) {
    std::sort(out.begin(), out.end(), [](std::string_view a, std::string_view b) { return *as_integer(a) < *as_integer(b); });
  }
  return out;
}

void Set::adopt_numbers(const Set& before) {
  _order.resume_from(before._order.reached());
  for (const auto& member : _members) {
    auto& entry = _index.at(member);
    const auto found = before._index.find(member);
    entry.number = found != before._index.end() ? found->second.number : _order.next();
  }
}

}  // namespace mimir::keyspace
