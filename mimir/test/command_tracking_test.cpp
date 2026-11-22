//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// Client-side caching (CLIENT TRACKING), as Valkey 8 documents it and Valkey
// 8.1.1 answers it (probed byte for byte): what a connection reads is
// remembered, and a change tells it once; BCAST by prefix; OPTIN and OPTOUT
// with CLIENT CACHING; NOLOOP; RESP2 through a redirected subscriber.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

using Args = std::vector<std::string>;

// A connection that hears what is pushed to it.
struct Client {
  Bed& bed;
  command::Session session;
  std::vector<std::string> pushed;

  explicit Client(Bed& b) : bed(b), session(b.make_session()) {
    session.push = [this](std::string bytes) { pushed.push_back(std::move(bytes)); };
    bed.database.welcome(session);
  }
  std::string run(Args args) { return bed.run_as(session, std::move(args)); }
  std::vector<std::string> take() { return std::exchange(pushed, {}); }
};

std::string invalidate(const std::vector<std::string>& keys) {
  std::string out = ">2\r\n$10\r\ninvalidate\r\n*" + std::to_string(keys.size()) + "\r\n";
  for (const auto& key : keys) out += bulk(key);
  return out;
}
const std::string flushed = ">2\r\n$10\r\ninvalidate\r\n_\r\n";

TEST(CommandTracking, AKeyReadIsInvalidatedOnceWhenChanged) {
  Bed bed;
  Client a(bed), b(bed);
  a.run({"HELLO", "3"});
  EXPECT_EQ(a.run({"CLIENT", "TRACKING", "ON"}), ok());
  EXPECT_EQ(a.run({"CLIENT", "TRACKINGINFO"}), "%3\r\n$5\r\nflags\r\n~1\r\n$2\r\non\r\n$8\r\nredirect\r\n:0\r\n$8\r\nprefixes\r\n*0\r\n");
  EXPECT_EQ(a.run({"CLIENT", "GETREDIR"}), integer(0));
  a.run({"SET", "k", "1"});
  a.run({"GET", "k"});
  a.run({"MGET", "k", "k2"});
  b.run({"SET", "k", "2"});
  EXPECT_EQ(a.take(), std::vector<std::string>{invalidate({"k"})});
  b.run({"SET", "k", "3"});
  EXPECT_TRUE(a.take().empty()) << "told once, then forgotten";
  // The key a missing read named is tracked too.
  b.run({"SET", "k2", "x"});
  EXPECT_EQ(a.take(), std::vector<std::string>{invalidate({"k2"})});
  // Its own change: told after its reply.
  a.run({"GET", "k"});
  EXPECT_EQ(a.run({"SET", "k", "4"}), "+OK\r\n" + invalidate({"k"}));
  // A flush tells every tracking connection, once, with null.
  a.run({"GET", "k"});
  b.run({"FLUSHALL"});
  EXPECT_EQ(a.take(), std::vector<std::string>{flushed});
  // NOLOOP: its own changes are not told.
  EXPECT_EQ(a.run({"CLIENT", "TRACKING", "ON", "NOLOOP"}), ok());
  EXPECT_EQ(a.run({"CLIENT", "TRACKINGINFO"}), "%3\r\n$5\r\nflags\r\n~2\r\n$2\r\non\r\n$6\r\nnoloop\r\n$8\r\nredirect\r\n:0\r\n$8\r\nprefixes\r\n*0\r\n");
  a.run({"GET", "k"});
  EXPECT_EQ(a.run({"SET", "k", "5"}), "+OK\r\n");
  EXPECT_EQ(a.run({"CLIENT", "TRACKING", "OFF"}), ok());
  EXPECT_EQ(a.run({"CLIENT", "GETREDIR"}), integer(-1));
  EXPECT_EQ(a.run({"CLIENT", "TRACKINGINFO"}), "%3\r\n$5\r\nflags\r\n~1\r\n$3\r\noff\r\n$8\r\nredirect\r\n:-1\r\n$8\r\nprefixes\r\n*0\r\n");
}

