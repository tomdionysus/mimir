//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The sorted set: its order (score, then the member's bytes) checked against a
// plain ordered reference under random operations, and the commands from
// Valkey 8's command documentation, their examples among them.

#include <gtest/gtest.h>

#include <random>
#include <set>

#include "mimir/command/table.h"
#include "mimir/keyspace/zset.h"
#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

const std::string wrongtype = error("WRONGTYPE Operation against a key holding the wrong kind of value");

std::string bulks(const std::vector<std::string>& items) {
  std::vector<std::string> out;
  for (const auto& item : items) out.push_back(bulk(item));
  return array(out);
}

TEST(KeyspaceSortedSet, AgreesWithAReferenceUnderRandomOperations) {
  keyspace::SortedSet zset;
  std::set<std::pair<double, std::string>> reference;
  std::map<std::string, double> scores;
  std::mt19937_64 random(7);
  for (int step = 0; step < 20000; ++step) {
    const std::string member = "m" + std::to_string(random() % 300);
    const double score = static_cast<double>(random() % 50) / 2.0;
    if (random() % 3 == 0) {
      const bool had = scores.count(member) > 0;
      EXPECT_EQ(zset.erase(member), had);
      if (had) {
        reference.erase({scores[member], member});
        scores.erase(member);
      }
    } else {
      const bool fresh = scores.count(member) == 0;
      EXPECT_EQ(zset.set(member, score), fresh);
      if (!fresh) reference.erase({scores[member], member});
      scores[member] = score;
      reference.insert({score, member});
    }
    if (step % 500 == 0) {
      ASSERT_EQ(zset.size(), reference.size());
      size_t rank = 0;
      for (const auto& [s, m] : reference) {
        ASSERT_EQ(zset.at(rank).member, m);
        ASSERT_EQ(zset.at(rank).score, s);
        ASSERT_EQ(zset.rank(m), rank);
        ++rank;
      }
      for (double probe : {-1.0, 0.0, 3.5, 12.0, 30.0}) {
        size_t below = 0, at_most = 0;
        for (const auto& [s, m] : reference) {
          below += s < probe;
          at_most += s <= probe;
        }
        ASSERT_EQ(zset.count_below_score(probe, false), below);
        ASSERT_EQ(zset.count_below_score(probe, true), at_most);
      }
    }
  }
  keyspace::SortedSet copy = zset;
  zset.set("only-in-original", 1);
  EXPECT_EQ(copy.size() + (scores.count("only-in-original") ? 0 : 1), zset.size());
}

TEST(CommandZsets, AddAndRange) {
  Bed bed;
  EXPECT_EQ(bed.run({"ZADD", "myzset", "1", "one"}), integer(1));
  EXPECT_EQ(bed.run({"ZADD", "myzset", "1", "uno"}), integer(1));
  EXPECT_EQ(bed.run({"ZADD", "myzset", "2", "two", "3", "two"}), integer(1));
  EXPECT_EQ(bed.run({"ZRANGE", "myzset", "0", "-1", "WITHSCORES"}), bulks({"one", "1", "uno", "1", "two", "3"}));
  EXPECT_EQ(bed.run({"ZRANGE", "myzset", "0", "-1"}), bulks({"one", "uno", "two"}));
  EXPECT_EQ(bed.run({"ZRANGE", "myzset", "2", "3"}), bulks({"two"}));
  EXPECT_EQ(bed.run({"ZRANGE", "myzset", "-2", "-1"}), bulks({"uno", "two"}));
  EXPECT_EQ(bed.run({"ZRANGE", "myzset", "0", "-1", "REV"}), bulks({"two", "uno", "one"}));
  EXPECT_EQ(bed.run({"ZCARD", "myzset"}), integer(3));
  EXPECT_EQ(bed.run({"TYPE", "myzset"}), "+zset\r\n");
}

