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
#include "mimir/resp/reply_reader.h"

namespace mimir::server {

namespace {

void append_command(std::string& out, std::initializer_list<std::string_view> args) {
  out += "*" + std::to_string(args.size()) + "\r\n";
  for (auto arg : args) {
    out += "$" + std::to_string(arg.size()) + "\r\n";
    out.append(arg);
    out += "\r\n";
  }
}

// MIGRATE's requests to its target, in Valkey's order: AUTH when given,
// SELECT, then RESTORE for each key.
std::string migrate_requests(const command::MigrateJob& job) {
  std::string out;
  if (job.password) {
    if (job.username)
      append_command(out, {"AUTH", *job.username, *job.password});
    else
      append_command(out, {"AUTH", *job.password});
  }
  append_command(out, {"SELECT", std::to_string(job.target_db)});
  for (const auto& item : job.items) {
    const std::string ttl = std::to_string(item.ttl_ms);
    if (job.replace)
      append_command(out, {"RESTORE", item.key, ttl, item.payload, "REPLACE"});
    else
      append_command(out, {"RESTORE", item.key, ttl, item.payload});
  }
  return out;
}

}  // namespace

// MIGRATE's talk with its target, on the target stream's own strand: every
// request in one write, then the replies read whole, each within the timeout
// (a reply that does not come in time is a failed read, as Valkey's). Told
// once.
class MigrateClient : public std::enable_shared_from_this<MigrateClient> {
 public:
  using Done = std::function<void(command::MigrateOutcome)>;

  MigrateClient(std::shared_ptr<dannan::listen::Stream> stream, size_t expected, std::chrono::milliseconds timeout, Done done)
      : _stream(std::move(stream)), _expected(expected), _timeout(timeout), _timer(_stream->get_executor()), _done(std::move(done)), _buffer(16 * 1024) {}

  void start(std::string requests) {
    auto self = shared_from_this();
    boost::asio::post(_stream->get_executor(), [self, requests = std::move(requests)]() mutable {
      self->arm();
      self->_stream->send(std::move(requests));
      self->read();
    });
  }

 private:
  void arm() {
    _timer.expires_after(_timeout);
    _timer.async_wait([self = shared_from_this()](boost::system::error_code error) {
      if (!error) self->finish(true);
    });
  }

  void read() {
    if (!_done) return;
    _stream->async_read_some(boost::asio::buffer(_buffer), [self = shared_from_this()](boost::system::error_code error, size_t bytes) {
      if (error) {
        self->finish(true);
        return;
      }
      try {
        self->_reader.feed(std::string_view(self->_buffer.data(), bytes));
        while (auto item = self->_reader.next()) {
          self->_outcome.replies.push_back(std::string(1, item->type) + item->text);
          if (self->_outcome.replies.size() == self->_expected) {
            self->finish(false);
            return;
          }
        }
      } catch (const resp::ProtocolError&) {
        self->finish(true);
        return;
      }
      self->arm();
      self->read();
    });
  }

  void finish(bool failed) {
    if (!_done) return;
    _timer.cancel();
    _outcome.read_failed = failed;
    _stream->close();
    auto done = std::move(_done);
    _done = nullptr;
    done(std::move(_outcome));
  }

  std::shared_ptr<dannan::listen::Stream> _stream;
  const size_t _expected;
  const std::chrono::milliseconds _timeout;
  boost::asio::steady_timer _timer;
  Done _done;
  std::vector<char> _buffer;
  resp::ReplyReader _reader;
  command::MigrateOutcome _outcome;
};

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
    if ((_wait || _migrating) && _parser.buffered() > held_while_parked) return;
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
    if (executed.migrate) {
      migrate(std::move(*executed.migrate));
      return false;
    }
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

  // MIGRATE: the connection waits, its pipeline behind it, while it talks to
  // the target off the database's lock; the database then finishes it.
  void migrate(command::MigrateJob job) {
    _migrating = true;
    auto self = shared_from_this();
    auto shared = std::make_shared<command::MigrateJob>(std::move(job));
    const auto timeout = std::chrono::milliseconds(shared->timeout_ms);
    auto port = command::parse_integer(shared->port);
    if (!port || *port < 1 || *port > 65535) {
      boost::asio::post(_stream->get_executor(), [self, shared] { self->migrated(*shared, command::MigrateOutcome{{}, true, false}); });
      return;
    }
    _server->connector().connect(_stream->get_executor(), shared->host, static_cast<uint16_t>(*port), std::nullopt, timeout,
                                 [self, shared, timeout](std::shared_ptr<dannan::listen::Stream> target, std::string) {
                                   if (!target) {
                                     self->migrated(*shared, command::MigrateOutcome{{}, true, false});
                                     return;
                                   }
                                   const size_t expected = (shared->password ? 1 : 0) + 1 + shared->items.size();
                                   auto client = std::make_shared<MigrateClient>(target, expected, timeout, [self, shared](command::MigrateOutcome outcome) {
                                     boost::asio::post(self->_stream->get_executor(),
                                                       [self, shared, outcome = std::move(outcome)] { self->migrated(*shared, outcome); });
                                   });
                                   client->start(migrate_requests(*shared));
                                 });
  }

  void migrated(const command::MigrateJob& job, const command::MigrateOutcome& outcome) {
    // Finished whether or not the client is still here: what moved is removed
    // by agreement, and the answer comes once it is.
    auto self = shared_from_this();
    _server->_database.finish_migrate(_session, job, outcome, [self](std::string reply) {
      boost::asio::post(self->_stream->get_executor(), [self, reply = std::move(reply)]() mutable { self->answered_migrate(std::move(reply)); });
    });
  }

  void answered_migrate(std::string reply) {
    _migrating = false;
    if (_closing) return;
    if (!_session.reply_off) _stream->send(std::move(reply));
    process();
    read();
  }

  // Runs every whole command buffered, in order, until one parks or the
  // connection is to close, and sends their replies together.
  void process() {
    if (_wait || _closing || _migrating) return;
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
  // MIGRATE is talking to its target.
  bool _migrating = false;
  bool _reading = false;
  bool _closing = false;
};

Server::Server(boost::asio::io_context& io, command::Database& database, dannan::status::ScopedLogger logger, ServerSettings settings)
    : _io(io), _database(database), _logger(std::move(logger)), _settings(settings) {}

void Server::serve(std::shared_ptr<dannan::listen::Stream> stream) {
  ++_accepted;
  std::make_shared<Connection>(shared_from_this(), std::move(stream))->start();
}

dannan::listen::StreamConnector& Server::connector() {
  dannan::contract::Lock lock(_streams_mutex);
  if (!_connector) {
    dannan::listen::ConnectorConfig config;
    config.name = "migrate";
    _connector = std::make_unique<dannan::listen::StreamConnector>(_io, config);
  }
  return *_connector;
}

void Server::close_all() {
  std::vector<std::shared_ptr<dannan::listen::Stream>> open;
  {
    dannan::contract::Lock lock(_streams_mutex);
    for (const auto& [id, stream] : _streams) {
      if (auto alive = stream.lock()) open.push_back(std::move(alive));
    }
    // MIGRATE's connections to its targets too.
    if (_connector) _connector->close_all();
  }
  // Closed outside the lock: a stream may finish, and take it, at once.
  for (const auto& stream : open) stream->close();
}

ServerCounters Server::counters() const noexcept { return ServerCounters{_accepted.load(), _open.load(), _commands.load(), _protocol_errors.load()}; }

}  // namespace mimir::server
