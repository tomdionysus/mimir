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
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "dannan/contract/time_source.h"
#include "mimir/command/acl.h"
#include "mimir/command/agreements.h"
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
  // Whether AUTH and HELLO's AUTH are taken on a plaintext connection (ruling
  // 28): off by default, so a password never crosses the network in clear.
  bool auth_plaintext = false;
  // The users the node starts with, as lines of a Valkey ACL file ("user
  // alice on >secret ~app:* +@read"), applied over the default user (ruling
  // 27). A line that does not parse refuses the construction.
  std::vector<std::string> users;
  // notify-keyspace-events at start, in Valkey's letters ("Khgx"); empty:
  // off, as Valkey's default. Refused at construction when not valid.
  std::string notify_keyspace_events;
  // SHUTDOWN's way to stop the node (ruling 31), called on the command's
  // thread under the database's lock: it must only post the stop. None: a
  // client cannot stop this node.
  std::function<void()> shutdown;
  // The cluster (stage 3): every write naming one key commits by agreement,
  // and one naming more is refused until Dannan 0.10.0 (ruling 35). None: the
  // database runs every command on its own keyspace.
  std::shared_ptr<Agreements> agreements;
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
  // Parked by CLIENT PAUSE, not run: when the wait ends (its wake, or
  // cancel_wait, with no bytes) the connection runs it again.
  bool postponed = false;
  // MIGRATE's talk with its target, for the connection to have, then hand to
  // finish_migrate. Nothing was appended to `out` for it.
  std::optional<MigrateJob> migrate;
};

// How MIGRATE's talk with its target went: the first-level replies read, in
// order (AUTH's if any, SELECT's, then one per key, each "+..." or "-..."),
// and whether the connection could not be made or a reply could not be read.
struct MigrateOutcome {
  std::vector<std::string> replies;
  bool unreachable = false;
  bool read_failed = false;
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

  // MIGRATE's end, once its connection has talked with the target: each key
  // the target took is removed here (unless COPY), but only if it has not
  // changed since it was read, and the reply's bytes are returned, as Valkey
  // answers. Thread-safe.
  std::string finish_migrate(Session& session, const MigrateJob& job, const MigrateOutcome& outcome);

  // Ends wait `id` at its timeout or its connection's close: the timeout
  // reply's bytes when it was still parked; none when a write served it first
  // (its wake has the reply). Thread-safe.
  std::optional<std::string> cancel_wait(uint64_t id);
  // Commands parked now.
  size_t waiting();

  // A connection has come: it is the default user, authenticated when that
  // user needs no password now (Valkey decides it at connection, not at the
  // first command). Thread-safe.
  void welcome(Session& session) {
    dannan::contract::Lock lock(_mutex);
    welcome_locked(session);
  }

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
  // The users, their permissions and the log of refusals.
  Acl& acl_locked() DANNAN_REQUIRES(_mutex) { return _acl; }
  // Whether `session` may run `args` under ACL, in `context` ("toplevel",
  // "multi" or "lua"): none when it may; else the refusal's words (without a
  // code), logged.
  // `denial`, when given, is told why.
  std::optional<std::string> acl_refusal_locked(Session& session, const std::vector<std::string>& args, std::string_view context, Denial* denial = nullptr)
      DANNAN_REQUIRES(_mutex);
  // A session as it is before AUTH: the default user, authenticated when that
  // user needs no password.
  void welcome_locked(Session& session) DANNAN_REQUIRES(_mutex);
  // A refused AUTH, for ACL LOG.
  void log_auth_failure_locked(Session& session, std::string_view username) DANNAN_REQUIRES(_mutex);
  // MONITOR: `session` is told of every command run from now on, until it goes.
  void monitor_locked(Session& session) DANNAN_REQUIRES(_mutex);
  // CLIENT UNBLOCK: ends connection `client`'s wait, with its timeout reply or
  // an UNBLOCKED error; whether it was waiting. Its wake is called once the
  // lock is let go. A command held by CLIENT PAUSE is not waiting.
  bool unblock_locked(int64_t client, bool error) DANNAN_REQUIRES(_mutex);
  // CLIENT PAUSE and UNPAUSE: until `until_ms` (wall clock), every command
  // (`all`) or those that write or may replicate are held. A later pause
  // keeps the later end and the stricter mode, as Valkey's does. UNPAUSE
  // releases the held commands to run.
  void pause_locked(uint64_t until_ms, bool all) DANNAN_REQUIRES(_mutex);
  void unpause_locked() DANNAN_REQUIRES(_mutex);
  // CLIENT TRACKING: a connection's tracking turned on (its options already
  // checked and set on its session) or off.
  void track_on_locked(Session& session) DANNAN_REQUIRES(_mutex);
  void track_off_locked(Session& session) DANNAN_REQUIRES(_mutex);
  // Whether connection `id` is known here (a REDIRECT's target).
  bool has_session_locked(int64_t id) DANNAN_REQUIRES(_mutex) { return _sessions.count(id) > 0; }

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
  // A write by agreement (stage 3): what it is, carried through its retries.
  struct Proposal {
    uint64_t wait = 0;
    // A copy of the connection's session: the change may run after the
    // connection has gone.
    Session session;
    const Spec* spec = nullptr;
    std::vector<std::string> args;
    size_t db = 0;
    std::string key;
    std::string operation;
    uint64_t issued_ms = 0;
    uint64_t deadline_ms = 0;
  };
  struct Waiter {
    Waiter(Session* session_, std::vector<std::string> args_, std::vector<std::string> keys_, bool null_bulk_, Wake wake_, bool postponed_ = false,
           bool proposal_ = false)
        : session(session_),
          args(std::move(args_)),
          keys(std::move(keys_)),
          null_bulk(null_bulk_),
          wake(std::move(wake_)),
          postponed(postponed_),
          proposal(proposal_) {}

