//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Transactions, from Valkey 8's documentation ("Transactions"): commands
// queued between MULTI and EXEC run together; an error while queueing aborts
// the EXEC; an error while running fails that command alone; WATCH makes
// EXEC refuse when a watched key changed since, by a write, a flush or its
// expiry.

#include <gtest/gtest.h>

#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

using namespace std::chrono_literals;

struct Other {
  explicit Other(Bed& bed_) : bed(bed_) { session.id = bed.database.next_client_id(); }
  std::string run(std::vector<std::string> args) {
    std::string out;
    bed.database.execute(session, args, out);
    return out;
  }
  Bed& bed;
  command::Session session;
};

const std::string queued = "+QUEUED\r\n";

TEST(CommandTransactions, QueuedCommandsRunTogether) {
  Bed bed;
  EXPECT_EQ(bed.run({"MULTI"}), ok());
  EXPECT_EQ(bed.run({"INCR", "foo"}), queued);
  EXPECT_EQ(bed.run({"INCR", "bar"}), queued);
  EXPECT_EQ(bed.run({"EXEC"}), array({integer(1), integer(1)}));
}

TEST(CommandTransactions, AnErrorWhileQueueingAbortsTheExec) {
  Bed bed;
  bed.run({"MULTI"});
  EXPECT_EQ(bed.run({"SET", "a"}), error("ERR wrong number of arguments for 'set' command"));
  EXPECT_EQ(bed.run({"SET", "b", "1"}), queued);
  EXPECT_EQ(bed.run({"EXEC"}), error("EXECABORT Transaction discarded because of previous errors."));
  EXPECT_EQ(bed.run({"EXISTS", "b"}), integer(0));
  bed.run({"MULTI"});
  bed.run({"NOSUCHCOMMAND"});
  EXPECT_EQ(bed.run({"EXEC"}), error("EXECABORT Transaction discarded because of previous errors."));
}

TEST(CommandTransactions, AnErrorWhileRunningFailsOnlyItsCommand) {
  Bed bed;
  bed.run({"MULTI"});
  bed.run({"SET", "a", "abc"});
  bed.run({"LPOP", "a"});
  bed.run({"SET", "b", "2"});
  EXPECT_EQ(bed.run({"EXEC"}), array({ok(), error("WRONGTYPE Operation against a key holding the wrong kind of value"), ok()}));
  EXPECT_EQ(bed.run({"GET", "b"}), bulk("2"));
}

TEST(CommandTransactions, DiscardAndTheMisuses) {
  Bed bed;
  EXPECT_EQ(bed.run({"EXEC"}), error("ERR EXEC without MULTI"));
  EXPECT_EQ(bed.run({"DISCARD"}), error("ERR DISCARD without MULTI"));
  bed.run({"MULTI"});
  bed.run({"SET", "k", "v"});
  EXPECT_EQ(bed.run({"DISCARD"}), ok());
  EXPECT_EQ(bed.run({"EXISTS", "k"}), integer(0));
  // MULTI and WATCH inside a transaction are refused, and abort its EXEC.
  bed.run({"MULTI"});
  EXPECT_EQ(bed.run({"MULTI"}), error("ERR Command not allowed inside a transaction"));
  EXPECT_EQ(bed.run({"WATCH", "k"}), error("ERR Command not allowed inside a transaction"));
  bed.run({"SET", "k", "v"});
  EXPECT_EQ(bed.run({"EXEC"}), error("EXECABORT Transaction discarded because of previous errors."));
}

TEST(CommandTransactions, WatchRefusesAfterAnotherClientsWrite) {
  Bed bed;
  Other other(bed);
  bed.run({"SET", "mykey", "1"});
  EXPECT_EQ(bed.run({"WATCH", "mykey"}), ok());
  other.run({"INCR", "mykey"});
  bed.run({"MULTI"});
  bed.run({"SET", "mykey", "100"});
  EXPECT_EQ(bed.run({"EXEC"}), "*-1\r\n");
  EXPECT_EQ(bed.run({"GET", "mykey"}), bulk("2"));
  // The watch ended with that EXEC: the next one runs.
  bed.run({"MULTI"});
  bed.run({"SET", "mykey", "100"});
  EXPECT_EQ(bed.run({"EXEC"}), array({ok()}));
}

TEST(CommandTransactions, AnUntouchedWatchLetsExecRun) {
  Bed bed;
  Other other(bed);
  bed.run({"WATCH", "a", "b"});
  other.run({"SET", "c", "1"});
  other.run({"GET", "a"});
  bed.run({"MULTI"});
  bed.run({"SET", "a", "1"});
  EXPECT_EQ(bed.run({"EXEC"}), array({ok()}));
}

TEST(CommandTransactions, UnwatchAndFlushAndExpiry) {
  Bed bed;
  Other other(bed);
  bed.run({"WATCH", "k"});
  other.run({"SET", "k", "1"});
  EXPECT_EQ(bed.run({"UNWATCH"}), ok());
  bed.run({"MULTI"});
  bed.run({"GET", "k"});
  EXPECT_EQ(bed.run({"EXEC"}), array({bulk("1")}));

  bed.run({"WATCH", "k"});
  other.run({"FLUSHALL"});
  bed.run({"MULTI"});
  EXPECT_EQ(bed.run({"EXEC"}), "*-1\r\n");

  bed.run({"SET", "t", "v", "PX", "100"});
  bed.run({"WATCH", "t"});
  bed.wall.advance(100ms);
  bed.run({"MULTI"});
  bed.run({"PING"});
  EXPECT_EQ(bed.run({"EXEC"}), "*-1\r\n");
}

TEST(CommandTransactions, ABlockingCommandInATransactionDoesNotWait) {
  Bed bed;
  bed.run({"MULTI"});
  bed.run({"BLPOP", "empty", "0"});
  EXPECT_EQ(bed.run({"EXEC"}), array({"*-1\r\n"}));
}

}  // namespace
}  // namespace mimir::test
