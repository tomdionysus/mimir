//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Keyspace notifications, from Valkey 8's documentation ("Keyspace
// notifications"): with K, an event on __keyspace@<db>__:<key> naming the
// event; with E, one on __keyevent@<db>__:<event> naming the key; only the
// classes notify-keyspace-events takes. Expiry is announced however a key is
// found expired, active expiry included; "new" and "keymiss" only when asked.

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "dannan/test/fixtures/stepped_time.h"
#include "mimir/command/database.h"
#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

using namespace std::chrono_literals;

// A node configured with letters notify-keyspace-events does not take is not
// made; with Valkey's letters it answers them back in Valkey's order.
TEST(CommandNotifications, TheSettingTakesOnlyValkeysLetters) {
  command::DatabaseSettings bad;
  bad.notify_keyspace_events = "KQ";
  EXPECT_THROW({ Bed refused(bad); }, std::invalid_argument);
  command::DatabaseSettings good;
  good.notify_keyspace_events = "Elg";
  Bed bed(good);
  EXPECT_EQ(bed.run({"CONFIG", "GET", "notify-keyspace-events"}), array({bulk("notify-keyspace-events"), bulk("glE")}));
}

struct World : Bed {
  command::Session& client = session;
  command::Session listener = make_session();
  std::vector<std::string> events;

  static command::DatabaseSettings settings() {
    command::DatabaseSettings s;
    s.databases = 16;
    return s;
  }
  command::Session make() { return make_session(); }
  World() : Bed(settings()) {
    // Each pushed pattern message as "channel message".
    listener.push = [this](std::string bytes) {
      std::vector<std::string> parts;
      for (size_t at = 0; (at = bytes.find('$', at)) != std::string::npos;) {
        const size_t end = bytes.find("\r\n", at);
        const size_t n = std::stoul(bytes.substr(at + 1, end - at - 1));
        parts.push_back(bytes.substr(end + 2, n));
        at = end + 2 + n;
      }
      if (parts.size() == 4 && parts[0] == "pmessage") events.push_back(parts[2] + " " + parts[3]);
    };
    run_as(listener, {"PSUBSCRIBE", "__key*@*__:*"});
  }
  std::vector<std::string> take() { return std::exchange(events, {}); }
};

using Events = std::vector<std::string>;

TEST(CommandNotifications, OffByDefault) {
  World w;
  w.run({"SET", "a", "1"});
  w.run({"DEL", "a"});
  EXPECT_EQ(w.take(), Events{});
}

TEST(CommandNotifications, KeyspaceAndKeyevent) {
  World w;
  w.run({"CONFIG", "SET", "notify-keyspace-events", "KEA"});
  w.run({"SET", "a", "1"});
  EXPECT_EQ(w.take(), (Events{"__keyspace@0__:a set", "__keyevent@0__:set a"}));
  w.run({"CONFIG", "SET", "notify-keyspace-events", "K$"});
  w.run({"SET", "a", "2"});
  w.run({"DEL", "a"});
  EXPECT_EQ(w.take(), (Events{"__keyspace@0__:a set"}));
  w.run({"CONFIG", "SET", "notify-keyspace-events", "Eg"});
  w.run({"SET", "a", "2"});
  w.run({"DEL", "a"});
  EXPECT_EQ(w.take(), (Events{"__keyevent@0__:del a"}));
}

TEST(CommandNotifications, AnEmptiedKeyIsDeleted) {
  World w;
  w.run({"CONFIG", "SET", "notify-keyspace-events", "Elg"});
  w.run({"RPUSH", "l", "a", "b"});
  w.run({"RPOPLPUSH", "l", "m"});
  w.run({"LMOVE", "l", "m", "LEFT", "RIGHT"});
  // The push is announced before the pop, the emptied source after both.
  EXPECT_EQ(w.take(), (Events{"__keyevent@0__:rpush l", "__keyevent@0__:lpush m", "__keyevent@0__:rpop l", "__keyevent@0__:rpush m", "__keyevent@0__:lpop l",
                              "__keyevent@0__:del l"}));
}

