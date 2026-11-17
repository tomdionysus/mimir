//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/command/database.h"

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <variant>

#include "dannan/net/codec.h"
#include "mimir/command/args.h"
#include "mimir/command/handlers.h"
#include "mimir/keyspace/rdb.h"

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

// Whether `flags` (COMMAND's, space-separated) holds `flag` as a whole word.
bool has_flag(std::string_view flags, std::string_view flag) {
  while (!flags.empty()) {
    const size_t space = flags.find(' ');
    if (flags.substr(0, space) == flag) return true;
    flags = space == std::string_view::npos ? std::string_view() : flags.substr(space + 1);
  }
  return false;
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

// Runs its function when it goes: declared before a lock, it runs once the
// lock is let go, however the block is left.
struct AfterUnlock {
  std::function<void()> fn;
  ~AfterUnlock() {
    if (fn) fn();
  }
};

// The clock a write runs on by agreement: its issue time, on every attempt.
class FixedClock final : public dannan::contract::WallClock {
 public:
  explicit FixedClock(uint64_t ms) : _ms(ms) {}
  uint64_t unix_ms() const noexcept override { return _ms; }

 private:
  uint64_t _ms;
};

// How long a write by agreement may take, retries included, before its
// connection is told the cluster could not decide it.
constexpr uint64_t proposal_deadline_ms = 15'000;

// A notification a write raises: its database, event and key.
using Raised = std::tuple<size_t, std::string, std::string>;

// A write's result as the attempt that committed carries it: its reply, the
// notifications it raises once committed, and whether it wrote (its watchers
// and trackers are told).
struct Result {
  std::string reply;
  std::vector<Raised> raised;
  bool wrote = false;
  // A blocking command that found nothing: it waits for its key to be written.
  bool blocked = false;
};

std::string encode_result(std::string_view reply, const std::vector<Raised>& raised, bool wrote = false, bool blocked = false) {
  dannan::net::Writer out;
  out.bytes({reinterpret_cast<const uint8_t*>(reply.data()), reply.size()});
  out.u32(static_cast<uint32_t>(raised.size()));
  for (const auto& [db, event, key] : raised) {
    out.u64(db);
    out.string(event);
    out.string(key);
  }
  out.u8(static_cast<uint8_t>((wrote ? 1 : 0) | (blocked ? 2 : 0)));
  const auto bytes = out.take();
  return std::string(bytes.begin(), bytes.end());
}

// What a result holds; none for bytes that are not one.
std::optional<Result> decode_result(std::string_view bytes) {
  try {
    dannan::net::Reader in({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
    const auto reply = in.view_bytes(512ull * 1024 * 1024);
    Result out;
    out.reply.assign(reinterpret_cast<const char*>(reply.data()), reply.size());
    const uint32_t count = in.u32();
    if (count > 1u << 20) return std::nullopt;
    for (uint32_t i = 0; i < count; ++i) {
      const auto db = in.u64();
      auto event = in.string();
      auto key = in.string(512ull * 1024 * 1024);
      out.raised.emplace_back(static_cast<size_t>(db), std::move(event), std::move(key));
    }
    const uint8_t flags = in.u8();
    out.wrote = (flags & 1) != 0;
    out.blocked = (flags & 2) != 0;
    in.finish();
    return out;
  } catch (const dannan::net::DecodeError&) {
    return std::nullopt;
  }
}

// A reason as one line of an error reply.
std::string one_line(std::string text) {
  std::replace(text.begin(), text.end(), '\r', ' ');
  std::replace(text.begin(), text.end(), '\n', ' ');
  return text;
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
  for (const std::string& line : _settings.users) {
    if (auto error = _acl.load_line(line)) throw std::invalid_argument("users: " + *error);
  }
  if (!set_notify_flags_locked(_settings.notify_keyspace_events))
    throw std::invalid_argument("notify keyspace events: not Valkey's letters: " + _settings.notify_keyspace_events);
  if (_settings.agreements)
    _settings.agreements->on_committed(
        [this](size_t db, const std::string& key, const std::string& version, const cluster::Record& record) { install_committed(db, key, version, record); });
}

void Database::welcome_locked(Session& session) {
  session.welcomed = true;
  // Known from its first moment, so it may be a REDIRECT's target before it
  // has run a command.
  _sessions.emplace(session.id, &session);
  session.user = "default";
  const Acl::User* user = _acl.find("default");
  session.authenticated = user && user->nopass && user->enabled;
}

std::optional<std::string> Database::acl_refusal_locked(Session& session, const std::vector<std::string>& args, std::string_view context, Denial* denial) {
  const Acl::User* user = _acl.find(session.user);
  // A user deleted under a connection: it is being closed, and may do nothing.
  if (!user) {
    if (denial) *denial = Denial::command;
    return Acl::message(AclVerdict{Denial::command, 0}, session.user, args, false);
  }
  const AclVerdict verdict = Acl::check(*user, args);
  if (verdict.denial == Denial::none) return std::nullopt;
  if (denial) *denial = verdict.denial;
  std::string reason = verdict.denial == Denial::command ? "command" : verdict.denial == Denial::key ? "key" : "channel";
  std::string object = verdict.denial == Denial::command ? Acl::full_name(args) : args[verdict.position];
  _acl.log(std::move(reason), std::string(context), std::move(object), session.user, connection::client_line(session, _wall.unix_ms()), _wall.unix_ms());
  return Acl::message(verdict, session.user, args, false);
}

void Database::log_auth_failure_locked(Session& session, std::string_view username) {
  _acl.log("auth", "toplevel", "AUTH", std::string(username), connection::client_line(session, _wall.unix_ms()), _wall.unix_ms());
}

void Call::notify(char cls, std::string_view event, std::string_view key) DANNAN_NO_THREAD_SAFETY_ANALYSIS { database.notify_locked(db, cls, event, key); }

void Database::bind_listeners() {
  for (size_t i = 0; i < _dbs.size(); ++i) {
    keyspace::Keyspace::Listener listener;
    listener.expired = [this, i](std::string_view key) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
      notify_locked(i, 'x', "expired", key);
      invalidate_locked(key, nullptr);
    };
    listener.created = [this, i](std::string_view key) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
      if (!_installing) notify_locked(i, 'n', "new", key);
    };
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
  keyspace::Keyspace& keys = _speculating && db == _scratch_db ? *_scratch : *_dbs[db];
  for (auto key : keys_of(spec, args)) {
    if (!keys.find(key)) notify_locked(db, 'm', "keymiss", key);
  }
}

void Database::publish_events() {
  // A speculative run's events are its result's, published at its commit.
  if (_events.empty() || _speculating) return;
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
  const size_t before = out.size();
  Executed executed = execute_one(session, args, out, std::move(wake));
  // CLIENT REPLY: OFF drops every reply; SKIP drops the reply of the command
  // after the one that set it. Read without the lock: only this connection's
  // own commands change them.
  if (session.reply_off || session.reply_skip) out.resize(before);
  session.reply_skip = false;
  if (session.reply_skip_next) {
    session.reply_skip = true;
    session.reply_skip_next = false;
  }
  // CLIENT CACHING is for the next command: spent by any but CLIENT's own,
  // and kept through a transaction until its EXEC.
  if (!session.in_multi && !args.empty() && !iequals(args[0], "client")) session.tracking.caching = false;
  return executed;
}

void Database::monitor_locked(Session& session) {
  session.monitoring = true;
  _monitors[session.id] = &session;
}

void Database::pause_locked(uint64_t until_ms, bool all) {
  const bool active = _pause_until_ms > _wall.unix_ms();
  _pause_all = all || (active && _pause_all);
  _pause_until_ms = std::max(active ? _pause_until_ms : 0, until_ms);
}

void Database::unpause_locked() {
  _pause_until_ms = 0;
  _pause_all = false;
  std::vector<uint64_t> held;
  for (const auto& [id, waiter] : _waiters)
    if (waiter.postponed) held.push_back(id);
  for (uint64_t id : held) {
    _released.emplace_back(std::move(_waiters.at(id).wake), std::string());
    forget_waiter(id);
  }
}

bool Database::paused_locked(const Spec& spec, const Session& session) {
  if (_pause_until_ms == 0) return false;
  if (_wall.unix_ms() >= _pause_until_ms) {
    _pause_until_ms = 0;
    return false;
  }
  if (_pause_all) return true;
  // A write pause holds what writes or may replicate: EXEC when its queue
  // does.
  const auto writes = [](const Spec& s) { return has_flag(s.flags, "write") || has_flag(s.flags, "may_replicate"); };
  if (writes(spec)) return true;
  if (spec.name == "exec") {
    for (const auto& queued : session.queued) {
      const Spec* each = find_spec(queued[0]);
      if (each && writes(*each)) return true;
    }
  }
  return false;
}

bool Database::unblock_locked(int64_t client, bool error) {
  for (auto& [id, waiter] : _waiters) {
    // A write waiting for its agreement is not blocked: it is answered by its
    // commit.
    if (waiter.proposal || !waiter.session || waiter.session->id != client || waiter.postponed) continue;
    std::string bytes;
    if (error) {
      resp::Reply reply(waiter.session->protocol);
      reply.error("UNBLOCKED client unblocked via CLIENT UNBLOCK");
      bytes = reply.take();
    } else {
      bytes = timeout_reply(waiter.session->protocol, waiter.null_bulk);
    }
    _released.emplace_back(std::move(waiter.wake), std::move(bytes));
    forget_waiter(id);
    return true;
  }
  return false;
}

namespace {

// An argument as MONITOR shows it: quoted, with Valkey's escapes.
void quote_into(std::string& out, std::string_view arg) {
  static constexpr char hex[] = "0123456789abcdef";
  out += '"';
  for (unsigned char c : arg) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\a':
        out += "\\a";
        break;
      case '\b':
        out += "\\b";
        break;
      default:
        if (c >= 0x20 && c < 0x7f) {
          out += static_cast<char>(c);
        } else {
          out += "\\x";
          out += hex[c >> 4];
          out += hex[c & 0xf];
        }
    }
  }
  out += '"';
}

// Whether argument `i` of `args` is a secret MONITOR must not show.
bool redacted(const std::vector<std::string>& args, size_t i) {
  if (iequals(args[0], "auth")) return i >= 1;
  if (iequals(args[0], "acl")) return args.size() > 1 && iequals(args[1], "setuser") && i >= 3;
  if (iequals(args[0], "hello")) {
    for (size_t j = 2; j + 2 < args.size(); ++j)
      if (iequals(args[j], "AUTH") && (i == j + 1 || i == j + 2)) return true;
  }
  return false;
}

}  // namespace

