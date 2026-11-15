//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Hash slots, from the cluster specification ("Keys distribution model" and
// "Hash tags"): CRC16 XMODEM, whose check value over "123456789" is 0x31C3,
// modulo 16384, over the hash tag when there is one.

#include <gtest/gtest.h>

#include "mimir/keyspace/glob.h"
#include "mimir/keyspace/slot.h"

namespace mimir::keyspace {
namespace {

TEST(KeyspaceSlot, Crc16IsXmodem) {
  EXPECT_EQ(crc16("123456789"), 0x31C3);
  EXPECT_EQ(crc16(""), 0);
}

TEST(KeyspaceSlot, KnownKeysLandInTheirSlots) {
  EXPECT_EQ(key_slot("foo"), 12182);
  EXPECT_EQ(key_slot("bar"), 5061);
  EXPECT_EQ(key_slot("123456789"), 0x31C3 % 16384);
}

TEST(KeyspaceSlot, AHashTagDecidesTheSlot) {
  EXPECT_EQ(key_slot("{user1000}.following"), key_slot("{user1000}.followers"));
  EXPECT_EQ(key_slot("{user1000}.following"), key_slot("user1000"));
  // Only the first '{' and the first '}' after it count.
  EXPECT_EQ(key_slot("foo{bar}{zap}"), key_slot("bar"));
  EXPECT_EQ(key_slot("foo{{bar}}zap"), key_slot("{bar"));
}

TEST(KeyspaceSlot, AnEmptyOrUnclosedTagIsNoTag) {
  EXPECT_EQ(key_slot("foo{}{bar}"), static_cast<uint16_t>(crc16("foo{}{bar}") % 16384));
  EXPECT_EQ(key_slot("foo{bar"), static_cast<uint16_t>(crc16("foo{bar") % 16384));
}

TEST(KeyspaceGlob, TheDocumentedPatterns) {
  // KEYS' documentation: h?llo, h*llo, h[ae]llo, h[^e]llo, h[a-b]llo.
  EXPECT_TRUE(glob_match("h?llo", "hello"));
  EXPECT_TRUE(glob_match("h?llo", "hallo"));
  EXPECT_FALSE(glob_match("h?llo", "hllo"));
  EXPECT_TRUE(glob_match("h*llo", "hllo"));
  EXPECT_TRUE(glob_match("h*llo", "heeeello"));
  EXPECT_TRUE(glob_match("h[ae]llo", "hello"));
  EXPECT_FALSE(glob_match("h[ae]llo", "hillo"));
  EXPECT_TRUE(glob_match("h[^e]llo", "hallo"));
  EXPECT_FALSE(glob_match("h[^e]llo", "hello"));
  EXPECT_TRUE(glob_match("h[a-b]llo", "hbllo"));
  EXPECT_FALSE(glob_match("h[a-b]llo", "hcllo"));
}

TEST(KeyspaceGlob, EscapesAndEdges) {
  EXPECT_TRUE(glob_match("a\\*b", "a*b"));
  EXPECT_FALSE(glob_match("a\\*b", "axb"));
  EXPECT_TRUE(glob_match("*", ""));
  EXPECT_TRUE(glob_match("", ""));
  EXPECT_FALSE(glob_match("", "a"));
  EXPECT_TRUE(glob_match("[z-a]", "m"));
  EXPECT_TRUE(glob_match("[\\]]", "]"));
  EXPECT_TRUE(glob_match("*a*b*c", "xxaxxbxxc"));
  EXPECT_FALSE(glob_match("*a*b*c", "xxaxxbxx"));
}

TEST(KeyspaceGlob, CaseFoldingWhenAsked) {
  EXPECT_FALSE(glob_match("HELLO", "hello"));
  EXPECT_TRUE(glob_match("HELLO", "hello", true));
  EXPECT_TRUE(glob_match("[A-C]x", "bx", true));
}

TEST(KeyspaceGlob, AHostilePatternIsNotExponential) {
  // Valkey's own regression: many stars against a long subject that fails.
  const std::string pattern = "a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*b";
  const std::string subject(4096, 'a');
  EXPECT_FALSE(glob_match(pattern, subject));
}

}  // namespace
}  // namespace mimir::keyspace
