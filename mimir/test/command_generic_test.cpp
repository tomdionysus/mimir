//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The generic key commands and the connection commands, from Valkey 8's
// command documentation, and the command table's classes from the design
// ("The two classes of command").

#include <gtest/gtest.h>

#include "mimir/command/table.h"
#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

using namespace std::chrono_literals;

TEST(CommandGeneric, DelExistsTouchType) {
  Bed bed;
  bed.run({"SET", "key1", "Hello"});
  bed.run({"SET", "key2", "World"});
  EXPECT_EQ(bed.run({"EXISTS", "key1"}), integer(1));
  EXPECT_EQ(bed.run({"EXISTS", "key1", "key1", "nosuchkey"}), integer(2));
  EXPECT_EQ(bed.run({"TOUCH", "key1", "key2", "x"}), integer(2));
  EXPECT_EQ(bed.run({"TYPE", "key1"}), "+string\r\n");
  EXPECT_EQ(bed.run({"TYPE", "nosuchkey"}), "+none\r\n");
  EXPECT_EQ(bed.run({"DEL", "key1", "key2", "key3"}), integer(2));
  bed.run({"SET", "key1", "x"});
  EXPECT_EQ(bed.run({"UNLINK", "key1", "key1"}), integer(1));
}

TEST(CommandGeneric, ExpireTtlPersist) {
  Bed bed;
  bed.run({"SET", "mykey", "Hello"});
  EXPECT_EQ(bed.run({"EXPIRE", "mykey", "10"}), integer(1));
  EXPECT_EQ(bed.run({"TTL", "mykey"}), integer(10));
  bed.run({"SET", "mykey", "Hello World"});
  EXPECT_EQ(bed.run({"TTL", "mykey"}), integer(-1));
  EXPECT_EQ(bed.run({"EXPIRE", "mykey", "10", "XX"}), integer(0));
  EXPECT_EQ(bed.run({"EXPIRE", "mykey", "10", "NX"}), integer(1));
  EXPECT_EQ(bed.run({"PERSIST", "mykey"}), integer(1));
  EXPECT_EQ(bed.run({"PERSIST", "mykey"}), integer(0));
  EXPECT_EQ(bed.run({"TTL", "missing"}), integer(-2));
  EXPECT_EQ(bed.run({"EXPIRE", "missing", "10"}), integer(0));
}

TEST(CommandGeneric, TtlRoundsToTheNearestSecond) {
  Bed bed;
  bed.run({"SET", "k", "v", "PX", "2600"});
  EXPECT_EQ(bed.run({"TTL", "k"}), integer(3));
  bed.wall.advance(200ms);
  EXPECT_EQ(bed.run({"TTL", "k"}), integer(2));
  EXPECT_EQ(bed.run({"PTTL", "k"}), integer(2400));
}

TEST(CommandGeneric, ExpireGtAndLt) {
  Bed bed;
  bed.run({"SET", "k", "v"});
  // No expiry is infinite: GT never beats it, LT always does.
  EXPECT_EQ(bed.run({"EXPIRE", "k", "100", "GT"}), integer(0));
  EXPECT_EQ(bed.run({"EXPIRE", "k", "100", "LT"}), integer(1));
  EXPECT_EQ(bed.run({"EXPIRE", "k", "50", "GT"}), integer(0));
  EXPECT_EQ(bed.run({"EXPIRE", "k", "200", "GT"}), integer(1));
  EXPECT_EQ(bed.run({"EXPIRE", "k", "300", "LT"}), integer(0));
  EXPECT_EQ(bed.run({"TTL", "k"}), integer(200));
  EXPECT_EQ(bed.run({"EXPIRE", "k", "1", "NX", "XX"}), error("ERR NX and XX, GT or LT options at the same time are not compatible"));
  EXPECT_EQ(bed.run({"EXPIRE", "k", "1", "GT", "LT"}), error("ERR GT and LT options at the same time are not compatible"));
  EXPECT_EQ(bed.run({"EXPIRE", "k", "1", "FOO"}), error("ERR Unsupported option FOO"));
}

