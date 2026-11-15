//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/command/database.h"

#include <algorithm>

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

bool spec_allowed_subscribed(std::string_view name) {
  for (std::string_view allowed : {"subscribe", "ssubscribe", "psubscribe", "unsubscribe", "sunsubscribe", "punsubscribe", "ping", "quit", "reset"}) {
    if (name == allowed) return true;
  }
  return false;
}

}  // namespace

Database::Database(const dannan::contract::WallClock& wall, DatabaseSettings settings)
    : _wall(wall), _settings(std::move(settings)), _keys(wall), _random(std::random_device{}()) {}

std::string Database::timeout_reply(resp::Protocol protocol, bool null_bulk) {
  resp::Reply reply(protocol);
  if (null_bulk)
    reply.null();
  else
    reply.null_array();
  return reply.take();
}

Executed Database::execute(Session& session, const std::vector<std::string>& args, std::string& out, Wake wake) {
  Executed executed;
  resp::Reply reply(session.protocol);
  if (args.empty()) return executed;
  const Spec* spec = find_spec(args[0]);
  if (!spec) {
    reply.error(unknown_command(args));
    out.append(reply.bytes());
    return executed;
  }
  const int count = static_cast<int>(args.size());
  if ((spec->arity > 0 && count != spec->arity) || (spec->arity < 0 && count < -spec->arity)) {
    reply.error("ERR wrong number of arguments for '" + std::string(spec->name) + "' command");
    out.append(reply.bytes());
    return executed;
  }
  // A RESP2 connection with subscriptions takes only the pub/sub commands,
  // PING, QUIT and RESET, as Valkey's subscribed mode does; RESP3 takes all.
  if (session.protocol == resp::Protocol::resp2 && (session.subscriptions > 0 || session.shard_subscriptions > 0) && !spec_allowed_subscribed(spec->name)) {
    reply.error("ERR Can't execute '" + std::string(spec->name) +
                "': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context");
    out.append(reply.bytes());
    return executed;
  }
  Woken woken;
  {
    dannan::contract::Lock lock(_mutex);
    Call call{session, *this, _keys, reply, args};
    try {
      spec->handler(call);
    } catch (const CommandError& e) {
      // A handler refuses before it changes anything; what it wrote is dropped.
      call.blocked = false;
      reply = resp::Reply(session.protocol);
      reply.error(e.what());
    }
    if (call.blocked) {
      if (wake) {
        const uint64_t id = _next_wait++;
        _waiters.emplace(id, Waiter{&session, args, call.block_keys, call.block_null_bulk, std::move(wake)});
        for (const auto& key : call.block_keys) _waiting_on[key].push_back(id);
        executed.wait = id;
        executed.wait_seconds = call.block_seconds;
        return executed;
      }
      // Nowhere to wait: the answer is the timeout's, at once.
      out.append(timeout_reply(session.protocol, call.block_null_bulk));
    } else {
      out.append(reply.bytes());
    }
    if (!_waiters.empty()) serve_ready(woken);
  }
  for (auto& [wake_one, bytes] : woken) wake_one(std::move(bytes));
  return executed;
}

void Database::forget_waiter(uint64_t id) {
  auto found = _waiters.find(id);
  if (found == _waiters.end()) return;
  for (const auto& key : found->second.keys) {
    auto queue = _waiting_on.find(key);
    if (queue == _waiting_on.end()) continue;
    auto& ids = queue->second;
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    if (ids.empty()) _waiting_on.erase(queue);
  }
  _waiters.erase(found);
}

void Database::serve_ready(Woken& woken) {
  // A pass may make other keys ready (BLMOVE's destination), so passes repeat
  // until one serves nothing; each pass serves at least one wait or stops, so
  // the passes are bounded by the waits parked.
  for (bool progress = true; progress;) {
    progress = false;
    std::vector<std::string> ready;
    for (const auto& [key, ids] : _waiting_on)
      if (_keys.contains(key)) ready.push_back(key);
    for (const std::string& key : ready) {
      for (;;) {
        auto queue = _waiting_on.find(key);
        if (queue == _waiting_on.end() || queue->second.empty() || !_keys.contains(key)) break;
        const uint64_t id = queue->second.front();
        Waiter& waiter = _waiters.at(id);
        const Spec* spec = find_spec(waiter.args[0]);
        resp::Reply reply(waiter.session->protocol);
        Call call{*waiter.session, *this, _keys, reply, waiter.args};
        call.ready_key = &key;
        try {
          spec->handler(call);
        } catch (const CommandError& e) {
          // The key was ready but the command cannot complete (a destination of
          // another type): it ends with its error, as Valkey ends it.
          call.blocked = false;
          reply = resp::Reply(waiter.session->protocol);
          reply.error(e.what());
        }
        // Not served from this key (another type now): the next key's turn.
        if (call.blocked) break;
        woken.emplace_back(std::move(waiter.wake), reply.take());
        forget_waiter(id);
        progress = true;
      }
    }
  }
}

std::optional<std::string> Database::cancel_wait(uint64_t id) {
  dannan::contract::Lock lock(_mutex);
  auto found = _waiters.find(id);
  if (found == _waiters.end()) return std::nullopt;
  std::string bytes = timeout_reply(found->second.session->protocol, found->second.null_bulk);
  forget_waiter(id);
  return bytes;
}

size_t Database::waiting() {
  dannan::contract::Lock lock(_mutex);
  return _waiters.size();
}

size_t Database::expire_step(size_t budget) {
  dannan::contract::Lock lock(_mutex);
  return _keys.expire_step(budget);
}

}  // namespace mimir::command
