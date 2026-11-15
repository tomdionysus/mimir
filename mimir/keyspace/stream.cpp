//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/keyspace/stream.h"

#include <charconv>

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

void Stream::append(StreamId id, std::vector<std::string> fields) {
  entries.emplace(id, std::move(fields));
  node_of.emplace(id, appended / node_entries);
  ++appended;
  last_id = id;
  ++entries_added;
}

bool Stream::remove(StreamId id) {
  if (entries.erase(id) == 0) return false;
  node_of.erase(id);
  if (id > max_deleted) max_deleted = id;
  return true;
}

}  // namespace mimir::keyspace
