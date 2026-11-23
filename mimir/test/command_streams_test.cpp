//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Streams, the log itself, from Valkey 8's documentation ("Streams" and each
// command's page): ids from the wall clock or given, ranges with their
// special bounds, deletion, trimming, reading after an id, and XREAD's
// blocking read woken by an XADD.

#include <gtest/gtest.h>

#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

using namespace std::chrono_literals;

std::string entry(const std::string& id, const std::vector<std::string>& fields) {
  std::vector<std::string> bulks;
  for (const auto& f : fields) bulks.push_back(bulk(f));
  return array({bulk(id), array(bulks)});
}

TEST(CommandStreams, AddGivesIdsFromTheWallClock) {
  Bed bed;
  const std::string now = std::to_string(bed.wall.unix_ms());
  EXPECT_EQ(bed.run({"XADD", "s", "*", "name", "Sara"}), bulk(now + "-0"));
  EXPECT_EQ(bed.run({"XADD", "s", "*", "name", "Jo"}), bulk(now + "-1"));
  bed.wall.advance(5ms);
  EXPECT_EQ(bed.run({"XADD", "s", "*", "x", "1"}), bulk(std::to_string(bed.wall.unix_ms()) + "-0"));
  EXPECT_EQ(bed.run({"XLEN", "s"}), integer(3));
  EXPECT_EQ(bed.run({"TYPE", "s"}), "+stream\r\n");
}

// Valkey 8.1.1's answers (probed): XSETID's options and their refusals in
// Valkey's order, XADD's LIMIT, and ids past the last possible.
TEST(CommandStreams, SetidAndTheLastIdsAsValkeyAnswers) {
  Bed bed;
  for (const char* id : {"1-1", "2-1"}) bed.run({"XADD", "s", id, "f", "v"});
  EXPECT_EQ(bed.run({"XADD", "s", "MAXLEN", "~", "1", "LIMIT", "100", "3-1", "f", "v"}), bulk("3-1"));
  EXPECT_EQ(bed.run({"XLEN", "s"}), integer(3));
  EXPECT_EQ(bed.run({"XADD", "s", "MAXLEN", "1", "LIMIT", "100", "4-1", "f", "v"}),
            error("ERR syntax error, LIMIT cannot be used without the special ~ option"));
  EXPECT_EQ(bed.run({"XRANGE", "s", "-", "+", "COUNT"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"XREAD", "BOGUS", "STREAMS", "s", "0"}), error("ERR syntax error"));

  EXPECT_EQ(bed.run({"XSETID", "s", "5-0", "ENTRIESADDED", "10", "MAXDELETEDID", "4-0"}), ok());
  EXPECT_EQ(bed.run({"XINFO", "STREAM", "s"}), array({bulk("length"),
                                                      integer(3),
                                                      bulk("radix-tree-keys"),
                                                      integer(1),
                                                      bulk("radix-tree-nodes"),
                                                      integer(2),
                                                      bulk("last-generated-id"),
                                                      bulk("5-0"),
                                                      bulk("max-deleted-entry-id"),
                                                      bulk("4-0"),
                                                      bulk("entries-added"),
                                                      integer(10),
                                                      bulk("recorded-first-entry-id"),
                                                      bulk("1-1"),
                                                      bulk("groups"),
                                                      integer(0),
                                                      bulk("first-entry"),
                                                      entry("1-1", {"f", "v"}),
                                                      bulk("last-entry"),
                                                      entry("3-1", {"f", "v"})}));
  EXPECT_EQ(bed.run({"XSETID", "s", "5-0", "ENTRIESADDED", "x"}), error("ERR value is not an integer or out of range"));
  EXPECT_EQ(bed.run({"XSETID", "s", "5-0", "ENTRIESADDED", "-1"}), error("ERR entries_added must be positive"));
  EXPECT_EQ(bed.run({"XSETID", "s", "5-0", "BOGUS"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"XSETID", "s", "5-0", "MAXDELETEDID", "6-0"}), error("ERR The ID specified in XSETID is smaller than the provided max_deleted_entry_id"));
  EXPECT_EQ(bed.run({"XSETID", "s", "5-0", "ENTRIESADDED", "0"}), error("ERR The entries_added specified in XSETID is smaller than the target stream length"));
  EXPECT_EQ(bed.run({"XSETID", "s", "1-0"}), error("ERR The ID specified in XSETID is smaller than current max_deleted_entry_id"));
  EXPECT_EQ(bed.run({"XSETID", "nokey", "5-0"}), error("ERR no such key"));

  EXPECT_EQ(bed.run({"XADD", "m", "18446744073709551615-18446744073709551615", "f", "v"}), bulk("18446744073709551615-18446744073709551615"));
  EXPECT_EQ(bed.run({"XADD", "m", "*", "f", "v"}), error("ERR The stream has exhausted the last possible ID, unable to add more items"));
  // A last id ahead of the clock, its sequence used up: the next millisecond.
  EXPECT_EQ(bed.run({"XADD", "n", "9999999999999-18446744073709551615", "f", "v"}), bulk("9999999999999-18446744073709551615"));
  EXPECT_EQ(bed.run({"XADD", "n", "*", "f", "v"}), bulk("10000000000000-0"));
}

