//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The bitmap commands, from Valkey 8's command documentation, their examples
// among them: bit 0 is the most significant bit of the first byte, and a
// string grows with zero bytes for a bit set past its end.

#include <gtest/gtest.h>

#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

TEST(CommandBitmaps, SetAndGetBit) {
  Bed bed;
  EXPECT_EQ(bed.run({"SETBIT", "mykey", "7", "1"}), integer(0));
  EXPECT_EQ(bed.run({"SETBIT", "mykey", "7", "0"}), integer(1));
  EXPECT_EQ(bed.run({"GET", "mykey"}), bulk(std::string(1, '\0')));
  EXPECT_EQ(bed.run({"SETBIT", "mykey", "0", "1"}), integer(0));
  EXPECT_EQ(bed.run({"GET", "mykey"}), bulk("\x80"));
  EXPECT_EQ(bed.run({"GETBIT", "mykey", "0"}), integer(1));
  EXPECT_EQ(bed.run({"GETBIT", "mykey", "100"}), integer(0));
  EXPECT_EQ(bed.run({"GETBIT", "nosuch", "0"}), integer(0));
  EXPECT_EQ(bed.run({"SETBIT", "grow", "17", "1"}), integer(0));
  EXPECT_EQ(bed.run({"STRLEN", "grow"}), integer(3));
  EXPECT_EQ(bed.run({"SETBIT", "k", "0", "2"}), error("ERR bit is not an integer or out of range"));
  EXPECT_EQ(bed.run({"SETBIT", "k", "-1", "1"}), error("ERR bit offset is not an integer or out of range"));
  EXPECT_EQ(bed.run({"SETBIT", "k", "4294967296", "1"}), error("ERR bit offset is not an integer or out of range"));
}

TEST(CommandBitmaps, Count) {
  Bed bed;
  bed.run({"SET", "mykey", "foobar"});
  EXPECT_EQ(bed.run({"BITCOUNT", "mykey"}), integer(26));
  EXPECT_EQ(bed.run({"BITCOUNT", "mykey", "0", "0"}), integer(4));
  EXPECT_EQ(bed.run({"BITCOUNT", "mykey", "1", "1"}), integer(6));
  EXPECT_EQ(bed.run({"BITCOUNT", "mykey", "1", "1", "BYTE"}), integer(6));
  EXPECT_EQ(bed.run({"BITCOUNT", "mykey", "5", "30", "BIT"}), integer(17));
  EXPECT_EQ(bed.run({"BITCOUNT", "mykey", "-2", "-1"}), integer(7));
  EXPECT_EQ(bed.run({"BITCOUNT", "nosuch"}), integer(0));
  // A start alone runs to the end, as Valkey 8 reads it.
  EXPECT_EQ(bed.run({"BITCOUNT", "mykey", "1"}), integer(22));
  EXPECT_EQ(bed.run({"BITCOUNT", "mykey", "0", "1", "WORD"}), error("ERR syntax error"));
}

TEST(CommandBitmaps, Pos) {
  Bed bed;
  bed.run({"SET", "mykey", "\xff\xf0"});
  EXPECT_EQ(bed.run({"BITPOS", "mykey", "0"}), integer(12));
  bed.run({"SET", "mykey", std::string("\x00\xff\xf0", 3)});
  EXPECT_EQ(bed.run({"BITPOS", "mykey", "1", "0"}), integer(8));
  EXPECT_EQ(bed.run({"BITPOS", "mykey", "1", "2"}), integer(16));
  EXPECT_EQ(bed.run({"BITPOS", "mykey", "1", "2", "-1", "BYTE"}), integer(16));
  EXPECT_EQ(bed.run({"BITPOS", "mykey", "1", "7", "15", "BIT"}), integer(8));
  bed.run({"SET", "zeros", std::string(3, '\0')});
  EXPECT_EQ(bed.run({"BITPOS", "zeros", "1"}), integer(-1));
  // All ones and no end: the first clear bit is just past the string.
  bed.run({"SET", "ones", "\xff\xff"});
  EXPECT_EQ(bed.run({"BITPOS", "ones", "0"}), integer(16));
  EXPECT_EQ(bed.run({"BITPOS", "ones", "0", "0", "-1"}), integer(-1));
  EXPECT_EQ(bed.run({"BITPOS", "nosuch", "1"}), integer(-1));
  EXPECT_EQ(bed.run({"BITPOS", "nosuch", "0"}), integer(0));
  EXPECT_EQ(bed.run({"BITPOS", "mykey", "2"}), error("ERR The bit argument must be 1 or 0."));
}

