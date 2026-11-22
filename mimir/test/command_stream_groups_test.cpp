//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Streams' consumer groups, from Valkey 8's documentation ("Streams",
// "Consumer groups" and each command's page): creating groups, reading new
// entries and a consumer's history, the pending entries list and its
// acknowledgement, claiming idle entries, and the lag XINFO reports.

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

struct Bed3 : Bed {
  Bed3() {
    run({"XADD", "s", "1-0", "a", "1"});
    run({"XADD", "s", "2-0", "b", "2"});
    run({"XADD", "s", "3-0", "c", "3"});
  }
};

TEST(CommandStreamGroups, CreateAndItsRefusals) {
  Bed3 bed;
  EXPECT_EQ(bed.run({"XGROUP", "CREATE", "s", "g", "0"}), ok());
  EXPECT_EQ(bed.run({"XGROUP", "CREATE", "s", "g", "$"}), error("BUSYGROUP Consumer Group name already exists"));
  EXPECT_EQ(bed.run({"XGROUP", "CREATE", "nosuch", "g", "$"}),
            error("ERR The XGROUP subcommand requires the key to exist. Note that for CREATE you may want to use the MKSTREAM option to create an "
                  "empty stream automatically."));
  EXPECT_EQ(bed.run({"XGROUP", "CREATE", "made", "g", "$", "MKSTREAM"}), ok());
  EXPECT_EQ(bed.run({"XLEN", "made"}), integer(0));
  EXPECT_EQ(bed.run({"XGROUP", "DESTROY", "made", "g"}), integer(1));
  EXPECT_EQ(bed.run({"XGROUP", "DESTROY", "made", "g"}), integer(0));
  EXPECT_EQ(bed.run({"XGROUP", "CREATECONSUMER", "s", "g", "alice"}), integer(1));
  EXPECT_EQ(bed.run({"XGROUP", "CREATECONSUMER", "s", "g", "alice"}), integer(0));
  EXPECT_EQ(bed.run({"XGROUP", "BOGUS"}), error("ERR unknown subcommand 'BOGUS'. Try XGROUP HELP."));
}

TEST(CommandStreamGroups, ReadNewThenHistoryThenAcknowledge) {
  Bed3 bed;
  bed.run({"XGROUP", "CREATE", "s", "g", "0"});
  EXPECT_EQ(bed.run({"XREADGROUP", "GROUP", "g", "alice", "COUNT", "2", "STREAMS", "s", ">"}),
            array({array({bulk("s"), array({entry("1-0", {"a", "1"}), entry("2-0", {"b", "2"})})})}));
  EXPECT_EQ(bed.run({"XREADGROUP", "GROUP", "g", "bob", "STREAMS", "s", ">"}), array({array({bulk("s"), array({entry("3-0", {"c", "3"})})})}));
  EXPECT_EQ(bed.run({"XREADGROUP", "GROUP", "g", "bob", "STREAMS", "s", ">"}), "*-1\r\n");
  // Alice's history: what she was given and has not acknowledged.
  EXPECT_EQ(bed.run({"XREADGROUP", "GROUP", "g", "alice", "STREAMS", "s", "0"}),
            array({array({bulk("s"), array({entry("1-0", {"a", "1"}), entry("2-0", {"b", "2"})})})}));
  EXPECT_EQ(bed.run({"XACK", "s", "g", "1-0", "9-0"}), integer(1));
  EXPECT_EQ(bed.run({"XPENDING", "s", "g"}),
            array({integer(2), bulk("2-0"), bulk("3-0"), array({array({bulk("alice"), bulk("1")}), array({bulk("bob"), bulk("1")})})}));
  // A deleted entry still pending reads back as its id and nothing.
  bed.run({"XDEL", "s", "2-0"});
  EXPECT_EQ(bed.run({"XREADGROUP", "GROUP", "g", "alice", "STREAMS", "s", "0"}), array({array({bulk("s"), array({array({bulk("2-0"), "*-1\r\n"})})})}));
}

