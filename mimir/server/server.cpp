//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/server/server.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <string>
#include <vector>

#include "mimir/command/args.h"

namespace mimir::server {

// One client: its parser, its session, the read buffer, and a wait it may be
// parked in. Everything here runs on the stream's strand. While parked, the
// commands behind the blocking one wait in the parser, as Valkey holds a
// blocked client's pipeline; reading goes on, so a client that disconnects is
// seen and its wait withdrawn rather than left to take an element it can no
// longer receive.
class Connection : public std::enable_shared_from_this<Connection> {
 public:
  Connection(std::shared_ptr<Server> server, std::shared_ptr<dannan::listen::Stream> stream)
      : _server(std::move(server)),
        _stream(std::move(stream)),
        _parser(_server->_settings.limits),
        _buffer(_server->_settings.read_size),
        _timer(_stream->get_executor()) {
    _session.id = _server->_database.next_client_id();
    _session.created_ms = _server->_database.now_ms();
    _session.address = _stream->peer().address().to_string() + ":" + std::to_string(_stream->peer().port());
    _session.tls = _stream->is_tls();
    _server->_database.welcome(_session);
    // Published messages are queued on the stream from the publisher's thread,
    // waiting on nothing; a stream gone takes nothing.
    _session.push = [stream = std::weak_ptr<dannan::listen::Stream>(_stream)](std::string bytes) {
      if (auto alive = stream.lock()) alive->send(std::move(bytes));
    };
    // CLIENT KILL, from another connection's strand: the stream closes itself.
    _session.kill = [stream = std::weak_ptr<dannan::listen::Stream>(_stream)] {
      if (auto alive = stream.lock()) alive->close();
    };
  }

  void start() {
    ++_server->_open;
    {
      dannan::contract::Lock lock(_server->_streams_mutex);
      _server->_streams[_session.id] = _stream;
    }
    _stream->when_finished([self = shared_from_this()] { self->finished(); });
    read();
  }

 private:
  // A parked connection reads on to see a close, up to this much held unread.
  static constexpr size_t held_while_parked = 1u << 20;

  static bool pushes(std::string_view name) {
    for (std::string_view one : {"subscribe", "psubscribe", "ssubscribe", "unsubscribe", "punsubscribe", "sunsubscribe", "publish", "spublish"}) {
      if (command::iequals(name, one)) return true;
    }
    return false;
  }

  void finished() {
    {
      dannan::contract::Lock lock(_server->_streams_mutex);
      _server->_streams.erase(_session.id);
    }
    --_server->_open;
    _closing = true;
    _server->_database.pubsub().drop(_session.id);
    _server->_database.forget(_session);
    if (_wait) {
      _server->_database.cancel_wait(*_wait);
      _wait.reset();
    }
    _timer.cancel();
  }

  void read() {
    if (_reading || _closing) return;
    if (_wait && _parser.buffered() > held_while_parked) return;
    _reading = true;
    _stream->async_read_some(boost::asio::buffer(_buffer), [self = shared_from_this()](boost::system::error_code error, size_t bytes) {
      self->_reading = false;
      if (error) {
        // The client went, or the stream closed: nothing to say to anyone.
        self->_stream->abort();
        return;
      }
      self->_parser.feed(std::string_view(self->_buffer.data(), bytes));
      self->process();
      self->read();
    });
  }

  command::Wake wake() {
    return [self = shared_from_this()](std::string reply) {
      boost::asio::post(self->_stream->get_executor(), [self, reply = std::move(reply)]() mutable { self->woken(std::move(reply)); });
    };
  }