void Database::feed_monitors(const Session& session, const Spec& spec, const std::vector<std::string>& args, size_t db) {
  if (_monitors.empty() || _speculating) return;
  // Valkey shows neither administrative commands nor those that ask not to be.
  if (spec.flags.find("admin") != std::string_view::npos || spec.flags.find("skip_monitor") != std::string_view::npos) return;
  const uint64_t now = _wall.unix_ms();
  std::string line = std::to_string(now / 1000) + ".";
  const std::string micros = std::to_string((now % 1000) * 1000);
  line += std::string(6 - micros.size(), '0') + micros;
  line += " [" + std::to_string(db) + " " + (session.scripted ? std::string("lua") : session.address) + "]";
  for (size_t i = 0; i < args.size(); ++i) {
    line += ' ';
    if (redacted(args, i))
      line += "\"(redacted)\"";
    else
      quote_into(line, args[i]);
  }
  for (const auto& [id, monitor] : _monitors) {
    if (!monitor->push) continue;
    resp::Reply reply(monitor->protocol);
    reply.simple(line);
    monitor->push(reply.take());
  }
}

void Database::track_on_locked(Session& session) {
  if (!session.tracking.bcast) return;
  for (const auto& prefix : session.tracking.prefixes) _prefixes[prefix].insert(session.id);
}