TEST(CommandStreamGroups, MissingGroupsAndBadIds) {
  Bed3 bed;
  EXPECT_EQ(bed.run({"XREADGROUP", "GROUP", "nog", "c", "STREAMS", "s", ">"}),
            error("NOGROUP No such key 's' or consumer group 'nog' in XREADGROUP with GROUP option"));
  bed.run({"XGROUP", "CREATE", "s", "g", "$"});
  EXPECT_EQ(bed.run({"XREADGROUP", "GROUP", "g", "c", "STREAMS", "s", "$"}).substr(0, 25), "-ERR The $ ID is meaningl");
  EXPECT_EQ(bed.run({"XPENDING", "s", "nog"}), error("NOGROUP No such key 's' or consumer group 'nog'"));
  EXPECT_EQ(bed.run({"XACK", "s", "nog", "1-0"}), integer(0));
  EXPECT_EQ(bed.run({"XPENDING", "s", "g"}), array({integer(0), nil(), nil(), "*-1\r\n"}));
}

TEST(CommandStreamGroups, PendingInDetailAndClaims) {
  Bed3 bed;
  bed.run({"XGROUP", "CREATE", "s", "g", "0"});
  bed.run({"XREADGROUP", "GROUP", "g", "alice", "STREAMS", "s", ">"});
  bed.wall.advance(5000ms);
  EXPECT_EQ(bed.run({"XPENDING", "s", "g", "-", "+", "10", "alice"}),
            array({array({bulk("1-0"), bulk("alice"), integer(5000), integer(1)}), array({bulk("2-0"), bulk("alice"), integer(5000), integer(1)}),
                   array({bulk("3-0"), bulk("alice"), integer(5000), integer(1)})}));
  EXPECT_EQ(bed.run({"XPENDING", "s", "g", "IDLE", "6000", "-", "+", "10"}), array({}));
  // Bob claims what has been idle an hour: nothing; then what has been idle a second.
  EXPECT_EQ(bed.run({"XCLAIM", "s", "g", "bob", "3600000", "1-0"}), array({}));
  EXPECT_EQ(bed.run({"XCLAIM", "s", "g", "bob", "1000", "1-0"}), array({entry("1-0", {"a", "1"})}));
  EXPECT_EQ(bed.run({"XPENDING", "s", "g", "-", "+", "1"}), array({array({bulk("1-0"), bulk("bob"), integer(0), integer(2)})}));
  EXPECT_EQ(bed.run({"XCLAIM", "s", "g", "carol", "0", "2-0", "JUSTID"}), array({bulk("2-0")}));
  EXPECT_EQ(bed.run({"XAUTOCLAIM", "s", "g", "dave", "1000", "0", "COUNT", "5"}), array({bulk("0-0"), array({entry("3-0", {"c", "3"})}), array({})}));
  bed.run({"XDEL", "s", "1-0"});
  EXPECT_EQ(bed.run({"XAUTOCLAIM", "s", "g", "dave", "0", "0", "JUSTID"}), array({bulk("0-0"), array({bulk("2-0"), bulk("3-0")}), array({bulk("1-0")})}));
  EXPECT_EQ(bed.run({"XGROUP", "DELCONSUMER", "s", "g", "dave"}), integer(2));
  EXPECT_EQ(bed.run({"XCLAIM", "s", "g", "x", "y", "1-0"}), error("ERR Invalid min-idle-time argument for XCLAIM"));
}

TEST(CommandStreamGroups, InfoReportsTheLag) {
  Bed3 bed;
  bed.run({"XGROUP", "CREATE", "s", "g", "0"});
  bed.run({"XREADGROUP", "GROUP", "g", "c", "COUNT", "1", "STREAMS", "s", ">"});
  const std::string groups = bed.run({"XINFO", "GROUPS", "s"});
  EXPECT_NE(groups.find(bulk("entries-read") + integer(1)), std::string::npos) << groups;
  EXPECT_NE(groups.find(bulk("lag") + integer(2)), std::string::npos) << groups;
  const std::string stream = bed.run({"XINFO", "STREAM", "s"});
  EXPECT_NE(stream.find(bulk("length") + integer(3)), std::string::npos) << stream;
  EXPECT_NE(stream.find(bulk("entries-added") + integer(3)), std::string::npos) << stream;
  EXPECT_EQ(bed.run({"XINFO", "STREAM", "nosuch"}), error("ERR no such key"));
}

