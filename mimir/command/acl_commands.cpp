//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// ACL and its subcommands, as Valkey 8 documents them. Users are cluster
// state (ruling 27): at the single-node stage SETUSER and DELUSER change this
// node's memory; in the cluster they commit by agreement. There is no ACL
// file, so LOAD and SAVE answer Valkey's refusal for an instance without one.

#include <algorithm>

#include "dannan/identity/crypto.h"
#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"
#include "mimir/keyspace/glob.h"

namespace mimir::command::acl {

namespace {

constexpr std::string_view help_lines[] = {
    "ACL <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
    "CAT [<category>]",
    "    List all commands that belong to <category>, or all command categories",
    "    when no category is specified.",
    "DELUSER <username> [<username> ...]",
    "    Delete a list of users.",
    "DRYRUN <username> <command> [<arg> ...]",
    "    Returns whether the user can execute the given command without executing the command.",
    "GETUSER <username>",
    "    Get the user's details.",
    "GENPASS [<bits>]",
    "    Generate a secure 256-bit user password. The optional `bits` argument can",
    "    be used to specify a different size.",
    "LIST",
    "    Show users details in config file format.",
    "LOAD",
    "    Reload users from the ACL file.",
    "LOG [<count> | RESET]",
    "    Show the ACL log entries.",
    "SAVE",
    "    Save the current config to the ACL file.",
    "SETUSER <username> <attribute> [<attribute> ...]",
    "    Create or modify a user with the specified attributes.",
    "USERS",
    "    List all the registered usernames.",
    "WHOAMI",
    "    Return the current connection username.",
    "HELP",
    "    Print this help.",
};

void close_session(Call& call, Session& session) {
  if (&session == &call.session)
    call.session.closing = true;
  else if (session.kill)
    session.kill();
}

void close_refused_subscribers(Call& call, const Acl::User& before, const Acl::User& after) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  for (Session* session : refused_subscribers(call.database, call.database.sessions_locked(), before, after)) close_session(call, *session);
}

}  // namespace

std::vector<Session*> refused_subscribers(Database& database, const std::map<int64_t, Session*>& sessions, const Acl::User& before, const Acl::User& after) {
  std::vector<Session*> refused;
  for (const auto& s : after.selectors)
    if (s.all_channels) return refused;
  std::vector<std::string> upcoming;
  for (const auto& s : after.selectors) upcoming.insert(upcoming.end(), s.channels.begin(), s.channels.end());
  bool kept = true;
  for (const auto& s : before.selectors) {
    if (s.all_channels) kept = false;
    for (const auto& c : s.channels)
      if (std::find(upcoming.begin(), upcoming.end(), c) == upcoming.end()) kept = false;
  }
  if (kept) return refused;
  const auto allowed = [&](const std::string& name, bool pattern) {
    return std::any_of(upcoming.begin(), upcoming.end(), [&](const std::string& p) { return pattern ? p == name : keyspace::glob_match(p, name); });
  };
  auto& pubsub = database.pubsub();
  for (const auto& [id, session] : sessions) {
    if (session->user != after.name || (session->subscriptions == 0 && session->shard_subscriptions == 0)) continue;
    bool close = false;
    for (const auto& name : pubsub.names_of(PubSub::Kind::pattern, id)) close = close || !allowed(name, true);
    for (const auto& name : pubsub.names_of(PubSub::Kind::channel, id)) close = close || !allowed(name, false);
    for (const auto& name : pubsub.names_of(PubSub::Kind::shard, id)) close = close || !allowed(name, false);
    if (close) refused.push_back(session);
  }
  return refused;
}

