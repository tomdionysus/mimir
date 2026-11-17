//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include "mimir/cluster/register.h"
#include "mimir/keyspace/slot.h"

namespace mimir::cluster {
namespace {

TEST(ClusterRegister, ANameHoldsItsDatabaseAndBinaryKey) {
  const std::string key("a\0b{x}", 6);
  const auto name = name_of(3, key);
  ASSERT_EQ(name.size(), 8u);
  EXPECT_EQ(name[0], 0);
  EXPECT_EQ(name[1], 3);
  const auto parsed = parse_name(name);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->first, 3);
  EXPECT_EQ(parsed->second, key);
  EXPECT_FALSE(parse_name(Bytes{7}));
  EXPECT_NE(name_of(0, "k"), name_of(1, "k"));
}

TEST(ClusterRegister, KeysSharingAHashTagShareAShard) {
  for (uint32_t shards : {1u, 4u, 256u, 8192u}) {
    EXPECT_EQ(shard_of("{user1000}.following", shards), shard_of("{user1000}.followers", shards));
    EXPECT_LT(shard_of("anything", shards), shards);
  }
}

TEST(ClusterRegister, ShardsAreContiguousRangesOfSlots) {
  for (uint32_t shards : {1u, 3u, 256u, 8192u}) {
    dannan::agree::Slot previous = 0;
    for (uint32_t slot = 0; slot < keyspace::slot_count; ++slot) {
      const auto shard = shard_of_slot(static_cast<uint16_t>(slot), shards);
      ASSERT_GE(shard, previous);
      ASSERT_LE(shard - previous, 1u);
      ASSERT_LT(shard, shards);
      previous = shard;
    }
    EXPECT_EQ(previous, shards - 1);
  }
}

Record sample() {
  Record record;
  record.payload = std::string(
      "\x00\x03"
      "abc"
      "\x0b\x00"
      "12345678",
      15);
  record.expires_at = 1'900'000'000'000;
  record.serial = 3;
  record.applied.push_back({"node-1/epoch/7", 1'000, "+OK\r\n"});
  record.applied.push_back({"node-2/epoch/9", 2'000, std::string("$1\r\n\0\r\n", 7)});
  return record;
}

TEST(ClusterRegister, ARecordRoundTrips) {
  const auto record = sample();
  const auto bytes = encode(record);
  ASSERT_TRUE(bytes);
  const auto back = decode(*bytes);
  ASSERT_TRUE(back);
  EXPECT_EQ(*back, record);

  Record bare;
  bare.payload = "";
  const auto bare_bytes = encode(bare);
  ASSERT_TRUE(bare_bytes);
  EXPECT_EQ(decode(*bare_bytes), bare);
}

TEST(ClusterRegister, AnEmptyRecordIsNoValue) {
  EXPECT_FALSE(encode(Record{}));
  const auto absent = decode_value(std::nullopt);
  ASSERT_TRUE(absent);
  EXPECT_TRUE(absent->empty());
  // A deleted key whose delete is remembered is a value.
  Record deleted;
  deleted.applied.push_back({"op", 1, ":1\r\n"});
  EXPECT_TRUE(encode(deleted));
}

TEST(ClusterRegister, DecodeRefusesWhatIsNotARecord) {
  const auto bytes = *encode(sample());
  EXPECT_FALSE(decode(Bytes{}));
  // Every truncation is refused.
  for (size_t length = 0; length < bytes.size(); ++length) EXPECT_FALSE(decode(std::span(bytes).first(length))) << length;
  // Trailing bytes, another format, unknown flags.
  auto longer = bytes;
  longer.push_back(0);
  EXPECT_FALSE(decode(longer));
  auto other = bytes;
  other[0] = 2;
  EXPECT_FALSE(decode(other));
  auto flags = bytes;
  flags[1] |= 0x80;
  EXPECT_FALSE(decode(flags));
  // The empty record's bytes are not a value.
  EXPECT_FALSE(decode(Bytes{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
  // A count beyond the bound, refused before anything is allocated.
  EXPECT_FALSE(decode(Bytes{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff}));
  // An operation id longer than the bound.
  Record long_id;
  long_id.applied.push_back({std::string(max_operation_id + 1, 'x'), 1, ""});
  EXPECT_FALSE(decode(*encode(long_id)));
}

TEST(ClusterRegister, AppliedOperationsAreFoundAndAgeOut) {
  Record record;
  ASSERT_TRUE(note_applied(record, {"a", 1'000, ":1\r\n"}));
  ASSERT_TRUE(note_applied(record, {"b", 2'000, ":2\r\n"}));
  ASSERT_NE(record.find("a"), nullptr);
  EXPECT_EQ(record.find("a")->reply, ":1\r\n");
  EXPECT_EQ(record.find("c"), nullptr);
  // An operation issued past the horizon drops those older than it.
  ASSERT_TRUE(note_applied(record, {"c", 1'000 + applied_horizon_ms + 1, ":3\r\n"}));
  EXPECT_EQ(record.find("a"), nullptr);
  EXPECT_NE(record.find("b"), nullptr);
  EXPECT_NE(record.find("c"), nullptr);
}

TEST(ClusterRegister, AFullRecordRefusesRatherThanForgetALiveOperation) {
  Record record;
  for (size_t i = 0; i < max_applied; ++i) ASSERT_TRUE(note_applied(record, {std::to_string(i), 5'000, ""}));
  const auto before = record;
  EXPECT_FALSE(note_applied(record, {"next", 5'001, ""}));
  EXPECT_EQ(record, before);
  // Once the others are past the horizon, it is taken.
  EXPECT_TRUE(note_applied(record, {"later", 5'000 + applied_horizon_ms + 1, ""}));
  EXPECT_EQ(record.applied.size(), 1u);
}

TEST(ClusterRegister, TheSummaryTellsRecordsApart) {
  auto other = sample();
  other.applied.back().reply = ":0\r\n";
  const auto a = summary_of(*encode(sample()));
  const auto b = summary_of(*encode(other));
  EXPECT_EQ(a.size(), 32u);
  EXPECT_LE(a.size(), dannan::agree::max_summary);
  EXPECT_NE(a, b);
  EXPECT_EQ(a, summary_of(*encode(sample())));
}

}  // namespace
}  // namespace mimir::cluster
