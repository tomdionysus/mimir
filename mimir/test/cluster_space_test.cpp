//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <array>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "dannan/identity/cluster_key.h"
#include "dannan/identity/node_identity.h"
#include "dannan/net/codec.h"
#include "dannan/net/message.h"
#include "dannan/node.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "mimir/cluster/agreements.h"
#include "mimir/cluster/fanout.h"
#include "mimir/cluster/register.h"
#include "mimir/cluster/space.h"
#include "mimir/command/database.h"
#include "mimir/test/fixtures/local_agreements.h"

namespace mimir::cluster {
namespace {

namespace agree = dannan::agree;
using namespace std::chrono_literals;

constexpr uint32_t shards = 8;

dannan::identity::ClusterKey cluster_key() {
  std::array<uint8_t, 32> master{};
  for (size_t i = 0; i < master.size(); ++i) master[i] = static_cast<uint8_t>(i * 7 + 1);
  return dannan::identity::ClusterKey::from_master(master);
}

// Real Dannan nodes on loopback, each with Mimir's space, on one io context
// run by two threads.
class Cluster {
 public:
  // `page`: how many names a node gives a catching-up peer at a time.
  explicit Cluster(size_t size, size_t page = shard_names_page) : _page(page), _work(boost::asio::make_work_guard(_io)) {
    for (int i = 0; i < 2; ++i) _threads.emplace_back([this] { _io.run(); });
    std::vector<dannan::identity::NodeId> genesis;
    for (size_t i = 0; i < size; ++i) {
      _dirs.push_back(std::make_unique<dannan::test::TempDir>());
      genesis.push_back(dannan::identity::load_or_create_node_id(_dirs.back()->path()));
    }
    ids = genesis;
    nodes.resize(size);
    spaces.resize(size);
    agreements.resize(size);
    databases.resize(size);
    fanouts.resize(size);
    sessions.resize(size);
    for (size_t i = 0; i < size; ++i) start(i, true);
  }

  // Node `i` from its state directory: a new process, its acceptors voting at
  // once if `ready`, else not until caught up.
  void start(size_t i, bool ready) {
    dannan::NodeConfig config;
    config.state_path = _dirs[i]->path();
    config.key = cluster_key();
    config.listen_host = "127.0.0.1";
    config.advertise_host = "127.0.0.1";
    config.port = 0;
    for (size_t j = 0; j < nodes.size(); ++j)
      if (j != i && nodes[j]) {
        config.bootstrap.push_back({"127.0.0.1", nodes[j]->port()});
        break;
      }
    config.heartbeat = 100ms;
    config.dead_after = 3s;
    config.telemetry_interval = 1s;
    config.agreement_slots = shards;
    config.agreement_genesis = ids;
    config.log = dannan::status::console_sink(dannan::status::LogFormat::text, dannan::status::LogLevel::error);
    nodes[i] = std::make_unique<dannan::Node>(_io, config);
    Agreements::register_types(nodes[i]->context());
    nodes[i]->start();
    spaces[i] = std::make_unique<Space>(nodes[i]->context(), ready);
    agreements[i] = std::make_shared<Agreements>(*spaces[i], std::chrono::seconds(5), _page);
    command::DatabaseSettings settings;
    settings.agreements = agreements[i];
    settings.notify_keyspace_events = "KA";
    databases[i] = std::make_unique<command::Database>(dannan::contract::system_wall_clock(), settings);
    fanouts[i] = std::make_unique<Fanout>(nodes[i]->context(), databases[i]->pubsub());
    sessions[i] = command::Session{};
    sessions[i].id = databases[i]->next_client_id();
  }

  // Node `i` stops, losing what it held in memory, and starts again.
  void restart(size_t i) {
    fanouts[i].reset();
    nodes[i]->stop();
    std::promise<void> drained;
    boost::asio::post(_io, [&drained] { drained.set_value(); });
    drained.get_future().wait();
    databases[i].reset();
    agreements[i].reset();
    spaces[i].reset();
    nodes[i].reset();
    start(i, false);
  }

  ~Cluster() {
    fanouts.clear();
    for (auto& node : nodes) node->stop();
    // What the stop posted runs before the databases go.
    std::promise<void> drained;
    boost::asio::post(_io, [&drained] { drained.set_value(); });
    drained.get_future().wait();
    databases.clear();
    agreements.clear();
    spaces.clear();
    nodes.clear();
    _work.reset();
    _io.stop();
    for (auto& thread : _threads) thread.join();
  }

