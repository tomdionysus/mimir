//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The rest of Valkey 8's command list as a standalone Valkey 8.1.1 answers it
// (probed): the containers' HELP, CLUSTER and its kin, DEBUG and MODULE,
// LATENCY and COMMANDLOG, COMMAND GETKEYS, CLIENT REPLY, UNBLOCK and the
// tracking queries, and MONITOR.

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

using Args = std::vector<std::string>;

std::string simple(const std::string& text) { return "+" + text + "\r\n"; }

TEST(CommandIntrospection, EveryContainerAnswersHelp) {
  Bed bed;
  const std::vector<std::pair<std::string, size_t>> containers{{"CLIENT", 78},  {"COMMAND", 21}, {"CONFIG", 11},     {"FUNCTION", 39}, {"MEMORY", 14},
                                                               {"PUBSUB", 14},  {"SCRIPT", 19},  {"SLOWLOG", 12},    {"XGROUP", 17},   {"XINFO", 9},
                                                               {"LATENCY", 17}, {"MODULE", 11},  {"COMMANDLOG", 16}, {"ACL", 29}};
  for (const auto& [name, lines] : containers) {
    const std::string help = bed.run({name, "HELP"});
    EXPECT_EQ(help.rfind("*" + std::to_string(lines) + "\r\n+" + name + " <subcommand> [<arg> [value] [opt] ...]. Subcommands are:\r\n", 0), 0u)
        << name << ": " << help.substr(0, 120);
  }
  EXPECT_EQ(bed.run({"CLIENT", "HELP", "x"}), error("ERR wrong number of arguments for 'client|help' command"));
}

TEST(CommandIntrospection, ClusterIsDisabledAsInAStandaloneValkey) {
  Bed bed;
  EXPECT_EQ(bed.run({"CLUSTER", "INFO"}), error("ERR This instance has cluster support disabled"));
  EXPECT_EQ(bed.run({"CLUSTER", "HELP"}), error("ERR This instance has cluster support disabled"));
  EXPECT_EQ(bed.run({"CLUSTER", "BOGUS"}), error("ERR unknown subcommand 'BOGUS'. Try CLUSTER HELP."));
  EXPECT_EQ(bed.run({"ASKING"}), error("ERR This instance has cluster support disabled"));
  EXPECT_EQ(bed.run({"READONLY"}), ok());
  EXPECT_EQ(bed.run({"READWRITE"}), ok());
}

TEST(CommandIntrospection, DebugAndModulesAreRefused) {
  Bed bed;
  const std::string debug =
      "ERR DEBUG command not allowed. If the enable-debug-command option is set to \"local\", you can run it from a local connection, otherwise you need "
      "to set this option in the configuration file, and then restart the server.";
  EXPECT_EQ(bed.run({"DEBUG", "SLEEP", "0"}), error(debug));
  EXPECT_EQ(bed.run({"MODULE", "LIST"}), array({}));
  EXPECT_EQ(bed.run({"MODULE", "LOAD", "/nonexistent.so"}),
            error("ERR MODULE command not allowed. If the enable-module-command option is set to \"local\", you can run it from a local connection, "
                  "otherwise you need to set this option in the configuration file, and then restart the server."));
  EXPECT_EQ(bed.run({"MODULE", "BOGUS"}), error("ERR unknown subcommand 'BOGUS'. Try MODULE HELP."));
  EXPECT_EQ(bed.run({"PFSELFTEST"}), ok());
}

TEST(CommandIntrospection, LatencyAndTheCommandLogAreEmpty) {
  Bed bed;
  EXPECT_EQ(bed.run({"LATENCY", "LATEST"}), array({}));
  EXPECT_EQ(bed.run({"LATENCY", "HISTORY", "command"}), array({}));
  EXPECT_EQ(bed.run({"LATENCY", "RESET"}), integer(0));
  EXPECT_EQ(bed.run({"LATENCY", "GRAPH", "command"}), error("ERR No samples available for event 'command'"));
  EXPECT_EQ(bed.run({"COMMANDLOG", "LEN", "slow"}), integer(0));
  EXPECT_EQ(bed.run({"COMMANDLOG", "LEN", "large-request"}), integer(0));
  EXPECT_EQ(bed.run({"COMMANDLOG", "GET", "10", "slow"}), array({}));
  EXPECT_EQ(bed.run({"COMMANDLOG", "RESET", "slow"}), ok());
  EXPECT_EQ(bed.run({"COMMANDLOG", "LEN", "bogus"}), error("ERR type should be one of the following: slow, large-request, large-reply"));
  EXPECT_EQ(bed.run({"COMMANDLOG", "GET", "x", "slow"}), error("ERR count should be greater than or equal to -1"));
  EXPECT_EQ(bed.run({"MEMORY", "PURGE"}), ok());
}

