//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstddef>
#include <cstdint>

// The thresholds at which Valkey changes a value's encoding, as CONFIG names
// them: agreed by the cluster (the maintainer, 2026-11-23), each defaulting to
// Valkey 8.1.1's (probed). A value converts as Valkey's does when a write
// takes it past one; most never convert back.
namespace mimir::keyspace {

struct Limits {
  size_t set_max_intset_entries = 512;
  size_t set_max_listpack_entries = 128;
  size_t set_max_listpack_value = 64;
  size_t hash_max_listpack_entries = 512;
  size_t hash_max_listpack_value = 64;
  size_t zset_max_listpack_entries = 128;
  size_t zset_max_listpack_value = 64;
  // Positive: entries per list node; -1 to -5: 4, 8, 16, 32 or 64 KB a node.
  int64_t list_max_listpack_size = -2;
  size_t hll_sparse_max_bytes = 3000;
  size_t stream_node_max_entries = 100;
  size_t stream_node_max_bytes = 4096;
};

}  // namespace mimir::keyspace