void Database::track_off_locked(Session& session) {
  for (const auto& prefix : session.tracking.prefixes) {
    auto found = _prefixes.find(prefix);
    if (found == _prefixes.end()) continue;
    found->second.erase(session.id);
    if (found->second.empty()) _prefixes.erase(found);
  }
  // The keys it read stay in the table until they change, as Valkey's do; a
  // connection no longer tracking is passed over then.
  session.tracking = Session::Tracking{};
}

void Database::track_reads_locked(Session& tracker, const Spec& spec, const std::vector<std::string>& args) {
  const auto& t = tracker.tracking;
  if (!t.on || t.bcast || _speculating) return;
  // A read-only script is not tracked itself, only the commands it runs.
  if (!has_flag(spec.flags, "readonly") || spec.name == "eval_ro" || spec.name == "evalsha_ro" || spec.name == "fcall_ro") return;
  if ((t.optin && !t.caching) || (t.optout && t.caching)) return;
  for (const auto& [position, flags] : Acl::keys_with_flags(args)) _tracked[args[position]].insert(tracker.id);
}

void Database::invalidate_locked(std::string_view key, const Session* modifier) {
  const int64_t by = modifier ? modifier->id : 0;
  for (const auto& [prefix, ids] : _prefixes) {
    if (key.substr(0, prefix.size()) != prefix) continue;
    auto [it, inserted] = _broadcast[prefix].emplace(std::string(key), by);
    // Changed by more than one connection: NOLOOP keeps it from none.
    if (!inserted && it->second != by) it->second = 0;
  }
  auto found = _tracked.find(std::string(key));
  if (found == _tracked.end()) return;
  const std::set<int64_t> readers = std::move(found->second);
  _tracked.erase(found);
  for (int64_t id : readers) {
    auto session = _sessions.find(id);
    if (session == _sessions.end()) continue;
    Session& target = *session->second;
    if (!target.tracking.on || target.tracking.bcast) continue;
    if (target.tracking.noloop && &target == modifier) continue;
    if (&target == _current) {
      _own_invalidations.emplace_back(std::string(key));
    } else {
      const std::vector<std::string> keys{std::string(key)};
      deliver_locked(target, &keys, nullptr);
    }
  }
}

void Database::invalidate_all_locked() {
  for (const auto& [id, session] : _sessions) {
    if (!session->tracking.on) continue;
    if (session == _current)
      _own_invalidations.emplace_back(std::nullopt);
    else
      deliver_locked(*session, nullptr, nullptr);
  }
  _tracked.clear();
}

void Database::flush_tracking_locked(std::string* out) {
  if (_current && out) {
    auto own = std::move(_own_invalidations);
    for (const auto& key : own) {
      if (key) {
        const std::vector<std::string> keys{*key};
        deliver_locked(*_current, &keys, out);
      } else {
        deliver_locked(*_current, nullptr, out);
      }
    }
  }
  _own_invalidations.clear();
  if (_broadcast.empty()) return;
  auto pending = std::move(_broadcast);
  _broadcast.clear();
  // One message per prefix, in the prefixes' order, as Valkey sends them.
  for (const auto& [prefix, keys] : pending) {
    auto clients = _prefixes.find(prefix);
    if (clients == _prefixes.end()) continue;
    for (int64_t id : clients->second) {
      auto session = _sessions.find(id);
      if (session == _sessions.end() || !session->second->tracking.on) continue;
      std::vector<std::string> list;
      for (const auto& [key, by] : keys)
        if (!(session->second->tracking.noloop && by == id)) list.push_back(key);
      if (!list.empty()) deliver_locked(*session->second, &list, out);
    }
  }
}

void Database::deliver_locked(Session& tracker, const std::vector<std::string>* keys, std::string* out) {
  Session* const current = _current;
  const auto emit = [&](Session& target, std::string bytes) {
    if (out && &target == current)
      out->append(bytes);
    else if (target.push)
      target.push(std::move(bytes));
  };
  Session* target = &tracker;
  bool redirected = false;
  if (tracker.tracking.redirect != 0) {
    auto found = _sessions.find(tracker.tracking.redirect);
    if (found == _sessions.end()) {
      // The connection it redirects to has gone: told so, in RESP3.
      tracker.tracking.broken = true;
      if (tracker.protocol == resp::Protocol::resp3) {
        resp::Reply reply(resp::Protocol::resp3);
        reply.push(2);
        reply.bulk("tracking-redir-broken");
        reply.integer(tracker.tracking.redirect);
        emit(tracker, reply.take());
      }
      return;
    }
    target = found->second;
    redirected = true;
  }
  resp::Reply reply(target->protocol);
  if (target->protocol == resp::Protocol::resp3) {
    reply.push(2);
    reply.bulk("invalidate");
  } else if (redirected && (target->subscriptions > 0 || target->shard_subscriptions > 0)) {
    // RESP2 has no push: a redirected connection in Pub/Sub mode is told as
    // a message on __redis__:invalidate.
    reply.array(3);
    reply.bulk("message");
    reply.bulk("__redis__:invalidate");
  } else {
    return;
  }
  if (keys) {
    reply.array(keys->size());
    for (const auto& key : *keys) reply.bulk(key);
  } else {
    // A flush: null, in the tracking connection's own protocol.
    reply.raw(tracker.protocol == resp::Protocol::resp3 ? "_\r\n" : "$-1\r\n");
  }
  emit(*target, reply.take());
}

