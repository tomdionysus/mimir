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
#include "mimir/keyspace/glob.h"
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
  const auto categories = Acl::categories_of(spec.name);
  reply.set(categories.size());
  for (const auto& category : categories) reply.simple(category);
  // Tips, key specifications and subcommands: not yet kept.
  reply.set(0);
  reply.array(0);
  reply.array(0);
}

// AUTH and HELLO's AUTH: refused on a plaintext connection unless the settings
// allow it (ruling 28), then the user's password checked; a refusal is logged.
void authenticate(Call& call, const std::string& username, const std::string& password) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  if (!call.session.tls && !call.database.settings().auth_plaintext)
    throw CommandError("ERR AUTH needs a TLS connection on this server: a password is never taken in clear (auth.plaintext is off)");
  if (!call.database.acl_locked().authenticate(username, password)) {
    call.database.log_auth_failure_locked(call.session, username);
    throw CommandError(wrong_pass);
  }
  call.session.user = username;
  call.session.authenticated = true;
}

// Whether one prefix begins the other: BCAST's prefixes for one connection
// must not, as Valkey requires.
bool overlaps(std::string_view a, std::string_view b) {
  const size_t n = std::min(a.size(), b.size());
  return a.substr(0, n) == b.substr(0, n);
}

// CLIENT TRACKING ON|OFF and its options, checked in Valkey's order.
void tracking(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  int64_t redirect = 0;
  bool bcast = false, optin = false, optout = false, noloop = false;
  std::vector<std::string> prefixes;
  for (size_t i = 3; i < call.args.size(); ++i) {
    const bool more = i + 1 < call.args.size();
    const std::string& option = call.args[i];
    if (iequals(option, "REDIRECT") && more) {
      if (redirect != 0) throw CommandError("ERR A client can only redirect to a single other client");
      redirect = integer_argument(call.args[++i]);
      if (!call.database.has_session_locked(redirect)) throw CommandError("ERR The client ID you want redirect to does not exist");
    } else if (iequals(option, "BCAST")) {
      bcast = true;
    } else if (iequals(option, "OPTIN")) {
      optin = true;
    } else if (iequals(option, "OPTOUT")) {
      optout = true;
    } else if (iequals(option, "NOLOOP")) {
      noloop = true;
    } else if (iequals(option, "PREFIX") && more) {
      prefixes.push_back(call.args[++i]);
    } else {
      throw CommandError(syntax_error);
    }
  }
  auto& t = call.session.tracking;
  if (iequals(call.args[2], "OFF")) {
    call.database.track_off_locked(call.session);
    call.reply.simple("OK");
    return;
  }
  if (!iequals(call.args[2], "ON")) throw CommandError(syntax_error);
  if (!bcast && !prefixes.empty()) throw CommandError("ERR PREFIX option requires BCAST mode to be enabled");
  if (t.on && t.bcast != bcast)
    throw CommandError("ERR You can't switch BCAST mode on/off before disabling tracking for this client, and then re-enabling it with a different mode.");
  if (bcast && (optin || optout)) throw CommandError("ERR OPTIN and OPTOUT are not compatible with BCAST");
  if (optin && optout) throw CommandError("ERR You can't specify both OPTIN mode and OPTOUT mode");
  if ((optin && t.optout) || (optout && t.optin))
    throw CommandError("ERR You can't switch OPTIN/OPTOUT mode before disabling tracking for this client, and then re-enabling it with a different mode.");
  if (bcast) {
    for (size_t i = 0; i < prefixes.size(); ++i) {
      for (const auto& existing : t.prefixes)
        if (overlaps(existing, prefixes[i]))
          throw CommandError("ERR Prefix '" + prefixes[i] + "' overlaps with an existing prefix '" + existing +
                             "'. Prefixes for a single client must not overlap.");
      for (size_t j = i + 1; j < prefixes.size(); ++j)
        if (overlaps(prefixes[i], prefixes[j]))
          throw CommandError("ERR Prefix '" + prefixes[i] + "' overlaps with another provided prefix '" + prefixes[j] +
                             "'. Prefixes for a single client must not overlap.");
    }
  }
  t.on = true;
  t.broken = false;
  t.redirect = redirect;
  t.bcast = bcast;
  t.optin = optin;
  t.optout = optout;
  t.noloop = noloop;
  if (bcast) {
    // BCAST with no prefix hears of every key.
    if (prefixes.empty()) prefixes.emplace_back();
    for (auto& prefix : prefixes) t.prefixes.insert(std::move(prefix));
    call.database.track_on_locked(call.session);
  }
  call.reply.simple("OK");
}

}  // namespace

