//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// ACL, AUTH and the permission checks, from Valkey 8's documented behaviour
// and Valkey 8.1.1's own answers (probed), with rulings 27 and 28.

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

#include "dannan/test/fixtures/stepped_time.h"
#include "mimir/command/acl.h"
#include "mimir/command/database.h"
#include "mimir/command/table.h"
#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

using Args = std::vector<std::string>;

// A node whose AUTH is taken on plaintext connections, as the differential
// runs it, with sessions made on it.
struct AclBed : Bed {
  explicit AclBed(command::DatabaseSettings settings = plaintext()) : Bed(std::move(settings)) {}

  static command::DatabaseSettings plaintext() {
    command::DatabaseSettings s;
    s.auth_plaintext = true;
    return s;
  }

  // Welcomed by its first command, where it then lives: the database keeps
  // its address, so a session is never welcomed and then moved.
  command::Session make() { return make_session(); }
};

// ACL LIST's line for one user.
std::string list_line(AclBed& bed, const std::string& name) {
  const std::string all = bed.run({"ACL", "LIST"});
  const std::string marker = "user " + name + " ";
  const size_t at = all.find(marker);
  if (at == std::string::npos) return {};
  const size_t end = all.find("\r\n", at);
  return all.substr(at, end - at);
}

TEST(Acl, TheDefaultUserMayDoEverything) {
  AclBed bed;
  EXPECT_EQ(bed.run({"ACL", "LIST"}), array({bulk("user default on nopass sanitize-payload ~* &* +@all")}));
  EXPECT_EQ(bed.run({"ACL", "WHOAMI"}), bulk("default"));
  EXPECT_EQ(bed.run({"ACL", "USERS"}), array({bulk("default")}));
}

TEST(Acl, ANewUserIsOffWithNothing) {
  AclBed bed;
  EXPECT_EQ(bed.run({"ACL", "SETUSER", "u"}), ok());
  EXPECT_EQ(list_line(bed, "u"), "user u off sanitize-payload resetchannels -@all");
  EXPECT_EQ(bed.run({"ACL", "GETUSER", "u"}),
            array({bulk("flags"), array({bulk("off"), bulk("sanitize-payload")}), bulk("passwords"), array({}), bulk("commands"), bulk("-@all"), bulk("keys"),
                   bulk(""), bulk("channels"), bulk(""), bulk("selectors"), array({})}));
  EXPECT_EQ(bed.run({"ACL", "GETUSER", "nosuch"}), nil());
}

TEST(Acl, RulesAreKeptAsValkeyDescribesThem) {
  AclBed bed;
  ASSERT_EQ(bed.run({"ACL",
                     "SETUSER",
                     "u",
                     "on",
                     ">p1",
                     ">p1",
                     "#aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                     "~a*",
                     "%R~b",
                     "%W~c",
                     "%RW~d",
                     "~b",
                     "&ch*",
                     "+get",
                     "+@read",
                     "-@read",
                     "+client|list",
                     "+client",
                     "-client|kill",
                     "+select|0",
                     "+@all",
                     "-get",
                     "+get"}),
            ok());
  EXPECT_EQ(list_line(bed, "u"),
            "user u on sanitize-payload #f64551fcd6f07823cb87971cfb91446425da18286b3ab1ef935e0cbd7a69f68a "
            "#aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa ~a* ~b %W~c ~d resetchannels &ch* +@all +get");
}