TEST(CommandZsets, AddOptions) {
  Bed bed;
  bed.run({"ZADD", "z", "1", "a", "2", "b"});
  EXPECT_EQ(bed.run({"ZADD", "z", "NX", "5", "a", "3", "c"}), integer(1));
  EXPECT_EQ(bed.run({"ZSCORE", "z", "a"}), bulk("1"));
  EXPECT_EQ(bed.run({"ZADD", "z", "XX", "5", "a", "4", "d"}), integer(0));
  EXPECT_EQ(bed.run({"ZSCORE", "z", "a"}), bulk("5"));
  EXPECT_EQ(bed.run({"ZSCORE", "z", "d"}), nil());
  EXPECT_EQ(bed.run({"ZADD", "z", "CH", "6", "a", "2", "b", "9", "e"}), integer(2));
  EXPECT_EQ(bed.run({"ZADD", "z", "GT", "CH", "1", "a", "7", "b"}), integer(1));
  EXPECT_EQ(bed.run({"ZSCORE", "z", "a"}), bulk("6"));
  EXPECT_EQ(bed.run({"ZSCORE", "z", "b"}), bulk("7"));
  EXPECT_EQ(bed.run({"ZADD", "z", "LT", "CH", "1", "a"}), integer(1));
  EXPECT_EQ(bed.run({"ZADD", "z", "INCR", "2", "a"}), bulk("3"));
  EXPECT_EQ(bed.run({"ZADD", "z", "NX", "INCR", "2", "a"}), nil());
  EXPECT_EQ(bed.run({"ZADD", "z", "NX", "XX", "1", "a"}), error("ERR XX and NX options at the same time are not compatible"));
  EXPECT_EQ(bed.run({"ZADD", "z", "GT", "LT", "1", "a"}), error("ERR GT, LT, and/or NX options at the same time are not compatible"));
  EXPECT_EQ(bed.run({"ZADD", "z", "INCR", "1", "a", "2", "b"}), error("ERR INCR option supports a single increment-element pair"));
  // Arity is checked first: three arguments are too few for ZADD.
  EXPECT_EQ(bed.run({"ZADD", "z", "1"}), error("ERR wrong number of arguments for 'zadd' command"));
  EXPECT_EQ(bed.run({"ZADD", "z", "1", "a", "2"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"ZADD", "z", "x", "a"}), error("ERR value is not a valid float"));
  EXPECT_EQ(bed.run({"ZADD", "z", "nan", "a"}), error("ERR value is not a valid float"));
  EXPECT_EQ(bed.run({"ZADD", "fresh", "XX", "1", "a"}), integer(0));
  EXPECT_EQ(bed.run({"EXISTS", "fresh"}), integer(0));
}

TEST(CommandZsets, IncrScoreMScore) {
  Bed bed;
  bed.run({"ZADD", "myzset", "1", "one", "2", "two"});
  EXPECT_EQ(bed.run({"ZINCRBY", "myzset", "2", "one"}), bulk("3"));
  EXPECT_EQ(bed.run({"ZRANGE", "myzset", "0", "-1", "WITHSCORES"}), bulks({"two", "2", "one", "3"}));
  EXPECT_EQ(bed.run({"ZMSCORE", "myzset", "one", "two", "nofield"}), array({bulk("3"), bulk("2"), nil()}));
  EXPECT_EQ(bed.run({"ZINCRBY", "myzset", "x", "one"}), error("ERR value is not a valid float"));
  bed.run({"ZADD", "inf", "inf", "a"});
  EXPECT_EQ(bed.run({"ZINCRBY", "inf", "-inf", "a"}), error("ERR resulting score is not a number (NaN)"));
  EXPECT_EQ(bed.run({"ZSCORE", "inf", "a"}), bulk("inf"));
}

