//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The set commands, from Valkey 8's command documentation, and the set's
// shapes: a small all-integer set answers sorted, a small set in the order its
// members were added, as Valkey's encodings answer.

#include <gtest/gtest.h>

#include <algorithm>

#include "mimir/command/table.h"
#include "mimir/keyspace/set.h"
#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

const std::string wrongtype = error("WRONGTYPE Operation against a key holding the wrong kind of value");

std::string bulks(const std::vector<std::string>& items) {
  std::vector<std::string> out;
  for (const auto& item : items) out.push_back(bulk(item));
  return array(out);
}

std::vector<std::string> as_strings(const keyspace::Set& set) {
  std::vector<std::string> out;
  for (auto member : set.ordered()) out.emplace_back(member);
  return out;
}

TEST(KeyspaceSet, ShapesChangeOneWayAsValkeysEncodings) {
  keyspace::Set set;
  set.add("10");
  set.add("-3");
  set.add("2");
  EXPECT_EQ(set.shape(), keyspace::Set::Shape::integers);
  EXPECT_EQ(as_strings(set), (std::vector<std::string>{"-3", "2", "10"}));
  // A non-canonical number is not an integer to an intset.
  set.add("07");
  EXPECT_EQ(set.shape(), keyspace::Set::Shape::small);
  // The intset's sorted order is kept at the change, as Valkey's conversion keeps it.
  EXPECT_EQ(as_strings(set), (std::vector<std::string>{"-3", "2", "10", "07"}));
  set.erase("07");
  EXPECT_EQ(set.shape(), keyspace::Set::Shape::small);
  set.add(std::string(65, 'x'));
  EXPECT_EQ(set.shape(), keyspace::Set::Shape::large);

  keyspace::Set many;
  for (int i = 0; i < 512; ++i) many.add(std::to_string(i));
  EXPECT_EQ(many.shape(), keyspace::Set::Shape::integers);
  many.add("512");
  EXPECT_EQ(many.shape(), keyspace::Set::Shape::large);

  keyspace::Set copy = set;
  set.add("only in the original");
  EXPECT_EQ(copy.size() + 1, set.size());
  EXPECT_TRUE(copy.contains("10"));
}

TEST(CommandSets, AddMembersCard) {
  Bed bed;
  EXPECT_EQ(bed.run({"SADD", "myset", "Hello"}), integer(1));
  EXPECT_EQ(bed.run({"SADD", "myset", "World"}), integer(1));
  EXPECT_EQ(bed.run({"SADD", "myset", "World"}), integer(0));
  EXPECT_EQ(bed.run({"SMEMBERS", "myset"}), bulks({"Hello", "World"}));
  EXPECT_EQ(bed.run({"SCARD", "myset"}), integer(2));
  EXPECT_EQ(bed.run({"SADD", "nums", "3", "1", "2"}), integer(3));
  EXPECT_EQ(bed.run({"SMEMBERS", "nums"}), bulks({"1", "2", "3"}));
  EXPECT_EQ(bed.run({"SMEMBERS", "nosuch"}), array({}));
  EXPECT_EQ(bed.run({"TYPE", "nums"}), "+set\r\n");
}

TEST(CommandSets, MembersAreASetInResp3) {
  Bed bed;
  bed.run({"SADD", "s", "a"});
  bed.run({"HELLO", "3"});
  EXPECT_EQ(bed.run({"SMEMBERS", "s"}), "~1\r\n" + bulk("a"));
}

TEST(CommandSets, IsMemberRem) {
  Bed bed;
  bed.run({"SADD", "myset", "one", "two", "three"});
  EXPECT_EQ(bed.run({"SISMEMBER", "myset", "one"}), integer(1));
  EXPECT_EQ(bed.run({"SISMEMBER", "myset", "four"}), integer(0));
  EXPECT_EQ(bed.run({"SMISMEMBER", "myset", "one", "notamember"}), array({integer(1), integer(0)}));
  EXPECT_EQ(bed.run({"SREM", "myset", "one", "four"}), integer(1));
  EXPECT_EQ(bed.run({"SREM", "myset", "two", "three"}), integer(2));
  EXPECT_EQ(bed.run({"EXISTS", "myset"}), integer(0));
  EXPECT_EQ(bed.run({"SREM", "nosuch", "a"}), integer(0));
}

TEST(CommandSets, PopAndRandMember) {
  Bed bed;
  bed.run({"SADD", "myset", "one", "two", "three"});
  const std::string one = bed.run({"SPOP", "myset"});
  EXPECT_TRUE(one == bulk("one") || one == bulk("two") || one == bulk("three")) << one;
  EXPECT_EQ(bed.run({"SCARD", "myset"}), integer(2));
  const std::string rest = bed.run({"SPOP", "myset", "5"});
  EXPECT_EQ(rest.substr(0, 4), "*2\r\n");
  EXPECT_EQ(bed.run({"EXISTS", "myset"}), integer(0));
  EXPECT_EQ(bed.run({"SPOP", "myset"}), nil());
  EXPECT_EQ(bed.run({"SPOP", "myset", "2"}), array({}));
  EXPECT_EQ(bed.run({"SPOP", "myset", "-1"}), error("ERR value is out of range, must be positive"));

  bed.run({"SADD", "r", "a", "b", "c"});
  EXPECT_EQ(bed.run({"SRANDMEMBER", "r", "5"}), bulks({"a", "b", "c"}));
  EXPECT_EQ(bed.run({"SRANDMEMBER", "r", "-5"}).substr(0, 4), "*5\r\n");
  EXPECT_EQ(bed.run({"SRANDMEMBER", "r", "2"}).substr(0, 4), "*2\r\n");
  EXPECT_EQ(bed.run({"SRANDMEMBER", "r", "0"}), array({}));
  EXPECT_EQ(bed.run({"SRANDMEMBER", "nosuch"}), nil());
  EXPECT_EQ(bed.run({"SRANDMEMBER", "nosuch", "3"}), array({}));
  EXPECT_EQ(bed.run({"SCARD", "r"}), integer(3));
}

