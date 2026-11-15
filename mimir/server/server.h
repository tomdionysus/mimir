//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "dannan/listen/stream.h"
#include "dannan/status/log.h"
#include "mimir/command/database.h"
#include "mimir/resp/parser.h"

// Serves RESP on the streams a Dannan listener hands up: each connection reads
// what has arrived, runs every whole command in it in order against the
// database, and sends their replies together, so a pipeline costs one write.
// A command runs on its connection's strand (foreground work: Dannan's advice
// for a command that is memory work only). Bytes that are not a request are
// answered with the protocol error and the connection is closed, as Valkey
// does.
namespace mimir::server {

struct ServerSettings {
  resp::ParserLimits limits;
  // Bytes read at a time.
  size_t read_size = 16 * 1024;
};

struct ServerCounters {
  uint64_t accepted = 0;
  uint64_t open = 0;
  uint64_t commands = 0;
  uint64_t protocol_errors = 0;
};

class Server : public std::enable_shared_from_this<Server> {
 public:
  Server(command::Database& database, dannan::status::ScopedLogger logger, ServerSettings settings = {});

  // Serves `stream` until it closes; called on the stream's executor, as a
  // listener's Accepted. Returns at once.
  void serve(std::shared_ptr<dannan::listen::Stream> stream);

  ServerCounters counters() const noexcept;

 private:
  friend class Connection;

  command::Database& _database;
  dannan::status::ScopedLogger _logger;
  const ServerSettings _settings;
  std::atomic<uint64_t> _accepted{0};
  std::atomic<uint64_t> _open{0};
  std::atomic<uint64_t> _commands{0};
  std::atomic<uint64_t> _protocol_errors{0};
};

}  // namespace mimir::server