TEST(CommandIntrospection, GetKeysFindsKeysByValkeysKeySpecs) {
  Bed bed;
  const auto keys = [&](Args args) {
    Args full{"COMMAND", "GETKEYS"};
    full.insert(full.end(), args.begin(), args.end());
    return bed.run(full);
  };
  EXPECT_EQ(keys({"SET", "a", "b"}), array({bulk("a")}));
  EXPECT_EQ(keys({"MSET", "a", "1", "b", "2"}), array({bulk("a"), bulk("b")}));
  EXPECT_EQ(keys({"GET"}), error("ERR Invalid number of arguments specified for command"));
  EXPECT_EQ(keys({"NOSUCH", "x"}), error("ERR Invalid command specified"));
  EXPECT_EQ(keys({"PING"}), error("ERR The command has no key arguments"));
  EXPECT_EQ(keys({"EVAL", "return 1", "2", "x", "y"}), array({bulk("x"), bulk("y")}));
  EXPECT_EQ(keys({"EVAL", "return 1", "0"}), array({}));
  EXPECT_EQ(keys({"SORT", "k", "STORE", "d"}), array({bulk("k"), bulk("d")}));
  EXPECT_EQ(keys({"ZUNIONSTORE", "d", "2", "a", "b"}), array({bulk("d"), bulk("a"), bulk("b")}));
  EXPECT_EQ(keys({"XREAD", "STREAMS", "a", "b", "0", "0"}), array({bulk("a"), bulk("b")}));
  EXPECT_EQ(keys({"OBJECT", "ENCODING", "k"}), array({bulk("k")}));
  const auto flags = [&](Args args) {
    Args full{"COMMAND", "GETKEYSANDFLAGS"};
    full.insert(full.end(), args.begin(), args.end());
    return bed.run(full);
  };
  EXPECT_EQ(flags({"SET", "a", "b"}), array({array({bulk("a"), array({simple("OW"), simple("update")})})}));
  EXPECT_EQ(flags({"SET", "a", "b", "GET"}), array({array({bulk("a"), array({simple("RW"), simple("access"), simple("update")})})}));
  EXPECT_EQ(flags({"COPY", "a", "b"}),
            array({array({bulk("a"), array({simple("RO"), simple("access")})}), array({bulk("b"), array({simple("OW"), simple("update")})})}));
  EXPECT_EQ(flags({"BITFIELD", "k", "GET", "u8", "0"}), array({array({bulk("k"), array({simple("RO"), simple("access")})})}));
}

TEST(CommandIntrospection, ClientReplyOffAndSkipSilenceReplies) {
  Bed bed;
  EXPECT_EQ(bed.run({"CLIENT", "REPLY", "OFF"}), "");
  EXPECT_EQ(bed.run({"SET", "k", "v"}), "");
  EXPECT_EQ(bed.run({"CLIENT", "REPLY", "ON"}), ok());
  EXPECT_EQ(bed.run({"CLIENT", "REPLY", "SKIP"}), "");
  EXPECT_EQ(bed.run({"GET", "k"}), "");
  EXPECT_EQ(bed.run({"GET", "k"}), bulk("v"));
  EXPECT_EQ(bed.run({"CLIENT", "REPLY", "BOGUS"}), error("ERR syntax error"));
}