  // A command run on node `at`'s database, its reply once decided. A
  // postponed command woken with no bytes is run again, as a connection does.
  std::string run(size_t at, std::vector<std::string> args) {
    for (;;) {
      auto woken = std::make_shared<std::promise<std::string>>();
      auto reply = woken->get_future();
      std::string out;
      const auto executed = databases.at(at)->execute(sessions.at(at), args, out, [woken](std::string bytes) { woken->set_value(std::move(bytes)); });
      if (!executed.wait) return out;
      if (reply.wait_for(10s) != std::future_status::ready) return "<no reply>";
      auto bytes = reply.get();
      if (executed.postponed && bytes.empty()) continue;
      return bytes;
    }
  }

  // Whether node `at` answers `args` with `expected` within five seconds:
  // another node's commit reaches it by its notice.
  bool eventually(size_t at, std::vector<std::string> args, const std::string& expected) {
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < until) {
      if (run(at, args) == expected) return true;
      std::this_thread::sleep_for(20ms);
    }
    return false;
  }

  // Whether every node sees every other advertising `mimir` within ten seconds.
  bool converged() {
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < until) {
      bool all = true;
      for (auto& node : nodes) {
        size_t serving = 0;
        for (const auto& record : node->context().nodes().active())
          if (record.adverts.contains(std::string(space))) ++serving;
        if (serving < nodes.size()) all = false;
      }
      if (all) return true;
      std::this_thread::sleep_for(50ms);
    }
    return false;
  }

  agree::Outcome found(size_t at) {
    auto done = std::make_shared<std::promise<agree::Outcome>>();
    auto got = done->get_future();
    spaces.at(at)->found(5s, [done](agree::Outcome outcome) { done->set_value(std::move(outcome)); });
    return wait(got);
  }

  agree::Outcome change(size_t at, const std::string& key, Record record) {
    auto done = std::make_shared<std::promise<agree::Outcome>>();
    auto got = done->get_future();
    nodes.at(at)->context().agreement().change(
        std::string(space), shard_of(key, shards), {name_of(0, key)}, ids, 0,
        [bytes = encode(record)](std::span<const std::optional<agree::Bytes>>) {
          agree::Changed out;
          out.values.push_back(bytes);
          return out;
        },
        {}, 5s, _io.get_executor(), [done](agree::Outcome outcome) { done->set_value(std::move(outcome)); });
    return wait(got);
  }

  // `key`'s register holding `bytes` as they are, with no notice sent.
  agree::Outcome put_raw(size_t at, const std::string& key, agree::Bytes bytes) {
    auto done = std::make_shared<std::promise<agree::Outcome>>();
    auto got = done->get_future();
    nodes.at(at)->context().agreement().change(
        std::string(space), shard_of(key, shards), {name_of(0, key)}, ids, 0,
        [bytes = std::move(bytes)](std::span<const std::optional<agree::Bytes>>) {
          agree::Changed out;
          out.values.push_back(bytes);
          return out;
        },
        {}, 5s, _io.get_executor(), [done](agree::Outcome outcome) { done->set_value(std::move(outcome)); });
    return wait(got);
  }

  // Node `from` calling node `to` with a message of `type`, waited for.
  dannan::net::CallOutcome ask(size_t from, size_t to, dannan::net::MessageType type, dannan::net::Bytes payload) {
    for (const auto& record : nodes.at(from)->context().nodes().active())
      if (record.id == ids.at(to))
        return nodes.at(from)->context().routes().call_and_wait(record, type, std::move(payload), dannan::sched::FrameType::control, 5s);
    ADD_FAILURE() << "node " << to << " is not active at node " << from;
    return {};
  }

  agree::Outcome read(size_t at, const std::string& key) {
    auto done = std::make_shared<std::promise<agree::Outcome>>();
    auto got = done->get_future();
    nodes.at(at)->context().agreement().read(std::string(space), shard_of(key, shards), {name_of(0, key)}, ids, 0, {}, 5s, _io.get_executor(),
                                             [done](agree::Outcome outcome) { done->set_value(std::move(outcome)); });
    return wait(got);
  }

