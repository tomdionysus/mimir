//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The server commands clients and tools call, from Valkey 8's command
// documentation: CONFIG, CLIENT LIST, INFO and KILL, INFO's sections, OBJECT,
// ROLE, WAIT, WAITAOF, MEMORY USAGE, SLOWLOG and LASTSAVE, answering what is
// true of a single-node Mimir.

#include <gtest/gtest.h>

#include <chrono>
#include <set>

#include "mimir/test/fixtures/bed.h"
#include "mimir/version.h"

namespace mimir::test {
namespace {

std::string bulks(const std::vector<std::string>& items) {
  std::vector<std::string> out;
  for (const auto& item : items) out.push_back(bulk(item));
  return array(out);
}

TEST(CommandServer, ConfigGetAndSet) {
  Bed bed;
  EXPECT_EQ(bed.run({"CONFIG", "GET", "maxmemory"}), bulks({"maxmemory", "0"}));
  EXPECT_EQ(bed.run({"CONFIG", "GET", "databases", "nosuch"}), bulks({"databases", "1"}));
  EXPECT_EQ(bed.run({"CONFIG", "SET", "notify-keyspace-events", "KEA"}), ok());
  // Answered in Valkey's order: 'A' first, then K, E (the differential gate's finding).
  EXPECT_EQ(bed.run({"CONFIG", "GET", "notify-keyspace-events"}), bulks({"notify-keyspace-events", "AKE"}));
  EXPECT_EQ(bed.run({"CONFIG", "SET", "notify-keyspace-events", "KEq"}),
            error("ERR CONFIG SET failed (possibly related to argument 'notify-keyspace-events') - Invalid event class character. Use 'Ag$lshzxeKEtmdn'."));
  EXPECT_EQ(bed.run({"CONFIG", "SET", "nosuch", "1"}), error("ERR Unknown option or number of arguments for CONFIG SET - 'nosuch'"));
  EXPECT_EQ(bed.run({"CONFIG", "SET", "databases", "16"}),
            error("ERR CONFIG SET failed (possibly related to argument 'databases') - can't set immutable config"));
  EXPECT_EQ(bed.run({"CONFIG", "GET", "set-max-*-entries"}), bulks({"set-max-intset-entries", "512", "set-max-listpack-entries", "128"}));
}

TEST(CommandServer, ClientListInfoAndKill) {
  Bed bed;
  bed.run({"CLIENT", "SETNAME", "me"});
  const std::string info = bed.run({"CLIENT", "INFO"});
  EXPECT_NE(info.find("id=" + std::to_string(bed.session.id) + " "), std::string::npos) << info;
  EXPECT_NE(info.find(" name=me "), std::string::npos) << info;
  // The command's full name, a container's subcommand with it, as Valkey 8.1.1
  // shows it ("cmd=client|list" from CLIENT LIST, probed).
  EXPECT_NE(info.find(" cmd=client|info "), std::string::npos) << info;
  command::Session other;
  other.id = bed.database.next_client_id();
  bool killed = false;
  other.kill = [&] { killed = true; };
  std::string out;
  bed.database.execute(other, {"PING"}, out);
  const std::string list = bed.run({"CLIENT", "LIST"});
  EXPECT_NE(list.find("id=" + std::to_string(other.id) + " "), std::string::npos) << list;
  EXPECT_EQ(bed.run({"CLIENT", "KILL", "ID", std::to_string(other.id)}), integer(1));
  EXPECT_TRUE(killed);
  EXPECT_EQ(bed.run({"CLIENT", "KILL", "ID", std::to_string(bed.session.id)}), integer(0));
  EXPECT_EQ(bed.run({"CLIENT", "KILL", "nowhere:1"}), error("ERR No such client"));
  EXPECT_EQ(bed.run({"CLIENT", "NO-EVICT", "on"}), ok());
}

TEST(CommandServer, InfoSections) {
  Bed bed;
  bed.run({"SET", "k", "v"});
  const std::string all = bed.run({"INFO"});
  for (const char* section : {"# Server", "# Clients", "# Memory", "# Persistence", "# Replication", "# Keyspace", "db0:keys=1"}) {
    EXPECT_NE(all.find(section), std::string::npos) << section;
  }
  const std::string one = bed.run({"INFO", "replication"});
  EXPECT_NE(one.find("role:master"), std::string::npos);
  EXPECT_EQ(one.find("# Server"), std::string::npos);
}

TEST(CommandServer, ObjectEncodings) {
  Bed bed;
  bed.run({"SET", "i", "12345"});
  bed.run({"SET", "e", "short"});
  bed.run({"SET", "r", std::string(50, 'x')});
  bed.run({"SADD", "is", "1", "2"});
  bed.run({"SADD", "ls", "a"});
  bed.run({"HSET", "h", "f", "v"});
  bed.run({"ZADD", "z", "1", "m"});
  bed.run({"RPUSH", "l", "x"});
  bed.run({"XADD", "st", "1-0", "f", "v"});
  EXPECT_EQ(bed.run({"OBJECT", "ENCODING", "i"}), bulk("int"));
  EXPECT_EQ(bed.run({"OBJECT", "ENCODING", "e"}), bulk("embstr"));
  EXPECT_EQ(bed.run({"OBJECT", "ENCODING", "r"}), bulk("raw"));
  EXPECT_EQ(bed.run({"OBJECT", "ENCODING", "is"}), bulk("intset"));
  EXPECT_EQ(bed.run({"OBJECT", "ENCODING", "ls"}), bulk("listpack"));
  EXPECT_EQ(bed.run({"OBJECT", "ENCODING", "h"}), bulk("listpack"));
  EXPECT_EQ(bed.run({"OBJECT", "ENCODING", "z"}), bulk("listpack"));
  EXPECT_EQ(bed.run({"OBJECT", "ENCODING", "l"}), bulk("listpack"));
  EXPECT_EQ(bed.run({"OBJECT", "ENCODING", "st"}), bulk("stream"));
  EXPECT_EQ(bed.run({"OBJECT", "ENCODING", "nosuch"}), nil());
  EXPECT_EQ(bed.run({"OBJECT", "REFCOUNT", "i"}), integer(1));
  EXPECT_EQ(bed.run({"OBJECT", "BOGUS", "i"}), error("ERR unknown subcommand 'BOGUS'. Try OBJECT HELP."));
}

TEST(CommandServer, ReplicationAndOthers) {
  Bed bed;
  EXPECT_EQ(bed.run({"ROLE"}), array({bulk("master"), integer(0), array({})}));
  EXPECT_EQ(bed.run({"WAIT", "1", "0"}), integer(0));
  EXPECT_EQ(bed.run({"WAITAOF", "1", "0", "0"}), error("ERR WAITAOF cannot be used when numlocal is set but appendonly is disabled."));
  EXPECT_EQ(bed.run({"SLOWLOG", "GET"}), array({}));
  EXPECT_EQ(bed.run({"SLOWLOG", "LEN"}), integer(0));
  EXPECT_EQ(bed.run({"LASTSAVE"}), integer(static_cast<int64_t>(bed.wall.unix_ms() / 1000)));
  bed.run({"SET", "k", "v"});
  EXPECT_EQ(bed.run({"MEMORY", "USAGE", "nosuch"}), nil());
  EXPECT_EQ(bed.run({"MEMORY", "USAGE", "k"}).substr(0, 1), ":");
}

// The rest are Valkey 8.1.1's answers (probed) to the same commands.
TEST(CommandServer, ServerCommandsRefuseAsValkeyDoes) {
  Bed bed;
  const std::string syntax = error("ERR syntax error");
  bed.run({"SET", "s", "v"});
  std::vector<std::string> members;
  for (int i = 0; i < 156; ++i) members.push_back("m" + std::to_string(i));
  std::vector<std::string> sadd{"SADD", "bigset"};
  sadd.insert(sadd.end(), members.begin(), members.end());
  EXPECT_EQ(bed.run(sadd), integer(156));
  EXPECT_EQ(bed.run({"OBJECT", "ENCODING", "bigset"}), bulk("hashtable"));
  EXPECT_EQ(bed.run({"OBJECT", "FREQ", "s"}),
            error("ERR An LFU maxmemory policy is not selected, access frequency not tracked. Please note that when switching between policies at runtime "
                  "LRU and LFU data will take some time to adjust."));
  EXPECT_EQ(bed.run({"OBJECT", "REFCOUNT", "nokey"}), nil());
  EXPECT_EQ(bed.run({"OBJECT", "ENCODING", "s", "extra"}), error("ERR wrong number of arguments for 'object|encoding' command"));

  EXPECT_EQ(bed.run({"MEMORY", "USAGE", "s", "SAMPLES", "5"}).substr(0, 1), ":");
  EXPECT_EQ(bed.run({"MEMORY", "USAGE", "s", "SAMPLES", "x"}), error("ERR value is not an integer or out of range"));
  EXPECT_EQ(bed.run({"MEMORY", "USAGE", "s", "BOGUS", "5"}), syntax);
  EXPECT_EQ(bed.run({"MEMORY", "USAGE", "s", "SAMPLES"}), syntax);
  EXPECT_EQ(bed.run({"MEMORY", "MALLOC-STATS", "extra"}), error("ERR wrong number of arguments for 'memory|malloc-stats' command"));
  EXPECT_EQ(bed.run({"MEMORY", "PURGE", "extra"}), error("ERR wrong number of arguments for 'memory|purge' command"));
  EXPECT_EQ(bed.run({"MEMORY", "BOGUS"}), error("ERR unknown subcommand 'BOGUS'. Try MEMORY HELP."));

  EXPECT_EQ(bed.run({"SLOWLOG", "RESET"}), ok());
  EXPECT_EQ(bed.run({"SLOWLOG", "GET", "-1"}), array({}));
  EXPECT_EQ(bed.run({"SLOWLOG", "GET", "x"}), error("ERR count should be greater than or equal to -1"));
  EXPECT_EQ(bed.run({"SLOWLOG", "GET", "-2"}), error("ERR count should be greater than or equal to -1"));
  EXPECT_EQ(bed.run({"SLOWLOG", "GET", "1", "2"}), error("ERR unknown subcommand or wrong number of arguments for 'GET'. Try SLOWLOG HELP."));
  EXPECT_EQ(bed.run({"SLOWLOG", "BOGUS"}), error("ERR unknown subcommand 'BOGUS'. Try SLOWLOG HELP."));

  EXPECT_EQ(bed.run({"CONFIG", "RESETSTAT"}), ok());
  EXPECT_EQ(bed.run({"CONFIG", "REWRITE"}), error("ERR The server is running without a config file"));
  EXPECT_EQ(bed.run({"CONFIG", "BOGUS"}), error("ERR unknown subcommand 'BOGUS'. Try CONFIG HELP."));

  EXPECT_EQ(bed.run({"WAIT", "0", "-1"}), error("ERR timeout is negative"));
  EXPECT_EQ(bed.run({"WAITAOF", "0", "0", "0"}), array({integer(0), integer(0)}));
  EXPECT_EQ(bed.run({"WAITAOF", "0", "0", "-1"}), error("ERR timeout is negative"));

  EXPECT_EQ(bed.run({"LATENCY", "DOCTOR", "extra"}), error("ERR wrong number of arguments for 'latency|doctor' command"));
  EXPECT_EQ(bed.run({"LATENCY", "BOGUS"}), error("ERR unknown subcommand 'BOGUS'. Try LATENCY HELP."));
  EXPECT_EQ(bed.run({"LATENCY", "LATEST", "extra"}), error("ERR wrong number of arguments for 'latency|latest' command"));
  EXPECT_EQ(bed.run({"LATENCY", "HISTORY"}), error("ERR wrong number of arguments for 'latency|history' command"));
  EXPECT_EQ(bed.run({"LATENCY", "GRAPH"}), error("ERR wrong number of arguments for 'latency|graph' command"));
  EXPECT_EQ(bed.run({"COMMANDLOG", "BOGUS"}), error("ERR unknown subcommand 'BOGUS'. Try COMMANDLOG HELP."));
  EXPECT_EQ(bed.run({"COMMANDLOG", "GET", "1"}), error("ERR wrong number of arguments for 'commandlog|get' command"));
  EXPECT_EQ(bed.run({"COMMANDLOG", "LEN"}), error("ERR wrong number of arguments for 'commandlog|len' command"));
  EXPECT_EQ(bed.run({"COMMANDLOG", "RESET"}), error("ERR wrong number of arguments for 'commandlog|reset' command"));
  EXPECT_EQ(bed.run({"COMMANDLOG", "RESET", "bogus"}), error("ERR type should be one of the following: slow, large-request, large-reply"));

  EXPECT_EQ(bed.run({"SHUTDOWN", "ABORT"}), error("ERR No shutdown in progress."));
  for (std::vector<std::string> args : {std::vector<std::string>{"SHUTDOWN", "NOSAVE", "SAVE"},
                                        {"SHUTDOWN", "ABORT", "NOW"},
                                        {"SHUTDOWN", "BOGUS"},
                                        {"BGSAVE", "BOGUS"},
                                        {"BGSAVE", "SCHEDULE", "extra"}}) {
    EXPECT_EQ(bed.run(args), syntax) << args[0] << " " << args[1];
  }
  EXPECT_EQ(bed.run({"REPLICAOF", "NO", "ONE"}), ok());
}

// The answers that hold no state: Valkey's texts, and Mimir's where it keeps
// no such state (no modules, no replication of Valkey's kind).
TEST(CommandServer, StatelessAnswers) {
  Bed bed;
  EXPECT_EQ(bed.run({"CONFIG", "SET", "hz", "20"}), ok());
  EXPECT_EQ(bed.run({"CONFIG", "GET", "hz"}), array({bulk("hz"), bulk("20")}));
  bed.run({"SET", "k", "v"});
  EXPECT_EQ(bed.run({"OBJECT", "IDLETIME", "k"}), integer(0));
  EXPECT_EQ(bed.run({"MEMORY", "DOCTOR"}), bulk("Hi Sam, I can't find any memory issue in your instance. I can only account for what occurs on this base."));
  EXPECT_EQ(bed.run({"MEMORY", "MALLOC-STATS"}), bulk("Stats not supported for the current allocator"));
  EXPECT_EQ(bed.run({"MODULE", "UNLOAD", "a", "b"}), error("ERR wrong number of arguments for 'module|unload' command"));
  EXPECT_EQ(bed.run({"MODULE", "LOAD"}), error("ERR wrong number of arguments for 'module|load' command"));
  EXPECT_EQ(bed.run({"LATENCY", "DOCTOR"}).substr(0, 1), "$");
  EXPECT_EQ(bed.run({"LATENCY", "HISTOGRAM"}), array({}));
  EXPECT_EQ(bed.run({"REPLCONF", "listening-port", "1"}),
            error("ERR Mimir replicates through its own cluster, with no primary or replica: REPLCONF is not offered"));
  EXPECT_EQ(bed.run({"LOLWUT"}), bulk("Mimir ver. " + std::string(mimir::version) + "\n"));
  // FORCE passes the save, and this node takes no shutdown from a client.
  EXPECT_EQ(bed.run({"SHUTDOWN", "SAVE", "FORCE"}), error("ERR this node cannot be shut down by a client"));
}

// MEMORY USAGE's estimate grows with what each type of value holds.
TEST(CommandServer, MemoryUsageGrowsWithEveryType) {
  Bed bed;
  const auto usage = [&bed](const std::string& key) { return std::stoll(bed.run({"MEMORY", "USAGE", key}).substr(1)); };
  bed.run({"RPUSH", "l", "a"});
  bed.run({"HSET", "h", "f", "v"});
  bed.run({"SADD", "s", "a"});
  bed.run({"ZADD", "z", "1", "a"});
  bed.run({"XADD", "x", "1-1", "f", "v"});
  for (const std::string key : {"l", "h", "s", "z", "x"}) {
    const auto before = usage(key);
    if (key == "l") bed.run({"RPUSH", "l", "bb"});
    if (key == "h") bed.run({"HSET", "h", "g", "w"});
    if (key == "s") bed.run({"SADD", "s", "bb"});
    if (key == "z") bed.run({"ZADD", "z", "2", "bb"});
    if (key == "x") bed.run({"XADD", "x", "1-2", "f", "v"});
    EXPECT_GT(usage(key), before) << key;
  }
}

TEST(CommandServer, ClientListAndKillFiltersAsValkeyTakesThem) {
  Bed bed;
  const std::string syntax = error("ERR syntax error");
  EXPECT_NE(bed.run({"CLIENT", "LIST", "TYPE", "normal"}).find("id=" + std::to_string(bed.session.id) + " "), std::string::npos);
  EXPECT_EQ(bed.run({"CLIENT", "LIST", "TYPE", "bogus"}), error("ERR Unknown client type 'bogus'"));
  EXPECT_EQ(bed.run({"CLIENT", "LIST", "ID", "0"}), error("ERR client-id should be greater than 0"));
  EXPECT_EQ(bed.run({"CLIENT", "LIST", "ID", "x"}), syntax);
  EXPECT_EQ(bed.run({"CLIENT", "LIST", "ID"}), syntax);
  EXPECT_EQ(bed.run({"CLIENT", "LIST", "BOGUS"}), syntax);
  EXPECT_EQ(bed.run({"CLIENT", "KILL", "ID", "x"}), syntax);
  EXPECT_EQ(bed.run({"CLIENT", "KILL", "ID", "0"}), error("ERR client-id should be greater than 0"));
  EXPECT_EQ(bed.run({"CLIENT", "KILL", "SKIPME", "maybe", "ID", "5"}), syntax);
  EXPECT_EQ(bed.run({"CLIENT", "KILL", "BOGUS", "x"}), syntax);
  EXPECT_EQ(bed.run({"CLIENT", "KILL", "ID", "1", "SKIPME"}), syntax);
  EXPECT_EQ(bed.run({"CLIENT", "KILL", "TYPE", "bogus"}), error("ERR Unknown client type 'bogus'"));
  EXPECT_EQ(bed.run({"CLIENT", "NO-EVICT", "maybe"}), syntax);
  EXPECT_EQ(bed.run({"CLIENT", "NO-TOUCH", "on"}), ok());

  // MAXAGE kills only what is older than it; LADDR matches no connection;
  // TYPE and ADDR and SKIPME as documented.
  command::Session other;
  other.id = bed.database.next_client_id();
  other.address = "192.0.2.1:5000";
  other.created_ms = bed.wall.unix_ms();
  int killed = 0;
  other.kill = [&] { ++killed; };
  std::string out;
  bed.database.execute(other, {"PING"}, out);
  bed.wall.advance(std::chrono::seconds(10));
  EXPECT_EQ(bed.run({"CLIENT", "KILL", "MAXAGE", "10", "ID", std::to_string(other.id)}), integer(0));
  EXPECT_EQ(bed.run({"CLIENT", "KILL", "LADDR", "192.0.2.9:6379"}), integer(0));
  EXPECT_EQ(bed.run({"CLIENT", "KILL", "TYPE", "pubsub"}), integer(0));
  EXPECT_EQ(bed.run({"CLIENT", "LIST", "ID", std::to_string(other.id)}).find("id=" + std::to_string(bed.session.id) + " "), std::string::npos);
  bed.wall.advance(std::chrono::seconds(1));
  EXPECT_EQ(bed.run({"CLIENT", "KILL", "MAXAGE", "10", "TYPE", "normal", "ADDR", "192.0.2.1:5000", "SKIPME", "yes"}), integer(1));
  EXPECT_EQ(killed, 1);
}

TEST(CommandServer, CommandListFiltersAsValkeys) {
  Bed bed;
  const auto names = [&](std::vector<std::string> args) {
    std::string reply = bed.run(args);
    std::set<std::string> out;
    size_t at = reply.find("\r\n") + 2;
    while (at < reply.size()) {
      const size_t length_end = reply.find("\r\n", at);
      const size_t length = std::stoul(reply.substr(at + 1, length_end - at - 1));
      out.insert(reply.substr(length_end + 2, length));
      at = length_end + 2 + length + 2;
    }
    return out;
  };
  EXPECT_EQ(names({"COMMAND", "LIST", "FILTERBY", "PATTERN", "config*"}),
            (std::set<std::string>{"config", "config|get", "config|help", "config|resetstat", "config|rewrite", "config|set"}));
  // Valkey's are pfadd, pfcount, pfmerge, pfselftest and pfdebug, which Mimir
  // does not offer yet.
  const auto hll = names({"COMMAND", "LIST", "FILTERBY", "ACLCAT", "hyperloglog"});
  for (const char* name : {"pfadd", "pfcount", "pfmerge", "pfselftest"}) EXPECT_TRUE(hll.count(name)) << name;
  for (const auto& name : hll) EXPECT_EQ(name.substr(0, 2), "pf") << name;
  const auto keyspace = names({"COMMAND", "LIST", "FILTERBY", "aclcat", "KEYSPACE"});
  EXPECT_TRUE(keyspace.count("object|encoding"));
  EXPECT_TRUE(keyspace.count("del"));
  EXPECT_FALSE(keyspace.count("object"));
  EXPECT_EQ(bed.run({"COMMAND", "LIST", "FILTERBY", "ACLCAT", "nosuch"}), array({}));
  EXPECT_EQ(bed.run({"COMMAND", "LIST", "FILTERBY", "MODULE", "nosuch"}), array({}));
  for (std::vector<std::string> args : {std::vector<std::string>{"COMMAND", "LIST", "FILTERBY", "ACLCAT"},
                                        {"COMMAND", "LIST", "FILTERBY", "PATTERN", "x", "y"},
                                        {"COMMAND", "LIST", "FILTERBY", "BOGUS", "x"},
                                        {"COMMAND", "LIST", "extra"}}) {
    EXPECT_EQ(bed.run(args), error("ERR syntax error")) << args.back();
  }
  EXPECT_EQ(bed.run({"COMMAND", "COUNT", "extra"}), error("ERR wrong number of arguments for 'command|count' command"));
  EXPECT_EQ(bed.run({"COMMAND", "BOGUS"}), error("ERR unknown subcommand 'BOGUS'. Try COMMAND HELP."));
  const std::string info = bed.run({"COMMAND", "INFO", "nosuch", "get"});
  EXPECT_EQ(info.substr(0, 9), "*2\r\n$-1\r\n") << info;
  // COMMAND alone and COMMAND INFO alone: every command, as many as COUNT.
  const std::string count = bed.run({"COMMAND", "COUNT"});
  const std::string all = "*" + count.substr(1, count.size() - 3) + "\r\n";
  EXPECT_EQ(bed.run({"COMMAND"}).substr(0, all.size()), all);
  EXPECT_EQ(bed.run({"COMMAND", "INFO"}).substr(0, all.size()), all);
}

}  // namespace
}  // namespace mimir::test
