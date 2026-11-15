//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// DUMP and RESTORE, FUNCTION DUMP and FUNCTION RESTORE, from Valkey 8's
// documentation: a key's value serialised and made again under any name, with
// a TTL in milliseconds (or an absolute one), refused onto an existing key
// without REPLACE, and refused when the payload's version or checksum is
// wrong or its body does not parse.

#include <gtest/gtest.h>

#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

using namespace std::chrono_literals;

// The bulk string a reply holds.
std::string payload_of(const std::string& reply) {
  const size_t end = reply.find("\r\n");
  const size_t n = std::stoul(reply.substr(1, end - 1));
  return reply.substr(end + 2, n);
}

TEST(CommandDump, RoundTripsEveryType) {
  Bed bed;
  bed.run({"SET", "s", "hello"});
  bed.run({"RPUSH", "l", "a", "1", "b"});
  bed.run({"HSET", "h", "f", "v"});
  bed.run({"SADD", "i", "3", "1", "2"});
  bed.run({"ZADD", "z", "1", "a", "2.5", "b"});
  bed.run({"XADD", "x", "1-1", "f", "v"});
  bed.run({"XGROUP", "CREATE", "x", "g", "0"});
  for (const char* key : {"s", "l", "h", "i", "z", "x"}) {
    const std::string dumped = payload_of(bed.run({"DUMP", key}));
    EXPECT_EQ(bed.run({"RESTORE", std::string(key) + "2", "0", dumped}), ok()) << key;
    EXPECT_EQ(payload_of(bed.run({"DUMP", std::string(key) + "2"})), dumped) << key;
  }
  EXPECT_EQ(bed.run({"LRANGE", "l2", "0", "-1"}), array({bulk("a"), bulk("1"), bulk("b")}));
  EXPECT_EQ(bed.run({"ZSCORE", "z2", "b"}), bulk("2.5"));
  EXPECT_EQ(bed.run({"XINFO", "GROUPS", "x2"}).find("$1\r\ng\r\n") != std::string::npos, true);
  EXPECT_EQ(bed.run({"DUMP", "nosuch"}), nil());
}

TEST(CommandDump, RestoreErrorsInValkeysOrder) {
  Bed bed;
  bed.run({"SET", "k", "v"});
  const std::string good = payload_of(bed.run({"DUMP", "k"}));
  EXPECT_EQ(bed.run({"RESTORE", "k", "0", good}), error("BUSYKEY Target key name already exists."));
  EXPECT_EQ(bed.run({"RESTORE", "k", "-1", "x", "REPLACE"}), error("ERR Invalid TTL value, must be >= 0"));
  EXPECT_EQ(bed.run({"RESTORE", "k", "x", "x", "REPLACE"}), error("ERR value is not an integer or out of range"));
  EXPECT_EQ(bed.run({"RESTORE", "k", "0", "x", "REPLACE"}), error("ERR DUMP payload version or checksum are wrong"));
  EXPECT_EQ(bed.run({"RESTORE", "k", "0", "x", "WAT"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"RESTORE", "k", "0", "x", "IDLETIME", "-1"}), error("ERR Invalid IDLETIME value, must be >= 0"));
  EXPECT_EQ(bed.run({"RESTORE", "k", "0", "x", "FREQ", "300"}), error("ERR Invalid FREQ value, must be >= 0 and <= 255"));
  EXPECT_EQ(bed.run({"RESTORE", "k", "0", "x", "IDLETIME", "1", "FREQ", "1"}), error("ERR syntax error"));
  std::string corrupt = good;
  corrupt[2] ^= 1;
  EXPECT_EQ(bed.run({"RESTORE", "other", "0", corrupt}), error("ERR DUMP payload version or checksum are wrong"));
}

TEST(CommandDump, RestoreTtl) {
  Bed bed;
  bed.run({"SET", "k", "v"});
  const std::string dumped = payload_of(bed.run({"DUMP", "k"}));
  EXPECT_EQ(bed.run({"RESTORE", "a", "5000", dumped}), ok());
  EXPECT_EQ(bed.run({"PTTL", "a"}), integer(5000));
  const uint64_t at = bed.wall.unix_ms() + 7000;
  EXPECT_EQ(bed.run({"RESTORE", "b", std::to_string(at), dumped, "ABSTTL"}), ok());
  EXPECT_EQ(bed.run({"PTTL", "b"}), integer(7000));
  // An absolute time already past makes nothing, and replaces away what was there.
  EXPECT_EQ(bed.run({"RESTORE", "a", "1", dumped, "ABSTTL", "REPLACE"}), ok());
  EXPECT_EQ(bed.run({"EXISTS", "a"}), integer(0));
  EXPECT_EQ(bed.run({"RESTORE", "c", "0", dumped, "IDLETIME", "10", "REPLACE"}), ok());
  EXPECT_EQ(bed.run({"GET", "c"}), bulk("v"));
}

TEST(CommandDump, Functions) {
  Bed bed;
  const std::string lib = "#!lua name=lib\nredis.register_function('f', function() return 1 end)";
  bed.run({"FUNCTION", "LOAD", lib});
  const std::string dumped = payload_of(bed.run({"FUNCTION", "DUMP"}));
  // APPEND, the default, refuses a library already there.
  EXPECT_EQ(bed.run({"FUNCTION", "RESTORE", dumped}), error("ERR Library lib already exists"));
  EXPECT_EQ(bed.run({"FUNCTION", "RESTORE", dumped, "REPLACE"}), ok());
  EXPECT_EQ(bed.run({"FUNCTION", "RESTORE", dumped, "WAT"}), error("ERR Wrong restore policy given, value should be either FLUSH, APPEND or REPLACE."));
  EXPECT_EQ(bed.run({"FUNCTION", "RESTORE", "x"}), error("ERR DUMP payload version or checksum are wrong"));
  bed.run({"FUNCTION", "DELETE", "lib"});
  bed.run({"FUNCTION", "LOAD", "#!lua name=other\nredis.register_function('f', function() return 2 end)"});
  EXPECT_EQ(bed.run({"FUNCTION", "RESTORE", dumped}), error("ERR Function f already exists"));
  EXPECT_EQ(bed.run({"FUNCTION", "RESTORE", dumped, "REPLACE"}), error("ERR Function f already exists"));
  EXPECT_EQ(bed.run({"FCALL", "f", "0"}), integer(2));
  EXPECT_EQ(bed.run({"FUNCTION", "RESTORE", dumped, "FLUSH"}), ok());
  EXPECT_EQ(bed.run({"FCALL", "f", "0"}), integer(1));
}

}  // namespace
}  // namespace mimir::test
