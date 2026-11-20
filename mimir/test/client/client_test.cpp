//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The real-client gate (ruling 44): hiredis, as applications use it, against
// a real three-node Mimir cluster of mimir-node processes. What is required is
// Valkey 8's documented behaviour and the design's rulings, on whichever node
// a client talks to.

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mimir/test/client/cluster.h"

namespace mimir::test {
namespace {

// One cluster for the cases that leave every node running; each case keeps
// to keys of its own.
class ClientGate : public ::testing::Test {
 protected:
  static void SetUpTestSuite() { _cluster = std::make_unique<Cluster>(3); }
  static void TearDownTestSuite() { _cluster.reset(); }
  static Cluster& cluster() { return *_cluster; }

 private:
  static inline std::unique_ptr<Cluster> _cluster;
};

// The elements of a RESP3 map, or of a RESP2 array of pairs, by key.
std::map<std::string, const redisReply*> fields(const redisReply* reply) {
  std::map<std::string, const redisReply*> out;
  for (size_t i = 0; i + 1 < reply->elements; i += 2) out[text(reply->element[i])] = reply->element[i + 1];
  return out;
}

TEST_F(ClientGate, HelloThreeAnswersWithTheServersMapAndClientSetinfoIsShown) {
  Client client(cluster().port(0));
  const auto hello = client.command({"HELLO", "3"});
  ASSERT_TRUE(hello) << client.error();
  ASSERT_EQ(hello->type, REDIS_REPLY_MAP);
  const auto map = fields(hello.get());
  ASSERT_TRUE(map.count("proto"));
  EXPECT_EQ(map.at("proto")->type, REDIS_REPLY_INTEGER);
  EXPECT_EQ(map.at("proto")->integer, 3);
  ASSERT_TRUE(map.count("server"));
  EXPECT_FALSE(text(map.at("server")).empty());
  ASSERT_TRUE(map.count("id"));

  const auto name = client.command({"CLIENT", "SETINFO", "LIB-NAME", "hiredis-gate"});
  ASSERT_TRUE(name);
  EXPECT_EQ(text(name.get()), "OK");
  const auto version = client.command({"CLIENT", "SETINFO", "LIB-VER", "1.2.0"});
  ASSERT_TRUE(version);
  EXPECT_EQ(text(version.get()), "OK");
  const auto info = client.command({"CLIENT", "INFO"});
  ASSERT_TRUE(info);
  EXPECT_EQ(info->type, REDIS_REPLY_VERB);
  const auto line = text(info.get());
  EXPECT_NE(line.find("lib-name=hiredis-gate"), std::string::npos) << line;
  EXPECT_NE(line.find("lib-ver=1.2.0"), std::string::npos) << line;
  EXPECT_NE(line.find("resp=3"), std::string::npos) << line;
}

TEST_F(ClientGate, PipelinedCommandsAreAnsweredInOrderAndEveryNodeReadsThemStrictly) {
  constexpr int count = 100;
  Client writer(cluster().port(0));
  for (int i = 0; i < count; ++i) writer.append({"SET", "pipe:" + std::to_string(i), "value-" + std::to_string(i)});
  for (int i = 0; i < count; ++i) writer.append({"INCR", "pipe:counter"});
  for (int i = 0; i < count; ++i) {
    const auto reply = writer.get_reply();
    ASSERT_TRUE(reply) << writer.error();
    ASSERT_EQ(reply->type, REDIS_REPLY_STATUS) << i << ": " << text(reply.get());
    EXPECT_EQ(text(reply.get()), "OK");
  }
  for (int i = 0; i < count; ++i) {
    const auto reply = writer.get_reply();
    ASSERT_TRUE(reply) << writer.error();
    ASSERT_EQ(reply->type, REDIS_REPLY_INTEGER) << i << ": " << text(reply.get());
    EXPECT_EQ(reply->integer, i + 1);
  }
  // Ruling 37: a strict read is confirmed with a majority, so a write
  // acknowledged anywhere is read everywhere.
  for (size_t node = 1; node < cluster().size(); ++node) {
    Client reader(cluster().port(node));
    const auto strict = reader.command({"CLIENT", "STRICT", "ON"});
    ASSERT_TRUE(strict) << reader.error();
    ASSERT_EQ(text(strict.get()), "OK");
    for (int i = 0; i < count; ++i) reader.append({"GET", "pipe:" + std::to_string(i)});
    reader.append({"GET", "pipe:counter"});
    for (int i = 0; i < count; ++i) {
      const auto reply = reader.get_reply();
      ASSERT_TRUE(reply) << reader.error();
      EXPECT_EQ(text(reply.get()), "value-" + std::to_string(i)) << "node " << node;
    }
    const auto counter = reader.get_reply();
    ASSERT_TRUE(counter) << reader.error();
    EXPECT_EQ(text(counter.get()), std::to_string(count)) << "node " << node;
  }
}

TEST_F(ClientGate, RespThreeRepliesAreMapsSetsAndDoubles) {
  Client client(cluster().port(1));
  ASSERT_TRUE(client.command({"HELLO", "3"}));
  ASSERT_TRUE(client.command({"HSET", "resp3:hash", "field", "value"}));
  ASSERT_TRUE(client.command({"SADD", "resp3:set", "a"}));
  ASSERT_TRUE(client.command({"SADD", "resp3:set", "b"}));
  ASSERT_TRUE(client.command({"ZADD", "resp3:zset", "1.5", "member"}));

  const auto hash = client.command({"HGETALL", "resp3:hash"});
  ASSERT_TRUE(hash);
  ASSERT_EQ(hash->type, REDIS_REPLY_MAP);
  const auto map = fields(hash.get());
  ASSERT_TRUE(map.count("field"));
  EXPECT_EQ(text(map.at("field")), "value");

  const auto set = client.command({"SMEMBERS", "resp3:set"});
  ASSERT_TRUE(set);
  ASSERT_EQ(set->type, REDIS_REPLY_SET);
  std::set<std::string> members;
  for (size_t i = 0; i < set->elements; ++i) members.insert(text(set->element[i]));
  EXPECT_EQ(members, (std::set<std::string>{"a", "b"}));

  const auto score = client.command({"ZSCORE", "resp3:zset", "member"});
  ASSERT_TRUE(score);
  ASSERT_EQ(score->type, REDIS_REPLY_DOUBLE);
  EXPECT_DOUBLE_EQ(score->dval, 1.5);
}

TEST_F(ClientGate, AMessagePublishedOnOneNodeIsPushedToASubscriberOnAnother) {
  Client subscriber(cluster().port(0));
  ASSERT_TRUE(subscriber.command({"HELLO", "3"}));
  const auto confirmed = subscriber.command({"SUBSCRIBE", "resp3:channel"});
  ASSERT_TRUE(confirmed) << subscriber.error();
  ASSERT_EQ(confirmed->type, REDIS_REPLY_PUSH);
  ASSERT_EQ(confirmed->elements, 3u);
  EXPECT_EQ(text(confirmed->element[0]), "subscribe");

  Client publisher(cluster().port(2));
  ASSERT_TRUE(publisher.command({"PUBLISH", "resp3:channel", "pushed"}));
  const auto message = subscriber.get_reply();
  ASSERT_TRUE(message) << subscriber.error();
  ASSERT_EQ(message->type, REDIS_REPLY_PUSH);
  ASSERT_EQ(message->elements, 3u);
  EXPECT_EQ(text(message->element[0]), "message");
  EXPECT_EQ(text(message->element[1]), "resp3:channel");
  EXPECT_EQ(text(message->element[2]), "pushed");
}

TEST_F(ClientGate, RespTwoSubscribedModeReceivesMessagesPublishedOnAnotherNode) {
  Client subscriber(cluster().port(1));
  const auto confirmed = subscriber.command({"SUBSCRIBE", "resp2:channel"});
  ASSERT_TRUE(confirmed) << subscriber.error();
  ASSERT_EQ(confirmed->type, REDIS_REPLY_ARRAY);
  EXPECT_EQ(text(confirmed->element[0]), "subscribe");

  // Subscribed mode takes only the subscription commands and PING.
  const auto refused = subscriber.command({"GET", "resp2:key"});
  ASSERT_TRUE(refused);
  EXPECT_EQ(refused->type, REDIS_REPLY_ERROR);

  Client publisher(cluster().port(0));
  ASSERT_TRUE(publisher.command({"PUBLISH", "resp2:channel", "arrived"}));
  const auto message = subscriber.get_reply();
  ASSERT_TRUE(message) << subscriber.error();
  ASSERT_EQ(message->type, REDIS_REPLY_ARRAY);
  ASSERT_EQ(message->elements, 3u);
  EXPECT_EQ(text(message->element[0]), "message");
  EXPECT_EQ(text(message->element[1]), "resp2:channel");
  EXPECT_EQ(text(message->element[2]), "arrived");
}

TEST_F(ClientGate, MimirNodesListsEveryNodeAClientMayUse) {
  Client client(cluster().port(2));
  const auto nodes = client.command({"MIMIR", "NODES"});
  ASSERT_TRUE(nodes) << client.error();
  ASSERT_EQ(nodes->type, REDIS_REPLY_ARRAY) << text(nodes.get());
  ASSERT_EQ(nodes->elements, cluster().size());
  std::set<long long> ports;
  int selves = 0;
  for (size_t i = 0; i < nodes->elements; ++i) {
    const auto node = fields(nodes->element[i]);
    ASSERT_TRUE(node.count("host") && node.count("port") && node.count("self"));
    EXPECT_EQ(text(node.at("host")), "127.0.0.1");
    ports.insert(node.at("port")->integer);
    selves += static_cast<int>(node.at("self")->integer);
  }
  EXPECT_EQ(ports, (std::set<long long>{cluster().port(0), cluster().port(1), cluster().port(2)}));
  EXPECT_EQ(selves, 1);
}

TEST_F(ClientGate, MimirsOwnErrorsReachTheClientAsErrors) {
  Client client(cluster().port(0));
  ASSERT_TRUE(client.command({"HSET", "errors:hash", "f", "v"}));
  const auto wrong_type = client.command({"GET", "errors:hash"});
  ASSERT_TRUE(wrong_type);
  ASSERT_EQ(wrong_type->type, REDIS_REPLY_ERROR);
  EXPECT_EQ(text(wrong_type.get()), "WRONGTYPE Operation against a key holding the wrong kind of value");

  const auto unknown = client.command({"NOSUCHCOMMAND", "x"});
  ASSERT_TRUE(unknown);
  ASSERT_EQ(unknown->type, REDIS_REPLY_ERROR);
  EXPECT_EQ(text(unknown.get()).rfind("ERR unknown command", 0), 0u) << text(unknown.get());

  // Ruling 35: a write of several keys is refused until Dannan's atomic
  // commit, and says so.
  const auto several = client.command({"MSET", "errors:a", "1", "errors:b", "2"});
  ASSERT_TRUE(several);
  ASSERT_EQ(several->type, REDIS_REPLY_ERROR);
  EXPECT_EQ(text(several.get()), "ERR this command writes more than one key, which Mimir refuses until Dannan's atomic commit across replica sets (ruling 35)");
}

// A cluster of its own: a node dies in it.
TEST(ClientFailover, AClientWhoseNodeDiesReconnectsToAnotherAndFindsItsWrites) {
  Cluster cluster(3);
  {
    Client client(cluster.port(1));
    const auto set = client.command({"SET", "failover:key", "before"});
    ASSERT_TRUE(set) << client.error();
    ASSERT_EQ(text(set.get()), "OK");
    cluster.kill(1);
    // The connection is gone, and hiredis says so rather than hanging.
    EXPECT_FALSE(client.command({"GET", "failover:key"}));
  }
  // Two of three still make a majority: another node reads and writes.
  Client client(cluster.port(2));
  ASSERT_TRUE(client.command({"CLIENT", "STRICT", "ON"}));
  const auto get = client.command({"GET", "failover:key"});
  ASSERT_TRUE(get) << client.error();
  EXPECT_EQ(text(get.get()), "before");
  const auto set = client.command({"SET", "failover:key", "after"});
  ASSERT_TRUE(set) << client.error();
  EXPECT_EQ(text(set.get()), "OK") << cluster.log(2);
  Client other(cluster.port(0));
  ASSERT_TRUE(other.command({"CLIENT", "STRICT", "ON"}));
  const auto read = other.command({"GET", "failover:key"});
  ASSERT_TRUE(read) << other.error();
  EXPECT_EQ(text(read.get()), "after");
}

}  // namespace
}  // namespace mimir::test
