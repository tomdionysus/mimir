//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/command/database.h"

#include "mimir/command/args.h"

namespace mimir::command {

namespace {

// Valkey's words for a command it does not know: the name and the first
// arguments, each cut so the whole stays short.
std::string unknown_command(const std::vector<std::string>& args) {
  std::string listed;
  for (size_t i = 1; i < args.size() && listed.size() < 128; ++i) {
    listed += "'" + args[i].substr(0, 128 - listed.size()) + "' ";
  }
  return "ERR unknown command '" + args[0].substr(0, 128) + "', with args beginning with: " + listed;
}

}  // namespace

Database::Database(const dannan::contract::WallClock& wall, DatabaseSettings settings)
    : _wall(wall), _settings(std::move(settings)), _keys(wall), _random(std::random_device{}()) {}

void Database::execute(Session& session, const std::vector<std::string>& args, std::string& out) {
  resp::Reply reply(session.protocol);
  if (args.empty()) return;
  const Spec* spec = find_spec(args[0]);
  if (!spec) {
    reply.error(unknown_command(args));
    out.append(reply.bytes());
    return;
  }
  const int count = static_cast<int>(args.size());
  if ((spec->arity > 0 && count != spec->arity) || (spec->arity < 0 && count < -spec->arity)) {
    reply.error("ERR wrong number of arguments for '" + std::string(spec->name) + "' command");
    out.append(reply.bytes());
    return;
  }
  dannan::contract::Lock lock(_mutex);
  Call call{session, *this, _keys, reply, args};
  try {
    spec->handler(call);
  } catch (const CommandError& e) {
    // A handler refuses before it changes anything; what it wrote is dropped.
    reply = resp::Reply(session.protocol);
    reply.error(e.what());
  }
  out.append(reply.bytes());
}

size_t Database::expire_step(size_t budget) {
  dannan::contract::Lock lock(_mutex);
  return _keys.expire_step(budget);
}

}  // namespace mimir::command
