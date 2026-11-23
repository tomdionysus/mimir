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
  // A key a script writes is declared (ruling 36: "all keys must be passed
  // explicitly"); the script may still name it literally.
  EXPECT_EQ(bed.run({"EVAL", "return redis.call('INCRBY', 'n', 5) + 1", "1", "n"}), integer(6));
  EXPECT_EQ(bed.run({"EVAL", "redis.call('RPUSH', 'l', 'a', 'b'); return redis.call('LRANGE', 'l', 0, -1)", "1", "l"}), array({bulk("a"), bulk("b")}));
  // A status reply is a table with ok.
  EXPECT_EQ(bed.run({"EVAL", "return redis.call('SET', 'k', 'w').ok", "1", "k"}), bulk("OK"));
}

TEST(CommandScripting, CommandErrorStopsTheScript) {
  Bed bed;
  bed.run({"SET", "s", "x"});
  const std::string body = "redis.call('LPUSH', 's', 1)\nredis.call('SET', 'after', 1)";
  EXPECT_EQ(bed.run({"EVAL", body, "2", "s", "after"}),
            error("WRONGTYPE Operation against a key holding the wrong kind of value" + suffix(command::sha1_hex(body), 1)));
  EXPECT_EQ(bed.run({"EXISTS", "after"}), integer(0));
}

TEST(CommandScripting, PcallReturnsTheError) {
  Bed bed;
  bed.run({"SET", "s", "x"});
  EXPECT_EQ(bed.run({"EVAL", "local r = redis.pcall('LPUSH', 's', 1); return r.err", "1", "s"}),
            bulk("WRONGTYPE Operation against a key holding the wrong kind of value"));
  EXPECT_EQ(bed.run({"EVAL", "return redis.pcall('LPUSH', 's', 1)", "1", "s"}), error("WRONGTYPE Operation against a key holding the wrong kind of value"));
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
  EXPECT_EQ(bed.run({"EVAL", "return type(os.time)", "0"}), bulk("nil"));
  for (const char* gone : {"dofile", "loadfile", "print", "io", "debug", "package", "require", "module", "getfenv", "setfenv", "newproxy"}) {
    EXPECT_EQ(bed.run({"EVAL", std::string("return type(rawget(_G, '") + gone + "'))", "0"}), bulk("nil")) << gone;
  }
  for (const char* kept : {"pairs", "loadstring", "load", "coroutine", "unpack", "gcinfo", "cjson", "bit", "cmsgpack", "struct", "redis", "server"}) {
    EXPECT_NE(bed.run({"EVAL", std::string("return type(rawget(_G, '") + kept + "'))", "0"}), bulk("nil")) << kept;
  }
}

TEST(CommandScripting, EveryLibraryIsReadOnly) {
  Bed bed;
  for (const std::string body : {"redis.call = nil", "cjson.encode = nil", "string.foo = 1", "server.x = 1", "struct.pack = nil"}) {
    EXPECT_EQ(bed.run({"EVAL", body, "0"}), error("ERR user_script:1: Attempt to modify a readonly table" + suffix(command::sha1_hex(body), 1))) << body;
  }
  for (const std::string body : {"rawset(_G, 'x', 1)", "setmetatable(_G, nil)"}) {
    EXPECT_EQ(bed.run({"EVAL", body, "0"}), error("ERR Attempt to modify a readonly table" + suffix(command::sha1_hex(body), 1))) << body;
  }
  const std::string missing = "return _G.x";
  EXPECT_EQ(bed.run({"EVAL", missing, "0"}),
            error("ERR user_script:1: Script attempted to access nonexistent global variable 'x'" + suffix(command::sha1_hex(missing), 1)));
  // Code compiled inside a script runs on the same read-only globals.
  EXPECT_EQ(bed.run({"EVAL", "local f = loadstring('y = 1'); return pcall(f)", "0"}), nil());
  // Strings keep their methods, and their metatable reads the string library.
  EXPECT_EQ(bed.run({"EVAL", "return ('abc'):upper()", "0"}), bulk("ABC"));
  EXPECT_EQ(bed.run({"EVAL", "return getmetatable('').__index == string", "0"}), integer(1));
  // A library is walked through its proxy, which is handed back, never the
  // table beneath.
  EXPECT_EQ(bed.run({"EVAL", "local n = 0 for k, v in pairs(struct) do n = n + 1 end return n", "0"}), integer(3));
  EXPECT_EQ(bed.run({"EVAL", "local f, s = pairs(string); return s == string", "0"}), integer(1));
  EXPECT_EQ(bed.run({"EVAL", "local k = next(struct); return type(k)", "0"}), bulk("string"));
  EXPECT_EQ(bed.run({"EVAL", "local n = 0 for i, v in ipairs({5, 6, 7}) do n = n + v end return n", "0"}), integer(18));
  // Tables a script makes are its own.
  EXPECT_EQ(bed.run({"EVAL", "local t = {}; t.x = 1; setmetatable(t, {}); rawset(t, 'y', 2); return t.x + t.y", "0"}), integer(3));
}

