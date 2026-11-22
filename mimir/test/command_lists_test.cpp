//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The list commands that do not block, from Valkey 8's command documentation:
// each command's documented replies and errors, its examples among them, and
// the rule that a list that becomes empty is removed.

#include <gtest/gtest.h>

#include "mimir/command/table.h"
#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

const std::string wrongtype = error("WRONGTYPE Operation against a key holding the wrong kind of value");

std::string list(const std::vector<std::string>& items) {
  std::vector<std::string> bulks;
  for (const auto& item : items) bulks.push_back(bulk(item));
  return array(bulks);
}

TEST(CommandLists, PushAndRange) {
  Bed bed;
  EXPECT_EQ(bed.run({"RPUSH", "mylist", "one", "two", "three"}), integer(3));
  EXPECT_EQ(bed.run({"LRANGE", "mylist", "0", "0"}), list({"one"}));
  EXPECT_EQ(bed.run({"LRANGE", "mylist", "-3", "2"}), list({"one", "two", "three"}));
  EXPECT_EQ(bed.run({"LRANGE", "mylist", "-100", "100"}), list({"one", "two", "three"}));
  EXPECT_EQ(bed.run({"LRANGE", "mylist", "5", "10"}), list({}));
  EXPECT_EQ(bed.run({"LPUSH", "l2", "world", "hello"}), integer(2));
  EXPECT_EQ(bed.run({"LRANGE", "l2", "0", "-1"}), list({"hello", "world"}));
  EXPECT_EQ(bed.run({"LRANGE", "nosuch", "0", "-1"}), list({}));
  EXPECT_EQ(bed.run({"LRANGE", "l2", "x", "1"}), error("ERR value is not an integer or out of range"));
  EXPECT_EQ(bed.run({"TYPE", "l2"}), "+list\r\n");
}

TEST(CommandLists, PushXOnlyWhereAListIs) {
  Bed bed;
  EXPECT_EQ(bed.run({"LPUSHX", "nosuch", "x"}), integer(0));
  EXPECT_EQ(bed.run({"EXISTS", "nosuch"}), integer(0));
  bed.run({"RPUSH", "l", "a"});
  EXPECT_EQ(bed.run({"LPUSHX", "l", "b"}), integer(2));
  EXPECT_EQ(bed.run({"RPUSHX", "l", "c"}), integer(3));
  EXPECT_EQ(bed.run({"LRANGE", "l", "0", "-1"}), list({"b", "a", "c"}));
}

TEST(CommandLists, PopsAndTheEmptyListGoes) {
  Bed bed;
  bed.run({"RPUSH", "mylist", "one", "two", "three", "four", "five"});
  EXPECT_EQ(bed.run({"LPOP", "mylist"}), bulk("one"));
  EXPECT_EQ(bed.run({"LPOP", "mylist", "2"}), list({"two", "three"}));
  EXPECT_EQ(bed.run({"RPOP", "mylist"}), bulk("five"));
  EXPECT_EQ(bed.run({"RPOP", "mylist", "0"}), list({}));
  EXPECT_EQ(bed.run({"RPOP", "mylist", "9"}), list({"four"}));
  EXPECT_EQ(bed.run({"EXISTS", "mylist"}), integer(0));
  EXPECT_EQ(bed.run({"LPOP", "mylist"}), nil());
  EXPECT_EQ(bed.run({"LPOP", "mylist", "1"}), "*-1\r\n");
  EXPECT_EQ(bed.run({"LPOP", "mylist", "-1"}), error("ERR value is out of range, must be positive"));
  EXPECT_EQ(bed.run({"LPOP", "a", "1", "2"}), error("ERR wrong number of arguments for 'lpop' command"));
}

TEST(CommandLists, LenIndexSet) {
  Bed bed;
  bed.run({"RPUSH", "mylist", "Hello", "World"});
  EXPECT_EQ(bed.run({"LLEN", "mylist"}), integer(2));
  EXPECT_EQ(bed.run({"LLEN", "nosuch"}), integer(0));
  EXPECT_EQ(bed.run({"LINDEX", "mylist", "0"}), bulk("Hello"));
  EXPECT_EQ(bed.run({"LINDEX", "mylist", "-1"}), bulk("World"));
  EXPECT_EQ(bed.run({"LINDEX", "mylist", "3"}), nil());
  EXPECT_EQ(bed.run({"LINDEX", "nosuch", "x"}), nil());
  EXPECT_EQ(bed.run({"LSET", "mylist", "0", "four"}), ok());
  EXPECT_EQ(bed.run({"LSET", "mylist", "-1", "five"}), ok());
  EXPECT_EQ(bed.run({"LRANGE", "mylist", "0", "-1"}), list({"four", "five"}));
  EXPECT_EQ(bed.run({"LSET", "mylist", "2", "x"}), error("ERR index out of range"));
  EXPECT_EQ(bed.run({"LSET", "nosuch", "0", "x"}), error("ERR no such key"));
}

