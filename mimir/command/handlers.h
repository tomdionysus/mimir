//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "mimir/command/table.h"

// The commands' handlers, one file per group, gathered by the table. Internal
// to the command subsystem.
namespace mimir::command {

// Shared by the groups.
// `key`'s value as a string, or null when absent; throws WRONGTYPE.
std::string* string_value(keyspace::Keyspace& keys, std::string_view key);
// An expiry argument made absolute, in Unix ms: `value` in seconds or ms,
// relative to now or not. Throws "ERR invalid expire time in '<command>'
// command" when it overflows, and, unless `allow_non_positive`, when value <= 0.
uint64_t absolute_expiry(int64_t value, bool seconds, bool relative, uint64_t now_ms, std::string_view command, bool allow_non_positive);
// A blocking command's timeout in seconds, as Valkey reads it: a non-negative
// number, zero for ever.
double block_timeout(std::string_view text);

namespace strings {
void get(Call&);
void set(Call&);
Class classify_set(const std::vector<std::string>&);
void setnx(Call&);
void setex(Call&);
void psetex(Call&);
void getset(Call&);
void getdel(Call&);
void getex(Call&);
void mget(Call&);
void mset(Call&);
void msetnx(Call&);
void incr(Call&);
void decr(Call&);
void incrby(Call&);
void decrby(Call&);
void incrbyfloat(Call&);
void append(Call&);
void strlen(Call&);
void getrange(Call&);
void setrange(Call&);
}  // namespace strings

namespace lists {
void lpush(Call&);
void rpush(Call&);
void lpushx(Call&);
void rpushx(Call&);
void lpop(Call&);
void rpop(Call&);
void llen(Call&);
void lrange(Call&);
void lindex(Call&);
void lset(Call&);
void linsert(Call&);
void lrem(Call&);
void ltrim(Call&);
void lpos(Call&);
void lmove(Call&);
void rpoplpush(Call&);
void lmpop(Call&);
void blpop(Call&);
void brpop(Call&);
void blmove(Call&);
void brpoplpush(Call&);
void blmpop(Call&);
}  // namespace lists

namespace hashes {
void hset(Call&);
void hmset(Call&);
void hsetnx(Call&);
void hget(Call&);
void hmget(Call&);
void hdel(Call&);
void hlen(Call&);
void hstrlen(Call&);
void hexists(Call&);
void hkeys(Call&);
void hvals(Call&);
void hgetall(Call&);
void hincrby(Call&);
void hincrbyfloat(Call&);
void hrandfield(Call&);
void hscan(Call&);
}  // namespace hashes

namespace sets {
void sadd(Call&);
void srem(Call&);
void smembers(Call&);
void sismember(Call&);
void smismember(Call&);
void scard(Call&);
void spop(Call&);
void srandmember(Call&);
void smove(Call&);
void sinter(Call&);
void sunion(Call&);
void sdiff(Call&);
void sinterstore(Call&);
void sunionstore(Call&);
void sdiffstore(Call&);
void sintercard(Call&);
void sscan(Call&);
}  // namespace sets

namespace zsets {
void zadd(Call&);
void zincrby(Call&);
void zscore(Call&);
void zmscore(Call&);
void zcard(Call&);
void zcount(Call&);
void zlexcount(Call&);
void zrange(Call&);
void zrangestore(Call&);
void zrevrange(Call&);
void zrangebyscore(Call&);
void zrevrangebyscore(Call&);
void zrangebylex(Call&);
void zrevrangebylex(Call&);
void zrank(Call&);
void zrevrank(Call&);
void zrem(Call&);
void zremrangebyrank(Call&);
void zremrangebyscore(Call&);
void zremrangebylex(Call&);
void zpopmin(Call&);
void zpopmax(Call&);
void zmpop(Call&);
void zrandmember(Call&);
void zscan(Call&);
void zunion(Call&);
void zinter(Call&);
void zdiff(Call&);
void zunionstore(Call&);
void zinterstore(Call&);
void zdiffstore(Call&);
void zintercard(Call&);
void bzpopmin(Call&);
void bzpopmax(Call&);
void bzmpop(Call&);
Class classify_zadd(const std::vector<std::string>&);
}  // namespace zsets

namespace generic {
void del(Call&);
void exists(Call&);
void type(Call&);
void expire(Call&);
void pexpire(Call&);
void expireat(Call&);
void pexpireat(Call&);
void ttl(Call&);
void pttl(Call&);
void expiretime(Call&);
void pexpiretime(Call&);
void persist(Call&);
void rename(Call&);
void renamenx(Call&);
void keys(Call&);
void scan(Call&);
void dbsize(Call&);
void flushall(Call&);
void randomkey(Call&);
void touch(Call&);
void copy(Call&);
}  // namespace generic

namespace connection {
void ping(Call&);
void echo(Call&);
void quit(Call&);
void select(Call&);
void hello(Call&);
void auth(Call&);
void client(Call&);
void command(Call&);
void info(Call&);
void time(Call&);
void reset(Call&);
}  // namespace connection

}  // namespace mimir::command
