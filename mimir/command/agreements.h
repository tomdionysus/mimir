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
#include <vector>

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
    // Committed: the commit's version, bytes ordered as the key's commits are.
    std::string version;
    // Not committed: why.
    std::string why;
  };

  // Changes the record of `key` in database `db` by agreement. `change` is
  // given the record as a majority holds it (empty for a key never written)
  // and may run more than once across retries; it must be repeatable, finding
  // its own operation in the record. `done` is posted once, never called
  // inline. Thread-safe; waits on nothing.
  virtual void change(size_t db, const std::string& key, Change change, std::function<void(Outcome)> done) = 0;

  // A change of several keys at once, atomically (rulings 22 and 29): each
  // name's record as a majority holds it, in the order named, and the new
  // record of each, in the same order. Repeatable as `change` is. `done` is
  // posted once, never called inline. Thread-safe; waits on nothing.
  struct Name {
    size_t db = 0;
    std::string key;
    bool operator==(const Name&) const = default;
  };
  struct ChangedKeys {
    std::vector<cluster::Record> records;
    std::string result;
  };
  using ChangeKeys = std::function<ChangedKeys(const std::vector<cluster::Record>& current)>;
  struct KeysOutcome {
    bool committed = false;
    bool unknown = false;
    // Committed: each name's record as committed, and its commit's version.
    std::vector<cluster::Record> records;
    std::vector<std::string> versions;
    std::string result;
    std::string why;
  };
  virtual void change_keys(std::vector<Name> names, ChangeKeys change, std::function<void(KeysOutcome)> done) = 0;
  // Whether change_keys can commit: the cluster's capability, not a node's.
  // Until Dannan's atomic commit across replica sets (its item 26) the
  // cluster's cannot, and a write of several keys is refused (ruling 35).
  virtual bool several_keys() const = 0;

  // A strict read of `key`'s record (ruling 5): committed with the record a
  // majority holds, or not committed and why. `done` is posted once.
  virtual void read(size_t db, const std::string& key, std::function<void(Outcome)> done) = 0;

  // A commit another node made (ruling 32's commit notice): the key, the
  // commit's version, and the record committed.
  using Committed = std::function<void(size_t db, const std::string& key, const std::string& version, const cluster::Record& record)>;
  // Tells `committed` of the commits other nodes make to keys this node
  // replicates, on any thread, in any order (the version orders them). Set
  // once, before the first proposal.
  virtual void on_committed(Committed committed) = 0;
};

}  // namespace mimir::command
