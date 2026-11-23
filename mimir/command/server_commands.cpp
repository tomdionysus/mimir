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
#include "mimir/version.h"

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
  if (help::reply_help(call, "config")) return;
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
      if (name == "latency-tracking" && !iequals(call.args[i + 1], "yes") && !iequals(call.args[i + 1], "no")) {
        throw CommandError("ERR CONFIG SET failed (possibly related to argument '" + call.args[i] + "') - argument must be 'yes' or 'no'");
      }
    }
    for (size_t i = 2; i < call.args.size(); i += 2) {
      const std::string name = lower(call.args[i]);
      if (name == "notify-keyspace-events")
        call.database.set_notify_flags_locked(call.args[i + 1]);
      else if (name == "latency-tracking")
        settings[name] = lower(call.args[i + 1]);
      else
        settings[name] = call.args[i + 1];
    }
    call.reply.simple("OK");
  } else if (sub == "resetstat") {
    call.database.reset_latency_locked();
    call.reply.simple("OK");
  } else if (sub == "rewrite") {
    throw CommandError("ERR The server is running without a config file");
  } else {
    throw CommandError("ERR unknown subcommand '" + call.args[1] + "'. Try CONFIG HELP.");
  }
}

void object(Call& call) {
  if (help::reply_help(call, "object")) return;
  const std::string sub = lower(call.args[1]);
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
  if (help::reply_help(call, "memory")) return;
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
  } else if (sub == "purge") {
    if (call.args.size() != 2) throw CommandError("ERR wrong number of arguments for 'memory|purge' command");
    call.reply.simple("OK");
  } else if (sub == "malloc-stats") {
    if (call.args.size() != 2) throw CommandError("ERR wrong number of arguments for 'memory|malloc-stats' command");
    call.reply.verbatim("txt", "Stats not supported for the current allocator");
  } else {
    throw CommandError("ERR unknown subcommand '" + call.args[1] + "'. Try MEMORY HELP.");
  }
}

