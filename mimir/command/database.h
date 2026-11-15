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
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "mimir/command/pubsub.h"
#include "mimir/command/scripting.h"
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
  // How many databases SELECT may choose among (ruling 21): one by default,
  // fixed for the cluster's life.
  size_t databases = 1;
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
  // The databases, for SELECT, MOVE, SWAPDB, COPY ... DB, FLUSHALL and INFO.
  // Fixed at construction, so read without the lock.
  size_t databases() const noexcept DANNAN_NO_THREAD_SAFETY_ANALYSIS { return _dbs.size(); }
  keyspace::Keyspace& db_locked(size_t index) DANNAN_REQUIRES(_mutex) { return *_dbs.at(index); }
  // SWAPDB: the two exchange their keys; watches on either break.
  void swap_locked(size_t a, size_t b) DANNAN_REQUIRES(_mutex);
  // The lock the transaction commands are called under, for their annotations.
  dannan::contract::Mutex& lock_for_transactions() DANNAN_RETURN_CAPABILITY(_mutex) { return _mutex; }
  // The scripts' interpreter and cache, one for every database as Valkey's.
  Scripting& scripting_locked() DANNAN_REQUIRES(_mutex) { return _scripting; }

  // Keyspace notifications (Valkey's "Keyspace notifications"): an event of
  // class `cls` ('g', '$', 'l', 's', 'h', 'z', 'x', 'e', 't', 'd', 'n' or
  // 'm') named `event` happened to `key` in database `db`. Published, if the
  // classes configured take it, once the command that caused it is done.
  void notify_locked(size_t db, char cls, std::string_view event, std::string_view key) DANNAN_REQUIRES(_mutex);
  // CONFIG SET notify-keyspace-events: whether `flags` is valid; if so it
  // takes effect, and CONFIG GET answers it in Valkey's order.
  bool set_notify_flags_locked(std::string_view flags) DANNAN_REQUIRES(_mutex);
  static bool valid_notify_flags(std::string_view flags);

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
  // Handlers, which hold the lock without saying so, call it.
  uint64_t random() DANNAN_NO_THREAD_SAFETY_ANALYSIS { return _random(); }

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
  void touch(const Spec& spec, const std::vector<std::string>& args, size_t db) DANNAN_REQUIRES(_mutex);
  // A key named within its database, for waits and watches: the database's
  // number, a NUL, then the key.
  static std::string scoped(size_t db, std::string_view key);
  static std::pair<size_t, std::string_view> unscoped(std::string_view scoped_key);
  static std::string timeout_reply(resp::Protocol protocol, bool null_bulk);
  // Each keyspace tells its own database's number of its expired and new keys;
  // bound again when SWAPDB exchanges two.
  void bind_listeners() DANNAN_REQUIRES(_mutex);
  // A read looks its keys up first, as Valkey's do: an expired one is removed
  // (and announced), a missing one is a "keymiss". Only while notifications
  // are on.
  void look_up_keys(const Spec& spec, const std::vector<std::string>& args, size_t db) DANNAN_REQUIRES(_mutex);
  void publish_events() DANNAN_REQUIRES(_mutex);

  struct Event {
    size_t db;
    std::string event;
    std::string key;
  };

  const dannan::contract::WallClock& _wall;
  const DatabaseSettings _settings;
  const uint64_t _started_ms;
  PubSub _pubsub;
  std::atomic<int64_t> _next_client{1};
  dannan::contract::Mutex _mutex;
  std::vector<std::unique_ptr<keyspace::Keyspace>> _dbs DANNAN_GUARDED_BY(_mutex);
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
  Scripting _scripting DANNAN_GUARDED_BY(_mutex);
  // The classes notify-keyspace-events takes, as bits; the events waiting to
  // be published.
  uint32_t _notify_flags DANNAN_GUARDED_BY(_mutex) = 0;
  std::vector<Event> _events DANNAN_GUARDED_BY(_mutex);
};

}  // namespace mimir::command
