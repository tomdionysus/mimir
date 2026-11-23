//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Stage 3's gate under faults: Mimir's database on Dannan's simulated nodes
// (SimNodes), one world on one thread, every choice from one seed. Clients on
// every node run counters, a lock (SET NX, and a release by script) and
// strict reads, while messages are lost, duplicated and delayed, nodes
// restart (losing their in-memory acceptors, so catching up before they
// vote) and the cluster splits and heals. Every history must be linearizable
// (Dannan's checker, against the model here): no command, from whichever
// node, may be told something no single order explains, and two clients
// never both hold the lock.

#include <gtest/gtest.h>

#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dannan/test/fixtures/sim/history.h"
#include "dannan/test/fixtures/sim/linearizable.h"
#include "dannan/test/fixtures/sim/nodes.h"
#include "mimir/cluster/agreements.h"
#include "mimir/cluster/space.h"
#include "mimir/command/database.h"

namespace mimir::test {
namespace {

namespace sim = dannan::test::sim;
using namespace std::chrono_literals;

// What a client asks, as the history records it: the operation's kind, then
// its argument.
sim::Bytes input(char kind, const std::string& argument = {}) {
  sim::Bytes out{static_cast<uint8_t>(kind)};
  out.insert(out.end(), argument.begin(), argument.end());
  return out;
}

std::string bulk(const std::string& value) { return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n"; }

// One key's value as Mimir answers it, the state the empty string for a key
// that does not exist: 'i' INCRBY n (the new value), 'g' GET under CLIENT
// STRICT ON, 'l' SET key owner NX, 'u' the release script (deletes the key
// only while it holds the owner).
class KeyModel final : public sim::Model {
 public:
  std::string initial(const std::vector<std::string>&) const override { return {}; }
  std::vector<std::string> step(const std::string& state, const sim::Operation& operation, const sim::Bytes* result) const override {
    if (operation.input.empty()) return {};
    const std::string argument(operation.input.begin() + 1, operation.input.end());
    std::string next = state;
    std::string expected;
    switch (operation.input[0]) {
      case 'i':
        next = std::to_string((state.empty() ? 0 : std::stoll(state)) + std::stoll(argument));
        expected = ":" + next + "\r\n";
        break;
      case 'g':
        expected = state.empty() ? "$-1\r\n" : bulk(state);
        break;
      case 'l':
        if (state.empty()) {
          next = argument;
          expected = "+OK\r\n";
        } else {
          expected = "$-1\r\n";
        }
        break;
      case 'u':
        if (state == argument) {
          next.clear();
          expected = ":1\r\n";
        } else {
          expected = ":0\r\n";
        }
        break;
      default:
        return {};
    }
    if (result && std::string(result->begin(), result->end()) != expected) return {};
    return {next};
  }
};

constexpr const char* release_script = "if redis.call('GET', KEYS[1]) == ARGV[1] then return redis.call('DEL', KEYS[1]) else return 0 end";

// Mimir on one simulated node, as mimir-node assembles it: its space, its
// agreements, its database on the world's clock.
struct Host {
  Host(sim::World& world, dannan::Node& node, bool fresh) : clock(world, 0, 0), space(node.context(), fresh) {
    agreements = std::make_shared<cluster::Agreements>(space);
    command::DatabaseSettings settings;
    settings.agreements = agreements;
    settings.time = &clock;
    settings.latency_tracking = false;
    database = std::make_unique<command::Database>(clock, settings);
    // A node that lost its acceptors catches up before it votes.
    if (!space.store().ready()) agreements->recover([] {});
  }
  sim::NodeClock clock;
  cluster::Space space;
  std::shared_ptr<cluster::Agreements> agreements;
  std::unique_ptr<command::Database> database;
};

// A client's operation in flight: its index in the history, and its
// session, which outlives the wait it parks.
struct Pending {
  size_t index = 0;
  command::Session session;
};

enum class Faults { restarts, split };

// One seeded run of `operations` client operations over a cluster of
// `size`: nothing if its history is linearizable, else why not.
std::optional<std::string> run(uint64_t seed, size_t size, Faults faults, size_t operations) {
  sim::World world(seed);
  sim::NetworkFaults network;
  network.drop_one_in = 12;
  network.duplicate_one_in = 12;
  network.max_delay_ms = 20;
  sim::SimNodes nodes(world, size, network, 50, 50, false);
  std::vector<std::unique_ptr<Host>> hosts(size);
  std::vector<bool> started(size, false);
  nodes.configure = [&](size_t, dannan::NodeConfig& config) {
    config.agreement_genesis = nodes.ids();
    config.agreement_slots = 8;
  };
  // Mimir's notice types are not registered here: SimNodes starts a node
  // before its host, and routes freeze at start. Their registrations fix
  // only lanes and frames, which the simulated transport does not use.
  nodes.host = [&](size_t i, dannan::Node& node, bool) {
    // Every Mimir restart loses its acceptors (they are in memory): only a
    // node's first start votes at once.
    hosts[i] = std::make_unique<Host>(world, node, !started[i]);
    started[i] = true;
  };
  // A node goes as mimir-node's does: stopped, what it posted drained, then
  // Mimir's parts, then the node.
  const auto take_down = [&](size_t i) {
    if (!nodes.up(i)) return;
    nodes.node(i).stop();
    world.io.restart();
    while (world.io.poll_one() > 0) {
    }
    hosts[i].reset();
  };
  sim::History history;
  std::vector<std::shared_ptr<Pending>> pending;
  // On any return: every node taken down before what its handlers touch goes.
  struct Finally {
    std::function<void()> then;
    ~Finally() { then(); }
  } finally{[&] {
    for (size_t i = 0; i < size; ++i) take_down(i);
  }};
  nodes.start_all();

  const auto ready = [&] {
    for (size_t i = 0; i < size; ++i) {
      if (!nodes.up(i)) return false;
      size_t serving = 0;
      for (const auto& record : nodes.node(i).context().nodes().active())
        if (record.adverts.contains("mimir")) ++serving;
      if (serving < size) return false;
    }
    return true;
  };
  if (!world.run_until(ready, 2'000'000)) return "the nodes never all advertised the space";
  auto founded = std::make_shared<std::optional<bool>>();
  hosts[0]->space.found(5000ms, [founded](dannan::agree::Outcome outcome) { *founded = outcome.committed; });
  if (!world.run_until([&] { return founded->has_value(); }, 2'000'000) || !**founded) return "the shards were not founded";

  const std::vector<std::string> counters = {"c:a", "c:b"};
  const std::string lock = "lock";
  const auto start = world.now_ms();
  for (size_t n = 0; n < operations; ++n)
    world.after(world.random.below(1500), [&, n] {
      size_t at = world.random.below(size);
      for (size_t tries = 0; tries < size && !nodes.up(at); ++tries) at = (at + 1) % size;
      if (!nodes.up(at)) return;
      auto op = std::make_shared<Pending>();
      auto& database = *hosts[at]->database;
      op->session.id = database.next_client_id();
      database.welcome(op->session);
      std::string ignored;
      database.execute(op->session, {"CLIENT", "STRICT", "ON"}, ignored);
      const std::string owner = "client-" + std::to_string(n);
      std::string key;
      sim::Bytes asked;
      std::vector<std::string> args;
      switch (world.random.below(5)) {
        case 0:
        case 1: {
          key = counters[world.random.below(counters.size())];
          const std::string by = std::to_string(1 + world.random.below(3));
          asked = input('i', by);
          args = {"INCRBY", key, by};
          break;
        }
        case 2:
          key = world.random.below(2) ? lock : counters[world.random.below(counters.size())];
          asked = input('g');
          args = {"GET", key};
          break;
        case 3:
          key = lock;
          asked = input('l', owner);
          args = {"SET", key, owner, "NX"};
          break;
        default:
          // A release names a recent client, which may or may not hold it.
          key = lock;
          {
            const std::string holder = "client-" + std::to_string(n > 0 ? world.random.below(n) : 0);
            asked = input('u', holder);
            args = {"EVAL", release_script, "1", key, holder};
          }
          break;
      }
      op->index = history.invoke(n, {key}, asked, world.now_ms(), at);
      pending.push_back(op);
      // A refusal or a failure to commit (an error reply) may still have
      // taken effect: it is left unanswered, as the checker reads one.
      const auto told = [&world, &history, op](const std::string& bytes) {
        if (!bytes.empty() && bytes[0] != '-') history.answer(op->index, sim::Bytes(bytes.begin(), bytes.end()), world.now_ms());
      };
      std::string out;
      const auto executed = database.execute(op->session, args, out, [told](std::string bytes) { told(bytes); });
      if (!executed.wait) told(out);
    });

  if (faults == Faults::restarts) {
    world.after(400, [&] {
      take_down(1);
      nodes.restart(1);
      history.fault({world.now_ms(), "restart", {1}, ""});
    });
    if (size > 3)
      world.after(900, [&] {
        take_down(3);
        nodes.restart(3, sim::Wipe::store);
        history.fault({world.now_ms(), "wipe", {3}, ""});
      });
  } else {
    std::vector<size_t> minority, majority;
    for (size_t i = 0; i < size; ++i) (i < size / 2 ? minority : majority).push_back(i);
    nodes.network().partition({minority, majority});
    history.fault({world.now_ms(), "partition", {}, ""});
    world.after(1200, [&] {
      nodes.network().heal();
      history.fault({world.now_ms(), "heal", {}, ""});
    });
  }
  world.after(1600, [&] { nodes.network().faults() = sim::NetworkFaults{}; });

  // Settled: every operation answered or given up on (the agreements'
  // deadline), and every node voting again.
  const auto settled = [&] {
    if (world.now_ms() < start + 1600 + 6000) return false;
    for (size_t i = 0; i < size; ++i)
      if (!nodes.up(i) || !hosts[i]->space.store().ready()) return false;
    return true;
  };
  if (!world.run_until(settled, 8'000'000)) return "the run did not settle";

  // A final strict read of every key, from node 0, after everything else.
  for (const auto& key : std::vector<std::string>{counters[0], counters[1], lock}) {
    auto op = std::make_shared<Pending>();
    auto& database = *hosts[0]->database;
    op->session.id = database.next_client_id();
    database.welcome(op->session);
    std::string ignored;
    database.execute(op->session, {"CLIENT", "STRICT", "ON"}, ignored);
    op->index = history.invoke(operations + pending.size(), {key}, input('g'), world.now_ms(), 0);
    pending.push_back(op);
    auto got = std::make_shared<std::optional<std::string>>();
    std::string out;
    const auto executed = database.execute(op->session, {"GET", key}, out, [got](std::string bytes) { *got = std::move(bytes); });
    if (!executed.wait) *got = out;
    if (!world.run_until([&] { return got->has_value(); }, 2'000'000)) return "the final read of " + key + " was not answered";
    const std::string& read = **got;
    if (read.empty() || read[0] == '-') return "the final read of " + key + " failed: " + read;
    history.answer(op->index, sim::Bytes(read.begin(), read.end()), world.now_ms());
  }

  size_t answered = 0;
  for (const auto& operation : history.operations())
    if (operation.answer == sim::Answer::ok) ++answered;
  const auto verdict = sim::check_linearizable(history, KeyModel{});
  if (verdict.exhausted) return "the checker ran out of budget";
  if (!verdict.linearizable) {
    std::string keys;
    for (const auto& key : verdict.keys) keys += key + " ";
    return "not linearizable over " + keys + ": " + verdict.why;
  }
  if (answered < operations / 2) return "only " + std::to_string(answered) + " operations were answered";
  return std::nullopt;
}

// The fixed seeds, or MIMIR_SIM_SEEDS for a longer run by hand.
uint64_t seeds(uint64_t fallback) {
  const char* set = std::getenv("MIMIR_SIM_SEEDS");
  return set && *set ? std::strtoull(set, nullptr, 10) : fallback;
}

TEST(ClusterSim, CommandsFromEveryNodeStayLinearizableThroughRestarts) {
  for (uint64_t seed = 1; seed <= seeds(5); ++seed) {
    const auto failed = run(seed, 5, Faults::restarts, 40);
    ASSERT_FALSE(failed) << "seed " << seed << ": " << *failed;
  }
}

TEST(ClusterSim, CommandsFromEveryNodeStayLinearizableThroughASplit) {
  for (uint64_t seed = 1; seed <= seeds(5); ++seed) {
    const auto failed = run(seed, 5, Faults::split, 40);
    ASSERT_FALSE(failed) << "seed " << seed << ": " << *failed;
  }
}

}  // namespace
}  // namespace mimir::test
