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
    : _wall(wall), _settings(std::move(settings)), _started_ms(wall.unix_ms()), _keys(wall), _random(std::random_device{}()) {
  // What CONFIG GET answers, in Valkey's names: one database, no persistence
  // of Valkey's kind, nothing evicted (ruling 9's eviction comes later).
  _config = {{"databases", "1"},
             {"maxmemory", "0"},
             {"maxmemory-policy", "noeviction"},
             {"notify-keyspace-events", ""},
             {"timeout", "0"},
             {"hz", "10"},
             {"save", ""},
             {"appendonly", "no"},
             {"proto-max-bulk-len", "536870912"},
             {"cluster-enabled", "no"},
             {"maxclients", "10000"},
             {"set-max-intset-entries", "512"},
             {"set-max-listpack-entries", "128"},
             {"set-max-listpack-value", "64"},
             {"hash-max-listpack-entries", "128"},
             {"hash-max-listpack-value", "64"},
             {"zset-max-listpack-entries", "128"},
             {"zset-max-listpack-value", "64"},
             {"list-max-listpack-size", "-2"},
             {"hll-sparse-max-bytes", "3000"},
             {"stream-node-max-entries", "100"},
             {"stream-node-max-bytes", "4096"},
             {"lua-time-limit", "5000"},
             {"busy-reply-threshold", "5000"},
             {"slowlog-log-slower-than", "10000"},
             {"slowlog-max-len", "128"}};
}

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
  // A command refused before it runs fails a transaction being queued.
  if (!spec) {
    session.multi_failed = session.multi_failed || session.in_multi;
    reply.error(unknown_command(args));
    out.append(reply.bytes());
    return executed;
  }
  const int count = static_cast<int>(args.size());
  if ((spec->arity > 0 && count != spec->arity) || (spec->arity < 0 && count < -spec->arity)) {
    session.multi_failed = session.multi_failed || session.in_multi;
    reply.error("ERR wrong number of arguments for '" + std::string(spec->name) + "' command");
    out.append(reply.bytes());
    return executed;
  }
  // Inside a transaction MULTI and WATCH are refused, and the refusal aborts
  // the EXEC, as Valkey 8 has it (the differential gate's finding).
  if (session.in_multi && (spec->name == "multi" || spec->name == "watch")) {
    session.multi_failed = true;
    reply.error("ERR Command not allowed inside a transaction");
    out.append(reply.bytes());
    return executed;
  }
  // Between MULTI and EXEC a command is queued, but for those that steer the
  // transaction itself.
  if (session.in_multi && spec->name != "exec" && spec->name != "discard" && spec->name != "quit" && spec->name != "reset") {
    session.queued.push_back(args);
    reply.simple("QUEUED");
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
    _touched.clear();
    _sessions.emplace(session.id, &session);
    session.last_ms = _wall.unix_ms();
    session.last_command = std::string(spec->name);
    ++session.commands;
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
        _waiters.emplace(id, Waiter{&session, call.block_args ? *call.block_args : args, call.block_keys, call.block_null_bulk, std::move(wake)});
        for (const auto& key : call.block_keys) _waiting_on[key].push_back(id);
        executed.wait = id;
        executed.wait_seconds = call.block_seconds;
        return executed;
      }
      // Nowhere to wait: the answer is the timeout's, at once.
      out.append(timeout_reply(session.protocol, call.block_null_bulk));
    } else {
      out.append(reply.bytes());
      touch(*spec, args);
    }
    if (!_waiters.empty()) serve_ready(woken);
  }
  for (auto& [wake_one, bytes] : woken) wake_one(std::move(bytes));
  return executed;
}

void Database::run_locked(Session& session, const std::vector<std::string>& args, resp::Reply& out) {
  // Queued commands were found and checked when queued.
  const Spec* spec = find_spec(args[0]);
  resp::Reply reply(session.protocol);
  Call call{session, *this, _keys, reply, args};
  try {
    spec->handler(call);
  } catch (const CommandError& e) {
    call.blocked = false;
    reply = resp::Reply(session.protocol);
    reply.error(e.what());
  }
  // Inside a transaction a blocking command does not wait.
  if (call.blocked) {
    out.raw(timeout_reply(session.protocol, call.block_null_bulk));
    return;
  }
  out.raw(reply.bytes());
  touch(*spec, args);
}

void Database::touch(const Spec& spec, const std::vector<std::string>& args) {
  if (spec.flags.find("write") == std::string_view::npos) return;
  if (spec.name == "flushall" || spec.name == "flushdb") {
    for (auto& [key, sessions] : _watchers)
      for (Session* s : sessions) s->watch_touched = true;
    return;
  }
  for (auto key : keys_of(spec, args)) {
    if (!_waiting_on.empty()) _touched.emplace_back(key);
    auto found = _watchers.find(std::string(key));
    if (found == _watchers.end()) continue;
    for (Session* s : found->second) s->watch_touched = true;
  }
}

void Database::watch_locked(Session& session, const std::string& key) {
  for (const auto& [watched, existed] : session.watched)
    if (watched == key) return;
  session.watched.emplace_back(key, _keys.contains(key));
  _watchers[key].push_back(&session);
}

void Database::unwatch_locked(Session& session) {
  for (const auto& [key, existed] : session.watched) {
    auto found = _watchers.find(key);
    if (found == _watchers.end()) continue;
    auto& sessions = found->second;
    sessions.erase(std::remove(sessions.begin(), sessions.end(), &session), sessions.end());
    if (sessions.empty()) _watchers.erase(found);
  }
  session.watched.clear();
  session.watch_touched = false;
}

bool Database::watch_broken_locked(Session& session) {
  if (session.watch_touched) return true;
  // A key that held a value when watched and is gone with no write naming it
  // has expired meanwhile: that too breaks the watch, as Valkey has it since
  // 6.0.9.
  for (const auto& [key, existed] : session.watched)
    if (existed && !_keys.contains(key)) return true;
  return false;
}

void Database::forget(Session& session) {
  dannan::contract::Lock lock(_mutex);
  unwatch_locked(session);
  _sessions.erase(session.id);
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
  // Only the keys a write named can have become ready, as Valkey signals only
  // those. Serving a wait may name more (BLMOVE's destination), so rounds
  // repeat until one names nothing; each serves at least one wait to name
  // anything, so the rounds are bounded by the waits parked.
  while (!_touched.empty()) {
    std::vector<std::string> ready = std::move(_touched);
    _touched.clear();
    std::sort(ready.begin(), ready.end());
    ready.erase(std::unique(ready.begin(), ready.end()), ready.end());
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
        const std::vector<std::string> served_args = waiter.args;
        forget_waiter(id);
        touch(*spec, served_args);
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
