//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// SORT and SORT_RO, from Valkey 8's documentation: numbers by default, ALPHA
// for bytes, DESC, LIMIT offset count, BY a pattern's values (with "->" a
// hash field; no '*' sorts nothing), GET patterns ("#" the element), STORE
// into a list.

#include <gtest/gtest.h>

#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

std::string bulks(const std::vector<std::string>& items) {
  std::vector<std::string> out;
  for (const auto& item : items) out.push_back(bulk(item));
  return array(out);
}

TEST(CommandSort, NumbersAlphaDescLimit) {
  Bed bed;
  bed.run({"RPUSH", "l", "3", "10", "1", "2.5"});
  EXPECT_EQ(bed.run({"SORT", "l"}), bulks({"1", "2.5", "3", "10"}));
  EXPECT_EQ(bed.run({"SORT", "l", "DESC"}), bulks({"10", "3", "2.5", "1"}));
  EXPECT_EQ(bed.run({"SORT", "l", "ALPHA"}), bulks({"1", "10", "2.5", "3"}));
  EXPECT_EQ(bed.run({"SORT", "l", "LIMIT", "1", "2"}), bulks({"2.5", "3"}));
  EXPECT_EQ(bed.run({"SORT", "l", "LIMIT", "10", "2"}), bulks({}));
  bed.run({"RPUSH", "words", "b", "a"});
  EXPECT_EQ(bed.run({"SORT", "words"}), error("ERR One or more scores can't be converted into double"));
  EXPECT_EQ(bed.run({"SORT", "nosuch"}), bulks({}));
  bed.run({"SET", "s", "x"});
  EXPECT_EQ(bed.run({"SORT", "s"}), error("WRONGTYPE Operation against a key holding the wrong kind of value"));
  EXPECT_EQ(bed.run({"SORT", "l", "LIMIT", "1"}), error("ERR syntax error"));
}

TEST(CommandSort, ByAndGet) {
  Bed bed;
  bed.run({"RPUSH", "ids", "1", "2", "3"});
  bed.run({"MSET", "w_1", "30", "w_2", "10", "w_3", "20", "name_1", "one", "name_2", "two"});
  bed.run({"HSET", "h_1", "age", "5"});
  bed.run({"HSET", "h_2", "age", "50"});
  bed.run({"HSET", "h_3", "age", "7"});
  EXPECT_EQ(bed.run({"SORT", "ids", "BY", "w_*"}), bulks({"2", "3", "1"}));
  EXPECT_EQ(bed.run({"SORT", "ids", "BY", "h_*->age", "DESC"}), bulks({"2", "3", "1"}));
  EXPECT_EQ(bed.run({"SORT", "ids", "BY", "nosort"}), bulks({"1", "2", "3"}));
  EXPECT_EQ(bed.run({"SORT", "ids", "BY", "w_*", "GET", "#", "GET", "name_*"}), array({bulk("2"), bulk("two"), bulk("3"), nil(), bulk("1"), bulk("one")}));
  EXPECT_EQ(bed.run({"SORT", "ids", "GET", "h_*->age"}), bulks({"5", "50", "7"}));
}

TEST(CommandSort, Store) {
  Bed bed;
  bed.run({"RPUSH", "l", "3", "1", "2"});
  bed.run({"SET", "name_1", "one"});
  EXPECT_EQ(bed.run({"SORT", "l", "GET", "name_*", "STORE", "out"}), integer(3));
  EXPECT_EQ(bed.run({"LRANGE", "out", "0", "-1"}), bulks({"one", "", ""}));
  EXPECT_EQ(bed.run({"SORT", "nosuch", "STORE", "out"}), integer(0));
  EXPECT_EQ(bed.run({"EXISTS", "out"}), integer(0));
  EXPECT_EQ(bed.run({"SORT_RO", "l", "STORE", "out"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"SORT_RO", "l"}), bulks({"1", "2", "3"}));
}

TEST(CommandSort, AnUnsortedSetIsSortedWhereItMustBeDeterministic) {
  Bed bed;
  bed.run({"SADD", "s", "c", "a", "b"});
  EXPECT_EQ(bed.run({"SORT", "s", "BY", "nosort", "STORE", "out"}), integer(3));
  EXPECT_EQ(bed.run({"LRANGE", "out", "0", "-1"}), bulks({"a", "b", "c"}));
  EXPECT_EQ(bed.run({"EVAL", "return redis.call('SORT', KEYS[1], 'BY', 'nosort')", "1", "s"}), bulks({"a", "b", "c"}));
}

}  // namespace
}  // namespace mimir::test
