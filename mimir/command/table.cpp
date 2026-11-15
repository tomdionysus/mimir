//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/command/table.h"

#include <array>
#include <string>
#include <unordered_map>

#include "mimir/command/args.h"
#include "mimir/command/handlers.h"

namespace mimir::command {

namespace {

Class classify_expire(const std::vector<std::string>& args) {
  for (size_t i = 3; i < args.size(); ++i) {
    if (iequals(args[i], "NX") || iequals(args[i], "XX") || iequals(args[i], "GT") || iequals(args[i], "LT")) return Class::agreement;
  }
  return Class::merge;
}

// The keys that follow a numkeys argument at `at`.
std::vector<std::string_view> numkeys_from(const std::vector<std::string>& args, size_t at) {
  std::vector<std::string_view> keys;
  if (at >= args.size()) return keys;
  auto count = parse_integer(args[at]);
  if (!count || *count <= 0) return keys;
  for (size_t i = at + 1; i < args.size() && i <= at + static_cast<size_t>(*count); ++i) keys.push_back(args[i]);
  return keys;
}
std::vector<std::string_view> numkeys_first(const std::vector<std::string>& args) { return numkeys_from(args, 1); }
std::vector<std::string_view> numkeys_second(const std::vector<std::string>& args) { return numkeys_from(args, 2); }

// XREAD's and XREADGROUP's keys: the first half of what follows STREAMS.
std::vector<std::string_view> after_streams(const std::vector<std::string>& args) {
  std::vector<std::string_view> keys;
  for (size_t i = 1; i < args.size(); ++i) {
    if (!iequals(args[i], "STREAMS")) continue;
    const size_t rest = args.size() - i - 1;
    for (size_t k = i + 1; k < i + 1 + rest / 2; ++k) keys.push_back(args[k]);
    break;
  }
  return keys;
}

// Valkey's arity, flags and key positions for each; Mimir's class.
const std::array specs{
    // Strings.
    Spec{"get", 2, "readonly fast", 1, 1, 1, Class::read, strings::get},
    Spec{"set", -3, "write denyoom", 1, 1, 1, Class::merge, strings::set, strings::classify_set},
    Spec{"setnx", 3, "write denyoom fast", 1, 1, 1, Class::agreement, strings::setnx},
    Spec{"setex", 4, "write denyoom", 1, 1, 1, Class::merge, strings::setex},
    Spec{"psetex", 4, "write denyoom", 1, 1, 1, Class::merge, strings::psetex},
    Spec{"getset", 3, "write denyoom fast", 1, 1, 1, Class::agreement, strings::getset},
    Spec{"getdel", 2, "write fast", 1, 1, 1, Class::agreement, strings::getdel},
    Spec{"getex", -2, "write fast", 1, 1, 1, Class::merge, strings::getex},
    Spec{"mget", -2, "readonly fast", 1, -1, 1, Class::read, strings::mget},
    Spec{"mset", -3, "write denyoom", 1, -1, 2, Class::merge, strings::mset},
    Spec{"msetnx", -3, "write denyoom", 1, -1, 2, Class::agreement, strings::msetnx},
    Spec{"incr", 2, "write denyoom fast", 1, 1, 1, Class::configurable, strings::incr},
    Spec{"decr", 2, "write denyoom fast", 1, 1, 1, Class::configurable, strings::decr},
    Spec{"incrby", 3, "write denyoom fast", 1, 1, 1, Class::configurable, strings::incrby},
    Spec{"decrby", 3, "write denyoom fast", 1, 1, 1, Class::configurable, strings::decrby},
    Spec{"incrbyfloat", 3, "write denyoom fast", 1, 1, 1, Class::configurable, strings::incrbyfloat},
    Spec{"append", 3, "write denyoom fast", 1, 1, 1, Class::configurable, strings::append},
    Spec{"strlen", 2, "readonly fast", 1, 1, 1, Class::read, strings::strlen},
    Spec{"getrange", 4, "readonly", 1, 1, 1, Class::read, strings::getrange},
    Spec{"substr", 4, "readonly", 1, 1, 1, Class::read, strings::getrange},
    Spec{"setrange", 4, "write denyoom", 1, 1, 1, Class::configurable, strings::setrange},
    // Bitmaps: strings read and written by bit. Setting bits merges (a bit is
    // a register); BITOP's store and BITFIELD's increments depend on the value
    // before them.
    Spec{"setbit", 4, "write denyoom", 1, 1, 1, Class::merge, bitmaps::setbit},
    Spec{"getbit", 3, "readonly fast", 1, 1, 1, Class::read, bitmaps::getbit},
    Spec{"bitcount", -2, "readonly", 1, 1, 1, Class::read, bitmaps::bitcount},
    Spec{"bitpos", -3, "readonly", 1, 1, 1, Class::read, bitmaps::bitpos},
    Spec{"bitop", -4, "write denyoom", 2, -1, 1, Class::agreement, bitmaps::bitop},
    Spec{"bitfield", -2, "write denyoom", 1, 1, 1, Class::configurable, bitmaps::bitfield},
    Spec{"bitfield_ro", -2, "readonly fast", 1, 1, 1, Class::read, bitmaps::bitfield_ro},
    // HyperLogLog: registers only grow, so adds and merges are joins.
    Spec{"pfadd", -2, "write denyoom fast", 1, 1, 1, Class::merge, hyperloglog::pfadd},
    Spec{"pfcount", -2, "readonly may_replicate", 1, -1, 1, Class::read, hyperloglog::pfcount},
    Spec{"pfmerge", -2, "write denyoom", 1, -1, 1, Class::merge, hyperloglog::pfmerge},
    // Lists. Pushes merge; what depends on positions or pops commits by agreement.
    Spec{"lpush", -3, "write denyoom fast", 1, 1, 1, Class::merge, lists::lpush},
    Spec{"rpush", -3, "write denyoom fast", 1, 1, 1, Class::merge, lists::rpush},
    Spec{"lpushx", -3, "write denyoom fast", 1, 1, 1, Class::agreement, lists::lpushx},
    Spec{"rpushx", -3, "write denyoom fast", 1, 1, 1, Class::agreement, lists::rpushx},
    Spec{"lpop", -2, "write fast", 1, 1, 1, Class::agreement, lists::lpop},
    Spec{"rpop", -2, "write fast", 1, 1, 1, Class::agreement, lists::rpop},
    Spec{"llen", 2, "readonly fast", 1, 1, 1, Class::read, lists::llen},
    Spec{"lrange", 4, "readonly", 1, 1, 1, Class::read, lists::lrange},
    Spec{"lindex", 3, "readonly", 1, 1, 1, Class::read, lists::lindex},
    Spec{"lset", 4, "write denyoom", 1, 1, 1, Class::agreement, lists::lset},
    Spec{"linsert", 5, "write denyoom", 1, 1, 1, Class::agreement, lists::linsert},
    Spec{"lrem", 4, "write", 1, 1, 1, Class::agreement, lists::lrem},
    Spec{"ltrim", 4, "write", 1, 1, 1, Class::agreement, lists::ltrim},
    Spec{"lpos", -3, "readonly", 1, 1, 1, Class::read, lists::lpos},
    Spec{"lmove", 5, "write denyoom", 1, 2, 1, Class::agreement, lists::lmove},
    Spec{"rpoplpush", 3, "write denyoom", 1, 2, 1, Class::agreement, lists::rpoplpush},
    // LMPOP's keys follow numkeys; the table's positions cannot say where they end.
    Spec{"lmpop", -4, "write", 0, 0, 0, Class::agreement, lists::lmpop, nullptr, numkeys_first},
    // The blocking pops: agreement, and a wait where nothing is there yet.
    Spec{"blpop", -3, "write blocking", 1, -2, 1, Class::agreement, lists::blpop},
    Spec{"brpop", -3, "write blocking", 1, -2, 1, Class::agreement, lists::brpop},
    Spec{"blmove", 6, "write denyoom blocking", 1, 2, 1, Class::agreement, lists::blmove},
    Spec{"brpoplpush", 4, "write denyoom blocking", 1, 2, 1, Class::agreement, lists::brpoplpush},
    Spec{"blmpop", -5, "write blocking", 0, 0, 0, Class::agreement, lists::blmpop, nullptr, numkeys_second},
    // Hashes. Field writes merge; what depends on a field's value is configurable
    // or commits by agreement.
    Spec{"hset", -4, "write denyoom fast", 1, 1, 1, Class::merge, hashes::hset},
    Spec{"hmset", -4, "write denyoom fast", 1, 1, 1, Class::merge, hashes::hmset},
    Spec{"hsetnx", 4, "write denyoom fast", 1, 1, 1, Class::agreement, hashes::hsetnx},
    Spec{"hget", 3, "readonly fast", 1, 1, 1, Class::read, hashes::hget},
    Spec{"hmget", -3, "readonly fast", 1, 1, 1, Class::read, hashes::hmget},
    Spec{"hdel", -3, "write fast", 1, 1, 1, Class::merge, hashes::hdel},
    Spec{"hlen", 2, "readonly fast", 1, 1, 1, Class::read, hashes::hlen},
    Spec{"hstrlen", 3, "readonly fast", 1, 1, 1, Class::read, hashes::hstrlen},
    Spec{"hexists", 3, "readonly fast", 1, 1, 1, Class::read, hashes::hexists},
    Spec{"hkeys", 2, "readonly", 1, 1, 1, Class::read, hashes::hkeys},
    Spec{"hvals", 2, "readonly", 1, 1, 1, Class::read, hashes::hvals},
    Spec{"hgetall", 2, "readonly", 1, 1, 1, Class::read, hashes::hgetall},
    Spec{"hincrby", 4, "write denyoom fast", 1, 1, 1, Class::configurable, hashes::hincrby},
    Spec{"hincrbyfloat", 4, "write denyoom fast", 1, 1, 1, Class::configurable, hashes::hincrbyfloat},
    Spec{"hrandfield", -2, "readonly", 1, 1, 1, Class::read, hashes::hrandfield},
    Spec{"hscan", -3, "readonly", 1, 1, 1, Class::read, hashes::hscan},
    // Sets. Adds and removes merge (observed-remove); pops, moves and stores
    // that overwrite a destination from other keys commit by agreement.
    Spec{"sadd", -3, "write denyoom fast", 1, 1, 1, Class::merge, sets::sadd},
    Spec{"srem", -3, "write fast", 1, 1, 1, Class::merge, sets::srem},
    Spec{"smembers", 2, "readonly", 1, 1, 1, Class::read, sets::smembers},
    Spec{"sismember", 3, "readonly fast", 1, 1, 1, Class::read, sets::sismember},
    Spec{"smismember", -3, "readonly fast", 1, 1, 1, Class::read, sets::smismember},
    Spec{"scard", 2, "readonly fast", 1, 1, 1, Class::read, sets::scard},
    Spec{"spop", -2, "write fast", 1, 1, 1, Class::agreement, sets::spop},
    Spec{"srandmember", -2, "readonly", 1, 1, 1, Class::read, sets::srandmember},
    Spec{"smove", 4, "write fast", 1, 2, 1, Class::agreement, sets::smove},
    Spec{"sinter", -2, "readonly", 1, -1, 1, Class::read, sets::sinter},
    Spec{"sunion", -2, "readonly", 1, -1, 1, Class::read, sets::sunion},
    Spec{"sdiff", -2, "readonly", 1, -1, 1, Class::read, sets::sdiff},
    Spec{"sinterstore", -3, "write denyoom", 1, -1, 1, Class::agreement, sets::sinterstore},
    Spec{"sunionstore", -3, "write denyoom", 1, -1, 1, Class::agreement, sets::sunionstore},
    Spec{"sdiffstore", -3, "write denyoom", 1, -1, 1, Class::agreement, sets::sdiffstore},
    // SINTERCARD's keys follow numkeys.
    Spec{"sintercard", -3, "readonly", 0, 0, 0, Class::read, sets::sintercard},
    Spec{"sscan", -3, "readonly", 1, 1, 1, Class::read, sets::sscan},
    // Sorted sets. Scores set and members removed merge; pops, range removals
    // and stores commit by agreement.
    Spec{"zadd", -4, "write denyoom fast", 1, 1, 1, Class::merge, zsets::zadd, zsets::classify_zadd},
    Spec{"zincrby", 4, "write denyoom fast", 1, 1, 1, Class::configurable, zsets::zincrby},
    Spec{"zscore", 3, "readonly fast", 1, 1, 1, Class::read, zsets::zscore},
    Spec{"zmscore", -3, "readonly fast", 1, 1, 1, Class::read, zsets::zmscore},
    Spec{"zcard", 2, "readonly fast", 1, 1, 1, Class::read, zsets::zcard},
    Spec{"zcount", 4, "readonly fast", 1, 1, 1, Class::read, zsets::zcount},
    Spec{"zlexcount", 4, "readonly fast", 1, 1, 1, Class::read, zsets::zlexcount},
    Spec{"zrange", -4, "readonly", 1, 1, 1, Class::read, zsets::zrange},
    Spec{"zrangestore", -5, "write denyoom", 1, 2, 1, Class::agreement, zsets::zrangestore},
    Spec{"zrevrange", -4, "readonly", 1, 1, 1, Class::read, zsets::zrevrange},
    Spec{"zrangebyscore", -4, "readonly", 1, 1, 1, Class::read, zsets::zrangebyscore},
    Spec{"zrevrangebyscore", -4, "readonly", 1, 1, 1, Class::read, zsets::zrevrangebyscore},
    Spec{"zrangebylex", -4, "readonly", 1, 1, 1, Class::read, zsets::zrangebylex},
    Spec{"zrevrangebylex", -4, "readonly", 1, 1, 1, Class::read, zsets::zrevrangebylex},
    Spec{"zrank", -3, "readonly fast", 1, 1, 1, Class::read, zsets::zrank},
    Spec{"zrevrank", -3, "readonly fast", 1, 1, 1, Class::read, zsets::zrevrank},
    Spec{"zrem", -3, "write fast", 1, 1, 1, Class::merge, zsets::zrem},
    Spec{"zremrangebyrank", 4, "write", 1, 1, 1, Class::agreement, zsets::zremrangebyrank},
    Spec{"zremrangebyscore", 4, "write", 1, 1, 1, Class::agreement, zsets::zremrangebyscore},
    Spec{"zremrangebylex", 4, "write", 1, 1, 1, Class::agreement, zsets::zremrangebylex},
    Spec{"zpopmin", -2, "write fast", 1, 1, 1, Class::agreement, zsets::zpopmin},
    Spec{"zpopmax", -2, "write fast", 1, 1, 1, Class::agreement, zsets::zpopmax},
    Spec{"zmpop", -4, "write", 0, 0, 0, Class::agreement, zsets::zmpop, nullptr, numkeys_first},
    Spec{"bzpopmin", -3, "write fast blocking", 1, -2, 1, Class::agreement, zsets::bzpopmin},
    Spec{"bzpopmax", -3, "write fast blocking", 1, -2, 1, Class::agreement, zsets::bzpopmax},
    Spec{"bzmpop", -5, "write blocking", 0, 0, 0, Class::agreement, zsets::bzmpop, nullptr, numkeys_second},
    Spec{"zrandmember", -2, "readonly", 1, 1, 1, Class::read, zsets::zrandmember},
    Spec{"zscan", -3, "readonly", 1, 1, 1, Class::read, zsets::zscan},
    // The combining commands' keys follow numkeys.
    Spec{"zunion", -3, "readonly", 0, 0, 0, Class::read, zsets::zunion},
    Spec{"zinter", -3, "readonly", 0, 0, 0, Class::read, zsets::zinter},
    Spec{"zdiff", -3, "readonly", 0, 0, 0, Class::read, zsets::zdiff},
    Spec{"zunionstore", -4, "write denyoom", 1, 1, 1, Class::agreement, zsets::zunionstore},
    Spec{"zinterstore", -4, "write denyoom", 1, 1, 1, Class::agreement, zsets::zinterstore},
    Spec{"zdiffstore", -4, "write denyoom", 1, 1, 1, Class::agreement, zsets::zdiffstore},
    Spec{"zintercard", -3, "readonly", 0, 0, 0, Class::read, zsets::zintercard},
    // Keys.
    Spec{"del", -2, "write", 1, -1, 1, Class::merge, generic::del},
    Spec{"unlink", -2, "write fast", 1, -1, 1, Class::merge, generic::del},
    Spec{"exists", -2, "readonly fast", 1, -1, 1, Class::read, generic::exists},
    Spec{"type", 2, "readonly fast", 1, 1, 1, Class::read, generic::type},
    Spec{"expire", -3, "write fast", 1, 1, 1, Class::merge, generic::expire, classify_expire},
    Spec{"pexpire", -3, "write fast", 1, 1, 1, Class::merge, generic::pexpire, classify_expire},
    Spec{"expireat", -3, "write fast", 1, 1, 1, Class::merge, generic::expireat, classify_expire},
    Spec{"pexpireat", -3, "write fast", 1, 1, 1, Class::merge, generic::pexpireat, classify_expire},
    Spec{"ttl", 2, "readonly fast", 1, 1, 1, Class::read, generic::ttl},
    Spec{"pttl", 2, "readonly fast", 1, 1, 1, Class::read, generic::pttl},
    Spec{"expiretime", 2, "readonly fast", 1, 1, 1, Class::read, generic::expiretime},
    Spec{"pexpiretime", 2, "readonly fast", 1, 1, 1, Class::read, generic::pexpiretime},
    Spec{"persist", 2, "write fast", 1, 1, 1, Class::merge, generic::persist},
    Spec{"rename", 3, "write", 1, 2, 1, Class::agreement, generic::rename},
    Spec{"renamenx", 3, "write fast", 1, 2, 1, Class::agreement, generic::renamenx},
    Spec{"copy", -3, "write denyoom", 1, 2, 1, Class::agreement, generic::copy},
    Spec{"keys", 2, "readonly", 0, 0, 0, Class::read, generic::keys},
    Spec{"scan", -2, "readonly", 0, 0, 0, Class::read, generic::scan},
    Spec{"dbsize", 1, "readonly fast", 0, 0, 0, Class::read, generic::dbsize},
    Spec{"randomkey", 1, "readonly", 0, 0, 0, Class::read, generic::randomkey},
    Spec{"touch", -2, "readonly fast", 1, -1, 1, Class::read, generic::touch},
    Spec{"flushall", -1, "write", 0, 0, 0, Class::admin, generic::flushall},
    Spec{"flushdb", -1, "write", 0, 0, 0, Class::admin, generic::flushdb},
    Spec{"move", 3, "write fast", 1, 1, 1, Class::agreement, generic::move},
    // DUMP and RESTORE in Valkey's RDB encoding. RESTORE without REPLACE
    // depends on the key's absence, so it commits by agreement.
    Spec{"dump", 2, "readonly", 1, 1, 1, Class::read, generic::dump},
    Spec{"restore", -4, "write denyoom", 1, 1, 1, Class::agreement, generic::restore},
    Spec{"swapdb", 3, "write fast", 0, 0, 0, Class::admin, generic::swapdb},
    // Streams: an append-only log. Appends are a sequence, so they commit by
    // agreement; deleting an entry merges.
    Spec{"xadd", -5, "write denyoom fast", 1, 1, 1, Class::agreement, streams::xadd},
    Spec{"xlen", 2, "readonly fast", 1, 1, 1, Class::read, streams::xlen},
    Spec{"xrange", -4, "readonly", 1, 1, 1, Class::read, streams::xrange},
    Spec{"xrevrange", -4, "readonly", 1, 1, 1, Class::read, streams::xrevrange},
    Spec{"xdel", -3, "write fast", 1, 1, 1, Class::merge, streams::xdel},
    Spec{"xtrim", -4, "write", 1, 1, 1, Class::agreement, streams::xtrim},
    Spec{"xsetid", -3, "write denyoom fast", 1, 1, 1, Class::agreement, streams::xsetid},
    Spec{"xread", -4, "readonly blocking", 0, 0, 0, Class::read, streams::xread, nullptr, after_streams},
    // Consumer groups: delivery and acknowledgement change shared state that
    // depends on what came before, so they commit by agreement.
    Spec{"xgroup", -2, "write", 2, 2, 1, Class::agreement, streams::xgroup},
    Spec{"xreadgroup", -7, "write blocking", 0, 0, 0, Class::agreement, streams::xreadgroup, nullptr, after_streams},
    Spec{"xack", -4, "write fast", 1, 1, 1, Class::agreement, streams::xack},
    Spec{"xpending", -3, "readonly", 1, 1, 1, Class::read, streams::xpending},
    Spec{"xclaim", -6, "write fast", 1, 1, 1, Class::agreement, streams::xclaim},
    Spec{"xautoclaim", -6, "write fast", 1, 1, 1, Class::agreement, streams::xautoclaim},
    Spec{"xinfo", -2, "readonly", 2, 2, 1, Class::read, streams::xinfo},
    // Transactions: queued, then run whole under one hold of the lock.
    Spec{"multi", 1, "noscript loading stale fast allow_busy", 0, 0, 0, Class::connection, transactions::multi},
    Spec{"exec", 1, "noscript loading stale skip_slowlog", 0, 0, 0, Class::connection, transactions::exec},
    Spec{"discard", 1, "noscript loading stale fast allow_busy", 0, 0, 0, Class::connection, transactions::discard},
    Spec{"watch", -2, "noscript loading stale fast allow_busy", 1, -1, 1, Class::connection, transactions::watch},
    Spec{"unwatch", 1, "noscript loading stale fast allow_busy", 0, 0, 0, Class::connection, transactions::unwatch},
    // Scripting (ruling 20): a script runs whole under one hold of the lock; in
    // the cluster it reads and writes its keys as one command, by agreement.
    Spec{"eval", -3, "noscript stale skip_monitor may_replicate no_mandatory_keys", 0, 0, 0, Class::agreement, scripts::eval, nullptr, numkeys_second},
    Spec{"evalsha", -3, "noscript stale skip_monitor may_replicate no_mandatory_keys", 0, 0, 0, Class::agreement, scripts::evalsha, nullptr, numkeys_second},
    Spec{"eval_ro", -3, "readonly noscript stale skip_monitor no_mandatory_keys", 0, 0, 0, Class::read, scripts::eval_ro, nullptr, numkeys_second},
    Spec{"evalsha_ro", -3, "readonly noscript stale skip_monitor no_mandatory_keys", 0, 0, 0, Class::read, scripts::evalsha_ro, nullptr, numkeys_second},
    Spec{"script", -2, "noscript", 0, 0, 0, Class::admin, scripts::script},
    Spec{"fcall", -3, "noscript stale skip_monitor may_replicate no_mandatory_keys", 0, 0, 0, Class::agreement, scripts::fcall, nullptr, numkeys_second},
    Spec{"fcall_ro", -3, "readonly noscript stale skip_monitor no_mandatory_keys", 0, 0, 0, Class::read, scripts::fcall_ro, nullptr, numkeys_second},
    Spec{"function", -2, "noscript", 0, 0, 0, Class::admin, scripts::function},
    // Publish and subscribe: at most once, on this node and (in the cluster)
    // the nodes with announced interest.
    Spec{"subscribe", -2, "pubsub noscript loading stale", 0, 0, 0, Class::connection, pubsub::subscribe},
    Spec{"psubscribe", -2, "pubsub noscript loading stale", 0, 0, 0, Class::connection, pubsub::psubscribe},
    Spec{"ssubscribe", -2, "pubsub noscript loading stale", 1, -1, 1, Class::connection, pubsub::ssubscribe},
    Spec{"unsubscribe", -1, "pubsub noscript loading stale", 0, 0, 0, Class::connection, pubsub::unsubscribe},
    Spec{"punsubscribe", -1, "pubsub noscript loading stale", 0, 0, 0, Class::connection, pubsub::punsubscribe},
    Spec{"sunsubscribe", -1, "pubsub noscript loading stale", 1, -1, 1, Class::connection, pubsub::sunsubscribe},
    Spec{"publish", 3, "pubsub loading stale fast may_replicate", 0, 0, 0, Class::connection, pubsub::publish},
    Spec{"spublish", 3, "pubsub loading stale fast may_replicate", 1, 1, 1, Class::connection, pubsub::spublish},
    Spec{"pubsub", -2, "", 0, 0, 0, Class::connection, pubsub::pubsub},
    // The server's own: settings, introspection, replication's waits.
    Spec{"config", -2, "admin noscript loading stale", 0, 0, 0, Class::admin, server::config},
    Spec{"object", -2, "readonly", 2, 2, 1, Class::read, server::object},
    Spec{"role", 1, "noscript loading stale fast", 0, 0, 0, Class::connection, server::role},
    Spec{"wait", 3, "", 0, 0, 0, Class::connection, server::wait},
    Spec{"waitaof", 4, "noscript", 0, 0, 0, Class::connection, server::waitaof},
    Spec{"memory", -2, "readonly", 0, 0, 0, Class::read, server::memory},
    Spec{"slowlog", -2, "admin loading stale", 0, 0, 0, Class::admin, server::slowlog},
    Spec{"lastsave", 1, "loading stale fast", 0, 0, 0, Class::connection, server::lastsave},
    // Connection and server.
    Spec{"ping", -1, "fast", 0, 0, 0, Class::connection, connection::ping},
    Spec{"echo", 2, "fast", 0, 0, 0, Class::connection, connection::echo},
    Spec{"quit", -1, "noscript loading stale fast no_auth allow_busy", 0, 0, 0, Class::connection, connection::quit},
    Spec{"select", 2, "loading stale fast", 0, 0, 0, Class::connection, connection::select},
    Spec{"hello", -1, "noscript loading stale fast no_auth allow_busy", 0, 0, 0, Class::connection, connection::hello},
    Spec{"auth", -2, "noscript loading stale fast no_auth allow_busy", 0, 0, 0, Class::connection, connection::auth},
    Spec{"client", -2, "", 0, 0, 0, Class::connection, connection::client},
    Spec{"command", -1, "loading stale", 0, 0, 0, Class::connection, connection::command},
    Spec{"info", -1, "loading stale", 0, 0, 0, Class::connection, connection::info},
    Spec{"time", 1, "loading stale fast", 0, 0, 0, Class::connection, connection::time},
    Spec{"reset", 1, "noscript loading stale fast no_auth allow_busy", 0, 0, 0, Class::connection, connection::reset},
};

const std::unordered_map<std::string, const Spec*>& by_name() {
  static const auto* map = [] {
    auto* made = new std::unordered_map<std::string, const Spec*>();
    for (const Spec& spec : specs) made->emplace(std::string(spec.name), &spec);
    return made;
  }();
  return *map;
}

}  // namespace

std::string_view class_name(Class cls) noexcept {
  switch (cls) {
    case Class::read:
      return "read";
    case Class::merge:
      return "merge";
    case Class::agreement:
      return "agreement";
    case Class::configurable:
      return "configurable";
    case Class::connection:
      return "connection";
    case Class::admin:
      return "admin";
  }
  return "unknown";
}

const Spec* find_spec(std::string_view name) noexcept {
  if (name.size() > 64) return nullptr;
  const auto& map = by_name();
  auto it = map.find(lower(name));
  return it == map.end() ? nullptr : it->second;
}

std::span<const Spec> all_specs() noexcept { return specs; }

Class class_of(const Spec& spec, const std::vector<std::string>& args) { return spec.classify ? spec.classify(args) : spec.cls; }

std::vector<std::string_view> keys_of(const Spec& spec, const std::vector<std::string>& args) {
  if (spec.find_keys) return spec.find_keys(args);
  std::vector<std::string_view> keys;
  if (spec.first_key <= 0) return keys;
  const int count = static_cast<int>(args.size());
  const int last = spec.last_key < 0 ? count + spec.last_key : spec.last_key;
  for (int i = spec.first_key; i <= last && i < count; i += spec.step) keys.push_back(args[static_cast<size_t>(i)]);
  return keys;
}

}  // namespace mimir::command
