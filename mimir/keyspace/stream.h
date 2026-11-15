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

struct Stream {
  std::map<StreamId, std::vector<std::string>> entries;
  StreamId last_id;
  StreamId max_deleted;
  uint64_t entries_added = 0;
  // For approximate trimming: the node, of up to node_entries, each entry was
  // appended into, as Valkey's radix tree groups them by insertion.
  std::map<StreamId, uint64_t> node_of;
  uint64_t appended = 0;
  std::map<std::string, StreamGroup> groups;

  static constexpr uint64_t node_entries = 100;

  StreamId first_id() const { return entries.empty() ? StreamId{} : entries.begin()->first; }
  void append(StreamId id, std::vector<std::string> fields);
  bool remove(StreamId id);
};

}  // namespace mimir::keyspace