  // Runs one command, its reply appended to `out`; false when it parked or
  // the connection is to close, so nothing after it may run yet.
  bool run(std::vector<std::string> args, std::string& out) {
    // A command that pushes (a subscription's confirmation, a message to this
    // connection itself) goes after the replies before it, so those are sent
    // first.
    if (!out.empty() && pushes(args.front())) {
      _stream->send(std::move(out));
      out.clear();
    }
    auto executed = _server->_database.execute(_session, args, out, wake());
    if (executed.wait) {
      // A command CLIENT PAUSE holds is run again when its wait ends.
      if (executed.postponed) _postponed = std::move(args);
      park(*executed.wait, executed.wait_seconds);
      return false;
    }
    if (_session.closing) {
      _closing = true;
      return false;
    }
    return true;
  }

  // Runs every whole command buffered, in order, until one parks or the
  // connection is to close, and sends their replies together.
  void process() {
    if (_wait || _closing) return;
    std::string out;
    try {
      for (;;) {
        // Until it authenticates, a connection's requests are held to
        // Valkey's small bounds. Only this strand changes `authenticated`.
        _parser.restrict(!_session.authenticated);
        auto args = _parser.next();
        if (!args) break;
        ++_server->_commands;
        if (!run(std::move(*args), out)) break;
      }
    } catch (const resp::ProtocolError& e) {
      ++_server->_protocol_errors;
      resp::Reply reply(_session.protocol);
      reply.error(std::string("ERR ") + e.what());
      out.append(reply.bytes());
      _server->_logger.debug("closing " + _session.address + ": " + e.what());
      _closing = true;
    }
    if (!out.empty()) _stream->send(std::move(out));
    if (_closing) _stream->close();
  }

  void park(uint64_t id, double seconds) {
    _wait = id;
    if (seconds <= 0) return;
    _timer.expires_after(std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds)));
    _timer.async_wait([self = shared_from_this(), id](boost::system::error_code error) {
      if (error || self->_wait != id) return;
      // Served meanwhile: its wake is on its way with the reply.
      auto bytes = self->_server->_database.cancel_wait(id);
      if (!bytes) return;
      self->resume(std::move(*bytes));
    });
  }

  void woken(std::string reply) {
    if (!_wait || _closing) return;
    _timer.cancel();
    resume(std::move(reply));
  }

  void resume(std::string reply) {
    _wait.reset();
    if (_postponed) {
      // The pause is over: the held command runs now (and may be held again,
      // or park as a blocking command).
      auto args = std::move(*_postponed);
      _postponed.reset();
      std::string out;
      const bool more = run(std::move(args), out);
      if (!out.empty()) _stream->send(std::move(out));
      if (_closing) {
        _stream->close();
        return;
      }
      if (!more) {
        read();
        return;
      }
    } else {
      _stream->send(std::move(reply));
    }
    process();
    read();
  }

  std::shared_ptr<Server> _server;
  std::shared_ptr<dannan::listen::Stream> _stream;
  resp::RequestParser _parser;
  command::Session _session;
  std::vector<char> _buffer;
  boost::asio::steady_timer _timer;
  std::optional<uint64_t> _wait;
  // The command CLIENT PAUSE holds, to run when its wait ends.
  std::optional<std::vector<std::string>> _postponed;
  bool _reading = false;
  bool _closing = false;
};

Server::Server(command::Database& database, dannan::status::ScopedLogger logger, ServerSettings settings)
    : _database(database), _logger(std::move(logger)), _settings(settings) {}

void Server::serve(std::shared_ptr<dannan::listen::Stream> stream) {
  ++_accepted;
  std::make_shared<Connection>(shared_from_this(), std::move(stream))->start();
}

void Server::close_all() {
  std::vector<std::shared_ptr<dannan::listen::Stream>> open;
  {
    dannan::contract::Lock lock(_streams_mutex);
    for (const auto& [id, stream] : _streams) {
      if (auto alive = stream.lock()) open.push_back(std::move(alive));
    }
  }
  // Closed outside the lock: a stream may finish, and take it, at once.
  for (const auto& stream : open) stream->close();
}

ServerCounters Server::counters() const noexcept { return ServerCounters{_accepted.load(), _open.load(), _commands.load(), _protocol_errors.load()}; }

}  // namespace mimir::server
