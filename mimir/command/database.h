//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "mimir/command/table.h"
#include "mimir/keyspace/keyspace.h"
#include "mimir/resp/reply.h"

// One node's database at the single-node stage: the keyspace and the command
// table's dispatch. Every connection's strand calls execute(); each command
// runs whole under the database's lock, so on one node every command is atomic
// as Valkey's are. The cluster stage replaces the one lock with a shard's.
namespace mimir::command {

struct DatabaseSettings {
  // What HELLO and INFO say the server is.
  std::string server_name = "mimir";
  // The Valkey behaviour this build implements, which clients read to decide
  // what they may use: HELLO's "version" and INFO's redis_version.
  std::string compatible_version = "8.1.1";
  // INFO's redis_version: what Valkey 8 reports there for clients that read it.
  std::string redis_version = "7.2.4";
};

class Database {
 public:
  Database(const dannan::contract::WallClock& wall, DatabaseSettings settings = {});

  // A fresh connection's id: unique and increasing on this node. Thread-safe.
  int64_t next_client_id() noexcept { return _next_client.fetch_add(1); }

  // Runs one command and appends its reply to `out`, in the session's protocol
  // (HELLO's reply in the protocol it chooses). A command that cannot run
  // (unknown, wrong arity, a refused argument) writes its error and changes
  // nothing. Waits on the database's lock; thread-safe.
  void execute(Session& session, const std::vector<std::string>& args, std::string& out);

  // Active expiry: at most `budget` keys with an expiry examined. Speculative
  // work; thread-safe.
  size_t expire_step(size_t budget);

  const DatabaseSettings& settings() const noexcept { return _settings; }
  uint64_t now_ms() const noexcept { return _wall.unix_ms(); }
  // A random number for RANDOMKEY and SPOP's like. Called under the lock.
  uint64_t random() { return _random(); }

 private:
  const dannan::contract::WallClock& _wall;
  const DatabaseSettings _settings;
  std::atomic<int64_t> _next_client{1};
  dannan::contract::Mutex _mutex;
  keyspace::Keyspace _keys DANNAN_GUARDED_BY(_mutex);
  std::mt19937_64 _random DANNAN_GUARDED_BY(_mutex);
};

}  // namespace mimir::command
