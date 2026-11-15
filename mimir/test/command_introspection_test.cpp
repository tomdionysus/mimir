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