TEST(CommandZsets, ScoresAreDoublesInResp3) {
  Bed bed;
  bed.run({"ZADD", "z", "1.5", "a"});
  bed.run({"HELLO", "3"});
  EXPECT_EQ(bed.run({"ZSCORE", "z", "a"}), ",1.5\r\n");
  EXPECT_EQ(bed.run({"ZRANGE", "z", "0", "-1", "WITHSCORES"}), "*1\r\n*2\r\n" + bulk("a") + ",1.5\r\n");
}

TEST(CommandZsets, ByScore) {
  Bed bed;
  bed.run({"ZADD", "myzset", "1", "one", "2", "two", "3", "three"});
  EXPECT_EQ(bed.run({"ZRANGEBYSCORE", "myzset", "-inf", "+inf"}), bulks({"one", "two", "three"}));
  EXPECT_EQ(bed.run({"ZRANGEBYSCORE", "myzset", "1", "2"}), bulks({"one", "two"}));
  EXPECT_EQ(bed.run({"ZRANGEBYSCORE", "myzset", "(1", "2"}), bulks({"two"}));
  EXPECT_EQ(bed.run({"ZRANGEBYSCORE", "myzset", "(1", "(2"}), bulks({}));
  EXPECT_EQ(bed.run({"ZREVRANGEBYSCORE", "myzset", "+inf", "-inf"}), bulks({"three", "two", "one"}));
  EXPECT_EQ(bed.run({"ZREVRANGEBYSCORE", "myzset", "2", "(1"}), bulks({"two"}));
  EXPECT_EQ(bed.run({"ZRANGE", "myzset", "(1", "+inf", "BYSCORE", "LIMIT", "1", "1"}), bulks({"three"}));
  EXPECT_EQ(bed.run({"ZRANGE", "myzset", "+inf", "-inf", "BYSCORE", "REV", "LIMIT", "0", "2"}), bulks({"three", "two"}));
  EXPECT_EQ(bed.run({"ZCOUNT", "myzset", "-inf", "+inf"}), integer(3));
  EXPECT_EQ(bed.run({"ZCOUNT", "myzset", "(1", "3"}), integer(2));
  EXPECT_EQ(bed.run({"ZCOUNT", "myzset", "x", "3"}), error("ERR min or max is not a float"));
  EXPECT_EQ(bed.run({"ZRANGE", "myzset", "0", "1", "LIMIT", "0", "1"}),
            error("ERR syntax error, LIMIT is only supported in combination with either BYSCORE or BYLEX"));
}

TEST(CommandZsets, ByLex) {
  Bed bed;
  bed.run({"ZADD", "myzset", "0", "a", "0", "b", "0", "c", "0", "d", "0", "e", "0", "f", "0", "g"});
  EXPECT_EQ(bed.run({"ZRANGEBYLEX", "myzset", "-", "[c"}), bulks({"a", "b", "c"}));
  EXPECT_EQ(bed.run({"ZRANGEBYLEX", "myzset", "-", "(c"}), bulks({"a", "b"}));
  EXPECT_EQ(bed.run({"ZRANGEBYLEX", "myzset", "[aaa", "(g"}), bulks({"b", "c", "d", "e", "f"}));
  EXPECT_EQ(bed.run({"ZREVRANGEBYLEX", "myzset", "[c", "-"}), bulks({"c", "b", "a"}));
  EXPECT_EQ(bed.run({"ZLEXCOUNT", "myzset", "-", "+"}), integer(7));
  EXPECT_EQ(bed.run({"ZLEXCOUNT", "myzset", "[b", "[f"}), integer(5));
  EXPECT_EQ(bed.run({"ZRANGEBYLEX", "myzset", "a", "[c"}), error("ERR min or max not valid string range item"));
  EXPECT_EQ(bed.run({"ZRANGE", "myzset", "-", "+", "BYLEX", "WITHSCORES"}), error("ERR syntax error, WITHSCORES not supported in combination with BYLEX"));
  EXPECT_EQ(bed.run({"ZREMRANGEBYLEX", "myzset", "[b", "(e"}), integer(3));
  EXPECT_EQ(bed.run({"ZRANGE", "myzset", "0", "-1"}), bulks({"a", "e", "f", "g"}));
}