TEST(Acl, LaterRulesReplaceEarlierOnesForTheSameCommand) {
  AclBed bed;
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "p1", "%R"}), ok());
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "p2", "%RW~*"}), ok());
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "p3", "%R~k", "%W~k", "~j", "%R~j"}), ok());
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "p4", ">a", "nopass", ">b"}), ok());
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "p5", ">a", ">b", "<a"}), ok());
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "p6", "+select|0", "+select|1", "-select", "+client|list", "-client"}), ok());
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "p7", "+@read", "+get", "-get", "-@read"}), ok());
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "p8", "on", "skip-sanitize-payload", "&a", "&a", "&b", "resetchannels", "&c"}), ok());
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "p9", "+client", "-client|kill", "+client|kill"}), ok());
  const std::string b = "#3e23e8160039594a33894f6564e1b1348bbd7a0088d42c4acb73eeaed59c009d";
  EXPECT_EQ(list_line(bed, "p1"), "user p1 off sanitize-payload %R~ resetchannels -@all");
  EXPECT_EQ(list_line(bed, "p2"), "user p2 off sanitize-payload ~* resetchannels -@all");
  EXPECT_EQ(list_line(bed, "p3"), "user p3 off sanitize-payload ~k ~j resetchannels -@all");
  EXPECT_EQ(list_line(bed, "p4"), "user p4 off sanitize-payload " + b + " resetchannels -@all");
  EXPECT_EQ(list_line(bed, "p5"), "user p5 off sanitize-payload " + b + " resetchannels -@all");
  EXPECT_EQ(list_line(bed, "p6"), "user p6 off sanitize-payload resetchannels -@all -select -client");
  EXPECT_EQ(list_line(bed, "p7"), "user p7 off sanitize-payload resetchannels -@all -get -@read");
  EXPECT_EQ(list_line(bed, "p8"), "user p8 on skip-sanitize-payload resetchannels &c -@all");
  EXPECT_EQ(list_line(bed, "p9"), "user p9 off sanitize-payload resetchannels -@all +client +client|kill");
}

TEST(Acl, SelectorsAreDescribedInParentheses) {
  AclBed bed;
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "v", "(~x", "+get)", "(&c", "-@all", "+set)", "+@read", "+@write", "-@write", "+GET", "allkeys", "allchannels"}), ok());
  EXPECT_EQ(list_line(bed, "v"), "user v off sanitize-payload ~* &* -@all +@read -@write +get (~x resetchannels -@all +get) (resetchannels &c -@all +set)");
  EXPECT_EQ(bed.run({"ACL", "GETUSER", "v"}),
            array({bulk("flags"), array({bulk("off"), bulk("sanitize-payload")}), bulk("passwords"), array({}), bulk("commands"),
                   bulk("-@all +@read -@write +get"), bulk("keys"), bulk("~*"), bulk("channels"), bulk("&*"), bulk("selectors"),
                   array({array({bulk("commands"), bulk("-@all +get"), bulk("keys"), bulk("~x"), bulk("channels"), bulk("")}),
                          array({bulk("commands"), bulk("-@all +set"), bulk("keys"), bulk(""), bulk("channels"), bulk("&c")})})}));
  EXPECT_EQ(bed.run({"ACL", "SETUSER", "v", "clearselectors", "reset"}), ok());
  EXPECT_EQ(list_line(bed, "v"), "user v off sanitize-payload resetchannels -@all");
}

TEST(Acl, RefusedRulesChangeNothing) {
  AclBed bed;
  const auto refused = [&](const std::string& rule, const std::string& why) {
    EXPECT_EQ(bed.run({"ACL", "SETUSER", "v", rule}), error("ERR Error in ACL SETUSER modifier '" + rule + "': " + why)) << rule;
  };
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "v", "~*", "&*"}), ok());
  refused("~x",
          "Adding a pattern after the * pattern (or the 'allkeys' flag) is not valid and does not have any effect. Try 'resetkeys' to start with an empty "
          "list of patterns");
  refused("&x",
          "Adding a pattern after the * pattern (or the 'allchannels' flag) is not valid and does not have any effect. Try 'resetchannels' to start with "
          "an empty list of channels");
  // A pattern after allkeys is refused before its flags are read.
  refused("%~x",
          "Adding a pattern after the * pattern (or the 'allkeys' flag) is not valid and does not have any effect. Try 'resetkeys' to start with an empty "
          "list of patterns");
  EXPECT_EQ(bed.run({"ACL", "SETUSER", "z", "%~x"}), error("ERR Error in ACL SETUSER modifier '%~x': Syntax error"));
  EXPECT_EQ(bed.run({"ACL", "SETUSER", "z", "%X~x"}), error("ERR Error in ACL SETUSER modifier '%X~x': Syntax error"));
  refused("+nosuch", "Unknown command or category name in ACL");
  refused("+client|nosuch", "Unknown command or category name in ACL");
  refused("-select|0", "Unknown command or category name in ACL");
  refused("+select|", "Syntax error");
  refused("+client|list|x", "Allowing first-arg of a subcommand is not supported");
  refused("+@nosuch", "Unknown command or category name in ACL");
  refused("<nope", "The password you are trying to remove from the user does not exist");
  refused("!nope", "The password hash must be exactly 64 characters and contain only lowercase hexadecimal characters");
  refused("#abc", "The password hash must be exactly 64 characters and contain only lowercase hexadecimal characters");
  refused("bogus", "Syntax error");
  EXPECT_EQ(bed.run({"ACL", "SETUSER", "a b"}), error("ERR Usernames can't contain spaces or null characters"));
  EXPECT_EQ(bed.run({"ACL", "SETUSER", "v", "(~x +get", "foo"}), error("ERR Unmatched parenthesis in acl selector starting at '(~x +get'."));
  // All or none: the rules before a refused one are not kept either.
  EXPECT_EQ(bed.run({"ACL", "SETUSER", "w", "on", "bogus"}), error("ERR Error in ACL SETUSER modifier 'bogus': Syntax error"));
  EXPECT_EQ(bed.run({"ACL", "GETUSER", "w"}), nil());
  EXPECT_EQ(list_line(bed, "v"), "user v off sanitize-payload ~* &* -@all");
}

