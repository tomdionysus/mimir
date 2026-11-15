//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// LCS, from Valkey 8's documentation and its example: "ohmytext" and
// "mynewtext" share "mytext".

#include <gtest/gtest.h>

#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

TEST(CommandLcs, TheDocumentationsExample) {
  Bed bed;
  bed.run({"MSET", "key1", "ohmytext", "key2", "mynewtext"});
  EXPECT_EQ(bed.run({"LCS", "key1", "key2"}), bulk("mytext"));
  EXPECT_EQ(bed.run({"LCS", "key1", "key2", "LEN"}), integer(6));
  const auto range = [](int64_t a, int64_t b) { return array({integer(a), integer(b)}); };
  EXPECT_EQ(bed.run({"LCS", "key1", "key2", "IDX"}),
            array({bulk("matches"), array({array({range(4, 7), range(5, 8)}), array({range(2, 3), range(0, 1)})}), bulk("len"), integer(6)}));
  EXPECT_EQ(bed.run({"LCS", "key1", "key2", "IDX", "MINMATCHLEN", "4", "WITHMATCHLEN"}),
            array({bulk("matches"), array({array({range(4, 7), range(5, 8), integer(4)})}), bulk("len"), integer(6)}));
  EXPECT_EQ(bed.run({"LCS", "key1", "key2", "IDX", "LEN"}), error("ERR If you want both the length and indexes, please just use IDX."));
  bed.run({"RPUSH", "l", "x"});
  EXPECT_EQ(bed.run({"LCS", "key1", "l"}), error("ERR The specified keys must contain string values"));
  EXPECT_EQ(bed.run({"LCS", "nosuch", "key1"}), bulk(""));
}

}  // namespace
}  // namespace mimir::test
