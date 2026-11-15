//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The server commands clients and tools call, as Valkey 8 documents them:
// CONFIG, OBJECT, ROLE, WAIT, WAITAOF, MEMORY USAGE, SLOWLOG and LASTSAVE.
// What they report of a single-node Mimir is true of it: one database, no
// replicas, nothing evicted yet.

#include <charconv>

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"
#include "mimir/keyspace/glob.h"

namespace mimir::command::server {

namespace {

bool fits_listpack(size_t count, size_t longest) { return count <= 128 && longest <= 64; }

// The encoding Valkey would report for a value of this shape.
std::string_view encoding_of(const keyspace::Entry& entry) {
  switch (entry.type()) {
    case keyspace::Type::string: {
      const auto& s = std::get<std::string>(entry.value);
      if (s.size() <= 20 && parse_integer(s)) return "int";
      return s.size() <= 44 ? "embstr" : "raw";
    }
    case keyspace::Type::list: {
      const auto& l = std::get<keyspace::List>(entry.value);
      size_t bytes = 0;
      for (const auto& e : l) bytes += e.size() + 11;
      return bytes <= 8192 ? "listpack" : "quicklist";
    }
    case keyspace::Type::hash: {
      const auto& h = std::get<keyspace::Hash>(entry.value);
      size_t longest = 0;
      for (const auto& [f, v] : h) longest = std::max({longest, f.size(), v.size()});
      return fits_listpack(h.size(), longest) ? "listpack" : "hashtable";
    }
    case keyspace::Type::set:
      switch (std::get<keyspace::Set>(entry.value).shape()) {
        case keyspace::Set::Shape::integers:
          return "intset";
        case keyspace::Set::Shape::small:
          return "listpack";
        case keyspace::Set::Shape::large:
          return "hashtable";
      }
      break;
    case keyspace::Type::zset: {
      const auto& z = std::get<keyspace::SortedSet>(entry.value);
      size_t longest = 0;
      for (size_t r = 0; r < z.size(); ++r) longest = std::max(longest, z.at(r).member.size());
      return fits_listpack(z.size(), longest) ? "listpack" : "skiplist";
    }
    case keyspace::Type::stream:
      return "stream";
  }
  return "raw";
}

// An estimate of a value's memory, in bytes.
uint64_t usage_of(const std::string& key, const keyspace::Entry& entry) {
  uint64_t bytes = 48 + key.size();
  std::visit(
      [&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::string>) {
          bytes += v.size();
        } else if constexpr (std::is_same_v<T, keyspace::List>) {
          for (const auto& e : v) bytes += 16 + e.size();
        } else if constexpr (std::is_same_v<T, keyspace::Hash>) {
          for (const auto& [f, val] : v) bytes += 48 + f.size() + val.size();
        } else if constexpr (std::is_same_v<T, keyspace::Set>) {
          for (auto m : v.ordered()) bytes += 40 + m.size();
        } else if constexpr (std::is_same_v<T, keyspace::SortedSet>) {
          for (size_t r = 0; r < v.size(); ++r) bytes += 64 + v.at(r).member.size();
        } else {
          for (const auto& [id, fields] : v.entries) {
            bytes += 32;
            for (const auto& f : fields) bytes += f.size();
          }
        }
      },
      entry.value);
  return bytes;
}

}  // namespace

void config(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  auto& settings = call.database.config_locked();
  const std::string sub = lower(call.args[1]);
  if (sub == "get") {
    if (call.args.size() < 3) throw CommandError("ERR wrong number of arguments for 'config|get' command");
    std::vector<std::pair<std::string, std::string>> found;
    for (size_t i = 2; i < call.args.size(); ++i) {
      for (const auto& [name, value] : settings) {
        if (!keyspace::glob_match(call.args[i], name, true)) continue;
        bool seen = false;
        for (const auto& f : found) seen = seen || f.first == name;
        if (!seen) found.emplace_back(name, value);
      }
    }
    call.reply.map(found.size());
    for (const auto& [name, value] : found) {
      call.reply.bulk(name);
      call.reply.bulk(value);
    }
  } else if (sub == "set") {
    if (call.args.size() < 4 || call.args.size() % 2 != 0) throw CommandError("ERR wrong number of arguments for 'config|set' command");
    for (size_t i = 2; i < call.args.size(); i += 2) {
      const std::string name = lower(call.args[i]);
      if (!settings.count(name)) throw CommandError("ERR Unknown option or number of arguments for CONFIG SET - '" + call.args[i] + "'");
      if (name == "databases" || name == "cluster-enabled") {
        throw CommandError("ERR CONFIG SET failed (possibly related to argument '" + call.args[i] + "') - can't set immutable config");
      }
      if (name == "notify-keyspace-events" && !Database::valid_notify_flags(call.args[i + 1])) {
        throw CommandError("ERR CONFIG SET failed (possibly related to argument '" + call.args[i] +
                           "') - Invalid event class character. Use 'Ag$lshzxeKEtmdn'.");
      }
    }
    for (size_t i = 2; i < call.args.size(); i += 2) {
      const std::string name = lower(call.args[i]);
      if (name == "notify-keyspace-events")
        call.database.set_notify_flags_locked(call.args[i + 1]);
      else
        settings[name] = call.args[i + 1];
    }
    call.reply.simple("OK");
  } else if (sub == "resetstat") {
    call.reply.simple("OK");
  } else if (sub == "rewrite") {
    throw CommandError("ERR The server is running without a config file");
  } else {
    throw CommandError("ERR unknown subcommand '" + call.args[1] + "'. Try CONFIG HELP.");
  }
}