TEST(CommandScripting, Cjson) {
  Bed bed;
  EXPECT_EQ(bed.run({"EVAL", "return cjson.encode({1, 2, {a=1}})", "0"}), bulk("[1,2,{\"a\":1}]"));
  EXPECT_EQ(bed.run({"EVAL", "return cjson.encode({a=1.5})", "0"}), bulk("{\"a\":1.5}"));
  EXPECT_EQ(bed.run({"EVAL", "return cjson.decode('[1,2,3.25]')", "0"}), array({integer(1), integer(2), integer(3)}));
  EXPECT_EQ(bed.run({"EVAL", "return cjson.encode(cjson.decode('[]'))", "0"}), bulk("{}"));
  EXPECT_EQ(bed.run({"EVAL", "return cjson.encode(1/3)", "0"}), bulk("0.33333333333333"));
  EXPECT_EQ(bed.run({"EVAL", "return cjson.encode({[1]=1, [3]=3})", "0"}), bulk("[1,null,3]"));
  EXPECT_EQ(bed.run({"EVAL", "return cjson.decode('null') == cjson.null", "0"}), integer(1));
}

TEST(CommandScripting, BitAndCmsgpack) {
  Bed bed;
  EXPECT_EQ(bed.run({"EVAL", "return bit.band(0xff, 0x0f)", "0"}), integer(15));
  EXPECT_EQ(bed.run({"EVAL", "return bit.tohex(255)", "0"}), bulk("000000ff"));
  EXPECT_EQ(bed.run({"EVAL", "return bit.tobit(4294967296 + 5)", "0"}), integer(5));
  // MessagePack: a positive fixint, a fixstr, a fixarray.
  EXPECT_EQ(bed.run({"EVAL", "return cmsgpack.pack(1, 'a', {1, 2})", "0"}), bulk(std::string("\x01\xa1"
                                                                                             "a\x92\x01\x02",
                                                                                             6)));
  EXPECT_EQ(bed.run({"EVAL", "return {cmsgpack.unpack(cmsgpack.pack(1, 'a'))}", "0"}), array({integer(1), bulk("a")}));
}

