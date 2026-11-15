//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <string>
#include <vector>

#include "dannan/test/fixtures/stepped_time.h"
#include "mimir/command/database.h"

// One node's database on stepped wall time, and a session, for the command
// tests: run() sends one command and returns the raw reply bytes.
namespace mimir::test {

struct Bed {
  dannan::test::SteppedWall wall;
  command::Database database{wall};
  command::Session session = make_session();

  command::Session make_session() {
    command::Session made;
    made.id = database.next_client_id();
    return made;
  }

  std::string run(std::vector<std::string> args) {
    std::string out;
    database.execute(session, args, out);
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
