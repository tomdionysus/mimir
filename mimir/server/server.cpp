//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/server/server.h"

#include <boost/asio/buffer.hpp>
#include <string>
#include <vector>

namespace mimir::server {

// One client: its parser, its session, and the read buffer. Lives while a
// read is outstanding; everything here runs on the stream's strand.
class Connection : public std::enable_shared_from_this<Connection> {
 public:
  Connection(std::shared_ptr<Server> server, std::shared_ptr<dannan::listen::Stream> stream)
      : _server(std::move(server)), _stream(std::move(stream)), _parser(_server->_settings.limits), _buffer(_server->_settings.read_size) {
    _session.id = _server->_database.next_client_id();
    _session.created_ms = _server->_database.now_ms();
    _session.address = _stream->peer().address().to_string() + ":" + std::to_string(_stream->peer().port());
  }

  void start() {
    ++_server->_open;
    _stream->when_finished([server = _server] { --server->_open; });
    read();
  }

 private:
  void read() {
    _stream->async_read_some(boost::asio::buffer(_buffer), [self = shared_from_this()](boost::system::error_code error, size_t bytes) {
      if (error) {
        // The client went, or the stream closed: nothing to say to anyone.
        self->_stream->abort();
        return;
      }
      self->received(bytes);
    });
  }

  void received(size_t bytes) {
    _parser.feed(std::string_view(_buffer.data(), bytes));
    std::string out;
    bool closing = false;
    try {
      while (auto args = _parser.next()) {
        ++_server->_commands;
        _server->_database.execute(_session, *args, out);
        if (_session.closing) {
          closing = true;
          break;
        }
      }
    } catch (const resp::ProtocolError& e) {
      ++_server->_protocol_errors;
      resp::Reply reply(_session.protocol);
      reply.error(std::string("ERR ") + e.what());
      out.append(reply.bytes());
      _server->_logger.debug("closing " + _session.address + ": " + e.what());
      closing = true;
    }
    if (!out.empty()) _stream->send(std::move(out));
    if (closing) {
      _stream->close();
      return;
    }
    read();
  }

  std::shared_ptr<Server> _server;
  std::shared_ptr<dannan::listen::Stream> _stream;
  resp::RequestParser _parser;
  command::Session _session;
  std::vector<char> _buffer;
};

Server::Server(command::Database& database, dannan::status::ScopedLogger logger, ServerSettings settings)
    : _database(database), _logger(std::move(logger)), _settings(settings) {}

void Server::serve(std::shared_ptr<dannan::listen::Stream> stream) {
  ++_accepted;
  std::make_shared<Connection>(shared_from_this(), std::move(stream))->start();
}

ServerCounters Server::counters() const noexcept { return ServerCounters{_accepted.load(), _open.load(), _commands.load(), _protocol_errors.load()}; }

}  // namespace mimir::server