TEST(CommandLists, Insert) {
  Bed bed;
  bed.run({"RPUSH", "mylist", "Hello", "World"});
  EXPECT_EQ(bed.run({"LINSERT", "mylist", "BEFORE", "World", "There"}), integer(3));
  EXPECT_EQ(bed.run({"LINSERT", "mylist", "AFTER", "World", "!"}), integer(4));
  EXPECT_EQ(bed.run({"LRANGE", "mylist", "0", "-1"}), list({"Hello", "There", "World", "!"}));
  EXPECT_EQ(bed.run({"LINSERT", "mylist", "BEFORE", "nope", "x"}), integer(-1));
  EXPECT_EQ(bed.run({"LINSERT", "nosuch", "BEFORE", "a", "x"}), integer(0));
  EXPECT_EQ(bed.run({"LINSERT", "mylist", "BESIDE", "World", "x"}), error("ERR syntax error"));
}

TEST(CommandLists, Rem) {
  Bed bed;
  bed.run({"RPUSH", "mylist", "hello", "hello", "foo", "hello"});
  EXPECT_EQ(bed.run({"LREM", "mylist", "-2", "hello"}), integer(2));
  EXPECT_EQ(bed.run({"LRANGE", "mylist", "0", "-1"}), list({"hello", "foo"}));
  bed.run({"RPUSH", "two", "a", "b", "a", "a"});
  EXPECT_EQ(bed.run({"LREM", "two", "1", "a"}), integer(1));
  EXPECT_EQ(bed.run({"LRANGE", "two", "0", "-1"}), list({"b", "a", "a"}));
  EXPECT_EQ(bed.run({"LREM", "two", "0", "a"}), integer(2));
  EXPECT_EQ(bed.run({"LREM", "two", "0", "b"}), integer(1));
  EXPECT_EQ(bed.run({"EXISTS", "two"}), integer(0));
}

TEST(CommandLists, Trim) {
  Bed bed;
  bed.run({"RPUSH", "mylist", "one", "two", "three"});
  EXPECT_EQ(bed.run({"LTRIM", "mylist", "1", "-1"}), ok());
  EXPECT_EQ(bed.run({"LRANGE", "mylist", "0", "-1"}), list({"two", "three"}));
  EXPECT_EQ(bed.run({"LTRIM", "mylist", "5", "10"}), ok());
  EXPECT_EQ(bed.run({"EXISTS", "mylist"}), integer(0));
  EXPECT_EQ(bed.run({"LTRIM", "nosuch", "0", "1"}), ok());
}

TEST(CommandLists, Pos) {
  Bed bed;
  bed.run({"RPUSH", "mylist", "a", "b", "c", "d", "1", "2", "3", "4", "3", "3", "3"});
  EXPECT_EQ(bed.run({"LPOS", "mylist", "3"}), integer(6));
  EXPECT_EQ(bed.run({"LPOS", "mylist", "3", "COUNT", "0", "RANK", "2"}), array({integer(8), integer(9), integer(10)}));
  EXPECT_EQ(bed.run({"LPOS", "mylist", "3", "RANK", "-1"}), integer(10));
  EXPECT_EQ(bed.run({"LPOS", "mylist", "3", "COUNT", "2"}), array({integer(6), integer(8)}));
  EXPECT_EQ(bed.run({"LPOS", "mylist", "3", "MAXLEN", "6"}), nil());
  EXPECT_EQ(bed.run({"LPOS", "mylist", "x"}), nil());
  EXPECT_EQ(bed.run({"LPOS", "nosuch", "x", "COUNT", "1"}), array({}));
  EXPECT_EQ(bed.run({"LPOS", "mylist", "3", "RANK", "0"}),
            error("ERR RANK can't be zero: use 1 to start from the first match, 2 from the second ... or use negative to start from the end of the list"));
  EXPECT_EQ(bed.run({"LPOS", "mylist", "3", "COUNT", "-1"}), error("ERR COUNT can't be negative"));
  EXPECT_EQ(bed.run({"LPOS", "mylist", "3", "MAXLEN", "-1"}), error("ERR MAXLEN can't be negative"));
  EXPECT_EQ(bed.run({"LPOS", "mylist", "3", "BOGUS"}), error("ERR syntax error"));
}