TEST(CommandIntrospection, ClientUnblockEndsAWait) {
  Bed bed;
  command::Session waiting = bed.make_session();
  std::string woken;
  std::string out;
  auto executed = bed.database.execute(waiting, {"BLPOP", "list", "0"}, out, [&](std::string reply) { woken = std::move(reply); });
  ASSERT_TRUE(executed.wait.has_value());
  EXPECT_EQ(bed.run({"CLIENT", "UNBLOCK", std::to_string(waiting.id)}), integer(1));
  EXPECT_EQ(woken, "*-1\r\n");
  EXPECT_EQ(bed.run({"CLIENT", "UNBLOCK", std::to_string(waiting.id)}), integer(0));
  bed.database.execute(waiting, {"BLPOP", "list", "0"}, out, [&](std::string reply) { woken = std::move(reply); });
  EXPECT_EQ(bed.run({"CLIENT", "UNBLOCK", std::to_string(waiting.id), "ERROR"}), integer(1));
  EXPECT_EQ(woken, error("UNBLOCKED client unblocked via CLIENT UNBLOCK"));
  EXPECT_EQ(bed.run({"CLIENT", "UNBLOCK", "999", "BOGUS"}), error("ERR CLIENT UNBLOCK reason should be TIMEOUT or ERROR"));
  EXPECT_EQ(bed.run({"CLIENT", "UNBLOCK", "x"}), error("ERR value is not an integer or out of range"));
}

TEST(CommandIntrospection, TrackingIsOff) {
  Bed bed;
  EXPECT_EQ(bed.run({"CLIENT", "GETREDIR"}), integer(-1));
  EXPECT_EQ(bed.run({"CLIENT", "TRACKINGINFO"}), array({bulk("flags"), array({bulk("off")}), bulk("redirect"), integer(-1), bulk("prefixes"), array({})}));
  EXPECT_EQ(bed.run({"CLIENT", "CAPA", "redirect"}), ok());
  EXPECT_EQ(bed.run({"CLIENT", "IMPORT-SOURCE", "ON"}), error("ERR Server is not in import mode"));
}

TEST(CommandIntrospection, ClientListAndKillTakeAUser) {
  Bed bed;
  EXPECT_EQ(bed.run({"CLIENT", "LIST", "USER", "nosuch"}), error("ERR No such user 'nosuch'"));
  EXPECT_EQ(bed.run({"CLIENT", "KILL", "USER", "nosuch"}), error("ERR No such user 'nosuch'"));
  const std::string list = bed.run({"CLIENT", "LIST", "USER", "default"});
  EXPECT_NE(list.find("user=default"), std::string::npos) << list;
  EXPECT_EQ(bed.run({"CLIENT", "KILL", "USER", "default"}), integer(0));
}

TEST(CommandIntrospection, MonitorSeesEveryCommandButSecretsAndAdministration) {
  Bed bed;
  command::Session watcher = bed.make_session();
  std::vector<std::string> lines;
  watcher.push = [&](std::string bytes) { lines.push_back(std::move(bytes)); };
  std::string out;
  bed.database.execute(watcher, {"MONITOR"}, out);
  EXPECT_EQ(out, ok());
  bed.session.address = "127.0.0.1:5000";
  bed.run({"SET", "k", "a b\"\n"});
  bed.run({"AUTH", "secret"});
  bed.run({"CONFIG", "GET", "maxmemory"});
  bed.run({"EVAL", "return redis.call('get','k')", "0"});
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_NE(lines[0].find(" [0 127.0.0.1:5000] \"SET\" \"k\" \"a b\\\"\\n\"\r\n"), std::string::npos) << lines[0];
  EXPECT_EQ(lines[0][0], '+');
  EXPECT_NE(lines[1].find("\"AUTH\" \"(redacted)\""), std::string::npos) << lines[1];
  EXPECT_NE(lines[2].find(" [0 lua] \"get\" \"k\""), std::string::npos) << lines[2];
}