// One line of CLIENT LIST, in Valkey's field order for the fields Mimir keeps.
std::string client_line(const Session& s, uint64_t now) {
  std::string flags;
  if (s.subscriptions > 0 || s.shard_subscriptions > 0) flags += "P";
  if (s.in_multi) flags += "x";
  if (flags.empty()) flags = "N";
  std::ostringstream line;
  line << "id=" << s.id << " addr=" << s.address << " laddr= fd=-1 name=" << s.name << " age=" << (now - std::min(now, s.created_ms)) / 1000
       << " idle=" << (now - std::min(now, s.last_ms)) / 1000 << " flags=" << flags << " capa= db=" << s.db << " sub=" << s.subscriptions
       << " psub=0 ssub=" << s.shard_subscriptions << " multi=" << (s.in_multi ? static_cast<int64_t>(s.queued.size()) : -1) << " watch=" << s.watched.size()
       << " qbuf=0 qbuf-free=0 argv-mem=0 multi-mem=0"
       << " rbs=0 rbp=0 obl=0 oll=0 omem=0 tot-mem=0 events=r cmd=" << s.last_command << " user=" << s.user
       << " redir=-1 resp=" << (s.protocol == resp::Protocol::resp3 ? 3 : 2) << " lib-name=" << s.lib_name << " lib-ver=" << s.lib_version
       << " tot-cmds=" << s.commands;
  // Mimir's own field, shown only where it is on, so a Valkey client's line
  // reads as Valkey's.
  if (s.strict) line << " strict=1";
  line << "\n";
  return line.str();
}

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
  if (index < 0 || static_cast<size_t>(index) >= call.database.databases()) throw CommandError("ERR DB index is out of range");
  call.session.db = static_cast<size_t>(index);
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
  std::optional<std::pair<std::string, std::string>> credentials;
  for (; i < call.args.size(); ++i) {
    const bool more = i + 1 < call.args.size();
    if (iequals(call.args[i], "AUTH") && i + 2 < call.args.size()) {
      credentials.emplace(call.args[i + 1], call.args[i + 2]);
      i += 2;
    } else if (iequals(call.args[i], "SETNAME") && more) {
      name = call.args[++i];
      if (!valid_name(*name)) throw CommandError("ERR Client names cannot contain spaces, newlines or special characters.");
    } else {
      throw CommandError("ERR Syntax error in HELLO option '" + call.args[i] + "'");
    }
  }
  if (credentials) authenticate(call, credentials->first, credentials->second);
  if (!call.session.authenticated)
    throw CommandError(
        "NOAUTH HELLO must be called with the client already authenticated, otherwise the HELLO <proto> AUTH <user> <pass> option can be used to "
        "authenticate the client and select the RESP protocol version at the same time");
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

void auth(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  if (call.args.size() > 3) throw CommandError(syntax_error);
  if (call.args.size() == 3) {
    authenticate(call, call.args[1], call.args[2]);
  } else {
    const Acl::User* fallback = call.database.acl_locked().find("default");
    if (fallback && fallback->nopass && (call.session.tls || call.database.settings().auth_plaintext))
      throw CommandError("ERR AUTH <password> called without any password configured for the default user. Are you sure your configuration is correct?");
    authenticate(call, "default", call.args[1]);
  }
  call.reply.simple("OK");
}