    Session* session;
    std::vector<std::string> args;
    std::vector<std::string> keys;
    bool null_bulk;
    Wake wake;
    // Held by CLIENT PAUSE: released with no bytes, to be run again.
    bool postponed = false;
    // A write waiting for its agreement: answered by its commit.
    bool proposal = false;
    // A blocking pop by agreement (stage 3): proposed again when its key is
    // written, never run here. `in_flight` while an attempt is proposed; a
    // timeout then waits for it (an element it popped is delivered), and
    // `timed_out` ends the wait if it found nothing.
    std::shared_ptr<Proposal> blocked;
    bool in_flight = false;
    bool timed_out = false;
  };
  // A blocking pop's next attempt, under a new operation id, to start once
  // the lock is let go.
  void repropose_locked(const std::shared_ptr<Proposal>& proposal) DANNAN_REQUIRES(_mutex);
  // Starts the attempts repropose_locked queued.
  void start_pending() DANNAN_EXCLUDES(_mutex);
  // A blocking write's timeout and timeout reply, read from its arguments:
  // the error when its timeout is refused.
  struct Blocking {
    double seconds = 0;
    bool null_bulk = false;
    std::string error;
  };
  Blocking blocking_locked(const Spec& spec, const std::vector<std::string>& args, Session& session, size_t db) DANNAN_REQUIRES(_mutex);
  // Whether this write must be refused in a cluster, and why (ruling 35, and
  // what step 8 brings).
  std::optional<std::string> cluster_refusal_locked(const Spec& spec, const std::vector<std::string>& args, const Session& session) DANNAN_REQUIRES(_mutex);
  // The command run against `base`, a copy of its key alone in a keyspace of
  // its own: the record it leaves and the result (its reply and the
  // notifications it would raise). Nothing outside it changes.
  Agreements::Changed speculate(const Proposal& proposal, const cluster::Record& base) DANNAN_EXCLUDES(_mutex);
  // Proposes it, and answers its connection once it is decided.
  void propose(std::shared_ptr<Proposal> proposal) DANNAN_EXCLUDES(_mutex);
  void decided(const std::shared_ptr<Proposal>& proposal, Agreements::Outcome outcome) DANNAN_EXCLUDES(_mutex);
  // A committed record made this node's view of its key, if `version` is newer
  // than the commit this node holds for it (ruling 32: never a value undone,
  // nor an older one over a newer). Whether it was.
  bool install_locked(size_t db, const std::string& key, const std::string& version, const cluster::Record& record) DANNAN_REQUIRES(_mutex);
  // A committed key's watchers, trackers (but `modifier`, for NOLOOP) and
  // waiters told of it, as a write's touch does.
  void touch_key_locked(size_t db, const std::string& key, const Session* modifier) DANNAN_REQUIRES(_mutex);
  // In a cluster: the one key an EXEC writes and watches, if it writes and
  // names no other; none for an EXEC that only reads. Refused (`several`)
  // when it names more than one.
  struct ExecKey {
    bool writes = false;
    bool several = false;
    std::string key;
  };
  ExecKey exec_key_locked(const Session& session) DANNAN_REQUIRES(_mutex);
  // WATCH in a cluster: each key's write count read from a majority, then the
  // watch taken; the connection waits for it.
  void watch_strictly(uint64_t wait, size_t db, std::vector<std::string> keys) DANNAN_EXCLUDES(_mutex);
  // Another node's commit, from its notice: installed, and its watchers,
  // trackers and waiters told as a local write's are.
  void install_committed(size_t db, const std::string& key, const std::string& version, const cluster::Record& record) DANNAN_EXCLUDES(_mutex);
  // Whether CLIENT PAUSE holds this command now.
  bool paused_locked(const Spec& spec, const Session& session) DANNAN_REQUIRES(_mutex);
  using Woken = std::vector<std::pair<Wake, std::string>>;