TEST(CommandStreams, AddWithAGivenIdAndItsRefusals) {
  Bed bed;
  EXPECT_EQ(bed.run({"XADD", "s", "5-1", "a", "1"}), bulk("5-1"));
  EXPECT_EQ(bed.run({"XADD", "s", "5-*", "a", "2"}), bulk("5-2"));
  EXPECT_EQ(bed.run({"XADD", "s", "7-*", "a", "3"}), bulk("7-0"));
  EXPECT_EQ(bed.run({"XADD", "s", "6-0", "a", "4"}), error("ERR The ID specified in XADD is equal or smaller than the target stream top item"));
  EXPECT_EQ(bed.run({"XADD", "t", "0-0", "a", "1"}), error("ERR The ID specified in XADD must be greater than 0-0"));
  EXPECT_EQ(bed.run({"XADD", "t", "x-1", "a", "1"}), error("ERR Invalid stream ID specified as stream command argument"));
  EXPECT_EQ(bed.run({"XADD", "t", "*", "a"}), error("ERR wrong number of arguments for 'xadd' command"));
  EXPECT_EQ(bed.run({"XADD", "t", "NOMKSTREAM", "*", "a", "1"}), nil());
  EXPECT_EQ(bed.run({"EXISTS", "t"}), integer(0));
  EXPECT_EQ(bed.run({"XADD", "u", "0-*", "a", "1"}), bulk("0-1"));
}

TEST(CommandStreams, Ranges) {
  Bed bed;
  bed.run({"XADD", "s", "1-0", "a", "1"});
  bed.run({"XADD", "s", "2-0", "b", "2"});
  bed.run({"XADD", "s", "2-1", "c", "3"});
  bed.run({"XADD", "s", "3-0", "d", "4"});
  EXPECT_EQ(bed.run({"XRANGE", "s", "-", "+"}),
            array({entry("1-0", {"a", "1"}), entry("2-0", {"b", "2"}), entry("2-1", {"c", "3"}), entry("3-0", {"d", "4"})}));
  EXPECT_EQ(bed.run({"XRANGE", "s", "2", "2"}), array({entry("2-0", {"b", "2"}), entry("2-1", {"c", "3"})}));
  EXPECT_EQ(bed.run({"XRANGE", "s", "(2-0", "+", "COUNT", "1"}), array({entry("2-1", {"c", "3"})}));
  EXPECT_EQ(bed.run({"XREVRANGE", "s", "+", "-", "COUNT", "2"}), array({entry("3-0", {"d", "4"}), entry("2-1", {"c", "3"})}));
  EXPECT_EQ(bed.run({"XREVRANGE", "s", "(3-0", "(1-0"}), array({entry("2-1", {"c", "3"}), entry("2-0", {"b", "2"})}));
  EXPECT_EQ(bed.run({"XRANGE", "s", "3", "1"}), array({}));
  EXPECT_EQ(bed.run({"XRANGE", "nosuch", "-", "+"}), array({}));
  EXPECT_EQ(bed.run({"XRANGE", "s", "(-", "+"}), error("ERR Invalid stream ID specified as stream command argument"));
  EXPECT_EQ(bed.run({"XRANGE", "s", "-", "+", "COUNT", "0"}), "*-1\r\n");
  EXPECT_EQ(bed.run({"XRANGE", "s", "x", "+"}), error("ERR Invalid stream ID specified as stream command argument"));
}

