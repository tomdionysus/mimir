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
      }
      boost::asio::post(_io, [done, outcome] { done(outcome); });
    });
  }

  cluster::Record& record(size_t db, const std::string& key) { return records[std::make_pair(db, key)]; }

  std::map<std::pair<size_t, std::string>, cluster::Record> records;
  int lose_next = 0;
  int refuse_next = 0;
  int proposals = 0;

 private:
  boost::asio::io_context& _io;
  uint64_t _next = 1;
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

TEST(CommandAgreement, BlockingCommandsAndWritingTransactionsWaitForStepEight) {
  AgreementBed bed;
  const std::string later = "-ERR Mimir does not yet run blocking commands, or transactions and scripts that write, by agreement\r\n";
  EXPECT_EQ(bed.run({"BLPOP", "l", "1"}), later);
  EXPECT_EQ(bed.run({"EVAL", "return 1", "0"}), later);
  EXPECT_EQ(bed.run({"MULTI"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"SET", "k", "v"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"EXEC"}), later);
  // The transaction is gone: a later command runs at once.
  EXPECT_EQ(bed.run({"GET", "k"}), "$-1\r\n");
  // A transaction that only reads runs.
  EXPECT_EQ(bed.run({"MULTI"}), "+OK\r\n");
  EXPECT_EQ(bed.run({"GET", "k"}), "+QUEUED\r\n");
  EXPECT_EQ(bed.run({"EXEC"}), "*1\r\n$-1\r\n");
}

}  // namespace
}  // namespace mimir::command
