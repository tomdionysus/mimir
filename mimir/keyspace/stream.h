//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "mimir/keyspace/limits.h"

// A stream: an append-only log of entries, each a list of field-value pairs
// under an id of milliseconds and a sequence number, kept in id order, with
// the bookkeeping Valkey keeps beside it (the last id given, the largest id
// deleted, how many entries were ever added) and its consumer groups.
namespace mimir::keyspace {

struct StreamId {
  uint64_t ms = 0;
  uint64_t seq = 0;
  auto operator<=>(const StreamId&) const = default;
  std::string text() const;
  static constexpr StreamId max() { return StreamId{UINT64_MAX, UINT64_MAX}; }
};

// "ms-seq", or "ms" with `missing_seq` for the sequence; none when not an id.
std::optional<StreamId> parse_stream_id(std::string_view text, uint64_t missing_seq);

struct StreamPending {
  std::string consumer;
  uint64_t delivered_ms = 0;
  uint64_t deliveries = 0;
};

struct StreamConsumer {
  uint64_t seen_ms = 0;
  uint64_t active_ms = 0;
  std::set<StreamId> pending;
};

struct StreamGroup {
  StreamId last_delivered;
  // Entries read by the group, when known: Valkey's lag bookkeeping.
  std::optional<uint64_t> entries_read;
  std::map<StreamId, StreamPending> pending;
  std::map<std::string, StreamConsumer> consumers;
};

// One node of Valkey's radix tree: entries appended together into one
// listpack, under the first's id. Valkey closes a node at
// stream-node-max-entries entries or when an entry would bring its listpack
// to stream-node-max-bytes (the agreed Limits), so its size is kept as
// Valkey's listpack would have it. A deleted entry keeps its bytes and its
// count; the node goes with its last live entry.
struct StreamNode {
  StreamId master;
  // The master entry's field names: the node's first entry's.
  std::vector<std::string> fields;
  uint64_t bytes = 0;
  // Appended into it, deleted ones included; and those still live.
  uint64_t entries = 0;
  uint64_t live = 0;
};

struct Stream {
  std::map<StreamId, std::vector<std::string>> entries;
  StreamId last_id;
  StreamId max_deleted;
  uint64_t entries_added = 0;
  // The node each live entry is in, by the node's number (in append order).
  std::map<StreamId, uint64_t> node_of;
  std::map<uint64_t, StreamNode> nodes;
  uint64_t next_node = 0;
  // Deleted entries, kept in their node, as Valkey keeps them (flagged in its
  // listpack), until the node goes: DUMP writes them, and they take their
  // node's bytes.
  struct Tombstone {
    uint64_t node = 0;
    std::vector<std::string> fields;
  };
  std::map<StreamId, Tombstone> tombstones;
  std::map<std::string, StreamGroup> groups;

  StreamId first_id() const { return entries.empty() ? StreamId{} : entries.begin()->first; }
  // Appends an entry, in a new node when the last is full by the agreed
  // stream-node-max-entries or stream-node-max-bytes.
  void append(StreamId id, std::vector<std::string> fields, const Limits& limits = Limits{});
  bool remove(StreamId id);
};

}  // namespace mimir::keyspace