// CLIENT PAUSE, as Valkey 8.1.1 behaves (probed by timing): WRITE holds what
// writes or may replicate, ALL holds everything, CLIENT UNPAUSE included; a
// later pause keeps the later end and the stricter mode.
TEST(CommandIntrospection, ClientPauseHoldsCommandsUntilItEnds) {
  Bed bed;
  command::Session other = bed.make_session();
  std::vector<std::string> woken;
  const auto run = [&](Args args, std::string& out) {
    return bed.database.execute(other, args, out, [&](std::string reply) { woken.push_back(std::move(reply)); });
  };
  EXPECT_EQ(bed.run({"CLIENT", "PAUSE", "1000", "WRITE"}), ok());
  std::string out;
  auto read = run({"GET", "k"}, out);
  EXPECT_FALSE(read.wait.has_value());
  EXPECT_EQ(out, nil());
  out.clear();
  auto write = run({"SET", "k", "v"}, out);
  ASSERT_TRUE(write.wait.has_value());
  EXPECT_TRUE(write.postponed);
  EXPECT_EQ(out, "");
  EXPECT_FALSE(run({"EVAL_RO", "return 1", "0"}, out).wait.has_value());
  EXPECT_TRUE(run({"PUBLISH", "c", "m"}, out).postponed);
  // A held command is not a blocked one.
  EXPECT_EQ(bed.run({"CLIENT", "UNBLOCK", std::to_string(other.id)}), integer(0));
  EXPECT_EQ(bed.run({"CLIENT", "UNPAUSE"}), ok());
  EXPECT_EQ(woken, (std::vector<std::string>{"", ""}));
  EXPECT_EQ(bed.database.waiting(), 0u);
  // Run again, it runs.
  out.clear();
  EXPECT_FALSE(run({"SET", "k", "v"}, out).wait.has_value());
  EXPECT_EQ(out, ok());

  // ALL holds even PING, and a later shorter pause does not shorten it.
  EXPECT_EQ(bed.run({"CLIENT", "PAUSE", "1000", "WRITE"}), ok());
  EXPECT_EQ(bed.run({"CLIENT", "PAUSE", "10", "ALL"}), ok());
  bed.wall.advance(std::chrono::milliseconds(500));
  auto held = run({"PING"}, out);
  ASSERT_TRUE(held.postponed);
  EXPECT_NEAR(held.wait_seconds, 0.5, 0.01);
  // Its time out, the held command's wait ends with nothing: run it again.
  bed.wall.advance(std::chrono::milliseconds(600));
  EXPECT_EQ(bed.database.cancel_wait(*held.wait), std::optional<std::string>(""));
  out.clear();
  EXPECT_FALSE(run({"PING"}, out).wait.has_value());
  EXPECT_EQ(out, "+PONG\r\n");
}

TEST(CommandIntrospection, ClientPauseRefusesWhatValkeyRefuses) {
  Bed bed;
  EXPECT_EQ(bed.run({"CLIENT", "PAUSE", "x"}), error("ERR timeout is not an integer or out of range"));
  EXPECT_EQ(bed.run({"CLIENT", "PAUSE", "-1"}), error("ERR timeout is negative"));
  EXPECT_EQ(bed.run({"CLIENT", "PAUSE", "10", "BOGUS"}), error("ERR CLIENT PAUSE mode must be WRITE or ALL"));
  // Without a way to wait (a script, a test), a held command runs at once.
  EXPECT_EQ(bed.run({"CLIENT", "PAUSE", "1000"}), ok());
  EXPECT_EQ(bed.run({"PING"}), "+PONG\r\n");
  EXPECT_EQ(bed.run({"CLIENT", "UNPAUSE"}), ok());
}

// Ruling 31: Valkey's persistence and replication commands, with Mimir's
// meaning.
TEST(CommandIntrospection, PersistenceAndReplicationTakeMimirsMeaning) {
  Bed bed;
  const std::string no_persistence =
      "ERR persistence is not available: Mimir persists in Dagda, which this node does not use yet, and has no RDB or AOF of Valkey's kind";
  EXPECT_EQ(bed.run({"SAVE"}), error(no_persistence));
  EXPECT_EQ(bed.run({"BGSAVE"}), error(no_persistence));
  EXPECT_EQ(bed.run({"BGSAVE", "SCHEDULE"}), error(no_persistence));
  EXPECT_EQ(bed.run({"BGSAVE", "BOGUS"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"BGREWRITEAOF"}), error(no_persistence));
  EXPECT_EQ(bed.run({"REPLICAOF", "NO", "ONE"}), ok());
  EXPECT_EQ(bed.run({"REPLICAOF", "10.0.0.1", "6379"}),
            error("ERR Mimir replicates through its own cluster, with no primary or replica: REPLICAOF is not offered"));
  EXPECT_EQ(bed.run({"SLAVEOF", "10.0.0.1", "6379"}),
            error("ERR Mimir replicates through its own cluster, with no primary or replica: SLAVEOF is not offered"));
  EXPECT_EQ(bed.run({"FAILOVER"}), error("ERR Mimir replicates through its own cluster, with no primary or replica: FAILOVER is not offered"));
  EXPECT_EQ(bed.run({"SYNC"}), error("ERR Mimir replicates through its own cluster, with no primary or replica: SYNC is not offered"));
  // RESTORE-ASKING is RESTORE.
  bed.run({"SET", "k", "v"});
  const std::string dump = bed.run({"DUMP", "k"});
  const std::string payload = dump.substr(dump.find("\r\n") + 2, dump.size() - dump.find("\r\n") - 4);
  EXPECT_EQ(bed.run({"RESTORE-ASKING", "k2", "0", payload}), ok());
  EXPECT_EQ(bed.run({"GET", "k2"}), bulk("v"));
}