TEST(CommandSets, Move) {
  Bed bed;
  bed.run({"SADD", "myset", "one", "two"});
  bed.run({"SADD", "myotherset", "three"});
  EXPECT_EQ(bed.run({"SMOVE", "myset", "myotherset", "two"}), integer(1));
  EXPECT_EQ(bed.run({"SMEMBERS", "myset"}), bulks({"one"}));
  EXPECT_EQ(bed.run({"SMEMBERS", "myotherset"}), bulks({"three", "two"}));
  EXPECT_EQ(bed.run({"SMOVE", "myset", "myotherset", "nope"}), integer(0));
  EXPECT_EQ(bed.run({"SMOVE", "myset", "myset", "one"}), integer(1));
  bed.run({"SET", "s", "v"});
  EXPECT_EQ(bed.run({"SMOVE", "myset", "s", "one"}), wrongtype);
  EXPECT_EQ(bed.run({"SISMEMBER", "myset", "one"}), integer(1));
}

TEST(CommandSets, InterUnionDiffAndTheirStores) {
  Bed bed;
  bed.run({"SADD", "key1", "a", "b", "c", "d"});
  bed.run({"SADD", "key2", "c"});
  bed.run({"SADD", "key3", "a", "c", "e"});
  EXPECT_EQ(bed.run({"SINTER", "key1", "key2", "key3"}), bulks({"c"}));
  EXPECT_EQ(bed.run({"SDIFF", "key1", "key2", "key3"}), bulks({"b", "d"}));
  EXPECT_EQ(bed.run({"SUNION", "key1", "key2", "key3"}), bulks({"a", "b", "c", "d", "e"}));
  EXPECT_EQ(bed.run({"SINTER", "key1", "nosuch"}), array({}));
  EXPECT_EQ(bed.run({"SINTERSTORE", "dest", "key1", "key3"}), integer(2));
  EXPECT_EQ(bed.run({"SMEMBERS", "dest"}), bulks({"a", "c"}));
  EXPECT_EQ(bed.run({"SUNIONSTORE", "dest", "key2", "key3"}), integer(3));
  EXPECT_EQ(bed.run({"SDIFFSTORE", "dest", "key2", "key1"}), integer(0));
  EXPECT_EQ(bed.run({"EXISTS", "dest"}), integer(0));
  bed.run({"SET", "s", "v"});
  EXPECT_EQ(bed.run({"SINTER", "key1", "s"}), wrongtype);
  // The destination is overwritten whatever its type.
  EXPECT_EQ(bed.run({"SUNIONSTORE", "s", "key2"}), integer(1));
  EXPECT_EQ(bed.run({"TYPE", "s"}), "+set\r\n");
}

TEST(CommandSets, InterCard) {
  Bed bed;
  bed.run({"SADD", "key1", "a", "b", "c", "d"});
  bed.run({"SADD", "key2", "c", "d", "e"});
  EXPECT_EQ(bed.run({"SINTERCARD", "2", "key1", "key2"}), integer(2));
  EXPECT_EQ(bed.run({"SINTERCARD", "2", "key1", "key2", "LIMIT", "1"}), integer(1));
  EXPECT_EQ(bed.run({"SINTERCARD", "0", "key1"}), error("ERR numkeys should be greater than 0"));
  EXPECT_EQ(bed.run({"SINTERCARD", "3", "key1", "key2"}), error("ERR Number of keys can't be greater than number of args"));
  EXPECT_EQ(bed.run({"SINTERCARD", "2", "key1", "key2", "LIMIT", "-1"}), error("ERR LIMIT can't be negative"));
  EXPECT_EQ(bed.run({"SINTERCARD", "2", "key1", "key2", "BOGUS"}), error("ERR syntax error"));
}

TEST(CommandSets, Scan) {
  Bed bed;
  bed.run({"SADD", "s", "apple", "banana", "avocado"});
  EXPECT_EQ(bed.run({"SSCAN", "s", "0"}), array({bulk("0"), bulks({"apple", "banana", "avocado"})}));
  EXPECT_EQ(bed.run({"SSCAN", "s", "0", "MATCH", "a*"}), array({bulk("0"), bulks({"apple", "avocado"})}));
  EXPECT_EQ(bed.run({"SSCAN", "s", "x"}), error("ERR invalid cursor"));
}

TEST(CommandSets, Classes) {
  using command::Class;
  for (const char* name : {"sadd", "srem"}) EXPECT_EQ(command::find_spec(name)->cls, Class::merge) << name;
  for (const char* name : {"spop", "smove", "sinterstore", "sunionstore", "sdiffstore"}) EXPECT_EQ(command::find_spec(name)->cls, Class::agreement) << name;
}

}  // namespace
}  // namespace mimir::test