Executed Database::execute_one(Session& session, const std::vector<std::string>& args, std::string& out, Wake wake) {
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
  Woken woken;
  {
    // A write by agreement is proposed once the lock is let go: the change runs
    // under the lock, from the agreement's own thread.
    AfterUnlock after;
    dannan::contract::Lock lock(_mutex);
    if (!session.welcomed) welcome_locked(session);
    _sessions.emplace(session.id, &session);
    _current = &session;
    _own_invalidations.clear();
    session.last_ms = _wall.unix_ms();
    session.last_command = Acl::full_name(args);
    // Valkey's order: authentication, then a transaction's own refusals, then
    // ACL; each refusal before queueing fails the transaction.
    const bool no_auth = spec->flags.find("no_auth") != std::string_view::npos;
    if (!no_auth && !session.authenticated && _acl.auth_required()) {
      session.multi_failed = session.multi_failed || session.in_multi;
      reply.error("NOAUTH Authentication required.");
      out.append(reply.bytes());
      return executed;
    }
    // Inside a transaction MULTI and WATCH are refused, and the refusal aborts
    // the EXEC, as Valkey 8 has it (the differential gate's finding).
    if (session.in_multi && (spec->name == "multi" || spec->name == "watch" || has_flag(spec->flags, "no_multi"))) {
      session.multi_failed = true;
      reply.error("ERR Command not allowed inside a transaction");
      out.append(reply.bytes());
      return executed;
    }
    if (auto refused = acl_refusal_locked(session, args, session.in_multi ? "multi" : "toplevel")) {
      session.multi_failed = session.multi_failed || session.in_multi;
      reply.error("NOPERM " + *refused);
      out.append(reply.bytes());
      return executed;
    }
    // CLIENT PAUSE holds the command, unrun, until the pause ends; a caller
    // that cannot wait has it run at once.
    if (wake && paused_locked(*spec, session)) {
      const uint64_t id = _next_wait++;
      _waiters.emplace(id, Waiter{&session, args, {}, false, std::move(wake), true});
      const uint64_t now = _wall.unix_ms();
      executed.wait = id;
      executed.wait_seconds = std::max(0.001, static_cast<double>(_pause_until_ms - std::min(now, _pause_until_ms)) / 1000.0);
      executed.postponed = true;
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
    _touched.clear();
    ++session.commands;
    // The database the command ran in, before it may change it (SELECT).
    const size_t db = session.db;
    // In a cluster (stage 3) a write naming one key commits by agreement; the
    // connection waits for it. Keyless writes (FUNCTION, SCRIPT) hold this
    // node's own state and run here.
    if (_settings.agreements) {
      if (auto refused = cluster_refusal_locked(*spec, args, session)) {
        if (spec->name == "exec") {
          session.in_multi = false;
          session.multi_failed = false;
          session.queued.clear();
          unwatch_locked(session);
        }
        reply.error(*refused);
        out.append(reply.bytes());
        return executed;
      }
      auto keys = spec->find_keys ? spec->find_keys(args) : keys_of(*spec, args);
      // WATCH reads each key's write count from a majority (ruling 5's strict
      // read), so the EXEC's agreement can tell whether anything wrote it.
      if (spec->name == "watch" && !session.in_multi && wake) {
        const uint64_t wait = _next_wait++;
        _waiters.emplace(wait, Waiter{&session, args, {}, false, std::move(wake), false, true});
        executed.wait = wait;
        std::vector<std::string> named(args.begin() + 1, args.end());
        after.fn = [this, wait, db, named = std::move(named)]() mutable { watch_strictly(wait, db, std::move(named)); };
        return executed;
      }
      // An EXEC whose writes and watches all name one key commits whole by
      // agreement; one that only reads runs here.
      std::string exec_key;
      if (spec->name == "exec" && session.in_multi && !session.multi_failed) {
        const auto named = exec_key_locked(session);
        if (named.writes) exec_key = named.key;
      }
      if ((has_flag(spec->flags, "write") && keys.size() == 1) || !exec_key.empty()) {
        if (!wake) {
          reply.error("ERR this write needs a connection that can wait for its agreement");
          out.append(reply.bytes());
          return executed;
        }
        const std::string key = exec_key.empty() ? std::string(keys[0]) : exec_key;
        look_up_keys(*spec, args, db);
        feed_monitors(session, *spec, args, db);
        // An expired key is found here, as Valkey's write finds it: its
        // "expired" is this node's to announce, once, before the write's own.
        _dbs[db]->find(key);
        publish_events();
        auto proposal = std::make_shared<Proposal>();
        proposal->wait = _next_wait++;
        proposal->session = session;
        proposal->spec = spec;
        proposal->args = args;
        proposal->db = db;
        proposal->key = key;
        if (!exec_key.empty()) {
          // The transaction is the agreement's now: this connection's ends.
          session.in_multi = false;
          session.multi_failed = false;
          session.queued.clear();
          unwatch_locked(session);
        }
        proposal->operation = _settings.agreements->operation_id();
        proposal->issued_ms = _wall.unix_ms();
        proposal->deadline_ms = proposal->issued_ms + proposal_deadline_ms;
        Waiter waiter{&session, args, {}, false, std::move(wake), false, true};
        executed.wait_seconds = 0;
        if (exec_key.empty() && has_flag(spec->flags, "blocking")) {
          // A blocking pop waits on its key with its own timeout; while an
          // attempt is proposed a timeout waits for it.
          const auto blocking = blocking_locked(*spec, args, session, db);
          if (!blocking.error.empty()) {
            reply.error(blocking.error);
            out.append(reply.bytes());
            return executed;
          }
          waiter.keys = {scoped(db, key)};
          waiter.null_bulk = blocking.null_bulk;
          waiter.blocked = proposal;
          waiter.in_flight = true;
          _waiting_on[waiter.keys.front()].push_back(proposal->wait);
          executed.wait_seconds = blocking.seconds;
        }
        _waiters.emplace(proposal->wait, std::move(waiter));
        executed.wait = proposal->wait;
        after.fn = [this, proposal] { propose(proposal); };
        return executed;
      }
    }
    Call call{session, *this, *_dbs[db], reply, args};
    call.db = db;
    look_up_keys(*spec, args, db);
    feed_monitors(session, *spec, args, db);
    try {
      spec->handler(call);
    } catch (const CommandError& e) {
      // A handler refuses before it changes anything; what it wrote is dropped.
      call.blocked = false;
      reply = resp::Reply(session.protocol);
      reply.error(e.what());
    }
    for (auto& released : _released) woken.push_back(std::move(released));
    _released.clear();
    if (call.migrate) {
      // MIGRATE's talk with its target is its connection's, off the lock; a
      // caller that cannot wait for it is refused.
      if (wake) {
        executed.migrate = std::move(call.migrate);
        publish_events();
        flush_tracking_locked(&out);
      } else {
        reply.error("ERR MIGRATE needs a connection that can wait for its target");
        out.append(reply.bytes());
      }
      for (auto& [wake_one, bytes] : woken) wake_one(std::move(bytes));
      return executed;
    }
    if (call.blocked) {
      if (wake) {
        const uint64_t id = _next_wait++;
        std::vector<std::string> keys;
        for (const auto& key : call.block_keys) keys.push_back(scoped(db, key));
        for (const auto& key : keys) _waiting_on[key].push_back(id);
        _waiters.emplace(id, Waiter{&session, call.block_args ? *call.block_args : args, std::move(keys), call.block_null_bulk, std::move(wake), false});
        executed.wait = id;
        executed.wait_seconds = call.block_seconds;
        publish_events();
        return executed;
      }
      // Nowhere to wait: the answer is the timeout's, at once.
      out.append(timeout_reply(session.protocol, call.block_null_bulk));
    } else {
      out.append(reply.bytes());
      track_reads_locked(session, *spec, args);
      touch(*spec, args, db, &session);
    }
    if (!_waiters.empty()) serve_ready(woken);
    publish_events();
    // Invalidations for this connection go after its reply.
    flush_tracking_locked(&out);
  }
  for (auto& [wake_one, bytes] : woken) wake_one(std::move(bytes));
  start_pending();
  return executed;
}

void Database::repropose_locked(const std::shared_ptr<Proposal>& proposal) {
  // A new attempt is a new operation: the last one found nothing and changed
  // nothing.
  proposal->operation = _settings.agreements->operation_id();
  proposal->issued_ms = _wall.unix_ms();
  proposal->deadline_ms = proposal->issued_ms + proposal_deadline_ms;
  _pending.push_back(proposal);
}

void Database::start_pending() {
  std::vector<std::shared_ptr<Proposal>> pending;
  {
    dannan::contract::Lock lock(_mutex);
    pending = std::move(_pending);
    _pending.clear();
  }
  for (auto& proposal : pending) propose(std::move(proposal));
}

Database::Blocking Database::blocking_locked(const Spec& spec, const std::vector<std::string>& args, Session& session, size_t db) {
  (void)session;
  (void)db;
  // A blocking write's timeout, read from its arguments as its handler reads
  // it: the last argument, BLMPOP's and BZMPOP's first, XREADGROUP's BLOCK
  // (milliseconds; without it the command never waits).
  Blocking out;
  const std::string_view name = spec.name;
  out.null_bulk = name == "blmove" || name == "brpoplpush";
  try {
    if (name == "xreadgroup") {
      for (size_t i = 1; i + 1 < args.size(); ++i) {
        if (iequals(args[i], "STREAMS")) break;
        if (!iequals(args[i], "BLOCK")) continue;
        const auto ms = parse_integer(args[i + 1]);
        // The handler refuses a bad one; it answers through the agreement.
        if (ms && *ms > 0) out.seconds = static_cast<double>(*ms) / 1000.0;
        break;
      }
    } else if (name == "blmpop" || name == "bzmpop") {
      out.seconds = block_timeout(args.at(1));
    } else {
      out.seconds = block_timeout(args.back());
    }
  } catch (const CommandError& e) {
    out.error = e.what();
  }
  return out;
}

std::optional<std::string> Database::cluster_refusal_locked(const Spec& spec, const std::vector<std::string>& args, const Session& session) {
  static const std::string several = "ERR this command writes more than one key, which Mimir refuses until Dannan 0.10.0's atomic commit (ruling 35)";
  static const std::string later = "ERR Mimir does not yet run blocking commands, or transactions and scripts that write, by agreement";
  const std::string_view name = spec.name;
  // A script may write any key it is given: refused until step 8, but for
  // the read-only forms.
  if (name == "eval" || name == "evalsha" || name == "fcall") return later;
  if (name == "exec") {
    if (!session.in_multi || session.multi_failed) return std::nullopt;
    // Inside a transaction a blocking command does not wait, as Valkey has it.
    for (const auto& queued : session.queued) {
      const Spec* inner = find_spec(queued[0]);
      if (inner && (inner->name == "eval" || inner->name == "evalsha" || inner->name == "fcall")) return later;
    }
    if (exec_key_locked(session).several) return several;
    return std::nullopt;
  }
  if (!has_flag(spec.flags, "write")) return std::nullopt;
  // These name keys in two databases, or every key there is.
  if (name == "flushall" || name == "flushdb" || name == "swapdb" || name == "move" || name == "copy" || name == "migrate") return several;
  const auto keys = spec.find_keys ? spec.find_keys(args) : keys_of(spec, args);
  if (keys.size() > 1) return several;
  return std::nullopt;
}

Agreements::Changed Database::speculate(const Proposal& proposal, const cluster::Record& base) {
  // Applied already, by an attempt an earlier round took: its result again.
  if (const auto* applied = base.find(proposal.operation)) return {base, applied->reply};
  dannan::contract::Lock lock(_mutex);
  FixedClock clock(proposal.issued_ms);
  keyspace::Keyspace scratch(clock);
  if (base.payload) {
    auto restored = keyspace::rdb::restore(*base.payload);
    auto* entry = std::get_if<keyspace::Entry>(&restored);
    if (!entry) return {base, encode_result("-ERR this key's stored value cannot be read\r\n", {})};
    entry->expires_at = base.expires_at;
    scratch.put(proposal.key, std::move(*entry));
  }
  const size_t before = _events.size();
  // An expiry is announced by the live keyspace, where the write found it;
  // here it is only taken out.
  keyspace::Keyspace::Listener listener;
  listener.created = [this, db = proposal.db](std::string_view key) DANNAN_NO_THREAD_SAFETY_ANALYSIS { notify_locked(db, 'n', "new", key); };
  scratch.set_listener(std::move(listener));
  // This attempt's own session, clock and random choices: every attempt of
  // one operation chooses alike. An EXEC's queued commands run against the
  // scratch keyspace too, and nothing they do leaves it but by the result.
  Session session = proposal.session;
  const auto saved = _random;
  _random.seed(std::hash<std::string>{}(proposal.operation));
  _speculating = true;
  _scratch = &scratch;
  _scratch_db = proposal.db;
  _scratch_serial = base.serial;
  resp::Reply reply(session.protocol);
  Call call{session, *this, scratch, reply, proposal.args};
  call.db = proposal.db;
  try {
    proposal.spec->handler(call);
  } catch (const CommandError& e) {
    call.blocked = false;
    reply = resp::Reply(session.protocol);
    reply.error(e.what());
  }
  _speculating = false;
  _scratch = nullptr;
  _random = saved;
  // A blocking pop that found nothing changes nothing: it waits for its key.
  if (call.blocked) {
    _events.erase(_events.begin() + static_cast<std::ptrdiff_t>(before), _events.end());
    return {base, encode_result(timeout_reply(session.protocol, call.block_null_bulk), {}, false, true)};
  }
  // A write that ran counts against WATCH; an error or an aborted EXEC does not.
  const std::string& bytes = reply.bytes();
  const bool exec = proposal.spec->name == "exec";
  const bool wrote = !bytes.empty() && bytes[0] != '-' && !(exec && (bytes == "*-1\r\n" || bytes == "_\r\n"));
  cluster::Record out;
  out.applied = base.applied;
  out.serial = base.serial + (wrote ? 1 : 0);
  if (const keyspace::Entry* entry = scratch.find(proposal.key)) {
    out.payload = keyspace::rdb::dump(*entry);
    out.expires_at = entry->expires_at;
  }
  std::vector<Raised> raised;
  for (size_t i = before; i < _events.size(); ++i) raised.emplace_back(_events[i].db, _events[i].event, _events[i].key);
  _events.erase(_events.begin() + static_cast<std::ptrdiff_t>(before), _events.end());
  std::string result = encode_result(bytes, raised, wrote);
  if (!cluster::note_applied(out, {proposal.operation, proposal.issued_ms, result}))
    return {base, encode_result("-ERR too many writes to this key are in flight; try again\r\n", {})};
  return {std::move(out), std::move(result)};
}

void Database::propose(std::shared_ptr<Proposal> proposal) {
  _settings.agreements->change(
      proposal->db, proposal->key, [this, proposal](const cluster::Record& base) { return speculate(*proposal, base); },
      [this, proposal](Agreements::Outcome outcome) { decided(proposal, std::move(outcome)); });
}

void Database::decided(const std::shared_ptr<Proposal>& proposal, Agreements::Outcome outcome) {
  // Whether it took effect is not known: proposed again, it finds its own
  // operation if it did, and is applied once.
  if (!outcome.committed && outcome.unknown && _wall.unix_ms() < proposal->deadline_ms) return propose(proposal);
  Woken woken;
  Wake wake;
  std::string bytes;
  {
    dannan::contract::Lock lock(_mutex);
    const auto found = _waiters.find(proposal->wait);
    Session* session = found != _waiters.end() ? found->second.session : nullptr;
    const auto result = outcome.committed ? decode_result(outcome.result) : std::nullopt;
    if (result && result->blocked && found != _waiters.end() && found->second.blocked && !found->second.timed_out) {
      // A blocking pop that found nothing waits for its key again; if the key
      // was written meanwhile, its turn comes round at once.
      found->second.in_flight = false;
      _touched.clear();
      if (_dbs[proposal->db]->contains(proposal->key)) _touched.push_back(scoped(proposal->db, proposal->key));
      serve_ready(woken);
    } else if (result && result->blocked) {
      // Its time ran out while it was proposed: the timeout's reply.
      bytes = result->reply;
    } else if (result) {
      // A newer commit's notice may have come first: the view keeps it.
      install_locked(proposal->db, proposal->key, outcome.version, outcome.record);
      for (const auto& [db, event, key] : result->raised) _events.push_back(Event{db, event, key});
      Session* const previous = _current;
      _current = session;
      _own_invalidations.clear();
      _touched.clear();
      if (result->wrote) touch_key_locked(proposal->db, proposal->key, session);
      bytes = result->reply;
      if (!_waiters.empty()) serve_ready(woken);
      publish_events();
      flush_tracking_locked(session ? &bytes : nullptr);
      _current = previous;
    } else if (outcome.committed) {
      bytes = "-ERR the write committed, but its reply cannot be read\r\n";
    } else {
      bytes = "-CLUSTERDOWN " + one_line(outcome.why.empty() ? "the write could not be agreed" : outcome.why) + "\r\n";
    }
    const bool waits_on = result && result->blocked && found != _waiters.end() && found->second.blocked && !found->second.timed_out;
    if (found != _waiters.end() && !waits_on) {
      wake = std::move(found->second.wake);
      forget_waiter(proposal->wait);
    }
  }
  for (auto& [wake_one, reply] : woken) wake_one(std::move(reply));
  if (wake) wake(std::move(bytes));
  start_pending();
}

void Database::install_committed(size_t db, const std::string& key, const std::string& version, const cluster::Record& record) {
  Woken woken;
  {
    dannan::contract::Lock lock(_mutex);
    if (db >= _dbs.size()) return;
    if (!install_locked(db, key, version, record)) return;
    Session* const previous = _current;
    _current = nullptr;
    _touched.clear();
    touch_key_locked(db, key, nullptr);
    if (!_waiters.empty()) serve_ready(woken);
    flush_tracking_locked(nullptr);
    _current = previous;
  }
  for (auto& [wake_one, reply] : woken) wake_one(std::move(reply));
  start_pending();
}

void Database::touch_key_locked(size_t db, const std::string& key, const Session* modifier) {
  invalidate_locked(key, modifier);
  std::string named = scoped(db, key);
  if (const auto found = _watchers.find(named); found != _watchers.end())
    for (Session* s : found->second) s->watch_touched = true;
  if (!_waiting_on.empty()) _touched.push_back(std::move(named));
}

Database::ExecKey Database::exec_key_locked(const Session& session) {
  ExecKey out;
  std::set<std::string> named;
  for (const auto& queued : session.queued) {
    const Spec* spec = find_spec(queued[0]);
    if (!spec) continue;
    if (has_flag(spec->flags, "write")) out.writes = true;
    // These name keys in two databases, or every key there is.
    if (spec->name == "flushall" || spec->name == "flushdb" || spec->name == "swapdb" || spec->name == "move" || spec->name == "copy" || spec->name == "select")
      out.several = true;
    for (const auto key : spec->find_keys ? spec->find_keys(queued) : keys_of(*spec, queued)) named.emplace(key);
  }
  if (!out.writes) return out;
  for (const auto& [scoped_key, existed] : session.watched) named.emplace(unscoped(scoped_key).second);
  if (named.size() > 1) out.several = true;
  if (named.size() == 1) out.key = *named.begin();
  // A transaction that writes no key it names (keyless writes) runs here.
  if (named.empty()) out.writes = false;
  return out;
}

void Database::watch_strictly(uint64_t wait, size_t db, std::vector<std::string> keys) {
  struct Gathered {
    std::mutex mutex;
    size_t left = 0;
    std::map<std::string, uint64_t> serials;
    std::string failure;
  };
  auto gathered = std::make_shared<Gathered>();
  gathered->left = keys.size();
  for (const auto& key : keys) {
    _settings.agreements->read(db, key, [this, wait, db, keys, key, gathered](Agreements::Outcome outcome) {
      {
        std::lock_guard guard(gathered->mutex);
        if (outcome.committed)
          gathered->serials[scoped(db, key)] = outcome.record.serial;
        else if (gathered->failure.empty())
          gathered->failure = outcome.why.empty() ? "the watched key could not be read" : outcome.why;
        if (--gathered->left > 0) return;
      }
      Wake wake;
      std::string bytes;
      {
        dannan::contract::Lock lock(_mutex);
        const auto found = _waiters.find(wait);
        if (found == _waiters.end()) return;
        Session& session = *found->second.session;
        if (gathered->failure.empty()) {
          for (const auto& watched : keys) watch_locked(session, watched);
          for (const auto& [named, serial] : gathered->serials) session.watched_serials[named] = serial;
          bytes = "+OK\r\n";
        } else {
          bytes = "-CLUSTERDOWN " + one_line(gathered->failure) + "\r\n";
        }
        wake = std::move(found->second.wake);
        forget_waiter(wait);
      }
      if (wake) wake(std::move(bytes));
    });
  }
}

bool Database::install_locked(size_t db, const std::string& key, const std::string& version, const cluster::Record& record) {
  auto& known = _versions[scoped(db, key)];
  if (!known.empty() && version <= known) return false;
  known = version;
  auto& live = *_dbs.at(db);
  _installing = true;
  if (record.payload) {
    auto restored = keyspace::rdb::restore(*record.payload);
    if (auto* entry = std::get_if<keyspace::Entry>(&restored)) {
      entry->expires_at = record.expires_at;
      live.put(key, std::move(*entry));
    }
  } else {
    live.erase(key);
  }
  _installing = false;
  return true;
}

void Database::run_locked(Session& session, const std::vector<std::string>& args, resp::Reply& out) {
  // Queued commands were found and checked when queued.
  const Spec* spec = find_spec(args[0]);
  resp::Reply reply(session.protocol);
  const size_t db = session.db;
  // An EXEC by agreement runs its queue against its own keyspace.
  Call call{session, *this, _speculating && db == _scratch_db ? *_scratch : *_dbs[db], reply, args};
  call.db = db;
  look_up_keys(*spec, args, db);
  feed_monitors(session, *spec, args, db);
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
  // Inside a transaction or a script MIGRATE cannot wait for its target: on
  // Mimir it would hold every other client meanwhile.
  if (call.migrate) {
    out.error("ERR MIGRATE cannot run inside a transaction or a script on Mimir");
    return;
  }
  // Inside a transaction a blocking command does not wait.
  if (call.blocked) {
    out.raw(timeout_reply(session.protocol, call.block_null_bulk));
    return;
  }
  out.raw(reply.bytes());
  // A transaction's or a script's reads are its caller's to track, as are the
  // changes its writes make.
  if (_current) track_reads_locked(*_current, *spec, args);
  touch(*spec, args, db, _current);
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

void Database::touch(const Spec& spec, const std::vector<std::string>& args, size_t db, const Session* modifier) {
  // A speculative run's writes are told at its commit.
  if (_speculating || spec.flags.find("write") == std::string_view::npos) return;
  if (spec.name == "flushall" || spec.name == "flushdb") {
    // FLUSHDB empties one database; its watches break, others' stand.
    for (auto& [key, sessions] : _watchers) {
      if (spec.name == "flushdb" && unscoped(key).first != db) continue;
      for (Session* s : sessions) s->watch_touched = true;
    }
    invalidate_all_locked();
    return;
  }
  for (auto key : keys_of(spec, args)) {
    invalidate_locked(key, modifier);
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
      invalidate_locked(key, modifier);
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
  session.watched_serials.clear();
}

bool Database::watch_broken_locked(Session& session) {
  // An EXEC by agreement: a watched key is broken when its write count, as
  // the agreement found it, is not the one read at WATCH.
  if (_speculating) {
    for (const auto& [named, serial] : session.watched_serials)
      if (serial != _scratch_serial) return true;
    return false;
  }
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
  track_off_locked(session);
  _sessions.erase(session.id);
  _monitors.erase(session.id);
  if (_current == &session) _current = nullptr;
  // Its waits end with it; one whose agreement is in flight is left for its
  // outcome to finish, answering nobody (an element it pops is gone with the
  // connection, as Valkey's is when a client leaves with a reply unread).
  std::vector<uint64_t> gone;
  for (auto& [id, waiter] : _waiters) {
    if (waiter.session != &session) continue;
    if (waiter.blocked && waiter.in_flight) {
      waiter.session = nullptr;
      waiter.wake = nullptr;
      waiter.timed_out = true;
    } else {
      gone.push_back(id);
    }
  }
  for (const uint64_t id : gone) forget_waiter(id);
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
        // A blocking pop by agreement is proposed again, not run here: the
        // oldest waiter's attempt, one at a time, so the order holds.
        if (waiter.blocked) {
          if (!waiter.in_flight) {
            waiter.in_flight = true;
            repropose_locked(waiter.blocked);
          }
          break;
        }
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
        const Session* served = waiter.session;
        forget_waiter(id);
        touch(*spec, served_args, db, served);
      }
    }
  }
}

