//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The connection and server commands, as Valkey 8 documents them.

#include <algorithm>
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

// One line of CLIENT LIST, in Valkey's field order for the fields Mimir keeps.
std::string client_line(const Session& s, uint64_t now) {
  std::string flags;
  if (s.subscriptions > 0 || s.shard_subscriptions > 0) flags += "P";
  if (s.in_multi) flags += "x";
  if (flags.empty()) flags = "N";
  std::ostringstream line;
  line << "id=" << s.id << " addr=" << s.address << " laddr= fd=-1 name=" << s.name << " age=" << (now - std::min(now, s.created_ms)) / 1000
       << " idle=" << (now - std::min(now, s.last_ms)) / 1000 << " flags=" << flags << " capa= db=0 sub=" << s.subscriptions
       << " psub=0 ssub=" << s.shard_subscriptions << " multi=" << (s.in_multi ? static_cast<int64_t>(s.queued.size()) : -1) << " watch=" << s.watched.size()
       << " qbuf=0 qbuf-free=0 argv-mem=0 multi-mem=0"
       << " rbs=0 rbp=0 obl=0 oll=0 omem=0 tot-mem=0 events=r cmd=" << s.last_command
       << " user=default redir=-1 resp=" << (s.protocol == resp::Protocol::resp3 ? 3 : 2) << " lib-name=" << s.lib_name << " lib-ver=" << s.lib_version
       << " tot-cmds=" << s.commands << "\n";
  return line.str();
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

// Called under the database's lock, as every handler is.
void client(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
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
  } else if (sub == "info") {
    if (call.args.size() != 2) wrong_arity("client|info");
    call.reply.verbatim("txt", client_line(call.session, call.keys.now_ms()));
  } else if (sub == "list") {
    std::optional<std::string> type;
    std::vector<int64_t> ids;
    for (size_t i = 2; i < call.args.size(); ++i) {
      if (iequals(call.args[i], "TYPE") && i + 1 < call.args.size()) {
        type = lower(call.args[++i]);
        if (*type != "normal" && *type != "pubsub" && *type != "master" && *type != "replica")
          throw CommandError("ERR Unknown client type '" + call.args[i] + "'");
      } else if (iequals(call.args[i], "ID") && i + 1 < call.args.size()) {
        while (i + 1 < call.args.size()) {
          auto id = parse_integer(call.args[++i]);
          if (!id || *id <= 0) throw CommandError("ERR Invalid client ID");
          ids.push_back(*id);
        }
      } else {
        throw CommandError(syntax_error);
      }
    }
    std::string text;
    const uint64_t now = call.keys.now_ms();
    for (const auto& [id, session] : call.database.sessions_locked()) {
      if (!ids.empty() && std::find(ids.begin(), ids.end(), id) == ids.end()) continue;
      const bool pubsub = session->subscriptions > 0 || session->shard_subscriptions > 0;
      if (type && ((*type == "pubsub") != pubsub || *type == "master" || *type == "replica")) continue;
      text += client_line(*session, now);
    }
    call.reply.verbatim("txt", text);
  } else if (sub == "kill") {
    // The old form, CLIENT KILL addr, answers OK; the new answers how many.
    if (call.args.size() == 3) {
      for (const auto& [id, session] : call.database.sessions_locked()) {
        if (session->address == call.args[2] && session->kill) {
          session->kill();
          call.reply.simple("OK");
          return;
        }
      }
      throw CommandError("ERR No such client");
    }
    std::optional<int64_t> by_id;
    std::optional<std::string> by_addr;
    bool skipme = true;
    for (size_t i = 2; i + 1 < call.args.size(); i += 2) {
      const std::string filter = lower(call.args[i]);
      if (filter == "id") {
        auto id = parse_integer(call.args[i + 1]);
        if (!id || *id <= 0) throw CommandError("ERR client-id should be greater than 0");
        by_id = *id;
      } else if (filter == "addr") {
        by_addr = call.args[i + 1];
      } else if (filter == "skipme") {
        skipme = !iequals(call.args[i + 1], "no");
      } else {
        throw CommandError(syntax_error);
      }
    }
    if (call.args.size() % 2 != 0) throw CommandError(syntax_error);
    int64_t killed = 0;
    for (const auto& [id, session] : call.database.sessions_locked()) {
      if (by_id && id != *by_id) continue;
      if (by_addr && session->address != *by_addr) continue;
      if (skipme && id == call.session.id) continue;
      if (session->kill) session->kill();
      ++killed;
    }
    call.reply.integer(killed);
  } else if (sub == "no-evict" || sub == "no-touch") {
    if (call.args.size() != 3 || (!iequals(call.args[2], "on") && !iequals(call.args[2], "off"))) throw CommandError(syntax_error);
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

void info(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  const DatabaseSettings& settings = call.database.settings();
  // The sections asked for: none or "default" or "all" or "everything" is
  // every section Mimir keeps; names are case-insensitive.
  std::vector<std::string> asked;
  for (size_t i = 1; i < call.args.size(); ++i) asked.push_back(lower(call.args[i]));
  const bool every =
      asked.empty() || std::any_of(asked.begin(), asked.end(), [](const std::string& s) { return s == "default" || s == "all" || s == "everything"; });
  const auto wanted = [&](std::string_view section) { return every || std::find(asked.begin(), asked.end(), section) != asked.end(); };
  const uint64_t now = call.keys.now_ms();
  const uint64_t uptime = (now - std::min(now, call.database.started_ms())) / 1000;
  std::ostringstream text;
  bool first = true;
  const auto section = [&](std::string_view title) {
    if (!first) text << "\r\n";
    first = false;
    text << "# " << title << "\r\n";
  };
  if (wanted("server")) {
    section("Server");
    text << "redis_version:" << settings.redis_version << "\r\n"
         << "server_name:" << settings.server_name << "\r\n"
         << "valkey_version:" << settings.compatible_version << "\r\n"
         << "mimir_version:" << mimir::version << "\r\n"
         << "server_mode:standalone\r\n"
         << "redis_mode:standalone\r\n"
         << "process_id:0\r\n"
         << "uptime_in_seconds:" << uptime << "\r\n"
         << "uptime_in_days:" << uptime / 86400 << "\r\n"
         << "hz:10\r\n";
  }
  if (wanted("clients")) {
    section("Clients");
    size_t pubsub = 0, watching = 0;
    for (const auto& [id, s] : call.database.sessions_locked()) {
      pubsub += (s->subscriptions > 0 || s->shard_subscriptions > 0) ? 1 : 0;
      watching += s->watched.empty() ? 0 : 1;
    }
    text << "connected_clients:" << call.database.sessions_locked().size() << "\r\n"
         << "blocked_clients:" << call.database.waiting_locked() << "\r\n"
         << "pubsub_clients:" << pubsub << "\r\n"
         << "watching_clients:" << watching << "\r\n";
  }
  if (wanted("memory")) {
    section("Memory");
    text << "maxmemory:" << call.database.config_locked()["maxmemory"] << "\r\n"
         << "maxmemory_policy:" << call.database.config_locked()["maxmemory-policy"] << "\r\n";
  }
  if (wanted("persistence")) {
    section("Persistence");
    text << "loading:0\r\n"
         << "aof_enabled:0\r\n";
  }
  if (wanted("replication")) {
    section("Replication");
    text << "role:master\r\n"
         << "connected_slaves:0\r\n";
  }
  if (wanted("keyspace")) {
    section("Keyspace");
    if (call.keys.size() > 0) text << "db0:keys=" << call.keys.size() << ",expires=" << call.keys.expiring() << ",avg_ttl=0\r\n";
  }
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
