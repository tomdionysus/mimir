//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The stream commands that read and write the log itself, as Valkey 8
// documents them: XADD, XLEN, XRANGE, XREVRANGE, XDEL, XTRIM, XREAD (with
// BLOCK) and XSETID. An entry's id is milliseconds and a sequence; "*" asks
// for one after the last, from the node's wall clock.

#include <climits>

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"

namespace mimir::command::streams {

namespace {

using keyspace::Stream;
using keyspace::StreamId;

constexpr const char* invalid_id = "ERR Invalid stream ID specified as stream command argument";

Stream* stream_value(keyspace::Keyspace& keys, std::string_view key) {
  keyspace::Entry* entry = keys.find(key);
  if (!entry) return nullptr;
  if (entry->type() != keyspace::Type::stream) throw CommandError(wrong_type);
  return &std::get<Stream>(entry->value);
}

StreamId id_argument(std::string_view text, uint64_t missing_seq) {
  auto id = keyspace::parse_stream_id(text, missing_seq);
  if (!id) throw CommandError(invalid_id);
  return *id;
}

// A range bound: "-" and "+", "(" for exclusive, a bare ms filled in.
StreamId range_bound(std::string_view text, bool start) {
  const bool exclusive = !text.empty() && text[0] == '(';
  if (exclusive) text.remove_prefix(1);
  if (text == "-" || text == "+") {
    // "(-" and "(+" are no ids at all, to Valkey (the differential gate's finding).
    if (exclusive) throw CommandError(invalid_id);
    return text == "-" ? StreamId{} : StreamId::max();
  }
  StreamId id = id_argument(text, start ? 0 : UINT64_MAX);
  if (!exclusive) return id;
  if (start) {
    if (id == StreamId::max()) throw CommandError("ERR invalid start ID for the interval");
    return id.seq == UINT64_MAX ? StreamId{id.ms + 1, 0} : StreamId{id.ms, id.seq + 1};
  }
  if (id == StreamId{}) throw CommandError("ERR invalid end ID for the interval");
  return id.seq == 0 ? StreamId{id.ms - 1, UINT64_MAX} : StreamId{id.ms, id.seq - 1};
}

void reply_entry(resp::Reply& reply, const StreamId& id, const std::vector<std::string>& fields) {
  reply.array(2);
  reply.bulk(id.text());
  reply.array(fields.size());
  for (const auto& field : fields) reply.bulk(field);
}

// XADD's and XTRIM's trimming: MAXLEN or MINID, exact or approximate.
struct Trim {
  bool by_length = true;
  bool approximate = false;
  uint64_t length = 0;
  StreamId minid;
  std::optional<uint64_t> limit;
};

// Parses a trim from `i`, which names MAXLEN or MINID; moves `i` past it.
Trim parse_trim(Call& call, size_t& i) {
  Trim trim;
  trim.by_length = iequals(call.args[i], "MAXLEN");
  ++i;
  if (i < call.args.size() && (call.args[i] == "~" || call.args[i] == "=")) {
    trim.approximate = call.args[i] == "~";
    ++i;
  }
  if (i >= call.args.size()) throw CommandError(syntax_error);
  if (trim.by_length) {
    auto n = parse_integer(call.args[i]);
    if (!n) throw CommandError(not_integer);
    if (*n < 0) throw CommandError("ERR The MAXLEN argument must be >= 0.");
    trim.length = static_cast<uint64_t>(*n);
  } else {
    trim.minid = id_argument(call.args[i], 0);
  }
  ++i;
  if (i + 1 < call.args.size() && iequals(call.args[i], "LIMIT")) {
    auto n = parse_integer(call.args[i + 1]);
    if (!n) throw CommandError(not_integer);
    if (*n < 0) throw CommandError("ERR The LIMIT argument must be >= 0.");
    if (!trim.approximate) throw CommandError("ERR syntax error, LIMIT cannot be used without the special ~ option");
    trim.limit = static_cast<uint64_t>(*n);
    i += 2;
  }
  return trim;
}

// Removes from the oldest as the trim says; how many went. Approximate trims
// remove whole nodes only, as Valkey's do, and at most `limit` entries.
uint64_t apply_trim(Stream& stream, const Trim& trim) {
  const auto over = [&](const StreamId& id) { return trim.by_length ? stream.entries.size() > trim.length : id < trim.minid; };
  uint64_t removed = 0;
  if (!trim.approximate) {
    while (!stream.entries.empty() && over(stream.entries.begin()->first)) {
      stream.remove(stream.entries.begin()->first);
      ++removed;
    }
    return removed;
  }
  // Valkey's default for an approximate trim: a hundred nodes' worth.
  const uint64_t limit = trim.limit ? (*trim.limit == 0 ? UINT64_MAX : *trim.limit) : Stream::node_entries * 100;
  while (!stream.entries.empty()) {
    // The oldest node's live entries.
    const uint64_t node = stream.node_of.at(stream.entries.begin()->first);
    std::vector<StreamId> in_node;
    for (const auto& [id, n] : stream.node_of) {
      if (n == node) in_node.push_back(id);
    }
    // The whole node goes only if what is left still satisfies the trim; the
    // newest node too, as Valkey's goes (the differential gate's finding).
    if (trim.by_length && stream.entries.size() - in_node.size() < trim.length) break;
    if (!trim.by_length && !(in_node.back() < trim.minid)) break;
    if (removed + in_node.size() > limit) break;
    for (const auto& id : in_node) stream.remove(id);
    removed += in_node.size();
  }
  return removed;
}

}  // namespace

void xadd(Call& call) {
  const std::string& key = call.args[1];
  bool nomkstream = false;
  std::optional<Trim> trim;
  size_t i = 2;
  for (; i < call.args.size();) {
    if (iequals(call.args[i], "NOMKSTREAM")) {
      nomkstream = true;
      ++i;
    } else if (iequals(call.args[i], "MAXLEN") || iequals(call.args[i], "MINID")) {
      trim = parse_trim(call, i);
    } else {
      break;
    }
  }
  if (i >= call.args.size()) throw CommandError("ERR wrong number of arguments for 'xadd' command");
  const std::string& id_text = call.args[i++];
  if (call.args.size() - i == 0 || (call.args.size() - i) % 2 != 0) throw CommandError("ERR wrong number of arguments for 'xadd' command");

  // The id asked for: "*", "ms-*", or "ms-seq".
  std::optional<StreamId> explicit_id;
  std::optional<uint64_t> fixed_ms;
  if (id_text != "*") {
    if (id_text.size() > 2 && id_text.substr(id_text.size() - 2) == "-*") {
      auto ms = parse_integer(std::string_view(id_text).substr(0, id_text.size() - 2));
      if (!ms || *ms < 0) throw CommandError(invalid_id);
      fixed_ms = static_cast<uint64_t>(*ms);
    } else {
      explicit_id = id_argument(id_text, 0);
      if (*explicit_id == StreamId{}) throw CommandError("ERR The ID specified in XADD must be greater than 0-0");
    }
  }

  Stream* stream = stream_value(call.keys, key);
  if (!stream) {
    if (nomkstream) {
      call.reply.null();
      return;
    }
  }
  const StreamId last = stream ? stream->last_id : StreamId{};
  StreamId id;
  if (explicit_id) {
    if (!(*explicit_id > last)) throw CommandError("ERR The ID specified in XADD is equal or smaller than the target stream top item");
    id = *explicit_id;
  } else if (fixed_ms) {
    if (*fixed_ms < last.ms) throw CommandError("ERR The ID specified in XADD is equal or smaller than the target stream top item");
    if (*fixed_ms == last.ms) {
      if (last.seq == UINT64_MAX) throw CommandError("ERR The ID specified in XADD is equal or smaller than the target stream top item");
      id = StreamId{last.ms, last.seq + 1};
    } else {
      id = StreamId{*fixed_ms, 0};
    }
    if (id == StreamId{}) id.seq = 1;
  } else {
    const uint64_t now = call.keys.now_ms();
    if (now > last.ms) {
      id = StreamId{now, 0};
    } else {
      if (last.seq == UINT64_MAX) {
        if (last.ms == UINT64_MAX) throw CommandError("ERR The stream has exhausted the last possible ID, unable to add more items");
        id = StreamId{last.ms + 1, 0};
      } else {
        id = StreamId{last.ms, last.seq + 1};
      }
    }
  }
  if (!stream) stream = &std::get<Stream>(call.keys.put(key, keyspace::Entry{Stream{}, std::nullopt}).value);
  stream->append(id, std::vector<std::string>(call.args.begin() + static_cast<std::ptrdiff_t>(i), call.args.end()));
  if (trim) apply_trim(*stream, *trim);
  call.reply.bulk(id.text());
}

void xlen(Call& call) {
  Stream* stream = stream_value(call.keys, call.args[1]);
  call.reply.integer(stream ? static_cast<int64_t>(stream->entries.size()) : 0);
}

namespace {
void range_generic(Call& call, bool reverse) {
  const StreamId start = range_bound(call.args[reverse ? 3 : 2], true);
  const StreamId end = range_bound(call.args[reverse ? 2 : 3], false);
  std::optional<int64_t> count;
  if (call.args.size() == 6 && iequals(call.args[4], "COUNT")) {
    count = integer_argument(call.args[5]);
  } else if (call.args.size() != 4) {
    throw CommandError(syntax_error);
  }
  Stream* stream = stream_value(call.keys, call.args[1]);
  // A count of zero or less asks for nothing: a null array, as Valkey answers.
  if (count && *count <= 0) {
    call.reply.null_array();
    return;
  }
  std::vector<std::pair<StreamId, const std::vector<std::string>*>> found;
  if (stream && !(end < start)) {
    const size_t limit = count && *count > 0 ? static_cast<size_t>(*count) : SIZE_MAX;
    if (!reverse) {
      for (auto it = stream->entries.lower_bound(start); it != stream->entries.end() && !(end < it->first) && found.size() < limit; ++it)
        found.emplace_back(it->first, &it->second);
    } else {
      auto it = stream->entries.upper_bound(end);
      while (it != stream->entries.begin() && found.size() < limit) {
        --it;
        if (it->first < start) break;
        found.emplace_back(it->first, &it->second);
      }
    }
  }
  call.reply.array(found.size());
  for (const auto& [id, fields] : found) reply_entry(call.reply, id, *fields);
}
}  // namespace

void xrange(Call& call) { range_generic(call, false); }
void xrevrange(Call& call) { range_generic(call, true); }

void xdel(Call& call) {
  std::vector<StreamId> ids;
  for (size_t i = 2; i < call.args.size(); ++i) ids.push_back(id_argument(call.args[i], 0));
  Stream* stream = stream_value(call.keys, call.args[1]);
  int64_t removed = 0;
  if (stream)
    for (const auto& id : ids) removed += stream->remove(id) ? 1 : 0;
  call.reply.integer(removed);
}

void xtrim(Call& call) {
  size_t i = 2;
  if (!iequals(call.args[i], "MAXLEN") && !iequals(call.args[i], "MINID")) throw CommandError(syntax_error);
  Trim trim = parse_trim(call, i);
  if (i != call.args.size()) throw CommandError(syntax_error);
  Stream* stream = stream_value(call.keys, call.args[1]);
  call.reply.integer(stream ? static_cast<int64_t>(apply_trim(*stream, trim)) : 0);
}

void xsetid(Call& call) {
  const StreamId id = id_argument(call.args[2], 0);
  std::optional<uint64_t> added;
  std::optional<StreamId> max_deleted;
  for (size_t i = 3; i < call.args.size(); ++i) {
    if (iequals(call.args[i], "ENTRIESADDED") && i + 1 < call.args.size()) {
      auto n = parse_integer(call.args[++i]);
      if (!n) throw CommandError(not_integer);
      if (*n < 0) throw CommandError("ERR entries_added must be positive");
      added = static_cast<uint64_t>(*n);
    } else if (iequals(call.args[i], "MAXDELETEDID") && i + 1 < call.args.size()) {
      max_deleted = id_argument(call.args[++i], 0);
    } else {
      throw CommandError(syntax_error);
    }
  }
  if (max_deleted && id < *max_deleted) throw CommandError("ERR The ID specified in XSETID is smaller than the provided max_deleted_entry_id");
  Stream* stream = stream_value(call.keys, call.args[1]);
  if (!stream) throw CommandError("ERR no such key");
  if (added && *added < stream->entries.size()) {
    throw CommandError("ERR The entries_added specified in XSETID is smaller than the target stream length");
  }
  if (!stream->entries.empty() && id < stream->entries.rbegin()->first) {
    throw CommandError("ERR The ID specified in XSETID is smaller than the target stream top item");
  }
  stream->last_id = id;
  if (added) stream->entries_added = *added;
  if (max_deleted) stream->max_deleted = *max_deleted;
  call.reply.simple("OK");
}

void xread(Call& call) {
  std::optional<int64_t> count;
  std::optional<int64_t> block_ms;
  size_t i = 1;
  for (; i < call.args.size(); ++i) {
    if (iequals(call.args[i], "COUNT") && i + 1 < call.args.size()) {
      count = integer_argument(call.args[++i]);
    } else if (iequals(call.args[i], "BLOCK") && i + 1 < call.args.size()) {
      auto ms = parse_integer(call.args[++i]);
      if (!ms) throw CommandError("ERR timeout is not an integer or out of range");
      if (*ms < 0) throw CommandError("ERR timeout is negative");
      block_ms = *ms;
    } else if (iequals(call.args[i], "STREAMS")) {
      break;
    } else {
      throw CommandError(syntax_error);
    }
  }
  if (i >= call.args.size()) throw CommandError(syntax_error);
  const size_t rest = call.args.size() - i - 1;
  if (rest == 0 || rest % 2 != 0) {
    throw CommandError("ERR Unbalanced 'xread' list of streams: for each stream key an ID or '$' must be specified.");
  }
  const size_t streams = rest / 2;
  const size_t keys_at = i + 1;
  const size_t ids_at = keys_at + streams;
  // Each key's id after which to read; "$" is the stream's last id now.
  std::vector<StreamId> after(streams);
  std::vector<std::string> resolved(call.args.begin(), call.args.end());
  for (size_t s = 0; s < streams; ++s) {
    const std::string& text = call.args[ids_at + s];
    if (text == ">") throw CommandError("ERR The > ID can be specified only when calling XREADGROUP using the GROUP <group> <consumer> option.");
    if (text == "$") {
      Stream* stream = stream_value(call.keys, call.args[keys_at + s]);
      after[s] = stream ? stream->last_id : StreamId{};
      resolved[ids_at + s] = after[s].text();
    } else {
      after[s] = id_argument(text, 0);
    }
  }
  struct Found {
    std::string key;
    std::vector<std::pair<StreamId, const std::vector<std::string>*>> entries;
  };
  std::vector<Found> found;
  const size_t limit = count && *count > 0 ? static_cast<size_t>(*count) : SIZE_MAX;
  for (size_t s = 0; s < streams; ++s) {
    const std::string& key = call.args[keys_at + s];
    if (call.ready_key && key != *call.ready_key) continue;
    keyspace::Entry* entry = call.keys.find(key);
    if (!entry) continue;
    if (entry->type() != keyspace::Type::stream) {
      if (call.ready_key) continue;
      throw CommandError(wrong_type);
    }
    auto& stream = std::get<Stream>(entry->value);
    Found f{key, {}};
    for (auto it = stream.entries.upper_bound(after[s]); it != stream.entries.end() && f.entries.size() < limit; ++it)
      f.entries.emplace_back(it->first, &it->second);
    if (!f.entries.empty()) found.push_back(std::move(f));
  }
  if (found.empty()) {
    if (block_ms) {
      call.blocked = true;
      call.block_keys.assign(call.args.begin() + static_cast<std::ptrdiff_t>(keys_at), call.args.begin() + static_cast<std::ptrdiff_t>(ids_at));
      call.block_seconds = static_cast<double>(*block_ms) / 1000.0;
      call.block_args = std::move(resolved);
      return;
    }
    call.reply.null_array();
    return;
  }
  call.reply.map(found.size());
  if (call.reply.protocol() == resp::Protocol::resp2) {
    // RESP2 gives each stream as a [key, entries] pair, not a flat map.
    resp::Reply flat(resp::Protocol::resp2);
    flat.array(found.size());
    for (const auto& f : found) {
      flat.array(2);
      flat.bulk(f.key);
      flat.array(f.entries.size());
      for (const auto& [id, fields] : f.entries) reply_entry(flat, id, *fields);
    }
    call.reply = std::move(flat);
    return;
  }
  for (const auto& f : found) {
    call.reply.bulk(f.key);
    call.reply.array(f.entries.size());
    for (const auto& [id, fields] : f.entries) reply_entry(call.reply, id, *fields);
  }
}

}  // namespace mimir::command::streams
