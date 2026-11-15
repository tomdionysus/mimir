//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The hash commands, from Valkey 8's command documentation, and the hash's own
// rule: fields come back in the order they were first set, as Valkey returns
// a small hash's.

#include <gtest/gtest.h>

#include "mimir/command/table.h"
#include "mimir/keyspace/hash.h"
#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

const std::string wrongtype = error("WRONGTYPE Operation against a key holding the wrong kind of value");

std::string bulks(const std::vector<std::string>& items) {
  std::vector<std::string> out;
  for (const auto& item : items) out.push_back(bulk(item));
  return array(out);
}

TEST(KeyspaceHash, KeepsFirstSetOrderAndSurvivesACopy) {
  keyspace::Hash hash;
  EXPECT_TRUE(hash.set("b", "1"));
  EXPECT_TRUE(hash.set("a", "2"));
  EXPECT_TRUE(hash.set("c", "3"));
  EXPECT_FALSE(hash.set("b", "4"));
  EXPECT_TRUE(hash.erase("a"));
  EXPECT_FALSE(hash.erase("a"));
  keyspace::Hash copy = hash;
  hash.set("b", "changed");
  std::vector<std::string> order;
  for (const auto& [field, value] : copy) order.push_back(field + "=" + value);
  EXPECT_EQ(order, (std::vector<std::string>{"b=4", "c=3"}));
  ASSERT_NE(copy.get("c"), nullptr);
  EXPECT_EQ(*copy.get("c"), "3");
  keyspace::Hash moved = std::move(copy);
  EXPECT_EQ(*moved.get("b"), "4");
}

TEST(CommandHashes, SetGetAll) {
  Bed bed;
  EXPECT_EQ(bed.run({"HSET", "myhash", "field1", "Hello"}), integer(1));
  EXPECT_EQ(bed.run({"HSET", "myhash", "field2", "Hi", "field3", "World"}), integer(2));
  EXPECT_EQ(bed.run({"HSET", "myhash", "field1", "Hey"}), integer(0));
  EXPECT_EQ(bed.run({"HGET", "myhash", "field1"}), bulk("Hey"));
  EXPECT_EQ(bed.run({"HGET", "myhash", "nofield"}), nil());
  EXPECT_EQ(bed.run({"HGETALL", "myhash"}), bulks({"field1", "Hey", "field2", "Hi", "field3", "World"}));
  EXPECT_EQ(bed.run({"HKEYS", "myhash"}), bulks({"field1", "field2", "field3"}));
  EXPECT_EQ(bed.run({"HVALS", "myhash"}), bulks({"Hey", "Hi", "World"}));
  EXPECT_EQ(bed.run({"HLEN", "myhash"}), integer(3));
  EXPECT_EQ(bed.run({"HSET", "myhash", "f"}), error("ERR wrong number of arguments for 'hset' command"));
  EXPECT_EQ(bed.run({"HMSET", "h2", "a", "1", "b", "2"}), ok());
  EXPECT_EQ(bed.run({"HMGET", "h2", "a", "nofield", "b"}), array({bulk("1"), nil(), bulk("2")}));
  EXPECT_EQ(bed.run({"HGETALL", "nosuch"}), array({}));
  EXPECT_EQ(bed.run({"TYPE", "h2"}), "+hash\r\n");
}

TEST(CommandHashes, HGetAllIsAMapInResp3) {
  Bed bed;
  bed.run({"HSET", "h", "a", "1"});
  bed.run({"HELLO", "3"});
  EXPECT_EQ(bed.run({"HGETALL", "h"}), "%1\r\n" + bulk("a") + bulk("1"));
}

TEST(CommandHashes, SetNxDelExistsStrlen) {
  Bed bed;
  EXPECT_EQ(bed.run({"HSETNX", "myhash", "field", "Hello"}), integer(1));
  EXPECT_EQ(bed.run({"HSETNX", "myhash", "field", "World"}), integer(0));
  EXPECT_EQ(bed.run({"HGET", "myhash", "field"}), bulk("Hello"));
  EXPECT_EQ(bed.run({"HEXISTS", "myhash", "field"}), integer(1));
  EXPECT_EQ(bed.run({"HEXISTS", "myhash", "field2"}), integer(0));
  EXPECT_EQ(bed.run({"HSTRLEN", "myhash", "field"}), integer(5));
  EXPECT_EQ(bed.run({"HSTRLEN", "myhash", "nofield"}), integer(0));
  EXPECT_EQ(bed.run({"HDEL", "myhash", "field", "nofield"}), integer(1));
  EXPECT_EQ(bed.run({"EXISTS", "myhash"}), integer(0));
  EXPECT_EQ(bed.run({"HDEL", "nosuch", "f"}), integer(0));
}

TEST(CommandHashes, IncrBy) {
  Bed bed;
  bed.run({"HSET", "myhash", "field", "5"});
  EXPECT_EQ(bed.run({"HINCRBY", "myhash", "field", "1"}), integer(6));
  EXPECT_EQ(bed.run({"HINCRBY", "myhash", "field", "-1"}), integer(5));
  EXPECT_EQ(bed.run({"HINCRBY", "myhash", "field", "-10"}), integer(-5));
  EXPECT_EQ(bed.run({"HINCRBY", "myhash", "new", "3"}), integer(3));
  EXPECT_EQ(bed.run({"HINCRBY", "fresh", "f", "2"}), integer(2));
  bed.run({"HSET", "myhash", "text", "abc"});
  EXPECT_EQ(bed.run({"HINCRBY", "myhash", "text", "1"}), error("ERR hash value is not an integer"));
  EXPECT_EQ(bed.run({"HINCRBY", "myhash", "field", "x"}), error("ERR value is not an integer or out of range"));
  bed.run({"HSET", "myhash", "max", "9223372036854775807"});
  EXPECT_EQ(bed.run({"HINCRBY", "myhash", "max", "1"}), error("ERR increment or decrement would overflow"));
}

