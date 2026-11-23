//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// HyperLogLog, from Valkey 8's command documentation and the shared format:
// MurmurHash64A's published behaviour, the header and both encodings read
// back to the same registers, estimates within the documented standard error
// (0.81%), and the commands' replies and refusals.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include "mimir/keyspace/hll.h"
#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

namespace hll = keyspace::hll;

// Valkey 8.1.1's answers (probed). A dense value written directly, every
// register at one value: at the largest rank the estimate is infinite, which
// Valkey's rounding gives as the least 64-bit integer. And the sparse bytes
// PFADD stores for elements six and seven bytes long.
TEST(CommandHyperLogLog, CountsAndBytesAsValkeys) {
  Bed bed;
  const std::string header = std::string("HYLL") + std::string(4, '\0') + std::string(7, '\0') + std::string(1, '\x80');
  const auto repeated = [](std::string three, size_t times) {
    std::string out;
    for (size_t i = 0; i < times; ++i) out += three;
    return out;
  };
  const std::string fifty_one("\xf3\x3c\xcf", 3);  // four registers at 51
  bed.run({"SET", "all51", header + repeated(fifty_one, 4096)});
  EXPECT_EQ(bed.run({"PFCOUNT", "all51"}), integer(INT64_MIN));
  bed.run({"SET", "half51", header + repeated(fifty_one, 2048) + std::string(3 * 2048, '\0')});
  EXPECT_EQ(bed.run({"PFCOUNT", "half51"}), integer(13268));
  bed.run({"SET", "all1", header + repeated(std::string("\x41\x10\x04", 3), 4096)});
  EXPECT_EQ(bed.run({"PFCOUNT", "all1"}), integer(23637));
  bed.run({"PFADD", "tails", "abcdef", "abcdefg"});
  const std::string stored = bed.run({"GET", "tails"});
  static constexpr char digits[] = "0123456789abcdef";
  std::string hex;
  for (unsigned char c : stored.substr(stored.find("\r\n") + 2, stored.size() - stored.find("\r\n") - 4)) {
    hex.push_back(digits[c >> 4]);
    hex.push_back(digits[c & 15]);
  }
  EXPECT_EQ(hex, "48594c4c0100000000000000000000805601845f4b844aaf");
  EXPECT_EQ(bed.run({"PFCOUNT", "tails"}), integer(2));
}

TEST(KeyspaceHll, AnEmptyValueIsSparseValidAndCountsZero) {
  const std::string empty = hll::make_empty();
  EXPECT_EQ(empty.substr(0, 4), "HYLL");
  EXPECT_FALSE(hll::is_dense(empty));
  EXPECT_TRUE(hll::looks_valid(empty));
  EXPECT_EQ(hll::cached(empty), 0u);
  auto regs = hll::decode(empty);
  ASSERT_TRUE(regs.has_value());
  for (uint8_t v : *regs) EXPECT_EQ(v, 0);
}

TEST(KeyspaceHll, BothEncodingsReadBackToTheSameRegisters) {
  hll::Registers regs{};
  for (int i = 0; i < 2000; ++i) {
    auto [index, count] = hll::position("element-" + std::to_string(i));
    regs[index] = std::max(regs[index], count);
  }
  const std::string sparse = hll::encode(regs, false);
  const std::string dense = hll::encode(regs, true);
  EXPECT_TRUE(hll::is_dense(dense));
  EXPECT_EQ(dense.size(), hll::dense_size);
  EXPECT_EQ(hll::decode(dense), regs);
  if (!hll::is_dense(sparse)) {
    EXPECT_EQ(hll::decode(sparse), regs);
  }
  // A value too large for the sparse form is dense whatever is asked.
  hll::Registers big{};
  big[5] = 40;
  EXPECT_TRUE(hll::is_dense(hll::encode(big, false)));
}

TEST(KeyspaceHll, CorruptBodiesAreRefused) {
  std::string value = hll::make_empty();
  value.push_back('\x01');  // a run past the last register
  EXPECT_EQ(hll::decode(value), std::nullopt);
  EXPECT_FALSE(hll::looks_valid("HYLX" + std::string(12, '\0')));
  EXPECT_FALSE(hll::looks_valid(std::string("HYLL\x00", 5) + std::string(11, '\0')));  // dense, too short
}

