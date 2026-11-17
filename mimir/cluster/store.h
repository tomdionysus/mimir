//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "dannan/agree/agreement.h"
#include "dannan/contract/thread_safety.h"

// This node's acceptors for the space `mimir` (Dannan's AcceptorStore): every
// shard's record and every register's promise, accepted ballot and value, held
// in memory until Dagda's log (ruling 17). Values are records' bytes
// (register.h), and a value's summary is their SHA-256.
//
// A node that starts holds nothing it voted before, so it is not ready (it may
// not vote) until it has caught up, unless it is told it holds everything there
// is to hold: a cluster's first start. Deltas are not taken yet (each is asked
// for whole), and value() always answers whole. Thread-safe; every completion
// is posted to the executor it was made with, never called inline.
namespace mimir::cluster {

class Store final : public dannan::agree::AcceptorStore {
 public:
  // `ready`: whether it may vote at once.
  Store(boost::asio::any_io_executor executor, bool ready);

  void load(dannan::agree::Slot slot, std::span<const dannan::agree::Name> names, bool every, bool values,
            std::function<void(dannan::agree::Held)> done) override;
  void keep(dannan::agree::Slot slot, const dannan::agree::Held& held, std::function<void(Kept)> done) override;
  void value(dannan::agree::Slot slot, const dannan::agree::Name& name, const dannan::agree::Ballot& ballot, const std::optional<dannan::agree::Bytes>& since,
             std::function<void(std::optional<Fetched>)> done) override;
  bool ready() const override { return _ready.load(); }
  void records(std::function<void(std::vector<std::pair<dannan::agree::Slot, dannan::agree::SlotState>>)> done) override;

  // Caught up: it may vote from now on.
  void set_ready() { _ready.store(true); }

  // What this acceptor holds for `name`, for tests and Status.
  std::optional<dannan::agree::AcceptorState> state(dannan::agree::Slot slot, const dannan::agree::Name& name) const;
  // How many registers it holds state for.
  size_t size() const;

 private:
  struct Shard {
    dannan::agree::SlotState record;
    // Keyed by the name's bytes as a string.
    std::map<std::string, dannan::agree::AcceptorState> names;
  };

  static std::string key_of(std::span<const uint8_t> name) { return {reinterpret_cast<const char*>(name.data()), name.size()}; }
  void keep_locked(dannan::agree::Slot slot, const dannan::agree::Held& held) DANNAN_REQUIRES(_mutex);

  boost::asio::any_io_executor _executor;
  std::atomic<bool> _ready;
  mutable dannan::contract::Mutex _mutex;
  std::map<dannan::agree::Slot, Shard> _shards DANNAN_GUARDED_BY(_mutex);
  dannan::agree::Ballot _floor DANNAN_GUARDED_BY(_mutex);
};

}  // namespace mimir::cluster
