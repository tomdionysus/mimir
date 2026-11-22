//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Functions, from Valkey 8's documentation ("Functions", FUNCTION LOAD,
// FCALL): a library's code names its engine and itself on its first line,
// runs once at load with only server.register_function and server.log to
// hand, and registers functions that FCALL runs with their keys and arguments.
// Libraries outlive SCRIPT FLUSH; a no-writes function may be called by
// FCALL_RO and may not write.

#include <gtest/gtest.h>

#include "mimir/keyspace/rdb.h"
#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

const std::string library =
    "#!lua name=lib1\n"
    "redis.register_function('echo', function(keys, args) return {keys[1], args[1]} end)\n"
    "redis.register_function{function_name='put', callback=function(keys, args) return redis.call('SET', keys[1], args[1]) end}\n"
    "redis.register_function{function_name='get', callback=function(keys, args) return redis.call('GET', keys[1]) end, flags={'no-writes'}, "
    "description='reads'}";

std::string function_error(const std::string& text, const std::string& name, int line) {
  return error(text + " script: " + name + ", on @user_function:" + std::to_string(line) + ".");
}

TEST(CommandFunctions, LoadAndCall) {
  Bed bed;
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", library}), bulk("lib1"));
  EXPECT_EQ(bed.run({"FCALL", "echo", "1", "k", "a"}), array({bulk("k"), bulk("a")}));
  EXPECT_EQ(bed.run({"FCALL", "put", "1", "k", "v"}), ok());
  EXPECT_EQ(bed.run({"FCALL_RO", "get", "1", "k"}), bulk("v"));
  EXPECT_EQ(bed.run({"FCALL", "get", "1", "k"}), bulk("v"));
}

// The libraries' record as agreed is bytes from the cluster: one that does not
// read as FUNCTION DUMP's payload refuses the next change.
TEST(CommandFunctions, ALibrariesRecordThatDoesNotDecodeIsRefused) {
  Bed bed;
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", library}), bulk("lib1"));
  bed.agreements->record(0xffff, "functions").payload = std::string("not a payload");
  EXPECT_EQ(bed.run({"FUNCTION", "FLUSH"}), error("ERR the libraries' stored record cannot be read"));
}

// One that decodes but holds a library that does not load is refused too, and
// changes nothing.
TEST(CommandFunctions, ALibrariesRecordWhoseCodeDoesNotLoadIsRefused) {
  Bed bed;
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", library}), bulk("lib1"));
  bed.agreements->record(0xffff, "functions").payload = keyspace::rdb::dump_functions({"#!lua name=bad\nthis is not lua"});
  EXPECT_EQ(bed.run({"FUNCTION", "FLUSH"}), error("ERR the libraries' stored record cannot be read"));
  EXPECT_EQ(bed.run({"FCALL", "echo", "1", "k", "a"}), array({bulk("k"), bulk("a")}));
}

// A function's description, as FUNCTION LIST gives it.
TEST(CommandFunctions, ListGivesADescription) {
  Bed bed;
  bed.run({"FUNCTION", "LOAD", "#!lua name=one\nredis.register_function{function_name='f', callback=function() return 1 end, description='says one'}"});
  EXPECT_EQ(bed.run({"FUNCTION", "LIST"}),
            array({array({bulk("library_name"), bulk("one"), bulk("engine"), bulk("LUA"), bulk("functions"),
                          array({array({bulk("name"), bulk("f"), bulk("description"), bulk("says one"), bulk("flags"), array({})})})})}));
}

TEST(CommandFunctions, LoadRefusesAnExistingLibraryUnlessReplacing) {
  Bed bed;
  bed.run({"FUNCTION", "LOAD", library});
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", library}), error("ERR Library 'lib1' already exists"));
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "REPLACE", "#!lua name=lib1\nredis.register_function('echo', function() return 3 end)"}), bulk("lib1"));
  EXPECT_EQ(bed.run({"FCALL", "echo", "0"}), integer(3));
  // The replaced library's other functions are gone with it.
  EXPECT_EQ(bed.run({"FCALL", "put", "1", "k", "v"}), error("ERR Function not found"));
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "WAT", library}), error("ERR Unknown option given: WAT"));
}

TEST(CommandFunctions, AFunctionNameBelongsToOneLibrary) {
  Bed bed;
  bed.run({"FUNCTION", "LOAD", library});
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!lua name=lib2\nredis.register_function('echo', function() return 1 end)"}),
            error("ERR Function echo already exists"));
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!lua name=lib2\nredis.register_function('a', function() end)\nredis.register_function('a', function() end)"}),
            error("ERR Function already exists in the library"));
}