TEST(CommandLists, MoveAndRPopLPush) {
  Bed bed;
  bed.run({"RPUSH", "mylist", "one", "two", "three"});
  EXPECT_EQ(bed.run({"LMOVE", "mylist", "myotherlist", "RIGHT", "LEFT"}), bulk("three"));
  EXPECT_EQ(bed.run({"LMOVE", "mylist", "myotherlist", "LEFT", "RIGHT"}), bulk("one"));
  EXPECT_EQ(bed.run({"LRANGE", "mylist", "0", "-1"}), list({"two"}));
  EXPECT_EQ(bed.run({"LRANGE", "myotherlist", "0", "-1"}), list({"three", "one"}));
  EXPECT_EQ(bed.run({"RPOPLPUSH", "mylist", "myotherlist"}), bulk("two"));
  EXPECT_EQ(bed.run({"EXISTS", "mylist"}), integer(0));
  EXPECT_EQ(bed.run({"LMOVE", "nosuch", "x", "LEFT", "LEFT"}), nil());
  EXPECT_EQ(bed.run({"LMOVE", "myotherlist", "x", "UP", "LEFT"}), error("ERR syntax error"));
  // The same list: a rotation.
  EXPECT_EQ(bed.run({"LMOVE", "myotherlist", "myotherlist", "LEFT", "RIGHT"}), bulk("two"));
  EXPECT_EQ(bed.run({"LRANGE", "myotherlist", "0", "-1"}), list({"three", "one", "two"}));
  // A destination of another type: nothing moves.
  bed.run({"SET", "s", "v"});
  EXPECT_EQ(bed.run({"LMOVE", "myotherlist", "s", "LEFT", "LEFT"}), wrongtype);
  EXPECT_EQ(bed.run({"LLEN", "myotherlist"}), integer(3));
}

TEST(CommandLists, MPop) {
  Bed bed;
  EXPECT_EQ(bed.run({"LMPOP", "2", "non1", "non2", "LEFT", "COUNT", "10"}), "*-1\r\n");
  bed.run({"LPUSH", "mylist", "one", "two", "three", "four", "five"});
  EXPECT_EQ(bed.run({"LMPOP", "1", "mylist", "LEFT"}), array({bulk("mylist"), list({"five"})}));
  EXPECT_EQ(bed.run({"LMPOP", "1", "mylist", "RIGHT", "COUNT", "10"}), array({bulk("mylist"), list({"one", "two", "three", "four"})}));
  bed.run({"LPUSH", "mylist2", "a", "b"});
  EXPECT_EQ(bed.run({"LMPOP", "2", "mylist", "mylist2", "RIGHT", "COUNT", "1"}), array({bulk("mylist2"), list({"a"})}));
  EXPECT_EQ(bed.run({"LMPOP", "0", "x", "LEFT"}), error("ERR numkeys should be greater than 0"));
  EXPECT_EQ(bed.run({"LMPOP", "1", "x", "LEFT", "COUNT", "0"}), error("ERR count should be greater than 0"));
  EXPECT_EQ(bed.run({"LMPOP", "5", "x", "LEFT"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"LMPOP", "1", "x", "MIDDLE"}), error("ERR syntax error"));
}

TEST(CommandLists, TypesAreKeptApart) {
  Bed bed;
  bed.run({"RPUSH", "l", "a"});
  bed.run({"SET", "s", "v"});
  EXPECT_EQ(bed.run({"GET", "l"}), wrongtype);
  EXPECT_EQ(bed.run({"INCR", "l"}), wrongtype);
  EXPECT_EQ(bed.run({"APPEND", "l", "x"}), wrongtype);
  EXPECT_EQ(bed.run({"SET", "l", "x", "GET"}), wrongtype);
  EXPECT_EQ(bed.run({"LLEN", "l"}), integer(1));
  EXPECT_EQ(bed.run({"LPUSH", "s", "x"}), wrongtype);
  EXPECT_EQ(bed.run({"LRANGE", "s", "0", "-1"}), wrongtype);
  EXPECT_EQ(bed.run({"MGET", "l", "s"}), array({nil(), bulk("v")}));
  // SET replaces a value of any type.
  EXPECT_EQ(bed.run({"SET", "l", "now a string"}), ok());
  EXPECT_EQ(bed.run({"TYPE", "l"}), "+string\r\n");
  bed.run({"RPUSH", "c", "x", "y"});
  EXPECT_EQ(bed.run({"COPY", "c", "d"}), integer(1));
  bed.run({"RPUSH", "c", "z"});
  EXPECT_EQ(bed.run({"LLEN", "d"}), integer(2));
}

TEST(CommandLists, Classes) {
  using command::Class;
  for (const char* name : {"lpush", "rpush"}) EXPECT_EQ(command::find_spec(name)->cls, Class::merge) << name;
  for (const char* name : {"lpop", "rpop", "lmove", "rpoplpush", "lmpop", "lset", "linsert", "lrem", "ltrim", "lpushx", "rpushx"}) {
    EXPECT_EQ(command::find_spec(name)->cls, Class::agreement) << name;
  }
}

TEST(CommandLists, MpopTakesOneCountAndLremOfNothingIsZero) {
  Bed bed;
  bed.run({"RPUSH", "l", "a"});
  EXPECT_EQ(bed.run({"LMPOP", "1", "l", "LEFT", "COUNT", "1", "COUNT", "2"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"LREM", "nosuch", "0", "x"}), integer(0));
}

}  // namespace
}  // namespace mimir::test