void slowlog(Call& call) {
  if (help::reply_help(call, "slowlog")) return;
  const std::string sub = lower(call.args[1]);
  if (sub == "get") {
    if (call.args.size() > 3) throw CommandError("ERR unknown subcommand or wrong number of arguments for '" + call.args[1] + "'. Try SLOWLOG HELP.");
    if (call.args.size() == 3) {
      auto count = parse_integer(call.args[2]);
      if (!count || *count < -1) throw CommandError("ERR count should be greater than or equal to -1");
    }
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

namespace {

constexpr const char* cluster_disabled = "ERR This instance has cluster support disabled";

// CLUSTER's subcommands in Valkey 8.1.1: each answered as a standalone Valkey
// answers it until Mimir's cluster stage gives them their meaning.
constexpr std::string_view cluster_subcommands[] = {"addslots",
                                                    "addslotsrange",
                                                    "bumpepoch",
                                                    "count-failure-reports",
                                                    "countkeysinslot",
                                                    "delslots",
                                                    "delslotsrange",
                                                    "failover",
                                                    "flushslots",
                                                    "forget",
                                                    "getkeysinslot",
                                                    "help",
                                                    "info",
                                                    "keyslot",
                                                    "links",
                                                    "meet",
                                                    "myid",
                                                    "myshardid",
                                                    "nodes",
                                                    "replicas",
                                                    "replicate",
                                                    "reset",
                                                    "saveconfig",
                                                    "set-config-epoch",
                                                    "setslot",
                                                    "shards",
                                                    "slaves",
                                                    "slots",
                                                    "slot-stats"};

std::string_view commandlog_type(const std::string& type) {
  for (std::string_view known : {"slow", "large-request", "large-reply"})
    if (iequals(type, known)) return known;
  throw CommandError("ERR type should be one of the following: slow, large-request, large-reply");
}

}  // namespace

void cluster(Call& call) {
  for (auto sub : cluster_subcommands)
    if (iequals(call.args[1], sub)) throw CommandError(cluster_disabled);
  throw CommandError("ERR unknown subcommand '" + call.args[1] + "'. Try CLUSTER HELP.");
}

void asking(Call&) { throw CommandError(cluster_disabled); }
void readonly(Call& call) { call.reply.simple("OK"); }
void readwrite(Call& call) { call.reply.simple("OK"); }

void debug(Call&) {
  throw CommandError(
      "ERR DEBUG command not allowed. If the enable-debug-command option is set to \"local\", you can run it from a local connection, otherwise you need to "
      "set this option in the configuration file, and then restart the server.");
}

void module(Call& call) {
  if (help::reply_help(call, "module")) return;
  const std::string sub = lower(call.args[1]);
  if (sub == "list") {
    if (call.args.size() != 2) throw CommandError("ERR wrong number of arguments for 'module|list' command");
    call.reply.array(0);
  } else if (sub == "load" || sub == "loadex" || sub == "unload") {
    if (call.args.size() < 3 || (sub == "unload" && call.args.size() != 3))
      throw CommandError("ERR wrong number of arguments for 'module|" + sub + "' command");
    // Mimir loads no modules; Valkey refuses them unless configured otherwise.
    throw CommandError(
        "ERR MODULE command not allowed. If the enable-module-command option is set to \"local\", you can run it from a local connection, otherwise you "
        "need to set this option in the configuration file, and then restart the server.");
  } else {
    throw CommandError("ERR unknown subcommand '" + call.args[1] + "'. Try MODULE HELP.");
  }
}

void pfselftest(Call& call) { call.reply.simple("OK"); }

// Latency monitoring of events is off, as Valkey's is by default: nothing is
// sampled. Each command's latency is tracked (HISTOGRAM). Called under the
// database's lock, as every handler is.
void latency(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  if (help::reply_help(call, "latency")) return;
  const std::string sub = lower(call.args[1]);
  const size_t argc = call.args.size();
  if (sub == "latest") {
    if (argc != 2) throw CommandError("ERR wrong number of arguments for 'latency|latest' command");
    call.reply.array(0);
  } else if (sub == "history") {
    if (argc != 3) throw CommandError("ERR wrong number of arguments for 'latency|history' command");
    call.reply.array(0);
  } else if (sub == "reset") {
    call.reply.integer(0);
  } else if (sub == "graph") {
    if (argc != 3) throw CommandError("ERR wrong number of arguments for 'latency|graph' command");
    throw CommandError("ERR No samples available for event '" + call.args[2] + "'");
  } else if (sub == "doctor") {
    if (argc != 2) throw CommandError("ERR wrong number of arguments for 'latency|doctor' command");
    call.reply.verbatim("txt",
                        "Latency monitoring is disabled in this Mimir node. You may use \"CONFIG SET latency-monitor-threshold <milliseconds>.\" in order "
                        "to enable it, once Mimir samples latency.");
  } else if (sub == "histogram") {
    // Every command run since the last CONFIG RESETSTAT, or those named: a
    // command with subcommands names each of them that ran; one not known,
    // or never run, nothing.
    const auto& all = call.database.latency_locked();
    std::vector<const std::pair<const std::string, Database::LatencyHistogram>*> chosen;
    if (argc == 2) {
      for (const auto& entry : all) chosen.push_back(&entry);
    } else {
      for (size_t i = 2; i < argc; ++i) {
        const std::string name = lower(call.args[i]);
        if (const auto exact = all.find(name); exact != all.end()) chosen.push_back(&*exact);
        const std::string container = name + "|";
        for (auto it = all.lower_bound(container); it != all.end() && it->first.compare(0, container.size(), container) == 0; ++it) chosen.push_back(&*it);
      }
    }
    call.reply.map(chosen.size());
    for (const auto* entry : chosen) {
      const auto& histogram = entry->second;
      call.reply.bulk(entry->first);
      call.reply.map(2);
      call.reply.bulk("calls");
      call.reply.integer(static_cast<int64_t>(histogram.calls));
      call.reply.bulk("histogram_usec");
      // Each step that holds a run, with how many ran within it, cumulatively.
      size_t holding = 0;
      for (const uint64_t count : histogram.steps) holding += count ? 1 : 0;
      call.reply.map(holding);
      uint64_t cumulative = 0;
      for (size_t step = 0; step < Database::latency_steps; ++step) {
        if (!histogram.steps[step]) continue;
        cumulative += histogram.steps[step];
        call.reply.integer(static_cast<int64_t>(Database::latency_step_ns(step) / 1000));
        call.reply.integer(static_cast<int64_t>(cumulative));
      }
    }
  } else {
    throw CommandError("ERR unknown subcommand '" + call.args[1] + "'. Try LATENCY HELP.");
  }
}

// Valkey 8.1's command log: nothing is logged yet, as SLOWLOG logs nothing.
void commandlog(Call& call) {
  if (help::reply_help(call, "commandlog")) return;
  const std::string sub = lower(call.args[1]);
  const size_t argc = call.args.size();
  if (sub == "get") {
    if (argc != 4) throw CommandError("ERR wrong number of arguments for 'commandlog|get' command");
    auto count = parse_integer(call.args[2]);
    if (!count || *count < -1) throw CommandError("ERR count should be greater than or equal to -1");
    commandlog_type(call.args[3]);
    call.reply.array(0);
  } else if (sub == "len") {
    if (argc != 3) throw CommandError("ERR wrong number of arguments for 'commandlog|len' command");
    commandlog_type(call.args[2]);
    call.reply.integer(0);
  } else if (sub == "reset") {
    if (argc != 3) throw CommandError("ERR wrong number of arguments for 'commandlog|reset' command");
    commandlog_type(call.args[2]);
    call.reply.simple("OK");
  } else {
    throw CommandError("ERR unknown subcommand '" + call.args[1] + "'. Try COMMANDLOG HELP.");
  }
}

namespace {

constexpr const char* no_persistence =
    "ERR persistence is not available: Mimir persists in Dagda, which this node does not use yet, and has no RDB or AOF of Valkey's kind";

[[noreturn]] void replicates_itself(std::string_view command) {
  throw CommandError("ERR Mimir replicates through its own cluster, with no primary or replica: " + upper(command) + " is not offered");
}

}  // namespace

// Ruling 31: Valkey's persistence commands wait on Dagda (as ruling 17).
void save(Call&) { throw CommandError(no_persistence); }
void bgsave(Call& call) {
  if (call.args.size() > 2 || (call.args.size() == 2 && !iequals(call.args[1], "SCHEDULE") && !iequals(call.args[1], "CANCEL")))
    throw CommandError(syntax_error);
  throw CommandError(no_persistence);
}
void bgrewriteaof(Call&) { throw CommandError(no_persistence); }

// SHUTDOWN [NOSAVE|SAVE] [NOW] [FORCE] [ABORT] [SAFE]: the node stops
// gracefully, its connections closed; this one gets no reply, as Valkey's.
// A shutdown is never in progress to abort: it happens at once.
void shutdown(Call& call) {
  bool nosave = false, save = false, abort = false, force = false, other = false;
  for (size_t i = 1; i < call.args.size(); ++i) {
    const std::string& option = call.args[i];
    if (iequals(option, "NOSAVE")) {
      nosave = true;
    } else if (iequals(option, "SAVE")) {
      save = true;
    } else if (iequals(option, "ABORT")) {
      abort = true;
    } else if (iequals(option, "FORCE")) {
      force = true;
      other = true;
    } else if (iequals(option, "NOW") || iequals(option, "SAFE")) {
      other = true;
    } else {
      throw CommandError(syntax_error);
    }
  }
  if ((abort && (nosave || save || other)) || (nosave && save)) throw CommandError(syntax_error);
  if (abort) throw CommandError("ERR No shutdown in progress.");
  if (save && !force) throw CommandError(no_persistence);
  const auto& stop = call.database.settings().shutdown;
  if (!stop) throw CommandError("ERR this node cannot be shut down by a client");
  stop();
  call.session.closing = true;
}

void replicaof(Call& call) {
  // Already no one's replica: NO ONE asks for what is.
  if (iequals(call.args[1], "NO") && iequals(call.args[2], "ONE")) {
    call.reply.simple("OK");
    return;
  }
  replicates_itself(call.args[0]);
}
void sync(Call& call) { replicates_itself(call.args[0]); }
void replconf(Call& call) { replicates_itself(call.args[0]); }
void failover(Call& call) { replicates_itself(call.args[0]); }

void lolwut(Call& call) { call.reply.verbatim("txt", "Mimir ver. " + std::string(mimir::version) + "\n"); }

// Called under the database's lock, as every handler is.
void monitor(Call& call) DANNAN_NO_THREAD_SAFETY_ANALYSIS {
  if (!call.session.push) throw CommandError("ERR MONITOR needs a connection that can be written to");
  call.database.monitor_locked(call.session);
  call.reply.simple("OK");
}

}  // namespace mimir::command::server