TEST(CommandIntrospection, MigrateRefusesWhatValkeyRefuses) {
  Bed bed;
  bed.run({"SET", "k", "v"});
  EXPECT_EQ(bed.run({"MIGRATE", "127.0.0.1", "7382", "nosuch", "0", "1000"}), "+NOKEY\r\n");
  EXPECT_EQ(bed.run({"MIGRATE", "127.0.0.1", "7382", "k", "0", "1000", "KEYS", "k2"}),
            error("ERR When using MIGRATE KEYS option, the key argument must be set to the empty string"));
  EXPECT_EQ(bed.run({"MIGRATE", "127.0.0.1", "7382", "k", "0", "1000", "BOGUS"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"MIGRATE", "127.0.0.1", "7382", "k", "0", "1000", "AUTH"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"MIGRATE", "127.0.0.1", "7382", "k", "x", "1000"}), error("ERR value is not an integer or out of range"));
  EXPECT_EQ(bed.run({"MIGRATE", "127.0.0.1", "7382", "k", "0", "x"}), error("ERR value is not an integer or out of range"));
  // Nothing can wait here for a target.
  EXPECT_EQ(bed.run({"MIGRATE", "127.0.0.1", "7382", "k", "0", "1000"}), error("ERR MIGRATE needs a connection that can wait for its target"));
  EXPECT_EQ(bed.run({"COMMAND", "GETKEYS", "MIGRATE", "h", "1", "", "0", "10", "AUTH", "KEYS", "KEYS", "a", "b"}), array({bulk("a"), bulk("b")}));
}

TEST(CommandIntrospection, ShutdownStopsTheNode) {
  dannan::test::SteppedWall wall;
  int stopped = 0;
  command::DatabaseSettings settings;
  settings.shutdown = [&] { ++stopped; };
  command::Database database(wall, settings);
  command::Session session;
  session.id = database.next_client_id();
  const auto run = [&](Args args) {
    std::string out;
    database.execute(session, args, out);
    return out;
  };
  EXPECT_EQ(run({"SHUTDOWN", "ABORT"}), error("ERR No shutdown in progress."));
  EXPECT_EQ(run({"SHUTDOWN", "BOGUS"}), error("ERR syntax error"));
  EXPECT_EQ(run({"SHUTDOWN", "NOW", "ABORT"}), error("ERR syntax error"));
  EXPECT_EQ(run({"SHUTDOWN", "SAVE", "NOSAVE"}), error("ERR syntax error"));
  EXPECT_EQ(run({"SHUTDOWN", "SAVE"}).rfind("-ERR persistence is not available", 0), 0u);
  EXPECT_EQ(run({"MULTI"}), ok());
  EXPECT_EQ(run({"SHUTDOWN"}), error("ERR Command not allowed inside a transaction"));
  EXPECT_EQ(run({"EXEC"}), error("EXECABORT Transaction discarded because of previous errors."));
  EXPECT_EQ(stopped, 0);
  // No reply; the connection closes and the node stops.
  EXPECT_EQ(run({"SHUTDOWN", "NOSAVE", "NOW"}), "");
  EXPECT_TRUE(session.closing);
  EXPECT_EQ(stopped, 1);
  Bed without;
  EXPECT_EQ(without.run({"SHUTDOWN"}), error("ERR this node cannot be shut down by a client"));
}

TEST(CommandIntrospection, ScriptShowAndDebug) {
  Bed bed;
  const std::string load = bed.run({"SCRIPT", "LOAD", "return 7"});
  const std::string sha = load.substr(load.find("\r\n") + 2, 40);
  EXPECT_EQ(bed.run({"SCRIPT", "SHOW", sha}), bulk("return 7"));
  EXPECT_EQ(bed.run({"SCRIPT", "SHOW", std::string(40, '0')}), error("NOSCRIPT No matching script."));
  EXPECT_EQ(bed.run({"SCRIPT", "DEBUG", "NO"}), ok());
}

}  // namespace
}  // namespace mimir::test