  std::vector<dannan::identity::NodeId> ids;
  std::vector<std::unique_ptr<dannan::Node>> nodes;
  std::vector<std::unique_ptr<Space>> spaces;
  std::vector<std::shared_ptr<Agreements>> agreements;
  std::vector<std::unique_ptr<command::Database>> databases;
  std::vector<std::unique_ptr<Fanout>> fanouts;
  std::vector<command::Session> sessions;

 private:
  static agree::Outcome wait(std::future<agree::Outcome>& got) {
    if (got.wait_for(10s) != std::future_status::ready) {
      ADD_FAILURE() << "no outcome";
      return {};
    }
    return got.get();
  }

  const size_t _page;
  boost::asio::io_context _io;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> _work;
  std::vector<std::thread> _threads;
  std::vector<std::unique_ptr<dannan::test::TempDir>> _dirs;
};

Record holding(std::string payload) {
  Record record;
  record.payload = std::move(payload);
  return record;
}

TEST(ClusterSpace, ALoneNodeFoundsItsShardsAndAgreesAlone) {
  Cluster cluster(1);
  EXPECT_EQ(cluster.spaces[0]->shards(), shards);
  const auto founded = cluster.found(0);
  ASSERT_TRUE(founded.committed) << founded.why;
  const auto changed = cluster.change(0, "k", holding("v"));
  ASSERT_TRUE(changed.committed) << changed.why;
  const auto read = cluster.read(0, "k");
  ASSERT_TRUE(read.committed) << read.why;
  EXPECT_EQ(decode_value(read.values.at(0))->payload, "v");
  // Founding again changes nothing.
  EXPECT_TRUE(cluster.found(0).committed);
}

TEST(ClusterSpace, ThreeNodesFoundTheirShardsAndAgreeFromAnyNode) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.converged());
  const auto founded = cluster.found(0);
  ASSERT_TRUE(founded.committed) << founded.why;
  // Every shard is recorded as served by the genesis set at a majority.
  size_t recorded = 0;
  for (auto& space : cluster.spaces) {
    std::promise<size_t> count;
    space->store().records([&count](std::vector<std::pair<agree::Slot, agree::SlotState>> records) { count.set_value(records.size()); });
    recorded += count.get_future().get() == shards ? 1 : 0;
  }
  EXPECT_GE(recorded, 2u);

  const auto changed = cluster.change(2, "{user}.name", holding("ada"));
  ASSERT_TRUE(changed.committed) << changed.why;
  const auto read = cluster.read(1, "{user}.name");
  ASSERT_TRUE(read.committed) << read.why;
  EXPECT_EQ(decode_value(read.values.at(0))->payload, "ada");
}

TEST(ClusterDatabase, AWriteOnOneNodeIsReadOnTheOthers) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(cluster.found(0).committed);
  EXPECT_EQ(cluster.run(0, {"SET", "greeting", "hello"}), "+OK\r\n");
  // The committing node reads its own write at once; the others once its
  // notice comes.
  EXPECT_EQ(cluster.run(0, {"GET", "greeting"}), "$5\r\nhello\r\n");
  EXPECT_TRUE(cluster.eventually(1, {"GET", "greeting"}, "$5\r\nhello\r\n"));
  EXPECT_TRUE(cluster.eventually(2, {"GET", "greeting"}, "$5\r\nhello\r\n"));
  EXPECT_EQ(cluster.run(2, {"DEL", "greeting"}), ":1\r\n");
  EXPECT_TRUE(cluster.eventually(0, {"EXISTS", "greeting"}, ":0\r\n"));
}

TEST(ClusterDatabase, IncrementsFromEveryNodeAtOnceEachApplyOnce) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(cluster.found(0).committed);
  // Three clients, one per node, racing on one key: every INCR is applied
  // once, and each answers a different count.
  std::vector<std::future<std::vector<std::string>>> clients;
  for (size_t at = 0; at < 3; ++at)
    clients.push_back(std::async(std::launch::async, [&cluster, at] {
      std::vector<std::string> replies;
      for (int i = 0; i < 10; ++i) replies.push_back(cluster.run(at, {"INCR", "counter"}));
      return replies;
    }));
  std::set<std::string> seen;
  for (auto& client : clients)
    for (const auto& reply : client.get()) {
      EXPECT_EQ(reply.front(), ':') << reply;
      EXPECT_TRUE(seen.insert(reply).second) << "two clients were given " << reply;
    }
  EXPECT_EQ(seen.size(), 30u);
  for (size_t at = 0; at < 3; ++at) EXPECT_TRUE(cluster.eventually(at, {"GET", "counter"}, "$2\r\n30\r\n")) << at;
}

