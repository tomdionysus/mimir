//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include <gtest/gtest.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dannan/contract/time_source.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "mimir/command/agreements.h"
#include "mimir/command/database.h"

namespace mimir::command {
namespace {

// The cluster as one register store on one io context: a change runs on the
// record held and commits, or, told to, is applied and then reported unknown
// (its answers lost), as a partial accept is.
class Registers final : public Agreements {
 public:
  explicit Registers(boost::asio::io_context& io) : _io(io) {}

  std::string operation_id() override { return "op-" + std::to_string(_next++); }

  void change(size_t db, const std::string& key, Change change, std::function<void(Outcome)> done) override {
    // The cluster's own registers (users, libraries, scripts) are counted
    // apart from keys'.
    ++(db == 0xffff ? system_proposals : proposals);
    boost::asio::post(_io, [this, db, key, change = std::move(change), done = std::move(done)] {
      auto& held = records[{db, key}];
      const auto changed = change(held);
      held = changed.record;
      Outcome outcome;
      if (lose_next > 0) {
        --lose_next;
        outcome.unknown = true;
        outcome.why = "the answers were lost";
      } else if (refuse_next > 0) {
        --refuse_next;
        held = {};
        outcome.why = "no majority of the key's replicas answered";
      } else {
        outcome.committed = true;
        outcome.record = changed.record;
        outcome.result = changed.result;
        outcome.version = version(++_commits);
      }
      boost::asio::post(_io, [done, outcome] { done(outcome); });
    });
  }

  void on_committed(Committed told) override { committed = std::move(told); }

  void read(size_t db, const std::string& key, std::function<void(Outcome)> done) override {
    ++reads;
    boost::asio::post(_io, [this, db, key, done = std::move(done)] {
      Outcome outcome;
      if (refuse_reads > 0) {
        --refuse_reads;
        outcome.why = "no majority of the key's replicas answered";
        return done(outcome);
      }
      outcome.committed = true;
      outcome.record = records[{db, key}];
      // A read commits too, above every write before it.
      outcome.version = version(++_commits);
      done(outcome);
    });
  }
  int reads = 0;
  int refuse_reads = 0;

  // Versions order as commits do: a fixed-width count.
  static std::string version(uint64_t n) {
    std::string out(8, '\0');
    for (int i = 7; i >= 0; --i, n >>= 8) out[static_cast<size_t>(i)] = static_cast<char>(n & 0xff);
    return out;
  }
  // Another node's commit, told as its notice would.
  void commit_elsewhere(size_t db, const std::string& key, cluster::Record committed_record) {
    auto& held = records[{db, key}];
    committed_record.serial = held.serial + 1;
    held = committed_record;
    committed(db, key, version(++_commits), committed_record);
  }
  uint64_t commits() const { return _commits; }

  cluster::Record& record(size_t db, const std::string& key) { return records[std::make_pair(db, key)]; }
  Committed committed;

  std::map<std::pair<size_t, std::string>, cluster::Record> records;
  int lose_next = 0;
  int refuse_next = 0;
  int proposals = 0;
  int system_proposals = 0;

 private:
  boost::asio::io_context& _io;
  uint64_t _next = 1;
  uint64_t _commits = 0;
};

class AgreementBed {
 public:
  // On the system's wall clock, or on stepped wall time.
  explicit AgreementBed(bool stepped = false) {
    registers = std::make_shared<Registers>(io);
    DatabaseSettings settings;
    settings.databases = 2;
    settings.agreements = registers;
    database = std::make_unique<Database>(stepped ? static_cast<const dannan::contract::WallClock&>(wall) : dannan::contract::system_wall_clock(), settings);
    session.id = database->next_client_id();
  }

  // Everything proposed meanwhile, decided.
  void settle() {
    io.restart();
    io.run();
  }

  dannan::test::SteppedWall wall;

