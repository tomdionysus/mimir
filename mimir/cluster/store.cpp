//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/cluster/store.h"

#include <boost/asio/post.hpp>

#include "mimir/cluster/register.h"

namespace mimir::cluster {

namespace agree = dannan::agree;

Store::Store(boost::asio::any_io_executor executor, bool ready) : _executor(std::move(executor)), _ready(ready) {}

void Store::load(agree::Slot slot, std::span<const agree::Name> names, bool every, bool values, std::function<void(agree::Held)> done) {
  agree::Held out;
  {
    dannan::contract::Lock lock(_mutex);
    out.floor = _floor;
    const auto shard = _shards.find(slot);
    if (shard != _shards.end()) out.slot = shard->second.record;
    for (const auto& name : names) {
      out.names.push_back(name);
      agree::AcceptorState state;
      if (shard != _shards.end()) {
        const auto held = shard->second.names.find(key_of(name));
        if (held != shard->second.names.end()) state = held->second;
      }
      out.states.push_back(std::move(state));
    }
    if (every && shard != _shards.end()) {
      std::map<std::string, bool> asked;
      for (const auto& name : names) asked[key_of(name)] = true;
      for (const auto& [key, state] : shard->second.names) {
        if (asked.contains(key)) continue;
        out.names.emplace_back(key.begin(), key.end());
        out.states.push_back(state);
      }
    }
  }
  if (!values)
    for (auto& state : out.states) state.value.reset();
  boost::asio::post(_executor, [done = std::move(done), out = std::move(out)]() mutable { done(std::move(out)); });
}

void Store::keep(agree::Slot slot, const agree::Held& held, std::function<void(Kept)> done) {
  Kept out;
  // No deltas yet: each is asked for whole, and nothing is kept (all or nothing).
  for (size_t i = 0; i < held.states.size(); ++i)
    if (held.states[i].delta) out.need_whole.push_back(i);
  if (out.need_whole.empty()) {
    dannan::contract::Lock lock(_mutex);
    keep_locked(slot, held);
    out.kept = true;
  }
  boost::asio::post(_executor, [done = std::move(done), out = std::move(out)]() mutable { done(std::move(out)); });
}

void Store::keep_locked(agree::Slot slot, const agree::Held& held) {
  _floor = held.floor;
  auto& shard = _shards[slot];
  shard.record = held.slot;
  for (size_t i = 0; i < held.names.size() && i < held.states.size(); ++i) {
    const auto key = key_of(held.names[i]);
    auto state = held.states[i];
    if (state == agree::AcceptorState{}) {
      shard.names.erase(key);
      continue;
    }
    if (state.has_value && !state.value) {
      // Held, not carried: the value and its summary stay as they are.
      const auto before = shard.names.find(key);
      if (before != shard.names.end()) {
        state.value = before->second.value;
        state.summary = before->second.summary;
      }
    } else {
      state.has_value = state.value.has_value();
      state.summary = state.value ? summary_of(*state.value) : agree::Bytes{};
    }
    shard.names[key] = std::move(state);
  }
  // A shard with neither record nor names holds nothing.
  if (shard.names.empty() && shard.record == agree::SlotState{}) _shards.erase(slot);
}

void Store::value(agree::Slot slot, const agree::Name& name, const agree::Ballot& ballot, const std::optional<agree::Bytes>& since,
                  std::function<void(std::optional<Fetched>)> done) {
  (void)since;
  std::optional<Fetched> out;
  {
    dannan::contract::Lock lock(_mutex);
    const auto shard = _shards.find(slot);
    if (shard != _shards.end()) {
      const auto held = shard->second.names.find(key_of(name));
      if (held != shard->second.names.end() && held->second.accepted == ballot && held->second.value) out = Fetched{*held->second.value, false};
    }
  }
  boost::asio::post(_executor, [done = std::move(done), out = std::move(out)]() mutable { done(std::move(out)); });
}

void Store::records(std::function<void(std::vector<std::pair<agree::Slot, agree::SlotState>>)> done) {
  std::vector<std::pair<agree::Slot, agree::SlotState>> out;
  {
    dannan::contract::Lock lock(_mutex);
    for (const auto& [slot, shard] : _shards)
      if (shard.record.served) out.emplace_back(slot, shard.record);
  }
  boost::asio::post(_executor, [done = std::move(done), out = std::move(out)]() mutable { done(std::move(out)); });
}

std::optional<agree::AcceptorState> Store::state(agree::Slot slot, const agree::Name& name) const {
  dannan::contract::Lock lock(_mutex);
  const auto shard = _shards.find(slot);
  if (shard == _shards.end()) return std::nullopt;
  const auto held = shard->second.names.find(key_of(name));
  if (held == shard->second.names.end()) return std::nullopt;
  return held->second;
}

std::vector<std::pair<agree::Name, agree::AcceptorState>> Store::held(agree::Slot slot) const {
  std::vector<std::pair<agree::Name, agree::AcceptorState>> out;
  dannan::contract::Lock lock(_mutex);
  const auto shard = _shards.find(slot);
  if (shard == _shards.end()) return out;
  for (const auto& [key, state] : shard->second.names)
    if (state.value) out.emplace_back(agree::Name(key.begin(), key.end()), state);
  return out;
}

size_t Store::size() const {
  dannan::contract::Lock lock(_mutex);
  size_t count = 0;
  for (const auto& [slot, shard] : _shards) count += shard.names.size();
  return count;
}

}  // namespace mimir::cluster
