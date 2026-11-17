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
    ++proposals;
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
      outcome.committed = true;
      outcome.record = records[{db, key}];
      done(outcome);
    });
  }
  int reads = 0;

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

 private:
  boost::asio::io_context& _io;
  uint64_t _next = 1;
  uint64_t _commits = 0;
};

class AgreementBed {
 public:
  AgreementBed() {
    registers = std::make_shared<Registers>(io);
    DatabaseSettings settings;
    settings.databases = 2;
    settings.agreements = registers;
    database = std::make_unique<Database>(dannan::contract::system_wall_clock(), settings);
    session.id = database->next_client_id();
  }

  // The command's reply, waiting for its agreement if it has one.
  std::string run(std::vector<std::string> args) {
    std::string out;
    std::optional<std::string> woken;
    const auto executed = database->execute(session, args, out, [&woken](std::string reply) { woken = std::move(reply); });
    if (!executed.wait) return out;
    io.restart();
    while (!woken && io.run_one() > 0) {
    }
    return woken.value_or("<no reply>");
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

TEST(CommandAgreement, WritingScriptsWaitForStepEight) {
  AgreementBed bed;
  const std::string later = "-ERR Mimir does not yet run blocking commands, or transactions and scripts that write, by agreement\r\n";
  EXPECT_EQ(bed.run({"EVAL", "return 1", "0"}), later);
  EXPECT_EQ(bed.run({"MULTI"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"EVAL", "return 1", "0"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"EXEC"}), later);
  // The transaction is gone: a later command runs at once.
  EXPECT_EQ(bed.run({"GET", "k"}), "$-1\r\n");
  // A transaction that only reads runs.
  EXPECT_EQ(bed.run({"MULTI"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"GET", "k"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"EXEC"}), "*1\r\n$-1\r\n");
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

}  // namespace
}  // namespace mimir::command
