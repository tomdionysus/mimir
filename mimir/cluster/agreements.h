//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include "mimir/cluster/space.h"
#include "mimir/command/agreements.h"

// The database's agreements over Dannan's (stage 3, step 3): a key's record
// is the register named by its database and key, in its shard, changed on a
// majority of the shard's replica set. Until membership moves shards (step 6)
// every shard is served by the genesis set, version 0.
namespace mimir::cluster {

class Agreements final : public command::Agreements {
 public:
  explicit Agreements(Space& served, std::chrono::milliseconds deadline = std::chrono::seconds(5));

  std::string operation_id() override;
  void change(size_t db, const std::string& key, Change change, std::function<void(Outcome)> done) override;

 private:
  Space& _space;
  const std::chrono::milliseconds _deadline;
  std::atomic<uint64_t> _next{1};
};

}  // namespace mimir::cluster