namespace {

void reply_selector(resp::Reply& reply, const Acl::Selector& s) {
  reply.bulk("commands");
  reply.bulk(Acl::describe_commands(s));
  reply.bulk("keys");
  reply.bulk(Acl::describe_keys(s));
  reply.bulk("channels");
  reply.bulk(Acl::describe_channels(s));
}

void wrong_arity(std::string_view sub) { throw CommandError("ERR wrong number of arguments for 'acl|" + std::string(sub) + "' command"); }

void syntax(const Call& call) {
  throw CommandError("ERR unknown subcommand or wrong number of arguments for '" + call.args[1].substr(0, 128) + "'. Try ACL HELP.");
}

}  // namespace

// Called under the database's lock, as every handler is.
void acl(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  const std::string sub = lower(call.args[1]);
  const size_t argc = call.args.size();
  Acl& users = call.database.acl_locked();
  if (sub == "setuser") {
    if (argc < 3) wrong_arity(sub);
    const Acl::User* existing = users.find(call.args[2]);
    std::optional<Acl::User> before;
    if (existing) before = *existing;
    if (auto error = users.set_user(call.args[2], std::span<const std::string>(call.args).subspan(3))) throw CommandError(*error);
    if (before) close_refused_subscribers(call, *before, *users.find(call.args[2]));
    call.reply.simple("OK");
  } else if (sub == "deluser") {
    if (argc < 3) wrong_arity(sub);
    for (size_t i = 2; i < argc; ++i)
      if (call.args[i] == "default") throw CommandError("ERR The 'default' user cannot be removed");
    int64_t deleted = 0;
    for (size_t i = 2; i < argc; ++i) {
      if (!users.delete_user(call.args[i])) continue;
      ++deleted;
      std::vector<Session*> theirs;
      for (const auto& [id, session] : call.database.sessions_locked())
        if (session->user == call.args[i]) theirs.push_back(session);
      for (Session* session : theirs) close_session(call, *session);
    }
    call.reply.integer(deleted);
  } else if (sub == "getuser") {
    if (argc != 3) wrong_arity(sub);
    const Acl::User* user = users.find(call.args[2]);
    if (!user) {
      call.reply.null();
      return;
    }
    call.reply.map(6);
    call.reply.bulk("flags");
    const auto flags = Acl::flags_of(*user);
    call.reply.set(flags.size());
    for (const auto& flag : flags) call.reply.bulk(flag);
    call.reply.bulk("passwords");
    call.reply.array(user->passwords.size());
    for (const auto& hash : user->passwords) call.reply.bulk(hash);
    reply_selector(call.reply, user->selectors.front());
    call.reply.bulk("selectors");
    call.reply.array(user->selectors.size() - 1);
    for (size_t i = 1; i < user->selectors.size(); ++i) {
      call.reply.map(3);
      reply_selector(call.reply, user->selectors[i]);
    }
  } else if (sub == "list") {
    if (argc != 2) wrong_arity(sub);
    call.reply.array(users.users().size());
    for (const auto& [name, user] : users.users()) call.reply.bulk("user " + name + " " + Acl::describe(user));
  } else if (sub == "users") {
    if (argc != 2) wrong_arity(sub);
    call.reply.array(users.users().size());
    for (const auto& [name, user] : users.users()) call.reply.bulk(name);
  } else if (sub == "whoami") {
    if (argc != 2) wrong_arity(sub);
    call.reply.bulk(call.session.user);
  } else if (sub == "cat") {
    if (argc == 2) {
      auto names = Acl::categories();
      call.reply.array(names.size());
      for (auto name : names) call.reply.bulk(name);
    } else if (argc == 3) {
      auto commands = Acl::commands_in(call.args[2]);
      if (!commands) throw CommandError("ERR Unknown category '" + call.args[2].substr(0, 128) + "'");
      call.reply.array(commands->size());
      for (const auto& name : *commands) call.reply.bulk(name);
    } else {
      syntax(call);
    }
  } else if (sub == "dryrun") {
    if (argc < 4) wrong_arity(sub);
    const Acl::User* user = users.find(call.args[2]);
    if (!user) throw CommandError("ERR User '" + call.args[2].substr(0, 128) + "' not found");
    const std::vector<std::string> args(call.args.begin() + 3, call.args.end());
    const Spec* spec = find_spec(args[0]);
    if (!spec || !Acl::known(args)) throw CommandError("ERR Command '" + args[0].substr(0, 128) + "' not found");
    const int count = static_cast<int>(args.size());
    if ((spec->arity > 0 && count != spec->arity) || (spec->arity < 0 && count < -spec->arity))
      throw CommandError("ERR wrong number of arguments for '" + Acl::full_name(args) + "' command");
    const AclVerdict verdict = Acl::check(*user, args);
    if (verdict.denial == Denial::none)
      call.reply.simple("OK");
    else
      call.reply.bulk(Acl::message(verdict, user->name, args, true));
  } else if (sub == "genpass") {
    int64_t bits = 256;
    if (argc == 3) {
      auto given = parse_integer(call.args[2]);
      if (!given) throw CommandError(not_integer);
      bits = *given;
      if (bits <= 0 || bits > 4096)
        throw CommandError("ERR ACL GENPASS argument must be the number of bits for the output password, a positive number up to 4096");
    } else if (argc > 3) {
      syntax(call);
    }
    // Four bits a hex digit, rounded up.
    const size_t chars = static_cast<size_t>((bits + 3) / 4);
    std::string text = dannan::identity::hex(dannan::identity::random_bytes((chars + 1) / 2));
    text.resize(chars);
    call.reply.bulk(text);
  } else if (sub == "log") {
    if (argc > 3) syntax(call);
    int64_t count = 10;
    if (argc == 3) {
      if (iequals(call.args[2], "RESET")) {
        users.reset_log();
        call.reply.simple("OK");
        return;
      }
      auto given = parse_integer(call.args[2]);
      if (!given) throw CommandError(not_integer);
      count = std::max<int64_t>(*given, 0);
    }
    const auto& entries = users.log_entries();
    const size_t shown = std::min(entries.size(), static_cast<size_t>(count));
    const uint64_t now = call.keys.now_ms();
    call.reply.array(shown);
    for (size_t i = 0; i < shown; ++i) {
      const auto& e = entries[i];
      call.reply.map(10);
      call.reply.bulk("count");
      call.reply.integer(static_cast<int64_t>(e.count));
      call.reply.bulk("reason");
      call.reply.bulk(e.reason);
      call.reply.bulk("context");
      call.reply.bulk(e.context);
      call.reply.bulk("object");
      call.reply.bulk(e.object);
      call.reply.bulk("username");
      call.reply.bulk(e.username);
      call.reply.bulk("age-seconds");
      call.reply.real(static_cast<double>(now - std::min(now, e.updated_ms)) / 1000.0);
      call.reply.bulk("client-info");
      call.reply.bulk(e.client_info);
      call.reply.bulk("entry-id");
      call.reply.integer(static_cast<int64_t>(e.id));
      call.reply.bulk("timestamp-created");
      call.reply.integer(static_cast<int64_t>(e.created_ms));
      call.reply.bulk("timestamp-last-updated");
      call.reply.integer(static_cast<int64_t>(e.updated_ms));
    }
  } else if (sub == "load" || sub == "save") {
    if (argc != 2) wrong_arity(sub);
    throw CommandError(
        "ERR This instance is not configured to use an ACL file. You may want to specify users via the ACL SETUSER command and then issue a CONFIG "
        "REWRITE (assuming you have a configuration file set) in order to store users in the configuration.");
  } else if (sub == "help") {
    if (argc != 2) wrong_arity(sub);
    call.reply.array(std::size(help_lines));
    for (auto line : help_lines) call.reply.simple(line);
  } else {
    throw CommandError("ERR unknown subcommand '" + call.args[1].substr(0, 128) + "'. Try ACL HELP.");
  }
}

}  // namespace mimir::command::acl
