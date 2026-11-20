//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/keyspace/hash.h"

namespace mimir::keyspace {

Hash& Hash::operator=(const Hash& other) {
  if (this == &other) return *this;
  _index.clear();
  _items = other._items;
  _order.restart();
  _index.reserve(_items.size());
  for (auto it = _items.begin(); it != _items.end(); ++it) _index.emplace(std::string_view(it->first), Entry{it, _order.next()});
  return *this;
}

bool Hash::set(std::string_view field, std::string value) {
  auto found = _index.find(field);
  if (found != _index.end()) {
    found->second.item->second = std::move(value);
    return false;
  }
  _items.emplace_back(std::string(field), std::move(value));
  auto it = std::prev(_items.end());
  const uint64_t number = _order.next();
  _index.emplace(std::string_view(it->first), Entry{it, number});
  _order.added(number, it);
  return true;
}

const std::string* Hash::get(std::string_view field) const {
  auto found = _index.find(field);
  return found == _index.end() ? nullptr : &found->second.item->second;
}

std::string* Hash::get(std::string_view field) {
  auto found = _index.find(field);
  return found == _index.end() ? nullptr : &found->second.item->second;
}

bool Hash::erase(std::string_view field) {
  auto found = _index.find(field);
  if (found == _index.end()) return false;
  const Entry entry = found->second;
  _order.removed(entry.number);
  _index.erase(found);
  _items.erase(entry.item);
  return true;
}

std::pair<std::vector<Hash::const_iterator>, uint64_t> Hash::page(uint64_t cursor, size_t count) {
  return _order.page(cursor, count, [this](auto visit) {
    for (const auto& [name, entry] : _index) visit(entry.number, entry.item);
  });
}

void Hash::adopt_numbers(const Hash& before) {
  _order.resume_from(before._order.reached());
  for (const auto& item : _items) {
    auto& entry = _index.at(item.first);
    const auto found = before._index.find(item.first);
    entry.number = found != before._index.end() ? found->second.number : _order.next();
  }
}

}  // namespace mimir::keyspace
