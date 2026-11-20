//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <hiredis/hiredis.h>
#include <signal.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dannan/test/fixtures/child_process.h"
#include "dannan/test/fixtures/ports.h"
#include "dannan/test/fixtures/temp_dir.h"

#ifndef MIMIR_NODE_BINARY
#error "MIMIR_NODE_BINARY must name the mimir-node built beside these tests"
#endif

namespace mimir::test {

struct FreeReply {
  void operator()(redisReply* reply) const {
    if (reply) freeReplyObject(reply);
  }
};
using Reply = std::unique_ptr<redisReply, FreeReply>;

// One hiredis connection to a node, as an application holds it. A push
// (RESP3) comes back from get_reply in order with the replies, rather than
// being dropped. Every wait is bounded by hiredis's ten-second timeout.
class Client {
 public:
  explicit Client(uint16_t port) {
    redisOptions options{};
    REDIS_OPTIONS_SET_TCP(&options, "127.0.0.1", port);
    const timeval timeout{10, 0};
    options.connect_timeout = &timeout;
    options.command_timeout = &timeout;
    options.options |= REDIS_OPT_NO_PUSH_AUTOFREE;
    _context.reset(redisConnectWithOptions(&options));
    if (!_context) throw std::runtime_error("hiredis could not allocate a context");
    if (_context->err) throw std::runtime_error("cannot connect to 127.0.0.1:" + std::to_string(port) + ": " + _context->errstr);
  }

  // The reply, or nothing when the connection failed or timed out.
  Reply command(const std::vector<std::string>& args) {
    append(args);
    return get_reply();
  }
  void append(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    std::vector<size_t> lengths;
    for (const auto& arg : args) {
      argv.push_back(arg.data());
      lengths.push_back(arg.size());
    }
    redisAppendCommandArgv(_context.get(), static_cast<int>(args.size()), argv.data(), lengths.data());
  }
  Reply get_reply() {
    void* reply = nullptr;
    if (redisGetReply(_context.get(), &reply) != REDIS_OK) return Reply();
    return Reply(static_cast<redisReply*>(reply));
  }
  std::string error() const { return _context->errstr; }

 private:
  struct FreeContext {
    void operator()(redisContext* context) const { redisFree(context); }
  };
  std::unique_ptr<redisContext, FreeContext> _context;
};

inline std::string text(const redisReply* reply) { return reply && reply->str ? std::string(reply->str, reply->len) : std::string(); }

// A real Mimir cluster: `size` mimir-node processes on loopback, every one a
// founder named in every node's genesis, each bootstrapping to the others. A
// node is stopped by its own process (SIGTERM, then SIGKILL), never by name:
// the build servers run mimir.service.
class Cluster {
 public:
  explicit Cluster(size_t size = 3) : _nodes(size) {
    std::random_device random;
    std::string key(32, '\0');
    for (auto& byte : key) byte = static_cast<char>(random());
    std::ofstream(_dir.path() / "cluster.key", std::ios::binary) << key;
    // The node's own file, so none on the host (/usr/local/etc/mimir) is read.
    std::ofstream(_dir.path() / "config.yaml") << "log:\n  level: ERROR\n";
    for (size_t i = 0; i < size; ++i) {
      _ports.push_back(dannan::test::free_port());
      _cluster_ports.push_back(dannan::test::free_port());
      _ids.push_back(init_state(i));
    }
    for (size_t i = 0; i < size; ++i) start(i);
    for (size_t i = 0; i < size; ++i) wait_ready(i);
  }

  size_t size() const noexcept { return _nodes.size(); }
  uint16_t port(size_t i) const { return _ports.at(i); }

  // Node `i` is told to stop (SIGTERM), as systemd stops it: its exit code
  // (0 when it stopped cleanly), and whether it said so with a `stopped`
  // event; nothing if it had not exited within 20 seconds.
  struct Stopped {
    std::optional<int> code;
    bool said_stopped = false;
  };
  Stopped stop(size_t i) {
    Stopped out;
    _nodes.at(i)->terminate();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (const auto line = _nodes[i]->line(deadline))
      if (line->find("\"event\":\"stopped\"") != std::string::npos) out.said_stopped = true;
    out.code = _nodes[i]->wait(deadline);
    return out;
  }

