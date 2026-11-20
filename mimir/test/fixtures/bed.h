//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/io_context.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dannan/test/fixtures/stepped_time.h"
#include "mimir/command/database.h"
#include "mimir/test/fixtures/local_agreements.h"

// One node's database on stepped wall time, and a session, for the command
// tests: run() sends one command and returns the raw reply bytes. Every write
// commits by agreement, on an in-process cluster of one (LocalAgreements), as
// on every mimir-node.
namespace mimir::test {

struct Bed {
  explicit Bed(command::DatabaseSettings settings = {}) : database(wall, settings_with(agreements, std::move(settings))) { session = make_session(); }
  Bed(const Bed&) = delete;
  Bed& operator=(const Bed&) = delete;

  boost::asio::io_context io;
  std::shared_ptr<LocalAgreements> agreements = std::make_shared<LocalAgreements>(io);
  dannan::test::SteppedWall wall;
  command::Database database;
  command::Session session;

  static command::DatabaseSettings settings_with(std::shared_ptr<LocalAgreements> agreements, command::DatabaseSettings settings = {}) {
    settings.agreements = std::move(agreements);
    return settings;
  }

  command::Session make_session() {
    command::Session made;
    made.id = database.next_client_id();
    return made;
  }

  // Everything proposed meanwhile, decided.
  void settle() {
    io.restart();
    io.run();
  }

  // The command's reply on `who`, waiting for its agreement if it has one; a
  // command a pause or a lookup postponed runs again, as a connection runs it.
  std::string run_as(command::Session& who, std::vector<std::string> args) {
    for (;;) {
      std::string out;
      std::optional<std::string> woken;
      const auto executed = database.execute(who, args, out, [&woken](std::string reply) { woken = std::move(reply); });
      if (!executed.wait) {
        settle();
        return out;
      }
      io.restart();
      while (!woken && io.run_one() > 0) {
      }
      if (executed.postponed && woken && woken->empty()) continue;
      settle();
      return woken.value_or("<no reply>");
    }
  }

  std::string run(std::vector<std::string> args) { return run_as(session, std::move(args)); }
  // The command from a caller that cannot wait (no wake): a write it cannot
  // commit without waiting is refused, and a blocking one with nothing to
  // take answers its timeout at once.
  std::string run_unwaited(std::vector<std::string> args) {
    std::string out;
    database.execute(session, args, out);
    settle();
    return out;
  }
};

// Reply bytes, for comparing.
inline std::string ok() { return "+OK\r\n"; }
inline std::string nil() { return "$-1\r\n"; }
inline std::string integer(int64_t value) { return ":" + std::to_string(value) + "\r\n"; }
inline std::string bulk(const std::string& value) { return "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n"; }
inline std::string error(const std::string& text) { return "-" + text + "\r\n"; }
inline std::string array(const std::vector<std::string>& items) {
  std::string out = "*" + std::to_string(items.size()) + "\r\n";
  for (const auto& item : items) out += item;
  return out;
}

}  // namespace mimir::test
