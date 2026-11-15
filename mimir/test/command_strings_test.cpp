//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The string commands, from Valkey 8's command documentation: each command's
// documented replies, options and errors, its examples among them.

#include <gtest/gtest.h>

#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

using namespace std::chrono_literals;
const std::string wrongtype = error("WRONGTYPE Operation against a key holding the wrong kind of value");
const std::string not_integer = error("ERR value is not an integer or out of range");
const std::string syntax = error("ERR syntax error");

TEST(CommandStrings, GetAndSet) {
  Bed bed;
  EXPECT_EQ(bed.run({"GET", "nonexisting"}), nil());
  EXPECT_EQ(bed.run({"SET", "mykey", "Hello"}), ok());
  EXPECT_EQ(bed.run({"GET", "mykey"}), bulk("Hello"));
  EXPECT_EQ(bed.run({"set", "mykey", "World"}), ok());
  EXPECT_EQ(bed.run({"get", "mykey"}), bulk("World"));
}

TEST(CommandStrings, SetWithAnExpiry) {
  Bed bed;
  EXPECT_EQ(bed.run({"SET", "a", "v", "EX", "10"}), ok());
  EXPECT_EQ(bed.run({"TTL", "a"}), integer(10));
  EXPECT_EQ(bed.run({"SET", "b", "v", "PX", "1500"}), ok());
  EXPECT_EQ(bed.run({"PTTL", "b"}), integer(1500));
  EXPECT_EQ(bed.run({"SET", "c", "v", "EXAT", std::to_string(bed.wall.unix_ms() / 1000 + 100)}), ok());
  EXPECT_EQ(bed.run({"EXPIRETIME", "c"}), integer(static_cast<int64_t>(bed.wall.unix_ms() / 1000 + 100)));
  EXPECT_EQ(bed.run({"SET", "d", "v", "PXAT", std::to_string(bed.wall.unix_ms() + 5000)}), ok());
  EXPECT_EQ(bed.run({"PEXPIRETIME", "d"}), integer(static_cast<int64_t>(bed.wall.unix_ms() + 5000)));
  bed.wall.advance(1500ms);
  EXPECT_EQ(bed.run({"GET", "b"}), nil());
  EXPECT_EQ(bed.run({"GET", "a"}), bulk("v"));
}

TEST(CommandStrings, SetReplacesTheTtlUnlessKeepTtl) {
  Bed bed;
  bed.run({"SET", "k", "v", "EX", "100"});
  EXPECT_EQ(bed.run({"SET", "k", "w", "KEEPTTL"}), ok());
  EXPECT_EQ(bed.run({"TTL", "k"}), integer(100));
  EXPECT_EQ(bed.run({"SET", "k", "x"}), ok());
  EXPECT_EQ(bed.run({"TTL", "k"}), integer(-1));
}

TEST(CommandStrings, SetNxAndXx) {
  Bed bed;
  EXPECT_EQ(bed.run({"SET", "k", "1", "XX"}), nil());
  EXPECT_EQ(bed.run({"EXISTS", "k"}), integer(0));
  EXPECT_EQ(bed.run({"SET", "k", "1", "NX"}), ok());
  EXPECT_EQ(bed.run({"SET", "k", "2", "NX"}), nil());
  EXPECT_EQ(bed.run({"GET", "k"}), bulk("1"));
  EXPECT_EQ(bed.run({"SET", "k", "3", "XX"}), ok());
  EXPECT_EQ(bed.run({"GET", "k"}), bulk("3"));
}

TEST(CommandStrings, SetGetReturnsTheOldValue) {
  Bed bed;
  EXPECT_EQ(bed.run({"SET", "k", "1", "GET"}), nil());
  EXPECT_EQ(bed.run({"SET", "k", "2", "GET"}), bulk("1"));
  // NX with GET: the old value, and nothing set.
  EXPECT_EQ(bed.run({"SET", "k", "3", "NX", "GET"}), bulk("2"));
  EXPECT_EQ(bed.run({"GET", "k"}), bulk("2"));
  EXPECT_EQ(bed.run({"SET", "k", "4", "XX", "GET"}), bulk("2"));
  EXPECT_EQ(bed.run({"GET", "k"}), bulk("4"));
}

