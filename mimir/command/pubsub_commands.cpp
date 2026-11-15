//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The pub/sub commands, as Valkey 8 documents them. A subscription's
// confirmation goes out through the connection's push, under the hub's lock,
// so nothing published on the channel reaches the client before it.

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"

namespace mimir::command::pubsub {

namespace {

using Kind = PubSub::Kind;

size_t& counter(Session& session, Kind kind) { return kind == Kind::shard ? session.shard_subscriptions : session.subscriptions; }

void subscribe_generic(Call& call, Kind kind, std::string_view verb) {
  if (!call.session.push) throw CommandError("ERR this connection cannot subscribe");
  for (size_t i = 1; i < call.args.size(); ++i) {
    call.database.pubsub().subscribe(kind, call.session.id, call.args[i], call.session.push, call.session.protocol, verb, counter(call.session, kind));
  }
}

void unsubscribe_generic(Call& call, Kind kind, std::string_view verb) {
  if (!call.session.push) throw CommandError("ERR this connection cannot subscribe");
  auto& hub = call.database.pubsub();
  std::vector<std::string> names(call.args.begin() + 1, call.args.end());
  if (names.empty()) names = hub.names_of(kind, call.session.id);
  if (names.empty()) {
    hub.unsubscribe(kind, call.session.id, std::nullopt, call.session.push, call.session.protocol, verb, counter(call.session, kind));
    return;
  }
  for (const auto& name : names) {
    hub.unsubscribe(kind, call.session.id, name, call.session.push, call.session.protocol, verb, counter(call.session, kind));
  }
}

}  // namespace

void subscribe(Call& call) { subscribe_generic(call, Kind::channel, "subscribe"); }
void psubscribe(Call& call) { subscribe_generic(call, Kind::pattern, "psubscribe"); }
void ssubscribe(Call& call) { subscribe_generic(call, Kind::shard, "ssubscribe"); }
void unsubscribe(Call& call) { unsubscribe_generic(call, Kind::channel, "unsubscribe"); }
void punsubscribe(Call& call) { unsubscribe_generic(call, Kind::pattern, "punsubscribe"); }
void sunsubscribe(Call& call) { unsubscribe_generic(call, Kind::shard, "sunsubscribe"); }

void publish(Call& call) { call.reply.integer(static_cast<int64_t>(call.database.pubsub().publish(call.args[1], call.args[2]))); }
void spublish(Call& call) { call.reply.integer(static_cast<int64_t>(call.database.pubsub().spublish(call.args[1], call.args[2]))); }

void pubsub(Call& call) {
  auto& hub = call.database.pubsub();
  const std::string sub = lower(call.args[1]);
  const auto names_reply = [&](Kind kind) {
    if (call.args.size() > 3) throw CommandError("ERR wrong number of arguments for 'pubsub|" + sub + "' command");
    std::optional<std::string> pattern;
    if (call.args.size() == 3) pattern = call.args[2];
    auto names = hub.channels(kind, pattern);
    call.reply.array(names.size());
    for (const auto& name : names) call.reply.bulk(name);
  };
  const auto counts_reply = [&](Kind kind) {
    call.reply.map(call.args.size() - 2);
    for (size_t i = 2; i < call.args.size(); ++i) {
      call.reply.bulk(call.args[i]);
      call.reply.integer(static_cast<int64_t>(hub.subscribers(kind, call.args[i])));
    }
  };
  if (sub == "channels") {
    names_reply(Kind::channel);
  } else if (sub == "shardchannels") {
    names_reply(Kind::shard);
  } else if (sub == "numsub") {
    counts_reply(Kind::channel);
  } else if (sub == "shardnumsub") {
    counts_reply(Kind::shard);
  } else if (sub == "numpat") {
    if (call.args.size() != 2) throw CommandError("ERR wrong number of arguments for 'pubsub|numpat' command");
    call.reply.integer(static_cast<int64_t>(hub.patterns()));
  } else {
    throw CommandError("ERR unknown subcommand '" + call.args[1] + "'. Try PUBSUB HELP.");
  }
}

}  // namespace mimir::command::pubsub