// Called under the database's lock, as every handler is.
void mimir(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  const std::string sub = lower(call.args[1]);
  if (sub == "nodes") {
    // Every node a client may use, so one whose node has gone can find
    // another (ruling 40). No slots: every node answers every key.
    if (call.args.size() != 2) throw CommandError("ERR wrong number of arguments for 'mimir|nodes' command");
    const auto& list = call.database.settings().nodes;
    const std::vector<NodeAddress> nodes = list ? list() : std::vector<NodeAddress>{};
    call.reply.array(nodes.size());
    for (const auto& node : nodes) {
      call.reply.map(6);
      call.reply.bulk("id");
      call.reply.bulk(node.id);
      call.reply.bulk("name");
      call.reply.bulk(node.name);
      call.reply.bulk("host");
      call.reply.bulk(node.host);
      call.reply.bulk("port");
      call.reply.integer(node.port);
      call.reply.bulk("tls-port");
      call.reply.integer(node.tls_port);
      call.reply.bulk("self");
      call.reply.integer(node.self ? 1 : 0);
    }
  } else if (sub == "help") {
    if (call.args.size() != 2) throw CommandError("ERR wrong number of arguments for 'mimir|help' command");
    constexpr std::string_view lines[] = {
        "MIMIR <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
        "NODES",
        "    Return every node of the cluster a client may connect to: its id, name, host, port, TLS port, and whether it is this node.",
        "HELP",
        "    Print this help.",
    };
    call.reply.array(std::size(lines));
    for (auto line : lines) call.reply.simple(line);
  } else {
    throw CommandError("ERR unknown subcommand '" + call.args[1].substr(0, 128) + "'. Try MIMIR HELP.");
  }
}

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
    std::optional<std::string> user;
    std::vector<int64_t> ids;
    for (size_t i = 2; i < call.args.size(); ++i) {
      if (iequals(call.args[i], "USER") && i + 1 < call.args.size()) {
        user = call.args[++i];
        if (!call.database.acl_locked().find(*user)) throw CommandError("ERR No such user '" + *user + "'");
      } else if (iequals(call.args[i], "TYPE") && i + 1 < call.args.size()) {
        type = lower(call.args[++i]);
        if (*type != "normal" && *type != "pubsub" && *type != "master" && *type != "replica")
          throw CommandError("ERR Unknown client type '" + call.args[i] + "'");
      } else if (iequals(call.args[i], "ID") && i + 1 < call.args.size()) {
        while (i + 1 < call.args.size()) {
          auto id = parse_integer(call.args[++i]);
          if (!id) throw CommandError(syntax_error);
          if (*id <= 0) throw CommandError("ERR client-id should be greater than 0");
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
      if (user && session->user != *user) continue;
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
    std::optional<std::string> by_laddr;
    std::optional<std::string> by_user;
    std::optional<std::string> by_type;
    std::optional<uint64_t> max_age_s;
    bool skipme = true;
    for (size_t i = 2; i + 1 < call.args.size(); i += 2) {
      const std::string filter = lower(call.args[i]);
      const std::string& value = call.args[i + 1];
      if (filter == "user") {
        by_user = value;
        if (!call.database.acl_locked().find(*by_user)) throw CommandError("ERR No such user '" + *by_user + "'");
      } else if (filter == "id") {
        auto id = parse_integer(value);
        if (!id) throw CommandError(syntax_error);
        if (*id <= 0) throw CommandError("ERR client-id should be greater than 0");
        by_id = *id;
      } else if (filter == "addr") {
        by_addr = value;
      } else if (filter == "laddr") {
        by_laddr = value;
      } else if (filter == "type") {
        by_type = lower(value);
        if (*by_type != "normal" && *by_type != "pubsub" && *by_type != "master" && *by_type != "replica" && *by_type != "slave")
          throw CommandError("ERR Unknown client type '" + value + "'");
      } else if (filter == "maxage") {
        auto age = parse_integer(value);
        if (!age) throw CommandError(syntax_error);
        max_age_s = static_cast<uint64_t>(std::max<int64_t>(*age, 0));
      } else if (filter == "skipme") {
        if (iequals(value, "yes"))
          skipme = true;
        else if (iequals(value, "no"))
          skipme = false;
        else
          throw CommandError(syntax_error);
      } else {
        throw CommandError(syntax_error);
      }
    }
    if (call.args.size() % 2 != 0) throw CommandError(syntax_error);
    int64_t killed = 0;
    const uint64_t now = call.keys.now_ms();
    for (const auto& [id, session] : call.database.sessions_locked()) {
      if (by_id && id != *by_id) continue;
      if (by_addr && session->address != *by_addr) continue;
      // Mimir's sessions keep no local address: only an empty one matches.
      if (by_laddr && !by_laddr->empty()) continue;
      if (by_user && session->user != *by_user) continue;
      if (by_type) {
        const bool pubsub = session->subscriptions > 0 || session->shard_subscriptions > 0;
        if (*by_type != (pubsub ? "pubsub" : "normal")) continue;
      }
      // MAXAGE: only connections older than that many seconds (0 is no limit).
      if (max_age_s && *max_age_s > 0 && (now - std::min(now, session->created_ms)) / 1000 <= *max_age_s) continue;
      if (skipme && id == call.session.id) continue;
      if (session->kill) session->kill();
      ++killed;
    }
    call.reply.integer(killed);
  } else if (sub == "no-evict" || sub == "no-touch") {
    if (call.args.size() != 3 || (!iequals(call.args[2], "on") && !iequals(call.args[2], "off"))) throw CommandError(syntax_error);
    call.reply.simple("OK");
  } else if (sub == "reply") {
    if (call.args.size() != 3) wrong_arity("client|reply");
    if (iequals(call.args[2], "ON")) {
      call.session.reply_off = false;
      call.session.reply_skip = false;
      call.reply.simple("OK");
    } else if (iequals(call.args[2], "OFF")) {
      call.session.reply_off = true;
    } else if (iequals(call.args[2], "SKIP")) {
      if (!call.session.reply_off) call.session.reply_skip_next = true;
    } else {
      throw CommandError(syntax_error);
    }
  } else if (sub == "unblock") {
    if (call.args.size() != 3 && call.args.size() != 4) wrong_arity("client|unblock");
    const int64_t id = integer_argument(call.args[2]);
    bool error = false;
    if (call.args.size() == 4) {
      if (iequals(call.args[3], "ERROR"))
        error = true;
      else if (!iequals(call.args[3], "TIMEOUT"))
        throw CommandError("ERR CLIENT UNBLOCK reason should be TIMEOUT or ERROR");
    }
    call.reply.integer(call.database.unblock_locked(id, error) ? 1 : 0);
  } else if (sub == "pause") {
    if (call.args.size() != 3 && call.args.size() != 4) wrong_arity("client|pause");
    auto timeout = parse_integer(call.args[2]);
    if (!timeout) throw CommandError("ERR timeout is not an integer or out of range");
    if (*timeout < 0) throw CommandError("ERR timeout is negative");
    bool all = true;
    if (call.args.size() == 4) {
      if (iequals(call.args[3], "WRITE"))
        all = false;
      else if (!iequals(call.args[3], "ALL"))
        throw CommandError("ERR CLIENT PAUSE mode must be WRITE or ALL");
    }
    call.database.pause_locked(call.keys.now_ms() + static_cast<uint64_t>(*timeout), all);
    call.reply.simple("OK");
  } else if (sub == "unpause") {
    if (call.args.size() != 2) wrong_arity("client|unpause");
    call.database.unpause_locked();
    call.reply.simple("OK");
  } else if (sub == "capa") {
    if (call.args.size() < 3) wrong_arity("client|capa");
    call.reply.simple("OK");
  } else if (sub == "getredir") {
    if (call.args.size() != 2) wrong_arity("client|getredir");
    call.reply.integer(call.session.tracking.on ? call.session.tracking.redirect : -1);
  } else if (sub == "trackinginfo") {
    if (call.args.size() != 2) wrong_arity("client|trackinginfo");
    const auto& t = call.session.tracking;
    std::vector<std::string_view> flags;
    if (!t.on) {
      flags.push_back("off");
    } else {
      flags.push_back("on");
      if (t.bcast) flags.push_back("bcast");
      if (t.optin) flags.push_back("optin");
      if (t.optin && t.caching) flags.push_back("caching-yes");
      if (t.optout) flags.push_back("optout");
      if (t.optout && t.caching) flags.push_back("caching-no");
      if (t.noloop) flags.push_back("noloop");
      if (t.broken) flags.push_back("broken_redirect");
    }
    call.reply.map(3);
    call.reply.bulk("flags");
    call.reply.set(flags.size());
    for (auto flag : flags) call.reply.bulk(flag);
    call.reply.bulk("redirect");
    call.reply.integer(t.on ? t.redirect : -1);
    call.reply.bulk("prefixes");
    call.reply.array(t.prefixes.size());
    for (const auto& prefix : t.prefixes) call.reply.bulk(prefix);
  } else if (sub == "tracking") {
    if (call.args.size() < 3) wrong_arity("client|tracking");
    tracking(call);
  } else if (sub == "caching") {
    if (call.args.size() != 3) wrong_arity("client|caching");
    auto& t = call.session.tracking;
    if (!t.on) throw CommandError("ERR CLIENT CACHING can be called only when the client is in tracking mode with OPTIN or OPTOUT mode enabled");
    if (iequals(call.args[2], "YES")) {
      if (!t.optin) throw CommandError("ERR CLIENT CACHING YES is only valid when tracking is enabled in OPTIN mode.");
    } else if (iequals(call.args[2], "NO")) {
      if (!t.optout) throw CommandError("ERR CLIENT CACHING NO is only valid when tracking is enabled in OPTOUT mode.");
    } else {
      throw CommandError(syntax_error);
    }
    t.caching = true;
    call.reply.simple("OK");
  } else if (sub == "strict") {
    // Mimir's (ruling 37): reads confirmed with a majority, or not.
    if (call.args.size() != 3) wrong_arity("client|strict");
    if (iequals(call.args[2], "ON"))
      call.session.strict = true;
    else if (iequals(call.args[2], "OFF"))
      call.session.strict = false;
    else
      throw CommandError(syntax_error);
    call.reply.simple("OK");
  } else if (sub == "import-source") {
    if (call.args.size() != 3) wrong_arity("client|import-source");
    throw CommandError("ERR Server is not in import mode");
  } else if (help::reply_help(call, "client")) {
    return;
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
    if (call.args.size() == 2) {
      auto specs = all_specs();
      call.reply.array(specs.size());
      for (const Spec& spec : specs) call.reply.bulk(spec.name);
      return;
    }
    // FILTERBY names subcommands too, by full name, as Valkey's does; Mimir
    // loads no modules.
    if (call.args.size() != 5 || !iequals(call.args[2], "FILTERBY")) throw CommandError(syntax_error);
    const std::string filter = lower(call.args[3]);
    std::vector<std::string> names;
    if (filter == "aclcat") {
      names = Acl::commands_in(call.args[4]).value_or(std::vector<std::string>{});
    } else if (filter == "pattern") {
      for (auto& name : Acl::command_names())
        if (keyspace::glob_match(call.args[4], name, true)) names.push_back(std::move(name));
    } else if (filter != "module") {
      throw CommandError(syntax_error);
    }
    call.reply.array(names.size());
    for (const auto& name : names) call.reply.bulk(name);
  } else if (sub == "getkeys" || sub == "getkeysandflags") {
    if (call.args.size() < 3) wrong_arity("command|" + sub);
    const std::vector<std::string> args(call.args.begin() + 2, call.args.end());
    const Spec* spec = find_spec(args[0]);
    if (!spec || !Acl::known(args)) throw CommandError("ERR Invalid command specified");
    if (!Acl::has_keys(args)) throw CommandError("ERR The command has no key arguments");
    const int count = static_cast<int>(args.size());
    if ((spec->arity > 0 && count != spec->arity) || (spec->arity < 0 && count < -spec->arity))
      throw CommandError("ERR Invalid number of arguments specified for command");
    const auto keys = Acl::keys_with_flags(args);
    if (keys.empty() && spec->flags.find("no_mandatory_keys") == std::string_view::npos) throw CommandError("ERR Invalid arguments specified for command");
    call.reply.array(keys.size());
    for (const auto& [position, flags] : keys) {
      if (sub == "getkeys") {
        call.reply.bulk(args[position]);
        continue;
      }
      call.reply.array(2);
      call.reply.bulk(args[position]);
      std::vector<std::string_view> words;
      std::string_view rest = flags;
      while (!rest.empty()) {
        const size_t space = rest.find(' ');
        words.push_back(rest.substr(0, space));
        rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
      }
      call.reply.set(words.size());
      for (auto word : words) call.reply.simple(word);
    }
  } else if (sub == "docs") {
    // Valkey's documentation is its own text, which Mimir does not copy: no
    // command has documentation here.
    call.reply.map(0);
  } else if (help::reply_help(call, "command")) {
    return;
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
    for (size_t i = 0; i < call.database.databases(); ++i) {
      auto& keys = call.database.db_locked(i);
      if (keys.size() > 0) text << "db" << i << ":keys=" << keys.size() << ",expires=" << keys.expiring() << ",avg_ttl=0\r\n";
    }
  }
  call.reply.verbatim("txt", text.str());
}

void time(Call& call) {
  const uint64_t now = call.keys.now_ms();
  call.reply.array(2);
  call.reply.bulk(std::to_string(now / 1000));
  call.reply.bulk(std::to_string((now % 1000) * 1000));
}

void reset(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  // The connection is the default user again, authenticated only if that
  // user needs no password, and tracks nothing.
  call.database.welcome_locked(call.session);
  call.database.track_off_locked(call.session);
  call.database.pubsub().drop(call.session.id);
  call.session.subscriptions = 0;
  call.session.shard_subscriptions = 0;
  call.session.protocol = resp::Protocol::resp2;
  call.session.db = 0;
  call.session.name.clear();
  call.reply = resp::Reply(resp::Protocol::resp2);
  call.reply.simple("RESET");
}

}  // namespace mimir::command::connection
