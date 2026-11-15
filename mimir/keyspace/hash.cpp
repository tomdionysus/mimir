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
  _index.reserve(_items.size());
  for (auto it = _items.begin(); it != _items.end(); ++it) _index.emplace(std::string_view(it->first), it);
  return *this;
}

bool Hash::set(std::string_view field, std::string value) {
  auto found = _index.find(field);
  if (found != _index.end()) {
    found->second->second = std::move(value);
    return false;
  }
  _items.emplace_back(std::string(field), std::move(value));
  auto it = std::prev(_items.end());
  _index.emplace(std::string_view(it->first), it);
  return true;
}

const std::string* Hash::get(std::string_view field) const {
  auto found = _index.find(field);
  return found == _index.end() ? nullptr : &found->second->second;
}

std::string* Hash::get(std::string_view field) {
  auto found = _index.find(field);
  return found == _index.end() ? nullptr : &found->second->second;
}

bool Hash::erase(std::string_view field) {
  auto found = _index.find(field);
  if (found == _index.end()) return false;
  auto it = found->second;
  _index.erase(found);
  _items.erase(it);
  return true;
}

}  // namespace mimir::keyspace