TEST(CommandZsets, RankRem) {
  Bed bed;
  bed.run({"ZADD", "myzset", "1", "one", "2", "two", "3", "three"});
  EXPECT_EQ(bed.run({"ZRANK", "myzset", "three"}), integer(2));
  EXPECT_EQ(bed.run({"ZRANK", "myzset", "four"}), nil());
  EXPECT_EQ(bed.run({"ZRANK", "myzset", "three", "WITHSCORE"}), array({integer(2), bulk("3")}));
  EXPECT_EQ(bed.run({"ZREVRANK", "myzset", "one"}), integer(2));
  EXPECT_EQ(bed.run({"ZREM", "myzset", "two", "nope"}), integer(1));
  EXPECT_EQ(bed.run({"ZRANGE", "myzset", "0", "-1"}), bulks({"one", "three"}));
  EXPECT_EQ(bed.run({"ZREMRANGEBYRANK", "myzset", "0", "0"}), integer(1));
  EXPECT_EQ(bed.run({"ZREMRANGEBYSCORE", "myzset", "-inf", "(3"}), integer(0));
  EXPECT_EQ(bed.run({"ZREMRANGEBYSCORE", "myzset", "-inf", "3"}), integer(1));
  EXPECT_EQ(bed.run({"EXISTS", "myzset"}), integer(0));
}

TEST(CommandZsets, Pops) {
  Bed bed;
  bed.run({"ZADD", "myzset", "1", "one", "2", "two", "3", "three"});
  EXPECT_EQ(bed.run({"ZPOPMIN", "myzset"}), bulks({"one", "1"}));
  EXPECT_EQ(bed.run({"ZPOPMAX", "myzset", "5"}), bulks({"three", "3", "two", "2"}));
  EXPECT_EQ(bed.run({"EXISTS", "myzset"}), integer(0));
  EXPECT_EQ(bed.run({"ZPOPMIN", "myzset"}), array({}));
  EXPECT_EQ(bed.run({"ZPOPMIN", "myzset", "-1"}), error("ERR value is out of range, must be positive"));
  bed.run({"ZADD", "a", "1", "x", "2", "y"});
  EXPECT_EQ(bed.run({"ZMPOP", "2", "nosuch", "a", "MAX", "COUNT", "5"}), array({bulk("a"), array({bulks({"y", "2"}), bulks({"x", "1"})})}));
  EXPECT_EQ(bed.run({"ZMPOP", "1", "a", "MIN"}), "*-1\r\n");
  EXPECT_EQ(bed.run({"ZMPOP", "1", "a", "SIDEWAYS"}), error("ERR syntax error"));
}