TEST(CommandScripting, Struct) {
  Bed bed;
  EXPECT_EQ(bed.run({"EVAL", "return struct.pack('<HH', 1, 2)", "0"}), bulk(std::string("\x01\x00\x02\x00", 4)));
  EXPECT_EQ(bed.run({"EVAL", "return struct.pack('>i4', -2)", "0"}), bulk("\xff\xff\xff\xfe"));
  EXPECT_EQ(bed.run({"EVAL", "return {struct.unpack('<HB', '\\1\\0\\7')}", "0"}), array({integer(1), integer(7), integer(4)}));
  EXPECT_EQ(bed.run({"EVAL", "return struct.size('i4d')", "0"}), integer(12));
  EXPECT_EQ(bed.run({"EVAL", "return struct.pack('s', 'ab')", "0"}), bulk(std::string("ab\0", 3)));
  EXPECT_EQ(bed.run({"EVAL", "return {struct.unpack('bc0', struct.pack('bc0', 3, 'xyz'))}", "0"}), array({bulk("xyz"), integer(5)}));
  // '!4' aligns the int to four bytes after the one-byte b.
  EXPECT_EQ(bed.run({"EVAL", "return struct.pack('<!4bi', 1, 2)", "0"}), bulk(std::string("\x01\0\0\0\x02\0\0\0", 8)));
  EXPECT_EQ(bed.run({"EVAL", "return struct.pack('x')", "0"}), bulk(std::string("\0", 1)));
  EXPECT_EQ(bed.run({"EVAL", "return {struct.unpack('>d', struct.pack('>d', 1.5))}", "0"}), array({integer(1), integer(9)}));
  const std::string bad = "return struct.pack('q', 1)";
  EXPECT_EQ(bed.run({"EVAL", bad, "0"}), error("ERR user_script:1: bad argument #1 to 'pack' (invalid format option 'q')" + suffix(command::sha1_hex(bad), 1)));
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

// Valkey 8.1.1's answers (probed) for scripts that call the library wrongly,
// and for the helpers redis.sha1hex, replicate_commands, setresp and os.clock.
TEST(CommandScripting, CallingTheLibraryWronglyAsValkeyAnswers) {
  Bed bed;
  const auto eval = [&](const std::string& body) { return bed.run({"EVAL", body, "0"}); };
  const auto failed = [](const std::string& body, const std::string& message) { return error(message + suffix(command::sha1_hex(body), 1)); };
  EXPECT_EQ(eval("return redis.call()"), failed("return redis.call()", "ERR Please specify at least one argument for this call"));
  EXPECT_EQ(eval("return redis.call({})"), failed("return redis.call({})", "ERR Command arguments must be strings or integers"));
  EXPECT_EQ(eval("return redis.pcall()"), error("ERR Please specify at least one argument for this call"));
  EXPECT_EQ(eval("return redis.call('nosuch')"), failed("return redis.call('nosuch')", "ERR Unknown command called from script"));
  EXPECT_EQ(eval("return redis.call('get')"), failed("return redis.call('get')", "ERR Wrong number of args calling command from script"));
  EXPECT_EQ(eval("return redis.call('subscribe', 'c')"), failed("return redis.call('subscribe', 'c')", "ERR This Redis command is not allowed from script"));
  // A null array reply is false in Lua.
  EXPECT_EQ(bed.run({"EVAL", "return tostring(redis.call('BLPOP', KEYS[1], '0'))", "1", "nokey"}), bulk("false"));

  EXPECT_EQ(eval("return redis.sha1hex('')"), bulk("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
  EXPECT_EQ(eval("return redis.sha1hex('abc')"), bulk("a9993e364706816aba3e25717850c26c9cd0d89d"));
  EXPECT_EQ(eval("return redis.replicate_commands()"), integer(1));
  EXPECT_EQ(eval("redis.setresp(4) return 1"), failed("redis.setresp(4) return 1", "ERR RESP version must be 2 or 3."));
  EXPECT_EQ(eval("redis.setresp(3) return 1"), integer(1));
  EXPECT_EQ(eval("return type(os.clock())"), bulk("number"));
}

// struct.pack, unpack and size as Valkey 8.1.1's scripts answer them (probed),
// bytes shown in hex.
TEST(CommandScripting, StructAsValkeysScriptsSeeIt) {
  Bed bed;
  const std::string hex = "local function hex(s) return (s:gsub('.', function(c) return string.format('%02x', string.byte(c)) end)) end ";
  const auto eval = [&](const std::string& body) { return bed.run({"EVAL", hex + body, "0"}); };
  const auto failed = [&](const std::string& body, const std::string& message) {
    return error("ERR user_script:1: " + message + suffix(command::sha1_hex(hex + body), 1));
  };
  EXPECT_EQ(eval("return hex(struct.pack('>l', 1))"), bulk("0000000000000001"));
  EXPECT_EQ(eval("return hex(struct.pack('<L', 2))"), bulk("0200000000000000"));
  EXPECT_EQ(eval("return hex(struct.pack('T', 5))"), bulk("0500000000000000"));
  EXPECT_EQ(eval("return hex(struct.pack('<f', 1.5))"), bulk("0000c03f"));
  EXPECT_EQ(eval("return hex(struct.pack('>f', 1.5))"), bulk("3fc00000"));
  EXPECT_EQ(eval("return hex(struct.pack('>d', 1.5))"), bulk("3ff8000000000000"));
  EXPECT_EQ(eval("return hex(struct.pack(' b', 1))"), bulk("01"));
  EXPECT_EQ(eval("return hex(struct.pack('!4 b i', 1, 2))"), bulk("0100000002000000"));
  EXPECT_EQ(eval("return hex(struct.pack('s', 'ab'))"), bulk("616200"));
  EXPECT_EQ(eval("return hex(struct.pack('c3', 'abcd'))"), bulk("616263"));
  EXPECT_EQ(eval("return hex(struct.pack('<i17', 1))"), bulk("0100000000000000000000000000000000"));
  EXPECT_EQ(eval("return hex(struct.pack('<i9', -1))"), bulk("ffffffffffffffff00"));
  EXPECT_EQ(eval("return hex(struct.pack('>i9', -2))"), bulk("00fffffffffffffffe"));

  EXPECT_EQ(eval("return struct.unpack('>i2', '\\1\\2')"), integer(258));
  EXPECT_EQ(eval("return struct.unpack('<i2', '\\1\\2')"), integer(513));
  EXPECT_EQ(eval("return struct.unpack('x b', '\\0\\5')"), integer(5));
  EXPECT_EQ(eval("return tostring(struct.unpack('<f', struct.pack('<f', 1.5)))"), bulk("1.5"));
  EXPECT_EQ(eval("return tostring(struct.unpack('>f', struct.pack('>f', -2.25)))"), bulk("-2.25"));
  EXPECT_EQ(eval("return {struct.unpack('s', 'ab\\0c')}"), array({bulk("ab"), integer(4)}));
  EXPECT_EQ(eval("return {struct.unpack('b c0', '\\2xy')}"), array({bulk("xy"), integer(4)}));
  EXPECT_EQ(eval("return struct.unpack('<i9', struct.pack('<i9', -3))"), integer(-3));
  EXPECT_EQ(eval("return struct.unpack('<I9', '\\1\\0\\0\\0\\0\\0\\0\\0\\1')"), integer(1));
  EXPECT_EQ(eval("return struct.size('!8 b d')"), integer(16));
  EXPECT_EQ(eval("return struct.size('>l')"), integer(8));

  EXPECT_EQ(eval("return struct.unpack('s', 'abc')"), failed("return struct.unpack('s', 'abc')", "unfinished string in data"));
  EXPECT_EQ(eval("return struct.unpack('c0', 'xy')"), failed("return struct.unpack('c0', 'xy')", "format 'c0' needs a previous size"));
  EXPECT_EQ(eval("return struct.size('s')"), failed("return struct.size('s')", "bad argument #1 to 'size' (option 's' has no fixed size)"));
  EXPECT_EQ(eval("return struct.size('c0')"), failed("return struct.size('c0')", "bad argument #1 to 'size' (option 'c0' has no fixed size)"));
  EXPECT_EQ(eval("return struct.pack('y', 1)"), failed("return struct.pack('y', 1)", "bad argument #1 to 'pack' (invalid format option 'y')"));
  EXPECT_EQ(eval("return struct.pack('!3 b', 1)"), failed("return struct.pack('!3 b', 1)", "alignment 3 is not a power of 2"));
  EXPECT_EQ(eval("return struct.pack('i33', 1)"), failed("return struct.pack('i33', 1)", "integral size 33 is larger than limit of 32"));
  EXPECT_EQ(eval("return struct.unpack('b', '')"), failed("return struct.unpack('b', '')", "bad argument #2 to 'unpack' (data string too short)"));
  EXPECT_EQ(eval("return struct.unpack('b', 'a', 0)"),
            failed("return struct.unpack('b', 'a', 0)", "bad argument #3 to 'unpack' (offset must be 1 or greater)"));
}

TEST(CommandScripting, ScriptSubcommandsRefuseWhatTheyDoNotTake) {
  Bed bed;
  EXPECT_EQ(bed.run({"SCRIPT", "FLUSH", "LATER"}), error("ERR SCRIPT FLUSH only support SYNC|ASYNC option"));
  EXPECT_EQ(bed.run({"SCRIPT", "FLUSH", "ASYNC", "SYNC"}), error("ERR SCRIPT FLUSH only support SYNC|ASYNC option"));
  EXPECT_EQ(bed.run({"SCRIPT", "DEBUG", "NO"}), ok());
  EXPECT_EQ(bed.run({"SCRIPT", "DEBUG", "MAYBE"}), error("ERR Use SCRIPT DEBUG YES/SYNC/NO"));
  // Mimir has no Lua debugger.
  EXPECT_EQ(bed.run({"SCRIPT", "DEBUG", "YES"}), error("ERR Mimir has no Lua debugger: SCRIPT DEBUG takes only NO"));
  EXPECT_EQ(bed.run({"SCRIPT", "DEBUG", "SYNC"}), error("ERR Mimir has no Lua debugger: SCRIPT DEBUG takes only NO"));
  EXPECT_EQ(bed.run({"SCRIPT", "WHAT"}), error("ERR unknown subcommand 'WHAT'. Try SCRIPT HELP."));
}

// redis.setresp(3), as Valkey 8.1.1 answers it (probed, byte for byte): the
// script's calls are answered in RESP3 and converted to Lua by its rules, its
// booleans become RESP3's, and its tables with a double, big_number, map, set
// or verbatim_string field become those types, for a RESP3 caller, or their
// RESP2 forms. The setting lasts for its own run.
TEST(CommandScripting, SetrespThreeAndTheResp3Conversions) {
  Bed bed;
  bed.run({"HSET", "h", "f", "v"});
  bed.run({"ZADD", "z", "1.5", "a", "2", "b"});
  bed.run({"SADD", "s", "a"});
  command::Session three = bed.make_session();
  bed.run_as(three, {"HELLO", "3"});
  const auto both = [&](const std::string& script, const std::string& resp2, const std::string& resp3) {
    EXPECT_EQ(bed.run({"EVAL", script, "0"}), resp2) << script;
    EXPECT_EQ(bed.run_as(three, {"EVAL", script, "0"}), resp3) << script;
  };
  // What a script's calls give it.
  const auto seen = [&](const std::string& script, const std::vector<std::string>& values) {
    std::vector<std::string> bulks;
    for (const auto& v : values) bulks.push_back(bulk(v));
    EXPECT_EQ(bed.run({"EVAL", script, "0"}), array(bulks)) << script;
  };
  seen("redis.setresp(3); local r = redis.call('HGETALL','h'); return {type(r), type(r.map), tostring(r.map.f)}", {"table", "table", "v"});
  seen("redis.setresp(3); local r = redis.call('ZSCORE','z','a'); return {type(r), tostring(r.double)}", {"table", "1.5"});
  seen("redis.setresp(3); local r = redis.call('SMEMBERS','s'); return {type(r), type(r.set), tostring(r.set.a)}", {"table", "table", "true"});
  seen(
      "redis.setresp(3); local r = redis.call('ZRANGE','z',0,-1,'WITHSCORES'); return {type(r[1]), tostring(r[1][1]), type(r[1][2]), tostring(r[1][2].double)}",
      {"table", "a", "table", "1.5"});
  seen("redis.setresp(3); local r = redis.call('EXISTS','h'); return {type(r), tostring(r)}", {"number", "1"});
  // A write is declared: Mimir commits it by agreement on its key.
  EXPECT_EQ(bed.run({"EVAL", "redis.setresp(3); local r = redis.call('SET',KEYS[1],'v'); return {type(r), tostring(r.ok)}", "1", "k"}),
            array({bulk("table"), bulk("OK")}));
  seen("redis.setresp(3); local r = redis.call('HGETALL','nosuch'); return {type(r), type(r.map), tostring(next(r.map))}", {"table", "table", "nil"});
  EXPECT_EQ(bed.run({"EVAL", "redis.setresp(3); local r = redis.pcall('INCR',KEYS[1]); return {type(r), tostring(r.err)}", "1", "h"}),
            array({bulk("table"), bulk("WRONGTYPE Operation against a key holding the wrong kind of value")}));
  EXPECT_EQ(bed.run({"EVAL", "redis.setresp(3); return tostring(redis.call('GET','nosuch'))", "0"}), bulk("nil"));
  EXPECT_EQ(bed.run({"EVAL", "redis.setresp(2); return tostring(redis.call('GET','nosuch'))", "0"}), bulk("false"));
  // The next run starts at 2 again.
  EXPECT_EQ(bed.run({"EVAL", "local r = redis.call('HGETALL','h'); return type(r.map)", "0"}), bulk("nil"));

  // What a script returns.
  both("return true", ":1\r\n", ":1\r\n");
  both("return false", "$-1\r\n", "_\r\n");
  both("redis.setresp(3); return true", ":1\r\n", "#t\r\n");
  both("redis.setresp(3); return false", ":0\r\n", "#f\r\n");
  both("redis.setresp(3); return nil", "$-1\r\n", "_\r\n");
  both("redis.setresp(3); return redis.call('GET','nosuch')", "$-1\r\n", "_\r\n");
  both("redis.setresp(3); return {1, false, true}", "*3\r\n:1\r\n:0\r\n:1\r\n", "*3\r\n:1\r\n#f\r\n#t\r\n");
  both("return {double=3.5}", "$3\r\n3.5\r\n", ",3.5\r\n");
  both("return {double=0.1}", "$3\r\n0.1\r\n", ",0.1\r\n");
  both("return {double=1e300}", "$6\r\n1e+300\r\n", ",1e+300\r\n");
  both("return {double=-0.0}", "$2\r\n-0\r\n", ",-0\r\n");
  both("return {double=1/0}", "$3\r\ninf\r\n", ",inf\r\n");
  both("return {big_number='123'}", "$3\r\n123\r\n", "(123\r\n");
  both("return {verbatim_string={format='txt', string='hello'}}", "$5\r\nhello\r\n", "=9\r\ntxt:hello\r\n");
  both("return {map={a=1}}", "*2\r\n$1\r\na\r\n:1\r\n", "%1\r\n$1\r\na\r\n:1\r\n");
  both("return {set={a=true}}", "*1\r\n$1\r\na\r\n", "~1\r\n$1\r\na\r\n");
  both("redis.setresp(3); return redis.call('ZSCORE','z','a')", "$3\r\n1.5\r\n", ",1.5\r\n");
  both("redis.setresp(3); return redis.call('HGETALL','h')", "*2\r\n$1\r\nf\r\n$1\r\nv\r\n", "%1\r\n$1\r\nf\r\n$1\r\nv\r\n");
  // A field of another Lua type leaves the table an array (here an empty one).
  for (const char* script :
       {"return {double='abc'}", "return {map='notatable'}", "return {set='notatable'}", "return {verbatim_string='x'}", "return {big_number=5}"}) {
    both(script, "*0\r\n", "*0\r\n");
  }

  // Its refusals.
  for (const auto& [script, message] : std::vector<std::pair<std::string, std::string>>{{"redis.setresp(4)", "ERR RESP version must be 2 or 3."},
                                                                                        {"redis.setresp()", "ERR server.setresp() requires one argument."}}) {
    EXPECT_EQ(bed.run({"EVAL", script, "0"}), error(message + suffix(command::sha1_hex(script), 1))) << script;
  }
  EXPECT_EQ(bed.run({"EVAL", "redis.setresp('3')", "0"}), nil());
}

TEST(CommandScripting, ScriptsInTransactions) {
  Bed bed;
  EXPECT_EQ(bed.run({"MULTI"}), ok());
  EXPECT_EQ(bed.run({"EVAL", "return redis.call('INCR', 'n')", "1", "n"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"EXEC"}), array({integer(1)}));
}

}  // namespace
}  // namespace mimir::test