// What one subscriber on a node has been pushed, gathered from any thread.
struct Heard {
  std::mutex mutex;
  std::vector<std::string> pushes;
  bool saw(const std::string& part) {
    std::lock_guard guard(mutex);
    for (const auto& push : pushes)
      if (push.find(part) != std::string::npos) return true;
    return false;
  }
};

bool eventually_heard(Heard& heard, const std::string& part, std::chrono::milliseconds within = 5s) {
  const auto until = std::chrono::steady_clock::now() + within;
  while (std::chrono::steady_clock::now() < until) {
    if (heard.saw(part)) return true;
    std::this_thread::sleep_for(20ms);
  }
  return false;
}

TEST(ClusterSpace, EveryNodeListsWhereEveryNodeTakesClients) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.converged());
  for (size_t i = 0; i < 3; ++i)
    cluster.spaces[i]->advertise_clients("node-" + std::to_string(i), i == 2 ? "" : "10.0.0." + std::to_string(i), static_cast<uint16_t>(7000 + i),
                                         i == 1 ? 7101 : 0);
  // The adverts reach every node; each lists all three, itself first.
  const auto until = std::chrono::steady_clock::now() + 10s;
  std::vector<command::NodeAddress> listed;
  while (std::chrono::steady_clock::now() < until) {
    listed = cluster.spaces[0]->client_addresses();
    if (listed.size() == 3) break;
    std::this_thread::sleep_for(20ms);
  }
  ASSERT_EQ(listed.size(), 3u);
  EXPECT_TRUE(listed[0].self);
  EXPECT_EQ(listed[0].name, "node-0");
  EXPECT_EQ(listed[0].host, "10.0.0.0");
  EXPECT_EQ(listed[0].port, 7000);
  std::map<std::string, command::NodeAddress> by_name;
  for (const auto& node : listed) by_name[node.name] = node;
  EXPECT_EQ(by_name["node-1"].tls_port, 7101);
  EXPECT_FALSE(by_name["node-1"].self);
  // No host given: the address peers dial it at.
  EXPECT_FALSE(by_name["node-2"].host.empty());

  // MIMIR NODES answers from that list, under ACL like any command.
  dannan::test::SteppedWall wall;
  boost::asio::io_context local;
  command::DatabaseSettings settings;
  settings.agreements = std::make_shared<test::LocalAgreements>(local);
  settings.nodes = [&cluster] { return cluster.spaces[0]->client_addresses(); };
  settings.users = {"user watcher on nopass +@all -mimir|nodes"};
  command::Database database(wall, settings);
  command::Session session;
  session.id = database.next_client_id();
  database.welcome(session);
  std::string out;
  database.execute(session, {"MIMIR", "NODES"}, out);
  EXPECT_EQ(out.rfind("*3\r\n*12\r\n$2\r\nid\r\n", 0), 0u) << out;
  EXPECT_NE(out.find("$6\r\nnode-1\r\n"), std::string::npos) << out;
  out.clear();
  database.execute(session, {"MIMIR", "WHAT"}, out);
  EXPECT_EQ(out, "-ERR unknown subcommand 'WHAT'. Try MIMIR HELP.\r\n");
  out.clear();
  database.execute(session, {"MIMIR", "HELP"}, out);
  EXPECT_EQ(out.rfind("*5\r\n+MIMIR <subcommand> [<arg> [value] [opt] ...]. Subcommands are:\r\n+NODES\r\n", 0), 0u) << out;
  out.clear();
  database.execute(session, {"MIMIR", "HELP", "x"}, out);
  EXPECT_EQ(out, "-ERR wrong number of arguments for 'mimir|help' command\r\n");
  out.clear();
  session.tls = true;
  database.execute(session, {"AUTH", "watcher", "x"}, out);
  ASSERT_EQ(out, "+OK\r\n");
  out.clear();
  database.execute(session, {"MIMIR", "NODES"}, out);
  EXPECT_EQ(out, "-NOPERM User watcher has no permissions to run the 'mimir|nodes' command\r\n");
}