TEST(CommandZsets, Combinations) {
  Bed bed;
  bed.run({"ZADD", "zset1", "1", "one", "2", "two"});
  bed.run({"ZADD", "zset2", "1", "one", "2", "two", "3", "three"});
  EXPECT_EQ(bed.run({"ZUNION", "2", "zset1", "zset2", "WITHSCORES"}), bulks({"one", "2", "three", "3", "two", "4"}));
  EXPECT_EQ(bed.run({"ZINTER", "2", "zset1", "zset2", "WITHSCORES"}), bulks({"one", "2", "two", "4"}));
  EXPECT_EQ(bed.run({"ZDIFF", "2", "zset2", "zset1", "WITHSCORES"}), bulks({"three", "3"}));
  EXPECT_EQ(bed.run({"ZUNIONSTORE", "out", "2", "zset1", "zset2", "WEIGHTS", "2", "3"}), integer(3));
  EXPECT_EQ(bed.run({"ZRANGE", "out", "0", "-1", "WITHSCORES"}), bulks({"one", "5", "three", "9", "two", "10"}));
  EXPECT_EQ(bed.run({"ZINTERSTORE", "out", "2", "zset1", "zset2", "AGGREGATE", "MAX"}), integer(2));
  EXPECT_EQ(bed.run({"ZRANGE", "out", "0", "-1", "WITHSCORES"}), bulks({"one", "1", "two", "2"}));
  EXPECT_EQ(bed.run({"ZINTERCARD", "2", "zset1", "zset2", "LIMIT", "1"}), integer(1));
  EXPECT_EQ(bed.run({"ZUNIONSTORE", "out", "0", "zset1"}), error("ERR at least 1 input key is needed for 'zunionstore' command"));
  EXPECT_EQ(bed.run({"ZUNION", "2", "zset1", "zset2", "WEIGHTS", "1", "x"}), error("ERR weight value is not a float"));
  EXPECT_EQ(bed.run({"ZDIFF", "2", "zset1", "zset2", "WEIGHTS", "1", "1"}), error("ERR syntax error"));
  // A set is an input with every score 1.
  bed.run({"SADD", "plain", "one", "four"});
  EXPECT_EQ(bed.run({"ZUNION", "2", "zset1", "plain", "WITHSCORES"}), bulks({"four", "1", "one", "2", "two", "2"}));
  bed.run({"SET", "s", "v"});
  EXPECT_EQ(bed.run({"ZUNION", "2", "zset1", "s"}), wrongtype);
}

TEST(CommandZsets, RangeStoreRandMemberScan) {
  Bed bed;
  bed.run({"ZADD", "srczset", "1", "one", "2", "two", "3", "three", "4", "four"});
  EXPECT_EQ(bed.run({"ZRANGESTORE", "dstzset", "srczset", "2", "-1"}), integer(2));
  EXPECT_EQ(bed.run({"ZRANGE", "dstzset", "0", "-1"}), bulks({"three", "four"}));
  EXPECT_EQ(bed.run({"ZRANDMEMBER", "nosuch"}), nil());
  EXPECT_EQ(bed.run({"ZRANDMEMBER", "srczset", "10", "WITHSCORES"}), bulks({"four", "4", "three", "3", "two", "2", "one", "1"}));
  EXPECT_EQ(bed.run({"ZRANDMEMBER", "srczset", "-3"}).substr(0, 4), "*3\r\n");
  EXPECT_EQ(bed.run({"ZSCAN", "srczset", "0", "MATCH", "t*"}), array({bulk("0"), bulks({"two", "2", "three", "3"})}));
  EXPECT_EQ(bed.run({"ZSCAN", "srczset", "0", "NOSCORES"}), array({bulk("0"), bulks({"one", "two", "three", "four"})}));
}

