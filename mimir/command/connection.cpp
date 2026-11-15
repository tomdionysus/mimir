//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The connection and server commands, as Valkey 8 documents them.

#include <sstream>

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"
#include "mimir/version.h"

namespace mimir::command::connection {

namespace {

// A client name or library attribute: printable, no spaces (Valkey's rule).
bool valid_name(std::string_view name) {
  for (unsigned char c : name) {
    if (c < '!' || c > '~') return false;
  }
  return true;
}

void unknown_subcommand(const std::string& subcommand, std::string_view command) {
  throw CommandError("ERR unknown subcommand '" + subcommand + "'. Try " + upper(command) + " HELP.");
}

void wrong_arity(std::string_view name) { throw CommandError("ERR wrong number of arguments for '" + std::string(name) + "' command"); }

void command_info(resp::Reply& reply, const Spec& spec) {
  reply.array(10);
  reply.bulk(spec.name);
  reply.integer(spec.arity);
  std::vector<std::string_view> flags;
  std::string_view rest = spec.flags;
  while (!rest.empty()) {
    const size_t space = rest.find(' ');
    flags.push_back(rest.substr(0, space));
    rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
  }
  reply.set(flags.size());
  for (auto flag : flags) reply.simple(flag);
  reply.integer(spec.first_key);
  reply.integer(spec.last_key);
  reply.integer(spec.step);
  // ACL categories, tips, key specifications and subcommands: not yet kept.
  reply.set(0);
  reply.set(0);
  reply.array(0);
  reply.array(0);
}

}  // namespace

void ping(Call& call) {
  if (call.args.size() > 2) wrong_arity("ping");
  // In RESP2's subscribed mode a PING answers as a message would.
  if (call.session.protocol == resp::Protocol::resp2 && (call.session.subscriptions > 0 || call.session.shard_subscriptions > 0)) {
    call.reply.array(2);
    call.reply.bulk("pong");
    call.reply.bulk(call.args.size() == 2 ? call.args[1] : "");
    return;
  }
  if (call.args.size() == 2)
    call.reply.bulk(call.args[1]);
  else
    call.reply.simple("PONG");
}

void echo(Call& call) { call.reply.bulk(call.args[1]); }

void quit(Call& call) {
  call.session.closing = true;
  call.reply.simple("OK");
}

void select(Call& call) {
  const int64_t index = integer_argument(call.args[1]);
  if (index != 0) throw CommandError("ERR DB index is out of range");
  call.reply.simple("OK");
}

void hello(Call& call) {
  resp::Protocol protocol = call.session.protocol;
  std::optional<std::string> name;
  size_t i = 1;
  if (call.args.size() > 1) {
    auto version = parse_integer(call.args[1]);
    if (!version) throw CommandError("ERR Protocol version is not an integer or out of range");
    if (*version != 2 && *version != 3) throw CommandError("NOPROTO unsupported protocol version");
    protocol = *version == 3 ? resp::Protocol::resp3 : resp::Protocol::resp2;
    i = 2;
  }
  for (; i < call.args.size(); ++i) {
    const bool more = i + 1 < call.args.size();
    if (iequals(call.args[i], "AUTH") && i + 2 < call.args.size()) {
      throw CommandError("ERR AUTH <password> called without any password configured for the default user. Are you sure your configuration is correct?");
    } else if (iequals(call.args[i], "SETNAME") && more) {
      name = call.args[++i];
      if (!valid_name(*name)) throw CommandError("ERR Client names cannot contain spaces, newlines or special characters.");
    } else {
      throw CommandError("ERR Syntax error in HELLO option '" + call.args[i] + "'");
    }
  }
  call.session.protocol = protocol;
  call.database.pubsub().set_protocol(call.session.id, protocol);
  call.reply = resp::Reply(protocol);
  if (name) call.session.name = *name;
  const DatabaseSettings& settings = call.database.settings();
  call.reply.map(7);
  call.reply.bulk("server");
  call.reply.bulk(settings.server_name);
  call.reply.bulk("version");
  call.reply.bulk(settings.compatible_version);
  call.reply.bulk("proto");
  call.reply.integer(protocol == resp::Protocol::resp3 ? 3 : 2);
  call.reply.bulk("id");
  call.reply.integer(call.session.id);
  call.reply.bulk("mode");
  call.reply.bulk("standalone");
  call.reply.bulk("role");
  call.reply.bulk("master");
  call.reply.bulk("modules");
  call.reply.array(0);
}

void auth(Call& call) {
  if (call.args.size() > 3) throw CommandError(syntax_error);
  throw CommandError("ERR AUTH <password> called without any password configured for the default user. Are you sure your configuration is correct?");
}

void client(Call& call) {
  const std::string sub = lower(call.args[1]);
  if (sub == "id") {
    if (call.args.size() != 2) wrong_arity("client|id");
    call.reply.integer(call.session.id);
  } else if (sub == "setname") {
    if (call.args.size() != 3) wrong_arity("client|setname");
    if (!valid_name(call.args[2])) throw CommandError("ERR Client names cannot contain spaces, newlines or special characters.");
    call.session.name = call.args[2];
    call.reply.simple("OK");
  } else if (sub == "getname") {
    if (call.args.size() != 2) wrong_arity("client|getname");
    if (call.session.name.empty())
      call.reply.null();
    else
      call.reply.bulk(call.session.name);
  } else if (sub == "setinfo") {
    if (call.args.size() != 4) wrong_arity("client|setinfo");
    const std::string attribute = lower(call.args[2]);
    if (attribute != "lib-name" && attribute != "lib-ver") throw CommandError("ERR Unrecognized option '" + call.args[2] + "'");
    if (!valid_name(call.args[3])) throw CommandError("ERR " + attribute + " cannot contain spaces, newlines or special characters.");
    (attribute == "lib-name" ? call.session.lib_name : call.session.lib_version) = call.args[3];
    call.reply.simple("OK");
  } else {
    unknown_subcommand(call.args[1], "client");
  }
}

void command(Call& call) {
  if (call.args.size() == 1) {
    auto specs = all_specs();
    call.reply.array(specs.size());
    for (const Spec& spec : specs) command_info(call.reply, spec);
    return;
  }
  const std::string sub = lower(call.args[1]);
  if (sub == "count") {
    if (call.args.size() != 2) wrong_arity("command|count");
    call.reply.integer(static_cast<int64_t>(all_specs().size()));
  } else if (sub == "info") {
    if (call.args.size() == 2) {
      auto specs = all_specs();
      call.reply.array(specs.size());
      for (const Spec& spec : specs) command_info(call.reply, spec);
      return;
    }
    call.reply.array(call.args.size() - 2);
    for (size_t i = 2; i < call.args.size(); ++i) {
      const Spec* spec = find_spec(call.args[i]);
      // An unknown name is a null bulk string, as Valkey answers it.
      if (spec)
        command_info(call.reply, *spec);
      else
        call.reply.null();
    }
  } else if (sub == "list") {
    if (call.args.size() != 2) throw CommandError(syntax_error);
    auto specs = all_specs();
    call.reply.array(specs.size());
    for (const Spec& spec : specs) call.reply.bulk(spec.name);
  } else {
    unknown_subcommand(call.args[1], "command");
  }
}

void info(Call& call) {
  const DatabaseSettings& settings = call.database.settings();
  std::ostringstream text;
  text << "# Server\r\n"
       << "redis_version:" << settings.redis_version << "\r\n"
       << "server_name:" << settings.server_name << "\r\n"
       << "valkey_version:" << settings.compatible_version << "\r\n"
       << "mimir_version:" << mimir::version << "\r\n"
       << "server_mode:standalone\r\n"
       << "redis_mode:standalone\r\n"
       << "\r\n# Keyspace\r\n";
  if (call.keys.size() > 0) text << "db0:keys=" << call.keys.size() << ",expires=" << call.keys.expiring() << ",avg_ttl=0\r\n";
  call.reply.verbatim("txt", text.str());
}

void time(Call& call) {
  const uint64_t now = call.keys.now_ms();
  call.reply.array(2);
  call.reply.bulk(std::to_string(now / 1000));
  call.reply.bulk(std::to_string((now % 1000) * 1000));
}

void reset(Call& call) {
  call.database.pubsub().drop(call.session.id);
  call.session.subscriptions = 0;
  call.session.shard_subscriptions = 0;
  call.session.protocol = resp::Protocol::resp2;
  call.session.name.clear();
  call.reply = resp::Reply(resp::Protocol::resp2);
  call.reply.simple("RESET");
}

}  // namespace mimir::command::connection
