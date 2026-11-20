//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dannan/agree/agreement.h"
#include "dannan/agree/memory_store.h"
#include "dannan/net/message.h"
#include "mimir/cluster/register.h"

namespace mimir::cluster {
namespace {

namespace agree = dannan::agree;
using namespace std::chrono_literals;
// The store Mimir serves its space from (Dannan's, from 0.10.1): these hold
// it to what Mimir relies on, its summary above all.
using Store = agree::MemoryStore;

// Real Agreements, one per node, each serving the space from its own store,
// their calls carried through one io context with nothing lost.
class Bed {
 public:
  static constexpr uint32_t shards = 4;

  explicit Bed(size_t size, bool ready = true) {
    std::vector<dannan::identity::NodeId> founders;
    for (size_t i = 0; i < size; ++i) {
      dannan::identity::NodeId id;
      id.bytes[15] = static_cast<uint8_t>(i + 1);
      _ids.push_back(id);
      founders.push_back(id);
      stores.push_back(std::make_shared<Store>(_io.get_executor(), ready));
      _nodes.emplace_back();
    }
    _genesis = agree::ReplicaSet::of(founders, 0);
    for (size_t i = 0; i < size; ++i) start(i);
  }

  const std::vector<dannan::identity::NodeId>& ids() const { return _ids; }

  agree::Outcome change(size_t at, agree::Slot slot, std::vector<agree::Name> names, agree::Change change) {
    std::optional<agree::Outcome> out;
    _nodes.at(at)->change(std::string(space), slot, std::move(names), _ids, 0, std::move(change), {}, 2s, _io.get_executor(),
                          [&out](agree::Outcome outcome) { out = std::move(outcome); });
    return wait(out);
  }

  agree::Outcome read(size_t at, agree::Slot slot, std::vector<agree::Name> names) {
    std::optional<agree::Outcome> out;
    _nodes.at(at)->read(std::string(space), slot, std::move(names), _ids, 0, {}, 2s, _io.get_executor(),
                        [&out](agree::Outcome outcome) { out = std::move(outcome); });
    return wait(out);
  }

  std::vector<std::shared_ptr<Store>> stores;

 private:
  agree::Outcome wait(std::optional<agree::Outcome>& out) {
    _io.restart();
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (!out && std::chrono::steady_clock::now() < until) _io.run_one_for(100ms);
    if (!out) {
      ADD_FAILURE() << "no outcome";
      return {};
    }
    return *out;
  }

  void start(size_t i) {
    dannan::identity::NodeId epoch;
    epoch.bytes[0] = static_cast<uint8_t>(i + 1);
    _nodes[i] = std::make_unique<agree::Agreement>(
        _io.get_executor(), _ids[i], epoch, agree::Agreement::Table{shards, _genesis},
        [this, i](const dannan::identity::NodeId& to, dannan::net::MessageType type, dannan::net::Bytes payload, std::chrono::milliseconds,
                  std::function<void(std::optional<dannan::net::Bytes>, std::string)> done) {
          boost::asio::post(_io, [this, i, to, type, payload = std::move(payload), done = std::move(done)]() mutable {
            size_t target = 0;
            while (target < _ids.size() && _ids[target] != to) ++target;
            auto answer = [this, done](dannan::net::Message reply) {
              boost::asio::post(_io, [done, reply = std::move(reply)] {
                if (const auto refusal = dannan::net::refusal_of(reply)) return done(std::nullopt, refusal->message);
                done(reply.payload, {});
              });
            };
            if (target == _ids.size()) return done(std::nullopt, "no such node");
            if (auto now = _nodes[target]->serve_request(_ids[i], dannan::net::Message{type, std::move(payload), {}}, answer)) answer(std::move(*now));
          });
        },
        [](const dannan::identity::NodeId&, const std::string&) { return true; });
    _nodes[i]->serve(std::string(space), std::string(space), stores[i]);
  }

