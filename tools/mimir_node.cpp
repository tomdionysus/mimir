//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// mimir-node: one Mimir node. It serves RESP on its own port (6379 by
// default, and optionally a TLS port), and is a Dannan node holding its share
// of Mimir's agreement space (stage 3; ruling 34: a lone node is a cluster of
// one). It reports what it does as one JSON object per line on stdout:
//
//   {"event":"listening","port":N,"tls_port":T,"id":"...","cluster_port":C}
//   {"event":"founded","committed":true|false}
//   {"event":"stopped"}
//
// It runs for --seconds (0: until SIGTERM or SIGINT).
//
// The cluster: `cluster: {key: FILE, state: DIR, port: 16379, bootstrap:
// [HOST:PORT...], genesis: [NODE-ID...], shards: 256}`. Without a state
// directory the node is a throwaway cluster of one: a fresh key and a
// temporary state directory, its cluster port on loopback, removed at exit.
// Without `genesis`, a node with no bootstrap peers founds the cluster alone.
//
// A cluster of several founders names each in every founder's `genesis`, so
// each founder's id is made first: `mimir-node --cluster-state DIR
// --init-state` makes DIR and the node's id, prints
// {"event":"initialised","id":"..."} and exits. Its state stays new (its
// acceptors vote at once) until its first start, and never again after.
//
// Every setting is also a key in a YAML file (--config PATH, $MIMIR_CONFIG, or
// /usr/local/etc/mimir/config.yaml when it exists), in secrets.yaml beside
// it, or a MIMIR_* variable, the flags winning: `--port 6379` is
// `listen: {port: 6379}` and MIMIR_PORT.
//
// Users are `acl: {users: [...]}`, each a line of a Valkey ACL file, best kept
// in secrets.yaml (`user alice on >secret ~app:* +@read`); AUTH is taken only
// over TLS unless `auth: {plaintext: true}` (ruling 28).
//
//   mimir-node [--listen ADDR] [--port N] [--databases N] [--tls-port N --tls-certificate PEM --tls-key PEM]
//              [--tls-client-ca PEM] [--auth-plaintext] [--user LINE]... [--seconds N] [--log-level LEVEL]
//              [--log-format text|json]
//   mimir-node --cluster-state DIR --init-state
//   mimir-node --version

#include <signal.h>
#include <stdlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dannan/config/config.h"
#include "dannan/identity/cluster_key.h"
#include "dannan/identity/crypto.h"
#include "dannan/identity/durable_file.h"
#include "dannan/identity/node_identity.h"
#include "dannan/listen/listener.h"
#include "dannan/node.h"
#include "dannan/status/log.h"
#include "dannan/storage/file_system.h"
#include "mimir/cluster/agreements.h"
#include "mimir/cluster/fanout.h"
#include "mimir/cluster/space.h"
#include "mimir/command/database.h"
#include "mimir/server/server.h"
#include "mimir/version.h"

namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;

