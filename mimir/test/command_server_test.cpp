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

#include "mimir/test/fixtures/bed.h"

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
  EXPECT_EQ(bed.run({"CONFIG", "GET", "notify-keyspace-events"}), bulks({"notify-keyspace-events", "KEA"}));
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
  EXPECT_NE(info.find(" cmd=client "), std::string::npos) << info;
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

}  // namespace
}  // namespace mimir::test