TEST(Acl, DryRunSaysWhatACommandWouldMeet) {
  AclBed bed;
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "w", "on", "nopass", "~k*", "&c1", "-@all", "+get", "+set", "+publish", "+subscribe", "+psubscribe"}), ok());
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "r", "on", "nopass", "%R~*", "+@all"}), ok());
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "wo", "on", "nopass", "%W~*", "+@all"}), ok());
  const auto dry = [&](Args args) {
    Args full{"ACL", "DRYRUN"};
    full.insert(full.end(), args.begin(), args.end());
    return bed.run(full);
  };
  EXPECT_EQ(dry({"w", "get", "k1"}), ok());
  EXPECT_EQ(dry({"w", "get", "x1"}), bulk("User w has no permissions to access the 'x1' key"));
  EXPECT_EQ(dry({"w", "del", "k1"}), bulk("User w has no permissions to run the 'del' command"));
  EXPECT_EQ(dry({"w", "publish", "c2", "m"}), bulk("User w has no permissions to access the 'c2' channel"));
  EXPECT_EQ(dry({"w", "publish", "c1", "m"}), ok());
  // A pattern subscription needs the very pattern, not one it matches.
  EXPECT_EQ(dry({"w", "psubscribe", "c*"}), bulk("User w has no permissions to access the 'c*' channel"));
  EXPECT_EQ(dry({"nosuch", "get", "k"}), error("ERR User 'nosuch' not found"));
  EXPECT_EQ(dry({"w", "nosuch", "k"}), error("ERR Command 'nosuch' not found"));
  EXPECT_EQ(dry({"w", "get"}), error("ERR wrong number of arguments for 'get' command"));
  EXPECT_EQ(dry({"w", "client", "nosuch"}), error("ERR Command 'client' not found"));
  EXPECT_EQ(dry({"w", "client", "list"}), bulk("User w has no permissions to run the 'client|list' command"));
  // Keys are checked by what each key spec does to its key.
  EXPECT_EQ(dry({"wo", "set", "k", "v"}), ok());
  EXPECT_EQ(dry({"wo", "set", "k", "v", "GET"}), bulk("User wo has no permissions to access the 'k' key"));
  EXPECT_EQ(dry({"wo", "incr", "k"}), bulk("User wo has no permissions to access the 'k' key"));
  EXPECT_EQ(dry({"wo", "lpush", "k", "v"}), ok());
  EXPECT_EQ(dry({"wo", "copy", "a", "b"}), bulk("User wo has no permissions to access the 'a' key"));
  EXPECT_EQ(dry({"r", "copy", "a", "b"}), bulk("User r has no permissions to access the 'b' key"));
  EXPECT_EQ(dry({"wo", "del", "k"}), ok());
  EXPECT_EQ(dry({"r", "get", "k"}), ok());
  EXPECT_EQ(dry({"r", "exists", "k"}), ok());
  EXPECT_EQ(dry({"wo", "exists", "k"}), ok());
  EXPECT_EQ(dry({"r", "sort", "k", "STORE", "d"}), bulk("User r has no permissions to access the 'd' key"));
  EXPECT_EQ(dry({"r", "bitfield", "k", "GET", "u8", "0"}), ok());
  EXPECT_EQ(dry({"r", "bitfield", "k", "SET", "u8", "0", "1"}), bulk("User r has no permissions to access the 'k' key"));
  EXPECT_EQ(dry({"r", "xread", "STREAMS", "a", "b", "0", "0"}), ok());
  EXPECT_EQ(dry({"wo", "xread", "STREAMS", "a", "b", "0", "0"}), bulk("User wo has no permissions to access the 'a' key"));
}