std::string Database::finish_migrate(Session& session, const MigrateJob& job, const MigrateOutcome& outcome) {
  resp::Reply reply(session.protocol);
  if (outcome.unreachable) {
    reply.error("IOERR error or timeout writing to target instance");
    return reply.take();
  }
  dannan::contract::Lock lock(_mutex);
  _current = &session;
  _own_invalidations.clear();
  // AUTH's and SELECT's replies first; an error there is the target's
  // answer for every key, and none is removed.
  const size_t preamble = (job.password ? 1 : 0) + 1;
  std::optional<std::string> refused;
  for (size_t i = 0; i < preamble && i < outcome.replies.size(); ++i)
    if (!outcome.replies[i].empty() && outcome.replies[i][0] == '-') refused = outcome.replies[i].substr(1);
  std::vector<std::string> changed;
  if (job.source_db < _dbs.size()) {
    keyspace::Keyspace& keys = *_dbs[job.source_db];
    const Spec* del = find_spec("del");
    for (size_t i = 0; i < job.items.size() && preamble + i < outcome.replies.size(); ++i) {
      const std::string& answer = outcome.replies[preamble + i];
      if (!answer.empty() && answer[0] == '-') {
        if (!refused) refused = answer.substr(1);
        continue;
      }
      if (refused || job.copy) continue;
      const auto& item = job.items[i];
      keyspace::Entry* entry = keys.find(item.key);
      if (!entry) continue;
      // Changed while it was moving: the target has what it was, and it
      // stays here (Valkey, which stops the world to move it, cannot see this).
      if (keyspace::rdb::dump(*entry) != item.payload) {
        changed.push_back(item.key);
        continue;
      }
      keys.erase(item.key);
      notify_locked(job.source_db, 'g', "del", item.key);
      touch(*del, {"DEL", item.key}, job.source_db, &session);
    }
  }
  if (outcome.read_failed && !refused) {
    reply.error("IOERR error or timeout reading to target instance");
  } else if (refused) {
    reply.error("ERR Target instance replied with error: " + *refused);
  } else if (!changed.empty()) {
    reply.error("ERR MIGRATE: key '" + changed.front() + "' changed while it was moving; the target has its earlier value and it is kept here");
  } else {
    reply.simple("OK");
  }
  std::string out = reply.take();
  publish_events();
  flush_tracking_locked(&out);
  return out;
}