TEST(CommandHashes, IncrByFloat) {
  Bed bed;
  bed.run({"HSET", "mykey", "field", "10.50"});
  EXPECT_EQ(bed.run({"HINCRBYFLOAT", "mykey", "field", "0.1"}), bulk("10.6"));
  EXPECT_EQ(bed.run({"HINCRBYFLOAT", "mykey", "field", "-5"}), bulk("5.6"));
  bed.run({"HSET", "mykey", "field", "5.0e3"});
  EXPECT_EQ(bed.run({"HINCRBYFLOAT", "mykey", "field", "2.0e2"}), bulk("5200"));
  bed.run({"HSET", "mykey", "text", "abc"});
  EXPECT_EQ(bed.run({"HINCRBYFLOAT", "mykey", "text", "1"}), error("ERR hash value is not a float"));
  EXPECT_EQ(bed.run({"HINCRBYFLOAT", "mykey", "field", "x"}), error("ERR value is not a valid float"));
  EXPECT_EQ(bed.run({"HINCRBYFLOAT", "nosuch", "f", "x"}), error("ERR value is not a valid float"));
  EXPECT_EQ(bed.run({"EXISTS", "nosuch"}), integer(0));
}

TEST(CommandHashes, RandField) {
  Bed bed;
  EXPECT_EQ(bed.run({"HRANDFIELD", "nosuch"}), nil());
  EXPECT_EQ(bed.run({"HRANDFIELD", "nosuch", "3"}), array({}));
  bed.run({"HSET", "coin", "heads", "obverse", "tails", "reverse", "edge", "null"});
  const std::string one = bed.run({"HRANDFIELD", "coin"});
  EXPECT_TRUE(one == bulk("heads") || one == bulk("tails") || one == bulk("edge")) << one;
  EXPECT_EQ(bed.run({"HRANDFIELD", "coin", "0"}), array({}));
  EXPECT_EQ(bed.run({"HRANDFIELD", "coin", "5"}), bulks({"heads", "tails", "edge"}));
  EXPECT_EQ(bed.run({"HRANDFIELD", "coin", "5", "WITHVALUES"}), bulks({"heads", "obverse", "tails", "reverse", "edge", "null"}));
  const std::string repeats = bed.run({"HRANDFIELD", "coin", "-5"});
  EXPECT_EQ(repeats.substr(0, 4), "*5\r\n");
  const std::string two = bed.run({"HRANDFIELD", "coin", "2"});
  EXPECT_EQ(two.substr(0, 4), "*2\r\n");
  EXPECT_EQ(bed.run({"HRANDFIELD", "coin", "1", "BOGUS"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"HRANDFIELD", "coin", "x"}), error("ERR value is not an integer or out of range"));
}

TEST(CommandHashes, Scan) {
  Bed bed;
  bed.run({"HSET", "h", "name", "Jack", "age", "33", "nick", "J"});
  EXPECT_EQ(bed.run({"HSCAN", "h", "0"}), array({bulk("0"), bulks({"name", "Jack", "age", "33", "nick", "J"})}));
  EXPECT_EQ(bed.run({"HSCAN", "h", "0", "MATCH", "n*"}), array({bulk("0"), bulks({"name", "Jack", "nick", "J"})}));
  EXPECT_EQ(bed.run({"HSCAN", "h", "0", "NOVALUES"}), array({bulk("0"), bulks({"name", "age", "nick"})}));
  EXPECT_EQ(bed.run({"HSCAN", "h", "x"}), error("ERR invalid cursor"));
  EXPECT_EQ(bed.run({"HSCAN", "h", "0", "COUNT", "0"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"HSCAN", "nosuch", "0"}), array({bulk("0"), array({})}));
}

TEST(CommandHashes, TypesAreKeptApartAndCopiesAreDeep) {
  Bed bed;
  bed.run({"SET", "s", "v"});
  EXPECT_EQ(bed.run({"HSET", "s", "f", "v"}), wrongtype);
  EXPECT_EQ(bed.run({"HGET", "s", "f"}), wrongtype);
  bed.run({"HSET", "h", "f", "v"});
  EXPECT_EQ(bed.run({"GET", "h"}), wrongtype);
  EXPECT_EQ(bed.run({"LPUSH", "h", "x"}), wrongtype);
  EXPECT_EQ(bed.run({"COPY", "h", "h2"}), integer(1));
  bed.run({"HSET", "h", "g", "w"});
  EXPECT_EQ(bed.run({"HLEN", "h2"}), integer(1));
  EXPECT_EQ(bed.run({"HGETALL", "h2"}), bulks({"f", "v"}));
}

TEST(CommandHashes, Classes) {
  using command::Class;
  for (const char* name : {"hset", "hmset", "hdel"}) EXPECT_EQ(command::find_spec(name)->cls, Class::merge) << name;
  EXPECT_EQ(command::find_spec("hsetnx")->cls, Class::agreement);
  for (const char* name : {"hincrby", "hincrbyfloat"}) EXPECT_EQ(command::find_spec(name)->cls, Class::configurable) << name;
}

}  // namespace
}  // namespace mimir::test