  // Serves parked commands whose keys now hold something, oldest first per
  // key, until a pass serves nothing; each served one's wake and reply go to
  // `woken`, to be called after the lock is let go.
  void serve_ready(Woken& woken) DANNAN_REQUIRES(_mutex);
  void forget_waiter(uint64_t id) DANNAN_REQUIRES(_mutex);
  // Marks the watchers of the keys a write named; every watcher for a flush.
  // `modifier` is the connection whose command it was, for NOLOOP.
  void touch(const Spec& spec, const std::vector<std::string>& args, size_t db, const Session* modifier) DANNAN_REQUIRES(_mutex);
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
  // MONITOR's line for a command run by `session` (or a script, "lua").
  void feed_monitors(const Session& session, const Spec& spec, const std::vector<std::string>& args, size_t db) DANNAN_REQUIRES(_mutex);
  // execute() without CLIENT REPLY's suppression.
  Executed execute_one(Session& session, const std::vector<std::string>& args, std::string& out, Wake wake);

  // Client-side caching, as Valkey's: the keys `tracker` read with a
  // read-only command are remembered; a key changed (or expired, or flushed)
  // tells each connection that read it, once, and the BCAST connections whose
  // prefixes it has. A message to the connection running the command goes
  // after its reply, into `out`.
  void track_reads_locked(Session& tracker, const Spec& spec, const std::vector<std::string>& args) DANNAN_REQUIRES(_mutex);
  void invalidate_locked(std::string_view key, const Session* modifier) DANNAN_REQUIRES(_mutex);
  void invalidate_all_locked() DANNAN_REQUIRES(_mutex);
  // Sends what is pending: the running connection's own invalidations into
  // `out` (none: there is no running connection), and BCAST's.
  void flush_tracking_locked(std::string* out) DANNAN_REQUIRES(_mutex);
  // One invalidation for `tracker`'s keys (none: a flush), where its
  // redirection says.
  void deliver_locked(Session& tracker, const std::vector<std::string>* keys, std::string* out) DANNAN_REQUIRES(_mutex);

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
  Acl _acl DANNAN_GUARDED_BY(_mutex);
  // MONITOR's connections, by id.
  std::map<int64_t, Session*> _monitors DANNAN_GUARDED_BY(_mutex);
  // Waits ended by CLIENT UNBLOCK or UNPAUSE during a command: woken after it.
  Woken _released DANNAN_GUARDED_BY(_mutex);
  // CLIENT PAUSE: until when (wall clock ms; zero: not paused), and whether
  // every command is held or only those that write.
  uint64_t _pause_until_ms DANNAN_GUARDED_BY(_mutex) = 0;
  bool _pause_all DANNAN_GUARDED_BY(_mutex) = false;
  // Client-side caching: per key (any database, as Valkey's), the connections
  // that read it; per BCAST prefix, its connections, and the keys changed
  // under it during this command with the connection that changed each (zero:
  // none, or more than one); the running connection's own invalidations (a
  // flush is none); and that connection.
  std::unordered_map<std::string, std::set<int64_t>> _tracked DANNAN_GUARDED_BY(_mutex);
  std::map<std::string, std::set<int64_t>> _prefixes DANNAN_GUARDED_BY(_mutex);
  std::map<std::string, std::map<std::string, int64_t>> _broadcast DANNAN_GUARDED_BY(_mutex);
  std::vector<std::optional<std::string>> _own_invalidations DANNAN_GUARDED_BY(_mutex);
  Session* _current DANNAN_GUARDED_BY(_mutex) = nullptr;
  // The classes notify-keyspace-events takes, as bits; the events waiting to
  // be published.
  uint32_t _notify_flags DANNAN_GUARDED_BY(_mutex) = 0;
  std::vector<Event> _events DANNAN_GUARDED_BY(_mutex);
  // A committed record being installed: its keyspace's "new" is the commit's
  // to announce, from the result it carries.
  bool _installing DANNAN_GUARDED_BY(_mutex) = false;
  // Per key (scoped), the version of the commit this node's view holds.
  std::unordered_map<std::string, std::string> _versions DANNAN_GUARDED_BY(_mutex);
  // A write by agreement running speculatively: its own keyspace for its
  // database (an EXEC's queued commands run against it too), the key's write
  // count as the agreement found it, and no side effect leaves it.
  bool _speculating DANNAN_GUARDED_BY(_mutex) = false;
  // Blocking pops' attempts to propose once the lock is let go.
  std::vector<std::shared_ptr<Proposal>> _pending DANNAN_GUARDED_BY(_mutex);
  keyspace::Keyspace* _scratch DANNAN_GUARDED_BY(_mutex) = nullptr;
  size_t _scratch_db DANNAN_GUARDED_BY(_mutex) = 0;
  uint64_t _scratch_serial DANNAN_GUARDED_BY(_mutex) = 0;
};

}  // namespace mimir::command