std::optional<std::string> Database::cancel_wait(uint64_t id) {
  dannan::contract::Lock lock(_mutex);
  auto found = _waiters.find(id);
  if (found == _waiters.end()) return std::nullopt;
  // A blocking pop being proposed may yet pop: its outcome answers, the
  // timeout's reply if it found nothing.
  if (found->second.blocked && found->second.in_flight) {
    found->second.timed_out = true;
    return std::nullopt;
  }
  // A command CLIENT PAUSE held has no reply yet: no bytes, to be run again.
  std::string bytes = found->second.postponed ? std::string() : timeout_reply(found->second.session->protocol, found->second.null_bulk);
  forget_waiter(id);
  return bytes;
}

size_t Database::waiting() {
  dannan::contract::Lock lock(_mutex);
  return _waiters.size();
}

size_t Database::expire_step(size_t budget) {
  dannan::contract::Lock lock(_mutex);
  // While CLIENT PAUSE holds writes nothing expires, as Valkey's pause holds
  // its expiry.
  if (_pause_until_ms > _wall.unix_ms()) return 0;
  // No connection's command runs here: every invalidation is pushed.
  _current = nullptr;
  size_t removed = 0;
  for (auto& db : _dbs) removed += db->expire_step(budget);
  publish_events();
  flush_tracking_locked(nullptr);
  return removed;
}

}  // namespace mimir::command
