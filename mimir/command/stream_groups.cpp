//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Streams' consumer groups and introspection, as Valkey 8 documents them:
// XGROUP, XREADGROUP (with BLOCK), XACK, XPENDING, XCLAIM, XAUTOCLAIM and
// XINFO. A group keeps its last delivered id, how many entries it has read
// (for its lag), and its pending entries list: each delivered, unacknowledged
// entry with its consumer, its last delivery time and its delivery count.

#include <climits>

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"

namespace mimir::command::streams {

namespace {

using keyspace::Stream;
using keyspace::StreamConsumer;
using keyspace::StreamGroup;
using keyspace::StreamId;
using keyspace::StreamPending;

constexpr const char* invalid_id = "ERR Invalid stream ID specified as stream command argument";

Stream* stream_of(keyspace::Keyspace& keys, std::string_view key) {
  keyspace::Entry* entry = keys.find(key);
  if (!entry) return nullptr;
  if (entry->type() != keyspace::Type::stream) throw CommandError(wrong_type);
  return &std::get<Stream>(entry->value);
}

StreamId id_of(std::string_view text, uint64_t missing_seq = 0) {
  auto id = keyspace::parse_stream_id(text, missing_seq);
  if (!id) throw CommandError(invalid_id);
  return *id;
}

std::string nogroup(const std::string& key, const std::string& group, std::string_view tail = "") {
  return "NOGROUP No such key '" + key + "' or consumer group '" + group + "'" + std::string(tail);
}

StreamGroup& group_of(Call& call, const std::string& key, const std::string& name, std::string_view tail = "") {
  Stream* stream = stream_of(call.keys, key);
  if (!stream) throw CommandError(nogroup(key, name, tail));
  auto found = stream->groups.find(name);
  if (found == stream->groups.end()) throw CommandError(nogroup(key, name, tail));
  return found->second;
}

// Whether a deletion falls in [start, end] of the stream: then a group's
// count of entries read cannot be trusted across it.
bool has_tombstones(const Stream& stream, const StreamId& start, std::optional<StreamId> end) {
  if (stream.entries.empty() || stream.max_deleted == StreamId{}) return false;
  const StreamId last = end ? *end : stream.last_id;
  return !(stream.max_deleted < start) && !(last < stream.max_deleted);
}

// How many entries came before and including `id` since the stream began,
// when that can be known; none when it cannot.
std::optional<uint64_t> distance_from_first(const Stream& stream, const StreamId& id) {
  if (stream.entries_added == 0) return 0;
  if (stream.entries.empty() && !(stream.last_id < id)) return stream.entries_added;
  if (id == stream.last_id) return stream.entries_added;
  if (stream.last_id < id) return std::nullopt;
  const StreamId first = stream.first_id();
  if (stream.max_deleted == StreamId{} || stream.max_deleted < first) {
    if (id < first) return stream.entries_added - stream.entries.size();
    if (id == first) return stream.entries_added - stream.entries.size() + 1;
  }
  return std::nullopt;
}

std::optional<uint64_t> lag_of(const Stream& stream, const StreamGroup& group) {
  if (stream.entries_added == 0) return 0;
  if (group.entries_read && !has_tombstones(stream, group.last_delivered, std::nullopt)) return stream.entries_added - *group.entries_read;
  auto read = distance_from_first(stream, group.last_delivered);
  if (!read) return std::nullopt;
  return stream.entries_added - *read;
}

void reply_entry(resp::Reply& reply, const StreamId& id, const std::vector<std::string>* fields) {
  reply.array(2);
  reply.bulk(id.text());
  if (!fields) {
    reply.null_array();
    return;
  }
  reply.array(fields->size());
  for (const auto& f : *fields) reply.bulk(f);
}

// The consumer named, made if it is not there yet; a new one is announced.
StreamConsumer& consumer_of(Call& call, const std::string& key, StreamGroup& group, const std::string& name, uint64_t now) {
  auto [it, fresh] = group.consumers.try_emplace(name);
  if (fresh) {
    it->second.seen_ms = now;
    it->second.active_ms = 0;
    call.notify('t', "xgroup-createconsumer", key);
  }
  return it->second;
}

// Moves a pending entry to `consumer`, as XCLAIM and XAUTOCLAIM do.
void own(StreamGroup& group, const StreamId& id, const std::string& consumer) {
  auto& pending = group.pending.at(id);
  if (pending.consumer != consumer) {
    auto old = group.consumers.find(pending.consumer);
    if (old != group.consumers.end()) old->second.pending.erase(id);
    pending.consumer = consumer;
  }
  group.consumers[consumer].pending.insert(id);
}

constexpr const char* requires_key =
    "ERR The XGROUP subcommand requires the key to exist. Note that for CREATE you may want to use the MKSTREAM option to create an empty "
    "stream automatically.";

// XGROUP's group: its own words for a missing key and a missing group
// (the differential gate's finding).
StreamGroup& xgroup_group(Call& call) {
  Stream* stream = stream_of(call.keys, call.args[2]);
  if (!stream) throw CommandError(requires_key);
  auto found = stream->groups.find(call.args[3]);
  if (found == stream->groups.end()) throw CommandError("NOGROUP No such consumer group '" + call.args[3] + "' for key name '" + call.args[2] + "'");
  return found->second;
}

}  // namespace

void xgroup(Call& call) {
  if (help::reply_help(call, "xgroup")) return;
  const std::string sub = lower(call.args[1]);
  const auto need = [&](size_t least, size_t most, std::string_view name) {
    if (call.args.size() < least || call.args.size() > most) throw CommandError("ERR wrong number of arguments for 'xgroup|" + std::string(name) + "' command");
  };
  if (sub == "create") {
    need(5, 8, "create");
    const std::string& key = call.args[2];
    const std::string& name = call.args[3];
    bool mkstream = false;
    std::optional<uint64_t> entries_read;
    for (size_t i = 5; i < call.args.size(); ++i) {
      if (iequals(call.args[i], "MKSTREAM")) {
        mkstream = true;
      } else if (iequals(call.args[i], "ENTRIESREAD") && i + 1 < call.args.size()) {
        auto n = parse_integer(call.args[++i]);
        if (!n) throw CommandError(not_integer);
        if (*n < 0 && *n != -1) throw CommandError("ERR value for ENTRIESREAD must be positive or -1");
        if (*n >= 0) entries_read = static_cast<uint64_t>(*n);
      } else {
        throw CommandError(syntax_error);
      }
    }
    Stream* stream = stream_of(call.keys, key);
    StreamId id;
    if (call.args[4] == "$") {
      id = stream ? stream->last_id : StreamId{};
    } else {
      id = id_of(call.args[4]);
    }
    if (!stream) {
      if (!mkstream) {
        throw CommandError(
            "ERR The XGROUP subcommand requires the key to exist. Note that for CREATE you may want to use the MKSTREAM option to create an empty "
            "stream automatically.");
      }
      stream = &std::get<Stream>(call.keys.put(key, keyspace::Entry{Stream{}, std::nullopt}).value);
    }
    if (stream->groups.count(name)) throw CommandError("BUSYGROUP Consumer Group name already exists");
    StreamGroup group;
    group.last_delivered = id;
    group.entries_read = entries_read;
    stream->groups.emplace(name, std::move(group));
    call.notify('t', "xgroup-create", key);
    call.reply.simple("OK");
  } else if (sub == "setid") {
    need(5, SIZE_MAX, "setid");
    // As Valkey's: the option parsed only in its own form, then the key and
    // the group, then any other form refused as an unknown subcommand.
    const std::string unknown = "ERR unknown subcommand or wrong number of arguments for '" + call.args[1] + "'. Try XGROUP HELP.";
    std::optional<uint64_t> entries_read;
    if (call.args.size() == 7) {
      if (!iequals(call.args[5], "ENTRIESREAD")) throw CommandError(unknown);
      auto n = parse_integer(call.args[6]);
      if (!n) throw CommandError(not_integer);
      if (*n < 0 && *n != -1) throw CommandError("ERR value for ENTRIESREAD must be positive or -1");
      if (*n >= 0) entries_read = static_cast<uint64_t>(*n);
    }
    StreamGroup& group = xgroup_group(call);
    if (call.args.size() != 5 && call.args.size() != 7) throw CommandError(unknown);
    Stream* stream = stream_of(call.keys, call.args[2]);
    group.last_delivered = call.args[4] == "$" ? stream->last_id : id_of(call.args[4]);
    group.entries_read = entries_read;
    call.notify('t', "xgroup-setid", call.args[2]);
    call.reply.simple("OK");
  } else if (sub == "destroy") {
    need(4, 4, "destroy");
    Stream* stream = stream_of(call.keys, call.args[2]);
    if (!stream) throw CommandError(requires_key);
    const bool destroyed = stream->groups.erase(call.args[3]) > 0;
    if (destroyed) call.notify('t', "xgroup-destroy", call.args[2]);
    call.reply.integer(destroyed ? 1 : 0);
  } else if (sub == "createconsumer") {
    need(5, 5, "createconsumer");
    StreamGroup& group = xgroup_group(call);
    const bool fresh = !group.consumers.count(call.args[4]);
    consumer_of(call, call.args[2], group, call.args[4], call.keys.now_ms());
    call.reply.integer(fresh ? 1 : 0);
  } else if (sub == "delconsumer") {
    need(5, 5, "delconsumer");
    StreamGroup& group = xgroup_group(call);
    auto found = group.consumers.find(call.args[4]);
    if (found == group.consumers.end()) {
      call.reply.integer(0);
      return;
    }
    const int64_t pending = static_cast<int64_t>(found->second.pending.size());
    for (const auto& id : found->second.pending) group.pending.erase(id);
    group.consumers.erase(found);
    call.notify('t', "xgroup-delconsumer", call.args[2]);
    call.reply.integer(pending);
  } else {
    throw CommandError("ERR unknown subcommand '" + call.args[1] + "'. Try XGROUP HELP.");
  }
}

void xreadgroup(Call& call) {
  if (!iequals(call.args[1], "GROUP")) throw CommandError(syntax_error);
  const std::string& group_name = call.args[2];
  const std::string& consumer_name = call.args[3];
  std::optional<int64_t> count;
  std::optional<int64_t> block_ms;
  bool noack = false;
  size_t i = 4;
  for (; i < call.args.size(); ++i) {
    if (iequals(call.args[i], "COUNT") && i + 1 < call.args.size()) {
      count = integer_argument(call.args[++i]);
    } else if (iequals(call.args[i], "BLOCK") && i + 1 < call.args.size()) {
      auto ms = parse_integer(call.args[++i]);
      if (!ms) throw CommandError("ERR timeout is not an integer or out of range");
      if (*ms < 0) throw CommandError("ERR timeout is negative");
      block_ms = *ms;
    } else if (iequals(call.args[i], "NOACK")) {
      noack = true;
    } else if (iequals(call.args[i], "STREAMS")) {
      break;
    } else {
      throw CommandError(syntax_error);
    }
  }
  if (i >= call.args.size()) throw CommandError(syntax_error);
  const size_t rest = call.args.size() - i - 1;
  if (rest == 0 || rest % 2 != 0) {
    throw CommandError("ERR Unbalanced 'xreadgroup' list of streams: for each stream key an ID or '>' must be specified.");
  }
  const size_t streams = rest / 2;
  const size_t keys_at = i + 1;
  const size_t ids_at = keys_at + streams;
  for (size_t s = 0; s < streams; ++s) {
    const std::string& text = call.args[ids_at + s];
    if (text == "$")
      throw CommandError(
          "ERR The $ ID is meaningless in the context of XREADGROUP: you want to read the history of this consumer by specifying a proper ID, or use the > ID "
          "to get new messages. The $ ID would just return an empty result set.");
    if (text != ">") id_of(text);
  }
  const uint64_t now = call.keys.now_ms();
  const size_t limit = count && *count > 0 ? static_cast<size_t>(*count) : SIZE_MAX;
  struct Read {
    std::string key;
    std::vector<std::pair<StreamId, const std::vector<std::string>*>> entries;
    bool history;
  };
  std::vector<Read> reads;
  bool any = false;
  // Groups first: every named stream must have the group.
  for (size_t s = 0; s < streams; ++s) {
    group_of(call, call.args[keys_at + s], group_name, " in XREADGROUP with GROUP option");
  }
  for (size_t s = 0; s < streams; ++s) {
    const std::string& key = call.args[keys_at + s];
    keyspace::Entry* entry = call.keys.find(key);
    if (!entry || entry->type() != keyspace::Type::stream) continue;
    auto& stream = std::get<Stream>(entry->value);
    auto group_found = stream.groups.find(group_name);
    if (group_found == stream.groups.end()) continue;
    StreamGroup& group = group_found->second;
    StreamConsumer& consumer = consumer_of(call, key, group, consumer_name, now);
    consumer.seen_ms = now;
    Read read{key, {}, call.args[ids_at + s] != ">"};
    if (read.history) {
      const StreamId after = id_of(call.args[ids_at + s]);
      for (auto it = consumer.pending.upper_bound(after); it != consumer.pending.end() && read.entries.size() < limit; ++it) {
        auto found = stream.entries.find(*it);
        auto& pending = group.pending.at(*it);
        if (found != stream.entries.end()) {
          pending.delivered_ms = now;
          ++pending.deliveries;
          read.entries.emplace_back(*it, &found->second);
        } else {
          read.entries.emplace_back(*it, nullptr);
        }
      }
      reads.push_back(std::move(read));
      any = true;
      continue;
    }
    for (auto it = stream.entries.upper_bound(group.last_delivered); it != stream.entries.end() && read.entries.size() < limit; ++it) {
      const StreamId id = it->first;
      // Entries read: counted on while no deletion lies behind, else estimated.
      if (group.entries_read && !has_tombstones(stream, id, std::nullopt)) {
        ++*group.entries_read;
      } else if (stream.entries_added) {
        group.entries_read = distance_from_first(stream, id);
      }
      group.last_delivered = id;
      if (!noack) {
        auto existing = group.pending.find(id);
        if (existing != group.pending.end()) {
          own(group, id, consumer_name);
          existing->second.delivered_ms = now;
          existing->second.deliveries = 1;
        } else {
          group.pending.emplace(id, StreamPending{consumer_name, now, 1});
          consumer.pending.insert(id);
        }
      }
      read.entries.emplace_back(id, &it->second);
    }
    if (!read.entries.empty()) {
      consumer.active_ms = now;
      reads.push_back(std::move(read));
      any = true;
    }
  }
  if (!any) {
    if (block_ms) {
      call.blocked = true;
      call.block_keys.assign(call.args.begin() + static_cast<std::ptrdiff_t>(keys_at), call.args.begin() + static_cast<std::ptrdiff_t>(ids_at));
      call.block_seconds = static_cast<double>(*block_ms) / 1000.0;
      return;
    }
    call.reply.null_array();
    return;
  }
  const bool resp3 = call.reply.protocol() == resp::Protocol::resp3;
  if (resp3)
    call.reply.map(reads.size());
  else
    call.reply.array(reads.size());
  for (const auto& read : reads) {
    if (!resp3) call.reply.array(2);
    call.reply.bulk(read.key);
    call.reply.array(read.entries.size());
    for (const auto& [id, fields] : read.entries) reply_entry(call.reply, id, fields);
  }
}

void xack(Call& call) {
  std::vector<StreamId> ids;
  for (size_t i = 3; i < call.args.size(); ++i) ids.push_back(id_of(call.args[i]));
  Stream* stream = stream_of(call.keys, call.args[1]);
  if (!stream) {
    call.reply.integer(0);
    return;
  }
  auto found = stream->groups.find(call.args[2]);
  if (found == stream->groups.end()) {
    call.reply.integer(0);
    return;
  }
  StreamGroup& group = found->second;
  int64_t acked = 0;
  for (const auto& id : ids) {
    auto pending = group.pending.find(id);
    if (pending == group.pending.end()) continue;
    auto consumer = group.consumers.find(pending->second.consumer);
    if (consumer != group.consumers.end()) consumer->second.pending.erase(id);
    group.pending.erase(pending);
    ++acked;
  }
  call.reply.integer(acked);
}

namespace {
// XPENDING's and XAUTOCLAIM's start and end, with "(" for exclusive.
StreamId bound(std::string_view text, bool start) {
  const bool exclusive = !text.empty() && text[0] == '(';
  if (exclusive) text.remove_prefix(1);
  if (text == "-" || text == "+") {
    if (exclusive) throw CommandError(invalid_id);
    return text == "-" ? StreamId{} : StreamId::max();
  }
  StreamId id = id_of(text, start ? 0 : UINT64_MAX);
  if (!exclusive) return id;
  if (start) {
    if (id == StreamId::max()) throw CommandError("ERR invalid start ID for the interval");
    return id.seq == UINT64_MAX ? StreamId{id.ms + 1, 0} : StreamId{id.ms, id.seq + 1};
  }
  if (id == StreamId{}) throw CommandError("ERR invalid end ID for the interval");
  return id.seq == 0 ? StreamId{id.ms - 1, UINT64_MAX} : StreamId{id.ms, id.seq - 1};
}
}  // namespace

void xpending(Call& call) {
  const std::string& key = call.args[1];
  const std::string& name = call.args[2];
  if (call.args.size() == 3) {
    StreamGroup& group = group_of(call, key, name);
    if (group.pending.empty()) {
      call.reply.array(4);
      call.reply.integer(0);
      call.reply.null();
      call.reply.null();
      call.reply.null_array();
      return;
    }
    call.reply.array(4);
    call.reply.integer(static_cast<int64_t>(group.pending.size()));
    call.reply.bulk(group.pending.begin()->first.text());
    call.reply.bulk(group.pending.rbegin()->first.text());
    size_t with = 0;
    for (const auto& [n, c] : group.consumers) with += !c.pending.empty();
    call.reply.array(with);
    for (const auto& [n, c] : group.consumers) {
      if (c.pending.empty()) continue;
      call.reply.array(2);
      call.reply.bulk(n);
      call.reply.bulk(std::to_string(c.pending.size()));
    }
    return;
  }
  size_t i = 3;
  uint64_t min_idle = 0;
  if (iequals(call.args[i], "IDLE")) {
    if (i + 1 >= call.args.size()) throw CommandError(syntax_error);
    const int64_t idle = integer_argument(call.args[i + 1]);
    min_idle = idle < 0 ? 0 : static_cast<uint64_t>(idle);
    i += 2;
  }
  if (call.args.size() - i < 3 || call.args.size() - i > 4) throw CommandError(syntax_error);
  const StreamId start = bound(call.args[i], true);
  const StreamId end = bound(call.args[i + 1], false);
  const int64_t count = integer_argument(call.args[i + 2]);
  std::optional<std::string> only;
  if (call.args.size() - i == 4) only = call.args[i + 3];
  StreamGroup& group = group_of(call, key, name);
  const uint64_t now = call.keys.now_ms();
  std::vector<std::pair<StreamId, const StreamPending*>> rows;
  if (count > 0 && !(end < start)) {
    for (auto it = group.pending.lower_bound(start); it != group.pending.end() && !(end < it->first) && rows.size() < static_cast<size_t>(count); ++it) {
      if (only && it->second.consumer != *only) continue;
      if (now - std::min(now, it->second.delivered_ms) < min_idle) continue;
      rows.emplace_back(it->first, &it->second);
    }
  }
  call.reply.array(rows.size());
  for (const auto& [id, p] : rows) {
    call.reply.array(4);
    call.reply.bulk(id.text());
    call.reply.bulk(p->consumer);
    call.reply.integer(static_cast<int64_t>(now - std::min(now, p->delivered_ms)));
    call.reply.integer(static_cast<int64_t>(p->deliveries));
  }
}

void xclaim(Call& call) {
  const std::string& key = call.args[1];
  const std::string& name = call.args[2];
  const std::string& consumer_name = call.args[3];
  auto min_idle_parsed = parse_integer(call.args[4]);
  if (!min_idle_parsed) throw CommandError("ERR Invalid min-idle-time argument for XCLAIM");
  const uint64_t min_idle = *min_idle_parsed < 0 ? 0 : static_cast<uint64_t>(*min_idle_parsed);
  std::vector<StreamId> ids;
  size_t i = 5;
  for (; i < call.args.size(); ++i) {
    auto id = keyspace::parse_stream_id(call.args[i], 0);
    if (!id) break;
    ids.push_back(*id);
  }
  const uint64_t now = call.keys.now_ms();
  std::optional<uint64_t> delivered_at;
  std::optional<uint64_t> retries;
  bool force = false, justid = false;
  std::optional<StreamId> lastid;
  for (; i < call.args.size(); ++i) {
    const bool more = i + 1 < call.args.size();
    if (iequals(call.args[i], "IDLE") && more) {
      const int64_t idle = integer_argument(call.args[++i]);
      delivered_at = now - std::min<uint64_t>(now, idle < 0 ? 0 : static_cast<uint64_t>(idle));
    } else if (iequals(call.args[i], "TIME") && more) {
      const int64_t at = integer_argument(call.args[++i]);
      delivered_at = at < 0 ? 0 : static_cast<uint64_t>(at);
    } else if (iequals(call.args[i], "RETRYCOUNT") && more) {
      const int64_t n = integer_argument(call.args[++i]);
      retries = n < 0 ? 0 : static_cast<uint64_t>(n);
    } else if (iequals(call.args[i], "FORCE")) {
      force = true;
    } else if (iequals(call.args[i], "JUSTID")) {
      justid = true;
    } else if (iequals(call.args[i], "LASTID") && more) {
      lastid = id_of(call.args[++i]);
    } else {
      throw CommandError("ERR Unrecognized XCLAIM option '" + call.args[i] + "'");
    }
  }
  StreamGroup& group = group_of(call, key, name);
  Stream* stream = stream_of(call.keys, key);
  if (lastid && group.last_delivered < *lastid) group.last_delivered = *lastid;
  StreamConsumer& consumer = consumer_of(call, key, group, consumer_name, now);
  consumer.seen_ms = now;
  std::vector<std::pair<StreamId, const std::vector<std::string>*>> claimed;
  for (const auto& id : ids) {
    auto entry = stream->entries.find(id);
    auto pending = group.pending.find(id);
    if (pending == group.pending.end()) {
      if (!force || entry == stream->entries.end()) continue;
      pending = group.pending.emplace(id, StreamPending{consumer_name, now, 0}).first;
    }
    if (entry == stream->entries.end()) {
      // A deleted entry leaves the pending list.
      auto owner = group.consumers.find(pending->second.consumer);
      if (owner != group.consumers.end()) owner->second.pending.erase(id);
      group.pending.erase(pending);
      continue;
    }
    if (min_idle > 0 && now - std::min(now, pending->second.delivered_ms) < min_idle) continue;
    own(group, id, consumer_name);
    pending->second.delivered_ms = delivered_at.value_or(now);
    if (retries)
      pending->second.deliveries = *retries;
    else if (!justid)
      ++pending->second.deliveries;
    claimed.emplace_back(id, &entry->second);
  }
  if (!claimed.empty()) consumer.active_ms = now;
  call.reply.array(claimed.size());
  for (const auto& [id, fields] : claimed) {
    if (justid)
      call.reply.bulk(id.text());
    else
      reply_entry(call.reply, id, fields);
  }
}

void xautoclaim(Call& call) {
  const std::string& key = call.args[1];
  const std::string& name = call.args[2];
  const std::string& consumer_name = call.args[3];
  auto min_idle_parsed = parse_integer(call.args[4]);
  if (!min_idle_parsed) throw CommandError("ERR Invalid min-idle-time argument for XAUTOCLAIM");
  const uint64_t min_idle = *min_idle_parsed < 0 ? 0 : static_cast<uint64_t>(*min_idle_parsed);
  const StreamId start = bound(call.args[5], true);
  int64_t count = 100;
  bool justid = false;
  for (size_t i = 6; i < call.args.size(); ++i) {
    if (iequals(call.args[i], "COUNT") && i + 1 < call.args.size()) {
      count = integer_argument(call.args[++i]);
      if (count < 1 || count > LLONG_MAX / 10) throw CommandError("ERR COUNT must be > 0");
    } else if (iequals(call.args[i], "JUSTID")) {
      justid = true;
    } else {
      throw CommandError(syntax_error);
    }
  }
  StreamGroup& group = group_of(call, key, name);
  Stream* stream = stream_of(call.keys, key);
  const uint64_t now = call.keys.now_ms();
  StreamConsumer& consumer = consumer_of(call, key, group, consumer_name, now);
  consumer.seen_ms = now;
  std::vector<std::pair<StreamId, const std::vector<std::string>*>> claimed;
  std::vector<StreamId> deleted;
  // At most ten times COUNT pending entries are looked at, as Valkey bounds it.
  uint64_t attempts = static_cast<uint64_t>(count) * 10;
  auto it = group.pending.lower_bound(start);
  while (it != group.pending.end() && claimed.size() < static_cast<size_t>(count) && attempts > 0) {
    --attempts;
    const StreamId id = it->first;
    auto entry = stream->entries.find(id);
    if (entry == stream->entries.end()) {
      auto owner = group.consumers.find(it->second.consumer);
      if (owner != group.consumers.end()) owner->second.pending.erase(id);
      it = group.pending.erase(it);
      deleted.push_back(id);
      continue;
    }
    if (min_idle > 0 && now - std::min(now, it->second.delivered_ms) < min_idle) {
      ++it;
      continue;
    }
    own(group, id, consumer_name);
    it->second.delivered_ms = now;
    if (!justid) ++it->second.deliveries;
    claimed.emplace_back(id, &entry->second);
    ++it;
  }
  if (!claimed.empty()) consumer.active_ms = now;
  call.reply.array(3);
  call.reply.bulk(it == group.pending.end() ? "0-0" : it->first.text());
  call.reply.array(claimed.size());
  for (const auto& [id, fields] : claimed) {
    if (justid)
      call.reply.bulk(id.text());
    else
      reply_entry(call.reply, id, fields);
  }
  call.reply.array(deleted.size());
  for (const auto& id : deleted) call.reply.bulk(id.text());
}

void xinfo(Call& call) {
  if (help::reply_help(call, "xinfo")) return;
  const std::string sub = lower(call.args[1]);
  const uint64_t now = call.keys.now_ms();
  if (sub == "stream") {
    if (call.args.size() != 3) throw CommandError(syntax_error);
    Stream* stream = stream_of(call.keys, call.args[2]);
    if (!stream) throw CommandError("ERR no such key");
    call.reply.map(10);
    call.reply.bulk("length");
    call.reply.integer(static_cast<int64_t>(stream->entries.size()));
    // The radix tree's figures are Valkey's layout; Mimir counts its nodes.
    std::set<uint64_t> nodes;
    for (const auto& [id, n] : stream->node_of) nodes.insert(n);
    call.reply.bulk("radix-tree-keys");
    call.reply.integer(static_cast<int64_t>(nodes.size()));
    call.reply.bulk("radix-tree-nodes");
    call.reply.integer(static_cast<int64_t>(nodes.size() + 1));
    call.reply.bulk("last-generated-id");
    call.reply.bulk(stream->last_id.text());
    call.reply.bulk("max-deleted-entry-id");
    call.reply.bulk(stream->max_deleted.text());
    call.reply.bulk("entries-added");
    call.reply.integer(static_cast<int64_t>(stream->entries_added));
    call.reply.bulk("recorded-first-entry-id");
    call.reply.bulk(stream->first_id().text());
    call.reply.bulk("groups");
    call.reply.integer(static_cast<int64_t>(stream->groups.size()));
    call.reply.bulk("first-entry");
    if (stream->entries.empty())
      call.reply.null();
    else
      reply_entry(call.reply, stream->entries.begin()->first, &stream->entries.begin()->second);
    call.reply.bulk("last-entry");
    if (stream->entries.empty())
      call.reply.null();
    else
      reply_entry(call.reply, stream->entries.rbegin()->first, &stream->entries.rbegin()->second);
  } else if (sub == "groups") {
    if (call.args.size() != 3) throw CommandError("ERR wrong number of arguments for 'xinfo|groups' command");
    Stream* stream = stream_of(call.keys, call.args[2]);
    if (!stream) throw CommandError("ERR no such key");
    call.reply.array(stream->groups.size());
    for (const auto& [name, group] : stream->groups) {
      call.reply.map(6);
      call.reply.bulk("name");
      call.reply.bulk(name);
      call.reply.bulk("consumers");
      call.reply.integer(static_cast<int64_t>(group.consumers.size()));
      call.reply.bulk("pending");
      call.reply.integer(static_cast<int64_t>(group.pending.size()));
      call.reply.bulk("last-delivered-id");
      call.reply.bulk(group.last_delivered.text());
      call.reply.bulk("entries-read");
      if (group.entries_read)
        call.reply.integer(static_cast<int64_t>(*group.entries_read));
      else
        call.reply.null();
      call.reply.bulk("lag");
      auto lag = lag_of(*stream, group);
      if (lag)
        call.reply.integer(static_cast<int64_t>(*lag));
      else
        call.reply.null();
    }
  } else if (sub == "consumers") {
    if (call.args.size() != 4) throw CommandError("ERR wrong number of arguments for 'xinfo|consumers' command");
    Stream* stream = stream_of(call.keys, call.args[2]);
    if (!stream) throw CommandError("ERR no such key");
    auto found = stream->groups.find(call.args[3]);
    if (found == stream->groups.end()) throw CommandError("NOGROUP No such consumer group '" + call.args[3] + "' for key name '" + call.args[2] + "'");
    call.reply.array(found->second.consumers.size());
    for (const auto& [name, consumer] : found->second.consumers) {
      call.reply.map(4);
      call.reply.bulk("name");
      call.reply.bulk(name);
      call.reply.bulk("pending");
      call.reply.integer(static_cast<int64_t>(consumer.pending.size()));
      call.reply.bulk("idle");
      call.reply.integer(static_cast<int64_t>(now - std::min(now, consumer.seen_ms)));
      call.reply.bulk("inactive");
      call.reply.integer(consumer.active_ms ? static_cast<int64_t>(now - std::min(now, consumer.active_ms)) : -1);
    }
  } else {
    throw CommandError("ERR unknown subcommand '" + call.args[1] + "'. Try XINFO HELP.");
  }
}

}  // namespace mimir::command::streams
