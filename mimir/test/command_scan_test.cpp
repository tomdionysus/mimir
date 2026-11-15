//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// HSCAN, SSCAN and ZSCAN over large collections (ruling 30): a page at a time,
// each bounded by COUNT, and SCAN's guarantee, as Valkey documents it: an
// element present from the start to the end of a full iteration is returned.
// Mimir's is stronger: exactly once.

#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

// One SCAN-family reply: its cursor and its elements.
struct Page {
  std::string cursor;
  std::vector<std::string> items;
};

std::string read_line(const std::string& bytes, size_t& at) {
  const size_t end = bytes.find("\r\n", at);
  std::string line = bytes.substr(at, end - at);
  at = end + 2;
  return line;
}

std::string read_bulk(const std::string& bytes, size_t& at) {
  const std::string head = read_line(bytes, at);
  const size_t length = std::stoul(head.substr(1));
  std::string out = bytes.substr(at, length);
  at += length + 2;
  return out;
}

Page parse(const std::string& bytes) {
  size_t at = 0;
  EXPECT_EQ(read_line(bytes, at), "*2") << bytes;
  Page page;
  page.cursor = read_bulk(bytes, at);
  const size_t count = std::stoul(read_line(bytes, at).substr(1));
  for (size_t i = 0; i < count; ++i) page.items.push_back(read_bulk(bytes, at));
  return page;
}

// Scans to the end, calling `between` after each page; every element seen, with
// how often.
template <typename Between>
std::map<std::string, int> scan_all(Bed& bed, const std::string& command, const std::string& key, size_t stride, size_t count, Between between, size_t& pages) {
  std::map<std::string, int> seen;
  std::string cursor = "0";
  pages = 0;
  do {
    Page page = parse(bed.run({command, key, cursor, "COUNT", std::to_string(count)}));
    EXPECT_LE(page.items.size(), count * stride) << "a page is bounded by COUNT";
    for (size_t i = 0; i < page.items.size(); i += stride) ++seen[page.items[i]];
    cursor = page.cursor;
    ++pages;
    between(pages);
  } while (cursor != "0" && pages < 10000);
  return seen;
}

TEST(CommandScan, ALargeHashIsPagedAndEveryFieldReturnedOnce) {
  Bed bed;
  for (int i = 0; i < 300; ++i) bed.run({"HSET", "h", "f" + std::to_string(i), "v" + std::to_string(i)});
  size_t pages = 0;
  auto seen = scan_all(bed, "HSCAN", "h", 2, 50, [](size_t) {}, pages);
  EXPECT_EQ(seen.size(), 300u);
  for (const auto& [field, times] : seen) EXPECT_EQ(times, 1) << field;
  EXPECT_GE(pages, 6u);
  // A value travels with its field.
  Page first = parse(bed.run({"HSCAN", "h", "0", "COUNT", "1"}));
  ASSERT_EQ(first.items.size(), 2u);
  EXPECT_EQ(first.items[1], "v" + first.items[0].substr(1));
  EXPECT_NE(first.cursor, "0");
}

TEST(CommandScan, ASmallHashIsOnePage) {
  Bed bed;
  for (int i = 0; i < 128; ++i) bed.run({"HSET", "h", "f" + std::to_string(i), "v"});
  Page page = parse(bed.run({"HSCAN", "h", "0", "COUNT", "1", "NOVALUES"}));
  EXPECT_EQ(page.cursor, "0");
  EXPECT_EQ(page.items.size(), 128u);
}

TEST(CommandScan, FieldsPresentThroughoutAreReturnedDespiteChanges) {
  Bed bed;
  for (int i = 0; i < 400; ++i) bed.run({"HSET", "h", "f" + std::to_string(i), "v"});
  // After the first page, half the fields go (some already returned, some not)
  // and new ones come; every field present throughout is still returned once.
  size_t pages = 0;
  auto seen = scan_all(
      bed, "HSCAN", "h", 2, 25,
      [&](size_t page) {
        if (page != 1) return;
        for (int i = 0; i < 400; i += 2) bed.run({"HDEL", "h", "f" + std::to_string(i)});
        for (int i = 400; i < 500; ++i) bed.run({"HSET", "h", "f" + std::to_string(i), "v"});
      },
      pages);
  for (int i = 1; i < 400; i += 2) EXPECT_EQ(seen["f" + std::to_string(i)], 1) << i;
  for (const auto& [field, times] : seen) EXPECT_EQ(times, 1) << field;
}

