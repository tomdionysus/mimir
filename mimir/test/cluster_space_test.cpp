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
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "dannan/identity/cluster_key.h"
#include "dannan/identity/node_identity.h"
#include "dannan/node.h"
#include "dannan/test/fixtures/temp_dir.h"
#include "mimir/cluster/register.h"
#include "mimir/cluster/space.h"

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
  explicit Cluster(size_t size) : _work(boost::asio::make_work_guard(_io)) {
    for (int i = 0; i < 2; ++i) _threads.emplace_back([this] { _io.run(); });
    std::vector<dannan::identity::NodeId> genesis;
    for (size_t i = 0; i < size; ++i) {
      _dirs.push_back(std::make_unique<dannan::test::TempDir>());
      genesis.push_back(dannan::identity::load_or_create_node_id(_dirs.back()->path()));
    }
    ids = genesis;
    for (size_t i = 0; i < size; ++i) {
      dannan::NodeConfig config;
      config.state_path = _dirs[i]->path();
      config.key = cluster_key();
      config.listen_host = "127.0.0.1";
      config.advertise_host = "127.0.0.1";
      config.port = 0;
      if (i > 0) config.bootstrap.push_back({"127.0.0.1", nodes[0]->port()});
      config.heartbeat = 100ms;
      config.dead_after = 3s;
      config.telemetry_interval = 1s;
      config.agreement_slots = shards;
      config.agreement_genesis = genesis;
      config.log = dannan::status::console_sink(dannan::status::LogFormat::text, dannan::status::LogLevel::error);
      nodes.push_back(std::make_unique<dannan::Node>(_io, config));
      nodes.back()->start();
      spaces.push_back(std::make_unique<Space>(nodes.back()->context(), true));
    }
  }
  ~Cluster() {
    spaces.clear();
    for (auto& node : nodes) node->stop();
    nodes.clear();
    _work.reset();
    _io.stop();
    for (auto& thread : _threads) thread.join();
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

 private:
  static agree::Outcome wait(std::future<agree::Outcome>& got) {
    if (got.wait_for(10s) != std::future_status::ready) {
      ADD_FAILURE() << "no outcome";
      return {};
    }
    return got.get();
  }

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

}  // namespace
}  // namespace mimir::cluster