TEST(CommandTracking, BroadcastByPrefix) {
  Bed bed;
  Client a(bed), b(bed);
  a.run({"HELLO", "3"});
  EXPECT_EQ(a.run({"CLIENT", "TRACKING", "ON", "BCAST", "PREFIX", "user:", "PREFIX", "item:"}), ok());
  EXPECT_EQ(a.run({"CLIENT", "TRACKINGINFO"}),
            "%3\r\n$5\r\nflags\r\n~2\r\n$2\r\non\r\n$5\r\nbcast\r\n$8\r\nredirect\r\n:0\r\n$8\r\nprefixes\r\n*2\r\n" + bulk("item:") + bulk("user:"));
  b.run({"SET", "user:1", "x"});
  EXPECT_EQ(a.take(), std::vector<std::string>{invalidate({"user:1"})});
  b.run({"MSET", "item:1", "x", "other", "y", "user:2", "z"});
  EXPECT_EQ(a.take(), (std::vector<std::string>{invalidate({"item:1"}), invalidate({"user:2"})}));
  EXPECT_EQ(a.run({"CLIENT", "TRACKING", "ON", "BCAST", "PREFIX", "use"}),
            error("ERR Prefix 'use' overlaps with an existing prefix 'user:'. Prefixes for a single client must not overlap."));
  EXPECT_EQ(a.run({"CLIENT", "TRACKING", "ON"}),
            error("ERR You can't switch BCAST mode on/off before disabling tracking for this client, and then re-enabling it with a different mode."));
  a.run({"CLIENT", "TRACKING", "OFF"});
  b.run({"SET", "user:3", "x"});
  EXPECT_TRUE(a.take().empty());
}

TEST(CommandTracking, OptInAndOptOutFollowClientCaching) {
  Bed bed;
  Client a(bed), b(bed);
  a.run({"HELLO", "3"});
  a.run({"CLIENT", "TRACKING", "ON", "OPTIN"});
  EXPECT_EQ(a.run({"CLIENT", "CACHING", "NO"}), error("ERR CLIENT CACHING NO is only valid when tracking is enabled in OPTOUT mode."));
  a.run({"GET", "k"});
  b.run({"SET", "k", "6"});
  EXPECT_TRUE(a.take().empty());
  EXPECT_EQ(a.run({"CLIENT", "CACHING", "YES"}), ok());
  a.run({"GET", "k"});
  b.run({"SET", "k", "7"});
  EXPECT_EQ(a.take(), std::vector<std::string>{invalidate({"k"})});
  // Spent by that one command.
  a.run({"GET", "k"});
  b.run({"SET", "k", "8"});
  EXPECT_TRUE(a.take().empty());
  a.run({"CLIENT", "TRACKING", "OFF"});
  a.run({"CLIENT", "TRACKING", "ON", "OPTOUT"});
  EXPECT_EQ(a.run({"CLIENT", "CACHING", "YES"}), error("ERR CLIENT CACHING YES is only valid when tracking is enabled in OPTIN mode."));
  EXPECT_EQ(a.run({"CLIENT", "CACHING", "NO"}), ok());
  EXPECT_EQ(a.run({"CLIENT", "TRACKINGINFO"}),
            "%3\r\n$5\r\nflags\r\n~3\r\n$2\r\non\r\n$6\r\noptout\r\n$10\r\ncaching-no\r\n$8\r\nredirect\r\n:0\r\n$8\r\nprefixes\r\n*0\r\n");
  a.run({"GET", "k"});
  b.run({"SET", "k", "9"});
  EXPECT_TRUE(a.take().empty());
}

TEST(CommandTracking, RefusesWhatValkeyRefuses) {
  Bed bed;
  Client a(bed);
  EXPECT_EQ(a.run({"CLIENT", "CACHING", "YES"}),
            error("ERR CLIENT CACHING can be called only when the client is in tracking mode with OPTIN or OPTOUT mode enabled"));
  EXPECT_EQ(a.run({"CLIENT", "TRACKING", "ON", "OPTIN", "OPTOUT"}), error("ERR You can't specify both OPTIN mode and OPTOUT mode"));
  EXPECT_EQ(a.run({"CLIENT", "TRACKING", "ON", "PREFIX", "x"}), error("ERR PREFIX option requires BCAST mode to be enabled"));
  EXPECT_EQ(a.run({"CLIENT", "TRACKING", "ON", "REDIRECT", "99999"}), error("ERR The client ID you want redirect to does not exist"));
  EXPECT_EQ(a.run({"CLIENT", "TRACKING", "ON", "BCAST", "OPTIN"}), error("ERR OPTIN and OPTOUT are not compatible with BCAST"));
  EXPECT_EQ(a.run({"CLIENT", "TRACKING", "BOGUS"}), error("ERR syntax error"));
  EXPECT_EQ(a.run({"CLIENT", "TRACKING", "ON", "BOGUS"}), error("ERR syntax error"));
  EXPECT_EQ(a.run({"CLIENT", "TRACKING", "ON", "BCAST", "PREFIX", "ab", "PREFIX", "a"}),
            error("ERR Prefix 'ab' overlaps with another provided prefix 'a'. Prefixes for a single client must not overlap."));
  EXPECT_EQ(a.run({"CLIENT", "TRACKING", "ON", "OPTIN"}), ok());
  EXPECT_EQ(a.run({"CLIENT", "CACHING", "MAYBE"}), error("ERR syntax error"));
  EXPECT_EQ(a.run({"CLIENT", "TRACKING", "ON", "OPTOUT"}),
            error("ERR You can't switch OPTIN/OPTOUT mode before disabling tracking for this client, and then re-enabling it with a different mode."));
}

