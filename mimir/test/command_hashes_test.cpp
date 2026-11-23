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

// Valkey 8.1.1's hash-max-listpack-entries is 512 (probed): a hash of 200
// fields is still a listpack, in OBJECT ENCODING and in DUMP's type byte
// (0x10, a listpack hash; 0x04, a hash table); 513 is a hash table.
TEST(CommandHashes, AListpackHoldsUpTo512Fields) {
  Bed bed;
  EXPECT_EQ(bed.run({"CONFIG", "GET", "hash-max-listpack-entries"}), array({bulk("hash-max-listpack-entries"), bulk("512")}));
  std::vector<std::string> hset{"HSET", "h"};
  for (int i = 1; i <= 200; ++i) {
    hset.push_back("f" + std::to_string(i));
    hset.push_back("v");
  }
  bed.run(hset);
  EXPECT_EQ(bed.run({"OBJECT", "ENCODING", "h"}), bulk("listpack"));
  const auto type_byte = [&bed](const std::string& key) {
    const std::string dumped = bed.run({"DUMP", key});
    return static_cast<int>(static_cast<unsigned char>(dumped[dumped.find("\r\n") + 2]));
  };
  EXPECT_EQ(type_byte("h"), 0x10);
  for (int i = 201; i <= 513; ++i) bed.run({"HSET", "h", "f" + std::to_string(i), "v"});
  EXPECT_EQ(bed.run({"OBJECT", "ENCODING", "h"}), bulk("hashtable"));
  EXPECT_EQ(type_byte("h"), 0x04);
}

// A hash's encoding as Valkey 8.1.1's changes (probed): a write takes it to a
// hash table past the agreed limits, and it stays one; a lowered limit
// converts on the next write, never on a read; a client's RESTORE loads a
// payload in the encoding its size gives, whatever it was dumped as.
TEST(CommandHashes, EncodingsChangeAsValkeysDo) {
  Bed bed;
  const auto encoding = [&bed](const std::string& key) { return bed.run({"OBJECT", "ENCODING", key}); };
  const auto payload = [&bed](const std::string& key) {
    const std::string dumped = bed.run({"DUMP", key});
    const size_t at = dumped.find("\r\n") + 2;
    return dumped.substr(at, dumped.size() - at - 2);
  };
  const std::string table = bulk("hashtable");
  const std::string listpack = bulk("listpack");
  std::vector<std::string> hset{"HSET", "h"};
  for (int i = 1; i <= 513; ++i) {
    hset.push_back("f" + std::to_string(i));
    hset.push_back("v");
  }
  bed.run(hset);
  EXPECT_EQ(encoding("h"), table);
  std::vector<std::string> hdel{"HDEL", "h"};
  for (int i = 1; i <= 512; ++i) hdel.push_back("f" + std::to_string(i));
  bed.run(hdel);
  EXPECT_EQ(encoding("h"), table);
  EXPECT_EQ(static_cast<int>(static_cast<unsigned char>(payload("h")[0])), 0x04);
  const std::string small_table = payload("h");

  bed.run({"HSET", "long_value", "a", std::string(65, 'x')});
  EXPECT_EQ(encoding("long_value"), table);
  bed.run({"HSET", "long_field", std::string(65, 'x'), "x"});
  EXPECT_EQ(encoding("long_field"), table);
  bed.run({"HSET", "float", "a", "1"});
  bed.run({"HINCRBYFLOAT", "float", "a", "1.5"});
  EXPECT_EQ(encoding("float"), listpack);
  bed.run({"HSET", "nx", "a", "1"});
  bed.run({"HSETNX", "nx", "b", std::string(70, 'x')});
  EXPECT_EQ(encoding("nx"), table);

  // A client's RESTORE: a small hash table loads as a listpack.
  EXPECT_EQ(bed.run({"RESTORE", "restored", "0", small_table}), ok());
  EXPECT_EQ(encoding("restored"), listpack);

  // A lowered limit: a hash already past it stays a listpack until a write.
  bed.run({"HSET", "three", "a", "1", "b", "2", "c", "3"});
  const std::string three = payload("three");
  EXPECT_EQ(bed.run({"CONFIG", "SET", "hash-max-listpack-entries", "2"}), ok());
  EXPECT_EQ(encoding("three"), listpack);
  bed.run({"HGET", "three", "a"});
  EXPECT_EQ(encoding("three"), listpack);
  bed.run({"HSET", "three", "a", "9"});
  EXPECT_EQ(encoding("three"), table);
  bed.run({"HSET", "fresh", "a", "1", "b", "2", "c", "3"});
  EXPECT_EQ(encoding("fresh"), table);
  // And a client's RESTORE of three fields under it loads a hash table.
  EXPECT_EQ(bed.run({"RESTORE", "loaded", "0", three}), ok());
  EXPECT_EQ(encoding("loaded"), table);
  EXPECT_EQ(bed.run({"CONFIG", "SET", "hash-max-listpack-entries", "512"}), ok());
}

TEST(CommandHashes, RandFieldAndScanRefuseWhatTheyDoNotTake) {
  Bed bed;
  bed.run({"HSET", "h", "f", "v"});
  EXPECT_EQ(bed.run({"HRANDFIELD", "h", "1", "WITHVALUES", "x"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"HSCAN", "h", "0", "BOGUS"}), error("ERR syntax error"));
  // RESP3 pairs each field with its value.
  bed.run({"HELLO", "3"});
  EXPECT_EQ(bed.run({"HRANDFIELD", "h", "1", "WITHVALUES"}), "*1\r\n*2\r\n$1\r\nf\r\n$1\r\nv\r\n");
}

}  // namespace
}  // namespace mimir::test
