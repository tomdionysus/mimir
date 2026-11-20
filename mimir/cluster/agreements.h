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
#include <memory>
#include <string>

#include "dannan/contract/thread_safety.h"
#include "mimir/cluster/space.h"
#include "mimir/command/agreements.h"

// The database's agreements over Dannan's (stage 3, steps 3 and 4): a key's
// record is the register named by its database and key, in its shard, changed
// on a majority of the shard's replica set. Until membership moves shards
// (step 6) every shard is served by the genesis set, version 0.
//
// After a commit the committing node sends a notice to the set's other nodes
// (ruling 32): the key, the commit's version and, when it is small, the record,
// so each serves it to plain reads. A lost notice leaves a replica stale, as
// ruling 5 allows; the version keeps a late one from undoing a newer commit.
namespace mimir::cluster {

// The commit notice's wire type (Dannan's user types start at 512).
inline constexpr dannan::net::MessageType commit_notice{600};
// A page of the names a node holds in a shard, for a restarted peer's
// catch-up: asked with the shard and where the page starts.
inline constexpr dannan::net::MessageType shard_names{601};
inline constexpr size_t shard_names_page = 1000;
// A record larger than this is not carried by a notice: its replicas stay
// stale until a later commit's notice is small enough, or a read is strict.
inline constexpr size_t notice_record_limit = 64 * 1024;

class Agreements final : public command::Agreements {
 public:
  // The notice's type, on a node not yet started (routes are frozen at start).
  static void register_types(dannan::Context& context);

  // Takes the notice's handler.
  explicit Agreements(Space& served, std::chrono::milliseconds deadline = std::chrono::seconds(5));
  ~Agreements() override;

  std::string operation_id() override;
  void change(size_t db, const std::string& key, Change change, std::function<void(Outcome)> done) override;
  void read(size_t db, const std::string& key, std::function<void(Outcome)> done) override;
  // Refused, posted, until Dannan's atomic commit across replica sets.
  void change_keys(std::vector<Name> names, ChangeKeys change, std::function<void(KeysOutcome)> done) override;
  bool several_keys() const override { return false; }
  void on_committed(Committed committed) override;

  // Catches this node up after a restart that lost its acceptor state (the
  // design's restart rule, step 6): for each shard, the names a ready peer
  // holds are read at a majority of the others and installed (Dannan's
  // catch_up), then this node's view is filled from them, and its acceptors
  // vote again. A shard that cannot be caught up yet is tried again a second
  // later. `done` is posted once every shard is.
  void recover(std::function<void()> done);

 private:
  struct Recovery;
  void recover_shard(std::shared_ptr<Recovery> recovery);
  void gather_names(std::shared_ptr<Recovery> recovery, size_t peer, uint32_t from);
  void finish_recovery(const std::shared_ptr<Recovery>& recovery);
  void notify(size_t db, const std::string& key, const std::string& version, const cluster::Record& record);

  Space& _space;
  const std::chrono::milliseconds _deadline;
  std::atomic<uint64_t> _next{1};
  // Shared with the handler, so a notice racing the destructor finds none.
  struct Hook {
    dannan::contract::Mutex mutex;
    Committed committed DANNAN_GUARDED_BY(mutex);
  };
  std::shared_ptr<Hook> _hook = std::make_shared<Hook>();
};

}  // namespace mimir::cluster
