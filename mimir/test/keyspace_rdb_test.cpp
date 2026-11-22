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

std::string from_hex(std::string_view hex) {
  std::string out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<char>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
  return out;
}

std::string to_hex(std::string_view bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : bytes) {
    out.push_back(digits[c >> 4]);
    out.push_back(digits[c & 15]);
  }
  return out;
}

// Letters that LZF barely shrinks: x = (x*75+74) mod 65537 from 1, each 'a' +
// x mod 26. The reference values were made from the same sequence.
std::string letters(size_t n) {
  std::string out;
  uint64_t x = 1;
  for (size_t i = 0; i < n; ++i) {
    x = (x * 75 + 74) % 65537;
    out.push_back(static_cast<char>('a' + x % 26));
  }
  return out;
}

// A body sealed with the version and a sound checksum.
std::string sealed(std::string body) {
  body.push_back(0x0b);
  body.push_back(0);
  const uint64_t c = rdb::crc64(0, body);
  for (int i = 0; i < 8; ++i) body.push_back(static_cast<char>((c >> (8 * i)) & 0xff));
  return body;
}

// A payload too long to quote: its length, first 16 bytes and checksum.
void expect_payload(const std::string& payload, size_t length, std::string_view head, std::string_view crc) {
  EXPECT_EQ(payload.size(), length);
  EXPECT_EQ(to_hex(payload.substr(0, 16)), head);
  EXPECT_EQ(to_hex(payload.substr(payload.size() - 8)), crc);
}

List restored_list(const std::string& payload) {
  auto back = rdb::restore(payload);
  if (!std::holds_alternative<Entry>(back)) return {};
  return std::get<List>(std::get<Entry>(back).value);
}

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

// Valkey 8.1.1's DUMP of SET n 100000, SET nn -100000 and SET big to 20,000
// letters: integers past 16 bits in four bytes, and LZF lengths past 14 bits
// in four.
TEST(KeyspaceRdb, LongerStringsAsValkeyDumpsThem) {
  EXPECT_EQ(to_hex(rdb::dump(string_entry("100000"))), "00c2a08601000b0098fbbe2392b04ffd");
  EXPECT_EQ(to_hex(rdb::dump(string_entry("-100000"))), "00c26079feff0b00c815390331be8414");
  const std::string big = rdb::dump(string_entry(letters(20000)));
  expect_payload(big, 18843, "00c380000049858000004e201f747262", "d41f4c119031e65e");
  for (const char* s : {"100000", "-100000"}) {
    auto back = rdb::restore(rdb::dump(string_entry(s)));
    ASSERT_TRUE(std::holds_alternative<Entry>(back));
    EXPECT_EQ(std::get<std::string>(std::get<Entry>(back).value), s);
  }
  auto back = rdb::restore(big);
  ASSERT_TRUE(std::holds_alternative<Entry>(back));
  EXPECT_EQ(std::get<std::string>(std::get<Entry>(back).value), letters(20000));
}

// Valkey 8.1.1's DUMP of lists made by RPUSH: a listpack's wider integers and
// longer strings; and an element over 8,192 bytes (list-max-listpack-size -2's
// node size) in a plain node of its own, the next element in a new node.
TEST(KeyspaceRdb, ListsOfLargerElementsAsValkeyDumpsThem) {
  const auto check = [](const List& list, std::string_view hex) {
    const std::string payload = rdb::dump(Entry{list, std::nullopt});
    EXPECT_EQ(to_hex(payload), hex);
    EXPECT_EQ(restored_list(payload), list);
  };
  check(List{"10000000", "5000000000", "-100000"}, "1201021c1c0000000300f38096980005f400f2052a0100000009f26079fe04ff0b004138cc56cba35eaa");
  check(List{std::string(100, 'x')}, "120102c310406e086e0000000100e06478e05a000166ff0b0007efd7ac9f6462af");
  // LZF of a run: a prefix, `repeats` back-references e0ff00, a suffix.
  const auto run = [](std::string_view prefix, int repeats, std::string_view suffix) {
    std::string hex(prefix);
    for (int i = 0; i < repeats; ++i) hex += "e0ff00";
    hex += suffix;
    return hex;
  };
  check(List{std::string(5000, 'y')}, run("120102c3404a539607961300000100f08820060079", 18, "e0ee0002278dff0b0073e66a840eb77e77"));
  check(List{std::string(20000, 'w')}, run("120101c340ea8000004e20017777", 75, "e0bb000177770b008119c55180ec84ba"));
  check(List{std::string(8192, 'u'), "tail"},
        run("120202c34070600e070e2000000100f00020060075", 31, "a000024085ff020d0d0000000100847461696c05ff0b00c87bdeefe6c146d5"));
  check(List{std::string(8193, 'u'), "tail"}, run("120201c340656001017575", 31, "6000017575020d0d0000000100847461696c05ff0b007abd70e7dfc6914b"));

  const List huge{std::string(2200000, 'v')};
  const std::string huge_payload = rdb::dump(Entry{huge, std::nullopt});
  expect_payload(huge_payload, 25032, "120101c380000061b080002191c00176", "5e3f73ad4ea1c83f");
  EXPECT_EQ(restored_list(huge_payload), huge);
  const List lettered{letters(20000)};
  const std::string lettered_payload = rdb::dump(Entry{lettered, std::nullopt});
  expect_payload(lettered_payload, 18845, "120101c380000049858000004e201f74", "3f427e343d02c615");
  EXPECT_EQ(restored_list(lettered_payload), lettered);
}