TEST(CommandStreams, DeleteAndTrim) {
  Bed bed;
  for (int i = 1; i <= 10; ++i) bed.run({"XADD", "s", std::to_string(i) + "-0", "n", std::to_string(i)});
  EXPECT_EQ(bed.run({"XDEL", "s", "1-0", "2-0", "99-0"}), integer(2));
  EXPECT_EQ(bed.run({"XLEN", "s"}), integer(8));
  EXPECT_EQ(bed.run({"XTRIM", "s", "MAXLEN", "5"}), integer(3));
  EXPECT_EQ(bed.run({"XRANGE", "s", "-", "+", "COUNT", "1"}), array({entry("6-0", {"n", "6"})}));
  EXPECT_EQ(bed.run({"XTRIM", "s", "MINID", "8"}), integer(2));
  EXPECT_EQ(bed.run({"XLEN", "s"}), integer(3));
  // An approximate trim removes whole nodes only: these all sit in one.
  EXPECT_EQ(bed.run({"XTRIM", "s", "MAXLEN", "~", "1"}), integer(0));
  EXPECT_EQ(bed.run({"XTRIM", "s", "MAXLEN", "=", "1", "LIMIT", "5"}), error("ERR syntax error, LIMIT cannot be used without the special ~ option"));
  EXPECT_EQ(bed.run({"XTRIM", "s", "MAXLEN", "-1"}), error("ERR The MAXLEN argument must be >= 0."));
  EXPECT_EQ(bed.run({"XADD", "s", "MAXLEN", "2", "20-0", "n", "20"}), bulk("20-0"));
  EXPECT_EQ(bed.run({"XLEN", "s"}), integer(2));
}

TEST(CommandStreams, ApproximateTrimRemovesWholeNodes) {
  Bed bed;
  for (int i = 1; i <= 250; ++i) bed.run({"XADD", "s", std::to_string(i) + "-0", "n", "x"});
  // Nodes of 100: removing the first leaves 150 >= 120, the second would
  // leave 50 < 120.
  EXPECT_EQ(bed.run({"XTRIM", "s", "MAXLEN", "~", "120"}), integer(100));
  EXPECT_EQ(bed.run({"XLEN", "s"}), integer(150));
}

TEST(CommandStreams, ReadAfterAnId) {
  Bed bed;
  bed.run({"XADD", "a", "1-0", "x", "1"});
  bed.run({"XADD", "a", "2-0", "x", "2"});
  bed.run({"XADD", "b", "5-0", "y", "5"});
  EXPECT_EQ(bed.run({"XREAD", "COUNT", "1", "STREAMS", "a", "b", "0", "0"}),
            array({array({bulk("a"), array({entry("1-0", {"x", "1"})})}), array({bulk("b"), array({entry("5-0", {"y", "5"})})})}));
  EXPECT_EQ(bed.run({"XREAD", "STREAMS", "a", "2-0"}), "*-1\r\n");
  EXPECT_EQ(bed.run({"XREAD", "STREAMS", "a", "$"}), "*-1\r\n");
  EXPECT_EQ(bed.run({"XREAD", "STREAMS", "a", "b", "0"}), error("ERR Unbalanced 'xread' list of streams: for each stream key an ID or '$' must be specified."));
  EXPECT_EQ(bed.run({"XREAD", "STREAMS", "a", ">"}),
            error("ERR The > ID can be specified only when calling XREADGROUP using the GROUP <group> <consumer> option."));
  bed.run({"HELLO", "3"});
  EXPECT_EQ(bed.run({"XREAD", "STREAMS", "a", "1-0"}), "%1\r\n" + bulk("a") + array({entry("2-0", {"x", "2"})}));
}