  // The command's reply, waiting for its agreement if it has one.
  // A postponed command woken with no bytes is run again, as the connection
  // runs it.
  std::string run(std::vector<std::string> args) {
    for (;;) {
      std::string out;
      std::optional<std::string> woken;
      const auto executed = database->execute(session, args, out, [&woken](std::string reply) { woken = std::move(reply); });
      if (!executed.wait) return out;
      io.restart();
      while (!woken && io.run_one() > 0) {
      }
      if (executed.postponed && woken && woken->empty()) continue;
      return woken.value_or("<no reply>");
    }
  }

  boost::asio::io_context io;
  std::shared_ptr<Registers> registers;
  std::unique_ptr<Database> database;
  Session session;
};

TEST(CommandAgreement, AWriteCommitsByAgreementAndIsReadHere) {
  AgreementBed bed;
  EXPECT_EQ(bed.run({"SET", "k", "v"}), "+OK\r\n");
  EXPECT_EQ(bed.registers->proposals, 1);
  const auto& record = bed.registers->record(0, "k");
  ASSERT_TRUE(record.payload);
  ASSERT_EQ(record.applied.size(), 1u);
  // The committed value is this node's view: a read sees it at once.
  EXPECT_EQ(bed.run({"GET", "k"}), "$1\r\nv\r\n");
  EXPECT_EQ(bed.registers->proposals, 1);
}

TEST(CommandAgreement, AWriteRunsOnTheValueTheClusterHolds) {
  AgreementBed bed;
  EXPECT_EQ(bed.run({"INCR", "n"}), ":1\r\n");
  // Another node's write, committed and not yet seen here.
  auto& record = bed.registers->record(0, "n");
  Database other(dannan::contract::system_wall_clock());
  Session session;
  std::string out;
  other.execute(session, {"SET", "n", "41"}, out);
  out.clear();
  other.execute(session, {"DUMP", "n"}, out);
  // The DUMP payload is the bulk string's body.
  const auto start = out.find("\r\n") + 2;
  record.payload = out.substr(start, out.size() - start - 2);
  EXPECT_EQ(bed.run({"INCR", "n"}), ":42\r\n");
  EXPECT_EQ(bed.run({"GET", "n"}), "$2\r\n42\r\n");
}

TEST(CommandAgreement, ARetryAfterLostAnswersAppliesTheWriteOnce) {
  AgreementBed bed;
  bed.registers->lose_next = 2;
  EXPECT_EQ(bed.run({"INCR", "n"}), ":1\r\n");
  EXPECT_EQ(bed.registers->proposals, 3);
  EXPECT_EQ(bed.run({"GET", "n"}), "$1\r\n1\r\n");
  EXPECT_EQ(bed.registers->record(0, "n").applied.size(), 1u);
}

TEST(CommandAgreement, AWriteTheClusterCannotDecideIsRefused) {
  AgreementBed bed;
  bed.registers->refuse_next = 1;
  EXPECT_EQ(bed.run({"SET", "k", "v"}), "-CLUSTERDOWN no majority of the key's replicas answered\r\n");
  EXPECT_EQ(bed.run({"GET", "k"}), "$-1\r\n");
}

TEST(CommandAgreement, ErrorsAndExpiriesCommitAsTheyWouldRunHere) {
  AgreementBed bed;
  EXPECT_EQ(bed.run({"LPUSH", "l", "a"}), ":1\r\n");
  EXPECT_EQ(bed.run({"INCR", "l"}), "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n");
  EXPECT_EQ(bed.run({"SET", "t", "v", "EX", "100"}), "+OK\r\n");
  const auto ttl = bed.run({"TTL", "t"});
  EXPECT_TRUE(ttl == ":100\r\n" || ttl == ":99\r\n") << ttl;
  ASSERT_TRUE(bed.registers->record(0, "t").expires_at);
  EXPECT_EQ(bed.run({"DEL", "t"}), ":1\r\n");
  EXPECT_FALSE(bed.registers->record(0, "t").payload);
  EXPECT_EQ(bed.run({"EXISTS", "t"}), ":0\r\n");
}

TEST(CommandAgreement, EachDatabaseHasItsOwnRegisters) {
  AgreementBed bed;
  EXPECT_EQ(bed.run({"SET", "k", "zero"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"SELECT", "1"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"SET", "k", "one"}), "+OK\r\n");
  EXPECT_EQ(bed.registers->records.size(), 2u);
  EXPECT_EQ(bed.run({"GET", "k"}), "$3\r\none\r\n");
}

TEST(CommandAgreement, WritesOfSeveralKeysAreRefusedUntilAtomicCommit) {
  AgreementBed bed;
  const std::string several = "-ERR this command writes more than one key, which Mimir refuses until Dannan 0.10.0's atomic commit (ruling 35)\r\n";
  EXPECT_EQ(bed.run({"MSET", "a", "1", "b", "2"}), several);
  EXPECT_EQ(bed.run({"RENAME", "a", "b"}), several);
  EXPECT_EQ(bed.run({"FLUSHALL"}), several);
  EXPECT_EQ(bed.run({"MOVE", "a", "1"}), several);
  EXPECT_EQ(bed.registers->proposals, 0);
  // Several keys read together are served here.
  EXPECT_EQ(bed.run({"MGET", "a", "b"}), "*2\r\n$-1\r\n$-1\r\n");
}

TEST(CommandAgreement, AScriptCommitsByAgreementOnItsOneDeclaredKey) {
  AgreementBed bed;
  EXPECT_EQ(bed.run({"EVAL", "redis.call('incr', KEYS[1]); return redis.call('incr', KEYS[1])", "1", "n"}), ":2\r\n");
  EXPECT_EQ(bed.registers->proposals, 1);
  EXPECT_EQ(bed.run({"GET", "n"}), "$1\r\n2\r\n");
  // Another key, read or written, is refused inside it.
  const auto other = bed.run({"EVAL", "return redis.call('set', 'other', 1)", "1", "n"});
  EXPECT_NE(other.find("names another: 'other'"), std::string::npos) << other;
  EXPECT_FALSE(bed.registers->record(0, "other").payload);
  // Several keys are several keys.
  EXPECT_EQ(bed.run({"EVAL", "return 1", "2", "a", "b"}),
            "-ERR this command writes more than one key, which Mimir refuses until Dannan 0.10.0's atomic commit (ruling 35)\r\n");
}

TEST(CommandAgreement, AScriptThatDeclaresNoKeyRunsHereAndOnlyReads) {
  AgreementBed bed;
  EXPECT_EQ(bed.run({"SET", "x", "v"}), "+OK\r\n");
  const int proposals = bed.registers->proposals;
  EXPECT_EQ(bed.run({"EVAL", "return redis.call('get', 'x')", "0"}), "$1\r\nv\r\n");
  const auto wrote = bed.run({"EVAL", "return redis.call('set', 'x', 'w')", "0"});
  EXPECT_NE(wrote.find("must declare it"), std::string::npos) << wrote;
  EXPECT_EQ(bed.registers->proposals, proposals);
  EXPECT_EQ(bed.run({"GET", "x"}), "$1\r\nv\r\n");
  // A transaction that only reads runs here too.
  EXPECT_EQ(bed.run({"MULTI"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"GET", "x"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"EXEC"}), "*1\r\n$1\r\nv\r\n");
}

TEST(CommandAgreement, ABlockingPopWithSomethingToTakeIsServedAtOnce) {
  AgreementBed bed;
  EXPECT_EQ(bed.run({"RPUSH", "l", "a", "b"}), ":2\r\n");
  EXPECT_EQ(bed.run({"BLPOP", "l", "1"}), "*2\r\n$1\r\nl\r\n$1\r\na\r\n");
  EXPECT_EQ(bed.run({"LRANGE", "l", "0", "-1"}), "*1\r\n$1\r\nb\r\n");
  // Inside a transaction it does not wait.
  EXPECT_EQ(bed.run({"MULTI"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"BLPOP", "empty", "1"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"EXEC"}), "*1\r\n*-1\r\n");
}

TEST(CommandAgreement, ABlockingPopWaitsForAWriteToItsKey) {
  AgreementBed bed;
  Session waiting;
  waiting.id = bed.database->next_client_id();
  std::string out;
  std::optional<std::string> woken;
  const auto executed = bed.database->execute(waiting, {"BLPOP", "q", "0"}, out, [&woken](std::string reply) { woken = std::move(reply); });
  ASSERT_TRUE(executed.wait);
  EXPECT_EQ(executed.wait_seconds, 0);
  bed.io.restart();
  bed.io.run();
  // Its attempt found nothing, changed nothing, and it waits.
  EXPECT_FALSE(woken);
  EXPECT_FALSE(bed.registers->record(0, "q").payload);
  // Another connection's push: the pop is proposed again and takes it.
  EXPECT_EQ(bed.run({"RPUSH", "q", "job"}), ":1\r\n");
  bed.io.restart();
  bed.io.run();
  ASSERT_TRUE(woken);
  EXPECT_EQ(*woken, "*2\r\n$1\r\nq\r\n$3\r\njob\r\n");
  EXPECT_EQ(bed.run({"EXISTS", "q"}), ":0\r\n");
}

TEST(CommandAgreement, ABlockingPopTimesOutWithItsReply) {
  AgreementBed bed;
  Session waiting;
  waiting.id = bed.database->next_client_id();
  std::string out;
  std::optional<std::string> woken;
  const auto executed = bed.database->execute(waiting, {"BLPOP", "q", "2.5"}, out, [&woken](std::string reply) { woken = std::move(reply); });
  ASSERT_TRUE(executed.wait);
  EXPECT_EQ(executed.wait_seconds, 2.5);
  // While its attempt is proposed, a timeout waits for the outcome.
  EXPECT_FALSE(bed.database->cancel_wait(*executed.wait));
  bed.io.restart();
  bed.io.run();
  ASSERT_TRUE(woken);
  EXPECT_EQ(*woken, "*-1\r\n");
  // Parked, it times out at once.
  woken.reset();
  const auto again = bed.database->execute(waiting, {"BLPOP", "q", "1"}, out, [&woken](std::string reply) { woken = std::move(reply); });
  bed.io.restart();
  bed.io.run();
  EXPECT_EQ(bed.database->cancel_wait(*again.wait), "*-1\r\n");
  // A bad timeout is refused before anything is proposed.
  const int proposals = bed.registers->proposals;
  EXPECT_EQ(bed.run({"BLPOP", "q", "soon"}), "-ERR timeout is not a float or out of range\r\n");
  EXPECT_EQ(bed.registers->proposals, proposals);
}

TEST(CommandAgreement, ABlockingPopOverSeveralKeysIsRefused) {
  AgreementBed bed;
  EXPECT_EQ(bed.run({"BLPOP", "a", "b", "1"}),
            "-ERR this command writes more than one key, which Mimir refuses until Dannan 0.10.0's atomic commit (ruling 35)\r\n");
}

cluster::Record string_record(const std::string& value) {
  // A string's DUMP payload, made by a database of its own.
  Database scratch(dannan::contract::system_wall_clock());
  Session session;
  std::string out;
  scratch.execute(session, {"SET", "x", value}, out);
  out.clear();
  scratch.execute(session, {"DUMP", "x"}, out);
  const auto start = out.find("\r\n") + 2;
  cluster::Record record;
  record.payload = out.substr(start, out.size() - start - 2);
  return record;
}

TEST(CommandAgreement, AnotherNodesCommitIsServedOnceItsNoticeComes) {
  AgreementBed bed;
  ASSERT_TRUE(bed.registers->committed);
  bed.registers->commit_elsewhere(0, "k", string_record("from-afar"));
  EXPECT_EQ(bed.run({"GET", "k"}), "$9\r\nfrom-afar\r\n");
  // A deletion is a commit too.
  bed.registers->commit_elsewhere(0, "k", cluster::Record{});
  EXPECT_EQ(bed.run({"EXISTS", "k"}), ":0\r\n");
}

TEST(CommandAgreement, AnOlderNoticeNeverUndoesANewerCommit) {
  AgreementBed bed;
  EXPECT_EQ(bed.run({"SET", "k", "new"}), "+OK\r\n");
  // A notice for a commit before it, arriving late.
  bed.registers->committed(0, "k", Registers::version(0), string_record("old"));
  EXPECT_EQ(bed.run({"GET", "k"}), "$3\r\nnew\r\n");
}

TEST(CommandAgreement, ATransactionOverOneKeyCommitsWhole) {
  AgreementBed bed;
  EXPECT_EQ(bed.run({"MULTI"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"INCR", "k"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"INCR", "k"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"EXPIRE", "k", "100"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"GET", "k"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"EXEC"}), "*4\r\n:1\r\n:2\r\n:1\r\n$1\r\n2\r\n");
  EXPECT_EQ(bed.registers->proposals, 1);
  EXPECT_EQ(bed.registers->record(0, "k").serial, 1u);
  EXPECT_EQ(bed.run({"GET", "k"}), "$1\r\n2\r\n");
  EXPECT_TRUE(bed.registers->record(0, "k").expires_at);
}

TEST(CommandAgreement, AWatchedKeyWrittenElsewhereAbortsTheExec) {
  AgreementBed bed;
  EXPECT_EQ(bed.run({"WATCH", "k"}), "+OK\r\n");
  EXPECT_EQ(bed.registers->reads, 1);
  // Another node writes it, and this node has not heard yet: the agreement
  // still sees it.
  bed.registers->record(0, "k").serial = 1;
  EXPECT_EQ(bed.run({"MULTI"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"SET", "k", "mine"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"EXEC"}), "*-1\r\n");
  EXPECT_FALSE(bed.registers->record(0, "k").payload);
}

TEST(CommandAgreement, AWatchedKeyNobodyWroteLetsTheExecCommit) {
  AgreementBed bed;
  EXPECT_EQ(bed.run({"WATCH", "k"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"MULTI"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"SET", "k", "mine"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"EXEC"}), "*1\r\n+OK\r\n");
  EXPECT_EQ(bed.run({"GET", "k"}), "$4\r\nmine\r\n");
}

TEST(CommandAgreement, ATransactionOverSeveralKeysIsRefused) {
  AgreementBed bed;
  const std::string several = "-ERR this command writes more than one key, which Mimir refuses until Dannan 0.10.0's atomic commit (ruling 35)\r\n";
  EXPECT_EQ(bed.run({"MULTI"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"SET", "a", "1"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"SET", "b", "2"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"EXEC"}), several);
  // A watch on another key than the one written is several keys too.
  EXPECT_EQ(bed.run({"WATCH", "a"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"MULTI"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"SET", "b", "2"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"EXEC"}), several);
  EXPECT_EQ(bed.registers->proposals, 0);
}

TEST(CommandAgreement, ANoticeBreaksAWatchOnItsKey) {
  AgreementBed bed;
  EXPECT_EQ(bed.run({"WATCH", "k"}), "+OK\r\n");
  bed.registers->commit_elsewhere(0, "k", string_record("v"));
  EXPECT_EQ(bed.run({"MULTI"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"GET", "k"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"EXEC"}), "*-1\r\n");
}

TEST(CommandAgreement, AnExpiredKeyIsDeletedByAgreement) {
  using namespace std::chrono_literals;
  AgreementBed bed(true);
  EXPECT_EQ(bed.run({"SET", "t", "v", "PX", "100"}), "+OK\r\n");
  const uint64_t serial = bed.registers->record(0, "t").serial;
  bed.wall.advance(200ms);
  // A read finds it expired here: answered at once, and its deletion is
  // proposed, once however often it is found meanwhile.
  EXPECT_EQ(bed.run({"GET", "t"}), "$-1\r\n");
  EXPECT_EQ(bed.run({"EXISTS", "t"}), ":0\r\n");
  EXPECT_EQ(bed.registers->proposals, 2);
  bed.settle();
  EXPECT_FALSE(bed.registers->record(0, "t").payload);
  // The deletion counts as a write, for WATCH.
  EXPECT_EQ(bed.registers->record(0, "t").serial, serial + 1);

  // Active expiry finds one no read asked for.
  EXPECT_EQ(bed.run({"SET", "a", "v", "PX", "100"}), "+OK\r\n");
  bed.wall.advance(200ms);
  EXPECT_EQ(bed.database->expire_step(100), 1u);
  bed.settle();
  EXPECT_FALSE(bed.registers->record(0, "a").payload);
}

TEST(CommandAgreement, AnExpiryGivenALaterOneElsewhereIsNotDeleted) {
  using namespace std::chrono_literals;
  AgreementBed bed(true);
  EXPECT_EQ(bed.run({"SET", "u", "v", "PX", "100"}), "+OK\r\n");
  // Another node's PEXPIRE committed, its notice not yet here.
  bed.registers->record(0, "u").expires_at = bed.wall.unix_ms() + 100000;
  bed.wall.advance(200ms);
  EXPECT_EQ(bed.run({"GET", "u"}), "$-1\r\n");
  bed.settle();
  // The deletion found the later expiry and changed nothing; what it
  // committed brought this node's view up to date.
  ASSERT_TRUE(bed.registers->record(0, "u").payload);
  EXPECT_EQ(bed.run({"GET", "u"}), "$1\r\nv\r\n");
}

TEST(CommandAgreement, AWriteToAnExpiredKeyTakesItOutItself) {
  using namespace std::chrono_literals;
  AgreementBed bed(true);
  EXPECT_EQ(bed.run({"SET", "w", "old", "PX", "100"}), "+OK\r\n");
  bed.wall.advance(200ms);
  EXPECT_EQ(bed.run({"SET", "w", "new", "NX"}), "+OK\r\n");
  bed.settle();
  // One proposal for the write, none for the expiry it found.
  EXPECT_EQ(bed.registers->proposals, 2);
  EXPECT_EQ(bed.run({"GET", "w"}), "$3\r\nnew\r\n");
}

TEST(CommandAgreement, UsersChangeByAgreementAndReachEveryNode) {
  AgreementBed here;
  here.session.tls = true;
  EXPECT_EQ(here.run({"ACL", "SETUSER", "alice", "on", ">secret", "~app:*", "&news", "+@read", "(~other:* +get)"}), "+OK\r\n");
  EXPECT_EQ(here.registers->system_proposals, 1);
  // The users' register holds them, and this node uses them at once.
  const cluster::Record users = here.registers->record(0xffff, "acl");
  ASSERT_TRUE(users.payload);
  const auto listed = here.run({"ACL", "LIST"});
  EXPECT_EQ(here.run({"AUTH", "alice", "secret"}), "+OK\r\n");
  EXPECT_EQ(here.run({"AUTH", "default", "any"}), "+OK\r\n");

  // Another node hears of the commit: its users become the same, line for line.
  AgreementBed there;
  there.session.tls = true;
  EXPECT_EQ(there.run({"AUTH", "alice", "secret"}), "-WRONGPASS invalid username-password pair or user is disabled.\r\n");
  there.registers->commit_elsewhere(0xffff, "acl", users);
  EXPECT_EQ(there.run({"ACL", "LIST"}), listed);
  EXPECT_EQ(there.run({"AUTH", "alice", "secret"}), "+OK\r\n");
  EXPECT_EQ(there.run({"AUTH", "default", "any"}), "+OK\r\n");

  // A user deleted elsewhere: its connections here close.
  bool killed = false;
  Session theirs;
  theirs.id = there.database->next_client_id();
  theirs.tls = true;
  theirs.kill = [&killed] { killed = true; };
  there.database->welcome(theirs);
  std::string out;
  there.database->execute(theirs, {"AUTH", "alice", "secret"}, out);
  ASSERT_EQ(out, "+OK\r\n");
  EXPECT_EQ(here.run({"ACL", "DELUSER", "alice"}), ":1\r\n");
  there.registers->commit_elsewhere(0xffff, "acl", here.registers->record(0xffff, "acl"));
  EXPECT_TRUE(killed);
  EXPECT_EQ(there.run({"ACL", "USERS"}), "*1\r\n$7\r\ndefault\r\n");
  there.database->forget(theirs);

  // A change refused, or inside MULTI, changes nothing.
  EXPECT_EQ(here.run({"ACL", "SETUSER", "bob", "nonsense"}), "-ERR Error in ACL SETUSER modifier 'nonsense': Syntax error\r\n");
  EXPECT_EQ(here.run({"ACL", "USERS"}), "*1\r\n$7\r\ndefault\r\n");
  EXPECT_EQ(here.run({"MULTI"}), "+OK\r\n");
  EXPECT_EQ(here.run({"ACL", "SETUSER", "carol", "on"}), "+QUEUED\r\n");
  EXPECT_EQ(here.run({"EXEC"}), "-ERR in a cluster ACL SETUSER and DELUSER commit on their own, not inside MULTI (ruling 27)\r\n");
  EXPECT_EQ(here.run({"ACL", "USERS"}), "*1\r\n$7\r\ndefault\r\n");
}

TEST(CommandAgreement, FunctionLibrariesChangeByAgreementAndReachEveryNode) {
  const std::string code = "#!lua name=lib\nserver.register_function('hello', function() return 'hi' end)";
  AgreementBed here;
  EXPECT_EQ(here.run({"FUNCTION", "LOAD", code}), "$3\r\nlib\r\n");
  EXPECT_EQ(here.registers->system_proposals, 1);
  const cluster::Record libraries = here.registers->record(0xffff, "functions");
  ASSERT_TRUE(libraries.payload);
  EXPECT_EQ(here.run({"FCALL", "hello", "0"}), "$2\r\nhi\r\n");
  // Loaded again, it is refused by the agreed libraries, and changes nothing.
  EXPECT_EQ(here.run({"FUNCTION", "LOAD", code}), "-ERR Library 'lib' already exists\r\n");

  // Another node hears of the commit and can call it.
  AgreementBed there;
  EXPECT_EQ(there.run({"FCALL", "hello", "0"}), "-ERR Function not found\r\n");
  there.registers->commit_elsewhere(0xffff, "functions", libraries);
  EXPECT_EQ(there.run({"FCALL", "hello", "0"}), "$2\r\nhi\r\n");
  EXPECT_EQ(there.run({"FUNCTION", "LIST"}), here.run({"FUNCTION", "LIST"}));

  // A flush is agreed too, and reaches it.
  EXPECT_EQ(here.run({"FUNCTION", "FLUSH"}), "+OK\r\n");
  EXPECT_EQ(here.run({"FCALL", "hello", "0"}), "-ERR Function not found\r\n");
  there.registers->commit_elsewhere(0xffff, "functions", here.registers->record(0xffff, "functions"));
  EXPECT_EQ(there.run({"FCALL", "hello", "0"}), "-ERR Function not found\r\n");
}

TEST(CommandAgreement, ScriptsAreFoundAcrossTheCluster) {
  using namespace std::chrono_literals;
  const std::string body = "return 'hi'";
  const std::string sha = sha1_hex(body);
  AgreementBed here(true);
  EXPECT_EQ(here.run({"SCRIPT", "LOAD", body}), "$40\r\n" + sha + "\r\n");
  EXPECT_EQ(here.registers->system_proposals, 1);
  const cluster::Record loaded = here.registers->record(0xffff, "script:" + sha);
  ASSERT_TRUE(loaded.payload);
  EXPECT_EQ(*loaded.payload, body);

  // Another node has not seen it: its EVALSHA reads the register, then runs.
  AgreementBed there(true);
  there.registers->record(0xffff, "script:" + sha) = loaded;
  EXPECT_EQ(there.run({"EVALSHA", sha, "0"}), "$2\r\nhi\r\n");
  EXPECT_EQ(there.registers->reads, 1);
  EXPECT_EQ(there.run({"EVALSHA", sha, "0"}), "$2\r\nhi\r\n");
  EXPECT_EQ(there.registers->reads, 1);
  EXPECT_EQ(there.run({"EVALSHA", std::string(40, 'a'), "0"}), "-NOSCRIPT No matching script.\r\n");
  EXPECT_EQ(there.registers->reads, 2);

  // SCRIPT FLUSH empties this node's cache, and a script loaded before it is
  // not brought back.
  there.wall.advance(1000ms);
  EXPECT_EQ(there.run({"SCRIPT", "FLUSH"}), "+OK\r\n");
  EXPECT_EQ(there.run({"EVALSHA", sha, "0"}), "-NOSCRIPT No matching script.\r\n");

  // An EVAL's script is shared in the background, once.
  EXPECT_EQ(here.run({"EVAL", "return 1", "0"}), ":1\r\n");
  EXPECT_EQ(here.run({"EVAL", "return 1", "0"}), ":1\r\n");
  here.settle();
  EXPECT_EQ(here.registers->system_proposals, 2);
  ASSERT_TRUE(here.registers->record(0xffff, "script:" + sha1_hex("return 1")).payload);
}

TEST(CommandAgreement, AStrictReadSeesWhatAMajorityHolds) {
  AgreementBed bed;
  EXPECT_EQ(bed.run({"SET", "k", "near"}), "+OK\r\n");
  // Another node commits, and its notice has not come yet.
  bed.registers->record(0, "k") = string_record("far");
  EXPECT_EQ(bed.run({"GET", "k"}), "$4\r\nnear\r\n");
  EXPECT_EQ(bed.run({"CLIENT", "STRICT", "ON"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"GET", "k"}), "$3\r\nfar\r\n");
  EXPECT_EQ(bed.registers->reads, 1);
  const auto info = bed.run({"CLIENT", "INFO"});
  EXPECT_NE(info.find(" strict=1"), std::string::npos) << info;
  // Several keys are each read from a majority.
  EXPECT_EQ(bed.run({"MGET", "k", "none"}), "*2\r\n$3\r\nfar\r\n$-1\r\n");
  EXPECT_EQ(bed.registers->reads, 3);
  // Without a majority it is refused, never answered stale.
  bed.registers->refuse_reads = 1;
  EXPECT_EQ(bed.run({"GET", "k"}), "-CLUSTERDOWN no majority of the key's replicas answered\r\n");
  // OFF: plain reads again, from this node's view, which the strict read
  // brought up to date.
  EXPECT_EQ(bed.run({"CLIENT", "STRICT", "OFF"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"GET", "k"}), "$3\r\nfar\r\n");
  EXPECT_EQ(bed.run({"CLIENT", "INFO"}).find("strict="), std::string::npos);
  EXPECT_EQ(bed.run({"CLIENT", "STRICT", "MAYBE"}), "-ERR syntax error\r\n");
}

}  // namespace
}  // namespace mimir::command
