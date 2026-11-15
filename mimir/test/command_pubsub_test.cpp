//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Publish and subscribe, from Valkey 8's documentation ("Pub/Sub"): the
// confirmations and messages and their shapes in RESP2 and RESP3, patterns,
// shard channels, the subscribed mode of a RESP2 connection, and PUBSUB's
// introspection.

#include <gtest/gtest.h>

#include <vector>

#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

std::string bulks(const std::vector<std::string>& items) {
  std::vector<std::string> out;
  for (const auto& item : items) out.push_back(bulk(item));
  return array(out);
}

// A connection whose pushes are recorded.
struct Listener {
  explicit Listener(Bed& bed_) : bed(bed_) {
    session.id = bed.database.next_client_id();
    session.push = [this](std::string bytes) { pushed.push_back(std::move(bytes)); };
  }
  std::string run(std::vector<std::string> args) {
    std::string out;
    bed.database.execute(session, args, out);
    return out;
  }
  Bed& bed;
  command::Session session;
  std::vector<std::string> pushed;
};

TEST(CommandPubSub, SubscribeConfirmsEachChannelWithTheCount) {
  Bed bed;
  Listener l(bed);
  EXPECT_EQ(l.run({"SUBSCRIBE", "first", "second"}), "");
  ASSERT_EQ(l.pushed.size(), 2u);
  EXPECT_EQ(l.pushed[0], array({bulk("subscribe"), bulk("first"), integer(1)}));
  EXPECT_EQ(l.pushed[1], array({bulk("subscribe"), bulk("second"), integer(2)}));
  l.run({"SUBSCRIBE", "first"});
  EXPECT_EQ(l.pushed[2], array({bulk("subscribe"), bulk("first"), integer(2)}));
}

TEST(CommandPubSub, PublishReachesChannelsAndPatterns) {
  Bed bed;
  Listener exact(bed);
  Listener pattern(bed);
  exact.run({"SUBSCRIBE", "news.art"});
  pattern.run({"PSUBSCRIBE", "news.*"});
  EXPECT_EQ(pattern.pushed[0], array({bulk("psubscribe"), bulk("news.*"), integer(1)}));
  EXPECT_EQ(bed.run({"PUBLISH", "news.art", "hello"}), integer(2));
  EXPECT_EQ(exact.pushed.back(), bulks({"message", "news.art", "hello"}));
  EXPECT_EQ(pattern.pushed.back(), bulks({"pmessage", "news.*", "news.art", "hello"}));
  EXPECT_EQ(bed.run({"PUBLISH", "sports", "x"}), integer(0));
}

TEST(CommandPubSub, Resp3MessagesArePushes) {
  Bed bed;
  Listener l(bed);
  l.run({"HELLO", "3"});
  l.run({"SUBSCRIBE", "c"});
  EXPECT_EQ(l.pushed[0], ">3\r\n" + bulk("subscribe") + bulk("c") + integer(1));
  bed.run({"PUBLISH", "c", "m"});
  EXPECT_EQ(l.pushed[1], ">3\r\n" + bulk("message") + bulk("c") + bulk("m"));
  // RESP3 is not held to subscribed mode.
  EXPECT_EQ(l.run({"GET", "k"}), "_\r\n");
}

TEST(CommandPubSub, UnsubscribeEndsOneOrAll) {
  Bed bed;
  Listener l(bed);
  l.run({"SUBSCRIBE", "a", "b", "c"});
  l.pushed.clear();
  l.run({"UNSUBSCRIBE", "b"});
  EXPECT_EQ(l.pushed[0], array({bulk("unsubscribe"), bulk("b"), integer(2)}));
  l.run({"UNSUBSCRIBE"});
  EXPECT_EQ(l.pushed.size(), 3u);
  EXPECT_EQ(l.pushed[2], array({bulk("unsubscribe"), bulk("c"), integer(0)}));
  l.run({"UNSUBSCRIBE"});
  EXPECT_EQ(l.pushed[3], array({bulk("unsubscribe"), nil(), integer(0)}));
  EXPECT_EQ(bed.run({"PUBLISH", "a", "x"}), integer(0));
}

TEST(CommandPubSub, SubscribedModeTakesOnlyItsCommands) {
  Bed bed;
  Listener l(bed);
  l.run({"SUBSCRIBE", "c"});
  EXPECT_EQ(l.run({"GET", "k"}), error("ERR Can't execute 'get': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context"));
  EXPECT_EQ(l.run({"PING"}), bulks({"pong", ""}));
  EXPECT_EQ(l.run({"PING", "hi"}), bulks({"pong", "hi"}));
  EXPECT_EQ(l.run({"RESET"}), "+RESET\r\n");
  EXPECT_EQ(l.run({"GET", "k"}), nil());
  EXPECT_EQ(bed.run({"PUBLISH", "c", "x"}), integer(0));
}

TEST(CommandPubSub, ShardChannels) {
  Bed bed;
  Listener l(bed);
  l.run({"SSUBSCRIBE", "orders"});
  EXPECT_EQ(l.pushed[0], array({bulk("ssubscribe"), bulk("orders"), integer(1)}));
  EXPECT_EQ(bed.run({"SPUBLISH", "orders", "1"}), integer(1));
  EXPECT_EQ(l.pushed[1], bulks({"smessage", "orders", "1"}));
  // A shard channel and a channel of one name are apart.
  EXPECT_EQ(bed.run({"PUBLISH", "orders", "2"}), integer(0));
  l.run({"SUNSUBSCRIBE"});
  EXPECT_EQ(l.pushed[2], array({bulk("sunsubscribe"), bulk("orders"), integer(0)}));
}

TEST(CommandPubSub, Introspection) {
  Bed bed;
  Listener a(bed);
  Listener b(bed);
  a.run({"SUBSCRIBE", "x", "y"});
  b.run({"SUBSCRIBE", "x"});
  b.run({"PSUBSCRIBE", "p*", "q*"});
  EXPECT_EQ(bed.run({"PUBSUB", "CHANNELS"}), bulks({"x", "y"}));
  EXPECT_EQ(bed.run({"PUBSUB", "CHANNELS", "y*"}), bulks({"y"}));
  EXPECT_EQ(bed.run({"PUBSUB", "NUMSUB", "x", "y", "z"}), array({bulk("x"), integer(2), bulk("y"), integer(1), bulk("z"), integer(0)}));
  EXPECT_EQ(bed.run({"PUBSUB", "NUMPAT"}), integer(2));
  EXPECT_EQ(bed.run({"PUBSUB", "SHARDCHANNELS"}), array({}));
  EXPECT_EQ(bed.run({"PUBSUB", "BOGUS"}), error("ERR unknown subcommand 'BOGUS'. Try PUBSUB HELP."));
  bed.database.pubsub().drop(b.session.id);
  EXPECT_EQ(bed.run({"PUBSUB", "NUMPAT"}), integer(0));
}

}  // namespace
}  // namespace mimir::test