  boost::asio::io_context _io;
  std::vector<dannan::identity::NodeId> _ids;
  agree::ReplicaSet _genesis;
  std::vector<std::unique_ptr<agree::Agreement>> _nodes;
};

Record value_record(std::string payload) {
  Record record;
  record.payload = std::move(payload);
  return record;
}

// A change that sets the register to `record`, whatever it held.
agree::Change set_to(const Record& record) {
  return [bytes = encode(record)](std::span<const std::optional<agree::Bytes>>) {
    agree::Changed out;
    out.values.push_back(bytes);
    return out;
  };
}

TEST(ClusterStore, AChangeIsHeldByAMajorityWithItsSummary) {
  Bed bed(3);
  const auto name = name_of(0, "k");
  const auto slot = shard_of("k", Bed::shards);
  const auto record = value_record("v1");
  const auto outcome = bed.change(0, slot, {name}, set_to(record));
  ASSERT_TRUE(outcome.committed) << outcome.why;
  ASSERT_EQ(outcome.values.size(), 1u);
  EXPECT_EQ(decode_value(outcome.values[0]), record);

  size_t holding = 0;
  for (const auto& store : bed.stores) {
    const auto state = store->state(slot, name);
    if (!state || state->accepted != outcome.ballot) continue;
    ++holding;
    ASSERT_TRUE(state->value);
    EXPECT_TRUE(state->has_value);
    EXPECT_EQ(state->summary, summary_of(*state->value));
  }
  EXPECT_GE(holding, 2u);
}

TEST(ClusterStore, ALaterChangeSeesTheCommittedValueFromAnyNode) {
  Bed bed(3);
  const auto name = name_of(0, "counter");
  const auto slot = shard_of("counter", Bed::shards);
  ASSERT_TRUE(bed.change(0, slot, {name}, set_to(value_record("1"))).committed);
  std::optional<Record> seen;
  const auto outcome = bed.change(2, slot, {name}, [&seen](std::span<const std::optional<agree::Bytes>> current) {
    seen = decode_value(current[0]);
    agree::Changed out;
    out.values.push_back(encode(value_record("2")));
    return out;
  });
  ASSERT_TRUE(outcome.committed) << outcome.why;
  ASSERT_TRUE(seen);
  EXPECT_EQ(seen->payload, "1");
  const auto read = bed.read(1, slot, {name});
  ASSERT_TRUE(read.committed) << read.why;
  EXPECT_EQ(decode_value(read.values.at(0))->payload, "2");
}

TEST(ClusterStore, ADeletedRegisterReadsAsAbsent) {
  Bed bed(3);
  const auto name = name_of(0, "gone");
  const auto slot = shard_of("gone", Bed::shards);
  ASSERT_TRUE(bed.change(0, slot, {name}, set_to(value_record("x"))).committed);
  ASSERT_TRUE(bed.change(1, slot, {name}, set_to(Record{})).committed);
  const auto read = bed.read(2, slot, {name});
  ASSERT_TRUE(read.committed) << read.why;
  EXPECT_FALSE(read.values.at(0));
  EXPECT_TRUE(decode_value(read.values.at(0))->empty());
}

TEST(ClusterStore, AStoreThatIsNotReadyDoesNotVote) {
  // Two of three lost their state: no majority may vote, so nothing commits.
  Bed bed(3, false);
  bed.stores[0]->set_ready();
  const auto name = name_of(0, "k");
  const auto outcome = bed.change(0, shard_of("k", Bed::shards), {name}, set_to(value_record("v")));
  EXPECT_FALSE(outcome.committed);
  EXPECT_EQ(bed.stores[1]->size(), 0u);
  EXPECT_EQ(bed.stores[2]->size(), 0u);
  // Caught up, they vote again.
  bed.stores[1]->set_ready();
  EXPECT_TRUE(bed.change(0, shard_of("k", Bed::shards), {name}, set_to(value_record("v"))).committed);
}

TEST(ClusterStore, LoadWithoutValuesSaysWhatIsHeld) {
  Bed bed(1);
  const auto name = name_of(0, "k");
  const auto slot = shard_of("k", Bed::shards);
  ASSERT_TRUE(bed.change(0, slot, {name}, set_to(value_record("v"))).committed);
  Store& store = *bed.stores[0];
  std::optional<agree::Held> held;
  const std::vector<agree::Name> names{name, name_of(0, "never")};
  store.load(slot, names, false, false, [&held](agree::Held out) { held = std::move(out); });
  // The store posts to the bed's executor; running a read through the bed runs it.
  bed.read(0, slot, {name});
  ASSERT_TRUE(held);
  ASSERT_EQ(held->states.size(), 2u);
  EXPECT_TRUE(held->states[0].has_value);
  EXPECT_FALSE(held->states[0].value);
  EXPECT_EQ(held->states[0].summary.size(), 32u);
  EXPECT_EQ(held->states[1], agree::AcceptorState{});
}

TEST(ClusterStore, AValueIsFetchedOnlyByItsExactBallot) {
  Bed bed(1);
  const auto name = name_of(0, "k");
  const auto slot = shard_of("k", Bed::shards);
  const auto outcome = bed.change(0, slot, {name}, set_to(value_record("v")));
  ASSERT_TRUE(outcome.committed);
  Store& store = *bed.stores[0];
  const auto held = store.state(slot, name);
  ASSERT_TRUE(held);
  std::optional<std::optional<Store::Fetched>> exact, other;
  store.value(slot, name, held->accepted, std::nullopt, [&exact](std::optional<Store::Fetched> out) { exact = std::move(out); });
  agree::Ballot wrong = held->accepted;
  ++wrong.counter;
  store.value(slot, name, wrong, std::nullopt, [&other](std::optional<Store::Fetched> out) { other = std::move(out); });
  bed.read(0, slot, {name});
  ASSERT_TRUE(exact && *exact);
  EXPECT_EQ((*exact)->bytes, *held->value);
  EXPECT_FALSE((*exact)->past);
  ASSERT_TRUE(other);
  EXPECT_FALSE(*other);
}

TEST(ClusterStore, ADeltaIsAskedForWholeAndNothingIsKept) {
  Bed bed(1);
  const auto name = name_of(0, "k");
  const auto other = name_of(0, "j");
  const auto slot = shard_of("k", Bed::shards);
  Store& store = *bed.stores[0];
  agree::Held held;
  held.names = {other, name};
  agree::AcceptorState whole;
  whole.accepted.counter = 5;
  whole.value = encode(value_record("w"));
  agree::AcceptorState delta;
  delta.accepted.counter = 5;
  delta.has_value = true;
  delta.delta = agree::Bytes{1, 2, 3};
  held.states = {whole, delta};
  std::optional<Store::Kept> kept;
  store.keep(slot, held, [&kept](Store::Kept out) { kept = std::move(out); });
  bed.read(0, slot, {name});
  ASSERT_TRUE(kept);
  EXPECT_FALSE(kept->kept);
  EXPECT_EQ(kept->need_whole, std::vector<size_t>{1});
  EXPECT_FALSE(store.state(slot, other));
}

}  // namespace
}  // namespace mimir::cluster