// Valkey 8.1.1's answers (probed) for the old range commands' options, the
// aggregates, stores that make nothing, a missing key, and the refusals.
TEST(CommandZsets, OptionsMissingKeysAndRefusalsAsValkeyAnswers) {
  Bed bed;
  const std::string syntax = error("ERR syntax error");
  bed.run({"ZADD", "z", "1", "a", "2", "b", "3", "c"});
  EXPECT_EQ(bed.run({"ZRANGE", "z", "0", "-1", "BOGUS"}), syntax);
  EXPECT_EQ(bed.run({"ZRANGE", "nokey", "0", "-1"}), array({}));
  EXPECT_EQ(bed.run({"ZREVRANGE", "z", "0", "-1", "WITHSCORES"}), bulks({"c", "3", "b", "2", "a", "1"}));
  EXPECT_EQ(bed.run({"ZREVRANGE", "z", "0", "-1", "LIMIT", "0", "1"}),
            error("ERR syntax error, LIMIT is only supported in combination with either BYSCORE or BYLEX"));
  EXPECT_EQ(bed.run({"ZRANGEBYSCORE", "z", "-inf", "+inf", "WITHSCORES", "LIMIT", "1", "1"}), bulks({"b", "2"}));
  EXPECT_EQ(bed.run({"ZRANGEBYLEX", "z", "-", "+", "WITHSCORES"}), error("ERR syntax error, WITHSCORES not supported in combination with BYLEX"));

  bed.run({"ZADD", "z2", "10", "a", "20", "d"});
  EXPECT_EQ(bed.run({"ZUNION", "2", "z", "z2", "AGGREGATE", "SUM", "WITHSCORES"}), bulks({"b", "2", "c", "3", "a", "11", "d", "20"}));
  EXPECT_EQ(bed.run({"ZUNION", "2", "z", "z2", "AGGREGATE", "MIN", "WITHSCORES"}), bulks({"a", "1", "b", "2", "c", "3", "d", "20"}));
  EXPECT_EQ(bed.run({"ZUNION", "2", "z", "z2", "AGGREGATE", "MAX", "WITHSCORES"}), bulks({"b", "2", "c", "3", "a", "10", "d", "20"}));
  EXPECT_EQ(bed.run({"ZUNION", "2", "z", "z2", "AGGREGATE", "BOGUS"}), syntax);
  EXPECT_EQ(bed.run({"ZINTER", "2", "z", "z2", "WITHSCORES"}), bulks({"a", "11"}));
  EXPECT_EQ(bed.run({"ZINTER", "2", "z", "z2", "AGGREGATE", "MAX", "WITHSCORES"}), bulks({"a", "10"}));

  // A store that makes nothing removes its destination, whatever it held.
  bed.run({"SET", "dst", "x"});
  EXPECT_EQ(bed.run({"ZUNIONSTORE", "dst", "1", "nokey"}), integer(0));
  EXPECT_EQ(bed.run({"EXISTS", "dst"}), integer(0));
  EXPECT_EQ(bed.run({"ZDIFFSTORE", "dst", "2", "z", "z2"}), integer(2));
  EXPECT_EQ(bed.run({"ZRANGE", "dst", "0", "-1", "WITHSCORES"}), bulks({"b", "2", "c", "3"}));
  EXPECT_EQ(bed.run({"ZRANGESTORE", "d", "z", "(1", "(3", "BYSCORE"}), integer(1));
  EXPECT_EQ(bed.run({"ZRANGE", "d", "0", "-1", "WITHSCORES"}), bulks({"b", "2"}));
  EXPECT_EQ(bed.run({"ZRANGESTORE", "d", "z", "[a", "(c", "BYLEX"}), integer(2));
  EXPECT_EQ(bed.run({"ZRANGE", "d", "0", "-1"}), bulks({"a", "b"}));
  EXPECT_EQ(bed.run({"ZRANGESTORE", "d", "nokey", "0", "-1"}), integer(0));
  EXPECT_EQ(bed.run({"EXISTS", "d"}), integer(0));

  EXPECT_EQ(bed.run({"ZADD", "nokey", "XX", "INCR", "1", "a"}), nil());
  EXPECT_EQ(bed.run({"ZADD", "inf", "INCR", "+inf", "a"}), bulk("inf"));
  EXPECT_EQ(bed.run({"ZADD", "inf", "INCR", "-inf", "a"}), error("ERR resulting score is not a number (NaN)"));
  EXPECT_EQ(bed.run({"ZSCORE", "inf", "a"}), bulk("inf"));

  for (const char* name : {"ZCOUNT", "ZREMRANGEBYSCORE"}) EXPECT_EQ(bed.run({name, "nokey", "0", "1"}), integer(0)) << name;
  for (const char* name : {"ZLEXCOUNT", "ZREMRANGEBYLEX"}) EXPECT_EQ(bed.run({name, "nokey", "-", "+"}), integer(0)) << name;
  EXPECT_EQ(bed.run({"ZREMRANGEBYRANK", "nokey", "0", "1"}), integer(0));
  EXPECT_EQ(bed.run({"ZREM", "nokey", "a"}), integer(0));

  EXPECT_EQ(bed.run({"ZRANK", "z", "a", "BOGUS"}), syntax);
  EXPECT_EQ(bed.run({"ZRANK", "z", "a", "WITHSCORE", "extra"}), error("ERR wrong number of arguments for 'zrank' command"));
  EXPECT_EQ(bed.run({"ZREVRANK", "z", "nosuch", "WITHSCORE"}), "*-1\r\n");
  EXPECT_EQ(bed.run({"ZREVRANK", "z", "a", "WITHSCORE"}), array({integer(2), bulk("1")}));

  EXPECT_EQ(bed.run({"ZRANDMEMBER", "z", "1", "BOGUS"}), syntax);
  EXPECT_EQ(bed.run({"ZRANDMEMBER", "z", "1", "WITHSCORES", "extra"}), syntax);
  EXPECT_EQ(bed.run({"ZRANDMEMBER", "z", "9223372036854775807", "WITHSCORES"}), error("ERR value is out of range"));
  // Fewer than the set holds: that many distinct members.
  const std::string two = bed.run({"ZRANDMEMBER", "z", "2"});
  ASSERT_EQ(two.substr(0, 4), "*2\r\n") << two;
  EXPECT_NE(two.substr(4, 7), two.substr(11, 7)) << two;
  EXPECT_EQ(bed.run({"ZSCAN", "z", "0", "BOGUS"}), syntax);
  EXPECT_EQ(bed.run({"ZINTERCARD", "1", "z", "BOGUS"}), syntax);
  EXPECT_EQ(bed.run({"ZMPOP", "1", "z", "MIN", "COUNT", "1", "COUNT", "1"}), syntax);
}

