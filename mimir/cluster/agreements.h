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
  void on_committed(Committed committed) override;

 private:
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
