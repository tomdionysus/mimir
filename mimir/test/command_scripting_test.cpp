//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Scripting, from Valkey 8's documentation ("Scripting with Lua", "Lua API
// reference", EVAL, EVALSHA, EVAL_RO, SCRIPT): a script runs whole, its
// server.call replies converted to Lua and its result back by the documented
// table; scripts are cached by the SHA-1 of their body; globals are read-only;
// errors from a command stop the script, and carry the script's sha and line.

#include <gtest/gtest.h>

#include "mimir/command/scripting.h"
#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

// The documentation's own example: EVALSHA's page.
const std::string return_one_sha = "e0e1f9fabfc9d4800c877a703b823ac0578ff8db";

std::string suffix(const std::string& sha, int line) { return " script: " + sha + ", on @user_script:" + std::to_string(line) + "."; }

TEST(CommandScripting, Sha1IsTheBodysDigest) {
  EXPECT_EQ(command::sha1_hex("return 1"), return_one_sha);
  EXPECT_EQ(command::sha1_hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST(CommandScripting, ConversionsLuaToReply) {
  Bed bed;
  EXPECT_EQ(bed.run({"EVAL", "return 1", "0"}), integer(1));
  EXPECT_EQ(bed.run({"EVAL", "return 3.99", "0"}), integer(3));
  EXPECT_EQ(bed.run({"EVAL", "return 'x'", "0"}), bulk("x"));
  EXPECT_EQ(bed.run({"EVAL", "return true", "0"}), integer(1));
  EXPECT_EQ(bed.run({"EVAL", "return false", "0"}), nil());
  EXPECT_EQ(bed.run({"EVAL", "return nil", "0"}), nil());
  EXPECT_EQ(bed.run({"EVAL", "return {1, 'a', {2}}", "0"}), array({integer(1), bulk("a"), array({integer(2)})}));
  // An array ends at its first nil.
  EXPECT_EQ(bed.run({"EVAL", "return {1, nil, 3}", "0"}), array({integer(1)}));
  EXPECT_EQ(bed.run({"EVAL", "return {ok='FINE'}", "0"}), "+FINE\r\n");
  EXPECT_EQ(bed.run({"EVAL", "return {err='MYERR bad'}", "0"}), error("MYERR bad"));
  EXPECT_EQ(bed.run({"EVAL", "return redis.status_reply('FINE')", "0"}), "+FINE\r\n");
  EXPECT_EQ(bed.run({"EVAL", "return redis.error_reply('oops')", "0"}), error("ERR oops"));
}

TEST(CommandScripting, KeysAndArgv) {
  Bed bed;
  EXPECT_EQ(bed.run({"EVAL", "return {KEYS[1], KEYS[2], ARGV[1]}", "2", "a", "b", "c"}), array({bulk("a"), bulk("b"), bulk("c")}));
  EXPECT_EQ(bed.run({"EVAL", "return #ARGV", "0", "x", "y"}), integer(2));
  EXPECT_EQ(bed.run({"EVAL", "return 1", "-1"}), error("ERR Number of keys can't be negative"));
  EXPECT_EQ(bed.run({"EVAL", "return 1", "2", "a"}), error("ERR Number of keys can't be greater than number of args"));
  EXPECT_EQ(bed.run({"EVAL", "return 1", "x"}), error("ERR value is not an integer or out of range"));
}

TEST(CommandScripting, CallRunsCommandsAndConvertsReplies) {
  Bed bed;
  EXPECT_EQ(bed.run({"EVAL", "return redis.call('SET', KEYS[1], ARGV[1])", "1", "k", "v"}), ok());
  EXPECT_EQ(bed.run({"GET", "k"}), bulk("v"));
  EXPECT_EQ(bed.run({"EVAL", "return server.call('GET', 'k')", "0"}), bulk("v"));
  // A null bulk reply is false in Lua.
  EXPECT_EQ(bed.run({"EVAL", "return redis.call('GET', 'none') == false", "0"}), integer(1));
  EXPECT_EQ(bed.run({"EVAL", "return redis.call('INCRBY', 'n', 5) + 1", "0"}), integer(6));
  EXPECT_EQ(bed.run({"EVAL", "redis.call('RPUSH', 'l', 'a', 'b'); return redis.call('LRANGE', 'l', 0, -1)", "0"}), array({bulk("a"), bulk("b")}));
  // A status reply is a table with ok.
  EXPECT_EQ(bed.run({"EVAL", "return redis.call('SET', 'k', 'w').ok", "0"}), bulk("OK"));
}

TEST(CommandScripting, CommandErrorStopsTheScript) {
  Bed bed;
  bed.run({"SET", "s", "x"});
  const std::string body = "redis.call('LPUSH', 's', 1)\nredis.call('SET', 'after', 1)";
  EXPECT_EQ(bed.run({"EVAL", body, "0"}), error("WRONGTYPE Operation against a key holding the wrong kind of value" + suffix(command::sha1_hex(body), 1)));
  EXPECT_EQ(bed.run({"EXISTS", "after"}), integer(0));
}

TEST(CommandScripting, PcallReturnsTheError) {
  Bed bed;
  bed.run({"SET", "s", "x"});
  EXPECT_EQ(bed.run({"EVAL", "local r = redis.pcall('LPUSH', 's', 1); return r.err", "0"}),
            bulk("WRONGTYPE Operation against a key holding the wrong kind of value"));
  EXPECT_EQ(bed.run({"EVAL", "return redis.pcall('LPUSH', 's', 1)", "0"}), error("WRONGTYPE Operation against a key holding the wrong kind of value"));
}

TEST(CommandScripting, RefusedCommands) {
  Bed bed;
  const std::string unknown = "return redis.call('NOSUCH')";
  EXPECT_EQ(bed.run({"EVAL", unknown, "0"}), error("ERR Unknown command called from script" + suffix(command::sha1_hex(unknown), 1)));
  const std::string noscript = "return redis.call('MULTI')";
  EXPECT_EQ(bed.run({"EVAL", noscript, "0"}), error("ERR This Redis command is not allowed from script" + suffix(command::sha1_hex(noscript), 1)));
  const std::string arity = "return redis.call('GET')";
  EXPECT_EQ(bed.run({"EVAL", arity, "0"}), error("ERR Wrong number of args calling command from script" + suffix(command::sha1_hex(arity), 1)));
}

TEST(CommandScripting, ErrorsFromTheScriptItself) {
  Bed bed;
  const std::string plain = "error('plain')";
  EXPECT_EQ(bed.run({"EVAL", plain, "0"}), error("ERR user_script:1: plain" + suffix(command::sha1_hex(plain), 1)));
  const std::string table = "error({err='CUSTOM x'})";
  EXPECT_EQ(bed.run({"EVAL", table, "0"}), error("CUSTOM x" + suffix(command::sha1_hex(table), 1)));
  const std::string bad = "return +";
  const std::string compiled = bed.run({"EVAL", bad, "0"});
  EXPECT_EQ(compiled.rfind("-ERR Error compiling script (new function): user_script:1: ", 0), 0u) << compiled;
}

TEST(CommandScripting, GlobalsAreReadOnly) {
  Bed bed;
  const std::string body = "x = 1";
  EXPECT_EQ(bed.run({"EVAL", body, "0"}), error("ERR user_script:1: Attempt to modify a readonly table" + suffix(command::sha1_hex(body), 1)));
  EXPECT_EQ(bed.run({"EVAL", "local x = 1; return x", "0"}), integer(1));
}

TEST(CommandScripting, NoFilesInTheSandbox) {
  Bed bed;
  EXPECT_EQ(bed.run({"EVAL", "return type(os.clock)", "0"}), bulk("function"));
  EXPECT_EQ(bed.run({"EVAL", "return rawget(_G, 'dofile') == nil and rawget(_G, 'io') == nil", "0"}), integer(1));
}

TEST(CommandScripting, EvalshaRunsACachedScript) {
  Bed bed;
  EXPECT_EQ(bed.run({"EVALSHA", return_one_sha, "0"}), error("NOSCRIPT No matching script."));
  EXPECT_EQ(bed.run({"SCRIPT", "LOAD", "return 1"}), bulk(return_one_sha));
  EXPECT_EQ(bed.run({"EVALSHA", return_one_sha, "0"}), integer(1));
  // The sha in any case.
  EXPECT_EQ(bed.run({"EVALSHA", "E0E1F9FABFC9D4800C877A703B823AC0578FF8DB", "0"}), integer(1));
  // EVAL caches what it runs.
  EXPECT_EQ(bed.run({"EVAL", "return 2", "0"}), integer(2));
  EXPECT_EQ(bed.run({"SCRIPT", "EXISTS", return_one_sha, command::sha1_hex("return 2"), "ffff"}), array({integer(1), integer(1), integer(0)}));
  EXPECT_EQ(bed.run({"SCRIPT", "FLUSH"}), ok());
  EXPECT_EQ(bed.run({"SCRIPT", "EXISTS", return_one_sha}), array({integer(0)}));
  EXPECT_EQ(bed.run({"EVALSHA", return_one_sha, "0"}), error("NOSCRIPT No matching script."));
}

TEST(CommandScripting, ScriptLoadRefusesWhatDoesNotCompile) {
  Bed bed;
  const std::string reply = bed.run({"SCRIPT", "LOAD", "return +"});
  EXPECT_EQ(reply.rfind("-ERR Error compiling script (new function): ", 0), 0u) << reply;
  EXPECT_EQ(bed.run({"SCRIPT", "KILL"}), error("NOTBUSY No scripts in execution right now."));
}

TEST(CommandScripting, ReadOnlyScriptsRefuseWrites) {
  Bed bed;
  bed.run({"SET", "k", "v"});
  EXPECT_EQ(bed.run({"EVAL_RO", "return redis.call('GET', KEYS[1])", "1", "k"}), bulk("v"));
  const std::string body = "return redis.call('SET', 'k', 'w')";
  EXPECT_EQ(bed.run({"EVAL_RO", body, "0"}), error("ERR Write commands are not allowed from read-only scripts." + suffix(command::sha1_hex(body), 1)));
  EXPECT_EQ(bed.run({"GET", "k"}), bulk("v"));
  bed.run({"SCRIPT", "LOAD", "return 1"});
  EXPECT_EQ(bed.run({"EVALSHA_RO", return_one_sha, "0"}), integer(1));
}

TEST(CommandScripting, SelectInAScriptIsTheScriptsOwn) {
  Bed bed;
  bed.run({"SET", "k", "zero"});
  // One database only by default: SELECT 0 is all a script may do here.
  EXPECT_EQ(bed.run({"EVAL", "redis.call('SELECT', 0); return redis.call('GET', 'k')", "0"}), bulk("zero"));
  EXPECT_EQ(bed.session.db, 0u);
}

TEST(CommandScripting, ScriptsInTransactions) {
  Bed bed;
  EXPECT_EQ(bed.run({"MULTI"}), ok());
  EXPECT_EQ(bed.run({"EVAL", "return redis.call('INCR', 'n')", "0"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"EXEC"}), array({integer(1)}));
}

}  // namespace
}  // namespace mimir::test