TEST(CommandBitmaps, Op) {
  Bed bed;
  bed.run({"SET", "key1", "foobar"});
  bed.run({"SET", "key2", "abcdef"});
  EXPECT_EQ(bed.run({"BITOP", "AND", "dest", "key1", "key2"}), integer(6));
  EXPECT_EQ(bed.run({"GET", "dest"}), bulk("`bc`ab"));
  EXPECT_EQ(bed.run({"BITOP", "OR", "dest", "key1", "nosuch"}), integer(6));
  EXPECT_EQ(bed.run({"GET", "dest"}), bulk("foobar"));
  EXPECT_EQ(bed.run({"BITOP", "NOT", "dest", "key1"}), integer(6));
  EXPECT_EQ(bed.run({"BITOP", "NOT", "dest", "key1", "key2"}), error("ERR BITOP NOT must be called with a single source key."));
  EXPECT_EQ(bed.run({"BITOP", "NAND", "dest", "key1"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"BITOP", "AND", "dest", "nosuch"}), integer(0));
  EXPECT_EQ(bed.run({"EXISTS", "dest"}), integer(0));
}

TEST(CommandBitmaps, Field) {
  Bed bed;
  EXPECT_EQ(bed.run({"BITFIELD", "mykey", "INCRBY", "i5", "100", "1", "GET", "u4", "0"}), array({integer(1), integer(0)}));
  EXPECT_EQ(bed.run({"BITFIELD", "mystring", "SET", "i8", "#0", "100", "SET", "i8", "#1", "200"}), array({integer(0), integer(0)}));
  EXPECT_EQ(bed.run({"BITFIELD", "mystring", "GET", "i8", "#0", "GET", "i8", "#1", "GET", "u8", "8"}), array({integer(100), integer(-56), integer(200)}));
  // Overflow: wrap, saturate, fail.
  bed.run({"SET", "c", std::string(1, '\0')});
  EXPECT_EQ(bed.run({"BITFIELD", "c", "INCRBY", "u2", "100", "1", "OVERFLOW", "SAT", "INCRBY", "u2", "102", "1"}), array({integer(1), integer(1)}));
  EXPECT_EQ(bed.run({"BITFIELD", "c", "INCRBY", "u2", "100", "1", "OVERFLOW", "SAT", "INCRBY", "u2", "102", "1"}), array({integer(2), integer(2)}));
  EXPECT_EQ(bed.run({"BITFIELD", "c", "INCRBY", "u2", "100", "1", "OVERFLOW", "SAT", "INCRBY", "u2", "102", "1"}), array({integer(3), integer(3)}));
  EXPECT_EQ(bed.run({"BITFIELD", "c", "INCRBY", "u2", "100", "1", "OVERFLOW", "SAT", "INCRBY", "u2", "102", "1"}), array({integer(0), integer(3)}));
  EXPECT_EQ(bed.run({"BITFIELD", "c", "OVERFLOW", "FAIL", "INCRBY", "u2", "102", "1"}), array({nil()}));
  EXPECT_EQ(bed.run({"BITFIELD", "c", "GET", "i64", "0", "GET", "i1", "0"}).substr(0, 4), "*2\r\n");
  EXPECT_EQ(bed.run({"BITFIELD", "c", "GET", "u64", "0"}),
            error("ERR Invalid bitfield type. Use something like i16 u8. Note that u64 is not supported but i64 is."));
  EXPECT_EQ(bed.run({"BITFIELD", "c", "OVERFLOW", "MAYBE"}), error("ERR Invalid OVERFLOW type specified"));
  EXPECT_EQ(bed.run({"BITFIELD_RO", "mystring", "GET", "i8", "0"}), array({integer(100)}));
  EXPECT_EQ(bed.run({"BITFIELD_RO", "mystring", "SET", "i8", "0", "1"}), error("ERR BITFIELD_RO only supports the GET subcommand"));
  EXPECT_EQ(bed.run({"BITFIELD", "nosuch", "GET", "u8", "0"}), array({integer(0)}));
  EXPECT_EQ(bed.run({"EXISTS", "nosuch"}), integer(0));
}

TEST(CommandBitmaps, OverflowRulesAndEmptyRanges) {
  Bed bed;
  EXPECT_EQ(bed.run({"BITFIELD", "b", "OVERFLOW", "WRAP", "INCRBY", "u8", "0", "300"}), array({integer(44)}));
  EXPECT_EQ(bed.run({"BITFIELD", "c", "OVERFLOW", "FAIL", "SET", "u8", "0", "300"}), array({nil()}));
  EXPECT_EQ(bed.run({"BITFIELD", "b", "BOGUS"}), error("ERR syntax error"));
  // A start past the end counts nothing and finds nothing.
  EXPECT_EQ(bed.run({"BITCOUNT", "b", "5", "2"}), integer(0));
  EXPECT_EQ(bed.run({"BITPOS", "b", "1", "5", "2"}), integer(-1));
}

}  // namespace
}  // namespace mimir::test