TEST(CommandStreams, ABlockedReadIsWokenByAnAddAndDollarMeansThenNotNow) {
  Bed bed;
  bed.run({"XADD", "s", "1-0", "old", "1"});
  command::Session reader;
  reader.id = bed.database.next_client_id();
  std::optional<std::string> woken;
  std::string out;
  auto executed = bed.database.execute(reader, {"XREAD", "BLOCK", "0", "STREAMS", "s", "$"}, out, [&](std::string reply) { woken = std::move(reply); });
  ASSERT_TRUE(executed.wait.has_value());
  EXPECT_EQ(out, "");
  // An unrelated write wakes nothing.
  bed.run({"SET", "other", "1"});
  EXPECT_FALSE(woken.has_value());
  bed.run({"XADD", "s", "2-0", "new", "2"});
  EXPECT_EQ(woken, array({array({bulk("s"), array({entry("2-0", {"new", "2"})})})}));
  EXPECT_EQ(bed.database.waiting(), 0u);
}

TEST(CommandStreams, SetId) {
  Bed bed;
  bed.run({"XADD", "s", "5-0", "a", "1"});
  EXPECT_EQ(bed.run({"XSETID", "s", "10-0"}), ok());
  EXPECT_EQ(bed.run({"XADD", "s", "9-0", "a", "1"}), error("ERR The ID specified in XADD is equal or smaller than the target stream top item"));
  EXPECT_EQ(bed.run({"XSETID", "s", "1-0"}), error("ERR The ID specified in XSETID is smaller than the target stream top item"));
  EXPECT_EQ(bed.run({"XSETID", "nosuch", "1-0"}), error("ERR no such key"));
}

// The agreed stream-node-max-entries and -bytes end a node.
TEST(CommandStreams, TheNodeLimitsAreTheAgreedOnes) {
  Bed bed;
  const auto keys = [&](const std::string& key) {
    const std::string info = bed.run({"XINFO", "STREAM", key});
    return std::stoi(info.substr(info.find("radix-tree-keys\r\n:") + 18));
  };
  EXPECT_EQ(bed.run({"CONFIG", "SET", "stream-node-max-entries", "2"}), ok());
  for (const char* id : {"1-1", "2-1", "3-1"}) bed.run({"XADD", "a", id, "f", "v"});
  EXPECT_EQ(keys("a"), 2);
  EXPECT_EQ(bed.run({"CONFIG", "SET", "stream-node-max-entries", "100", "stream-node-max-bytes", "64"}), ok());
  for (const char* id : {"1-1", "2-1", "3-1"}) bed.run({"XADD", "b", id, "f", std::string(30, 'x')});
  EXPECT_EQ(keys("b"), 3);
}

