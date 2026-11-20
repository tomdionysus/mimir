//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The server over a real socket on loopback, from the protocol's
// specification: a request gets its reply, pipelined requests get theirs in
// order, a protocol error is answered and the connection closed, and QUIT
// closes after its reply.

#include "mimir/server/server.h"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <thread>

#include "dannan/listen/listener.h"
#include "dannan/test/fixtures/stepped_time.h"
#include "mimir/test/fixtures/bed.h"

namespace mimir::server {
namespace {

namespace asio = boost::asio;

struct ServerBed {
  dannan::test::SteppedWall wall;
  asio::io_context io;
  // Every write commits by agreement, on an in-process cluster of one run on
  // the server's own io thread.
  command::Database database{wall, test::Bed::settings_with(std::make_shared<test::LocalAgreements>(io))};
  asio::executor_work_guard<asio::io_context::executor_type> work{io.get_executor()};
  std::shared_ptr<Server> server = std::make_shared<Server>(io, database, dannan::status::ScopedLogger(dannan::status::null_sink(), "server"));
  dannan::listen::StreamListener listener{io, loopback()};

  static dannan::listen::ListenerConfig loopback() {
    dannan::listen::ListenerConfig config;
    config.name = "resp";
    config.host = "127.0.0.1";
    return config;
  }
  std::thread thread;

  ServerBed() {
    listener.start(io.get_executor(), [s = server](std::shared_ptr<dannan::listen::Stream> stream) { s->serve(std::move(stream)); });
    thread = std::thread([this] { io.run(); });
  }
  ~ServerBed() {
    listener.stop();
    // Every connection finishes before the executor stops, or a connection
    // and its stream, holding each other, are never freed.
    server->close_all();
    wait_until([&] { return server->counters().open == 0; });
    work.reset();
    io.stop();
    thread.join();
  }
  // Polls the server thread's progress: a socket's close is not steppable time.
  template <typename Done>
  static bool wait_until(Done done) {
    for (int i = 0; i < 5000 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return done();
  }
};

// A blocking client whose every read gives up after five seconds.
struct Client {
  asio::io_context io;
  asio::ip::tcp::socket socket{io};