TEST(KeyspaceHll, EstimatesAreWithinTheStandardError) {
  for (int n : {10, 100, 1000, 10000, 100000}) {
    hll::Registers regs{};
    for (int i = 0; i < n; ++i) {
      auto [index, count] = hll::position(std::to_string(i));
      regs[index] = std::max(regs[index], count);
    }
    const double estimate = static_cast<double>(hll::estimate(regs));
    // Three standard errors.
    EXPECT_NEAR(estimate, n, std::max(1.0, n * 0.0081 * 3)) << n;
  }
}

TEST(CommandHyperLogLog, AddAndCount) {
  Bed bed;
  EXPECT_EQ(bed.run({"PFADD", "hll", "a", "b", "c", "d", "e", "f", "g"}), integer(1));
  EXPECT_EQ(bed.run({"PFCOUNT", "hll"}), integer(7));
  EXPECT_EQ(bed.run({"PFADD", "hll", "a", "b"}), integer(0));
  EXPECT_EQ(bed.run({"PFADD", "fresh"}), integer(1));
  EXPECT_EQ(bed.run({"PFADD", "fresh"}), integer(0));
  EXPECT_EQ(bed.run({"PFCOUNT", "fresh"}), integer(0));
  EXPECT_EQ(bed.run({"PFCOUNT", "nosuch"}), integer(0));
  EXPECT_EQ(bed.run({"TYPE", "hll"}), "+string\r\n");
}

TEST(CommandHyperLogLog, CountOverSeveralKeysAndMerge) {
  Bed bed;
  bed.run({"PFADD", "hll1", "foo", "bar", "zap", "a"});
  bed.run({"PFADD", "hll2", "a", "b", "c", "foo"});
  EXPECT_EQ(bed.run({"PFCOUNT", "hll1", "hll2"}), integer(6));
  EXPECT_EQ(bed.run({"PFMERGE", "hll3", "hll1", "hll2"}), ok());
  EXPECT_EQ(bed.run({"PFCOUNT", "hll3"}), integer(6));
  EXPECT_EQ(bed.run({"PFMERGE", "empty"}), ok());
  EXPECT_EQ(bed.run({"PFCOUNT", "empty"}), integer(0));
}

TEST(CommandHyperLogLog, AValueThatIsNoHyperLogLogIsRefused) {
  Bed bed;
  bed.run({"SET", "s", "plain"});
  const std::string refused = error("WRONGTYPE Key is not a valid HyperLogLog string value.");
  EXPECT_EQ(bed.run({"PFADD", "s", "x"}), refused);
  EXPECT_EQ(bed.run({"PFCOUNT", "s"}), refused);
  EXPECT_EQ(bed.run({"PFMERGE", "d", "s"}), refused);
  bed.run({"RPUSH", "l", "x"});
  EXPECT_EQ(bed.run({"PFADD", "l", "x"}), error("WRONGTYPE Operation against a key holding the wrong kind of value"));
}

TEST(CommandHyperLogLog, PfCountCachesItsAnswerInTheValue) {
  Bed bed;
  bed.run({"PFADD", "h", "x", "y"});
  const std::string before = bed.run({"GET", "h"});
  EXPECT_EQ(bed.run({"PFCOUNT", "h"}), integer(2));
  const std::string after = bed.run({"GET", "h"});
  EXPECT_NE(before, after);
  // The cache byte's top bit is clear and the count is in it.
  bed.run({"PFADD", "h", "z"});
  EXPECT_EQ(bed.run({"PFCOUNT", "h"}), integer(3));
}

