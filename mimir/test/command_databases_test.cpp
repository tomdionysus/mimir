//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Several databases (ruling 21), from Valkey 8's documentation for SELECT,
// MOVE, SWAPDB, COPY ... DB, FLUSHDB and FLUSHALL: each database its own
// keyspace, and the waits and watches on a key kept within its database.

#include <gtest/gtest.h>

#include "dannan/test/fixtures/stepped_time.h"
#include "mimir/command/database.h"
#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

struct Sixteen : Bed {
  Sixteen() : Bed(settings()) {}

  static command::DatabaseSettings settings() {
    command::DatabaseSettings s;
    s.databases = 16;
    return s;
  }
  command::Session make() { return make_session(); }
  std::string run(command::Session& on, std::vector<std::string> args) { return run_as(on, std::move(args)); }
  std::string run(std::vector<std::string> args) { return run_as(session, std::move(args)); }
};

TEST(CommandDatabases, OneByDefault) {
  Bed bed;
  EXPECT_EQ(bed.run({"SELECT", "0"}), ok());
  EXPECT_EQ(bed.run({"SELECT", "1"}), error("ERR DB index is out of range"));
  EXPECT_EQ(bed.run({"CONFIG", "GET", "databases"}), array({bulk("databases"), bulk("1")}));
}

TEST(CommandDatabases, EachDatabaseIsItsOwnKeyspace) {
  Sixteen s;
  s.run({"SET", "k", "zero"});
  EXPECT_EQ(s.run({"SELECT", "3"}), ok());
  EXPECT_EQ(s.run({"GET", "k"}), nil());
  s.run({"SET", "k", "three"});
  EXPECT_EQ(s.run({"DBSIZE"}), integer(1));
  s.run({"SELECT", "0"});
  EXPECT_EQ(s.run({"GET", "k"}), bulk("zero"));
  EXPECT_EQ(s.run({"SELECT", "16"}), error("ERR DB index is out of range"));
  const std::string info = s.run({"INFO", "keyspace"});
  EXPECT_NE(info.find("db0:keys=1"), std::string::npos);
  EXPECT_NE(info.find("db3:keys=1"), std::string::npos);
}

TEST(CommandDatabases, MoveSwapCopyFlush) {
  Sixteen s;
  s.run({"SET", "a", "1", "EX", "100"});
  EXPECT_EQ(s.run({"MOVE", "a", "1"}), integer(1));
  EXPECT_EQ(s.run({"EXISTS", "a"}), integer(0));
  EXPECT_EQ(s.run({"MOVE", "a", "1"}), integer(0));
  EXPECT_EQ(s.run({"MOVE", "a", "0"}), error("ERR source and destination objects are the same"));
  EXPECT_EQ(s.run({"MOVE", "a", "99"}), error("ERR DB index is out of range"));
  s.run({"SELECT", "1"});
  EXPECT_EQ(s.run({"TTL", "a"}), integer(100));
  EXPECT_EQ(s.run({"COPY", "a", "a", "DB", "2"}), integer(1));
  EXPECT_EQ(s.run({"COPY", "a", "a"}), error("ERR source and destination objects are the same"));
  EXPECT_EQ(s.run({"SWAPDB", "1", "5"}), ok());
  EXPECT_EQ(s.run({"EXISTS", "a"}), integer(0));
  s.run({"SELECT", "5"});
  EXPECT_EQ(s.run({"EXISTS", "a"}), integer(1));
  EXPECT_EQ(s.run({"SWAPDB", "x", "1"}), error("ERR invalid first DB index"));
  EXPECT_EQ(s.run({"SWAPDB", "1", "99"}), error("ERR DB index is out of range"));
  EXPECT_EQ(s.run({"FLUSHDB"}), ok());
  EXPECT_EQ(s.run({"DBSIZE"}), integer(0));
  s.run({"SELECT", "2"});
  EXPECT_EQ(s.run({"DBSIZE"}), integer(1));
  EXPECT_EQ(s.run({"FLUSHALL"}), ok());
  EXPECT_EQ(s.run({"DBSIZE"}), integer(0));
}

TEST(CommandDatabases, WaitsAndWatchesStayInTheirDatabase) {
  Sixteen s;
  command::Session other = s.make();
  // A watch on k in database 0 is not broken by k in database 1.
  s.run({"WATCH", "k"});
  s.run(other, {"SELECT", "1"});
  s.run(other, {"SET", "k", "1"});
  s.run({"MULTI"});
  s.run({"PING"});
  EXPECT_EQ(s.run({"EXEC"}), array({"+PONG\r\n"}));
  // A wait on q in database 0 is not served by q in database 1.
  std::optional<std::string> woken;
  std::string out;
  s.database.execute(s.session, {"BLPOP", "q", "0"}, out, [&](std::string r) { woken = std::move(r); });
  s.run(other, {"RPUSH", "q", "elsewhere"});
  EXPECT_FALSE(woken.has_value());
  s.run(other, {"MOVE", "q", "0"});
  EXPECT_EQ(woken, array({bulk("q"), bulk("elsewhere")}));
}

// A watch breaks when MOVE or COPY ... DB writes its key into its database,
// and when SWAPDB swaps its database while its key exists in either of the
// two (Valkey's touchAllWatchedKeysInDb); a watch on a key in neither stands.
TEST(CommandDatabases, MoveCopyAndSwapdbBreakTheWatchesTheyTouch) {
  Sixteen s;
  command::Session other = s.make();
  const auto exec_after = [&](const std::string& db, const std::string& key, const std::vector<std::string>& write) {
    s.run({"SELECT", db});
    s.run({"WATCH", key});
    s.run(other, write);
    s.run({"MULTI"});
    s.run({"PING"});
    return s.run({"EXEC"});
  };
  s.run(other, {"SET", "m", "v"});
  EXPECT_EQ(exec_after("1", "m", {"MOVE", "m", "1"}), "*-1\r\n");
  s.run(other, {"SET", "c", "v"});
  EXPECT_EQ(exec_after("2", "c", {"COPY", "c", "c", "DB", "2"}), "*-1\r\n");
  s.run(other, {"SELECT", "4"});
  s.run(other, {"SET", "w", "v"});
  EXPECT_EQ(exec_after("3", "w", {"SWAPDB", "3", "4"}), "*-1\r\n");
  EXPECT_EQ(exec_after("3", "absent", {"SWAPDB", "3", "4"}), array({"+PONG\r\n"}));
}

}  // namespace
}  // namespace mimir::test
