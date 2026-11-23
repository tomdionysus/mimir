//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/keyspace/stream.h"

#include <charconv>
#include <iterator>

#include "mimir/keyspace/rdb.h"

namespace mimir::keyspace {

namespace {
std::optional<uint64_t> number(std::string_view text) {
  if (text.empty()) return std::nullopt;
  uint64_t value = 0;
  auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size()) return std::nullopt;
  return value;
}
}  // namespace

std::string StreamId::text() const { return std::to_string(ms) + "-" + std::to_string(seq); }

std::optional<StreamId> parse_stream_id(std::string_view text, uint64_t missing_seq) {
  const size_t dash = text.find('-');
  if (dash == std::string_view::npos) {
    auto ms = number(text);
    if (!ms) return std::nullopt;
    return StreamId{*ms, missing_seq};
  }
  auto ms = number(text.substr(0, dash));
  auto seq = number(text.substr(dash + 1));
  if (!ms || !seq) return std::nullopt;
  return StreamId{*ms, *seq};
}

void Stream::append(StreamId id, std::vector<std::string> fields, const Limits& limits) {
  using rdb::listpack_element_size;
  using rdb::listpack_integer_size;
  // Valkey's test, before the entry goes in: the node's bytes and the entry's
  // fields and values, as they are, against stream-node-max-bytes; or a node
  // full by stream-node-max-entries.
  uint64_t raw = 0;
  for (const auto& f : fields) raw += f.size();
  auto last = nodes.empty() ? nodes.end() : std::prev(nodes.end());
  // A limit of zero is none, as Valkey's.
  const bool too_big = limits.stream_node_max_bytes > 0 && last != nodes.end() && last->second.bytes + raw >= limits.stream_node_max_bytes;
  const bool too_many = limits.stream_node_max_entries > 0 && last != nodes.end() && last->second.entries >= limits.stream_node_max_entries;
  if (last == nodes.end() || too_big || too_many) {
    StreamNode made;
    made.master = id;
    for (size_t f = 0; f + 1 < fields.size(); f += 2) made.fields.push_back(fields[f]);
    // The header and end, then the master entry: the live and deleted counts
    // (one and none; up to 127 they keep their size), the field count and
    // names, and its terminator.
    made.bytes = 7 + listpack_integer_size(1) + listpack_integer_size(0) + listpack_integer_size(static_cast<int64_t>(made.fields.size()));
    for (const auto& name : made.fields) made.bytes += listpack_element_size(name);
    made.bytes += listpack_integer_size(0);
    last = nodes.emplace(next_node++, std::move(made)).first;
  }
  StreamNode& node = last->second;
  const size_t count = fields.size() / 2;
  bool same = count == node.fields.size();
  for (size_t f = 0; same && f < count; ++f) same = fields[2 * f] == node.fields[f];
  // The entry: its flags, its id as differences from the master's, its values
  // (with their fields and count when they are not the master's), and the
  // count of its elements.
  uint64_t bytes = listpack_integer_size(same ? 2 : 0) + listpack_integer_size(static_cast<int64_t>(id.ms - node.master.ms)) +
                   listpack_integer_size(static_cast<int64_t>(id.seq - node.master.seq));
  if (same) {
    for (size_t f = 0; f < count; ++f) bytes += listpack_element_size(fields[2 * f + 1]);
    bytes += listpack_integer_size(static_cast<int64_t>(count + 3));
  } else {
    bytes += listpack_integer_size(static_cast<int64_t>(count));
    for (const auto& f : fields) bytes += listpack_element_size(f);
    bytes += listpack_integer_size(static_cast<int64_t>(2 * count + 4));
  }
  node.bytes += bytes;
  ++node.entries;
  ++node.live;
  node_of.emplace(id, last->first);
  entries.emplace(id, std::move(fields));
  last_id = id;
  ++entries_added;
}

bool Stream::remove(StreamId id) {
  auto entry = entries.find(id);
  if (entry == entries.end()) return false;
  if (auto at = node_of.find(id); at != node_of.end()) {
    const uint64_t number = at->second;
    node_of.erase(at);
    auto node = nodes.find(number);
    if (node != nodes.end() && --node->second.live == 0) {
      // A node whose last live entry goes is removed, as Valkey removes it,
      // its deleted entries with it.
      nodes.erase(node);
      for (auto it = tombstones.begin(); it != tombstones.end();) it = it->second.node == number ? tombstones.erase(it) : std::next(it);
    } else {
      tombstones.emplace(id, Tombstone{number, std::move(entry->second)});
    }
  }
  entries.erase(entry);
  if (id > max_deleted) max_deleted = id;
  return true;
}

}  // namespace mimir::keyspace
