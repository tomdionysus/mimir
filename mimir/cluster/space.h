//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dannan/agree/memory_store.h"
#include "dannan/context.h"
#include "mimir/command/database.h"

// Mimir's agreement space on one node (stage 3, ruling 26): this node's
// acceptors, served from Dannan's in-memory store (until Dagda's log, ruling
// 17) to proposers that advertise `mimir`, and
// the founding of every shard on the genesis set. The shard count is the
// agreement table's slot count (NodeConfig::agreement_slots), one value for
// the cluster's life.
namespace mimir::cluster {

class Space {
 public:
  // Advertises `mimir` and serves the space. `ready`: whether this node's
  // acceptors may vote at once (a node that never voted, or a genesis set of
  // one); otherwise not until caught up.
  Space(dannan::Context& context, bool ready);
  ~Space();
  Space(const Space&) = delete;
  Space& operator=(const Space&) = delete;

  // Records every shard as served by the genesis set (Dannan's found(), one
  // change per shard). Repeatable; a node outside the genesis set has nothing
  // to found. `done` is posted once.
  void found(std::chrono::milliseconds deadline, std::function<void(dannan::agree::Outcome)> done);

  uint32_t shards() const;
  dannan::agree::MemoryStore& store() noexcept { return *_store; }

  // This node's name and where it takes clients, advertised for MIMIR NODES
  // (ruling 40). An empty host: the address peers dial this node at.
  // `tls_port` zero: none.
  void advertise_clients(const std::string& name, const std::string& host, uint16_t port, uint16_t tls_port);
  // Every active node that advertises where it takes clients, this one
  // first. Thread-safe; waits on nothing.
  std::vector<command::NodeAddress> client_addresses() const;
  static constexpr const char* client_advert = "mimir.client";
  dannan::Context& context() noexcept { return _context; }

 private:
  dannan::Context& _context;
  std::shared_ptr<dannan::agree::MemoryStore> _store;
};

}  // namespace mimir::cluster
