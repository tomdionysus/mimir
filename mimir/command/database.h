//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <array>
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
#include "mimir/command/args.h"
#include "mimir/command/pubsub.h"
#include "mimir/command/scripting.h"
#include "mimir/command/table.h"
#include "mimir/keyspace/keyspace.h"
#include "mimir/keyspace/limits.h"
#include "mimir/resp/reply.h"

// One node's database at the single-node stage: the keyspace and the command
// table's dispatch. Every connection's strand calls execute(); each command
// runs whole under the database's lock, so on one node every command is atomic
// as Valkey's are. The cluster stage replaces the one lock with a shard's.
namespace mimir::command {

// A node of the cluster as a client can reach it (ruling 40): its id, its
// name, where it takes clients, and whether it is the node asked.
struct NodeAddress {
  std::string id;
  std::string name;
  std::string host;
  uint16_t port = 0;
  // Zero: no TLS port.
  uint16_t tls_port = 0;
  bool self = false;
};

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
  // latency-tracking at start: each command's runs timed into its histogram,
  // LATENCY HISTOGRAM's answer. On by default, as Valkey's; CONFIG SET
  // changes it.
  bool latency_tracking = true;
  // The monotonic clock commands are timed by; none: the steady clock.
  const dannan::contract::TimeSource* time = nullptr;
  // SHUTDOWN's way to stop the node (ruling 31), called on the command's
  // thread under the database's lock: it must only post the stop. None: a
  // client cannot stop this node.
  std::function<void()> shutdown;
  // The cluster: every write commits by agreement (ruling 34). Required; a
  // write of several keys is refused where the agreements cannot commit
  // several at once (ruling 35).
  std::shared_ptr<Agreements> agreements;
  // Every active node's client address, this one's included (MIMIR NODES,
  // ruling 40). Called under the database's lock: it must only read. None:
  // the list is empty.
  std::function<std::vector<NodeAddress>()> nodes;
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
  // Closes its agreements: nothing they were given calls back into it after.
  ~Database();

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
  // the target took is removed by agreement (unless COPY), but only while it
  // holds what was moved, and `done` is given the reply's bytes once every
  // removal is decided, as Valkey answers only once the keys are gone. `done`
  // is called on the thread that decides the last, or at once when there is
  // none; the caller posts it on. Thread-safe.
  void finish_migrate(Session& session, const MigrateJob& job, const MigrateOutcome& outcome, std::function<void(std::string reply)> done);

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
  // None while an ACL change runs speculatively: it closes no connection
  // until it commits.
  std::map<int64_t, Session*>& sessions_locked() DANNAN_REQUIRES(_mutex) { return _scratch_acl ? _no_sessions : _sessions; }
  // While a CONFIG SET runs speculatively (in a cluster), its own copy.
  std::map<std::string, std::string>& config_locked() DANNAN_REQUIRES(_mutex) { return _scratch_config ? *_scratch_config : _config; }
  // Whether `name` (lower case) is a cluster-wide setting: one CONFIG SET
  // changes on every node, by agreement.
  static bool cluster_setting(std::string_view name) noexcept;
  // The encoding thresholds as the settings hold them now.
  keyspace::Limits limits_locked() DANNAN_REQUIRES(_mutex);
  // LATENCY HISTOGRAM: per command's full name ("get", "config|get"), how
  // many runs were timed and how many fell in each step, as Valkey's HDR
  // histogram reports them: step i holds runs up to latency_step_ns(i)
  // nanoseconds (1,031, 2,063, 4,127 ...), shown as that over a thousand
  // microseconds. A run is counted at least 1,024 ns and at most a second.
  static constexpr size_t latency_steps = 21;
  struct LatencyHistogram {
    uint64_t calls = 0;
    std::array<uint64_t, latency_steps> steps{};
  };
  static uint64_t latency_step_ns(size_t step) noexcept { return (uint64_t{1024} << step) + (uint64_t{8} << step) - 1; }
  const std::map<std::string, LatencyHistogram>& latency_locked() DANNAN_REQUIRES(_mutex) { return _latency; }
  // CONFIG RESETSTAT.
  void reset_latency_locked() DANNAN_REQUIRES(_mutex) { _latency.clear(); }
  // A run of `name` that started at `started`, counted while latency-tracking
  // is on.
  void record_latency_locked(const std::string& name, dannan::contract::TimePoint started) DANNAN_REQUIRES(_mutex);
  dannan::contract::TimePoint latency_now() const noexcept { return _time.now(); }
  size_t waiting_locked() DANNAN_REQUIRES(_mutex) { return _waiters.size(); }
  // The databases, for SELECT, MOVE, SWAPDB, COPY ... DB, FLUSHALL and INFO.
  // Fixed at construction, so read without the lock.
  size_t databases() const noexcept DANNAN_NO_THREAD_SAFETY_ANALYSIS { return _dbs.size(); }
  // During a speculative run, its own keyspace of that database; one it does
  // not name is refused.
  keyspace::Keyspace& db_locked(size_t index) DANNAN_REQUIRES(_mutex) {
    if (!_speculating) return *_dbs.at(index);
    if (!_scratches.count(index)) throw CommandError("ERR Mimir runs this by agreement on the databases it names");
    return *_scratches.at(index);
  }
  // SWAPDB: the two exchange their keys; watches on either break.
  void swap_locked(size_t a, size_t b) DANNAN_REQUIRES(_mutex);
  // The lock the transaction commands are called under, for their annotations.
  dannan::contract::Mutex& lock_for_transactions() DANNAN_RETURN_CAPABILITY(_mutex) { return _mutex; }
  // The scripts' interpreter and cache, one for every database as Valkey's.
  // While a FUNCTION change runs speculatively (in a cluster), its own copy.
  Scripting& scripting_locked() DANNAN_REQUIRES(_mutex) { return _scratch_scripting ? *_scratch_scripting : _scripting; }
  // The users, their permissions and the log of refusals. While an ACL change
  // runs speculatively (in a cluster), its own copy of the users.
  Acl& acl_locked() DANNAN_REQUIRES(_mutex) { return _scratch_acl ? *_scratch_acl : _acl; }
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
    // Every key it names, in its databases, when it names more than one (a
    // change of several keys: rulings 22 and 29); empty for one, `db` and
    // `key` then naming it.
    std::vector<Agreements::Name> names;
    // MIGRATE's removal of a key it moved: only while the key still holds
    // this payload (a write since keeps it). No spec, no waiter.
    std::optional<std::string> remove_if;
    // Called once it is decided, whatever the outcome (MIGRATE's answer
    // waits for its removals).
    std::function<void()> finished;
    // A blocking pop proposed again because a write made one of its keys
    // ready: a key now of another type serves it nothing, and it waits on.
    bool woken = false;
    // Each key's committed version in this node's view when this attempt was
    // made: a blocking pop that finds nothing is tried again at once only if
    // one of its keys has been written since.
    std::vector<std::string> seen;
    // Not a client's command: the deletion of a key this node's view found
    // expired, which commits only if the key is expired at `issued_ms` in
    // the agreed record (a later EXPIRE elsewhere wins). No spec, no waiter.
    bool expiry = false;
    // ACL SETUSER or DELUSER, on the users' register (ruling 27).
    bool acl = false;
    // FUNCTION LOAD, DELETE, FLUSH or RESTORE, on the libraries' register
    // (ruling 39).
    bool functions = false;
    // CONFIG SET of a cluster-wide setting, on the settings' register (the
    // maintainer, 2026-11-23: the behaviour settings).
    bool config = false;
    // SCRIPT LOAD, or an EVAL's script shared: on the register of its SHA
    // (ruling 39). An EVAL's has no waiter.
    bool script = false;
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
  // In a cluster, a key this node's view found expired is deleted by
  // agreement, so the acceptors drop it too; queued for start_pending.
  void expire_by_agreement_locked(size_t db, std::string_view key) DANNAN_REQUIRES(_mutex);
  // The expiry's change: the key deleted if it has expired by the
  // proposal's issue time, otherwise nothing changed.
  Agreements::Changed speculate_expiry(const Proposal& proposal, const cluster::Record& base);
  // An ACL change's change: run on the users the record holds (or, before the
  // first, those the settings seed), the new users its record.
  Agreements::Changed speculate_acl(const Proposal& proposal, const cluster::Record& base);
  // The users' record committed: this node's users become it, and the
  // connections of a user gone, or whose channels narrowed under them, are
  // closed (`own` after its reply, into `closing`). Whether it was newer.
  bool install_acl_locked(const std::string& version, const cluster::Record& record, Session* own, std::vector<Session*>& closing) DANNAN_REQUIRES(_mutex);
  // A FUNCTION change's change, on the libraries the record holds (none
  // before the first), and the libraries' record installed as this node's.
  Agreements::Changed speculate_functions(const Proposal& proposal, const cluster::Record& base);
  bool install_functions_locked(const std::string& version, const cluster::Record& record) DANNAN_REQUIRES(_mutex);
  // A CONFIG SET of a cluster-wide setting: run on the settings the record
  // holds (this node's before the first), the cluster-wide ones its record;
  // and the record installed as this node's.
  Agreements::Changed speculate_config(const Proposal& proposal, const cluster::Record& base);
  bool install_config_locked(const std::string& version, const cluster::Record& record) DANNAN_REQUIRES(_mutex);
  // A script's change: SCRIPT LOAD run here (it compiles the body, and
  // caches it), the body its register's record.
  Agreements::Changed speculate_script(const Proposal& proposal, const cluster::Record& base);
  // An EVAL's script, shared with the cluster once by this node, in the
  // background; queued for start_pending.
  void share_script_locked(const std::string& body) DANNAN_REQUIRES(_mutex);
  // An EVALSHA's script missing here, read from its register at a majority
  // and cached; the command then runs again (its wait is a postponed one).
  void find_script(uint64_t wait, std::string sha);
  // A held SUBSCRIBE's names are routed here by the other nodes: it runs
  // again (its wait is a postponed one).
  void subscribe_routed(uint64_t wait);
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
  // The same over every key it names, each in a keyspace of its database
  // holding only the named keys (`bases` in the order named).
  Agreements::ChangedKeys speculate_keys(const Proposal& proposal, const std::vector<cluster::Record>& bases) DANNAN_EXCLUDES(_mutex);
  // MIGRATE's removal of a moved key: the key deleted while it still holds
  // the payload moved, otherwise nothing changed.
  Agreements::Changed speculate_removal(const Proposal& proposal, const cluster::Record& base);
  // The keys a write names, in their databases: its own, MOVE's and COPY's
  // target, every key of this node's view for the flushes and SWAPDB, and an
  // EXEC's queue's and watches'.
  std::vector<Agreements::Name> names_locked(const Spec& spec, const std::vector<std::string>& args, const Session& session, size_t db) DANNAN_REQUIRES(_mutex);
  // The versions this node's view holds of a proposal's keys, now.
  std::vector<std::string> versions_of_locked(const Proposal& proposal) DANNAN_REQUIRES(_mutex);
  // The outcome of a change of several keys.
  void decided_keys(const std::shared_ptr<Proposal>& proposal, Agreements::KeysOutcome outcome);
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
  // A read on a CLIENT STRICT connection (ruling 37): each key read from a
  // majority and made this node's view, then the command run here and
  // answered; refused when a majority cannot be reached.
  void read_strictly(uint64_t wait, size_t db, std::vector<std::string> keys, std::vector<std::string> args) DANNAN_EXCLUDES(_mutex);
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
  const dannan::contract::TimeSource& _time;
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
  std::map<std::string, LatencyHistogram> _latency DANNAN_GUARDED_BY(_mutex);
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
  // Blocking pops' attempts, and expiries, to propose once the lock is let go.
  std::vector<std::shared_ptr<Proposal>> _pending DANNAN_GUARDED_BY(_mutex);
  // An ACL change's speculative users, and the connections it may not close.
  Acl* _scratch_acl DANNAN_GUARDED_BY(_mutex) = nullptr;
  // A FUNCTION change's speculative libraries.
  Scripting* _scratch_scripting DANNAN_GUARDED_BY(_mutex) = nullptr;
  // A CONFIG SET's speculative settings.
  std::map<std::string, std::string>* _scratch_config DANNAN_GUARDED_BY(_mutex) = nullptr;
  // The scripts this node has shared with the cluster since its last SCRIPT
  // FLUSH, and when that was: a script's register last written before it
  // is not brought back here by EVALSHA.
  std::set<std::string> _shared_scripts DANNAN_GUARDED_BY(_mutex);
  uint64_t _scripts_flushed_ms DANNAN_GUARDED_BY(_mutex) = 0;
  std::map<int64_t, Session*> _no_sessions DANNAN_GUARDED_BY(_mutex);
  // Keys (scoped) whose expiry is proposed: one proposal each at a time.
  std::set<std::string> _expiring DANNAN_GUARDED_BY(_mutex);
  // Set while a write looks its own key up before proposing: an expiry it
  // finds is its own speculation's to take out, not a separate proposal's.
  bool _writing_lookup DANNAN_GUARDED_BY(_mutex) = false;
  // A speculative run's own keyspace of each database it names, the keys it
  // may touch (scoped), and each one's write count as the agreement found it.
  std::map<size_t, keyspace::Keyspace*> _scratches DANNAN_GUARDED_BY(_mutex);
  std::set<std::string> _scratch_names DANNAN_GUARDED_BY(_mutex);
  std::map<std::string, uint64_t> _scratch_serials DANNAN_GUARDED_BY(_mutex);
};

}  // namespace mimir::command