// PFDEBUG, as Valkey 8.1.1 answers it (probed): DECODE names the sparse
// form's opcodes, ENCODING the form, TODENSE converts and says whether it
// did, GETREG converts and gives every register; the key must exist and be a
// HyperLogLog before the subcommand is looked at.
TEST(CommandHyperLogLog, Pfdebug) {
  Bed bed;
  bed.run({"PFADD", "h", "a", "b", "c"});
  EXPECT_EQ(bed.run({"PFDEBUG", "ENCODING", "h"}), "+sparse\r\n");
  EXPECT_EQ(bed.run({"PFDEBUG", "DECODE", "h"}), "+Z:8436 v:1,1 Z:4274 v:2,1 Z:3068 v:1,1 Z:603\r\n");
  bed.run({"PFADD", "one", "x"});
  EXPECT_EQ(bed.run({"PFDEBUG", "DECODE", "one"}), "+Z:16374 v:2,1 z:9\r\n");
  bed.run({"PFADD", "e"});
  EXPECT_EQ(bed.run({"PFDEBUG", "DECODE", "e"}), "+Z:16384\r\n");
  std::vector<std::string> twenty{"PFADD", "many"};
  for (char c = 'a'; c <= 't'; ++c) twenty.emplace_back(1, c);
  bed.run(twenty);
  EXPECT_EQ(bed.run({"PFDEBUG", "DECODE", "many"}),
            "+Z:1094 v:4,1 Z:551 v:1,1 Z:138 v:3,1 Z:2144 v:2,1 Z:253 v:1,1 Z:197 v:2,1 Z:1169 v:6,1 Z:1127 v:1,1 Z:611 v:1,1 Z:1085 v:2,1 z:57 v:1,1 "
            "Z:574 v:1,1 Z:2002 v:1,1 Z:945 v:1,1 Z:750 v:2,1 Z:509 v:4,1 Z:916 v:2,1 Z:1018 v:1,1 Z:622 v:1,1 Z:603\r\n");

  EXPECT_EQ(bed.run({"PFDEBUG", "TODENSE", "h"}), integer(1));
  EXPECT_EQ(bed.run({"PFDEBUG", "ENCODING", "h"}), "+dense\r\n");
  EXPECT_EQ(bed.run({"PFDEBUG", "TODENSE", "h"}), integer(0));
  EXPECT_EQ(bed.run({"PFDEBUG", "DECODE", "h"}), error("ERR HLL encoding is not sparse"));
  EXPECT_EQ(bed.run({"PFCOUNT", "h"}), integer(3));

  // GETREG makes a sparse value dense, and answers each register.
  bed.run({"PFADD", "s2", "a", "b", "c"});
  std::vector<std::string> registers(hll::registers, integer(0));
  registers[8436] = integer(1);
  registers[12711] = integer(2);
  registers[15780] = integer(1);
  EXPECT_EQ(bed.run({"PFDEBUG", "GETREG", "s2"}), array(registers));
  EXPECT_EQ(bed.run({"PFDEBUG", "ENCODING", "s2"}), "+dense\r\n");
  EXPECT_EQ(bed.run({"PFCOUNT", "s2"}), integer(3));

  bed.run({"SET", "str", "x"});
  EXPECT_EQ(bed.run({"PFDEBUG", "ENCODING", "str"}), error("WRONGTYPE Key is not a valid HyperLogLog string value."));
  EXPECT_EQ(bed.run({"PFDEBUG", "GETREG", "nosuch"}), error("ERR The specified key does not exist"));
  EXPECT_EQ(bed.run({"PFDEBUG", "BOGUS", "nosuch"}), error("ERR The specified key does not exist"));
  EXPECT_EQ(bed.run({"PFDEBUG", "BOGUS", "h"}), error("ERR Unknown PFDEBUG subcommand 'BOGUS'"));
  EXPECT_EQ(bed.run({"PFDEBUG", "GETREG", "h", "extra"}), error("ERR wrong number of arguments for 'pfdebug' command"));
  EXPECT_EQ(bed.run({"PFDEBUG", "GETREG"}), error("ERR wrong number of arguments for 'pfdebug' command"));
  EXPECT_EQ(bed.run({"EVAL", "return redis.call('PFDEBUG','ENCODING',KEYS[1])", "1", "one"}), "+sparse\r\n");
}

}  // namespace
}  // namespace mimir::test
