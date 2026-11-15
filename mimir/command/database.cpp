//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/command/database.h"

#include <algorithm>
#include <optional>

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

// notify-keyspace-events' classes as bits, in Valkey's letters.
constexpr std::string_view notify_letters = "g$lshzxetdnmKE";
constexpr uint32_t notify_bit(char letter) {
  for (size_t i = 0; i < notify_letters.size(); ++i) {
    if (notify_letters[i] == letter) return 1u << i;
  }
  return 0;
}
// 'A': every class of event but "new" and "keymiss".
constexpr uint32_t notify_all = (1u << 10) - 1;
constexpr uint32_t notify_keyspace = notify_bit('K');
constexpr uint32_t notify_keyevent = notify_bit('E');

std::optional<uint32_t> parse_notify_flags(std::string_view text) {
  uint32_t flags = 0;
  for (char c : text) {
    if (c == 'A') {
      flags |= notify_all;
    } else if (const uint32_t bit = notify_bit(c)) {
      flags |= bit;
    } else {
      return std::nullopt;
    }
  }
  return flags;
}

// CONFIG GET's answer: 'A' or the classes in Valkey's order, then K, E, m.
std::string notify_flags_text(uint32_t flags) {
  std::string out;
  if ((flags & notify_all) == notify_all) {
    out = "A";
  } else {
    for (char c : std::string_view("g$lshzxetdn")) {
      if (flags & notify_bit(c)) out += c;
    }
  }
  for (char c : std::string_view("KEm")) {
    if (flags & notify_bit(c)) out += c;
  }
  return out;
}

bool reads_its_keys(const Spec& spec) {
  // A script's keys are the script's to read, not the command's.
  if (spec.flags.find("noscript") != std::string_view::npos) return false;
  if (spec.name == "getdel" || spec.name == "getex") return true;
  return spec.flags.find("readonly") != std::string_view::npos;
}

bool spec_allowed_subscribed(std::string_view name) {
  for (std::string_view allowed : {"subscribe", "ssubscribe", "psubscribe", "unsubscribe", "sunsubscribe", "punsubscribe", "ping", "quit", "reset"}) {
    if (name == allowed) return true;
  }
  return false;
}

}  // namespace

Database::Database(const dannan::contract::WallClock& wall, DatabaseSettings settings)
    : _wall(wall),
      _settings(std::move(settings)),
      _started_ms(wall.unix_ms()),
      _random(std::random_device{}()),
      _scripting(ScriptingNames{_settings.server_name, _settings.compatible_version, _settings.redis_version}) {
  const size_t count = std::max<size_t>(1, _settings.databases);
  for (size_t i = 0; i < count; ++i) _dbs.push_back(std::make_unique<keyspace::Keyspace>(wall));
  // What CONFIG GET answers, in Valkey's names: no persistence of Valkey's
  // kind, nothing evicted (ruling 9's eviction comes later).
  _config = {{"databases", std::to_string(count)},
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
  dannan::contract::Lock lock(_mutex);
  bind_listeners();
}

void Call::notify(char cls, std::string_view event, std::string_view key) DANNAN_NO_THREAD_SAFETY_ANALYSIS { database.notify_locked(db, cls, event, key); }

void Database::bind_listeners() {
  for (size_t i = 0; i < _dbs.size(); ++i) {
    keyspace::Keyspace::Listener listener;
    listener.expired = [this, i](std::string_view key) DANNAN_NO_THREAD_SAFETY_ANALYSIS { notify_locked(i, 'x', "expired", key); };
    listener.created = [this, i](std::string_view key) DANNAN_NO_THREAD_SAFETY_ANALYSIS { notify_locked(i, 'n', "new", key); };
    _dbs[i]->set_listener(std::move(listener));
  }
}

bool Database::valid_notify_flags(std::string_view flags) { return parse_notify_flags(flags).has_value(); }

bool Database::set_notify_flags_locked(std::string_view text) {
  auto flags = parse_notify_flags(text);
  if (!flags) return false;
  _notify_flags = *flags;
  _config["notify-keyspace-events"] = notify_flags_text(*flags);
  return true;
}

void Database::notify_locked(size_t db, char cls, std::string_view event, std::string_view key) {
  if (!(_notify_flags & (notify_keyspace | notify_keyevent))) return;
  if (!(_notify_flags & notify_bit(cls))) return;
  _events.push_back(Event{db, std::string(event), std::string(key)});
}

void Database::look_up_keys(const Spec& spec, const std::vector<std::string>& args, size_t db) {
  if (!(_notify_flags & (notify_keyspace | notify_keyevent)) || !reads_its_keys(spec)) return;
  for (auto key : keys_of(spec, args)) {
    if (!_dbs[db]->find(key)) notify_locked(db, 'm', "keymiss", key);
  }
}

void Database::publish_events() {
  if (_events.empty()) return;
  // Taken first: a subscriber's push may not reach back here, but a
  // publication made now must not see events queued during it.
  std::vector<Event> events = std::move(_events);
  _events.clear();
  for (const auto& e : events) {
    const std::string db = std::to_string(e.db);
    if (_notify_flags & notify_keyspace) _pubsub.publish("__keyspace@" + db + "__:" + e.key, e.event);
    if (_notify_flags & notify_keyevent) _pubsub.publish("__keyevent@" + db + "__:" + e.event, e.key);
  }
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
    // The database the command ran in, before it may change it (SELECT).
    const size_t db = session.db;
    Call call{session, *this, *_dbs[db], reply, args};
    call.db = db;
    look_up_keys(*spec, args, db);
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
        std::vector<std::string> keys;
        for (const auto& key : call.block_keys) keys.push_back(scoped(db, key));
        for (const auto& key : keys) _waiting_on[key].push_back(id);
        _waiters.emplace(id, Waiter{&session, call.block_args ? *call.block_args : args, std::move(keys), call.block_null_bulk, std::move(wake)});
        executed.wait = id;
        executed.wait_seconds = call.block_seconds;
        publish_events();
        return executed;
      }
      // Nowhere to wait: the answer is the timeout's, at once.
      out.append(timeout_reply(session.protocol, call.block_null_bulk));
    } else {
      out.append(reply.bytes());
      touch(*spec, args, db);
    }
    if (!_waiters.empty()) serve_ready(woken);
    publish_events();
  }
  for (auto& [wake_one, bytes] : woken) wake_one(std::move(bytes));
  return executed;
}