TEST(Acl, AFirstArgumentMayBeAllowedAlone) {
  AclBed bed;
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "s", "on", "nopass", "+select|0"}), ok());
  EXPECT_EQ(bed.run({"ACL", "DRYRUN", "s", "select", "0"}), ok());
  EXPECT_EQ(bed.run({"ACL", "DRYRUN", "s", "select", "1"}), bulk("User s has no permissions to run the 'select' command"));
}

TEST(Acl, RefusalsAreAnsweredNoPerm) {
  AclBed bed;
  ASSERT_EQ(
      bed.run({"ACL", "SETUSER", "w", "on", ">pw", "~k*", "&c1", "-@all", "+get", "+set", "+publish", "+subscribe", "+multi", "+exec", "+eval", "+acl|whoami"}),
      ok());
  ASSERT_EQ(bed.run({"AUTH", "w", "pw"}), ok());
  EXPECT_EQ(bed.run({"ACL", "WHOAMI"}), bulk("w"));
  EXPECT_EQ(bed.run({"GET", "x1"}), error("NOPERM No permissions to access a key"));
  EXPECT_EQ(bed.run({"DEL", "k1"}), error("NOPERM User w has no permissions to run the 'del' command"));
  EXPECT_EQ(bed.run({"PUBLISH", "c2", "m"}), error("NOPERM No permissions to access a channel"));
  EXPECT_EQ(bed.run({"SUBSCRIBE", "c1", "c2"}), error("NOPERM No permissions to access a channel"));
  EXPECT_EQ(bed.run({"SET", "k1", "v"}), ok());
  EXPECT_EQ(bed.run({"GET", "k1"}), bulk("v"));
  // A container's subcommands are allowed one by one.
  EXPECT_EQ(bed.run({"ACL", "SETUSER", "w", "-@all"}), error("NOPERM User w has no permissions to run the 'acl|setuser' command"));
}

TEST(Acl, ARefusalWhileQueueingAbortsTheTransaction) {
  AclBed bed;
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "w", "on", ">pw", "~k*", "-@all", "+get", "+multi", "+exec"}), ok());
  ASSERT_EQ(bed.run({"AUTH", "w", "pw"}), ok());
  EXPECT_EQ(bed.run({"MULTI"}), ok());
  EXPECT_EQ(bed.run({"GET", "x1"}), error("NOPERM No permissions to access a key"));
  EXPECT_EQ(bed.run({"GET", "k1"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"EXEC"}), error("EXECABORT Transaction discarded because of previous errors."));
}

TEST(Acl, ExecChecksAgainEachQueuedCommand) {
  AclBed bed;
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "x", "on", "nopass", "~*", "+@all"}), ok());
  command::Session other = bed.make();
  ASSERT_EQ(bed.run_as(other, {"AUTH", "x", "any"}), ok());
  EXPECT_EQ(bed.run_as(other, {"MULTI"}), ok());
  EXPECT_EQ(bed.run_as(other, {"SET", "k", "v"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run_as(other, {"PING"}), "+QUEUED\r\n");
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "x", "-set"}), ok());
  EXPECT_EQ(bed.run_as(other, {"EXEC"}),
            array({error("NOPERM ACLs rules changed between the moment the transaction was accumulated and the EXEC call. This command is no longer "
                         "allowed for the following reason: no permission to execute the command or subcommand"),
                   "+PONG\r\n"}));
  EXPECT_EQ(bed.run({"EXISTS", "k"}), integer(0));
}

