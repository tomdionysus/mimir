//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mimir/keyspace/keyspace.h"
#include "mimir/resp/reply.h"

// The command table: every command Mimir answers, with what Valkey declares of
// it (arity, flags, where its keys are) and what Mimir adds, its class in the
// cluster (design, "The two classes of command"). The class decides, from the
// cluster stage on, whether a command commits by agreement on a majority of
// its key's replicas or merges; on one node every class runs alike.
namespace mimir::command {

enum class Class : uint8_t {
  // Reads a key's value and changes nothing.
  read,
  // Its effect commutes or has a deterministic winner: both sides of a split
  // accept it (ruling 2).
  merge,
  // Its reply or effect depends on the value before it in a way that cannot
  // merge: committed by agreement (ruling 1).
  agreement,
  // A read-modify-write that the operator sets to agreement (the default) or
  // merge (ruling 3).
  configurable,
  // The connection's own state; touches no key.
  connection,
  // The node or the whole keyspace.
  admin,
};

std::string_view class_name(Class) noexcept;

class Database;

// A connection's state, owned by its strand.
struct Session {
  int64_t id = 0;
  // The database SELECT chose.
  size_t db = 0;
  resp::Protocol protocol = resp::Protocol::resp2;
  std::string name;
  std::string lib_name;
  std::string lib_version;
  std::string address;
  uint64_t created_ms = 0;
  // Set by QUIT: the connection closes once this command's reply is sent.
  bool closing = false;
  // Who it is (ACL): its user's name, and whether it has authenticated (or
  // needed not: the default user had no password when it first spoke). Read
  // and written under the database's lock; `welcomed` once they are set.
  std::string user = "default";
  bool authenticated = false;
  bool welcomed = false;
  // It came over TLS: AUTH on a plaintext connection is refused unless the
  // settings allow it (ruling 28).
  bool tls = false;
  // A script's or a function's own session: what it runs must be
  // deterministic (SORT orders a set it would otherwise leave unordered).
  bool scripted = false;
  // How a message published to this connection reaches it, from any thread,
  // waiting on nothing; none: it cannot subscribe.
  std::function<void(std::string)> push;
  // Its channel and pattern subscriptions, and its shard channel ones: while
  // either is above zero a RESP2 connection is in subscribed mode.
  size_t subscriptions = 0;
  size_t shard_subscriptions = 0;

  // What CLIENT LIST shows of it: when it last ran a command and what.
  uint64_t last_ms = 0;
  std::string last_command;
  uint64_t commands = 0;
  // Closes the connection from any thread (CLIENT KILL); none in tests.
  std::function<void()> kill;

  // A transaction: between MULTI and EXEC commands are queued; an error while
  // queueing aborts the EXEC. The keys WATCHed, each with whether it held a
  // value then; `watch_touched` is set, under the database's lock, by any
  // write to one of them.
  bool in_multi = false;
  bool multi_failed = false;
  std::vector<std::vector<std::string>> queued;
  std::vector<std::pair<std::string, bool>> watched;
  bool watch_touched = false;
};

// One command's call: its arguments (its name first), the connection's
// session, the database it runs against, and the reply it writes.
struct Call {
  Call(Session& session_, Database& database_, keyspace::Keyspace& keys_, resp::Reply& reply_, const std::vector<std::string>& args_)
      : session(session_), database(database_), keys(keys_), reply(reply_), args(args_) {}

  Session& session;
  Database& database;
  keyspace::Keyspace& keys;
  resp::Reply& reply;
  const std::vector<std::string>& args;
  // The number of the database `keys` is.
  size_t db = 0;

  // A keyspace notification for `key` in this call's database (Database::
  // notify_locked). Called after the change it announces.
  void notify(char cls, std::string_view event, std::string_view key);

  // A blocking command (BLPOP and its kin). Serving a waiter that a write woke,
  // `ready_key` is the key that became ready and the only one the command may
  // take from. A blocking command that finds nothing to take writes no reply
  // and sets `blocked`, the keys it waits on and its timeout in seconds (zero:
  // for ever); the database then parks it, or answers its timeout reply at
  // once where it may not wait.
  const std::string* ready_key = nullptr;
  bool blocked = false;
  std::vector<std::string> block_keys;
  double block_seconds = 0;
  // Its timeout reply is a null bulk string (BLMOVE, BRPOPLPUSH), not a null array.
  bool block_null_bulk = false;
  // The arguments the parked command is run with when woken, when they differ
  // from those it came with (XREAD's "$" made the id it stood for).
  std::optional<std::vector<std::string>> block_args;
};

using Handler = void (*)(Call&);
// The class of a command whose class depends on its arguments (SET NX).
using Classifier = Class (*)(const std::vector<std::string>&);
// The keys of a command whose keys the positions cannot give (numkeys).
using KeyFinder = std::vector<std::string_view> (*)(const std::vector<std::string>&);

struct Spec {
  // Lower case, as Valkey names it in errors and COMMAND.
  std::string_view name;
  // Valkey's convention: N exactly N arguments with the name; -N at least N.
  int arity;
  // COMMAND's flags, space-separated: "write denyoom", "readonly fast".
  std::string_view flags;
  // The first and last key's argument index (last -1: the final argument) and
  // the step between keys; 0, 0, 0 for none.
  int first_key;
  int last_key;
  int step;
  Class cls;
  Handler handler;
  Classifier classify = nullptr;
  KeyFinder find_keys = nullptr;
};

// The command named `name`, in any case, or null.
const Spec* find_spec(std::string_view name) noexcept;
std::span<const Spec> all_specs() noexcept;
// The class of this call of `spec`.
Class class_of(const Spec& spec, const std::vector<std::string>& args);
// The key arguments of this call of `spec`, by its first, last and step.
std::vector<std::string_view> keys_of(const Spec& spec, const std::vector<std::string>& args);

}  // namespace mimir::command