TEST(CommandTracking, ResptwoIsToldThroughARedirectedSubscriber) {
  Bed bed;
  Client r(bed), c(bed), b(bed), d(bed);
  r.run({"SUBSCRIBE", "__redis__:invalidate"});
  r.take();
  EXPECT_EQ(c.run({"CLIENT", "TRACKING", "ON", "REDIRECT", std::to_string(r.session.id)}), ok());
  EXPECT_EQ(c.run({"CLIENT", "GETREDIR"}), integer(r.session.id));
  EXPECT_EQ(c.run({"CLIENT", "TRACKINGINFO"}),
            "*6\r\n$5\r\nflags\r\n*1\r\n$2\r\non\r\n$8\r\nredirect\r\n:" + std::to_string(r.session.id) + "\r\n$8\r\nprefixes\r\n*0\r\n");
  c.run({"GET", "k"});
  b.run({"SET", "k", "9"});
  EXPECT_EQ(r.take(), std::vector<std::string>{"*3\r\n$7\r\nmessage\r\n$20\r\n__redis__:invalidate\r\n*1\r\n$1\r\nk\r\n"});
  b.run({"FLUSHDB"});
  EXPECT_EQ(r.take(), std::vector<std::string>{"*3\r\n$7\r\nmessage\r\n$20\r\n__redis__:invalidate\r\n$-1\r\n"});
  // RESP2 with no redirection: nothing can be told.
  d.run({"CLIENT", "TRACKING", "ON"});
  d.run({"GET", "k"});
  b.run({"SET", "k", "10"});
  EXPECT_TRUE(d.take().empty());
}

TEST(CommandTracking, ABrokenRedirectionIsTold) {
  Bed bed;
  Client f(bed), b(bed);
  auto e = std::make_unique<Client>(bed);
  const int64_t gone = e->session.id;
  f.run({"HELLO", "3"});
  f.run({"CLIENT", "TRACKING", "ON", "REDIRECT", std::to_string(gone)});
  f.run({"GET", "k"});
  bed.database.forget(e->session);
  e.reset();
  b.run({"SET", "k", "11"});
  EXPECT_EQ(f.take(), std::vector<std::string>{">2\r\n$21\r\ntracking-redir-broken\r\n:" + std::to_string(gone) + "\r\n"});
  EXPECT_EQ(f.run({"CLIENT", "TRACKINGINFO"}),
            "%3\r\n$5\r\nflags\r\n~2\r\n$2\r\non\r\n$15\r\nbroken_redirect\r\n$8\r\nredirect\r\n:" + std::to_string(gone) + "\r\n$8\r\nprefixes\r\n*0\r\n");
}

TEST(CommandTracking, TransactionsAndScriptsReadAndChangeForTheirCaller) {
  Bed bed;
  Client a(bed), b(bed);
  a.run({"HELLO", "3"});
  a.run({"CLIENT", "TRACKING", "ON"});
  a.run({"GET", "k"});
  // A script declares the key it writes (ruling 36).
  b.run({"EVAL", "redis.call('set','k','12')", "1", "k"});
  EXPECT_EQ(a.take(), std::vector<std::string>{invalidate({"k"})});
  a.run({"MULTI"});
  a.run({"GET", "k"});
  a.run({"SET", "k", "13"});
  EXPECT_EQ(a.run({"EXEC"}), "*2\r\n$2\r\n12\r\n+OK\r\n" + invalidate({"k"}));
  // A script's reads are its caller's.
  a.run({"EVAL", "return redis.call('get','k')", "0"});
  b.run({"SET", "k", "14"});
  EXPECT_EQ(a.take(), std::vector<std::string>{invalidate({"k"})});
}

}  // namespace
}  // namespace mimir::test