TEST(ClusterFanout, ANodeWantsThePrefixesItsSubscriptionsNeed) {
  using Kind = command::PubSub::Kind;
  EXPECT_EQ(Fanout::prefix_of(Kind::channel, "news"), "c:news");
  EXPECT_EQ(Fanout::prefix_of(Kind::shard, "news"), "s:news");
  EXPECT_EQ(Fanout::prefix_of(Kind::pattern, "news.*"), "c:news.");
  EXPECT_EQ(Fanout::prefix_of(Kind::pattern, "__keyspace@0__:*"), "c:__keyspace@0__:");
  EXPECT_EQ(Fanout::prefix_of(Kind::pattern, "h?llo"), "c:h");
  EXPECT_EQ(Fanout::prefix_of(Kind::pattern, "*"), "c:");
  // A prefix another covers is left out, so a topic matches at most one.
  EXPECT_EQ(Fanout::minimal({{"c:news", 1}, {"c:news.", 2}, {"c:sport", 1}, {"s:news", 1}}), (std::vector<std::string>{"c:news", "c:sport", "s:news"}));
  EXPECT_EQ(Fanout::minimal({{"c:", 1}, {"c:news", 1}, {"s:x", 1}}), (std::vector<std::string>{"c:", "s:x"}));
}

TEST(ClusterDatabase, APublishAndANotificationReachEveryNodesSubscribers) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(cluster.found(0).committed);
  auto heard = std::make_shared<Heard>();
  size_t count = 0;
  const auto push = [heard](std::string bytes) {
    std::lock_guard guard(heard->mutex);
    heard->pushes.push_back(std::move(bytes));
  };
  cluster.databases[2]->pubsub().subscribe(command::PubSub::Kind::channel, 9001, "news", push, resp::Protocol::resp2, "subscribe", count);
  cluster.databases[2]->pubsub().subscribe(command::PubSub::Kind::pattern, 9001, "__keyspace@0__:*", push, resp::Protocol::resp2, "psubscribe", count);
  // Its interest reaches the other nodes as a push, not at once: a publish
  // made before it arrived goes nowhere (at most once), so publish until heard.
  bool reached = false;
  for (int attempt = 0; attempt < 50 && !reached; ++attempt) {
    EXPECT_EQ(cluster.run(0, {"PUBLISH", "news", "hello"}), ":0\r\n");
    reached = eventually_heard(*heard, "hello", 100ms);
  }
  EXPECT_TRUE(reached);
  // A write on another node: its notification is raised there and heard here.
  EXPECT_EQ(cluster.run(1, {"SET", "k", "v"}), "+OK\r\n");
  EXPECT_TRUE(eventually_heard(*heard, "__keyspace@0__:k"));
}

// Valkey's promise: once SUBSCRIBE is confirmed, a PUBLISH made after it
// reaches the subscriber, whichever node it is made on. The confirmation waits
// until the other nodes route the channel here (as Lir holds SUBACK).
TEST(ClusterDatabase, APublishMadeAfterSubscribeIsConfirmedReachesTheSubscriberFromAnyNode) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(cluster.found(0).committed);
  auto heard = std::make_shared<Heard>();
  cluster.sessions[2].push = [heard](std::string bytes) {
    std::lock_guard guard(heard->mutex);
    heard->pushes.push_back(std::move(bytes));
  };
  for (int i = 0; i < 5; ++i) {
    const std::string channel = "fresh:" + std::to_string(i);
    const std::string message = "message-" + std::to_string(i);
    cluster.run(2, {"SUBSCRIBE", channel});
    ASSERT_TRUE(heard->saw(channel)) << "no confirmation for " << channel;
    // Once, with no retry: the publisher's node must already route it.
    EXPECT_EQ(cluster.run(static_cast<size_t>(i % 2), {"PUBLISH", channel, message}), ":0\r\n");
    EXPECT_TRUE(eventually_heard(*heard, message)) << channel;
  }
}

// A node that loses its last subscriber stops routing the channel, and routes
// it again before a new SUBSCRIBE is confirmed.
TEST(ClusterDatabase, ASubscriberThatLeavesAndComesBackIsReachedAgain) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(cluster.found(0).committed);
  auto heard = std::make_shared<Heard>();
  cluster.sessions[2].push = [heard](std::string bytes) {
    std::lock_guard guard(heard->mutex);
    heard->pushes.push_back(std::move(bytes));
  };
  cluster.run(2, {"SUBSCRIBE", "news"});
  ASSERT_TRUE(heard->saw("subscribe"));
  // Answered in its reply or pushed, as the connection takes it.
  const std::string left = cluster.run(2, {"UNSUBSCRIBE", "news"});
  ASSERT_TRUE(left.find("unsubscribe") != std::string::npos || heard->saw("unsubscribe")) << left;
  cluster.run(2, {"SUBSCRIBE", "news"});
  EXPECT_EQ(cluster.run(0, {"PUBLISH", "news", "again"}), ":0\r\n");
  EXPECT_TRUE(eventually_heard(*heard, "again"));
}