void object(Call& call) {
  const std::string sub = lower(call.args[1]);
  if (sub == "help") {
    call.reply.array(1);
    call.reply.simple("OBJECT <subcommand> [<arg> [value] [opt] ...]. Subcommands are: ENCODING, FREQ, IDLETIME, REFCOUNT.");
    return;
  }
  if (sub != "encoding" && sub != "refcount" && sub != "idletime" && sub != "freq") {
    throw CommandError("ERR unknown subcommand '" + call.args[1] + "'. Try OBJECT HELP.");
  }
  if (call.args.size() != 3) throw CommandError("ERR wrong number of arguments for 'object|" + sub + "' command");
  keyspace::Entry* entry = call.keys.find(call.args[2]);
  if (!entry) {
    call.reply.null();
    return;
  }
  if (sub == "encoding") {
    call.reply.bulk(encoding_of(*entry));
  } else if (sub == "refcount") {
    call.reply.integer(1);
  } else if (sub == "idletime") {
    call.reply.integer(0);
  } else {
    throw CommandError(
        "ERR An LFU maxmemory policy is not selected, access frequency not tracked. Please note that when switching between policies at runtime LRU and "
        "LFU data will take some time to adjust.");
  }
}

void role(Call& call) {
  call.reply.array(3);
  call.reply.bulk("master");
  call.reply.integer(0);
  call.reply.array(0);
}

void wait(Call& call) {
  integer_argument(call.args[1]);
  const int64_t timeout = integer_argument(call.args[2]);
  if (timeout < 0) throw CommandError("ERR timeout is negative");
  // No replicas at the single-node stage: none acknowledged.
  call.reply.integer(0);
}

void waitaof(Call& call) {
  const int64_t local = integer_argument(call.args[1]);
  integer_argument(call.args[2]);
  const int64_t timeout = integer_argument(call.args[3]);
  if (timeout < 0) throw CommandError("ERR timeout is negative");
  if (local > 0) throw CommandError("ERR WAITAOF cannot be used when numlocal is set but appendonly is disabled.");
  call.reply.array(2);
  call.reply.integer(0);
  call.reply.integer(0);
}

void memory(Call& call) {
  const std::string sub = lower(call.args[1]);
  if (sub == "usage") {
    if (call.args.size() != 3 && call.args.size() != 5) throw CommandError(syntax_error);
    if (call.args.size() == 5) {
      if (!iequals(call.args[3], "SAMPLES")) throw CommandError(syntax_error);
      integer_argument(call.args[4]);
    }
    keyspace::Entry* entry = call.keys.find(call.args[2]);
    if (!entry)
      call.reply.null();
    else
      call.reply.integer(static_cast<int64_t>(usage_of(call.args[2], *entry)));
  } else if (sub == "doctor") {
    call.reply.verbatim("txt", "Hi Sam, I can't find any memory issue in your instance. I can only account for what occurs on this base.");
  } else {
    throw CommandError("ERR unknown subcommand '" + call.args[1] + "'. Try MEMORY HELP.");
  }
}

void slowlog(Call& call) {
  const std::string sub = lower(call.args[1]);
  if (sub == "get") {
    if (call.args.size() == 3) integer_argument(call.args[2]);
    call.reply.array(0);
  } else if (sub == "len") {
    call.reply.integer(0);
  } else if (sub == "reset") {
    call.reply.simple("OK");
  } else {
    throw CommandError("ERR unknown subcommand '" + call.args[1] + "'. Try SLOWLOG HELP.");
  }
}

void lastsave(Call& call) { call.reply.integer(static_cast<int64_t>(call.database.started_ms() / 1000)); }

}  // namespace mimir::command::server
