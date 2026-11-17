//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include "mimir/cluster/register.h"

// What the database needs of the cluster (stage 3, step 3): one key's record
// changed by agreement on a majority of its replicas. The cluster's own
// implementation is cluster::Agreements; a database without one runs every
// command on its own keyspace (the unit tests' single node).
namespace mimir::command {

class Agreements {
 public:
  virtual ~Agreements() = default;

  // A fresh operation id: unique across the cluster and its restarts, at most
  // cluster::max_operation_id bytes. Thread-safe.
  virtual std::string operation_id() = 0;

  // A change's outcome, made from the record it was given.
  struct Changed {
    cluster::Record record;
    // Carried back to the proposer from the attempt that committed.
    std::string result;
  };
  using Change = std::function<Changed(const cluster::Record& current)>;

  struct Outcome {
    bool committed = false;
    // Not committed, but an accept may have been taken: whether it took
    // effect is not known (a repeatable change may be proposed again).
    bool unknown = false;
    // Committed: the record as committed, and the committing attempt's result.
    cluster::Record record;
    std::string result;
    // Not committed: why.
    std::string why;
  };

  // Changes the record of `key` in database `db` by agreement. `change` is
  // given the record as a majority holds it (empty for a key never written)
  // and may run more than once across retries; it must be repeatable, finding
  // its own operation in the record. `done` is posted once, never called
  // inline. Thread-safe; waits on nothing.
  virtual void change(size_t db, const std::string& key, Change change, std::function<void(Outcome)> done) = 0;
};

}  // namespace mimir::command