TEST(ClusterDatabase, ANodeRestartedWithoutItsStateCatchesUpBeforeItVotes) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(cluster.found(0).committed);
  for (int i = 0; i < 20; ++i) ASSERT_EQ(cluster.run(i % 2, {"SET", "key:" + std::to_string(i), std::to_string(i)}), "+OK\r\n");
  cluster.restart(2);
  EXPECT_FALSE(cluster.spaces[2]->store().ready());
  ASSERT_TRUE(cluster.converged());
  auto caught = std::make_shared<std::promise<void>>();
  cluster.agreements[2]->recover([caught] { caught->set_value(); });
  ASSERT_EQ(caught->get_future().wait_for(30s), std::future_status::ready);
  EXPECT_TRUE(cluster.spaces[2]->store().ready());
  // Its view holds every key, and it serves writes again.
  for (int i = 0; i < 20; ++i)
    EXPECT_EQ(cluster.run(2, {"GET", "key:" + std::to_string(i)}), "$" + std::to_string(std::to_string(i).size()) + "\r\n" + std::to_string(i) + "\r\n") << i;
  EXPECT_EQ(cluster.run(2, {"INCR", "key:3"}), ":4\r\n");
  EXPECT_TRUE(cluster.eventually(0, {"GET", "key:3"}, "$1\r\n4\r\n"));
}

TEST(ClusterDatabase, ACatchUpOverManyPagesGathersEveryName) {
  // Three names a page: each shard's names come in several pages, each
  // resumed after the last name given.
  Cluster cluster(3, 3);
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(cluster.found(0).committed);
  constexpr int keys = 60;
  for (int i = 0; i < keys; ++i) ASSERT_EQ(cluster.run(i % 2, {"SET", "key:" + std::to_string(i), std::to_string(i)}), "+OK\r\n");
  cluster.restart(2);
  ASSERT_TRUE(cluster.converged());
  auto caught = std::make_shared<std::promise<void>>();
  cluster.agreements[2]->recover([caught] { caught->set_value(); });
  ASSERT_EQ(caught->get_future().wait_for(30s), std::future_status::ready);
  for (int i = 0; i < keys; ++i)
    EXPECT_EQ(cluster.run(2, {"GET", "key:" + std::to_string(i)}), "$" + std::to_string(std::to_string(i).size()) + "\r\n" + std::to_string(i) + "\r\n") << i;
}

TEST(ClusterDatabase, AnExpiryIsHeardOnceWhereverItIsFound) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(cluster.found(0).committed);
  auto heard = std::make_shared<Heard>();
  size_t count = 0;
  cluster.databases[2]->pubsub().subscribe(
      command::PubSub::Kind::channel, 9002, "__keyspace@0__:brief",
      [heard](std::string bytes) {
        std::lock_guard guard(heard->mutex);
        heard->pushes.push_back(std::move(bytes));
      },
      resp::Protocol::resp2, "subscribe", count);
  EXPECT_EQ(cluster.run(0, {"SET", "brief", "v", "PX", "100"}), "+OK\r\n");
  ASSERT_TRUE(cluster.eventually(2, {"EXISTS", "brief"}, ":1\r\n"));
  std::this_thread::sleep_for(300ms);
  // Every node finds it expired in its own view; each tells its own
  // subscribers, and none carries it to the others.
  for (size_t at = 0; at < 3; ++at) EXPECT_EQ(cluster.run(at, {"GET", "brief"}), "$-1\r\n");
  std::this_thread::sleep_for(300ms);
  std::lock_guard guard(heard->mutex);
  size_t expiries = 0;
  for (const auto& push : heard->pushes)
    if (push.find("expired") != std::string::npos) ++expiries;
  EXPECT_EQ(expiries, 1u);
}