// The rest of this file's cases are Valkey 8.1.1's answers (probed) to the same
// commands, on entries 1-1, 2-1 and 3-1 {f v}.
struct BedV : Bed {
  BedV() {
    for (const char* id : {"1-1", "2-1", "3-1"}) run({"XADD", "s", id, "f", "v"});
    run({"XGROUP", "CREATE", "s", "g", "0"});
  }
  // XINFO GROUPS' one group, in RESP2's flattened map.
  static std::string group(const std::string& name, int consumers, int pending, const std::string& last, const std::string& read, const std::string& lag) {
    return array({array({bulk("name"), bulk(name), bulk("consumers"), integer(consumers), bulk("pending"), integer(pending), bulk("last-delivered-id"),
                         bulk(last), bulk("entries-read"), read, bulk("lag"), lag})});
  }
};

TEST(CommandStreamGroups, SetidMovesTheGroupAndItsCountOfEntriesRead) {
  BedV bed;
  EXPECT_EQ(bed.run({"XGROUP", "SETID", "s", "g", "2-1"}), ok());
  EXPECT_EQ(bed.run({"XINFO", "GROUPS", "s"}), BedV::group("g", 0, 0, "2-1", nil(), nil()));
  EXPECT_EQ(bed.run({"XGROUP", "SETID", "s", "g", "$", "ENTRIESREAD", "3"}), ok());
  EXPECT_EQ(bed.run({"XINFO", "GROUPS", "s"}), BedV::group("g", 0, 0, "3-1", integer(3), integer(0)));
  EXPECT_EQ(bed.run({"XGROUP", "SETID", "s", "g", "0", "ENTRIESREAD", "-1"}), ok());
  EXPECT_EQ(bed.run({"XINFO", "GROUPS", "s"}), BedV::group("g", 0, 0, "0-0", nil(), integer(3)));

  const std::string unknown = error("ERR unknown subcommand or wrong number of arguments for 'SETID'. Try XGROUP HELP.");
  EXPECT_EQ(bed.run({"XGROUP", "SETID", "s", "g", "0", "ENTRIESREAD", "x"}), error("ERR value is not an integer or out of range"));
  EXPECT_EQ(bed.run({"XGROUP", "SETID", "s", "g", "0", "ENTRIESREAD", "-5"}), error("ERR value for ENTRIESREAD must be positive or -1"));
  EXPECT_EQ(bed.run({"XGROUP", "SETID", "s", "g", "0", "BOGUS", "1"}), unknown);
  EXPECT_EQ(bed.run({"XGROUP", "SETID", "s", "g", "0", "ENTRIESREAD"}), unknown);
  EXPECT_EQ(bed.run({"XGROUP", "SETID", "s", "nog", "0"}), error("NOGROUP No such consumer group 'nog' for key name 's'"));
  EXPECT_EQ(bed.run({"XGROUP", "SETID", "nokey", "g", "0"}),
            error("ERR The XGROUP subcommand requires the key to exist. Note that for CREATE you may want to use the MKSTREAM option to create an empty "
                  "stream automatically."));
  EXPECT_EQ(bed.run({"XGROUP", "CREATE", "s", "g2", "0", "ENTRIESREAD", "x"}), error("ERR value is not an integer or out of range"));
  EXPECT_EQ(bed.run({"XGROUP", "CREATE", "s", "g2", "0", "ENTRIESREAD", "-2"}), error("ERR value for ENTRIESREAD must be positive or -1"));
  EXPECT_EQ(bed.run({"XGROUP", "DELCONSUMER", "s", "g", "nobody"}), integer(0));
}

// An entry given again by '>' after SETID moves to its new reader; NOACK
// leaves the pending list as it was.
TEST(CommandStreamGroups, RedeliveryOwnsAndNoackLeavesPending) {
  BedV bed;
  bed.run({"XREADGROUP", "GROUP", "g", "c", "COUNT", "2", "STREAMS", "s", ">"});
  bed.run({"XGROUP", "SETID", "s", "g", "0"});
  EXPECT_EQ(bed.run({"XREADGROUP", "GROUP", "g", "c2", "COUNT", "1", "STREAMS", "s", ">"}), array({array({bulk("s"), array({entry("1-1", {"f", "v"})})})}));
  const std::string pending = array({integer(2), bulk("1-1"), bulk("2-1"), array({array({bulk("c"), bulk("1")}), array({bulk("c2"), bulk("1")})})});
  EXPECT_EQ(bed.run({"XPENDING", "s", "g"}), pending);
  EXPECT_EQ(bed.run({"XREADGROUP", "GROUP", "g", "c3", "NOACK", "STREAMS", "s", ">"}),
            array({array({bulk("s"), array({entry("2-1", {"f", "v"}), entry("3-1", {"f", "v"})})})}));
  EXPECT_EQ(bed.run({"XPENDING", "s", "g"}), pending);
  EXPECT_EQ(bed.run({"XREADGROUP", "GROUP", "g", "c", "BOGUS", "STREAMS", "s", ">"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"XACK", "nokey", "g", "1-1"}), integer(0));
  EXPECT_EQ(bed.run({"XACK", "s", "nog", "1-1"}), integer(0));
}