// Where a stream's nodes end, as Valkey 8.1.1 ends them (probed, through
// XINFO STREAM's radix-tree-keys after each append): at 4,096 listpack bytes
// as well as at 100 entries; a deleted entry keeps its bytes; a restored
// stream's last node is filled by its bytes; an approximate trim takes whole
// nodes.
TEST(CommandStreams, NodesEndByBytesAsValkeysDo) {
  Bed bed;
  const auto keys = [&](const std::string& key) {
    const std::string info = bed.run({"XINFO", "STREAM", key});
    const size_t at = info.find("radix-tree-keys\r\n:");
    return at == std::string::npos ? -1 : std::stoi(info.substr(at + 18));
  };
  const auto nodes_after = [&](const std::string& key, size_t value_size, int appends) {
    std::string seen;
    for (int i = 1; i <= appends; ++i) {
      bed.run({"XADD", key, std::to_string(i) + "-1", "f", std::string(value_size, 'x')});
      seen += " " + std::to_string(keys(key));
    }
    return seen;
  };
  EXPECT_EQ(nodes_after("a", 900, 10), " 1 1 1 1 2 2 2 2 3 3");
  EXPECT_EQ(nodes_after("b100", 100, 12), " 1 1 1 1 1 1 1 1 1 1 1 1");
  EXPECT_EQ(nodes_after("b500", 500, 12), " 1 1 1 1 1 1 1 2 2 2 2 2");
  EXPECT_EQ(nodes_after("b1000", 1000, 12), " 1 1 1 1 2 2 2 2 3 3 3 3");
  EXPECT_EQ(nodes_after("b1500", 1500, 12), " 1 1 2 2 3 3 4 4 5 5 6 6");
  EXPECT_EQ(nodes_after("b2000", 2000, 12), " 1 1 2 2 3 3 4 4 5 5 6 6");
  EXPECT_EQ(nodes_after("b3000", 3000, 12), " 1 2 3 4 5 6 7 8 9 10 11 12");
  EXPECT_EQ(nodes_after("b5000", 5000, 4), " 1 2 3 4");
  // Fields other than the first entry's.
  std::string two;
  for (int i = 1; i <= 10; ++i) {
    bed.run({"XADD", "c", std::to_string(i) + "-1", "g" + std::to_string(i), std::string(600, 'x'), "h" + std::to_string(i), "x"});
    two += " " + std::to_string(keys("c"));
  }
  EXPECT_EQ(two, " 1 1 1 1 1 1 2 2 2 2");
  // A deleted entry keeps its bytes.
  const std::string k1000(1000, 'x');
  for (const char* id : {"1-1", "2-1", "3-1"}) bed.run({"XADD", "d", id, "f", k1000});
  bed.run({"XDEL", "d", "2-1", "3-1"});
  std::string deleted;
  for (const char* id : {"4-1", "5-1", "6-1"}) {
    bed.run({"XADD", "d", id, "f", k1000});
    deleted += " " + std::to_string(keys("d"));
  }
  EXPECT_EQ(deleted, " 1 2 2");
  // A restored stream's last node is filled by its bytes.
  const std::string k1500(1500, 'x');
  bed.run({"XADD", "r", "1-1", "f", k1500});
  bed.run({"XADD", "r", "2-1", "f", k1500});
  const std::string dumped = bed.run({"DUMP", "r"});
  const size_t body = dumped.find("\r\n") + 2;
  bed.run({"DEL", "r"});
  EXPECT_EQ(bed.run({"RESTORE", "r", "0", dumped.substr(body, dumped.size() - body - 2)}), ok());
  std::string restored = " " + std::to_string(keys("r"));
  for (const char* id : {"3-1", "4-1", "5-1"}) {
    bed.run({"XADD", "r", id, "f", std::string(500, 'x')});
    restored += " " + std::to_string(keys("r"));
  }
  EXPECT_EQ(restored, " 1 1 1 2");
  // An approximate trim takes whole nodes, two entries each here.
  for (int i = 1; i <= 7; ++i) bed.run({"XADD", "t", std::to_string(i) + "-1", "f", k1500});
  EXPECT_EQ(keys("t"), 4);
  EXPECT_EQ(bed.run({"XTRIM", "t", "MAXLEN", "~", "5"}), integer(2));
  EXPECT_EQ(bed.run({"XLEN", "t"}), integer(5));
  EXPECT_EQ(keys("t"), 3);
  EXPECT_EQ(bed.run({"XTRIM", "t", "MAXLEN", "~", "2"}), integer(2));
  EXPECT_EQ(bed.run({"XLEN", "t"}), integer(3));
  EXPECT_EQ(keys("t"), 2);
}

}  // namespace
}  // namespace mimir::test