// Valkey 8.1.1's DUMP of XADD s 1-1 f <value> for a 20,000-letter value and a
// 2,200,000-byte one: listpack entries whose back-lengths take three bytes and
// four.
TEST(KeyspaceRdb, StreamsOfLargeValuesAsValkeyDumpsThem) {
  const auto stream_of = [](std::string value) {
    Stream stream;
    stream.append(StreamId{1, 1}, {"f", std::move(value)});
    return Entry{std::move(stream), std::nullopt};
  };
  const std::string s1 = rdb::dump(stream_of(letters(20000)));
  expect_payload(s1, 18904, "15011000000000000000010000000000", "676c9263fff7bccd");
  const std::string s2 = rdb::dump(stream_of(std::string(2200000, 'v')));
  expect_payload(s2, 25089, "15011000000000000000010000000000", "ac01d04049eb5d91");
  for (const auto* payload : {&s1, &s2}) {
    auto back = rdb::restore(*payload);
    ASSERT_TRUE(std::holds_alternative<Entry>(back));
    EXPECT_EQ(rdb::dump(std::get<Entry>(back)), *payload);
  }
}

// Bodies in the encodings before Valkey 7, built by hand, that Valkey 8.1.1
// restores (with sanitize-dump-payload yes), and what it reads back from each.
TEST(KeyspaceRdb, OlderEncodingsRestoreAsValkeyReadsThem) {
  EXPECT_EQ(restored_list(sealed(from_hex("010201610162"))), (List{"a", "b"}));
  // A ziplist with every entry kind: 6-bit, 14-bit and 32-bit strings, 16-,
  // 32-, 64-, 24- and 8-bit integers, an immediate, and a five-byte previous
  // length after the 303-byte entry.
  std::string ziplist = "0a4166660100005a010000090000016103c02c0104d0a086010006e000f2052a010000000af06079fe05fefb03f802412c";
  for (int i = 0; i < 300; ++i) ziplist += "78";
  ziplist +=
      "fe2f010000"
      "80000000017a"
      "ff";
  List expected{"a", "300", "100000", "5000000000", "-100000", "-5", "7", std::string(300, 'x'), "z"};
  EXPECT_EQ(restored_list(sealed(from_hex(ziplist))), expected);
  // Quicklist version 1 (a node is a ziplist), and a plain node in version 2.
  EXPECT_EQ(restored_list(sealed(from_hex("0e0111110000000d0000000200000161030162ff"))), (List{"a", "b"}));
  EXPECT_EQ(restored_list(sealed(from_hex("1201010568656c6c6f"))), (List{"hello"}));
  // A ziplist string entry headed 0x81: Valkey reads any 10xxxxxx as a 32-bit
  // length, its low bits ignored.
  EXPECT_EQ(restored_list(sealed(from_hex("0a12120000000a00000001000081000000017aff"))), (List{"z"}));

  // The first sorted set type, scores as text (254 is +inf, 255 -inf), and a
  // sorted set in a ziplist.
  auto old = rdb::restore(sealed(from_hex("0303016103312e350162fe0163ff")));
  ASSERT_TRUE(std::holds_alternative<Entry>(old));
  const auto& old_zset = std::get<SortedSet>(std::get<Entry>(old).value);
  EXPECT_EQ(old_zset.score("a"), 1.5);
  EXPECT_EQ(old_zset.score("b"), INFINITY);
  EXPECT_EQ(old_zset.score("c"), -INFINITY);
  auto zl = rdb::restore(sealed(from_hex("0c181800000012000000040000016103f30201620303322e35ff")));
  ASSERT_TRUE(std::holds_alternative<Entry>(zl));
  EXPECT_EQ(std::get<SortedSet>(std::get<Entry>(zl).value).score("a"), 2);
  EXPECT_EQ(std::get<SortedSet>(std::get<Entry>(zl).value).score("b"), 2.5);

  // The first stream type: no first id, deleted maximum or entries added,
  // which Valkey reports as the stream's length.
  auto v1 = rdb::restore(sealed(from_hex("0f0110000000000000000100000000000000011d1d0000000a0001010001010181660200010201000100018176020401ff01010100")));
  ASSERT_TRUE(std::holds_alternative<Entry>(v1));
  const auto& stream = std::get<Stream>(std::get<Entry>(v1).value);
  EXPECT_EQ(stream.entries.size(), 1u);
  EXPECT_EQ(stream.entries_added, 1u);
  EXPECT_EQ(stream.last_id, (StreamId{1, 1}));
}

// Bodies Valkey 8.1.1 refuses with "Bad data format": a quicklist node
// container that is neither plain nor packed, a ziplist integer header it does
// not know (0xc1), a listpack entry header it does not know (0xf5), a NaN score
// in the first sorted set type, and a length header 0x82.
TEST(KeyspaceRdb, RefusesTheOlderEncodingsValkeyRefuses) {
  for (const char* hex : {"1201030161", "0a0f0f0000000a000000010000c10000ff", "12010209090000000100f501ff", "03010161fd", "0082000000000568656c6c6f"}) {
    EXPECT_EQ(std::get<rdb::Refusal>(rdb::restore(sealed(from_hex(hex)))), rdb::Refusal::format) << hex;
  }
  // A function payload whose code is cut short.
  EXPECT_EQ(std::get<rdb::Refusal>(rdb::restore_functions(sealed(from_hex("f5056162")))), rdb::Refusal::format);
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