void Database::run_locked(Session& session, const std::vector<std::string>& args, resp::Reply& out) {
  // Queued commands were found and checked when queued.
  const Spec* spec = find_spec(args[0]);
  resp::Reply reply(session.protocol);
  const size_t db = session.db;
  Call call{session, *this, *_dbs[db], reply, args};
  call.db = db;
  look_up_keys(*spec, args, db);
  try {
    spec->handler(call);
  } catch (const CommandError& e) {
    call.blocked = false;
    reply = resp::Reply(session.protocol);
    reply.error(e.what());
  }
  // Each command's events go out as it ends, inside a transaction or a script
  // as outside, as Valkey sends them.
  publish_events();
  // Inside a transaction a blocking command does not wait.
  if (call.blocked) {
    out.raw(timeout_reply(session.protocol, call.block_null_bulk));
    return;
  }
  out.raw(reply.bytes());
  touch(*spec, args, db);
}

std::string Database::scoped(size_t db, std::string_view key) {
  std::string out = std::to_string(db);
  out.push_back('\0');
  out.append(key);
  return out;
}

std::pair<size_t, std::string_view> Database::unscoped(std::string_view scoped_key) {
  const size_t nul = scoped_key.find('\0');
  size_t db = 0;
  for (char c : scoped_key.substr(0, nul)) db = db * 10 + static_cast<size_t>(c - '0');
  return {db, scoped_key.substr(nul + 1)};
}

void Database::touch(const Spec& spec, const std::vector<std::string>& args, size_t db) {
  if (spec.flags.find("write") == std::string_view::npos) return;
  if (spec.name == "flushall" || spec.name == "flushdb") {
    // FLUSHDB empties one database; its watches break, others' stand.
    for (auto& [key, sessions] : _watchers) {
      if (spec.name == "flushdb" && unscoped(key).first != db) continue;
      for (Session* s : sessions) s->watch_touched = true;
    }
    return;
  }
  for (auto key : keys_of(spec, args)) {
    std::string named = scoped(db, key);
    auto found = _watchers.find(named);
    if (found != _watchers.end())
      for (Session* s : found->second) s->watch_touched = true;
    if (!_waiting_on.empty()) _touched.push_back(std::move(named));
  }
  // MOVE and COPY ... DB name a key in another database too.
  if (spec.name == "move" || spec.name == "copy") {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
      if (spec.name == "copy" && !iequals(args[i], "DB")) continue;
      auto target = parse_integer(args[spec.name == "move" ? 2 : i + 1]);
      if (!target || *target < 0) break;
      const std::string& key = args[spec.name == "move" ? 1 : 2];
      std::string named = scoped(static_cast<size_t>(*target), key);
      auto found = _watchers.find(named);
      if (found != _watchers.end())
        for (Session* s : found->second) s->watch_touched = true;
      if (!_waiting_on.empty()) _touched.push_back(std::move(named));
      break;
    }
  }
}

void Database::swap_locked(size_t a, size_t b) {
  std::swap(_dbs.at(a), _dbs.at(b));
  bind_listeners();
  for (auto& [key, sessions] : _watchers) {
    const size_t db = unscoped(key).first;
    if (db != a && db != b) continue;
    for (Session* s : sessions) s->watch_touched = true;
  }
}

void Database::watch_locked(Session& session, const std::string& key) {
  const std::string named = scoped(session.db, key);
  for (const auto& [watched, existed] : session.watched)
    if (watched == named) return;
  session.watched.emplace_back(named, _dbs[session.db]->contains(key));
  _watchers[named].push_back(&session);
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
  for (const auto& [named, existed] : session.watched) {
    auto [db, key] = unscoped(named);
    if (existed && !_dbs[db]->contains(key)) return true;
  }
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
    for (const std::string& named : ready) {
      auto [db, key_view] = unscoped(named);
      const std::string key(key_view);
      for (;;) {
        auto queue = _waiting_on.find(named);
        if (queue == _waiting_on.end() || queue->second.empty() || !_dbs[db]->contains(key)) break;
        const uint64_t id = queue->second.front();
        Waiter& waiter = _waiters.at(id);
        const Spec* spec = find_spec(waiter.args[0]);
        resp::Reply reply(waiter.session->protocol);
        Call call{*waiter.session, *this, *_dbs[db], reply, waiter.args};
        call.db = db;
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
        touch(*spec, served_args, db);
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
  size_t removed = 0;
  for (auto& db : _dbs) removed += db->expire_step(budget);
  publish_events();
  return removed;
}

}  // namespace mimir::command