TEST(Acl, ScriptsRunAsTheirCaller) {
  AclBed bed;
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "w", "on", ">pw", "~k*", "-@all", "+get", "+eval"}), ok());
  ASSERT_EQ(bed.run({"AUTH", "w", "pw"}), ok());
  const std::string key = bed.run({"EVAL", "return redis.call('get','x1')", "0"});
  EXPECT_EQ(key.rfind("-ERR ACL failure in script: No permissions to access a key script: ", 0), 0u) << key;
  const std::string command = bed.run({"EVAL", "return redis.call('del','k1')", "0"});
  EXPECT_EQ(command.rfind("-ERR ACL failure in script: User w has no permissions to run the 'del' command script: ", 0), 0u) << command;
  EXPECT_EQ(bed.run({"EVAL", "return redis.pcall('del','k1')", "0"}), error("ERR ACL failure in script: User w has no permissions to run the 'del' command"));
  // EVAL's own keys are checked as EVAL's: read and write.
  EXPECT_EQ(bed.run({"EVAL", "return 1", "1", "x1"}), error("NOPERM No permissions to access a key"));
  EXPECT_EQ(bed.run({"EVAL", "return redis.call('get','k1')", "1", "k1"}), nil());
}

TEST(Acl, APasswordForTheDefaultUserRequiresAuthentication) {
  AclBed bed;
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "default", ">dpw"}), ok());
  command::Session fresh = bed.make();
  // Unknown commands and wrong arities are answered first, as Valkey does.
  EXPECT_EQ(bed.run_as(fresh, {"ECHO"}), error("ERR wrong number of arguments for 'echo' command"));
  EXPECT_EQ(bed.run_as(fresh, {"GET", "x"}), error("NOAUTH Authentication required."));
  EXPECT_EQ(bed.run_as(fresh, {"PING"}), error("NOAUTH Authentication required."));
  const std::string hello =
      "NOAUTH HELLO must be called with the client already authenticated, otherwise the HELLO <proto> AUTH <user> <pass> option can be used to "
      "authenticate the client and select the RESP protocol version at the same time";
  EXPECT_EQ(bed.run_as(fresh, {"HELLO", "3"}), error(hello));
  EXPECT_EQ(bed.run_as(fresh, {"HELLO", "3", "SETNAME", "x"}), error(hello));
  EXPECT_EQ(bed.run_as(fresh, {"AUTH", "x"}), error(command::wrong_pass));
  EXPECT_EQ(bed.run_as(fresh, {"MULTI"}), error("NOAUTH Authentication required."));
  EXPECT_EQ(bed.run_as(fresh, {"AUTH", "dpw"}), ok());
  EXPECT_EQ(bed.run_as(fresh, {"GET", "x"}), nil());
  // The connection that was in before the password stays in.
  EXPECT_EQ(bed.run({"GET", "x"}), nil());
  // RESET makes a connection the default user again, to authenticate anew.
  EXPECT_EQ(bed.run_as(fresh, {"RESET"}), "+RESET\r\n");
  EXPECT_EQ(bed.run_as(fresh, {"GET", "x"}), error("NOAUTH Authentication required."));
  command::Session third = bed.make();
  const std::string answer = bed.run_as(third, {"HELLO", "2", "AUTH", "default", "dpw"});
  EXPECT_EQ(answer.rfind("*14\r\n", 0), 0u) << answer;
}

TEST(Acl, AuthAsValkeyAnswersIt) {
  AclBed bed;
  EXPECT_EQ(bed.run({"AUTH", "a", "b", "c"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"AUTH", "x"}),
            error("ERR AUTH <password> called without any password configured for the default user. Are you sure your configuration is correct?"));
  EXPECT_EQ(bed.run({"AUTH", "default", "x"}), ok());
  EXPECT_EQ(bed.run({"AUTH", "nosuch", "x"}), error(command::wrong_pass));
  const std::string hello = bed.run({"HELLO", "3", "AUTH", "default", "x"});
  EXPECT_EQ(hello.rfind("%7\r\n", 0), 0u) << hello;
  EXPECT_EQ(bed.run({"HELLO", "3", "AUTH", "nosuch", "x"}), "-" + std::string(command::wrong_pass) + "\r\n");
  // A disabled user cannot authenticate, whatever its password.
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "off", "off", "nopass"}), ok());
  EXPECT_EQ(bed.run({"AUTH", "off", "x"}), "-" + std::string(command::wrong_pass) + "\r\n");
}