// With a count, RESP3 nests each popped pair; RESP2 flattens them.
TEST(CommandZsets, PopsWithACountNestInResp3) {
  Bed bed;
  bed.run({"ZADD", "z", "1", "a", "2", "b", "3", "c"});
  bed.run({"HELLO", "3"});
  EXPECT_EQ(bed.run({"ZPOPMIN", "z", "2"}), "*2\r\n*2\r\n$1\r\na\r\n,1\r\n*2\r\n$1\r\nb\r\n,2\r\n");
  EXPECT_EQ(bed.run({"ZPOPMAX", "z"}), "*2\r\n$1\r\nc\r\n,3\r\n");
}

TEST(CommandZsets, Classes) {
  using command::Class;
  const auto cls = [](std::vector<std::string> args) { return command::class_of(*command::find_spec(args[0]), args); };
  EXPECT_EQ(cls({"ZADD", "z", "1", "a"}), Class::merge);
  EXPECT_EQ(cls({"ZADD", "z", "NX", "1", "a"}), Class::agreement);
  EXPECT_EQ(cls({"ZADD", "z", "INCR", "1", "a"}), Class::configurable);
  EXPECT_EQ(cls({"ZREM", "z", "a"}), Class::merge);
  for (const char* name : {"ZPOPMIN", "ZPOPMAX", "ZMPOP", "ZREMRANGEBYRANK", "ZUNIONSTORE", "ZRANGESTORE"}) {
    EXPECT_EQ(command::find_spec(name)->cls, Class::agreement) << name;
  }
}