dannan::config::Reader reader() {
  using dannan::config::Setting;
  dannan::config::Reader r;
  r.config_variable = "MIMIR_CONFIG";
  r.default_path = "/usr/local/etc/mimir/config.yaml";
  r.settings = {
      Setting{"listen.host", Setting::Kind::value, "--listen", "MIMIR_LISTEN", "an address to listen on"},
      Setting{"listen.port", Setting::Kind::value, "--port", "MIMIR_PORT", "a port, 0 to 65535"},
      Setting{"tls.port", Setting::Kind::value, "--tls-port", "MIMIR_TLS_PORT", "a port, 0 to 65535"},
      Setting{"tls.certificate", Setting::Kind::value, "--tls-certificate", "MIMIR_TLS_CERTIFICATE", "a PEM chain's path"},
      Setting{"tls.key", Setting::Kind::value, "--tls-key", "MIMIR_TLS_KEY", "a PEM key's path"},
      Setting{"tls.client_ca", Setting::Kind::value, "--tls-client-ca", "MIMIR_TLS_CLIENT_CA", "a PEM bundle's path"},
      Setting{"databases", Setting::Kind::value, "--databases", "MIMIR_DATABASES", "a count of databases, 1 to 1024"},
      Setting{"auth.plaintext", Setting::Kind::toggle, "--auth-plaintext", "MIMIR_AUTH_PLAINTEXT", "whether AUTH is taken without TLS"},
      Setting{"acl.users", Setting::Kind::list, "--user", "", "lines of a Valkey ACL file: \"user <name> <rules>...\""},
      Setting{"notify.keyspace_events", Setting::Kind::value, "--notify-keyspace-events", "MIMIR_NOTIFY_KEYSPACE_EVENTS",
              "Valkey's notify-keyspace-events letters, as \"Khgx\""},
      Setting{"cluster.key", Setting::Kind::value, "--cluster-key", "MIMIR_CLUSTER_KEY", "the cluster key file"},
      Setting{"cluster.state", Setting::Kind::value, "--cluster-state", "MIMIR_CLUSTER_STATE", "the node's state directory"},
      Setting{"cluster.listen", Setting::Kind::value, "--cluster-listen", "MIMIR_CLUSTER_LISTEN", "an address for the cluster port"},
      Setting{"cluster.port", Setting::Kind::value, "--cluster-port", "MIMIR_CLUSTER_PORT", "the cluster port, 0 to 65535"},
      Setting{"cluster.advertise", Setting::Kind::value, "--cluster-advertise", "MIMIR_CLUSTER_ADVERTISE", "the host peers dial"},
      Setting{"cluster.bootstrap", Setting::Kind::list, "--bootstrap", "", "a peer's cluster port, HOST:PORT"},
      Setting{"cluster.genesis", Setting::Kind::list, "--genesis", "", "a founding node's id, 32 hex digits"},
      Setting{"cluster.shards", Setting::Kind::value, "--shards", "MIMIR_SHARDS", "the shard count, 1 to 8192, fixed for the cluster's life"},
      Setting{"cluster.name", Setting::Kind::value, "--cluster-name", "MIMIR_CLUSTER_NAME", "this node's name"},
      Setting{"cluster.domain", Setting::Kind::value, "--cluster-domain", "MIMIR_CLUSTER_DOMAIN", "this node's failure domain"},
      Setting{"seconds", Setting::Kind::value, "--seconds", "", "seconds to run, 0 for ever"},
      Setting{"log.level", Setting::Kind::value, "--log-level", "MIMIR_LOG_LEVEL", "ALL, DEBUG, INFO, WARN or ERROR"},
      Setting{"log.format", Setting::Kind::value, "--log-format", "MIMIR_LOG_FORMAT", "text or json"},
      Setting{"init_state", Setting::Kind::toggle, "--init-state", "", "make cluster.state and the node's id, print the id, and exit"},
      Setting{"version", Setting::Kind::toggle, "--version", "", "print the version and exit"},
  };
  return r;
}

// Beside the node id of a state made by --init-state and never started: the
// node is new, and votes at once, on its first start alone.
inline constexpr const char* never_started = "never-started";

std::mutex output;

void say(const std::string& line) {
  std::lock_guard lock(output);
  std::cout << line << std::endl;
}

// The io context's threads: joined when the node is done, or when a start
// fails part way, so none is left running.
class IoThreads {
 public:
  IoThreads(asio::io_context& io, int count) : _io(io), _work(asio::make_work_guard(io)) {
    for (int i = 0; i < count; ++i) _threads.emplace_back([this] { _io.run(); });
  }
  ~IoThreads() { join(); }
  IoThreads(const IoThreads&) = delete;
  IoThreads& operator=(const IoThreads&) = delete;
  void join() {
    _work.reset();
    _io.stop();
    for (auto& thread : _threads)
      if (thread.joinable()) thread.join();
  }

 private:
  asio::io_context& _io;
  asio::executor_work_guard<asio::io_context::executor_type> _work;
  std::vector<std::thread> _threads;
};

dannan::net::Endpoint endpoint(const std::string& text) {
  const auto colon = text.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 == text.size()) throw dannan::config::ConfigError("expected HOST:PORT, got " + text);
  const auto port = std::stoul(text.substr(colon + 1));
  if (port > 65535) throw dannan::config::ConfigError("a port is 0 to 65535: " + text);
  return {text.substr(0, colon), static_cast<uint16_t>(port)};
}

