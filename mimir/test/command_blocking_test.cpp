//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The blocking pops, from Valkey 8's command documentation (BLPOP's "Blocking
// behavior" and "What key is served first? What client? What element?
// Priority ordering details"): a command that finds something answers at
// once; one that finds nothing waits until a write gives it something, the
// longest-waiting client first; a wait ends at its timeout with a null reply;
// where it may not wait it answers that at once.

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

std::string bulks(const std::vector<std::string>& items) {
  std::vector<std::string> out;
  for (const auto& item : items) out.push_back(bulk(item));
  return array(out);
}

// A second connection on the same database, whose waits record their wake.
struct Waiter {
  explicit Waiter(Bed& bed_) : bed(bed_) {}

  Bed& bed;
  command::Session session = make();
  std::optional<std::string> woken;
  std::optional<uint64_t> wait;

  command::Session make() {
    command::Session made;
    made.id = bed.database.next_client_id();
    return made;
  }
  // Runs a command that may park; its reply once its agreement is decided,
  // if it has one, or none while it waits.
  std::string run(std::vector<std::string> args) {
    std::string out;
    woken.reset();
    auto executed = bed.database.execute(session, args, out, [this](std::string reply) { woken = std::move(reply); });
    wait = executed.wait;
    bed.settle();
    if (wait && woken) {
      wait.reset();
      return *std::exchange(woken, std::nullopt);
    }
    return out;
  }
};

TEST(CommandBlocking, AnswersAtOnceWhenSomethingIsThere) {
  Bed bed;
  bed.run({"RPUSH", "list1", "a", "b", "c"});
  Waiter w{bed};
  EXPECT_EQ(w.run({"BLPOP", "list2", "list1", "0"}), bulks({"list1", "a"}));
  EXPECT_FALSE(w.wait.has_value());
  EXPECT_EQ(w.run({"BRPOP", "list1", "0"}), bulks({"list1", "c"}));
}

TEST(CommandBlocking, WithNowhereToWaitItAnswersItsTimeoutAtOnce) {
  Bed bed;
  EXPECT_EQ(bed.run_unwaited({"BLPOP", "empty", "0"}), "*-1\r\n");
  EXPECT_EQ(bed.run_unwaited({"BLMOVE", "empty", "dst", "LEFT", "RIGHT", "1"}), nil());
  EXPECT_EQ(bed.database.waiting(), 0u);
}

TEST(CommandBlocking, AWriteWakesTheWaiter) {
  Bed bed;
  Waiter w{bed};
  EXPECT_EQ(w.run({"BLPOP", "a", "b", "0"}), "");
  ASSERT_TRUE(w.wait.has_value());
  EXPECT_EQ(bed.database.waiting(), 1u);
  EXPECT_EQ(bed.run({"RPUSH", "b", "x", "y"}), integer(2));
  EXPECT_EQ(w.woken, bulks({"b", "x"}));
  EXPECT_EQ(bed.database.waiting(), 0u);
  EXPECT_EQ(bed.run({"LRANGE", "b", "0", "-1"}), bulks({"y"}));
}

TEST(CommandBlocking, TheLongestWaitingIsServedFirst) {
  Bed bed;
  Waiter first{bed};
  Waiter second{bed};
  first.run({"BRPOP", "q", "0"});
  second.run({"BRPOP", "q", "0"});
  bed.run({"LPUSH", "q", "one"});
  EXPECT_EQ(first.woken, bulks({"q", "one"}));
  EXPECT_FALSE(second.woken.has_value());
  bed.run({"LPUSH", "q", "two"});
  EXPECT_EQ(second.woken, bulks({"q", "two"}));
}

TEST(CommandBlocking, OnePushServesAsManyWaitersAsItHasElements) {
  Bed bed;
  Waiter first{bed};
  Waiter second{bed};
  first.run({"BLPOP", "q", "0"});
  second.run({"BLPOP", "q", "0"});
  bed.run({"RPUSH", "q", "1", "2", "3"});
  EXPECT_EQ(first.woken, bulks({"q", "1"}));
  EXPECT_EQ(second.woken, bulks({"q", "2"}));
  EXPECT_EQ(bed.run({"LRANGE", "q", "0", "-1"}), bulks({"3"}));
}

TEST(CommandBlocking, ATimeoutEndsTheWaitOnceAndNotAfterServing) {
  Bed bed;
  Waiter w{bed};
  w.run({"BLPOP", "q", "1.5"});
  ASSERT_TRUE(w.wait.has_value());
  EXPECT_EQ(bed.database.cancel_wait(*w.wait), "*-1\r\n");
  EXPECT_EQ(bed.database.cancel_wait(*w.wait), std::nullopt);
  // A push after the timeout is not taken by the ended wait.
  bed.run({"RPUSH", "q", "kept"});
  EXPECT_FALSE(w.woken.has_value());
  EXPECT_EQ(bed.run({"LLEN", "q"}), integer(1));

  Waiter served{bed};
  served.run({"BLPOP", "r", "1"});
  bed.run({"RPUSH", "r", "x"});
  EXPECT_EQ(bed.database.cancel_wait(*served.wait), std::nullopt);
}