TEST(Acl, PlaintextAuthIsRefusedUnlessEnabled) {
  AclBed bed{command::DatabaseSettings{}};
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "u", "on", ">pw", "+@all", "~*"}), ok());
  const std::string refusal = "ERR AUTH needs a TLS connection on this server: a password is never taken in clear (auth.plaintext is off)";
  EXPECT_EQ(bed.run({"AUTH", "u", "pw"}), error(refusal));
  EXPECT_EQ(bed.run({"AUTH", "pw"}), error(refusal));
  EXPECT_EQ(bed.run({"HELLO", "3", "AUTH", "u", "pw"}), error(refusal));
  EXPECT_EQ(bed.run({"ACL", "WHOAMI"}), bulk("default"));
  command::Session secure = bed.make();
  secure.tls = true;
  EXPECT_EQ(bed.run_as(secure, {"AUTH", "u", "pw"}), ok());
  EXPECT_EQ(bed.run_as(secure, {"ACL", "WHOAMI"}), bulk("u"));
}

TEST(Acl, TheSubcommandsAnswerAsValkeys) {
  AclBed bed;
  EXPECT_EQ(bed.run({"ACL", "CAT", "nosuch"}), error("ERR Unknown category 'nosuch'"));
  const std::string cats = bed.run({"ACL", "CAT"});
  EXPECT_EQ(cats.rfind("*21\r\n$8\r\nkeyspace\r\n$4\r\nread\r\n", 0), 0u);
  EXPECT_NE(cats.find("$9\r\nscripting\r\n"), std::string::npos);
  const std::string transaction = bed.run({"ACL", "CAT", "transaction"});
  EXPECT_EQ(transaction.rfind("*5\r\n", 0), 0u) << transaction;
  for (const char* name : {"discard", "multi", "exec", "unwatch", "watch"}) EXPECT_NE(transaction.find(bulk(name)), std::string::npos) << name;
  EXPECT_EQ(bed.run({"ACL", "DELUSER", "default"}), error("ERR The 'default' user cannot be removed"));
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "u"}), ok());
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "v"}), ok());
  EXPECT_EQ(bed.run({"ACL", "USERS"}), array({bulk("default"), bulk("u"), bulk("v")}));
  EXPECT_EQ(bed.run({"ACL", "DELUSER", "nosuch", "u", "v"}), integer(2));
  EXPECT_EQ(bed.run({"ACL", "GENPASS"}).size(), std::string("$64\r\n\r\n").size() + 64);
  EXPECT_EQ(bed.run({"ACL", "GENPASS", "5"}).size(), std::string("$2\r\n\r\n").size() + 2);
  const std::string bits = "ERR ACL GENPASS argument must be the number of bits for the output password, a positive number up to 4096";
  EXPECT_EQ(bed.run({"ACL", "GENPASS", "0"}), error(bits));
  EXPECT_EQ(bed.run({"ACL", "GENPASS", "4097"}), error(bits));
  EXPECT_EQ(bed.run({"ACL", "nosuch"}), error("ERR unknown subcommand 'nosuch'. Try ACL HELP."));
  EXPECT_EQ(bed.run({"ACL", "LOG"}), array({}));
  EXPECT_EQ(bed.run({"ACL", "LOG", "RESET"}), ok());
  EXPECT_EQ(bed.run({"ACL", "LOG", "x"}), error("ERR value is not an integer or out of range"));
  const std::string no_file =
      "ERR This instance is not configured to use an ACL file. You may want to specify users via the ACL SETUSER command and then issue a CONFIG "
      "REWRITE (assuming you have a configuration file set) in order to store users in the configuration.";
  EXPECT_EQ(bed.run({"ACL", "LOAD"}), error(no_file));
  EXPECT_EQ(bed.run({"ACL", "SAVE"}), error(no_file));
  const std::string help = bed.run({"ACL", "HELP"});
  EXPECT_EQ(help.rfind("*29\r\n+ACL <subcommand> [<arg> [value] [opt] ...]. Subcommands are:\r\n", 0), 0u) << help;
}