// A throwaway node's state directory, removed when it goes.
class TemporaryState {
 public:
  TemporaryState() {
    auto pattern = (std::filesystem::temp_directory_path() / "mimir-node-XXXXXX").string();
    if (!mkdtemp(pattern.data())) throw std::runtime_error("cannot make a temporary state directory");
    _path = pattern;
  }
  ~TemporaryState() {
    std::error_code ignored;
    std::filesystem::remove_all(_path, ignored);
  }
  TemporaryState(const TemporaryState&) = delete;
  TemporaryState& operator=(const TemporaryState&) = delete;
  const std::filesystem::path& path() const noexcept { return _path; }

 private:
  std::filesystem::path _path;
};

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto settings = reader().read(std::vector<std::string>(argv + 1, argv + argc));
    if (settings.toggle("version").value_or(false)) {
      std::cout << "mimir-node " << mimir::version << std::endl;
      return 0;
    }
    if (settings.toggle("init_state").value_or(false)) {
      const auto named = settings.path_of("cluster.state");
      if (!named) throw dannan::config::ConfigError("--init-state needs cluster.state");
      const std::filesystem::path state = *named;
      if (std::filesystem::exists(state / "node-id")) throw dannan::config::ConfigError("cluster.state already holds a node id: " + state.string());
      // The id first: a crash between the two leaves a node that counts as
      // restarted, which catches up before it votes, never the reverse.
      const auto id = dannan::identity::load_or_create_node_id(state);
      dannan::identity::durable_replace_file(state / never_started, "");
      std::cout << "{\"event\":\"initialised\",\"id\":\"" << dannan::identity::to_string(id) << "\"}" << std::endl;
      return 0;
    }
    const auto level = dannan::status::parse_log_level(settings.text("log.level").value_or("INFO"));
    const auto format = dannan::status::parse_log_format(settings.text("log.format").value_or("text"));
    auto sink = std::make_shared<dannan::status::AsyncSink>(dannan::status::console_sink(format, level));
    dannan::status::ScopedLogger logger(sink, "mimir");
    for (const auto& warning : settings.warnings) logger.warn(warning);

    const std::string host = settings.text("listen.host").value_or("0.0.0.0");
    const auto port = static_cast<uint16_t>(settings.number("listen.port", 0, 65535).value_or(6379));
    const auto tls_port = settings.number("tls.port", 0, 65535);
    const auto seconds = settings.number("seconds", 0, 1u << 30).value_or(0);

    // The cluster (stage 3). Its shard count is the agreement table's slot
    // count, the same on every node for the cluster's life (ruling 26).
    const auto shards = static_cast<uint32_t>(settings.number("cluster.shards", 1, dannan::agree::max_slots).value_or(256));
    std::vector<dannan::net::Endpoint> bootstrap;
    for (const auto& text : settings.list("cluster.bootstrap")) bootstrap.push_back(endpoint(text));
    std::vector<dannan::identity::NodeId> genesis;
    for (const auto& text : settings.list("cluster.genesis")) {
      const auto id = dannan::identity::NodeId::parse(text);
      if (!id) throw settings.refuse("cluster.genesis", "a node id, 32 hex digits");
      genesis.push_back(*id);
    }
    dannan::NodeConfig node_config;
    std::optional<TemporaryState> temporary;
    if (const auto state = settings.path_of("cluster.state")) {
      const auto key = settings.path_of("cluster.key");
      if (!key) throw dannan::config::ConfigError("cluster.state needs cluster.key");
      node_config.state_path = *state;
      node_config.key = dannan::identity::load_cluster_key(*key);
      node_config.listen_host = settings.text("cluster.listen").value_or("0.0.0.0");
      node_config.port = static_cast<uint16_t>(settings.number("cluster.port", 0, 65535).value_or(16379));
    } else {
      if (!bootstrap.empty() || !genesis.empty() || settings.text("cluster.key"))
        throw dannan::config::ConfigError("a node that joins or founds a cluster with others needs cluster.state");
      // A throwaway cluster of one: nothing outside this host can reach it.
      temporary.emplace();
      node_config.state_path = temporary->path();
      const auto master = dannan::identity::random_bytes(32);
      std::array<uint8_t, 32> bytes{};
      std::copy(master.begin(), master.end(), bytes.begin());
      node_config.key = dannan::identity::ClusterKey::from_master(bytes);
      node_config.listen_host = "127.0.0.1";
      node_config.port = 0;
    }
    // A node whose state is new never voted, so its acceptors may vote at once:
    // one with no id yet, or one made by --init-state and never started.
    const auto unstarted = node_config.state_path / never_started;
    const bool fresh = !std::filesystem::exists(node_config.state_path / "node-id") || std::filesystem::exists(unstarted);
    const auto self = dannan::identity::load_or_create_node_id(node_config.state_path);
    // Spent, durably, before this node can vote: started once, it is never new
    // again.
    if (std::filesystem::exists(unstarted)) {
      if (!dannan::storage::posix_file_system().remove(unstarted))
        throw std::runtime_error("cannot remove " + unstarted.string() + ": the node would vote as new at its next start");
      dannan::identity::durable_sync_directory(node_config.state_path);
    }
    if (genesis.empty() && bootstrap.empty()) genesis.push_back(self);
    const bool founder = std::find(genesis.begin(), genesis.end(), self) != genesis.end();
    node_config.advertise_host = settings.text("cluster.advertise").value_or("");
    node_config.bootstrap = bootstrap;
    node_config.node_name = settings.text("cluster.name").value_or("");
    node_config.failure_domain = settings.text("cluster.domain").value_or("");
    node_config.version = "mimir-node " + std::string(mimir::version);
    node_config.log = sink;
    node_config.agreement_slots = shards;
    node_config.agreement_genesis = genesis;
    // One rule for every node, a cluster of one included (the maintainer, 2026-11-20: no
    // special case for a single node): a node whose state is not new restarted
    // without its acceptor state, and does not vote until it has caught up.
    if (!fresh) logger.info("this node restarted without its acceptor state: it catches up before it votes");

    signal(SIGPIPE, SIG_IGN);
    asio::io_context io;
    IoThreads threads(io, 2);
    // Destroyed before the io threads end: a node destroyed after its io
    // stopped never returns.
    std::optional<dannan::Node> node;
    node.emplace(io, node_config);
    mimir::cluster::Agreements::register_types(node->context());
    node->start();
    auto space = std::make_unique<mimir::cluster::Space>(node->context(), fresh);
    if (founder)
      space->found(10s, [](dannan::agree::Outcome outcome) {
        say(std::string("{\"event\":\"founded\",\"committed\":") + (outcome.committed ? "true" : "false") + "}");
      });

    mimir::command::DatabaseSettings database_settings;
    database_settings.databases = static_cast<size_t>(settings.number("databases", 1, 1024).value_or(1));
    database_settings.auth_plaintext = settings.toggle("auth.plaintext").value_or(false);
    database_settings.users = settings.list("acl.users");
    database_settings.notify_keyspace_events = settings.text("notify.keyspace_events").value_or("");
    // SHUTDOWN (ruling 31): the stop is posted, never run on the command's
    // thread under the database's lock.
    std::function<void()> stop_node;
    database_settings.shutdown = [&io, &stop_node] { asio::post(io, [&stop_node] { stop_node(); }); };
    // Every write commits by agreement (ruling 34), a lone node's included.
    auto agreements = std::make_shared<mimir::cluster::Agreements>(*space);
    database_settings.agreements = agreements;
    // MIMIR NODES (ruling 40): every node's client address, read from the roster.
    database_settings.nodes = [&space] { return space->client_addresses(); };
    std::unique_ptr<mimir::command::Database> made;
    try {
      made = std::make_unique<mimir::command::Database>(dannan::contract::system_wall_clock(), database_settings);
    } catch (const std::invalid_argument& e) {
      throw dannan::config::ConfigError(e.what());
    }
    mimir::command::Database& database = *made;
    // A member that restarted without its acceptor state catches up before
    // it votes (the restart rule); its view fills as it does.
    if (!space->store().ready())
      agreements->recover([&logger] {
        say("{\"event\":\"caught_up\"}");
        logger.info("caught up with the cluster: this node's acceptors vote again");
      });
    // PUBLISH and keyspace notifications reach every node's subscribers.
    auto fanout = std::make_unique<mimir::cluster::Fanout>(node->context(), database.pubsub());
    auto server = std::make_shared<mimir::server::Server>(io, database, logger.child("server"));
    const auto accepted = [server](std::shared_ptr<dannan::listen::Stream> stream) { server->serve(std::move(stream)); };

    dannan::listen::ListenerConfig plain_config;
    plain_config.name = "resp";
    plain_config.host = host;
    plain_config.port = port;
    dannan::listen::StreamListener plain(io, plain_config);
    plain.start(io.get_executor(), accepted);

    std::unique_ptr<dannan::listen::StreamListener> tls;
    if (tls_port) {
      dannan::listen::CertificatePaths paths;
      paths.chain = settings.path_of("tls.certificate").value_or("");
      paths.key = settings.path_of("tls.key").value_or("");
      if (paths.chain.empty() || paths.key.empty()) throw dannan::config::ConfigError("tls.port needs tls.certificate and tls.key");
      paths.client_ca = settings.text("tls.client_ca").value_or("");
      dannan::listen::ListenerConfig config;
      config.name = "resp-tls";
      config.host = host;
      config.port = static_cast<uint16_t>(*tls_port);
      config.certificates = std::make_shared<dannan::listen::Certificates>(paths);
      config.tls.minimum = dannan::listen::TlsVersion::tls12;
      if (!paths.client_ca.empty()) config.tls.client_certificates = dannan::listen::ClientCertificates::required;
      tls = std::make_unique<dannan::listen::StreamListener>(io, config);
      tls->start(io.get_executor(), accepted);
    }
    // Where clients may reach this node, for every node's MIMIR NODES: the
    // listening host, unless it is a wildcard, then the cluster's advertised
    // host, then the address peers dial.
    const bool wildcard = host == "0.0.0.0" || host == "::" || host.empty();
    space->advertise_clients(node_config.node_name, wildcard ? node_config.advertise_host : host, plain.port(), static_cast<uint16_t>(tls ? tls->port() : 0));
    say("{\"event\":\"listening\",\"port\":" + std::to_string(plain.port()) + ",\"tls_port\":" + std::to_string(tls ? tls->port() : 0) + ",\"id\":\"" +
        dannan::identity::to_string(self) + "\",\"cluster_port\":" + std::to_string(node->port()) + "}");
    logger.info("mimir-node " + std::string(mimir::version) + " listening on " + host + ":" + std::to_string(plain.port()));

    // Active expiry, ten times a second as Valkey's default hz, bounded per
    // turn (speculative work: a turn never holds the database for long).
    asio::steady_timer expiry(io);
    std::function<void()> expire_turn = [&] {
      expiry.expires_after(100ms);
      expiry.async_wait([&](boost::system::error_code error) {
        if (error) return;
        database.expire_step(200);
        expire_turn();
      });
    };
    expire_turn();

    // At a stop, every open connection is closed and given two seconds to
    // finish before the executor stops: one left open holds itself forever.
    std::promise<void> finished;
    asio::steady_timer drain(io);
    int drain_turns = 0;
    std::function<void()> drained = [&] {
      if (server->counters().open == 0 || ++drain_turns > 200) {
        finished.set_value();
        return;
      }
      drain.expires_after(std::chrono::milliseconds(10));
      drain.async_wait([&](boost::system::error_code) { drained(); });
    };
    std::atomic<bool> stopping{false};
    const auto stop = [&] {
      if (stopping.exchange(true)) return;
      plain.stop();
      if (tls) tls->stop();
      expiry.cancel();
      server->close_all();
      drained();
    };
    stop_node = [&] {
      logger.info("SHUTDOWN from a client: stopping");
      stop();
    };
    asio::signal_set signals(io, SIGINT, SIGTERM);
    signals.async_wait([&](boost::system::error_code, int) { stop(); });
    asio::steady_timer deadline(io);
    if (seconds > 0) {
      deadline.expires_after(std::chrono::seconds(seconds));
      deadline.async_wait([&](boost::system::error_code error) {
        if (!error) stop();
      });
    }
    finished.get_future().wait();
    // The cluster goes after the clients, while the io still runs: the node
    // stops first, so no write in flight is proposed again; then the database
    // and its agreements, which hold the space by reference, then the space,
    // then the node. A store completion that runs after them reaches nothing
    // of them (Dannan 0.10.0).
    fanout.reset();
    node->stop();
    made.reset();
    database_settings = {};
    agreements.reset();
    space.reset();
    node.reset();
    threads.join();
    say("{\"event\":\"stopped\"}");
    return 0;
  } catch (const dannan::config::ConfigError& e) {
    std::cerr << "mimir-node: " << e.what() << std::endl;
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "mimir-node: " << e.what() << std::endl;
    return 1;
  }
}