TEST(CommandBlocking, BlMoveWaitsOnItsSourceAndWakesTheNextWaiterOnItsDestination) {
  Bed bed;
  Waiter mover{bed};
  Waiter reader{bed};
  EXPECT_EQ(mover.run({"BLMOVE", "src", "dst", "LEFT", "RIGHT", "0"}), "");
  EXPECT_EQ(reader.run({"BLPOP", "dst", "0"}), "");
  bed.run({"RPUSH", "src", "item"});
  EXPECT_EQ(mover.woken, bulk("item"));
  // The move made dst ready, and the reader took it.
  EXPECT_EQ(reader.woken, bulks({"dst", "item"}));
  EXPECT_EQ(bed.run({"EXISTS", "src", "dst"}), integer(0));
  // The timeout reply is a null bulk string.
  Waiter late{bed};
  late.run({"BRPOPLPUSH", "none", "x", "1"});
  EXPECT_EQ(bed.database.cancel_wait(*late.wait), nil());
}

TEST(CommandBlocking, AKeyOfAnotherTypeServesNoWait) {
  Bed bed;
  Waiter w{bed};
  w.run({"BLPOP", "k", "0"});
  bed.run({"SET", "k", "a string"});
  EXPECT_FALSE(w.woken.has_value());
  EXPECT_EQ(bed.database.waiting(), 1u);
  bed.run({"DEL", "k"});
  bed.run({"RPUSH", "k", "now a list"});
  EXPECT_EQ(w.woken, bulks({"k", "now a list"}));
  // At once, another type is WRONGTYPE as for any list command.
  bed.run({"SET", "s", "v"});
  EXPECT_EQ(bed.run({"BLPOP", "s", "0"}), error("WRONGTYPE Operation against a key holding the wrong kind of value"));
}

TEST(CommandBlocking, SortedSetPops) {
  Bed bed;
  bed.run({"ZADD", "zset1", "0", "a", "1", "b", "2", "c"});
  Waiter w{bed};
  EXPECT_EQ(w.run({"BZPOPMIN", "zset1", "zset2", "0"}), bulks({"zset1", "a", "0"}));
  EXPECT_EQ(w.run({"BZPOPMAX", "zset1", "0"}), bulks({"zset1", "c", "2"}));
  w.run({"BZPOPMIN", "zset3", "0"});
  bed.run({"ZADD", "zset3", "5", "late"});
  EXPECT_EQ(w.woken, bulks({"zset3", "late", "5"}));
  Waiter m{bed};
  m.run({"BZMPOP", "0", "2", "x", "y", "MAX", "COUNT", "2"});
  bed.run({"ZADD", "y", "1", "p", "2", "q"});
  EXPECT_EQ(m.woken, array({bulk("y"), array({bulks({"q", "2"}), bulks({"p", "1"})})}));
}

// A woken XREAD runs again over every stream it names, as Valkey 8.1.1 reruns
// it (probed): two streams written at once are both answered, in the order
// named; a key that became another type keeps it waiting, and is WRONGTYPE
// once a stream it names wakes it.
TEST(CommandBlocking, AWokenXreadRunsAgainOverEveryStream) {
  Bed bed;
  Waiter both{bed};
  EXPECT_EQ(both.run({"XREAD", "BLOCK", "0", "STREAMS", "s1", "s2", "0-0", "0-0"}), "");
  bed.run({"MULTI"});
  bed.run({"XADD", "s2", "2-1", "f", "v"});
  bed.run({"XADD", "s1", "1-1", "f", "v"});
  bed.run({"EXEC"});
  EXPECT_EQ(both.woken, array({array({bulk("s1"), array({array({bulk("1-1"), bulks({"f", "v"})})})}),
                               array({bulk("s2"), array({array({bulk("2-1"), bulks({"f", "v"})})})})}));

  Waiter typed{bed};
  EXPECT_EQ(typed.run({"XREAD", "BLOCK", "0", "STREAMS", "u", "w", "0-0", "0-0"}), "");
  bed.run({"SET", "u", "str"});
  EXPECT_FALSE(typed.woken.has_value());
  bed.run({"XADD", "w", "1-1", "f", "v"});
  EXPECT_EQ(typed.woken, error("WRONGTYPE Operation against a key holding the wrong kind of value"));
}

TEST(CommandBlocking, ListMPop) {
  Bed bed;
  Waiter w{bed};
  w.run({"BLMPOP", "0", "2", "a", "b", "RIGHT", "COUNT", "5"});
  bed.run({"RPUSH", "b", "1", "2"});
  EXPECT_EQ(w.woken, array({bulk("b"), bulks({"2", "1"})}));
  EXPECT_EQ(bed.run({"BLMPOP", "0", "0", "a", "LEFT"}), error("ERR numkeys should be greater than 0"));
}

TEST(CommandBlocking, TimeoutsAreCheckedAsValkeyChecksThem) {
  Bed bed;
  EXPECT_EQ(bed.run({"BLPOP", "q", "-1"}), error("ERR timeout is negative"));
  EXPECT_EQ(bed.run({"BLPOP", "q", "soon"}), error("ERR timeout is not a float or out of range"));
  EXPECT_EQ(bed.run({"BZPOPMIN", "q", "inf"}), error("ERR timeout is out of range"));
  EXPECT_EQ(bed.run({"BLMOVE", "a", "b", "UP", "LEFT", "0"}), error("ERR syntax error"));
  EXPECT_EQ(bed.database.waiting(), 0u);
}

}  // namespace
}  // namespace mimir::test