  // Node `i` dies at once, as a power loss would take it.
  void kill(size_t i) {
    _nodes.at(i)->signal(SIGKILL);
    if (!_nodes[i]->wait(std::chrono::steady_clock::now() + std::chrono::seconds(10))) throw std::runtime_error("node " + std::to_string(i) + " did not die");
  }

  // What node `i` wrote to its log, for a failure's message.
  std::string log(size_t i) const {
    std::ifstream in(log_path(i));
    return std::string(std::istreambuf_iterator<char>(in), {});
  }

 private:
  std::filesystem::path state(size_t i) const { return _dir.path() / ("node-" + std::to_string(i)); }
  std::filesystem::path log_path(size_t i) const { return _dir.path() / ("node-" + std::to_string(i) + ".log"); }

  std::vector<std::string> common(size_t i) const {
    return {MIMIR_NODE_BINARY, "--config", (_dir.path() / "config.yaml").string(), "--cluster-state", state(i).string()};
  }

  std::string init_state(size_t i) {
    auto argv = common(i);
    argv.push_back("--init-state");
    dannan::test::ChildProcess child(argv, log_path(i));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    const auto line = child.line(deadline);
    const auto code = child.wait(deadline);
    const std::string marker = "\"id\":\"";
    const auto at = line ? line->find(marker) : std::string::npos;
    if (!code || *code != 0 || at == std::string::npos) throw std::runtime_error("mimir-node --init-state failed: " + line.value_or("") + log(i));
    return line->substr(at + marker.size(), 32);
  }

  void start(size_t i) {
    auto argv = common(i);
    for (const auto& item :
         std::vector<std::string>{"--listen", "127.0.0.1", "--port", std::to_string(_ports[i]), "--databases", "16", "--cluster-key",
                                  (_dir.path() / "cluster.key").string(), "--cluster-listen", "127.0.0.1", "--cluster-advertise", "127.0.0.1", "--cluster-port",
                                  std::to_string(_cluster_ports[i]), "--cluster-name", "node-" + std::to_string(i), "--seconds", "0"})
      argv.push_back(item);
    for (const auto& id : _ids) {
      argv.push_back("--genesis");
      argv.push_back(id);
    }
    for (size_t j = 0; j < _nodes.size(); ++j)
      if (j != i) {
        argv.push_back("--bootstrap");
        argv.push_back("127.0.0.1:" + std::to_string(_cluster_ports[j]));
      }
    _nodes[i] = std::make_unique<dannan::test::ChildProcess>(argv, log_path(i));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (;;) {
      const auto line = _nodes[i]->line(deadline);
      if (!line) throw std::runtime_error("node " + std::to_string(i) + " never listened: " + log(i));
      if (line->find("\"event\":\"listening\"") != std::string::npos) return;
    }
  }

  // Ready when a write through it commits: the cluster is founded and this
  // node takes part.
  void wait_ready(size_t i) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    std::string last;
    while (std::chrono::steady_clock::now() < deadline) {
      try {
        Client client(_ports[i]);
        const auto reply = client.command({"SET", "mimir-client:ready:" + std::to_string(i), "1"});
        if (reply && reply->type == REDIS_REPLY_STATUS && text(reply.get()) == "OK") return;
        last = reply ? text(reply.get()) : client.error();
      } catch (const std::runtime_error& e) {
        last = e.what();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    throw std::runtime_error("node " + std::to_string(i) + " never took a write: " + last + "\n" + log(i));
  }

  dannan::test::TempDir _dir;
  std::vector<std::unique_ptr<dannan::test::ChildProcess>> _nodes;
  std::vector<uint16_t> _ports;
  std::vector<uint16_t> _cluster_ports;
  std::vector<std::string> _ids;
};

}  // namespace mimir::test
