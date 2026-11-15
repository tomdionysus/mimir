//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/keyspace/keyspace.h"

#include "mimir/keyspace/slot.h"

namespace mimir::keyspace {

std::string_view type_name(Type type) noexcept {
  switch (type) {
    case Type::string:
      return "string";
    case Type::list:
      return "list";
    case Type::hash:
      return "hash";
    case Type::set:
      return "set";
    case Type::zset:
      return "zset";
    case Type::stream:
      return "stream";
  }
  return "none";
}

uint64_t position_of(std::string_view key) noexcept {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (unsigned char c : key) {
    hash ^= c;
    hash *= 0x100000001b3ull;
  }
  return (static_cast<uint64_t>(key_slot(key)) << 48) | (hash >> 16);
}

Keyspace::Keyspace(const dannan::contract::WallClock& wall) : _wall(wall) {}

void Keyspace::forget(Map::iterator it) {
  if (it->second.expires_at) --_expiring;
  _keys.erase(it);
}

Entry* Keyspace::find(std::string_view key) {
  auto it = _keys.find(std::pair<uint64_t, std::string_view>(position_of(key), key));
  if (it == _keys.end()) return nullptr;
  if (expired(it->second)) {
    forget(it);
    return nullptr;
  }
  return &it->second;
}

bool Keyspace::contains(std::string_view key) const {
  auto it = _keys.find(std::pair<uint64_t, std::string_view>(position_of(key), key));
  return it != _keys.end() && !expired(it->second);
}

Entry& Keyspace::put(std::string_view key, Entry entry) {
  const uint64_t position = position_of(key);
  auto it = _keys.find(std::pair<uint64_t, std::string_view>(position, key));
  if (entry.expires_at) ++_expiring;
  if (it == _keys.end()) return _keys.emplace(Key(position, std::string(key)), std::move(entry)).first->second;
  if (it->second.expires_at) --_expiring;
  it->second = std::move(entry);
  return it->second;
}

bool Keyspace::erase(std::string_view key) {
  auto it = _keys.find(std::pair<uint64_t, std::string_view>(position_of(key), key));
  if (it == _keys.end()) return false;
  const bool live = !expired(it->second);
  forget(it);
  return live;
}

void Keyspace::rename(std::string_view from, std::string_view to) {
  auto it = _keys.find(std::pair<uint64_t, std::string_view>(position_of(from), from));
  if (it == _keys.end()) return;
  Entry entry = std::move(it->second);
  forget(it);
  put(to, std::move(entry));
}

void Keyspace::clear() noexcept {
  _keys.clear();
  _expiring = 0;
  _expire_cursor = 0;
}

ScanPage Keyspace::scan(uint64_t cursor, size_t count, std::optional<Type> type) {
  ScanPage page;
  if (count == 0) count = 1;
  auto it = _keys.lower_bound(std::pair<uint64_t, std::string_view>(cursor, std::string_view()));
  // A page ends between positions, never inside one, so keys that share a
  // position (a hash collision) are returned together.
  size_t examined = 0;
  uint64_t last = 0;
  while (it != _keys.end()) {
    if (examined >= count && it->first.first != last) break;
    ++examined;
    last = it->first.first;
    auto here = it++;
    if (expired(here->second)) {
      forget(here);
      continue;
    }
    if (type && here->second.type() != *type) continue;
    page.keys.push_back(here->first.second);
  }
  if (it == _keys.end()) {
    page.cursor = 0;
  } else {
    page.cursor = it->first.first;
    // A key at position zero is only ever the first; a later cursor of zero
    // would read as complete, so it cannot arise past the first page.
    if (page.cursor == 0) page.cursor = 1;
  }
  return page;
}

std::vector<std::string> Keyspace::keys() {
  std::vector<std::string> out;
  out.reserve(_keys.size());
  for (auto it = _keys.begin(); it != _keys.end();) {
    auto here = it++;
    if (expired(here->second)) {
      forget(here);
      continue;
    }
    out.push_back(here->first.second);
  }
  return out;
}

std::optional<std::string> Keyspace::random_key(uint64_t seed) {
  // A random position, then the first live key at or after it, wrapping once.
  for (int attempt = 0; attempt < 2 && !_keys.empty(); ++attempt) {
    auto it = _keys.lower_bound(std::pair<uint64_t, std::string_view>(seed, std::string_view()));
    for (size_t looked = 0; looked < _keys.size(); ++looked) {
      if (it == _keys.end()) it = _keys.begin();
      if (!expired(it->second)) return it->first.second;
      auto here = it++;
      forget(here);
      if (_keys.empty()) return std::nullopt;
    }
  }
  return std::nullopt;
}

size_t Keyspace::expire_step(size_t budget) {
  size_t removed = 0;
  size_t examined = 0;
  auto it = _keys.lower_bound(std::pair<uint64_t, std::string_view>(_expire_cursor, std::string_view()));
  size_t visited = 0;
  while (examined < budget && _expiring > 0 && visited < _keys.size()) {
    if (it == _keys.end()) it = _keys.begin();
    ++visited;
    auto here = it++;
    if (!here->second.expires_at) continue;
    ++examined;
    if (expired(here->second)) {
      forget(here);
      ++removed;
    }
  }
  _expire_cursor = it == _keys.end() ? 0 : it->first.first;
  return removed;
}

}  // namespace mimir::keyspace