TEST(CommandNotifications, ExpiryHoweverFound) {
  World w;
  w.run({"CONFIG", "SET", "notify-keyspace-events", "Ex"});
  w.run({"SET", "a", "1", "PX", "10"});
  w.run({"SET", "b", "1", "PX", "10"});
  w.run({"SET", "c", "1", "PX", "10"});
  w.wall.advance(20ms);
  w.run({"GET", "a"});
  EXPECT_EQ(w.take(), (Events{"__keyevent@0__:expired a"}));
  // Written over, the key has expired first.
  w.run({"SET", "b", "2"});
  EXPECT_EQ(w.take(), (Events{"__keyevent@0__:expired b"}));
  // Active expiry finds the rest.
  w.database.expire_step(100);
  EXPECT_EQ(w.take(), (Events{"__keyevent@0__:expired c"}));
  // A time already past expires the key at once.
  w.run({"EXPIRE", "b", "0"});
  EXPECT_EQ(w.take(), (Events{"__keyevent@0__:expired b"}));
}

TEST(CommandNotifications, NewAndKeymissOnlyWhenAsked) {
  World w;
  w.run({"CONFIG", "SET", "notify-keyspace-events", "KEA"});
  EXPECT_EQ(w.run({"CONFIG", "GET", "notify-keyspace-events"}), array({bulk("notify-keyspace-events"), bulk("AKE")}));
  w.run({"GET", "nosuch"});
  w.run({"SET", "a", "1"});
  EXPECT_EQ(w.take(), (Events{"__keyspace@0__:a set", "__keyevent@0__:set a"}));
  w.run({"CONFIG", "SET", "notify-keyspace-events", "Enm"});
  EXPECT_EQ(w.run({"CONFIG", "GET", "notify-keyspace-events"}), array({bulk("notify-keyspace-events"), bulk("nEm")}));
  w.run({"GET", "nosuch"});
  w.run({"MGET", "a", "other"});
  w.run({"SET", "b", "1"});
  w.run({"SET", "b", "2"});
  EXPECT_EQ(w.take(), (Events{"__keyevent@0__:keymiss nosuch", "__keyevent@0__:keymiss other", "__keyevent@0__:new b"}));
}

TEST(CommandNotifications, OtherDatabasesAndSwapdb) {
  World w;
  w.run({"CONFIG", "SET", "notify-keyspace-events", "Egx"});
  w.run({"SET", "a", "1"});
  w.run({"MOVE", "a", "3"});
  w.run({"SELECT", "3"});
  w.run({"COPY", "a", "b", "DB", "5"});
  EXPECT_EQ(w.take(), (Events{"__keyevent@0__:move_from a", "__keyevent@3__:move_to a", "__keyevent@5__:copy_to b"}));
  // After SWAPDB a key's expiry is told in the database it is in now.
  w.run({"PEXPIRE", "a", "10"});
  w.take();
  w.run({"SWAPDB", "3", "7"});
  w.wall.advance(20ms);
  w.run({"SELECT", "7"});
  w.run({"GET", "a"});
  EXPECT_EQ(w.take(), (Events{"__keyevent@7__:expired a"}));
}

TEST(CommandNotifications, InvalidFlagsChangeNothing) {
  World w;
  w.run({"CONFIG", "SET", "notify-keyspace-events", "KEA"});
  EXPECT_EQ(w.run({"CONFIG", "SET", "notify-keyspace-events", "KEq"}),
            error("ERR CONFIG SET failed (possibly related to argument 'notify-keyspace-events') - Invalid event class character. Use 'Ag$lshzxeKEtmdn'."));
  EXPECT_EQ(w.run({"CONFIG", "GET", "notify-keyspace-events"}), array({bulk("notify-keyspace-events"), bulk("AKE")}));
}

}  // namespace
}  // namespace mimir::test
