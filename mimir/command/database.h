//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "mimir/command/pubsub.h"
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

// A parked command's wake: given its reply once a write has served it. Called
// on the thread of the write that served it, never under the database's lock;
// it posts to the waiting connection's own strand.
using Wake = std::function<void(std::string reply)>;

// What execute() did with a command that may wait.
struct Executed {
  // The command is parked: its wait's id, and its timeout in seconds (zero:
  // for ever). Nothing was appended to `out` for it.
  std::optional<uint64_t> wait;
  double wait_seconds = 0;
};

class Database {
 public:
  Database(const dannan::contract::WallClock& wall, DatabaseSettings settings = {});

  // A fresh connection's id: unique and increasing on this node. Thread-safe.
  int64_t next_client_id() noexcept { return _next_client.fetch_add(1); }

  // Runs one command and appends its reply to `out`, in the session's protocol
  // (HELLO's reply in the protocol it chooses). A command that cannot run
  // (unknown, wrong arity, a refused argument) writes its error and changes
  // nothing. A blocking command that finds nothing is parked when `wake` is
  // given, and answered with its timeout reply at once when it is not. After
  // a write, parked commands whose keys it made ready are served, oldest
  // first, and their wakes called once the lock is let go. Waits on the
  // database's lock; thread-safe. `session` outlives any wait it parks.
  Executed execute(Session& session, const std::vector<std::string>& args, std::string& out, Wake wake = nullptr);

  // Ends wait `id` at its timeout or its connection's close: the timeout
  // reply's bytes when it was still parked; none when a write served it first
  // (its wake has the reply). Thread-safe.
  std::optional<std::string> cancel_wait(uint64_t id);
  // Commands parked now.
  size_t waiting();

  // A connection has gone: its watches end. Thread-safe.
  void forget(Session& session);

  // For the transaction commands, which run under the database's lock: one
  // queued command run whole, its reply appended to `reply` (a blocking one
  // answers its timeout); WATCH, UNWATCH, and whether a watched key has
  // changed since it was watched (written, or expired, or flushed).
  void run_locked(Session& session, const std::vector<std::string>& args, resp::Reply& reply) DANNAN_REQUIRES(_mutex);
  void watch_locked(Session& session, const std::string& key) DANNAN_REQUIRES(_mutex);
  void unwatch_locked(Session& session) DANNAN_REQUIRES(_mutex);
  bool watch_broken_locked(Session& session) DANNAN_REQUIRES(_mutex);
  // For the server commands, under the lock: the connections known, and the
  // settings CONFIG reads and writes.
  std::map<int64_t, Session*>& sessions_locked() DANNAN_REQUIRES(_mutex) { return _sessions; }
  std::map<std::string, std::string>& config_locked() DANNAN_REQUIRES(_mutex) { return _config; }
  size_t waiting_locked() DANNAN_REQUIRES(_mutex) { return _waiters.size(); }
  // The lock the transaction commands are called under, for their annotations.
  dannan::contract::Mutex& lock_for_transactions() DANNAN_RETURN_CAPABILITY(_mutex) { return _mutex; }

  // Active expiry: at most `budget` keys with an expiry examined. Speculative
  // work; thread-safe.
  size_t expire_step(size_t budget);

  // Publish and subscribe on this node: its own lock, never the keyspace's.
  PubSub& pubsub() noexcept { return _pubsub; }

  const DatabaseSettings& settings() const noexcept { return _settings; }
  // When this database began, in Unix ms (LASTSAVE, INFO's uptime).
  uint64_t started_ms() const noexcept { return _started_ms; }
  uint64_t now_ms() const noexcept { return _wall.unix_ms(); }
  // A random number for RANDOMKEY and SPOP's like. Called under the lock.
  uint64_t random() { return _random(); }

 private:
  struct Waiter {
    Session* session;
    std::vector<std::string> args;
    std::vector<std::string> keys;
    bool null_bulk;
    Wake wake;
  };
  using Woken = std::vector<std::pair<Wake, std::string>>;

  // Serves parked commands whose keys now hold something, oldest first per
  // key, until a pass serves nothing; each served one's wake and reply go to
  // `woken`, to be called after the lock is let go.
  void serve_ready(Woken& woken) DANNAN_REQUIRES(_mutex);
  void forget_waiter(uint64_t id) DANNAN_REQUIRES(_mutex);
  // Marks the watchers of the keys a write named; every watcher for a flush.
  void touch(const Spec& spec, const std::vector<std::string>& args) DANNAN_REQUIRES(_mutex);
  static std::string timeout_reply(resp::Protocol protocol, bool null_bulk);

  const dannan::contract::WallClock& _wall;
  const DatabaseSettings _settings;
  const uint64_t _started_ms;
  PubSub _pubsub;
  std::atomic<int64_t> _next_client{1};
  dannan::contract::Mutex _mutex;
  keyspace::Keyspace _keys DANNAN_GUARDED_BY(_mutex);
  std::mt19937_64 _random DANNAN_GUARDED_BY(_mutex);
  std::map<uint64_t, Waiter> _waiters DANNAN_GUARDED_BY(_mutex);
  // Per key, the waits parked on it in the order they parked.
  std::unordered_map<std::string, std::deque<uint64_t>> _waiting_on DANNAN_GUARDED_BY(_mutex);
  uint64_t _next_wait DANNAN_GUARDED_BY(_mutex) = 1;
  // The keys this command's writes named: what serve_ready looks at.
  std::vector<std::string> _touched DANNAN_GUARDED_BY(_mutex);
  // The connections this database has seen, for CLIENT LIST and CLIENT KILL.
  std::map<int64_t, Session*> _sessions DANNAN_GUARDED_BY(_mutex);
  // Settings CONFIG SET may change, as text.
  std::map<std::string, std::string> _config DANNAN_GUARDED_BY(_mutex);
  // Per key, the sessions watching it.
  std::unordered_map<std::string, std::vector<Session*>> _watchers DANNAN_GUARDED_BY(_mutex);
};

}  // namespace mimir::command
