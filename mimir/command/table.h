//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
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
  resp::Protocol protocol = resp::Protocol::resp2;
  std::string name;
  std::string lib_name;
  std::string lib_version;
  std::string address;
  uint64_t created_ms = 0;
  // Set by QUIT: the connection closes once this command's reply is sent.
  bool closing = false;
};

// One command's call: its arguments (its name first), the connection's
// session, the database it runs against, and the reply it writes.
struct Call {
  Session& session;
  Database& database;
  keyspace::Keyspace& keys;
  resp::Reply& reply;
  const std::vector<std::string>& args;
};

using Handler = void (*)(Call&);
// The class of a command whose class depends on its arguments (SET NX).
using Classifier = Class (*)(const std::vector<std::string>&);

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
};

// The command named `name`, in any case, or null.
const Spec* find_spec(std::string_view name) noexcept;
std::span<const Spec> all_specs() noexcept;
// The class of this call of `spec`.
Class class_of(const Spec& spec, const std::vector<std::string>& args);
// The key arguments of this call of `spec`, by its first, last and step.
std::vector<std::string_view> keys_of(const Spec& spec, const std::vector<std::string>& args);

}  // namespace mimir::command