TEST(CommandScan, ALargeSetIsPagedAndEveryMemberReturnedOnce) {
  Bed bed;
  for (int i = 0; i < 300; ++i) bed.run({"SADD", "s", "m" + std::to_string(i)});
  size_t pages = 0;
  auto seen = scan_all(
      bed, "SSCAN", "s", 1, 40,
      [&](size_t page) {
        if (page == 2) bed.run({"SREM", "s", "m0", "m299"});
      },
      pages);
  for (int i = 1; i < 299; ++i) EXPECT_EQ(seen["m" + std::to_string(i)], 1) << i;
  for (const auto& [member, times] : seen) EXPECT_EQ(times, 1) << member;
  EXPECT_GE(pages, 7u);
}

TEST(CommandScan, AnIntegerSetIsOnePageAsValkeysIntset) {
  Bed bed;
  for (int i = 0; i < 500; ++i) bed.run({"SADD", "s", std::to_string(i)});
  Page page = parse(bed.run({"SSCAN", "s", "0", "COUNT", "5"}));
  EXPECT_EQ(page.cursor, "0");
  EXPECT_EQ(page.items.size(), 500u);
  EXPECT_EQ(page.items.front(), "0");
}

TEST(CommandScan, ALargeSortedSetIsPagedWithItsScores) {
  Bed bed;
  for (int i = 0; i < 300; ++i) bed.run({"ZADD", "z", std::to_string(i), "m" + std::to_string(i)});
  size_t pages = 0;
  std::map<std::string, int> seen;
  std::string cursor = "0";
  do {
    Page page = parse(bed.run({"ZSCAN", "z", cursor, "COUNT", "64"}));
    EXPECT_LE(page.items.size(), 128u);
    for (size_t i = 0; i + 1 < page.items.size(); i += 2) {
      ++seen[page.items[i]];
      if (page.items[i] == "m299")
        EXPECT_TRUE(page.items[i + 1] == "299" || page.items[i + 1] == "1000");
      else
        EXPECT_EQ(page.items[i], "m" + page.items[i + 1]);
    }
    cursor = page.cursor;
    // A score changed mid-scan keeps the member's place in the scan.
    if (++pages == 1) bed.run({"ZADD", "z", "1000", "m299"});
  } while (cursor != "0" && pages < 100);
  EXPECT_EQ(seen.size(), 300u);
  for (const auto& [member, times] : seen) EXPECT_EQ(times, 1) << member;
}

TEST(CommandScan, MatchFiltersAPageAndTheCursorStillRunsOn) {
  Bed bed;
  for (int i = 0; i < 200; ++i) bed.run({"SADD", "s", (i % 2 ? "odd" : "even") + std::to_string(i)});
  size_t pages = 0;
  std::set<std::string> seen;
  std::string cursor = "0";
  do {
    Page page = parse(bed.run({"SSCAN", "s", cursor, "MATCH", "odd*", "COUNT", "30"}));
    for (const auto& member : page.items) {
      EXPECT_EQ(member.rfind("odd", 0), 0u);
      seen.insert(member);
    }
    cursor = page.cursor;
  } while (cursor != "0" && ++pages < 100);
  EXPECT_EQ(seen.size(), 100u);
}

TEST(CommandScan, ACopiedCollectionScansOnItsOwn) {
  Bed bed;
  for (int i = 0; i < 200; ++i) bed.run({"HSET", "h", "f" + std::to_string(i), "v"});
  parse(bed.run({"HSCAN", "h", "0"}));
  ASSERT_EQ(bed.run({"COPY", "h", "c"}), integer(1));
  bed.run({"HDEL", "h", "f1"});
  size_t pages = 0;
  auto seen = scan_all(bed, "HSCAN", "c", 2, 50, [](size_t) {}, pages);
  EXPECT_EQ(seen.size(), 200u);
}

}  // namespace
}  // namespace mimir::test
