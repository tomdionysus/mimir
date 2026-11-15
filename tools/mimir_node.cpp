//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// mimir-node: one Mimir node. At the single-node stage it serves RESP on its
// own port (6379 by default, and optionally a TLS port) from one in-memory
// keyspace. It reports what it does as one JSON object per line on stdout:
//
//   {"event":"listening","port":N,"tls_port":T}        (zero: that listener is off)
//   {"event":"stopped"}
//
// It runs for --seconds (0: until SIGTERM or SIGINT).
//
// Every setting is also a key in a YAML file (--config PATH, $MIMIR_CONFIG, or
// /usr/local/etc/mimir/config.yaml when it exists), in secrets.yaml beside
// it, or a MIMIR_* variable, the flags winning: `--port 6379` is
// `listen: {port: 6379}` and MIMIR_PORT.
//
//   mimir-node [--listen ADDR] [--port N] [--databases N] [--tls-port N --tls-certificate PEM --tls-key PEM]
//              [--tls-client-ca PEM] [--seconds N] [--log-level LEVEL] [--log-format text|json]
//   mimir-node --version

#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dannan/config/config.h"
#include "dannan/listen/listener.h"
#include "dannan/status/log.h"
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
      Setting{"seconds", Setting::Kind::value, "--seconds", "", "seconds to run, 0 for ever"},
      Setting{"log.level", Setting::Kind::value, "--log-level", "MIMIR_LOG_LEVEL", "ALL, DEBUG, INFO, WARN or ERROR"},
      Setting{"log.format", Setting::Kind::value, "--log-format", "MIMIR_LOG_FORMAT", "text or json"},
      Setting{"version", Setting::Kind::toggle, "--version", "", "print the version and exit"},
  };
  return r;
}

void say(const std::string& line) { std::cout << line << std::endl; }

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto settings = reader().read(std::vector<std::string>(argv + 1, argv + argc));
    if (settings.toggle("version").value_or(false)) {
      std::cout << "mimir-node " << mimir::version << std::endl;
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

    asio::io_context io;
    mimir::command::DatabaseSettings database_settings;
    database_settings.databases = static_cast<size_t>(settings.number("databases", 1, 1024).value_or(1));
    mimir::command::Database database(dannan::contract::system_wall_clock(), database_settings);
    auto server = std::make_shared<mimir::server::Server>(database, logger.child("server"));
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
    say("{\"event\":\"listening\",\"port\":" + std::to_string(plain.port()) + ",\"tls_port\":" + std::to_string(tls ? tls->port() : 0) + "}");
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

    const auto stop = [&] {
      plain.stop();
      if (tls) tls->stop();
      expiry.cancel();
      io.stop();
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
    io.run();
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