  explicit Client(uint16_t port) {
    socket.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    timeval limit{5, 0};
    setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
  }
  void send(const std::string& bytes) { asio::write(socket, asio::buffer(bytes)); }
  std::string read(size_t bytes) {
    std::string out(bytes, '\0');
    boost::system::error_code error;
    const size_t got = asio::read(socket, asio::buffer(out), error);
    out.resize(got);
    return out;
  }
  // Everything until the server closes.
  std::string read_to_end() {
    std::string out;
    char chunk[4096];
    for (;;) {
      boost::system::error_code error;
      const size_t got = socket.read_some(asio::buffer(chunk), error);
      out.append(chunk, got);
      if (error) return out;
    }
  }
};

TEST(Server, ARequestGetsItsReply) {
  ServerBed bed;
  Client client(bed.listener.port());
  client.send("*1\r\n$4\r\nPING\r\n");
  EXPECT_EQ(client.read(7), "+PONG\r\n");
  client.send("SET k v\r\n");
  EXPECT_EQ(client.read(5), "+OK\r\n");
  client.send("*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
  EXPECT_EQ(client.read(7), "$1\r\nv\r\n");
}

TEST(Server, PipelinedRequestsAreAnsweredInOrder) {
  ServerBed bed;
  Client client(bed.listener.port());
  std::string requests;
  std::string expected;
  for (int i = 0; i < 100; ++i) {
    requests += "INCR n\r\n";
    expected += ":" + std::to_string(i + 1) + "\r\n";
  }
  client.send(requests);
  EXPECT_EQ(client.read(expected.size()), expected);
}

TEST(Server, AProtocolErrorIsAnsweredAndTheConnectionClosed) {
  ServerBed bed;
  Client client(bed.listener.port());
  client.send("PING\r\n*1\r\n$x\r\nPING\r\n");
  EXPECT_EQ(client.read_to_end(), "+PONG\r\n-ERR Protocol error: invalid bulk length\r\n");
  EXPECT_EQ(bed.server->counters().protocol_errors, 1u);
}

TEST(Server, QuitClosesAfterItsReplyAndIgnoresWhatFollows) {
  ServerBed bed;
  Client client(bed.listener.port());
  client.send("PING\r\nQUIT\r\nPING\r\n");
  EXPECT_EQ(client.read_to_end(), "+PONG\r\n+OK\r\n");
}

TEST(Server, ABlockedClientIsWokenByAnotherAndItsPipelineWaitsBehindIt) {
  ServerBed bed;
  Client waiting(bed.listener.port());
  Client pushing(bed.listener.port());
  // The PING behind the BLPOP is answered only after the BLPOP is.
  waiting.send("BLPOP jobs 0\r\nPING\r\n");
  // Wait for the BLPOP to be parked: an event, not a time.
  for (int i = 0; i < 100000 && bed.database.waiting() == 0; ++i) std::this_thread::yield();
  ASSERT_EQ(bed.database.waiting(), 1u);
  pushing.send("RPUSH jobs work\r\n");
  EXPECT_EQ(pushing.read(4), ":1\r\n");
  const std::string expected = "*2\r\n$4\r\njobs\r\n$4\r\nwork\r\n+PONG\r\n";
  EXPECT_EQ(waiting.read(expected.size()), expected);
}

TEST(Server, AClientThatLeavesWhileBlockedTakesNothing) {
  ServerBed bed;
  {
    Client gone(bed.listener.port());
    gone.send("BLPOP jobs 0\r\n");
    for (int i = 0; i < 100000 && bed.database.waiting() == 0; ++i) std::this_thread::yield();
    ASSERT_EQ(bed.database.waiting(), 1u);
  }
  // The close is seen and the wait withdrawn: an event, waited on.
  for (int i = 0; i < 1000000 && bed.database.waiting() != 0; ++i) std::this_thread::yield();
  ASSERT_EQ(bed.database.waiting(), 0u);
  Client pushing(bed.listener.port());
  pushing.send("RPUSH jobs work\r\nLLEN jobs\r\n");
  EXPECT_EQ(pushing.read(8), ":1\r\n:1\r\n");
}

TEST(Server, ASubscriberHearsItsConfirmationThenEveryMessageInOrder) {
  ServerBed bed;
  Client subscriber(bed.listener.port());
  Client publisher(bed.listener.port());
  // A reply already owed goes before the confirmation behind it.
  subscriber.send("PING\r\nSUBSCRIBE news\r\n");
  const std::string confirmed = "+PONG\r\n*3\r\n$9\r\nsubscribe\r\n$4\r\nnews\r\n:1\r\n";
  EXPECT_EQ(subscriber.read(confirmed.size()), confirmed);
  publisher.send("PUBLISH news one\r\nPUBLISH news two\r\n");
  EXPECT_EQ(publisher.read(8), ":1\r\n:1\r\n");
  const std::string messages = "*3\r\n$7\r\nmessage\r\n$4\r\nnews\r\n$3\r\none\r\n*3\r\n$7\r\nmessage\r\n$4\r\nnews\r\n$3\r\ntwo\r\n";
  EXPECT_EQ(subscriber.read(messages.size()), messages);
}

// A connection lives while its stream is unfinished; shutdown closes them all,
// so none outlives the executor (found by LeakSanitizer in hosted CI: three
// connections left open at a test's end were never freed).
TEST(Server, CloseAllFinishesEveryConnection) {
  ServerBed bed;
  Client a(bed.listener.port());
  Client b(bed.listener.port());
  a.send("PING\r\n");
  b.send("PING\r\n");
  EXPECT_EQ(a.read(7), "+PONG\r\n");
  EXPECT_EQ(b.read(7), "+PONG\r\n");
  EXPECT_EQ(bed.server->counters().open, 2u);
  bed.server->close_all();
  EXPECT_TRUE(ServerBed::wait_until([&] { return bed.server->counters().open == 0; }));
  EXPECT_EQ(a.read_to_end(), "");
  EXPECT_EQ(b.read_to_end(), "");
}

TEST(Server, ConnectionsShareOneKeyspaceAndHaveTheirOwnIds) {
  ServerBed bed;
  Client a(bed.listener.port());
  Client b(bed.listener.port());
  a.send("SET shared 1\r\nCLIENT ID\r\n");
  const std::string first = a.read(5 + 4);
  b.send("GET shared\r\nCLIENT ID\r\n");
  const std::string second = b.read(7 + 4);
  EXPECT_EQ(second.substr(0, 7), "$1\r\n1\r\n");
  EXPECT_NE(first.substr(5), second.substr(7));
}

// MIGRATE between two nodes, as Valkey 8.1.1 answers it (probed): the key
// moves with its life, a target's refusal is relayed, the connection waits
// with its pipeline behind it, and the source is held by none of it.
TEST(Server, MigrateMovesKeysToAnotherServer) {
  ServerBed source;
  ServerBed target;
  const std::string port = std::to_string(target.listener.port());
  Client s(source.listener.port());
  Client t(target.listener.port());
  const auto ask = [](Client& c, const std::string& line, const std::string& reply) {
    c.send(line + "\r\n");
    EXPECT_EQ(c.read(reply.size()), reply) << line;
  };
  ask(s, "SET k v PX 100000", "+OK\r\n");
  // The pipeline after MIGRATE waits for it, and is answered after it.
  ask(s, "MIGRATE 127.0.0.1 " + port + " k 0 1000\r\nEXISTS k", "+OK\r\n:0\r\n");
  ask(t, "GET k", "$1\r\nv\r\n");
  // Its whole life: both nodes' clocks are stepped, and have not moved.
  ask(t, "PTTL k", ":100000\r\n");
  ask(s, "MIGRATE 127.0.0.1 " + port + " k 0 1000", "+NOKEY\r\n");
  ask(s, "SET k w", "+OK\r\n");
  ask(s, "MIGRATE 127.0.0.1 " + port + " k 0 1000", "-ERR Target instance replied with error: BUSYKEY Target key name already exists.\r\n");
  ask(s, "EXISTS k", ":1\r\n");
  ask(s, "MIGRATE 127.0.0.1 " + port + " k 0 1000 COPY REPLACE", "+OK\r\n");
  ask(s, "EXISTS k", ":1\r\n");
  ask(t, "GET k", "$1\r\nw\r\n");
  ask(s, "RPUSH l a b", ":2\r\n");
  ask(s, "MIGRATE 127.0.0.1 " + port + " \"\" 0 1000 REPLACE KEYS l k nosuch", "+OK\r\n");
  ask(s, "EXISTS l k", ":0\r\n");
  ask(t, "LRANGE l 0 -1", "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
  ask(s, "SET k x", "+OK\r\n");
  ask(s, "MIGRATE 127.0.0.1 " + port + " k 9 1000", "-ERR Target instance replied with error: ERR DB index is out of range\r\n");
  ask(s, "EXISTS k", ":1\r\n");
}

TEST(Server, MigrateToNoServerIsAnIoError) {
  ServerBed source;
  Client s(source.listener.port());
  s.send("SET k v\r\n");
  EXPECT_EQ(s.read(5), "+OK\r\n");
  // Port 1 on loopback refuses.
  s.send("MIGRATE 127.0.0.1 1 k 0 500\r\nEXISTS k\r\n");
  const std::string expected = "-IOERR error or timeout writing to target instance\r\n:1\r\n";
  EXPECT_EQ(s.read(expected.size()), expected);
  s.send("MIGRATE 127.0.0.1 notaport k 0 500\r\n");
  const std::string bad = "-IOERR error or timeout writing to target instance\r\n";
  EXPECT_EQ(s.read(bad.size()), bad);
  // In a transaction it cannot wait for its target.
  s.send("MULTI\r\nMIGRATE 127.0.0.1 1 k 0 500\r\nEXEC\r\n");
  const std::string transaction = "+OK\r\n+QUEUED\r\n*1\r\n-ERR MIGRATE cannot run inside a transaction or a script on Mimir\r\n";
  EXPECT_EQ(s.read(transaction.size()), transaction);
}

}  // namespace
}  // namespace mimir::server