TEST(CommandStrings, SetRefusesConflictingOptions) {
  Bed bed;
  EXPECT_EQ(bed.run({"SET", "k", "v", "NX", "XX"}), syntax);
  EXPECT_EQ(bed.run({"SET", "k", "v", "EX", "10", "PX", "100"}), syntax);
  EXPECT_EQ(bed.run({"SET", "k", "v", "KEEPTTL", "EX", "10"}), syntax);
  EXPECT_EQ(bed.run({"SET", "k", "v", "EX"}), syntax);
  EXPECT_EQ(bed.run({"SET", "k", "v", "BOGUS"}), syntax);
  EXPECT_EQ(bed.run({"EXISTS", "k"}), integer(0));
}

TEST(CommandStrings, SetRefusesABadExpiry) {
  Bed bed;
  EXPECT_EQ(bed.run({"SET", "k", "v", "EX", "0"}), error("ERR invalid expire time in 'set' command"));
  EXPECT_EQ(bed.run({"SET", "k", "v", "PX", "-5"}), error("ERR invalid expire time in 'set' command"));
  EXPECT_EQ(bed.run({"SET", "k", "v", "EX", "9223372036854775807"}), error("ERR invalid expire time in 'set' command"));
  EXPECT_EQ(bed.run({"SET", "k", "v", "EX", "ten"}), not_integer);
  EXPECT_EQ(bed.run({"EXISTS", "k"}), integer(0));
}

TEST(CommandStrings, SetAtATimeAlreadyPastLeavesNoKey) {
  Bed bed;
  bed.run({"SET", "k", "old"});
  EXPECT_EQ(bed.run({"SET", "k", "v", "PXAT", "1"}), ok());
  EXPECT_EQ(bed.run({"EXISTS", "k"}), integer(0));
}

TEST(CommandStrings, SetNx) {
  Bed bed;
  EXPECT_EQ(bed.run({"SETNX", "mykey", "Hello"}), integer(1));
  EXPECT_EQ(bed.run({"SETNX", "mykey", "World"}), integer(0));
  EXPECT_EQ(bed.run({"GET", "mykey"}), bulk("Hello"));
}

TEST(CommandStrings, SetExAndPSetEx) {
  Bed bed;
  EXPECT_EQ(bed.run({"SETEX", "mykey", "10", "Hello"}), ok());
  EXPECT_EQ(bed.run({"TTL", "mykey"}), integer(10));
  EXPECT_EQ(bed.run({"PSETEX", "mykey", "1000", "Hello"}), ok());
  EXPECT_EQ(bed.run({"PTTL", "mykey"}), integer(1000));
  EXPECT_EQ(bed.run({"SETEX", "k", "0", "v"}), error("ERR invalid expire time in 'setex' command"));
  EXPECT_EQ(bed.run({"PSETEX", "k", "-1", "v"}), error("ERR invalid expire time in 'psetex' command"));
  EXPECT_EQ(bed.run({"SETEX", "k", "x", "v"}), not_integer);
}

TEST(CommandStrings, GetSetGetDel) {
  Bed bed;
  bed.run({"SET", "mykey", "Hello", "EX", "100"});
  EXPECT_EQ(bed.run({"GETSET", "mykey", "World"}), bulk("Hello"));
  EXPECT_EQ(bed.run({"TTL", "mykey"}), integer(-1));
  EXPECT_EQ(bed.run({"GETDEL", "mykey"}), bulk("World"));
  EXPECT_EQ(bed.run({"GET", "mykey"}), nil());
  EXPECT_EQ(bed.run({"GETSET", "new", "v"}), nil());
  EXPECT_EQ(bed.run({"GETDEL", "missing"}), nil());
}

