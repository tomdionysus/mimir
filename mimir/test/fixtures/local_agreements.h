//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "mimir/command/agreements.h"

// The cluster as one in-process register store, for the command tests: every
// change commits on a majority of one, on an io context the test drives, and
// a change of several keys commits atomically, as Dannan's atomic commit
// across replica sets will. Every command test runs through agreement, as
// every mimir-node does (no single-node path; Tom, 2026-11-20).
namespace mimir::test {

class LocalAgreements final : public command::Agreements {
 public:
  explicit LocalAgreements(boost::asio::io_context& io) : _io(io) {}

  std::string operation_id() override { return "local-" + std::to_string(_next++); }

  void change(size_t db, const std::string& key, Change change, std::function<void(Outcome)> done) override {
    boost::asio::post(_io, [this, db, key, change = std::move(change), done = std::move(done)] {
      auto& held = _records[{db, key}];
      auto changed = change(held);
      held = changed.record;
      Outcome outcome;
      outcome.committed = true;
      outcome.record = std::move(changed.record);
      outcome.result = std::move(changed.result);
      outcome.version = version(++_commits);
      boost::asio::post(_io, [done, outcome = std::move(outcome)] { done(outcome); });
    });
  }

  void change_keys(std::vector<Name> names, ChangeKeys change, std::function<void(KeysOutcome)> done) override {
    boost::asio::post(_io, [this, names = std::move(names), change = std::move(change), done = std::move(done)] {
      std::vector<cluster::Record> current;
      for (const auto& name : names) current.push_back(_records[{name.db, name.key}]);
      auto changed = change(current);
      KeysOutcome outcome;
      outcome.committed = true;
      const std::string committed = version(++_commits);
      for (size_t i = 0; i < names.size() && i < changed.records.size(); ++i) {
        _records[{names[i].db, names[i].key}] = changed.records[i];
        outcome.versions.push_back(committed);
      }
      outcome.records = std::move(changed.records);
      outcome.result = std::move(changed.result);
      boost::asio::post(_io, [done, outcome = std::move(outcome)] { done(outcome); });
    });
  }
  bool several_keys() const override { return true; }

  void read(size_t db, const std::string& key, std::function<void(Outcome)> done) override {
    boost::asio::post(_io, [this, db, key, done = std::move(done)] {
      Outcome outcome;
      outcome.committed = true;
      outcome.record = _records[{db, key}];
      outcome.version = version(++_commits);
      done(outcome);
    });
  }

  // One node: no other commits to hear of.
  void on_committed(Committed) override {}

  cluster::Record& record(size_t db, const std::string& key) { return _records[{db, key}]; }

 private:
  // Versions order as commits do: a fixed-width count.
  static std::string version(uint64_t n) {
    std::string out(8, '\0');
    for (int i = 7; i >= 0; --i, n >>= 8) out[static_cast<size_t>(i)] = static_cast<char>(n & 0xff);
    return out;
  }

  boost::asio::io_context& _io;
  std::map<std::pair<size_t, std::string>, cluster::Record> _records;
  uint64_t _next = 1;
  uint64_t _commits = 0;
};

}  // namespace mimir::test