TEST(CommandGeneric, ANonPositiveExpiryDeletes) {
  Bed bed;
  bed.run({"SET", "k", "v"});
  EXPECT_EQ(bed.run({"EXPIRE", "k", "-1"}), integer(1));
  EXPECT_EQ(bed.run({"EXISTS", "k"}), integer(0));
  bed.run({"SET", "k", "v"});
  EXPECT_EQ(bed.run({"PEXPIREAT", "k", "0"}), integer(1));
  EXPECT_EQ(bed.run({"EXISTS", "k"}), integer(0));
  EXPECT_EQ(bed.run({"EXPIRE", "k", "9223372036854775807"}), error("ERR invalid expire time in 'expire' command"));
}

TEST(CommandGeneric, ExpireAtAndExpireTime) {
  Bed bed;
  bed.run({"SET", "mykey", "Hello"});
  const uint64_t at = bed.wall.unix_ms() / 1000 + 1000;
  EXPECT_EQ(bed.run({"EXPIREAT", "mykey", std::to_string(at)}), integer(1));
  EXPECT_EQ(bed.run({"EXPIRETIME", "mykey"}), integer(static_cast<int64_t>(at)));
  EXPECT_EQ(bed.run({"PEXPIRETIME", "mykey"}), integer(static_cast<int64_t>(at * 1000)));
  EXPECT_EQ(bed.run({"EXPIRETIME", "missing"}), integer(-2));
  bed.run({"SET", "p", "v"});
  EXPECT_EQ(bed.run({"EXPIRETIME", "p"}), integer(-1));
}

TEST(CommandGeneric, RenameAndRenameNx) {
  Bed bed;
  bed.run({"SET", "mykey", "Hello", "EX", "100"});
  EXPECT_EQ(bed.run({"RENAME", "mykey", "myotherkey"}), ok());
  EXPECT_EQ(bed.run({"GET", "myotherkey"}), bulk("Hello"));
  EXPECT_EQ(bed.run({"TTL", "myotherkey"}), integer(100));
  EXPECT_EQ(bed.run({"RENAME", "nosuch", "x"}), error("ERR no such key"));
  EXPECT_EQ(bed.run({"RENAME", "myotherkey", "myotherkey"}), ok());
  bed.run({"SET", "other", "World"});
  EXPECT_EQ(bed.run({"RENAMENX", "myotherkey", "other"}), integer(0));
  EXPECT_EQ(bed.run({"RENAMENX", "myotherkey", "third"}), integer(1));
  EXPECT_EQ(bed.run({"GET", "third"}), bulk("Hello"));
}

TEST(CommandGeneric, Copy) {
  Bed bed;
  bed.run({"SET", "dolly", "sheep"});
  EXPECT_EQ(bed.run({"COPY", "dolly", "clone"}), integer(1));
  EXPECT_EQ(bed.run({"GET", "clone"}), bulk("sheep"));
  EXPECT_EQ(bed.run({"COPY", "dolly", "clone"}), integer(0));
  bed.run({"SET", "dolly", "lamb"});
  EXPECT_EQ(bed.run({"COPY", "dolly", "clone", "REPLACE"}), integer(1));
  EXPECT_EQ(bed.run({"GET", "clone"}), bulk("lamb"));
  EXPECT_EQ(bed.run({"COPY", "dolly", "x", "DB", "1"}), error("ERR DB index is out of range"));
  EXPECT_EQ(bed.run({"COPY", "nosuch", "x"}), integer(0));
}

TEST(CommandGeneric, KeysAndDbSize) {
  Bed bed;
  bed.run({"MSET", "firstname", "Jack", "lastname", "Stuntman", "age", "35"});
  const std::string names = bed.run({"KEYS", "*name*"});
  EXPECT_TRUE(names == array({bulk("firstname"), bulk("lastname")}) || names == array({bulk("lastname"), bulk("firstname")})) << names;
  EXPECT_EQ(bed.run({"KEYS", "a??"}), array({bulk("age")}));
  EXPECT_EQ(bed.run({"DBSIZE"}), integer(3));
  EXPECT_EQ(bed.run({"FLUSHALL"}), ok());
  EXPECT_EQ(bed.run({"DBSIZE"}), integer(0));
  EXPECT_EQ(bed.run({"FLUSHDB", "BOGUS"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"RANDOMKEY"}), nil());
  bed.run({"SET", "only", "1"});
  EXPECT_EQ(bed.run({"RANDOMKEY"}), bulk("only"));
}

