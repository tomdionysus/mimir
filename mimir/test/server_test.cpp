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

namespace mimir::server {
namespace {

namespace asio = boost::asio;

struct ServerBed {
  dannan::test::SteppedWall wall;
  command::Database database{wall};
  asio::io_context io;
  asio::executor_work_guard<asio::io_context::executor_type> work{io.get_executor()};
  std::shared_ptr<Server> server = std::make_shared<Server>(database, dannan::status::ScopedLogger(dannan::status::null_sink(), "server"));
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
    work.reset();
    io.stop();
    thread.join();
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

}  // namespace
}  // namespace mimir::server