TEST(CommandFunctions, Metadata) {
  Bed bed;
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "no shebang"}), error("ERR Missing library metadata"));
  // Code that is its first line alone.
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!lua"}), error("ERR Invalid library metadata"));
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!lua name=d"}), error("ERR Invalid library metadata"));
  const std::string bad_name = "ERR Library names can only contain letters, numbers, or underscores(_) and must be at least one character long";
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!lua name=bad-name!\nx"}), error(bad_name));
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!lua name=\nx"}), error(bad_name));
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!lua name=l x=1\nx"}), error("ERR Invalid metadata value given: x=1"));
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!lua name\nx"}), error("ERR Invalid metadata value given: name"));
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!lua\nx"}), error("ERR Library name was not given"));
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!\nx"}), error("ERR Library name was not given"));
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!py name=l\nx"}), error("ERR Engine 'py' not found"));
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#! name=x\nx"}), error("ERR Engine '' not found"));
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!lua name=d name=e\nx"}), error("ERR Invalid metadata value, name argument was given multiple times"));
  // The engine in any case, spaces anywhere.
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!LUA   name=sp   \nredis.register_function('sp', function() return 1 end)"}), bulk("sp"));
}

TEST(CommandFunctions, LoadTimeErrors) {
  Bed bed;
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!lua name=l\nreturn 1"}), error("ERR No functions registered"));
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!lua name=l\nlocal x = +"}), error("ERR Error compiling function: user_function:2: unexpected symbol near '+'"));
  // At load only register_function, log and the version are to hand.
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!lua name=l\nredis.call('PING')"}),
            error("ERR Error registering functions: ERR user_function:2: Script attempted to access nonexistent global variable 'call'"));
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!lua name=l\nerror('boom')"}),
            error("ERR Error registering functions: ERR user_function:2: Script attempted to access nonexistent global variable 'error'"));
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!lua name=l\nx = 1"}),
            error("ERR Error registering functions: ERR user_function:2: Attempt to modify a readonly table"));
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD", "#!lua name=l\nredis.log(redis.LOG_WARNING, 'x')\nredis.register_function('lg', function() return 1 end)"}),
            bulk("l"));
}

TEST(CommandFunctions, RegisterFunctionArguments) {
  Bed bed;
  const auto load = [&](const std::string& line) { return bed.run({"FUNCTION", "LOAD", "REPLACE", "#!lua name=d\n" + line}); };
  const std::string wrapped = "ERR Error registering functions: ";
  EXPECT_EQ(load("redis.register_function()"), error(wrapped + "ERR wrong number of arguments to server.register_function"));
  EXPECT_EQ(load("redis.register_function('a', function() end, 1)"), error(wrapped + "ERR wrong number of arguments to server.register_function"));
  EXPECT_EQ(load("redis.register_function(5)"),
            error(wrapped + "ERR calling server.register_function with a single argument is only applicable to Lua table (representing named arguments)."));
  EXPECT_EQ(load("redis.register_function({}, 5)"), error(wrapped + "ERR first argument to server.register_function must be a string"));
  EXPECT_EQ(load("redis.register_function('d1', 5)"), error(wrapped + "ERR second argument to server.register_function must be a function"));
  EXPECT_EQ(load("redis.register_function{function_name='d1', callback=5}"),
            error(wrapped + "ERR callback argument given to server.register_function must be a function"));
  EXPECT_EQ(load("redis.register_function{function_name='n'}"), error(wrapped + "ERR server.register_function must get a callback argument"));
  EXPECT_EQ(load("redis.register_function{function_name='n', callback=function() end, wat=1}"),
            error(wrapped + "ERR unknown argument given to server.register_function"));
  EXPECT_EQ(load("redis.register_function{function_name='n', callback=function() end, flags='no-writes'}"),
            error(wrapped + "ERR flags argument to server.register_function must be a table representing function flags"));
  EXPECT_EQ(load("redis.register_function{function_name='x', callback=function() return 1 end, flags={'wat'}}"), error(wrapped + "ERR unknown flag given"));
  EXPECT_EQ(load("redis.register_function('bad name', function() return 1 end)"),
            error("ERR Function names can only contain letters, numbers, or underscores(_) and must be at least one character long"));
  // A number is a name; a description is made a string.
  EXPECT_EQ(load("redis.register_function(1, function() return 1 end)"), bulk("d"));
  EXPECT_EQ(load("redis.register_function{function_name='d1', callback=function() return 1 end, description=5}"), bulk("d"));
}

TEST(CommandFunctions, FcallErrors) {
  Bed bed;
  bed.run({"FUNCTION", "LOAD", library});
  EXPECT_EQ(bed.run({"FCALL", "nosuch", "0"}), error("ERR Function not found"));
  EXPECT_EQ(bed.run({"FCALL", "echo", "x"}), error("ERR Bad number of keys provided"));
  EXPECT_EQ(bed.run({"FCALL", "echo", "-1"}), error("ERR Number of keys can't be negative"));
  EXPECT_EQ(bed.run({"FCALL", "echo", "5", "a"}), error("ERR Number of keys can't be greater than number of args"));
  EXPECT_EQ(bed.run({"FCALL_RO", "put", "1", "k", "v"}), error("ERR Can not execute a script with write flag using *_ro command."));
  bed.run({"FUNCTION", "LOAD",
           "#!lua name=w\nredis.register_function{function_name='w', callback=function(keys) return redis.call('SET', keys[1], 1) end, flags={'no-writes'}}"});
  EXPECT_EQ(bed.run({"FCALL", "w", "1", "k"}), function_error("ERR Write commands are not allowed from read-only scripts.", "w", 2));
  bed.run({"FUNCTION", "LOAD", "#!lua name=h\nredis.register_function('h', function(keys, args) error('inner') end)"});
  EXPECT_EQ(bed.run({"FCALL", "h", "0"}), function_error("ERR user_function:2: inner", "h", 2));
  // Functions take their keys and arguments as parameters, not as globals.
  bed.run({"FUNCTION", "LOAD", "#!lua name=k\nredis.register_function('k', function(keys, args) return KEYS end)"});
  EXPECT_EQ(bed.run({"FCALL", "k", "0"}), function_error("ERR user_function:2: Script attempted to access nonexistent global variable 'KEYS'", "k", 2));
}