TEST(CommandGeneric, ScanWalksEverythingAndRefusesBadArguments) {
  Bed bed;
  for (int i = 0; i < 50; ++i) bed.run({"SET", "key:" + std::to_string(i), "v"});
  EXPECT_EQ(bed.run({"SCAN", "x"}), error("ERR invalid cursor"));
  EXPECT_EQ(bed.run({"SCAN", "0", "COUNT", "0"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"SCAN", "0", "TYPE", "foo"}), error("ERR unknown type name 'foo'"));
  EXPECT_EQ(bed.run({"SCAN", "0", "BOGUS"}), error("ERR syntax error"));
  const std::string all = bed.run({"SCAN", "0", "COUNT", "1000", "MATCH", "key:1*"});
  // key:1 and key:10 to key:19.
  EXPECT_EQ(all.substr(0, 9), "*2\r\n$1\r\n0") << all;
  EXPECT_NE(all.find("*11\r\n"), std::string::npos) << all;
}

TEST(CommandConnection, PingEchoSelectTime) {
  Bed bed;
  EXPECT_EQ(bed.run({"PING"}), "+PONG\r\n");
  EXPECT_EQ(bed.run({"PING", "hello world"}), bulk("hello world"));
  EXPECT_EQ(bed.run({"PING", "a", "b"}), error("ERR wrong number of arguments for 'ping' command"));
  EXPECT_EQ(bed.run({"ECHO", "Hello World!"}), bulk("Hello World!"));
  EXPECT_EQ(bed.run({"SELECT", "0"}), ok());
  EXPECT_EQ(bed.run({"SELECT", "1"}), error("ERR DB index is out of range"));
  EXPECT_EQ(bed.run({"SELECT", "x"}), error("ERR value is not an integer or out of range"));
  const uint64_t now = bed.wall.unix_ms();
  EXPECT_EQ(bed.run({"TIME"}), array({bulk(std::to_string(now / 1000)), bulk(std::to_string((now % 1000) * 1000))}));
}

TEST(CommandConnection, HelloSwitchesTheProtocol) {
  Bed bed;
  const std::string three = bed.run({"HELLO", "3"});
  EXPECT_EQ(three.substr(0, 4), "%7\r\n");
  EXPECT_NE(three.find("$5\r\nproto\r\n:3\r\n"), std::string::npos);
  EXPECT_EQ(bed.run({"GET", "missing"}), "_\r\n");
  const std::string two = bed.run({"HELLO", "2"});
  EXPECT_EQ(two.substr(0, 5), "*14\r\n");
  EXPECT_EQ(bed.run({"GET", "missing"}), nil());
  EXPECT_EQ(bed.run({"HELLO", "4"}), error("NOPROTO unsupported protocol version"));
  EXPECT_EQ(bed.run({"HELLO", "x"}), error("ERR Protocol version is not an integer or out of range"));
  bed.run({"HELLO", "2", "SETNAME", "myname"});
  EXPECT_EQ(bed.run({"CLIENT", "GETNAME"}), bulk("myname"));
}