// A strict read asks a majority, whichever node took the write.
TEST(ClusterDatabase, AStrictReadOnAnyNodeSeesTheCommit) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(cluster.found(0).committed);
  EXPECT_EQ(cluster.run(0, {"SET", "k", "v"}), "+OK\r\n");
  EXPECT_EQ(cluster.run(1, {"CLIENT", "STRICT", "ON"}), "+OK\r\n");
  EXPECT_EQ(cluster.run(1, {"GET", "k"}), "$1\r\nv\r\n");
  EXPECT_EQ(cluster.run(1, {"GET", "never"}), "$-1\r\n");
}

// A register that does not hold a record is neither read nor written over,
// and the client is told why.
TEST(ClusterDatabase, ARegisterThatHoldsNoRecordIsRefused) {
  Cluster cluster(3);
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(cluster.found(0).committed);
  ASSERT_TRUE(cluster.put_raw(0, "bad", {'n', 'o', 't'}).committed);
  EXPECT_EQ(cluster.run(1, {"CLIENT", "STRICT", "ON"}), "+OK\r\n");
  EXPECT_EQ(cluster.run(1, {"GET", "bad"}), "-CLUSTERDOWN the key's stored record cannot be read\r\n");
  EXPECT_EQ(cluster.run(0, {"SET", "bad", "x"}), "-CLUSTERDOWN the key's stored record cannot be read\r\n");
  const auto still = cluster.read(2, "bad");
  ASSERT_TRUE(still.committed) << still.why;
  EXPECT_EQ(still.values.at(0), (agree::Bytes{'n', 'o', 't'}));
}

dannan::net::Bytes notice(size_t version_bytes, std::optional<agree::Bytes> carried) {
  dannan::net::Writer out;
  out.u16(0);
  out.bytes(agree::Bytes{'k'});
  out.bytes(agree::Bytes(version_bytes, 0));
  out.u8(carried ? 1 : 0);
  if (carried) out.bytes(*carried);
  return out.take();
}

std::string refusal(const dannan::net::CallOutcome& outcome) {
  if (!outcome.reply) return "<no reply: " + outcome.failure + ">";
  const auto refused = dannan::net::refusal_of(*outcome.reply);
  if (!refused) return "";
  return std::to_string(refused->status) + " " + refused->code + " " + refused->message;
}

// The notice and the catch-up's request are untrusted bytes: what cannot be
// read is refused, and nothing of it is kept.
TEST(ClusterDatabase, NoticesAndNameRequestsThatCannotBeReadAreRefused) {
  Cluster cluster(2);
  ASSERT_TRUE(cluster.converged());
  ASSERT_TRUE(cluster.found(0).committed);
  EXPECT_EQ(refusal(cluster.ask(0, 1, commit_notice, {1, 2})), "400 bad_notice the notice cannot be read");
  EXPECT_EQ(refusal(cluster.ask(0, 1, commit_notice, notice(3, std::nullopt))), "400 bad_notice a version is 40 bytes");
  EXPECT_EQ(refusal(cluster.ask(0, 1, commit_notice, notice(40, agree::Bytes{'z', 'z'}))), "400 bad_notice the record cannot be read");
  // A notice that carries no record is taken, and changes nothing.
  EXPECT_EQ(refusal(cluster.ask(0, 1, commit_notice, notice(40, std::nullopt))), "");
  EXPECT_EQ(cluster.run(1, {"EXISTS", "k"}), ":0\r\n");
  EXPECT_EQ(refusal(cluster.ask(0, 1, shard_names, {1, 2})), "400 bad_request the request cannot be read");
}

// A change of several keys waits for Dannan's atomic commit across replica
// sets: refused, posted rather than called inline.
TEST(ClusterDatabase, AChangeOfSeveralKeysIsRefusedUntilItCanBeAtomic) {
  Cluster cluster(1);
  EXPECT_FALSE(cluster.agreements[0]->several_keys());
  auto done = std::make_shared<std::promise<command::Agreements::KeysOutcome>>();
  auto got = done->get_future();
  cluster.agreements[0]->change_keys(
      {{0, "a"}, {0, "b"}}, [](const std::vector<Record>&) { return command::Agreements::ChangedKeys{}; },
      [done](command::Agreements::KeysOutcome outcome) { done->set_value(std::move(outcome)); });
  ASSERT_EQ(got.wait_for(5s), std::future_status::ready);
  const auto outcome = got.get();
  EXPECT_FALSE(outcome.committed);
  EXPECT_FALSE(outcome.unknown);
  EXPECT_NE(outcome.why.find("atomic commit"), std::string::npos) << outcome.why;
}

}  // namespace
}  // namespace mimir::cluster