TEST(CommandStrings, GetEx) {
  Bed bed;
  bed.run({"SET", "mykey", "Hello"});
  EXPECT_EQ(bed.run({"GETEX", "mykey"}), bulk("Hello"));
  EXPECT_EQ(bed.run({"TTL", "mykey"}), integer(-1));
  EXPECT_EQ(bed.run({"GETEX", "mykey", "EX", "60"}), bulk("Hello"));
  EXPECT_EQ(bed.run({"TTL", "mykey"}), integer(60));
  EXPECT_EQ(bed.run({"GETEX", "mykey", "PERSIST"}), bulk("Hello"));
  EXPECT_EQ(bed.run({"TTL", "mykey"}), integer(-1));
  EXPECT_EQ(bed.run({"GETEX", "mykey", "PERSIST", "EX", "1"}), syntax);
  EXPECT_EQ(bed.run({"GETEX", "mykey", "EX", "0"}), error("ERR invalid expire time in 'getex' command"));
  EXPECT_EQ(bed.run({"GETEX", "missing", "EX", "10"}), nil());
  EXPECT_EQ(bed.run({"GETEX", "mykey", "PXAT", "1"}), bulk("Hello"));
  EXPECT_EQ(bed.run({"EXISTS", "mykey"}), integer(0));
}

TEST(CommandStrings, MGetMSetMSetNx) {
  Bed bed;
  EXPECT_EQ(bed.run({"MSET", "key1", "Hello", "key2", "World"}), ok());
  EXPECT_EQ(bed.run({"MGET", "key1", "key2", "nonexisting"}), array({bulk("Hello"), bulk("World"), nil()}));
  EXPECT_EQ(bed.run({"MSET", "a", "1", "b"}), error("ERR wrong number of arguments for 'mset' command"));
  EXPECT_EQ(bed.run({"MSETNX", "key3", "x", "key1", "y"}), integer(0));
  EXPECT_EQ(bed.run({"EXISTS", "key3"}), integer(0));
  EXPECT_EQ(bed.run({"MSETNX", "key3", "x", "key4", "y"}), integer(1));
  EXPECT_EQ(bed.run({"MGET", "key3", "key4"}), array({bulk("x"), bulk("y")}));
}

TEST(CommandStrings, IncrAndDecr) {
  Bed bed;
  bed.run({"SET", "mykey", "10"});
  EXPECT_EQ(bed.run({"INCR", "mykey"}), integer(11));
  EXPECT_EQ(bed.run({"GET", "mykey"}), bulk("11"));
  EXPECT_EQ(bed.run({"DECR", "mykey"}), integer(10));
  EXPECT_EQ(bed.run({"INCRBY", "mykey", "5"}), integer(15));
  EXPECT_EQ(bed.run({"DECRBY", "mykey", "20"}), integer(-5));
  EXPECT_EQ(bed.run({"INCR", "fresh"}), integer(1));
  EXPECT_EQ(bed.run({"DECR", "fresh2"}), integer(-1));
}

TEST(CommandStrings, IncrKeepsTheTtl) {
  Bed bed;
  bed.run({"SET", "k", "1", "EX", "100"});
  bed.run({"INCR", "k"});
  EXPECT_EQ(bed.run({"TTL", "k"}), integer(100));
}

TEST(CommandStrings, IncrRefusesWhatIsNotAnIntegerAndOverflow) {
  Bed bed;
  bed.run({"SET", "s", "234293482390480948029348230948"});
  EXPECT_EQ(bed.run({"INCR", "s"}), not_integer);
  bed.run({"SET", "s", " 1"});
  EXPECT_EQ(bed.run({"INCR", "s"}), not_integer);
  bed.run({"SET", "s", "01"});
  EXPECT_EQ(bed.run({"INCR", "s"}), not_integer);
  bed.run({"SET", "max", "9223372036854775807"});
  EXPECT_EQ(bed.run({"INCR", "max"}), error("ERR increment or decrement would overflow"));
  bed.run({"SET", "min", "-9223372036854775808"});
  EXPECT_EQ(bed.run({"DECR", "min"}), error("ERR increment or decrement would overflow"));
  EXPECT_EQ(bed.run({"DECRBY", "x", "-9223372036854775808"}), error("ERR decrement would overflow"));
  EXPECT_EQ(bed.run({"INCRBY", "x", "1.5"}), not_integer);
}