TEST(CommandStreamGroups, PendingBoundsAndClaimOptions) {
  BedV bed;
  bed.run({"XREADGROUP", "GROUP", "g", "c", "COUNT", "2", "STREAMS", "s", ">"});
  bed.run({"XGROUP", "SETID", "s", "g", "0"});
  bed.run({"XREADGROUP", "GROUP", "g", "c2", "COUNT", "1", "STREAMS", "s", ">"});
  bed.run({"XREADGROUP", "GROUP", "g", "c3", "NOACK", "STREAMS", "s", ">"});
  EXPECT_EQ(bed.run({"XPENDING", "s", "g", "IDLE"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"XPENDING", "s", "g", "(+", "+", "10"}), error("ERR Invalid stream ID specified as stream command argument"));
  EXPECT_EQ(bed.run({"XPENDING", "s", "g", "(18446744073709551615-18446744073709551615", "+", "10"}), error("ERR invalid start ID for the interval"));
  EXPECT_EQ(bed.run({"XPENDING", "s", "g", "-", "(0-0", "10"}), error("ERR invalid end ID for the interval"));
  bed.wall.advance(2ms);
  EXPECT_EQ(bed.run({"XPENDING", "s", "g", "(1-1", "+", "10", "c"}), array({array({bulk("2-1"), bulk("c"), integer(2), integer(1)})}));
  EXPECT_EQ(bed.run({"XPENDING", "s", "g", "-", "(2-0", "10", "c2"}), array({array({bulk("1-1"), bulk("c2"), integer(2), integer(1)})}));

  EXPECT_EQ(bed.run({"XCLAIM", "s", "g", "c", "0", "2-1", "JUSTID", "RETRYCOUNT", "7", "LASTID", "3-1"}), array({bulk("2-1")}));
  EXPECT_EQ(bed.run({"XINFO", "GROUPS", "s"}), BedV::group("g", 3, 2, "3-1", integer(3), integer(0)));
  EXPECT_EQ(bed.run({"XCLAIM", "s", "g", "c", "0", "2-1", "BOGUS"}), error("ERR Unrecognized XCLAIM option 'BOGUS'"));
  EXPECT_EQ(bed.run({"XCLAIM", "s", "g", "c", "0", "3-1", "FORCE", "JUSTID"}), array({bulk("3-1")}));
  EXPECT_EQ(bed.run({"XPENDING", "s", "g"}),
            array({integer(3), bulk("1-1"), bulk("3-1"), array({array({bulk("c"), bulk("2")}), array({bulk("c2"), bulk("1")})})}));
  // A deleted entry is not claimed, and leaves the pending list.
  bed.run({"XDEL", "s", "2-1"});
  EXPECT_EQ(bed.run({"XCLAIM", "s", "g", "c", "0", "2-1", "JUSTID"}), array({}));
  EXPECT_EQ(bed.run({"XPENDING", "s", "g"}),
            array({integer(2), bulk("1-1"), bulk("3-1"), array({array({bulk("c"), bulk("1")}), array({bulk("c2"), bulk("1")})})}));
  EXPECT_EQ(bed.run({"XAUTOCLAIM", "s", "g", "c", "0", "0", "BOGUS"}), error("ERR syntax error"));
  // IDLE sets how long the claimed entry has been idle (XCLAIM's page).
  EXPECT_EQ(bed.run({"XCLAIM", "s", "g", "c2", "0", "1-1", "IDLE", "5", "JUSTID"}), array({bulk("1-1")}));
  EXPECT_EQ(bed.run({"XPENDING", "s", "g", "-", "+", "10", "c2"}), array({array({bulk("1-1"), bulk("c2"), integer(5), integer(1)})}));
}

