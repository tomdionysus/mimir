//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// DUMP's serialisation, from Valkey 8's DUMP and RESTORE documentation and
// Valkey 8.1.1's own payloads: each expected payload below is what that server
// answered DUMP with, as valkey-cli quotes it, for a value made the same way.

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <variant>

#include "dannan/test/fixtures/stepped_time.h"
#include "mimir/keyspace/rdb.h"

namespace mimir::keyspace {
namespace {

// valkey-cli's quoting undone: \xNN, \n, \r, \t, \a, \b, \" and \\.
std::string unquote(std::string_view quoted) {
  std::string out;
  for (size_t i = 0; i < quoted.size(); ++i) {
    if (quoted[i] != '\\') {
      out.push_back(quoted[i]);
      continue;
    }
    const char c = quoted[++i];
    switch (c) {
      case 'x':
        out.push_back(static_cast<char>(std::stoi(std::string(quoted.substr(i + 1, 2)), nullptr, 16)));
        i += 2;
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'a':
        out.push_back('\a');
        break;
      case 'b':
        out.push_back('\b');
        break;
      default:
        out.push_back(c);
    }
  }
  return out;
}

Entry string_entry(std::string s) { return Entry{std::move(s), std::nullopt}; }

TEST(KeyspaceRdb, Crc64IsJones) { EXPECT_EQ(rdb::crc64(0, "123456789"), 0xe9c6d914c4b8d9caull); }

TEST(KeyspaceRdb, LzfAsValkeyWritesIt) {
  const std::string fifty(50, 'a');
  auto compressed = rdb::lzf_compress(fifty, fifty.size() - 4);
  ASSERT_TRUE(compressed);
  EXPECT_EQ(*compressed, unquote(R"(\x01aa\xe0%\x00\x01aa)"));
  EXPECT_EQ(rdb::lzf_decompress(*compressed, 50), fifty);
  // What does not shrink is not compressed.
  EXPECT_FALSE(rdb::lzf_compress("abcdefghijklmnopqrstuvwxyz0123456789", 32));
  // A reference before the start, or a length that does not match, is refused.
  EXPECT_FALSE(rdb::lzf_decompress(unquote(R"(\xe0%\x00)"), 46));
  EXPECT_FALSE(rdb::lzf_decompress(*compressed, 49));
}

TEST(KeyspaceRdb, StringsAsValkeyDumpsThem) {
  EXPECT_EQ(rdb::dump(string_entry("hello")), unquote(R"(\x00\x05hello\x0b\x00\n\xadb\x05\x98\xab\xc9\x83)"));
  EXPECT_EQ(rdb::dump(string_entry("12345")), unquote(R"(\x00\xc190\x0b\x00\xf4KWTnc\xe10)"));
  EXPECT_EQ(rdb::dump(string_entry("-7")), unquote(R"(\x00\xc0\xf9\x0b\x007\xf9lC*=\xab\")"));
  EXPECT_EQ(rdb::dump(string_entry("123456789012")), unquote(R"(\x00\x0c123456789012\x0b\x00\xf8\xcf\xbc\xc2,\x89\x0e%)"));
  EXPECT_EQ(rdb::dump(string_entry(std::string(50, 'a'))), unquote(R"(\x00\xc3\t2\x01aa\xe0%\x00\x01aa\x0b\x00`s\xf5\xcaV\x8a\x12@)"));
  EXPECT_EQ(rdb::dump(string_entry("abcdefghijklmnopqrstuvwxyz0123456789")),
            unquote(R"(\x00$abcdefghijklmnopqrstuvwxyz0123456789\x0b\x00\xaa\x0e3)\x11\x12f\x84)"));
  EXPECT_EQ(
      rdb::dump(string_entry("The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog again and again.")),
      unquote(R"(\x00\xc3@D@i\x1fThe quick brown fox jumps over t \x1e\nlazy dog. T \r\xe0\x1e,\t again and`\t\x01n.\x0b\x00\xec\x93|\x16\xe0\xa5\xc2=)"));
}

TEST(KeyspaceRdb, CollectionsAsValkeyDumpsThem) {
  List list{"a", "1", "bcd", "300", "-5", "70000"};
  EXPECT_EQ(
      rdb::dump(Entry{list, std::nullopt}),
      unquote(
          R"(\x12\x01\x02\x1c\x1c\x00\x00\x00\x06\x00\x81a\x02\x01\x01\x83bcd\x04\xc1,\x02\xdf\xfb\x02\xf2p\x11\x01\x04\xff\x0b\x00@\x9c\x8a\xdeu\a-\xd5)"));
  Hash hash;
  hash.set("f", "v");
  hash.set("n", "7");
  EXPECT_EQ(rdb::dump(Entry{hash, std::nullopt}),
            unquote(R"(\x10\x12\x12\x00\x00\x00\x04\x00\x81f\x02\x81v\x02\x81n\x02\a\x01\xff\x0b\x00\xd1\xf5c\xdb\xbd\xe0\x13!)"));
  Set integers;
  for (const char* m : {"1", "2", "3"}) integers.add(m);
  EXPECT_EQ(rdb::dump(Entry{integers, std::nullopt}),
            unquote(R"(\x0b\x0e\x02\x00\x00\x00\x03\x00\x00\x00\x01\x00\x02\x00\x03\x00\x0b\x00\xcc\xdd\xe1\x91\x90\xf1\xa4\x92)"));
  Set small;
  small.add("a");
  small.add("b");
  EXPECT_EQ(rdb::dump(Entry{small, std::nullopt}), unquote(R"(\x14\r\r\x00\x00\x00\x02\x00\x81a\x02\x81b\x02\xff\x0b\x00\n\xec\n\xb4I\xa3\xd6T)"));
  SortedSet zset;
  zset.set("a", 1);
  zset.set("b", 2.5);
  EXPECT_EQ(rdb::dump(Entry{zset, std::nullopt}),
            unquote(R"(\x11\x14\x14\x00\x00\x00\x04\x00\x81a\x02\x01\x01\x81b\x02\x832.5\x04\xff\x0b\x00\xbd\x93\xd3\xd7\x0b\xdb0^)"));
  Stream stream;
  stream.append(StreamId{1, 1}, {"f", "v"});
  EXPECT_EQ(
      rdb::dump(Entry{stream, std::nullopt}),
      unquote(
          R"(\x15\x01\x10\x00\x00\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x01\x1d\x1d\x00\x00\x00\n\x00\x01\x01\x00\x01\x01\x01\x81f\x02\x00\x01\x02\x01\x00\x01\x00\x01\x81v\x02\x04\x01\xff\x01\x01\x01\x01\x01\x00\x00\x01\x00\x0b\x00\xe5Z\xac\xd2\xa8\x04\x8cs)"));
}

TEST(KeyspaceRdb, ValkeysLargerEncodingsRestore) {
  // A hash and a set in hash tables, a sorted set in a skiplist, a list in two
  // nodes: Valkey's payloads for each, read back.
  auto hash = rdb::restore(unquote(R"(\x04\x01\x01f\x01v\x0b\x00\xc4-\xfe%L\x19\xe7u)"));
  ASSERT_TRUE(std::holds_alternative<Entry>(hash));
  EXPECT_EQ(*std::get<Hash>(std::get<Entry>(hash).value).get("f"), "v");
  auto set = rdb::restore(unquote(R"(\x02\x02\x01a\x01b\x0b\x00\x97\xbb\x9cV\x16\x9c\xd4\x84)"));
  ASSERT_TRUE(std::holds_alternative<Entry>(set));
  EXPECT_TRUE(std::get<Set>(std::get<Entry>(set).value).contains("b"));
  auto zset = rdb::restore(unquote(R"(\x05\x02\x01b\x00\x00\x00\x00\x00\x00\x04@\x01a\x00\x00\x00\x00\x00\x00\xf0?\x0b\x00\x9d\xb1\nJ\xe4\xe9\x9b\x18)"));
  ASSERT_TRUE(std::holds_alternative<Entry>(zset));
  EXPECT_EQ(std::get<SortedSet>(std::get<Entry>(zset).value).score("b"), 2.5);
  auto list = rdb::restore(
      unquote(R"(\x12\x02\x02\n\n\x00\x00\x00\x01\x00\x81a\x02\xff\x02\n\n\x00\x00\x00\x01\x00\x81b\x02\xff\x0b\x00\xc3\x95|\xf3\xa3\xb6\xd4\xa7)"));
  ASSERT_TRUE(std::holds_alternative<Entry>(list));
  EXPECT_EQ(std::get<List>(std::get<Entry>(list).value), (List{"a", "b"}));
}

TEST(KeyspaceRdb, EveryTypeRoundTrips) {
  dannan::test::SteppedWall wall;
  std::vector<Entry> values;
  values.push_back(string_entry(std::string(5000, 'x') + "tail"));
  List big;
  for (int i = 0; i < 3000; ++i) big.push_back("element-" + std::to_string(i));
  values.push_back(Entry{big, std::nullopt});
  Hash wide;
  for (int i = 0; i < 300; ++i) wide.set("field" + std::to_string(i), std::to_string(i * 7));
  values.push_back(Entry{wide, std::nullopt});
  Set members;
  for (int i = 0; i < 200; ++i) members.add("m" + std::to_string(i));
  values.push_back(Entry{members, std::nullopt});
  SortedSet scores;
  for (int i = 0; i < 200; ++i) scores.set("z" + std::to_string(i), i * 0.25 - 10);
  scores.set("inf", INFINITY);
  values.push_back(Entry{scores, std::nullopt});
  Stream stream;
  for (uint64_t i = 1; i <= 250; ++i) {
    stream.append(StreamId{i, 0}, i % 3 ? std::vector<std::string>{"f", std::to_string(i)} : std::vector<std::string>{"g", "x", "h", "y"});
  }
  stream.remove(StreamId{5, 0});
  StreamGroup group;
  group.last_delivered = StreamId{10, 0};
  group.entries_read = 9;
  group.pending.emplace(StreamId{7, 0}, StreamPending{"alice", 1234, 2});
  group.consumers["alice"].pending.insert(StreamId{7, 0});
  group.consumers["bob"].seen_ms = 99;
  stream.groups.emplace("g1", std::move(group));
  values.push_back(Entry{stream, std::nullopt});
  for (const auto& value : values) {
    const std::string payload = rdb::dump(value);
    auto back = rdb::restore(payload);
    ASSERT_TRUE(std::holds_alternative<Entry>(back)) << static_cast<int>(value.type());
    EXPECT_EQ(rdb::dump(std::get<Entry>(back)), payload) << static_cast<int>(value.type());
  }
  const auto restored = rdb::restore(rdb::dump(values.back()));
  const auto& restored_stream = std::get<Stream>(std::get<Entry>(restored).value);
  EXPECT_EQ(restored_stream.entries.size(), 249u);
  EXPECT_EQ(restored_stream.max_deleted, (StreamId{5, 0}));
  EXPECT_EQ(restored_stream.groups.at("g1").pending.at(StreamId{7, 0}).consumer, "alice");
}

TEST(KeyspaceRdb, RefusesWhatDoesNotHold) {
  const std::string good = rdb::dump(string_entry("hello"));
  // A wrong checksum, a newer version, too short.
  std::string bad_crc = good;
  bad_crc.back() ^= 1;
  EXPECT_EQ(std::get<rdb::Refusal>(rdb::restore(bad_crc)), rdb::Refusal::checksum);
  std::string newer = good.substr(0, good.size() - 10);
  newer += unquote(R"(\x0c\x00)");
  const uint64_t crc = rdb::crc64(0, newer);
  for (int i = 0; i < 8; ++i) newer.push_back(static_cast<char>((crc >> (8 * i)) & 0xff));
  EXPECT_EQ(std::get<rdb::Refusal>(rdb::restore(newer)), rdb::Refusal::checksum);
  EXPECT_EQ(std::get<rdb::Refusal>(rdb::restore("abc")), rdb::Refusal::checksum);
  // A sound footer over a body that is not a value.
  const auto sealed = [](std::string body) {
    body.push_back(0x0b);
    body.push_back(0);
    const uint64_t c = rdb::crc64(0, body);
    for (int i = 0; i < 8; ++i) body.push_back(static_cast<char>((c >> (8 * i)) & 0xff));
    return body;
  };
  for (const char* body :
       {R"(\x63)", R"(\x00\x05hel)", R"(\x00\x05helloextra)", R"(\x02\x00)", R"(\x12\x01\x02\x01x)", R"(\x00\xc3\x01\xff\x00)", R"(\x09\x00)"}) {
    EXPECT_EQ(std::get<rdb::Refusal>(rdb::restore(sealed(unquote(body)))), rdb::Refusal::format) << body;
  }
}

TEST(KeyspaceRdb, Functions) {
  EXPECT_EQ(rdb::dump_functions({"#!lua name=lib\nredis.register_function(\"f\", function() return 1 end)"}),
            unquote(R"(\xf5@D#!lua name=lib\nredis.register_function(\"f\", function() return 1 end)\x0b\x00P\xdd\x1c\x8a\xdf\xca\xcb~)"));
  EXPECT_EQ(rdb::dump_functions({}), unquote(R"(\x0b\x004D\xe13\xf2\xe0KS)"));
  auto back = rdb::restore_functions(rdb::dump_functions({"one", "two"}));
  ASSERT_TRUE((std::holds_alternative<std::vector<std::string>>(back)));
  EXPECT_EQ(std::get<std::vector<std::string>>(back), (std::vector<std::string>{"one", "two"}));
}

// A restored stream fills its last node before it starts another, as Valkey's
// does: approximate trimming removes whole nodes, so where they end is visible
// (stage 3 restores a stream on every write by agreement).
TEST(KeyspaceRdb, ARestoredStreamFillsItsLastNode) {
  Stream stream;
  for (uint64_t i = 1; i <= 150; ++i) stream.append(StreamId{i, 0}, {"f", "v"});
  auto back = rdb::restore(rdb::dump(Entry{stream, std::nullopt}));
  ASSERT_TRUE(std::holds_alternative<Entry>(back));
  auto& restored = std::get<Stream>(std::get<Entry>(back).value);
  EXPECT_EQ(restored.appended, 150u);
  restored.append(StreamId{151, 0}, {"f", "v"});
  EXPECT_EQ(restored.node_of.at(StreamId{151, 0}), 1u);
  EXPECT_EQ(restored.node_of.at(StreamId{150, 0}), 1u);
  EXPECT_EQ(restored.node_of.at(StreamId{100, 0}), 0u);
  // A full last node is followed by a new one.
  Stream full;
  for (uint64_t i = 1; i <= 100; ++i) full.append(StreamId{i, 0}, {"f", "v"});
  auto again = rdb::restore(rdb::dump(Entry{full, std::nullopt}));
  auto& restored_full = std::get<Stream>(std::get<Entry>(again).value);
  restored_full.append(StreamId{101, 0}, {"f", "v"});
  EXPECT_EQ(restored_full.node_of.at(StreamId{101, 0}), 1u);
}

}  // namespace
}  // namespace mimir::keyspace
