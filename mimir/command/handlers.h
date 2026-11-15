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