TEST(CommandStreamGroups, InfoOfConsumersAnEmptyStreamAndAnUnknownLag) {
  BedV bed;
  EXPECT_EQ(bed.run({"XINFO", "CONSUMERS", "s"}), error("ERR wrong number of arguments for 'xinfo|consumers' command"));
  EXPECT_EQ(bed.run({"XINFO", "CONSUMERS", "nokey", "g"}), error("ERR no such key"));
  EXPECT_EQ(bed.run({"XINFO", "CONSUMERS", "s", "nog"}), error("NOGROUP No such consumer group 'nog' for key name 's'"));
  EXPECT_EQ(bed.run({"XINFO", "BOGUS"}), error("ERR unknown subcommand 'BOGUS'. Try XINFO HELP."));
  // XINFO CONSUMERS' page: idle is since the consumer was last seen,
  // inactive since it last read or claimed, -1 when it never has.
  bed.run({"XREADGROUP", "GROUP", "g", "c", "COUNT", "2", "STREAMS", "s", ">"});
  bed.wall.advance(5ms);
  bed.run({"XGROUP", "CREATECONSUMER", "s", "g", "d"});
  EXPECT_EQ(bed.run({"XINFO", "CONSUMERS", "s", "g"}),
            array({array({bulk("name"), bulk("c"), bulk("pending"), integer(2), bulk("idle"), integer(5), bulk("inactive"), integer(5)}),
                   array({bulk("name"), bulk("d"), bulk("pending"), integer(0), bulk("idle"), integer(0), bulk("inactive"), integer(-1)})}));

  bed.run({"XADD", "e", "1-1", "f", "v"});
  bed.run({"XDEL", "e", "1-1"});
  EXPECT_EQ(bed.run({"XINFO", "STREAM", "e"}), array({bulk("length"),
                                                      integer(0),
                                                      bulk("radix-tree-keys"),
                                                      integer(0),
                                                      bulk("radix-tree-nodes"),
                                                      integer(1),
                                                      bulk("last-generated-id"),
                                                      bulk("1-1"),
                                                      bulk("max-deleted-entry-id"),
                                                      bulk("1-1"),
                                                      bulk("entries-added"),
                                                      integer(1),
                                                      bulk("recorded-first-entry-id"),
                                                      bulk("0-0"),
                                                      bulk("groups"),
                                                      integer(0),
                                                      bulk("first-entry"),
                                                      nil(),
                                                      bulk("last-entry"),
                                                      nil()}));

  // A deletion after what the group has read makes its lag unknown.
  for (const char* id : {"1-1", "2-1", "3-1", "4-1"}) bed.run({"XADD", "t", id, "f", "v"});
  bed.run({"XGROUP", "CREATE", "t", "g", "0"});
  bed.run({"XREADGROUP", "GROUP", "g", "c", "COUNT", "1", "STREAMS", "t", ">"});
  bed.run({"XDEL", "t", "3-1"});
  EXPECT_EQ(bed.run({"XINFO", "GROUPS", "t"}), BedV::group("g", 1, 1, "1-1", integer(1), nil()));
}

TEST(CommandStreamGroups, ABlockedGroupReadIsWokenByAnAdd) {
  Bed3 bed;
  bed.run({"XGROUP", "CREATE", "s", "g", "$"});
  command::Session reader;
  reader.id = bed.database.next_client_id();
  std::optional<std::string> woken;
  std::string out;
  auto executed =
      bed.database.execute(reader, {"XREADGROUP", "GROUP", "g", "c", "BLOCK", "0", "STREAMS", "s", ">"}, out, [&](std::string r) { woken = std::move(r); });
  ASSERT_TRUE(executed.wait.has_value());
  bed.run({"XADD", "s", "4-0", "d", "4"});
  EXPECT_EQ(woken, array({array({bulk("s"), array({entry("4-0", {"d", "4"})})})}));
  EXPECT_EQ(bed.run({"XPENDING", "s", "g"}), array({integer(1), bulk("4-0"), bulk("4-0"), array({array({bulk("c"), bulk("1")})})}));
}

}  // namespace
}  // namespace mimir::test
