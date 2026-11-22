//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The keyspace, from Valkey's documented behaviour and the design: expiry is
// an absolute wall-clock time, an expired key is never seen (EXPIRE: "keys
// expire passively when accessed, and actively"), and SCAN returns every key
// present for the whole scan at least once, whatever else changes (SCAN's
// "Scan guarantees").

#include "mimir/keyspace/keyspace.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

#include "dannan/test/fixtures/stepped_time.h"

namespace mimir::keyspace {
namespace {

using namespace std::chrono_literals;

Entry string_entry(std::string value, std::optional<uint64_t> expires_at = std::nullopt) { return Entry{std::move(value), expires_at}; }

TEST(KeyspaceKeyspace, PutFindErase) {
  dannan::test::SteppedWall wall;
  Keyspace keys(wall);
  EXPECT_EQ(keys.find("k"), nullptr);
  keys.put("k", string_entry("v"));
  ASSERT_NE(keys.find("k"), nullptr);
  EXPECT_EQ(std::get<std::string>(keys.find("k")->value), "v");
  EXPECT_EQ(keys.size(), 1u);
  EXPECT_TRUE(keys.erase("k"));
  EXPECT_FALSE(keys.erase("k"));
  EXPECT_EQ(keys.size(), 0u);
}

TEST(KeyspaceKeyspace, AnExpiredKeyIsNeverSeenAndGoesWhenFound) {
  dannan::test::SteppedWall wall;
  Keyspace keys(wall);
  keys.put("k", string_entry("v", wall.unix_ms() + 1000));
  EXPECT_EQ(keys.expiring(), 1u);
  wall.advance(999ms);
  EXPECT_NE(keys.find("k"), nullptr);
  wall.advance(1ms);
  EXPECT_FALSE(keys.contains("k"));
  EXPECT_EQ(keys.size(), 1u);
  EXPECT_EQ(keys.find("k"), nullptr);
  EXPECT_EQ(keys.size(), 0u);
  EXPECT_EQ(keys.expiring(), 0u);
}

TEST(KeyspaceKeyspace, ReplacingAKeyKeepsTheExpiryCountRight) {
  dannan::test::SteppedWall wall;
  Keyspace keys(wall);
  keys.put("k", string_entry("v", wall.unix_ms() + 1000));
  keys.put("k", string_entry("w"));
  EXPECT_EQ(keys.expiring(), 0u);
  keys.put("k", string_entry("x", wall.unix_ms() + 1000));
  keys.rename("k", "j");
  EXPECT_EQ(keys.expiring(), 1u);
  EXPECT_EQ(keys.find("k"), nullptr);
  ASSERT_NE(keys.find("j"), nullptr);
  EXPECT_EQ(keys.find("j")->expires_at, wall.unix_ms() + 1000);
}

TEST(KeyspaceKeyspace, ActiveExpiryIsBoundedAndResumes) {
  dannan::test::SteppedWall wall;
  Keyspace keys(wall);
  for (int i = 0; i < 100; ++i) keys.put("e" + std::to_string(i), string_entry("v", wall.unix_ms() + 10));
  for (int i = 0; i < 100; ++i) keys.put("p" + std::to_string(i), string_entry("v"));
  wall.advance(10ms);
  size_t removed = 0;
  int steps = 0;
  while (keys.expiring() > 0) {
    const size_t now = keys.expire_step(20);
    EXPECT_LE(now, 20u);
    removed += now;
    ASSERT_LT(++steps, 50);
  }
  EXPECT_EQ(removed, 100u);
  EXPECT_EQ(keys.size(), 100u);
}

TEST(KeyspaceKeyspace, AScanReturnsEveryKeyOnceWhenNothingChanges) {
  dannan::test::SteppedWall wall;
  Keyspace keys(wall);
  for (int i = 0; i < 1000; ++i) keys.put("k" + std::to_string(i), string_entry("v"));
  std::multiset<std::string> seen;
  uint64_t cursor = 0;
  int pages = 0;
  do {
    auto page = keys.scan(cursor, 10);
    seen.insert(page.keys.begin(), page.keys.end());
    cursor = page.cursor;
    ASSERT_LT(++pages, 1000);
  } while (cursor != 0);
  EXPECT_EQ(seen.size(), 1000u);
  EXPECT_EQ(std::set<std::string>(seen.begin(), seen.end()).size(), 1000u);
}

TEST(KeyspaceKeyspace, AScanSeesEveryKeyPresentThroughoutWhateverChanges) {
  dannan::test::SteppedWall wall;
  Keyspace keys(wall);
  for (int i = 0; i < 500; ++i) keys.put("stay" + std::to_string(i), string_entry("v"));
  std::set<std::string> seen;
  uint64_t cursor = 0;
  int round = 0;
  do {
    auto page = keys.scan(cursor, 7);
    seen.insert(page.keys.begin(), page.keys.end());
    cursor = page.cursor;
    // Churn between pages.
    for (int j = 0; j < 20; ++j) keys.put("new" + std::to_string(round * 20 + j), string_entry("v"));
    for (int j = 0; j < 10; ++j) keys.erase("new" + std::to_string(round * 10 + j));
    ++round;
  } while (cursor != 0);
  for (int i = 0; i < 500; ++i) EXPECT_TRUE(seen.count("stay" + std::to_string(i))) << i;
}

TEST(KeyspaceKeyspace, AScanFiltersByTypeAndSkipsTheExpired) {
  dannan::test::SteppedWall wall;
  Keyspace keys(wall);
  keys.put("a", string_entry("v"));
  keys.put("b", string_entry("v", wall.unix_ms() + 1));
  wall.advance(1ms);
  auto page = keys.scan(0, 100, Type::string);
  EXPECT_EQ(page.keys, std::vector<std::string>{"a"});
  EXPECT_EQ(page.cursor, 0u);
  EXPECT_TRUE(keys.scan(0, 100, Type::list).keys.empty());
}

TEST(KeyspaceKeyspace, ARandomKeyIsLive) {
  dannan::test::SteppedWall wall;
  Keyspace keys(wall);
  EXPECT_EQ(keys.random_key(7), std::nullopt);
  keys.put("dead", string_entry("v", wall.unix_ms() + 1));
  keys.put("live", string_entry("v"));
  wall.advance(1ms);
  for (uint64_t seed : {0ull, 1ull << 40, ~0ull}) EXPECT_EQ(keys.random_key(seed), "live");
}

// An expired key is expired wherever it is met, and told as expired once: a
// write over it (which then makes a new key), an erase (which then finds
// nothing), the full listing, and a random pick that finds only the dead.
TEST(KeyspaceKeyspace, AnExpiredKeyExpiresWhereverItIsMet) {
  dannan::test::SteppedWall wall;
  Keyspace keys(wall);
  std::vector<std::string> expired;
  std::vector<std::string> created;
  keys.set_listener({[&](std::string_view k) { expired.emplace_back(k); }, [&](std::string_view k) { created.emplace_back(k); }});
  for (const char* k : {"put", "erase", "listed", "picked"}) keys.put(k, string_entry("v", wall.unix_ms() + 1));
  created.clear();
  wall.advance(1ms);
  keys.put("put", string_entry("w"));
  EXPECT_EQ(expired, std::vector<std::string>{"put"});
  EXPECT_EQ(created, std::vector<std::string>{"put"});
  EXPECT_EQ(keys.expiring(), 3u);
  EXPECT_FALSE(keys.erase("erase"));
  EXPECT_EQ(expired.back(), "erase");
  keys.erase("put");
  EXPECT_EQ(keys.keys(), std::vector<std::string>{});
  EXPECT_EQ(expired.size(), 4u);
  EXPECT_EQ(keys.size(), 0u);

  for (int i = 0; i < 3; ++i) keys.put("dead" + std::to_string(i), string_entry("v", wall.unix_ms() + 1));
  wall.advance(1ms);
  EXPECT_EQ(keys.random_key(0), std::nullopt);
  EXPECT_EQ(keys.size(), 0u);
  EXPECT_EQ(expired.size(), 7u);
}

TEST(KeyspaceKeyspace, PositionsFollowTheSlot) {
  EXPECT_LT(position_of("bar"), position_of("foo"));  // slots 5061 and 12182
  EXPECT_EQ(position_of("{t}a") >> 48, position_of("{t}b") >> 48);
}

}  // namespace
}  // namespace mimir::keyspace