TEST(CommandConnection, ClientNamesAndInfo) {
  Bed bed;
  EXPECT_EQ(bed.run({"CLIENT", "GETNAME"}), nil());
  EXPECT_EQ(bed.run({"CLIENT", "SETNAME", "connection-1"}), ok());
  EXPECT_EQ(bed.run({"CLIENT", "GETNAME"}), bulk("connection-1"));
  EXPECT_EQ(bed.run({"CLIENT", "SETNAME", "has space"}), error("ERR Client names cannot contain spaces, newlines or special characters."));
  EXPECT_EQ(bed.run({"CLIENT", "ID"}), integer(bed.session.id));
  EXPECT_EQ(bed.run({"CLIENT", "SETINFO", "LIB-NAME", "redis-py"}), ok());
  EXPECT_EQ(bed.run({"CLIENT", "SETINFO", "LIB-VER", "5.0.0"}), ok());
  EXPECT_EQ(bed.run({"CLIENT", "SETINFO", "COLOUR", "x"}), error("ERR Unrecognized option 'COLOUR'"));
  EXPECT_EQ(bed.run({"CLIENT", "foo"}), error("ERR unknown subcommand 'foo'. Try CLIENT HELP."));
}

TEST(CommandConnection, QuitClosesAfterItsReply) {
  Bed bed;
  EXPECT_EQ(bed.run({"QUIT"}), ok());
  EXPECT_TRUE(bed.session.closing);
}

TEST(CommandConnection, CommandCountsAndDescribes) {
  Bed bed;
  EXPECT_EQ(bed.run({"COMMAND", "COUNT"}), integer(static_cast<int64_t>(command::all_specs().size())));
  const std::string get = bed.run({"COMMAND", "INFO", "get", "nosuch"});
  EXPECT_EQ(get.substr(0, 4 + 5 + 9), "*2\r\n*10\r\n$3\r\nget\r\n");
  EXPECT_NE(get.find(":2\r\n*2\r\n+readonly\r\n+fast\r\n:1\r\n:1\r\n:1\r\n"), std::string::npos) << get;
  // An unknown name is a null bulk string (the differential gate's finding).
  EXPECT_EQ(get.substr(get.size() - 5), "$-1\r\n");
}

TEST(CommandTable, EachCommandHasTheClassTheRulingsGiveIt) {
  using command::Class;
  const auto cls = [](std::vector<std::string> args) { return command::class_of(*command::find_spec(args[0]), args); };
  // Ruling 1: what depends on the value before it commits by agreement.
  EXPECT_EQ(cls({"SET", "k", "v", "NX"}), Class::agreement);
  EXPECT_EQ(cls({"SET", "k", "v", "GET"}), Class::agreement);
  EXPECT_EQ(cls({"SETNX", "k", "v"}), Class::agreement);
  EXPECT_EQ(cls({"GETDEL", "k"}), Class::agreement);
  EXPECT_EQ(cls({"EXPIRE", "k", "1", "GT"}), Class::agreement);
  EXPECT_EQ(cls({"RENAME", "a", "b"}), Class::agreement);
  // Ruling 2: what merges.
  EXPECT_EQ(cls({"SET", "k", "v"}), Class::merge);
  EXPECT_EQ(cls({"SET", "k", "v", "EX", "1"}), Class::merge);
  EXPECT_EQ(cls({"DEL", "k"}), Class::merge);
  EXPECT_EQ(cls({"EXPIRE", "k", "1"}), Class::merge);
  // Ruling 3: read-modify-write is configurable.
  for (const char* name : {"INCR", "DECR", "INCRBY", "DECRBY", "INCRBYFLOAT", "APPEND"}) {
    EXPECT_EQ(command::find_spec(name)->cls, Class::configurable) << name;
  }
  EXPECT_EQ(cls({"GET", "k"}), Class::read);
}

TEST(CommandTable, KeysAreFoundByPosition) {
  const auto* mset = command::find_spec("mset");
  std::vector<std::string> args{"MSET", "a", "1", "b", "2"};
  EXPECT_EQ(command::keys_of(*mset, args), (std::vector<std::string_view>{"a", "b"}));
  const auto* rename = command::find_spec("RENAME");
  std::vector<std::string> two{"RENAME", "x", "y"};
  EXPECT_EQ(command::keys_of(*rename, two), (std::vector<std::string_view>{"x", "y"}));
  EXPECT_TRUE(command::keys_of(*command::find_spec("ping"), {"PING"}).empty());
}

}  // namespace
}  // namespace mimir::test
