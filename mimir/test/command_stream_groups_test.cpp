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