TEST(CommandStrings, IncrByFloat) {
  Bed bed;
  bed.run({"SET", "mykey", "10.50"});
  EXPECT_EQ(bed.run({"INCRBYFLOAT", "mykey", "0.1"}), bulk("10.6"));
  EXPECT_EQ(bed.run({"INCRBYFLOAT", "mykey", "-5"}), bulk("5.6"));
  bed.run({"SET", "mykey", "5.0e3"});
  EXPECT_EQ(bed.run({"INCRBYFLOAT", "mykey", "2.0e2"}), bulk("5200"));
  EXPECT_EQ(bed.run({"INCRBYFLOAT", "mykey", "x"}), error("ERR value is not a valid float"));
  EXPECT_EQ(bed.run({"INCRBYFLOAT", "mykey", "inf"}), error("ERR increment would produce NaN or Infinity"));
  bed.run({"SET", "bad", "abc"});
  EXPECT_EQ(bed.run({"INCRBYFLOAT", "bad", "1"}), error("ERR value is not a valid float"));
}

TEST(CommandStrings, AppendAndStrlen) {
  Bed bed;
  EXPECT_EQ(bed.run({"EXISTS", "mykey"}), integer(0));
  EXPECT_EQ(bed.run({"APPEND", "mykey", "Hello"}), integer(5));
  EXPECT_EQ(bed.run({"APPEND", "mykey", " World"}), integer(11));
  EXPECT_EQ(bed.run({"GET", "mykey"}), bulk("Hello World"));
  EXPECT_EQ(bed.run({"STRLEN", "mykey"}), integer(11));
  EXPECT_EQ(bed.run({"STRLEN", "nonexisting"}), integer(0));
}

TEST(CommandStrings, GetRange) {
  Bed bed;
  bed.run({"SET", "mykey", "This is a string"});
  EXPECT_EQ(bed.run({"GETRANGE", "mykey", "0", "3"}), bulk("This"));
  EXPECT_EQ(bed.run({"GETRANGE", "mykey", "-3", "-1"}), bulk("ing"));
  EXPECT_EQ(bed.run({"GETRANGE", "mykey", "0", "-1"}), bulk("This is a string"));
  EXPECT_EQ(bed.run({"GETRANGE", "mykey", "10", "100"}), bulk("string"));
  EXPECT_EQ(bed.run({"GETRANGE", "mykey", "-1", "-5"}), bulk(""));
  EXPECT_EQ(bed.run({"GETRANGE", "mykey", "5", "3"}), bulk(""));
  EXPECT_EQ(bed.run({"GETRANGE", "missing", "0", "-1"}), bulk(""));
  EXPECT_EQ(bed.run({"SUBSTR", "mykey", "0", "3"}), bulk("This"));
}

TEST(CommandStrings, SetRange) {
  Bed bed;
  bed.run({"SET", "key1", "Hello World"});
  EXPECT_EQ(bed.run({"SETRANGE", "key1", "6", "Redis"}), integer(11));
  EXPECT_EQ(bed.run({"GET", "key1"}), bulk("Hello Redis"));
  EXPECT_EQ(bed.run({"SETRANGE", "key2", "6", "Redis"}), integer(11));
  EXPECT_EQ(bed.run({"GET", "key2"}), bulk(std::string(6, '\0') + "Redis"));
  EXPECT_EQ(bed.run({"SETRANGE", "key3", "0", ""}), integer(0));
  EXPECT_EQ(bed.run({"EXISTS", "key3"}), integer(0));
  EXPECT_EQ(bed.run({"SETRANGE", "key1", "-1", "x"}), error("ERR offset is out of range"));
  EXPECT_EQ(bed.run({"SETRANGE", "key1", "536870912", "x"}), error("ERR string exceeds maximum allowed size (proto-max-bulk-len)"));
}

TEST(CommandStrings, AnUnknownCommandAndAWrongArity) {
  Bed bed;
  EXPECT_EQ(bed.run({"foo", "a", "b"}), error("ERR unknown command 'foo', with args beginning with: 'a' 'b' "));
  EXPECT_EQ(bed.run({"GET"}), error("ERR wrong number of arguments for 'get' command"));
  EXPECT_EQ(bed.run({"GET", "a", "b"}), error("ERR wrong number of arguments for 'get' command"));
  EXPECT_EQ(bed.run({"SET", "a"}), error("ERR wrong number of arguments for 'set' command"));
}

}  // namespace
}  // namespace mimir::test