TEST(Acl, TheLogCountsRepeatedRefusals) {
  AclBed bed;
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "w", "on", ">pw", "~k*", "-@all", "+get"}), ok());
  command::Session other = bed.make();
  ASSERT_EQ(bed.run_as(other, {"AUTH", "w", "pw"}), ok());
  bed.run_as(other, {"GET", "x1"});
  bed.wall.advance(std::chrono::milliseconds(10));
  bed.run_as(other, {"GET", "x1"});
  bed.run_as(other, {"DEL", "k1"});
  bed.run({"AUTH", "nosuch", "x"});
  const std::string log = bed.run({"ACL", "LOG"});
  ASSERT_EQ(log.rfind("*3\r\n", 0), 0u) << log;
  // Newest first: the AUTH, the DEL, then the key refused twice.
  const size_t auth = log.find("$4\r\nauth\r\n");
  const size_t del = log.find("$3\r\ndel\r\n");
  const size_t key = log.find("$2\r\nx1\r\n");
  ASSERT_NE(auth, std::string::npos);
  ASSERT_NE(del, std::string::npos);
  ASSERT_NE(key, std::string::npos);
  EXPECT_LT(auth, del);
  EXPECT_LT(del, key);
  EXPECT_NE(log.find("$5\r\ncount\r\n:2\r\n$6\r\nreason\r\n$3\r\nkey\r\n$7\r\ncontext\r\n$8\r\ntoplevel\r\n"), std::string::npos) << log;
  EXPECT_NE(log.find("user=w"), std::string::npos);
  EXPECT_EQ(bed.run({"ACL", "LOG", "1"}).rfind("*1\r\n", 0), 0u);
}

TEST(Acl, DeletingAUserClosesItsConnections) {
  AclBed bed;
  ASSERT_EQ(bed.run({"ACL", "SETUSER", "u", "on", "nopass", "+@all", "~*"}), ok());
  command::Session theirs = bed.make();
  bool killed = false;
  theirs.kill = [&] { killed = true; };
  ASSERT_EQ(bed.run_as(theirs, {"AUTH", "u", "x"}), ok());
  ASSERT_EQ(bed.run_as(theirs, {"ACL", "WHOAMI"}), bulk("u"));
  EXPECT_EQ(bed.run({"ACL", "DELUSER", "u"}), integer(1));
  EXPECT_TRUE(killed);
  EXPECT_EQ(bed.run_as(theirs, {"GET", "k"}), error("NOPERM User u has no permissions to run the 'get' command"));
}

TEST(Acl, UsersAreSeededFromTheSettings) {
  command::DatabaseSettings settings = AclBed::plaintext();
  settings.users = {"user alice on >secret ~app:* +@read", "user default off"};
  AclBed bed{settings};
  command::Session fresh = bed.make();
  EXPECT_EQ(bed.run_as(fresh, {"GET", "app:x"}), error("NOAUTH Authentication required."));
  EXPECT_EQ(bed.run_as(fresh, {"AUTH", "alice", "secret"}), ok());
  EXPECT_EQ(bed.run_as(fresh, {"GET", "app:x"}), nil());
  EXPECT_EQ(bed.run_as(fresh, {"SET", "app:x", "1"}), error("NOPERM User alice has no permissions to run the 'set' command"));
  command::DatabaseSettings bad;
  bad.users = {"user bob bogus"};
  EXPECT_THROW({ Bed refused(bad); }, std::invalid_argument);
}

TEST(Acl, EveryCommandHasCategories) {
  for (const command::Spec& spec : command::all_specs()) EXPECT_FALSE(command::Acl::categories_of(spec.name).empty()) << spec.name;
  AclBed bed;
  EXPECT_NE(bed.run({"COMMAND", "INFO", "get"}).find("+@read\r\n+@string\r\n+@fast\r\n"), std::string::npos);
}

}  // namespace
}  // namespace mimir::test