TEST(CommandFunctions, HelpersSeeTheRuntimeGlobals) {
  Bed bed;
  EXPECT_EQ(bed.run({"FUNCTION", "LOAD",
                     "#!lua name=h\nlocal function h() return string.upper('a') .. type(redis.call) end\n"
                     "redis.register_function('h', function(keys) return h() .. #keys end)"}),
            bulk("h"));
  EXPECT_EQ(bed.run({"FCALL", "h", "2", "a", "b"}), bulk("Afunction2"));
  // Keeping register_function for later does not let a function register.
  bed.run({"FUNCTION", "LOAD", "#!lua name=r\nlocal reg = redis.register_function\nredis.register_function('r', function() reg('z', function() end) end)"});
  EXPECT_EQ(bed.run({"FCALL", "r", "0"}), function_error("ERR server.register_function can only be called on FUNCTION LOAD command", "r", 3));
}

TEST(CommandFunctions, ListStatsDeleteFlush) {
  Bed bed;
  bed.run(
      {"FUNCTION", "LOAD", "#!lua name=one\nredis.register_function{function_name='f', callback=function() return 1 end, flags={'no-writes', 'allow-oom'}}"});
  const std::string listed =
      array({array({bulk("library_name"), bulk("one"), bulk("engine"), bulk("LUA"), bulk("functions"),
                    array({array({bulk("name"), bulk("f"), bulk("description"), nil(), bulk("flags"), array({"+no-writes\r\n", "+allow-oom\r\n"})})})})});
  EXPECT_EQ(bed.run({"FUNCTION", "LIST"}), listed);
  EXPECT_EQ(bed.run({"FUNCTION", "LIST", "LIBRARYNAME", "o*"}), listed);
  EXPECT_EQ(bed.run({"FUNCTION", "LIST", "LIBRARYNAME", "zz"}), array({}));
  EXPECT_EQ(bed.run({"FUNCTION", "LIST", "LIBRARYNAME"}), error("ERR library name argument was not given"));
  EXPECT_EQ(bed.run({"FUNCTION", "LIST", "WAT"}), error("ERR Unknown argument WAT"));
  const std::string coded = bed.run({"FUNCTION", "LIST", "WITHCODE"});
  EXPECT_NE(coded.find("library_code"), std::string::npos);
  EXPECT_EQ(bed.run({"FUNCTION", "STATS"}), array({bulk("running_script"), nil(), bulk("engines"),
                                                   array({bulk("LUA"), array({bulk("libraries_count"), integer(1), bulk("functions_count"), integer(1)})})}));
  // Libraries outlive SCRIPT FLUSH.
  EXPECT_EQ(bed.run({"SCRIPT", "FLUSH"}), ok());
  EXPECT_EQ(bed.run({"FCALL", "f", "0"}), integer(1));
  EXPECT_EQ(bed.run({"FUNCTION", "DELETE", "one"}), ok());
  EXPECT_EQ(bed.run({"FUNCTION", "DELETE", "one"}), error("ERR Library not found"));
  EXPECT_EQ(bed.run({"FCALL", "f", "0"}), error("ERR Function not found"));
  bed.run({"FUNCTION", "LOAD", library});
  EXPECT_EQ(bed.run({"FUNCTION", "FLUSH", "WAT"}), error("ERR FUNCTION FLUSH only supports SYNC|ASYNC option"));
  EXPECT_EQ(bed.run({"FUNCTION", "FLUSH"}), ok());
  EXPECT_EQ(bed.run({"FCALL", "echo", "0"}), error("ERR Function not found"));
  EXPECT_EQ(bed.run({"FUNCTION", "KILL"}), error("NOTBUSY No scripts in execution right now."));
  EXPECT_EQ(bed.run({"FUNCTION", "WAT"}), error("ERR unknown subcommand 'WAT'. Try FUNCTION HELP."));
}

TEST(CommandFunctions, VersionFields) {
  Bed bed;
  EXPECT_EQ(bed.run({"EVAL", "return {redis.REDIS_VERSION, redis.REDIS_VERSION_NUM, redis.VALKEY_VERSION, redis.VALKEY_VERSION_NUM, redis.SERVER_NAME}", "0"}),
            array({bulk("7.2.4"), integer(459268), bulk("8.1.1"), integer(524545), bulk("mimir")}));
}

}  // namespace
}  // namespace mimir::test