// A sorted set's encoding as Valkey 8.1.1's changes (probed): a new member
// past the agreed limits makes it a skiplist for good, a score update never
// does; a store's result takes the encoding its size gives; a client's
// RESTORE loads by size.
TEST(CommandZsets, EncodingsChangeAsValkeysDo) {
  Bed bed;
  const auto encoding = [&bed](const std::string& key) { return bed.run({"OBJECT", "ENCODING", key}); };
  const auto payload = [&bed](const std::string& key) {
    const std::string dumped = bed.run({"DUMP", key});
    const size_t at = dumped.find("\r\n") + 2;
    return dumped.substr(at, dumped.size() - at - 2);
  };
  std::vector<std::string> zadd{"ZADD", "big"};
  for (int i = 1; i <= 129; ++i) {
    zadd.push_back(std::to_string(i));
    zadd.push_back("m" + std::to_string(i));
  }
  bed.run(zadd);
  EXPECT_EQ(encoding("big"), bulk("skiplist"));
  bed.run({"ZRANGESTORE", "all", "big", "0", "-1"});
  EXPECT_EQ(encoding("all"), bulk("skiplist"));
  bed.run({"ZRANGESTORE", "two", "big", "0", "1"});
  EXPECT_EQ(encoding("two"), bulk("listpack"));
  bed.run({"ZINTERSTORE", "inter", "2", "big", "big"});
  EXPECT_EQ(encoding("inter"), bulk("skiplist"));
  bed.run({"ZDIFFSTORE", "diff", "2", "big", "two"});
  EXPECT_EQ(encoding("diff"), bulk("listpack"));
  bed.run({"ZPOPMIN", "big", "127"});
  EXPECT_EQ(encoding("big"), bulk("skiplist"));
  EXPECT_EQ(static_cast<int>(static_cast<unsigned char>(payload("big")[0])), 0x05);
  EXPECT_EQ(bed.run({"RESTORE", "restored", "0", payload("big")}), ok());
  EXPECT_EQ(encoding("restored"), bulk("listpack"));
  bed.run({"ZADD", "long", "1", std::string(70, 'x'), "2", "b"});
  EXPECT_EQ(encoding("long"), bulk("skiplist"));
  bed.run({"ZRANGESTORE", "short", "long", "1", "1"});
  EXPECT_EQ(encoding("short"), bulk("listpack"));
  bed.run({"ZADD", "inc", "1", "a"});
  bed.run({"ZINCRBY", "inc", "1", std::string(70, 'x')});
  EXPECT_EQ(encoding("inc"), bulk("skiplist"));
  bed.run({"GEOADD", "geo", "13.361389", "38.115556", std::string(70, 'x')});
  EXPECT_EQ(encoding("geo"), bulk("skiplist"));

  // A lowered limit: only a new member converts.
  bed.run({"ZADD", "z", "1", "a", "2", "b", "3", "c"});
  EXPECT_EQ(bed.run({"CONFIG", "SET", "zset-max-listpack-entries", "2"}), ok());
  bed.run({"ZADD", "z", "9", "a"});
  bed.run({"ZINCRBY", "z", "1", "b"});
  EXPECT_EQ(encoding("z"), bulk("listpack"));
  bed.run({"ZREM", "z", "c"});
  EXPECT_EQ(encoding("z"), bulk("listpack"));
  bed.run({"ZADD", "z", "4", "d"});
  EXPECT_EQ(encoding("z"), bulk("skiplist"));
  EXPECT_EQ(bed.run({"CONFIG", "SET", "zset-max-listpack-entries", "128", "zset-max-listpack-value", "3"}), ok());
  bed.run({"ZADD", "y", "1", "a"});
  bed.run({"ZADD", "y", "2", "abcd"});
  EXPECT_EQ(encoding("y"), bulk("skiplist"));
  EXPECT_EQ(bed.run({"CONFIG", "SET", "zset-max-listpack-value", "64"}), ok());
}

TEST(CommandZsets, RandMemberWithoutACountOrWithNone) {
  Bed bed;
  bed.run({"ZADD", "z", "1", "a"});
  EXPECT_EQ(bed.run({"ZRANDMEMBER", "z"}), bulk("a"));
  EXPECT_EQ(bed.run({"ZRANDMEMBER", "z", "0"}), array({}));
  EXPECT_EQ(bed.run({"ZRANDMEMBER", "nosuch", "3"}), array({}));
  EXPECT_EQ(bed.run({"ZRANGEBYSCORE", "z", "0", "1", "BOGUS"}), error("ERR syntax error"));
}

}  // namespace
}  // namespace mimir::test
