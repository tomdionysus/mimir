//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The differential gate: the same commands sent to a running mimir-node and to
// a Valkey 8 server, and their replies compared byte for byte. "Compatible"
// is Valkey 8's behaviour, and this is where the documentation's reading is
// checked against the server itself. Run by hand on a build server, with both
// listening on loopback:
//
//   MIMIR_DIFF_MIMIR_PORT=7380 MIMIR_DIFF_VALKEY_PORT=7379 ./mimir_tests --gtest_filter='Differential.*'
//
// Skipped when either port is not given. Only deterministic replies are
// compared: no clocks, ids, random keys or orders the server chooses.

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mimir {
namespace {

namespace asio = boost::asio;

std::optional<uint16_t> port_from(const char* variable) {
  const char* text = std::getenv(variable);
  if (!text || !*text) return std::nullopt;
  return static_cast<uint16_t>(std::stoi(text));
}

// A client that reads one whole reply at a time, whatever its shape.
class Client {
 public:
  explicit Client(uint16_t port) {
    _socket.connect(asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port));
    timeval limit{5, 0};
    setsockopt(_socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
  }

  std::string call(const std::vector<std::string>& args) {
    send(args);
    return read_reply();
  }
  void send(const std::vector<std::string>& args) {
    std::string request = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& arg : args) request += "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
    asio::write(_socket, asio::buffer(request));
  }
  // The next whole reply or pushed message.
  std::string read() { return read_reply(); }

 private:
  std::string line() {
    for (;;) {
      const size_t end = _buffer.find("\r\n");
      if (end != std::string::npos) {
        std::string out = _buffer.substr(0, end + 2);
        _buffer.erase(0, end + 2);
        return out;
      }
      fill();
    }
  }
  std::string bytes(size_t count) {
    while (_buffer.size() < count) fill();
    std::string out = _buffer.substr(0, count);
    _buffer.erase(0, count);
    return out;
  }
  void fill() {
    char chunk[4096];
    boost::system::error_code error;
    const size_t got = _socket.read_some(asio::buffer(chunk), error);
    if (error) throw std::runtime_error("read: " + error.message());
    _buffer.append(chunk, got);
  }
  std::string read_reply() {
    std::string head = line();
    const char type = head[0];
    const auto count = [&] { return std::stoll(head.substr(1, head.size() - 3)); };
    switch (type) {
      case '$':
      case '=':
      case '!': {
        const long long n = count();
        if (n < 0) return head;
        return head + bytes(static_cast<size_t>(n) + 2);
      }
      case '*':
      case '~':
      case '>': {
        if (head == "*-1\r\n") return head;
        const long long n = count();
        std::string out = head;
        for (long long i = 0; i < n; ++i) out += read_reply();
        return out;
      }
      case '%': {
        const long long n = count();
        std::string out = head;
        for (long long i = 0; i < 2 * n; ++i) out += read_reply();
        return out;
      }
      default:
        return head;
    }
  }

  asio::io_context _io;
  asio::ip::tcp::socket _socket{_io};
  std::string _buffer;
};

class Differential : public ::testing::Test {
 protected:
  void SetUp() override {
    auto mimir_port = port_from("MIMIR_DIFF_MIMIR_PORT");
    auto valkey_port = port_from("MIMIR_DIFF_VALKEY_PORT");
    if (!mimir_port || !valkey_port) GTEST_SKIP() << "MIMIR_DIFF_MIMIR_PORT and MIMIR_DIFF_VALKEY_PORT are not set";
    _mimir.emplace(*mimir_port);
    _valkey.emplace(*valkey_port);
    same({"FLUSHALL"});
  }

  // Sends `args` to both and expects the same reply.
  void same(const std::vector<std::string>& args) {
    const std::string valkey = _valkey->call(args);
    const std::string mimir = _mimir->call(args);
    std::string shown;
    for (const auto& arg : args) shown += arg + " ";
    EXPECT_EQ(mimir, valkey) << shown;
  }
  // Sends `args` to both and compares nothing.
  void both(const std::vector<std::string>& args) {
    _valkey->call(args);
    _mimir->call(args);
  }
  void script(const std::vector<std::vector<std::string>>& commands) {
    for (const auto& args : commands) same(args);
  }

  std::optional<Client> _mimir;
  std::optional<Client> _valkey;
};

TEST_F(Differential, Strings) {
  script({
      {"GET", "k"},
      {"SET", "k", "v"},
      {"GET", "k"},
      {"SET", "k", "w", "NX"},
      {"SET", "k", "w", "XX", "GET"},
      {"SET", "k", "x", "NX", "GET"},
      {"SET", "k", "v", "NX", "XX"},
      {"SET", "k", "v", "EX", "0"},
      {"SET", "k", "v", "EX", "ten"},
      {"SET", "k", "v", "EX", "9223372036854775807"},
      {"SET", "k", "v", "PXAT", "1"},
      {"EXISTS", "k"},
      {"SET", "k", "v", "KEEPTTL", "EX", "5"},
      {"SET", "k", "v", "BOGUS"},
      {"SETNX", "a", "1"},
      {"SETNX", "a", "2"},
      {"SETEX", "a", "0", "x"},
      {"PSETEX", "a", "-1", "x"},
      {"GETSET", "a", "3"},
      {"GETDEL", "a"},
      {"GETDEL", "a"},
      {"MSET", "a", "1", "b", "2"},
      {"MSET", "a", "1", "b"},
      {"MGET", "a", "b", "c"},
      {"MSETNX", "a", "1", "c", "2"},
      {"MSETNX", "c", "1", "d", "2"},
      {"INCR", "n"},
      {"INCRBY", "n", "41"},
      {"DECRBY", "n", "-9223372036854775808"},
      {"INCRBY", "n", "1.5"},
      {"SET", "big", "9223372036854775807"},
      {"INCR", "big"},
      {"SET", "s", "01"},
      {"INCR", "s"},
      {"SET", "f", "10.50"},
      {"INCRBYFLOAT", "f", "0.1"},
      {"INCRBYFLOAT", "f", "-5"},
      {"SET", "f", "5.0e3"},
      {"INCRBYFLOAT", "f", "2.0e2"},
      {"INCRBYFLOAT", "f", "x"},
      {"INCRBYFLOAT", "f", "inf"},
      {"APPEND", "ap", "Hello"},
      {"APPEND", "ap", " World"},
      {"STRLEN", "ap"},
      {"GETRANGE", "ap", "0", "3"},
      {"GETRANGE", "ap", "-3", "-1"},
      {"GETRANGE", "ap", "-1", "-5"},
      {"GETRANGE", "ap", "5", "3"},
      {"GETRANGE", "ap", "10", "100"},
      {"SUBSTR", "ap", "0", "3"},
      {"SETRANGE", "ap", "6", "Redis"},
      {"SETRANGE", "sr", "3", "x"},
      {"GET", "sr"},
      {"SETRANGE", "empty", "0", ""},
      {"SETRANGE", "ap", "-1", "x"},
      {"SETRANGE", "ap", "536870912", "x"},
      {"GETEX", "ap", "PERSIST", "EX", "1"},
      {"GETEX", "ap", "EX", "0"},
      {"GETEX", "nosuch", "EX", "10"},
      {"GETEX", "ap", "EX", "100"},
      {"TTL", "ap"},
      {"GETEX", "ap", "PERSIST"},
      {"TTL", "ap"},
  });
}

TEST_F(Differential, Keys) {
  script({
      {"SET", "k", "v"},
      {"EXISTS", "k", "k", "nosuch"},
      {"TYPE", "k"},
      {"TYPE", "nosuch"},
      {"EXPIRE", "k", "100"},
      {"TTL", "k"},
      {"EXPIRE", "k", "100", "NX"},
      {"EXPIRE", "k", "50", "GT"},
      {"EXPIRE", "k", "200", "GT"},
      {"EXPIRE", "k", "300", "LT"},
      {"TTL", "k"},
      {"EXPIRE", "k", "1", "NX", "XX"},
      {"EXPIRE", "k", "1", "GT", "LT"},
      {"EXPIRE", "k", "1", "FOO"},
      {"EXPIRE", "k", "9223372036854775807"},
      {"PERSIST", "k"},
      {"PERSIST", "k"},
      {"TTL", "k"},
      {"TTL", "nosuch"},
      {"EXPIRETIME", "k"},
      {"EXPIRETIME", "nosuch"},
      {"PEXPIREAT", "k", "0"},
      {"EXISTS", "k"},
      {"SET", "a", "1"},
      {"RENAME", "a", "b"},
      {"RENAME", "a", "b"},
      {"RENAME", "b", "b"},
      {"SET", "c", "3"},
      {"RENAMENX", "b", "c"},
      {"RENAMENX", "b", "d"},
      {"COPY", "d", "e"},
      {"COPY", "d", "e"},
      {"COPY", "d", "e", "REPLACE"},
      {"COPY", "d", "d"},
      // Run against a mimir-node with --databases 16, as standalone Valkey has.
      {"COPY", "d", "x", "DB", "1"},
      {"COPY", "d", "d", "DB", "1"},
      {"MOVE", "d", "2"},
      {"MOVE", "d", "2"},
      {"MOVE", "d", "0"},
      {"MOVE", "x", "99"},
      {"SWAPDB", "0", "2"},
      {"EXISTS", "d"},
      {"SWAPDB", "0", "2"},
      {"SWAPDB", "x", "1"},
      {"SWAPDB", "0", "99"},
      {"SELECT", "15"},
      {"SELECT", "16"},
      {"SET", "in15", "v"},
      {"DBSIZE"},
      {"FLUSHDB"},
      {"SELECT", "0"},
      {"CONFIG", "GET", "databases"},
      {"DEL", "c", "d", "e", "nosuch"},
      {"DBSIZE"},
      {"SCAN", "x"},
      {"SCAN", "0", "COUNT", "0"},
      {"SCAN", "0", "TYPE", "foo"},
      {"SCAN", "0", "BOGUS"},
      {"KEYS", "nomatch*"},
      {"FLUSHDB", "BOGUS"},
      {"FLUSHALL"},
      {"RANDOMKEY"},
  });
}

TEST_F(Differential, Lists) {
  script({
      {"RPUSH", "l", "a", "b", "c", "b", "a"},
      {"LPUSH", "l", "z"},
      {"LPUSHX", "nosuch", "x"},
      {"RPUSHX", "l", "y"},
      {"LRANGE", "l", "0", "-1"},
      {"LRANGE", "l", "-100", "100"},
      {"LRANGE", "l", "5", "2"},
      {"LRANGE", "l", "x", "1"},
      {"LLEN", "l"},
      {"LINDEX", "l", "-1"},
      {"LINDEX", "l", "99"},
      {"LINDEX", "nosuch", "x"},
      {"LSET", "l", "0", "first"},
      {"LSET", "l", "99", "x"},
      {"LSET", "nosuch", "0", "x"},
      {"LINSERT", "l", "BEFORE", "b", "before-b"},
      {"LINSERT", "l", "AFTER", "nope", "x"},
      {"LINSERT", "nosuch", "AFTER", "a", "x"},
      {"LINSERT", "l", "BESIDE", "a", "x"},
      {"LPOS", "l", "b"},
      {"LPOS", "l", "b", "RANK", "-1"},
      {"LPOS", "l", "b", "COUNT", "0"},
      {"LPOS", "l", "b", "RANK", "0"},
      {"LPOS", "l", "b", "COUNT", "-1"},
      {"LPOS", "l", "b", "MAXLEN", "-1"},
      {"LPOS", "l", "b", "MAXLEN", "2"},
      {"LPOS", "nosuch", "b", "COUNT", "2"},
      {"LREM", "l", "-1", "a"},
      {"LREM", "l", "0", "b"},
      {"LRANGE", "l", "0", "-1"},
      {"LTRIM", "l", "1", "-2"},
      {"LRANGE", "l", "0", "-1"},
      {"LPOP", "l"},
      {"LPOP", "l", "2"},
      {"RPOP", "l", "0"},
      {"LPOP", "l", "-1"},
      {"LPOP", "nosuch", "1"},
      {"LPOP", "nosuch"},
      {"RPUSH", "m", "1", "2", "3"},
      {"LMOVE", "m", "n", "RIGHT", "LEFT"},
      {"LMOVE", "m", "m", "LEFT", "RIGHT"},
      {"LMOVE", "m", "n", "UP", "LEFT"},
      {"RPOPLPUSH", "m", "n"},
      {"LRANGE", "n", "0", "-1"},
      {"SET", "s", "v"},
      {"LMOVE", "m", "s", "LEFT", "LEFT"},
      {"LPUSH", "s", "x"},
      {"GET", "n"},
      {"LMPOP", "2", "nosuch", "n", "LEFT", "COUNT", "10"},
      {"LMPOP", "2", "nosuch", "n", "LEFT"},
      {"LMPOP", "0", "n", "LEFT"},
      {"LMPOP", "1", "n", "LEFT", "COUNT", "0"},
      {"LMPOP", "5", "n", "LEFT"},
      {"LMPOP", "1", "n", "MIDDLE"},
      {"LTRIM", "m", "5", "10"},
      {"EXISTS", "m", "n"},
      // Not SCAN: its order is the server's own, and Mimir's follows hash slots.
      {"DBSIZE"},
  });
}

TEST_F(Differential, Hashes) {
  script({
      {"HSET", "h", "b", "1", "a", "2", "c", "3"},
      {"HSET", "h", "b", "4"},
      {"HSET", "h", "x"},
      {"HGETALL", "h"},
      {"HKEYS", "h"},
      {"HVALS", "h"},
      {"HDEL", "h", "a", "nofield"},
      {"HGETALL", "h"},
      {"HSET", "h", "a", "5"},
      {"HGETALL", "h"},
      {"HMSET", "h2", "f", "v"},
      {"HMGET", "h", "a", "nofield", "c"},
      {"HGET", "h", "nofield"},
      {"HSETNX", "h", "a", "x"},
      {"HSETNX", "h", "new", "x"},
      {"HLEN", "h"},
      {"HSTRLEN", "h", "new"},
      {"HEXISTS", "h", "new"},
      {"HINCRBY", "h", "n", "5"},
      {"HINCRBY", "h", "n", "-10"},
      {"HINCRBY", "h", "new", "1"},
      {"HINCRBY", "h", "n", "x"},
      {"HSET", "h", "max", "9223372036854775807"},
      {"HINCRBY", "h", "max", "1"},
      {"HSET", "h", "fl", "10.50"},
      {"HINCRBYFLOAT", "h", "fl", "0.1"},
      {"HINCRBYFLOAT", "h", "new", "1"},
      {"HINCRBYFLOAT", "h", "fl", "x"},
      {"HRANDFIELD", "nosuch"},
      {"HRANDFIELD", "nosuch", "3"},
      {"HRANDFIELD", "h", "0"},
      {"HRANDFIELD", "h", "1", "BOGUS"},
      {"HRANDFIELD", "h", "x"},
      {"HRANDFIELD", "h", "100", "WITHVALUES"},
      {"HSCAN", "h", "0"},
      {"HSCAN", "h", "0", "MATCH", "n*", "NOVALUES"},
      {"HSCAN", "h", "x"},
      {"HSCAN", "h", "0", "COUNT", "0"},
      {"HSCAN", "nosuch", "0"},
      {"HGET", "nosuch", "f"},
      {"SET", "s", "v"},
      {"HSET", "s", "f", "v"},
      {"HGETALL", "s"},
      {"GET", "h"},
      {"HDEL", "h2", "f"},
      {"EXISTS", "h2"},
      {"DBSIZE"},
  });
}

TEST_F(Differential, Sets) {
  script({
      {"SADD", "nums", "10", "-3", "2"},
      {"SMEMBERS", "nums"},
      {"SADD", "nums", "07"},
      {"SMEMBERS", "nums"},
      {"SADD", "words", "pear", "apple", "fig"},
      {"SMEMBERS", "words"},
      {"SADD", "words", "apple"},
      {"SREM", "words", "apple", "nope"},
      {"SMEMBERS", "words"},
      {"SISMEMBER", "words", "fig"},
      {"SMISMEMBER", "words", "fig", "nope"},
      {"SCARD", "words"},
      {"SPOP", "nosuch"},
      {"SPOP", "nosuch", "2"},
      {"SPOP", "words", "-1"},
      {"SPOP", "words", "0"},
      {"SRANDMEMBER", "nosuch"},
      {"SRANDMEMBER", "nosuch", "3"},
      {"SRANDMEMBER", "words", "0"},
      {"SRANDMEMBER", "words", "10"},
      {"SRANDMEMBER", "nums", "10"},
      {"SADD", "a", "1", "2", "3", "4"},
      {"SADD", "b", "3", "4", "5"},
      {"SADD", "c", "x", "3"},
      {"SINTER", "a", "b"},
      {"SINTER", "a", "nosuch"},
      {"SINTERSTORE", "d", "a", "b"},
      {"SMEMBERS", "d"},
      {"SUNIONSTORE", "d", "a", "b"},
      {"SMEMBERS", "d"},
      {"SDIFFSTORE", "d", "a", "b"},
      {"SMEMBERS", "d"},
      {"SDIFFSTORE", "d", "b", "b"},
      {"EXISTS", "d"},
      {"SINTERCARD", "2", "a", "b"},
      {"SINTERCARD", "2", "a", "b", "LIMIT", "1"},
      {"SINTERCARD", "0", "a"},
      {"SINTERCARD", "3", "a", "b"},
      {"SINTERCARD", "2", "a", "b", "LIMIT", "-1"},
      {"SMOVE", "a", "c", "1"},
      {"SMOVE", "a", "c", "nope"},
      {"SMOVE", "a", "a", "2"},
      {"SMEMBERS", "c"},
      {"SSCAN", "words", "0"},
      {"SSCAN", "words", "0", "MATCH", "p*"},
      {"SSCAN", "words", "x"},
      {"SET", "s", "v"},
      {"SADD", "s", "x"},
      {"SINTER", "a", "s"},
      {"SMOVE", "a", "s", "2"},
      {"SUNIONSTORE", "s", "b"},
      {"TYPE", "s"},
      {"DBSIZE"},
  });
}

TEST_F(Differential, SortedSets) {
  script({
      {"ZADD", "z", "1", "one", "1", "uno", "2", "two", "3", "three", "0.1", "tenth", "-inf", "low", "+inf", "high"},
      {"ZRANGE", "z", "0", "-1", "WITHSCORES"},
      {"ZRANGE", "z", "1", "3", "REV"},
      {"ZRANGE", "z", "(1", "3", "BYSCORE", "WITHSCORES"},
      {"ZRANGE", "z", "3", "(1", "BYSCORE", "REV"},
      {"ZRANGE", "z", "-inf", "+inf", "BYSCORE", "LIMIT", "1", "2"},
      {"ZRANGE", "z", "-inf", "+inf", "BYSCORE", "LIMIT", "-1", "2"},
      {"ZRANGE", "z", "0", "1", "LIMIT", "0", "1"},
      {"ZRANGE", "z", "-", "+", "BYLEX", "WITHSCORES"},
      {"ZRANGE", "z", "x", "1"},
      {"ZRANGEBYSCORE", "z", "1", "2", "WITHSCORES"},
      {"ZRANGEBYSCORE", "z", "x", "2"},
      {"ZREVRANGEBYSCORE", "z", "+inf", "(1", "LIMIT", "0", "2"},
      {"ZREVRANGE", "z", "0", "2", "WITHSCORES"},
      {"ZSCORE", "z", "tenth"},
      {"ZSCORE", "z", "high"},
      {"ZSCORE", "z", "nope"},
      {"ZMSCORE", "z", "one", "nope", "low"},
      {"ZCARD", "z"},
      {"ZCOUNT", "z", "(1", "+inf"},
      {"ZCOUNT", "z", "x", "1"},
      {"ZRANK", "z", "two"},
      {"ZRANK", "z", "two", "WITHSCORE"},
      {"ZRANK", "z", "nope"},
      {"ZRANK", "z", "nope", "WITHSCORE"},
      {"ZREVRANK", "z", "two"},
      {"ZADD", "z", "NX", "5", "one", "4", "four"},
      {"ZADD", "z", "XX", "CH", "5", "one", "9", "nine"},
      {"ZADD", "z", "GT", "CH", "1", "one", "8", "two"},
      {"ZADD", "z", "LT", "4", "three"},
      {"ZADD", "z", "INCR", "0.5", "four"},
      {"ZADD", "z", "XX", "INCR", "1", "nope"},
      {"ZADD", "z", "NX", "XX", "1", "a"},
      {"ZADD", "z", "GT", "LT", "1", "a"},
      {"ZADD", "z", "INCR", "1", "a", "2", "b"},
      {"ZADD", "z", "1"},
      {"ZADD", "z", "x", "a"},
      {"ZADD", "z", "nan", "a"},
      {"ZINCRBY", "z", "1.25", "one"},
      {"ZINCRBY", "z", "-inf", "high"},
      {"ZRANGE", "z", "0", "-1", "WITHSCORES"},
      {"ZADD", "lex", "0", "a", "0", "b", "0", "c", "0", "d", "0", "e"},
      {"ZRANGEBYLEX", "lex", "-", "[c"},
      {"ZRANGEBYLEX", "lex", "(a", "+", "LIMIT", "1", "2"},
      {"ZREVRANGEBYLEX", "lex", "[d", "(a"},
      {"ZRANGEBYLEX", "lex", "a", "c"},
      {"ZLEXCOUNT", "lex", "[b", "[d"},
      {"ZREMRANGEBYLEX", "lex", "[b", "(d"},
      {"ZRANGE", "lex", "0", "-1"},
      {"ZREMRANGEBYRANK", "z", "0", "1"},
      {"ZREMRANGEBYSCORE", "z", "(4", "5"},
      {"ZRANGE", "z", "0", "-1", "WITHSCORES"},
      {"ZREM", "z", "nine", "nope"},
      {"ZPOPMIN", "z"},
      {"ZPOPMAX", "z", "2"},
      {"ZPOPMIN", "nosuch"},
      {"ZPOPMIN", "nosuch", "2"},
      {"ZPOPMIN", "z", "-1"},
      {"ZADD", "a", "1", "one", "2", "two"},
      {"ZADD", "b", "1", "one", "2", "two", "3", "three"},
      {"SADD", "plain", "one", "four"},
      {"ZUNION", "2", "a", "b", "WITHSCORES"},
      {"ZUNION", "3", "a", "b", "plain", "WITHSCORES", "AGGREGATE", "MAX"},
      {"ZINTER", "2", "a", "b", "WEIGHTS", "2", "3", "WITHSCORES"},
      {"ZDIFF", "2", "b", "a", "WITHSCORES"},
      {"ZUNIONSTORE", "out", "2", "a", "b", "WEIGHTS", "2", "3"},
      {"ZRANGE", "out", "0", "-1", "WITHSCORES"},
      {"ZINTERSTORE", "out", "2", "a", "b", "AGGREGATE", "MIN"},
      {"ZDIFFSTORE", "out", "2", "a", "b"},
      {"EXISTS", "out"},
      {"ZINTERCARD", "2", "a", "b"},
      {"ZINTERCARD", "2", "a", "b", "LIMIT", "1"},
      {"ZUNIONSTORE", "out", "0", "a"},
      {"ZUNION", "2", "a", "b", "WEIGHTS", "1", "x"},
      {"ZDIFF", "2", "a", "b", "AGGREGATE", "MAX"},
      {"ZMPOP", "2", "nosuch", "b", "MAX", "COUNT", "2"},
      {"ZMPOP", "1", "nosuch", "MIN"},
      {"ZRANGESTORE", "dst", "b", "0", "0"},
      {"ZRANGE", "dst", "0", "-1", "WITHSCORES"},
      {"ZRANDMEMBER", "nosuch"},
      {"ZRANDMEMBER", "nosuch", "2"},
      {"ZRANDMEMBER", "a", "10", "WITHSCORES"},
      {"ZSCAN", "a", "0"},
      {"ZSCAN", "a", "0", "NOSCORES"},
      {"SET", "s", "v"},
      {"ZADD", "s", "1", "a"},
      {"ZUNION", "2", "a", "s"},
  });
  both({"HELLO", "3"});
  script({{"ZRANGE", "a", "0", "-1", "WITHSCORES"}, {"ZSCORE", "a", "one"}, {"ZPOPMIN", "a"}, {"ZPOPMIN", "a", "1"}, {"ZRANK", "b", "two", "WITHSCORE"}});
  both({"HELLO", "2"});
}

TEST_F(Differential, BlockingPopsThatAnswerAtOnce) {
  script({
      {"RPUSH", "l", "a", "b", "c"},
      {"BLPOP", "nosuch", "l", "0"},
      {"BRPOP", "l", "0"},
      {"BLMOVE", "l", "m", "LEFT", "RIGHT", "0"},
      {"BRPOPLPUSH", "m", "l", "0"},
      {"BLMPOP", "0", "2", "nosuch", "l", "LEFT", "COUNT", "5"},
      {"ZADD", "z", "1", "a", "2", "b"},
      {"BZPOPMIN", "z", "0"},
      {"BZPOPMAX", "nosuch", "z", "0"},
      {"ZADD", "z", "3", "c", "4", "d"},
      {"BZMPOP", "0", "1", "z", "MAX", "COUNT", "1"},
      {"BLPOP", "l", "-1"},
      {"BLPOP", "l", "soon"},
      {"BZPOPMIN", "z", "inf"},
      {"BLMOVE", "a", "b", "UP", "LEFT", "0"},
      {"BLMPOP", "0", "0", "a", "LEFT"},
      {"SET", "s", "v"},
      {"BLPOP", "s", "0"},
      {"BZPOPMIN", "s", "0"},
  });
}

TEST_F(Differential, Bitmaps) {
  script({
      {"SETBIT", "b", "7", "1"},
      {"SETBIT", "b", "100", "1"},
      {"GET", "b"},
      {"GETBIT", "b", "100"},
      {"GETBIT", "b", "1000"},
      {"SETBIT", "b", "0", "2"},
      {"SETBIT", "b", "-1", "1"},
      {"SETBIT", "b", "4294967296", "1"},
      {"SET", "f", "foobar"},
      {"BITCOUNT", "f"},
      {"BITCOUNT", "f", "1", "1"},
      {"BITCOUNT", "f", "5", "30", "BIT"},
      {"BITCOUNT", "f", "-2", "-1"},
      {"BITCOUNT", "f", "3", "1"},
      {"BITCOUNT", "f", "0"},
      {"BITCOUNT", "f", "0", "1", "WORD"},
      {"BITPOS", "f", "1"},
      {"BITPOS", "f", "0", "2"},
      {"BITPOS", "f", "1", "7", "15", "BIT"},
      {"BITPOS", "f", "1", "-1", "-1"},
      {"SET", "ones", "\xff\xff"},
      {"BITPOS", "ones", "0"},
      {"BITPOS", "ones", "0", "0", "-1"},
      {"BITPOS", "nosuch", "0"},
      {"BITPOS", "nosuch", "1"},
      {"BITPOS", "f", "2"},
      {"SET", "g", "abcdef"},
      {"BITOP", "AND", "d", "f", "g"},
      {"GET", "d"},
      {"BITOP", "XOR", "d", "f", "g", "nosuch"},
      {"GET", "d"},
      {"BITOP", "NOT", "d", "f"},
      {"GET", "d"},
      {"BITOP", "NOT", "d", "f", "g"},
      {"BITOP", "NAND", "d", "f"},
      {"BITOP", "OR", "d", "nosuch"},
      {"EXISTS", "d"},
      {"BITFIELD", "bf", "INCRBY", "i5", "100", "1", "GET", "u4", "0"},
      {"BITFIELD", "bf", "SET", "i8", "#0", "100", "SET", "i8", "#1", "200", "GET", "i8", "#1", "GET", "u8", "8"},
      {"BITFIELD", "bf", "INCRBY", "u2", "100", "1", "OVERFLOW", "SAT", "INCRBY", "u2", "102", "1"},
      {"BITFIELD", "bf", "INCRBY", "u2", "100", "1", "OVERFLOW", "SAT", "INCRBY", "u2", "102", "1"},
      {"BITFIELD", "bf", "INCRBY", "u2", "100", "1", "OVERFLOW", "SAT", "INCRBY", "u2", "102", "1"},
      {"BITFIELD", "bf", "INCRBY", "u2", "100", "1", "OVERFLOW", "SAT", "INCRBY", "u2", "102", "1"},
      {"BITFIELD", "bf", "OVERFLOW", "FAIL", "INCRBY", "u2", "102", "1"},
      {"BITFIELD", "bf", "OVERFLOW", "WRAP", "INCRBY", "i8", "0", "200", "INCRBY", "i64", "8", "9223372036854775807", "INCRBY", "i64", "8", "1"},
      {"BITFIELD", "bf", "OVERFLOW", "SAT", "INCRBY", "i8", "0", "-500", "SET", "u3", "0", "100"},
      {"BITFIELD", "bf", "GET", "u64", "0"},
      {"BITFIELD", "bf", "GET", "i65", "0"},
      {"BITFIELD", "bf", "OVERFLOW", "MAYBE"},
      {"BITFIELD", "bf", "GET", "u8"},
      {"BITFIELD_RO", "bf", "GET", "i8", "0"},
      {"BITFIELD_RO", "bf", "SET", "i8", "0", "1"},
      {"BITFIELD", "nosuch", "GET", "u8", "0"},
      {"GET", "bf"},
      {"RPUSH", "l", "x"},
      {"SETBIT", "l", "0", "1"},
      {"BITCOUNT", "l"},
  });
}

TEST_F(Differential, HyperLogLog) {
  script({
      {"PFADD", "h", "a", "b", "c", "d", "e", "f", "g"},
      {"PFCOUNT", "h"},
      {"PFADD", "h", "a"},
      {"PFADD", "empty"},
      {"PFADD", "empty"},
      {"PFCOUNT", "empty"},
      {"GET", "empty"},
      {"PFCOUNT", "nosuch"},
      {"PFADD", "h1", "foo", "bar", "zap", "a"},
      {"PFADD", "h2", "a", "b", "c", "foo"},
      {"PFCOUNT", "h1", "h2"},
      {"PFMERGE", "h3", "h1", "h2"},
      {"PFCOUNT", "h3"},
      {"PFMERGE", "made"},
      {"PFCOUNT", "made"},
      {"SET", "s", "plain"},
      {"PFADD", "s", "x"},
      {"PFCOUNT", "s"},
      {"PFMERGE", "d", "s"},
      {"RPUSH", "l", "x"},
      {"PFADD", "l", "x"},
  });
  // Larger sets: the estimate, which depends only on the registers.
  for (int round = 0; round < 3; ++round) {
    std::vector<std::string> add{"PFADD", "big"};
    for (int i = 0; i < 4000; ++i) add.push_back("member-" + std::to_string(round) + "-" + std::to_string(i));
    same(add);
    same({"PFCOUNT", "big"});
  }
  std::vector<std::string> other{"PFADD", "other"};
  for (int i = 0; i < 300; ++i) other.push_back("x" + std::to_string(i * 7));
  same(other);
  same({"PFCOUNT", "big", "other"});
  same({"PFMERGE", "both", "big", "other"});
  same({"PFCOUNT", "both"});
  // A dense value's bytes are the format's alone, so they match too.
  same({"GET", "big"});
}

TEST_F(Differential, Streams) {
  script({
      {"XADD", "s", "5-1", "a", "1"},
      {"XADD", "s", "5-*", "a", "2"},
      {"XADD", "s", "7-*", "a", "3"},
      {"XADD", "s", "6-0", "a", "4"},
      {"XADD", "t", "0-0", "a", "1"},
      {"XADD", "t", "x-1", "a", "1"},
      {"XADD", "t", "*", "a"},
      {"XADD", "t", "NOMKSTREAM", "*", "a", "1"},
      {"XADD", "u", "0-*", "a", "1"},
      {"XADD", "s", "8", "b", "1"},
      {"XLEN", "s"},
      {"XRANGE", "s", "-", "+"},
      {"XRANGE", "s", "5", "5"},
      {"XRANGE", "s", "(5-1", "+", "COUNT", "2"},
      {"XRANGE", "s", "-", "+", "COUNT", "0"},
      {"XREVRANGE", "s", "+", "-", "COUNT", "2"},
      {"XREVRANGE", "s", "(8-0", "(5-1"},
      {"XRANGE", "s", "7", "5"},
      {"XRANGE", "nosuch", "-", "+"},
      {"XRANGE", "s", "(-", "+"},
      {"XRANGE", "s", "-", "(+"},
      {"XRANGE", "s", "x", "+"},
      {"XRANGE", "s", "-", "+", "BOGUS", "1"},
      {"XDEL", "s", "5-1", "99-0"},
      {"XDEL", "s", "x"},
      {"XLEN", "s"},
      {"XTRIM", "s", "MAXLEN", "2"},
      {"XRANGE", "s", "-", "+"},
      {"XTRIM", "s", "MINID", "8"},
      {"XTRIM", "s", "MAXLEN", "~", "0"},
      {"XTRIM", "s", "MAXLEN", "=", "1", "LIMIT", "5"},
      {"XTRIM", "s", "MAXLEN", "-1"},
      {"XTRIM", "s", "BOGUS", "1"},
      {"XADD", "s", "MAXLEN", "1", "20-0", "n", "20"},
      {"XRANGE", "s", "-", "+"},
      {"XADD", "r", "1-0", "x", "1"},
      {"XADD", "r", "2-0", "x", "2"},
      {"XADD", "q", "5-0", "y", "5"},
      {"XREAD", "COUNT", "1", "STREAMS", "r", "q", "0", "0"},
      {"XREAD", "STREAMS", "r", "2-0"},
      {"XREAD", "STREAMS", "r", "$"},
      {"XREAD", "STREAMS", "r", "q", "0"},
      {"XREAD", "STREAMS", "r", ">"},
      {"XREAD", "BLOCK", "-1", "STREAMS", "r", "0"},
      {"XREAD", "BLOCK", "x", "STREAMS", "r", "0"},
      {"XSETID", "r", "10-0"},
      {"XADD", "r", "9-0", "a", "1"},
      {"XSETID", "r", "1-0"},
      {"XSETID", "nosuch", "1-0"},
      {"SET", "str", "v"},
      {"XADD", "str", "*", "a", "1"},
      {"XRANGE", "str", "-", "+"},
      {"XREAD", "STREAMS", "str", "0"},
      {"TYPE", "r"},
  });
  both({"HELLO", "3"});
  script({{"XREAD", "STREAMS", "r", "q", "0", "0"}, {"XREAD", "STREAMS", "r", "$"}});
  both({"HELLO", "2"});
  // Many entries, for approximate trimming's whole nodes.
  for (int i = 1; i <= 250; ++i) same({"XADD", "big", std::to_string(i) + "-0", "n", "x"});
  same({"XTRIM", "big", "MAXLEN", "~", "120"});
  same({"XLEN", "big"});
  same({"XTRIM", "big", "MINID", "~", "240"});
  same({"XLEN", "big"});
}

TEST_F(Differential, StreamGroups) {
  script({
      {"XADD", "s", "1-0", "a", "1"},
      {"XADD", "s", "2-0", "b", "2"},
      {"XADD", "s", "3-0", "c", "3"},
      {"XGROUP", "CREATE", "s", "g", "0"},
      {"XGROUP", "CREATE", "s", "g", "$"},
      {"XGROUP", "CREATE", "nosuch", "g", "$"},
      {"XGROUP", "CREATE", "made", "g", "$", "MKSTREAM"},
      {"XLEN", "made"},
      {"XGROUP", "DESTROY", "made", "g"},
      {"XGROUP", "DESTROY", "made", "g"},
      {"XGROUP", "CREATECONSUMER", "s", "g", "alice"},
      {"XGROUP", "CREATECONSUMER", "s", "g", "alice"},
      {"XGROUP", "BOGUS"},
      {"XREADGROUP", "GROUP", "g", "alice", "COUNT", "2", "STREAMS", "s", ">"},
      {"XREADGROUP", "GROUP", "g", "bob", "STREAMS", "s", ">"},
      {"XREADGROUP", "GROUP", "g", "bob", "STREAMS", "s", ">"},
      {"XREADGROUP", "GROUP", "g", "alice", "STREAMS", "s", "0"},
      {"XREADGROUP", "GROUP", "nog", "c", "STREAMS", "s", ">"},
      {"XREADGROUP", "GROUP", "g", "c", "STREAMS", "s", "$"},
      {"XREADGROUP", "GROUP", "g", "c", "STREAMS", "s"},
      {"XACK", "s", "g", "1-0", "9-0"},
      {"XACK", "s", "nog", "1-0"},
      {"XACK", "s", "g", "x"},
      {"XPENDING", "s", "g"},
      {"XPENDING", "s", "nog"},
      {"XDEL", "s", "2-0"},
      {"XREADGROUP", "GROUP", "g", "alice", "STREAMS", "s", "0"},
      {"XINFO", "GROUPS", "s"},
      {"XGROUP", "SETID", "s", "g", "0"},
      {"XINFO", "GROUPS", "s"},
      {"XGROUP", "SETID", "s", "g", "$", "ENTRIESREAD", "3"},
      {"XINFO", "GROUPS", "s"},
      {"XGROUP", "SETID", "s", "nog", "0"},
      {"XCLAIM", "s", "g", "carol", "0", "3-0", "JUSTID"},
      {"XCLAIM", "s", "g", "x", "y", "1-0"},
      {"XAUTOCLAIM", "s", "g", "dave", "0", "0", "JUSTID"},
      {"XAUTOCLAIM", "s", "g", "dave", "0", "0", "COUNT", "0"},
      {"XGROUP", "DELCONSUMER", "s", "g", "dave"},
      {"XPENDING", "s", "g"},
      {"XINFO", "STREAM", "nosuch"},
      {"XINFO", "GROUPS", "nosuch"},
      {"XINFO", "CONSUMERS", "s", "nog"},
      {"XINFO", "BOGUS", "s"},
  });
}

TEST_F(Differential, Transactions) {
  script({
      {"EXEC"},
      {"DISCARD"},
      {"MULTI"},
      {"MULTI"},
      {"WATCH", "x"},
      {"INCR", "foo"},
      {"SET", "a", "abc"},
      {"LPOP", "a"},
      {"BLPOP", "empty", "0"},
      {"EXEC"},
      {"MULTI"},
      {"SET", "a"},
      {"SET", "b", "1"},
      {"EXEC"},
      {"EXISTS", "b"},
      {"MULTI"},
      {"NOSUCH"},
      {"EXEC"},
      {"MULTI"},
      {"SET", "k", "v"},
      {"DISCARD"},
      {"EXISTS", "k"},
      {"SET", "w", "1"},
      {"WATCH", "w"},
      {"UNWATCH"},
      {"MULTI"},
      {"GET", "w"},
      {"EXEC"},
  });
  // A watch broken by another connection's write.
  Client mimir_other(*port_from("MIMIR_DIFF_MIMIR_PORT"));
  Client valkey_other(*port_from("MIMIR_DIFF_VALKEY_PORT"));
  same({"WATCH", "w"});
  EXPECT_EQ(mimir_other.call({"INCR", "w"}), valkey_other.call({"INCR", "w"}));
  same({"MULTI"});
  same({"SET", "w", "100"});
  same({"EXEC"});
  same({"GET", "w"});
}

TEST_F(Differential, PubSub) {
  // A second pair of connections subscribes on each server.
  Client mimir_sub(*port_from("MIMIR_DIFF_MIMIR_PORT"));
  Client valkey_sub(*port_from("MIMIR_DIFF_VALKEY_PORT"));
  const auto both_sub = [&](const std::vector<std::string>& args, int replies) {
    mimir_sub.send(args);
    valkey_sub.send(args);
    for (int i = 0; i < replies; ++i) EXPECT_EQ(mimir_sub.read(), valkey_sub.read()) << args[0];
  };
  both_sub({"SUBSCRIBE", "news", "sport"}, 2);
  both_sub({"PSUBSCRIBE", "n*"}, 1);
  same({"PUBLISH", "news", "hello"});
  EXPECT_EQ(mimir_sub.read(), valkey_sub.read());
  EXPECT_EQ(mimir_sub.read(), valkey_sub.read());
  same({"PUBLISH", "nothing", "x"});
  EXPECT_EQ(mimir_sub.read(), valkey_sub.read());
  same({"PUBLISH", "elsewhere", "x"});
  same({"PUBSUB", "CHANNELS"});
  same({"PUBSUB", "NUMSUB", "news", "sport", "none"});
  same({"PUBSUB", "NUMPAT"});
  same({"PUBSUB", "BOGUS"});
  both_sub({"GET", "k"}, 1);
  both_sub({"PING"}, 1);
  both_sub({"PING", "hi"}, 1);
  both_sub({"UNSUBSCRIBE", "sport"}, 1);
  both_sub({"PUNSUBSCRIBE"}, 1);
  both_sub({"UNSUBSCRIBE"}, 1);
  both_sub({"UNSUBSCRIBE"}, 1);
  both_sub({"SSUBSCRIBE", "orders"}, 1);
  same({"SPUBLISH", "orders", "1"});
  EXPECT_EQ(mimir_sub.read(), valkey_sub.read());
  same({"PUBSUB", "SHARDCHANNELS"});
  same({"PUBSUB", "SHARDNUMSUB", "orders"});
  both_sub({"SUNSUBSCRIBE"}, 1);
  both_sub({"HELLO", "3"}, 0);
  mimir_sub.read();
  valkey_sub.read();
  both_sub({"SUBSCRIBE", "r3"}, 1);
  same({"PUBLISH", "r3", "m"});
  EXPECT_EQ(mimir_sub.read(), valkey_sub.read());
  both_sub({"GET", "k"}, 1);
}

TEST_F(Differential, ServerCommands) {
  script({
      {"CONFIG", "GET", "maxmemory"},
      {"CONFIG", "GET", "maxmemory-policy"},
      {"CONFIG", "GET", "notify-keyspace-events"},
      {"CONFIG", "GET", "nosuch"},
      {"CONFIG", "SET", "nosuch", "1"},
      {"CONFIG", "BOGUS"},
      {"SET", "i", "12345"},
      {"SET", "e", "short"},
      {"SET", "r", std::string(50, 'x')},
      {"SADD", "is", "1", "2"},
      {"SADD", "ls", "a"},
      {"HSET", "h", "f", "v"},
      {"ZADD", "z", "1", "m"},
      {"RPUSH", "l", "x"},
      {"XADD", "st", "1-0", "f", "v"},
      {"OBJECT", "ENCODING", "i"},
      {"OBJECT", "ENCODING", "e"},
      {"OBJECT", "ENCODING", "r"},
      {"OBJECT", "ENCODING", "is"},
      {"OBJECT", "ENCODING", "ls"},
      {"OBJECT", "ENCODING", "h"},
      {"OBJECT", "ENCODING", "z"},
      {"OBJECT", "ENCODING", "l"},
      {"OBJECT", "ENCODING", "st"},
      {"OBJECT", "ENCODING", "nosuch"},
      {"OBJECT", "REFCOUNT", "i"},
      {"OBJECT", "FREQ", "i"},
      {"OBJECT", "BOGUS", "i"},
      {"ROLE"},
      {"WAIT", "0", "0"},
      {"WAITAOF", "1", "0", "0"},
      {"MEMORY", "USAGE", "nosuch"},
      {"CLIENT", "KILL", "nowhere:1"},
      {"CLIENT", "NO-EVICT", "on"},
      {"CLIENT", "NO-TOUCH", "off"},
  });
}

TEST_F(Differential, Connection) {
  script({
      {"PING"},
      {"PING", "hello"},
      {"PING", "a", "b"},
      {"ECHO", "x"},
      {"SELECT", "0"},
      {"SELECT", "x"},
      {"HELLO", "4"},
      {"HELLO", "x"},
      {"CLIENT", "GETNAME"},
      {"CLIENT", "SETNAME", "has space"},
      {"CLIENT", "SETNAME", "named"},
      {"CLIENT", "GETNAME"},
      {"CLIENT", "SETINFO", "LIB-NAME", "x"},
      {"CLIENT", "SETINFO", "COLOUR", "x"},
      {"CLIENT", "foo"},
      {"foo", "a", "b"},
      {"GET"},
      {"COMMAND", "INFO", "nosuch"},
  });
  // HELLO's own reply names the server and the connection, so it is not
  // compared; what follows it, in RESP3, is.
  both({"HELLO", "3"});
  script({{"GET", "nosuch"}, {"MGET", "nosuch", "nosuch"}, {"SET", "k", "1"}, {"INCRBYFLOAT", "k", "0.5"}, {"SET", "k", "v", "XX", "GET"}});
  both({"HELLO", "2"});
}

TEST_F(Differential, Scripting) {
  both({"SCRIPT", "FLUSH"});
  script({
      {"EVAL", "return 1", "0"},
      {"EVAL", "return 3.99", "0"},
      {"EVAL", "return {1, 'a', {2}, nil, 5}", "0"},
      {"EVAL", "return {ok='FINE'}", "0"},
      {"EVAL", "return {err='MYERR bad'}", "0"},
      {"EVAL", "return redis.error_reply('oops')", "0"},
      {"EVAL", "return redis.error_reply('MY oops')", "0"},
      {"EVAL", "return true", "0"},
      {"EVAL", "return false", "0"},
      {"EVAL", "return {KEYS[1], ARGV[1]}", "1", "a", "b"},
      {"EVAL", "return 1", "-1"},
      {"EVAL", "return 1", "3", "a"},
      {"EVAL", "return 1", "x"},
      {"EVAL", "return redis.call('SET', KEYS[1], ARGV[1])", "1", "k", "v"},
      {"EVAL", "return redis.call('GET', 'k')", "0"},
      {"EVAL", "return redis.call('GET', 'none')", "0"},
      {"EVAL", "return redis.call('INCRBY', 'n', 5) + 1", "0"},
      {"EVAL", "redis.call('LPUSH', 'k', 1)", "0"},
      {"EVAL", "return redis.pcall('LPUSH', 'k', 1)", "0"},
      {"EVAL", "return redis.call('NOSUCH')", "0"},
      {"EVAL", "return redis.call('MULTI')", "0"},
      {"EVAL", "return redis.call('GET')", "0"},
      {"EVAL", "error('plain')", "0"},
      {"EVAL", "error({err='CUSTOM x'})", "0"},
      {"EVAL", "x = 1", "0"},
      {"EVAL", "return +", "0"},
      {"EVAL", "return redis.sha1hex('')", "0"},
      {"EVALSHA", "e0e1f9fabfc9d4800c877a703b823ac0578ff8db", "0"},
      {"SCRIPT", "LOAD", "return 1"},
      {"EVALSHA", "e0e1f9fabfc9d4800c877a703b823ac0578ff8db", "0"},
      {"SCRIPT", "EXISTS", "e0e1f9fabfc9d4800c877a703b823ac0578ff8db", "ffff"},
      {"SCRIPT", "LOAD", "return +"},
      {"SCRIPT", "KILL"},
      {"EVAL_RO", "return redis.call('GET', 'k')", "1", "k"},
      {"EVAL_RO", "return redis.call('SET', 'k', 'w')", "0"},
      {"SCRIPT", "FLUSH"},
      {"SCRIPT", "EXISTS", "e0e1f9fabfc9d4800c877a703b823ac0578ff8db"},
  });
}

// Keyspace notifications: every command's events, compared one command at a
// time. A subscriber on each server listens to every notification and to a
// marker channel; after each command a marker is published, and what came
// before it is that command's events.
class Notifications {
 public:
  explicit Notifications(uint16_t port) : _client(port) {
    _client.call({"PSUBSCRIBE", "__key*@*__:*"});
    _client.call({"SUBSCRIBE", "marker"});
  }
  std::string until_marker() {
    std::string events;
    for (;;) {
      const std::string message = _client.read();
      if (message.find("$6\r\nmarker\r\n") != std::string::npos && message.rfind("*3\r\n$7\r\nmessage\r\n", 0) == 0) return events;
      events += message;
    }
  }

 private:
  Client _client;
};

void notifications_script(const std::string& flags, const std::vector<std::vector<std::string>>& commands) {
  Client mimir(*port_from("MIMIR_DIFF_MIMIR_PORT"));
  Client valkey(*port_from("MIMIR_DIFF_VALKEY_PORT"));
  mimir.call({"FLUSHALL"});
  valkey.call({"FLUSHALL"});
  EXPECT_EQ(mimir.call({"CONFIG", "SET", "notify-keyspace-events", flags}), valkey.call({"CONFIG", "SET", "notify-keyspace-events", flags}));
  EXPECT_EQ(mimir.call({"CONFIG", "GET", "notify-keyspace-events"}), valkey.call({"CONFIG", "GET", "notify-keyspace-events"}));
  Notifications mimir_events(*port_from("MIMIR_DIFF_MIMIR_PORT"));
  Notifications valkey_events(*port_from("MIMIR_DIFF_VALKEY_PORT"));
  // Markers go on their own connections, so a command in MULTI does not queue them.
  Client mimir_marker(*port_from("MIMIR_DIFF_MIMIR_PORT"));
  Client valkey_marker(*port_from("MIMIR_DIFF_VALKEY_PORT"));
  for (const auto& args : commands) {
    std::string shown;
    for (const auto& arg : args) shown += arg + " ";
    if (args[0] == "SLEEP") {
      usleep(static_cast<useconds_t>(std::stoi(args[1]) * 1000));
      continue;
    }
    EXPECT_EQ(mimir.call(args), valkey.call(args)) << shown;
    mimir_marker.call({"PUBLISH", "marker", "x"});
    valkey_marker.call({"PUBLISH", "marker", "x"});
    EXPECT_EQ(mimir_events.until_marker(), valkey_events.until_marker()) << shown;
  }
  mimir.call({"CONFIG", "SET", "notify-keyspace-events", ""});
  valkey.call({"CONFIG", "SET", "notify-keyspace-events", ""});
}

TEST_F(Differential, NotificationFlags) {
  for (const std::string flags : {"KEA", "AKE", "Kg$", "El", "Ksx", "KEhzt", "KEAnm", "Exe", "K", "E", "A", "", "KEd", "KA$lshzxetd"}) {
    same({"CONFIG", "SET", "notify-keyspace-events", flags});
    same({"CONFIG", "GET", "notify-keyspace-events"});
  }
  same({"CONFIG", "SET", "notify-keyspace-events", "KEq"});
  same({"CONFIG", "GET", "notify-keyspace-events"});
  same({"CONFIG", "SET", "notify-keyspace-events", ""});
}

TEST_F(Differential, NotificationsGenericAndStrings) {
  notifications_script("KEA", {
                                  {"SET", "a", "1"},
                                  {"SET", "a", "2", "EX", "100"},
                                  {"SET", "a", "3", "KEEPTTL"},
                                  {"SET", "a", "4", "NX"},
                                  {"SET", "b", "4", "XX"},
                                  {"SET", "a", "5", "GET"},
                                  {"SETNX", "a", "x"},
                                  {"SETNX", "c", "x"},
                                  {"SETEX", "d", "100", "v"},
                                  {"PSETEX", "e", "100000", "v"},
                                  {"GETSET", "a", "6"},
                                  {"GETDEL", "e"},
                                  {"GETDEL", "e"},
                                  {"GETEX", "a", "EX", "100"},
                                  {"GETEX", "a", "PERSIST"},
                                  {"GETEX", "a"},
                                  {"MSET", "m1", "1", "m2", "2"},
                                  {"MSETNX", "m3", "1", "m4", "2"},
                                  {"MSETNX", "m3", "1", "m5", "2"},
                                  {"INCR", "n"},
                                  {"DECR", "n"},
                                  {"INCRBY", "n", "5"},
                                  {"DECRBY", "n", "2"},
                                  {"INCRBYFLOAT", "n", "1.5"},
                                  {"APPEND", "s", "abc"},
                                  {"APPEND", "s", "def"},
                                  {"SETRANGE", "s", "2", "XY"},
                                  {"SETRANGE", "z", "0", ""},
                                  {"SETBIT", "bits", "7", "1"},
                                  {"BITFIELD", "bf", "SET", "u8", "0", "200"},
                                  {"BITFIELD", "bf", "GET", "u8", "0"},
                                  {"BITOP", "AND", "dest", "bits", "bf"},
                                  {"BITOP", "AND", "dest", "nosuch1", "nosuch2"},
                                  {"PFADD", "hll", "a", "b"},
                                  {"PFADD", "hll", "a"},
                                  {"PFMERGE", "hll2", "hll"},
                                  {"PFCOUNT", "hll"},
                                  {"EXPIRE", "a", "100"},
                                  {"PEXPIRE", "a", "100000"},
                                  {"EXPIREAT", "a", "4000000000"},
                                  {"PEXPIREAT", "a", "4000000000000"},
                                  {"EXPIRE", "a", "100", "NX"},
                                  {"PERSIST", "a"},
                                  {"PERSIST", "a"},
                                  {"EXPIRE", "m1", "0"},
                                  {"PEXPIRE", "m2", "-5"},
                                  {"EXPIREAT", "m4", "1"},
                                  {"DEL", "a", "nosuch", "c"},
                                  {"UNLINK", "d", "nosuch"},
                                  {"SET", "r1", "v"},
                                  {"RENAME", "r1", "r2"},
                                  {"RENAMENX", "r2", "r3"},
                                  {"SET", "r4", "v"},
                                  {"RENAMENX", "r3", "r4"},
                                  {"RENAME", "r3", "r3"},
                                  {"COPY", "r3", "r5"},
                                  {"COPY", "r3", "r5", "REPLACE"},
                                  {"COPY", "r3", "r6", "DB", "1"},
                                  {"MOVE", "r5", "2"},
                                  {"SET", "t1", "v"},
                                  {"PEXPIRE", "t1", "1"},
                                  {"SLEEP", "30"},
                                  {"GET", "t1"},
                                  {"SET", "t2", "v", "PX", "1"},
                                  {"SLEEP", "30"},
                                  {"EXISTS", "t2"},
                                  {"SET", "t3", "v", "PX", "1"},
                                  {"SLEEP", "30"},
                                  {"SET", "t3", "w"},
                                  {"TOUCH", "r3"},
                                  {"TYPE", "r3"},
                                  {"SWAPDB", "0", "1"},
                                  {"SWAPDB", "0", "1"},
                                  {"FLUSHDB"},
                              });
}

TEST_F(Differential, NotificationsCollections) {
  notifications_script("KEA", {
                                  {"LPUSH", "l", "a", "b"},
                                  {"RPUSH", "l", "c"},
                                  {"LPUSHX", "l", "d"},
                                  {"RPUSHX", "l", "e"},
                                  {"LPUSHX", "nol", "d"},
                                  {"LINSERT", "l", "BEFORE", "c", "x"},
                                  {"LINSERT", "l", "BEFORE", "zz", "x"},
                                  {"LSET", "l", "0", "y"},
                                  {"LREM", "l", "1", "x"},
                                  {"LREM", "l", "1", "zz"},
                                  {"LTRIM", "l", "0", "2"},
                                  {"LPOP", "l"},
                                  {"RPOP", "l", "1"},
                                  {"RPUSH", "l2", "a", "b"},
                                  {"LMOVE", "l2", "l3", "LEFT", "RIGHT"},
                                  {"RPOPLPUSH", "l2", "l3"},
                                  {"LMPOP", "1", "l3", "LEFT", "COUNT", "5"},
                                  {"RPUSH", "l4", "a"},
                                  {"BLPOP", "l4", "1"},
                                  {"RPUSH", "l5", "a", "b"},
                                  {"LTRIM", "l5", "5", "10"},
                                  {"HSET", "h", "f1", "1", "f2", "2"},
                                  {"HSET", "h", "f1", "1"},
                                  {"HSETNX", "h", "f3", "3"},
                                  {"HSETNX", "h", "f3", "3"},
                                  {"HMSET", "h", "f4", "4"},
                                  {"HINCRBY", "h", "f1", "2"},
                                  {"HINCRBYFLOAT", "h", "f1", "0.5"},
                                  {"HDEL", "h", "f1", "nosuch"},
                                  {"HDEL", "h", "nosuch"},
                                  {"HDEL", "h", "f2", "f3", "f4"},
                                  {"SADD", "s", "a", "b", "c"},
                                  {"SADD", "s", "a"},
                                  {"SREM", "s", "a", "zz"},
                                  {"SREM", "s", "zz"},
                                  {"SMOVE", "s", "s2", "b"},
                                  {"SMOVE", "s", "s2", "zz"},
                                  {"SPOP", "s"},
                                  {"SADD", "s3", "a", "b"},
                                  {"SINTERSTORE", "sd", "s2", "s3"},
                                  {"SUNIONSTORE", "sd", "s2", "s3"},
                                  {"SDIFFSTORE", "sd", "s2", "s2"},
                                  {"SDIFFSTORE", "sd", "nosuch"},
                                  {"ZADD", "z", "1", "a", "2", "b", "3", "c"},
                                  {"ZADD", "z", "1", "a"},
                                  {"ZADD", "z", "XX", "5", "zz"},
                                  {"ZINCRBY", "z", "1", "a"},
                                  {"ZREM", "z", "a", "zz"},
                                  {"ZREM", "z", "zz"},
                                  {"ZADD", "z", "1", "a", "4", "d", "5", "e", "6", "f"},
                                  {"ZREMRANGEBYSCORE", "z", "1", "1"},
                                  {"ZREMRANGEBYRANK", "z", "0", "0"},
                                  {"ZREMRANGEBYLEX", "z", "[a", "[a"},
                                  {"ZPOPMIN", "z"},
                                  {"ZPOPMAX", "z", "1"},
                                  {"ZMPOP", "1", "z", "MIN"},
                                  {"ZADD", "z2", "1", "a", "2", "b"},
                                  {"ZUNIONSTORE", "zd", "1", "z2"},
                                  {"ZINTERSTORE", "zd", "2", "z2", "nosuch"},
                                  {"ZDIFFSTORE", "zd2", "1", "z2"},
                                  {"ZRANGESTORE", "zd3", "z2", "0", "-1"},
                                  {"ZRANGESTORE", "zd3", "z2", "5", "6"},
                                  {"ZPOPMIN", "z2", "5"},
                                  {"XADD", "x", "1-0", "f", "v"},
                                  {"XADD", "x", "MAXLEN", "1", "2-0", "f", "v"},
                                  {"XADD", "x", "NOMKSTREAM", "3-0", "f", "v"},
                                  {"XADD", "nox", "NOMKSTREAM", "3-0", "f", "v"},
                                  {"XDEL", "x", "3-0"},
                                  {"XDEL", "x", "9-0"},
                                  {"XTRIM", "x", "MAXLEN", "0"},
                                  {"XSETID", "x", "10-0"},
                                  {"XGROUP", "CREATE", "x", "g", "0"},
                                  {"XGROUP", "CREATE", "x2", "g", "$", "MKSTREAM"},
                                  {"XGROUP", "CREATECONSUMER", "x", "g", "c1"},
                                  {"XGROUP", "SETID", "x", "g", "0"},
                                  {"XADD", "x", "11-0", "f", "v"},
                                  {"XREADGROUP", "GROUP", "g", "c2", "STREAMS", "x", ">"},
                                  {"XACK", "x", "g", "11-0"},
                                  {"XCLAIM", "x", "g", "c3", "0", "11-0"},
                                  {"XAUTOCLAIM", "x", "g", "c4", "0", "0"},
                                  {"XGROUP", "DELCONSUMER", "x", "g", "c1"},
                                  {"XGROUP", "DESTROY", "x", "g"},
                                  {"DEL", "x", "x2"},
                              });
}

TEST_F(Differential, NotificationsNewAndMiss) {
  notifications_script("KEAnm", {
                                    {"SET", "a", "1"},
                                    {"SET", "a", "2"},
                                    {"GET", "nosuch"},
                                    {"GET", "a"},
                                    {"MGET", "a", "nosuch"},
                                    {"LPUSH", "l", "x"},
                                    {"LRANGE", "nol", "0", "-1"},
                                    {"HGET", "noh", "f"},
                                    {"EXISTS", "nosuch"},
                                    {"TYPE", "nosuch"},
                                    {"INCR", "n"},
                                    {"SADD", "s", "a"},
                                    {"STRLEN", "nosuch"},
                                    {"COPY", "a", "b"},
                                    {"RENAME", "b", "c"},
                                    {"ZSCORE", "noz", "m"},
                                    {"XADD", "x", "1-0", "f", "v"},
                                    {"GETDEL", "nosuch"},
                                });
}

TEST_F(Differential, NotificationsInScriptsAndTransactions) {
  notifications_script("KEA", {
                                  {"EVAL", "redis.call('SET', 'a', 1); redis.call('INCR', 'a'); return redis.call('DEL', 'a')", "0"},
                                  {"MULTI"},
                                  {"SET", "b", "1"},
                                  {"LPUSH", "c", "x"},
                                  {"EXEC"},
                              });
}

// The bulk string a reply holds.
std::string bulk_of(const std::string& reply) {
  const size_t end = reply.find("\r\n");
  const size_t n = std::stoul(reply.substr(1, end - 1));
  return reply.substr(end + 2, n);
}

TEST_F(Differential, Sort) {
  script({
      {"RPUSH", "l", "3", "10", "1", "2.5", "-4", "1e3", "  7", "", "0x10"},
      {"RPUSH", "nums", "3", "10", "1", "2.5", "-4", "1e3", "3"},
      {"SORT", "nums"},
      {"SORT", "nums", "DESC"},
      {"SORT", "nums", "ALPHA"},
      {"SORT", "nums", "ALPHA", "DESC"},
      {"SORT", "l"},
      {"SORT", "nums", "LIMIT", "1", "2"},
      {"SORT", "nums", "LIMIT", "-1", "2"},
      {"SORT", "nums", "LIMIT", "2", "-1"},
      {"SORT", "nums", "LIMIT", "10", "2"},
      {"SORT", "nums", "LIMIT", "1"},
      {"SORT", "nums", "LIMIT", "x", "1"},
      {"SORT", "nums", "WAT"},
      {"SORT", "nosuch"},
      {"SORT", "nosuch", "STORE", "out"},
      {"RPUSH", "ids", "1", "2", "3", "4"},
      {"MSET", "w_1", "30", "w_2", "10", "w_3", "20", "name_1", "one", "name_2", "two", "w_4", "x"},
      {"HSET", "h_1", "age", "5", "nick", "a"},
      {"HSET", "h_2", "age", "50"},
      {"HSET", "h_3", "age", "7", "nick", "c"},
      {"SORT", "ids", "BY", "w_*"},
      {"SORT", "ids", "BY", "w_*", "LIMIT", "0", "3"},
      {"SORT", "ids", "BY", "h_*->age", "DESC"},
      {"SORT", "ids", "BY", "h_*->age", "GET", "#", "GET", "h_*->nick", "GET", "name_*", "GET", "nopattern"},
      {"SORT", "ids", "BY", "nosort"},
      {"SORT", "ids", "BY", "nosort", "DESC"},
      {"SORT", "ids", "BY", "nosort", "DESC", "LIMIT", "1", "2"},
      {"SORT", "ids", "BY", "nosort", "LIMIT", "1", "2"},
      // Ties (two names missing) order as Valkey's unstable sort leaves them;
      // only the slice past them is compared.
      {"SORT", "ids", "BY", "name_*", "ALPHA", "LIMIT", "2", "2"},
      {"SORT", "ids", "GET", "h_*->age", "GET", "h_*->"},
      {"SORT", "ids", "BY", "w_*", "GET", "#", "STORE", "out"},
      {"LRANGE", "out", "0", "-1"},
      {"SORT", "ids", "BY", "nosort", "GET", "name_*", "STORE", "out2"},
      {"LRANGE", "out2", "0", "-1"},
      {"ZADD", "z", "1", "c", "2", "a", "3", "b"},
      {"SORT", "z", "ALPHA"},
      {"SORT", "z", "BY", "nosort"},
      {"SORT", "z", "BY", "nosort", "DESC"},
      {"SORT", "z", "BY", "nosort", "DESC", "LIMIT", "1", "1"},
      {"SORT", "z", "BY", "nosort", "LIMIT", "1", "5"},
      {"SADD", "s", "c", "a", "b"},
      {"SORT", "s", "ALPHA"},
      {"SORT", "s", "BY", "nosort", "STORE", "out3"},
      {"LRANGE", "out3", "0", "-1"},
      {"EVAL", "return redis.call('SORT', KEYS[1], 'BY', 'nosort')", "1", "s"},
      {"SADD", "si", "5", "1", "3"},
      {"SORT", "si"},
      {"SORT", "si", "BY", "nosort"},
      {"SET", "str", "x"},
      {"SORT", "str"},
      {"SORT_RO", "ids", "BY", "w_*"},
      {"SORT_RO", "ids", "STORE", "out"},
      {"SORT", "ids", "STORE"},
  });
}

TEST_F(Differential, DumpBytes) {
  const std::string fifty(50, 'a');
  const std::string text = "The quick brown fox jumps over the lazy dog. The quick brown fox jumps over the lazy dog again and again.";
  script({
      {"SET", "s1", "hello"},
      {"SET", "s2", "12345"},
      {"SET", "s3", "-7"},
      {"SET", "s4", "123456789012"},
      {"SET", "s5", fifty},
      {"SET", "s6", "abcdefghijklmnopqrstuvwxyz0123456789"},
      {"SET", "s7", text},
      {"SET", "s8", std::string(3000, 'x') + "y" + std::string(500, 'z')},
      {"RPUSH", "l1", "a", "1", "bcd", "300", "-5", "70000", "-70000", "2147483648", "-9000000000"},
      {"HSET", "h1", "f", "v", "n", "7"},
      {"SADD", "i1", "1", "2", "3"},
      {"SADD", "i2", "1", "70000", "-5"},
      {"SADD", "i3", "1", "5000000000"},
      {"SADD", "p1", "a", "b"},
      {"ZADD", "z1", "1", "a", "2.5", "b", "-3", "c", "1e20", "d"},
      {"XADD", "x", "1-1", "f", "v"},
      {"XADD", "x", "1-2", "f", "w"},
      {"XADD", "x", "2-0", "g", "1", "h", "2"},
  });
  for (const char* key : {"s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "l1", "h1", "i1", "i2", "i3", "p1", "z1", "x", "nosuch"}) same({"DUMP", key});
}

TEST_F(Differential, DumpRestoreAcross) {
  // Values in Valkey's larger encodings, made alike on both servers.
  std::vector<std::string> hset{"HSET", "h"}, sadd{"SADD", "s"}, zadd{"ZADD", "z"}, rpush{"RPUSH", "l"}, ints{"SADD", "n"};
  std::vector<std::string> fields, members;
  for (int i = 0; i < 200; ++i) {
    fields.push_back("field" + std::to_string(i));
    hset.push_back(fields.back());
    hset.push_back("value" + std::to_string(i * 3));
    members.push_back("member" + std::to_string(i));
    sadd.push_back(members.back());
    zadd.push_back(std::to_string(i * 0.5 - 7));
    zadd.push_back("z" + std::to_string(i));
  }
  for (int i = 0; i < 3000; ++i) rpush.push_back("element-" + std::to_string(i));
  for (int i = 0; i < 600; ++i) ints.push_back(std::to_string(i * 11));
  script({hset, sadd, zadd, rpush, ints});
  for (int i = 1; i <= 250; ++i) same({"XADD", "x", std::to_string(i) + "-0", "f", std::to_string(i)});
  script({{"XDEL", "x", "5-0"}, {"XGROUP", "CREATE", "x", "g", "0"}, {"XREADGROUP", "GROUP", "g", "alice", "COUNT", "3", "STREAMS", "x", ">"}});
  std::vector<std::string> hmget{"HMGET", "KEY"}, smismember{"SMISMEMBER", "KEY"};
  hmget.insert(hmget.end(), fields.begin(), fields.end());
  smismember.insert(smismember.end(), members.begin(), members.end());
  const std::vector<std::pair<std::string, std::vector<std::vector<std::string>>>> reads = {
      {"h", {{"HLEN", "KEY"}, hmget}},
      {"s", {{"SCARD", "KEY"}, smismember}},
      {"z", {{"ZRANGE", "KEY", "0", "-1", "WITHSCORES"}}},
      {"l", {{"LRANGE", "KEY", "0", "-1"}}},
      {"n", {{"SCARD", "KEY"}, {"SISMEMBER", "KEY", "6589"}, {"SISMEMBER", "KEY", "6590"}}},
      {"x", {{"XRANGE", "KEY", "-", "+"}, {"XLEN", "KEY"}, {"XINFO", "GROUPS", "KEY"}, {"XPENDING", "KEY", "g"}}},
  };
  for (const auto& [key, commands] : reads) {
    const std::string from_valkey = bulk_of(_valkey->call({"DUMP", key}));
    const std::string from_mimir = bulk_of(_mimir->call({"DUMP", key}));
    // Each server restores the other's payload under a new name.
    EXPECT_EQ(_mimir->call({"RESTORE", key + ":other", "0", from_valkey}), "+OK\r\n") << key;
    EXPECT_EQ(_valkey->call({"RESTORE", key + ":other", "0", from_mimir}), "+OK\r\n") << key;
    for (auto command : commands) {
      std::vector<std::string> original = command, restored = command;
      for (auto& arg : original)
        if (arg == "KEY") arg = key;
      for (auto& arg : restored)
        if (arg == "KEY") arg = key + ":other";
      EXPECT_EQ(_mimir->call(restored), _valkey->call(original)) << key << " " << command[0];
      EXPECT_EQ(_valkey->call(restored), _mimir->call(original)) << key << " " << command[0];
    }
  }
}

TEST_F(Differential, RestoreErrors) {
  script({{"SET", "k", "v"}});
  const std::string good = bulk_of(_valkey->call({"DUMP", "k"}));
  std::string corrupt = good;
  corrupt[2] ^= 1;
  script({
      {"RESTORE", "k", "0", good},
      {"RESTORE", "k", "-1", "x", "REPLACE"},
      {"RESTORE", "k", "x", "x", "REPLACE"},
      {"RESTORE", "k", "0", "x", "REPLACE"},
      {"RESTORE", "k", "0", "x", "WAT"},
      {"RESTORE", "k", "0", "x", "IDLETIME", "-1"},
      {"RESTORE", "k", "0", "x", "FREQ", "300"},
      {"RESTORE", "k", "0", "x", "IDLETIME", "1", "FREQ", "1"},
      {"RESTORE", "other", "0", corrupt},
      {"RESTORE", "k2", "5000", good},
      {"PTTL", "k2"},
      {"RESTORE", "k2", "1", good, "ABSTTL", "REPLACE"},
      {"EXISTS", "k2"},
      {"RESTORE", "k3", "0", good, "IDLETIME", "10"},
      {"GET", "k3"},
      {"RESTORE", "k4", "0", good, "FREQ", "5"},
      {"GET", "k4"},
  });
}

TEST_F(Differential, FunctionDumpRestore) {
  both({"FUNCTION", "FLUSH"});
  const std::string lib = "#!lua name=lib\nredis.register_function('f', function() return 1 end)";
  same({"FUNCTION", "LOAD", lib});
  same({"FUNCTION", "DUMP"});
  const std::string dumped = bulk_of(_valkey->call({"FUNCTION", "DUMP"}));
  script({
      {"FUNCTION", "RESTORE", dumped},
      {"FUNCTION", "RESTORE", dumped, "APPEND"},
      {"FUNCTION", "RESTORE", dumped, "REPLACE"},
      {"FUNCTION", "RESTORE", dumped, "WAT"},
      {"FUNCTION", "RESTORE", "x"},
      {"FUNCTION", "RESTORE"},
      {"FUNCTION", "DELETE", "lib"},
      {"FUNCTION", "LOAD", "#!lua name=other\nredis.register_function('f', function() return 2 end)"},
      {"FUNCTION", "RESTORE", dumped},
      {"FUNCTION", "RESTORE", dumped, "REPLACE"},
      {"FCALL", "f", "0"},
      {"FUNCTION", "RESTORE", dumped, "FLUSH"},
      {"FCALL", "f", "0"},
      {"FUNCTION", "FLUSH"},
      {"FUNCTION", "DUMP"},
  });
  both({"FUNCTION", "FLUSH"});
}

TEST_F(Differential, Functions) {
  both({"FUNCTION", "FLUSH"});
  const std::string lib =
      "#!lua name=lib1\n"
      "redis.register_function('echo', function(keys, args) return {keys[1], args[1]} end)\n"
      "redis.register_function{function_name='put', callback=function(keys, args) return redis.call('SET', keys[1], args[1]) end}\n"
      "redis.register_function{function_name='get', callback=function(keys, args) return redis.call('GET', keys[1]) end, flags={'no-writes'}, "
      "description='reads'}";
  const std::string one = "#!lua name=one\nredis.register_function{function_name='f', callback=function() return 1 end, flags={'no-writes', 'allow-oom'}}";
  script({
      {"FUNCTION", "LOAD", lib},
      {"FUNCTION", "LOAD", lib},
      {"FCALL", "echo", "1", "k", "a"},
      {"FCALL", "put", "1", "k", "v"},
      {"FCALL_RO", "get", "1", "k"},
      {"FCALL_RO", "put", "1", "k", "v"},
      {"FCALL", "nosuch", "0"},
      {"FCALL", "echo", "x"},
      {"FCALL", "echo", "-1"},
      {"FCALL", "echo", "5", "a"},
      {"FUNCTION", "LOAD", one},
      {"FUNCTION", "LIST", "LIBRARYNAME", "one"},
      {"FUNCTION", "LIST", "WITHCODE", "LIBRARYNAME", "on?"},
      {"FUNCTION", "LIST", "LIBRARYNAME", "zz"},
      {"FUNCTION", "LIST", "LIBRARYNAME"},
      {"FUNCTION", "LIST", "WAT"},
      {"FUNCTION", "STATS"},
      {"FUNCTION", "LOAD", "no shebang"},
      {"FUNCTION", "LOAD", "#!lua"},
      {"FUNCTION", "LOAD", "#!lua name=d"},
      {"FUNCTION", "LOAD", "#!lua name=bad-name!\nx"},
      {"FUNCTION", "LOAD", "#!lua name=l x=1\nx"},
      {"FUNCTION", "LOAD", "#!lua name=\nx"},
      {"FUNCTION", "LOAD", "#!lua name\nx"},
      {"FUNCTION", "LOAD", "#!lua\nx"},
      {"FUNCTION", "LOAD", "#!\nx"},
      {"FUNCTION", "LOAD", "#! name=x\nx"},
      {"FUNCTION", "LOAD", "#!py name=l\nx"},
      {"FUNCTION", "LOAD", "#!lua name=d name=e\nx"},
      {"FUNCTION", "LOAD", "#!lua name=l\nreturn 1"},
      {"FUNCTION", "LOAD", "#!lua name=l\nlocal x = +"},
      {"FUNCTION", "LOAD", "#!lua name=l\nredis.call('PING')"},
      {"FUNCTION", "LOAD", "#!lua name=l\nerror('boom')"},
      {"FUNCTION", "LOAD", "#!lua name=l\nx = 1"},
      {"FUNCTION", "LOAD", "#!lua name=l\nredis.register_function('echo', function() return 1 end)"},
      {"FUNCTION", "LOAD", "#!lua name=l\nredis.register_function('a', function() end)\nredis.register_function('a', function() end)"},
      {"FUNCTION", "LOAD", "#!lua name=l\nredis.register_function('bad name', function() return 1 end)"},
      {"FUNCTION", "LOAD", "#!lua name=l\nredis.register_function()"},
      {"FUNCTION", "LOAD", "#!lua name=l\nredis.register_function(5)"},
      {"FUNCTION", "LOAD", "#!lua name=l\nredis.register_function({}, 5)"},
      {"FUNCTION", "LOAD", "#!lua name=l\nredis.register_function('d1', 5)"},
      {"FUNCTION", "LOAD", "#!lua name=l\nredis.register_function{function_name='d1', callback=5}"},
      {"FUNCTION", "LOAD", "#!lua name=l\nredis.register_function{function_name='n'}"},
      {"FUNCTION", "LOAD", "#!lua name=l\nredis.register_function{callback=function() end}"},
      {"FUNCTION", "LOAD", "#!lua name=l\nredis.register_function{function_name='n', callback=function() end, wat=1}"},
      {"FUNCTION", "LOAD", "#!lua name=l\nredis.register_function{function_name='n', callback=function() end, flags='no-writes'}"},
      {"FUNCTION", "LOAD", "#!lua name=l\nredis.register_function{function_name='n', callback=function() end, flags={'wat'}}"},
      {"FUNCTION", "LOAD", "#!lua name=l\nredis.register_function{function_name='n', callback=function() end, description={}}"},
      {"FUNCTION", "LOAD",
       "#!lua name=w\nredis.register_function{function_name='w', callback=function(keys) return redis.call('SET', keys[1], 1) end, flags={'no-writes'}}"},
      {"FCALL", "w", "1", "k"},
      {"FUNCTION", "LOAD", "#!lua name=h\nredis.register_function('h', function(keys, args) error('inner') end)"},
      {"FCALL", "h", "0"},
      {"FUNCTION", "LOAD", "#!lua name=kk\nredis.register_function('kk', function(keys, args) return KEYS end)"},
      {"FCALL", "kk", "0"},
      {"FUNCTION", "LOAD",
       "#!lua name=hh\nlocal function h() return string.upper('a') .. type(redis.call) end\nredis.register_function('hh', function(keys) return h() .. #keys "
       "end)"},
      {"FCALL", "hh", "2", "a", "b"},
      {"FUNCTION", "LOAD", "#!lua name=r\nlocal reg = redis.register_function\nredis.register_function('r', function() reg('z', function() end) end)"},
      {"FCALL", "r", "0"},
      {"FUNCTION", "LOAD",
       "#!lua name=v\nredis.register_function('v', function() return {redis.REDIS_VERSION, redis.REDIS_VERSION_NUM, redis.VALKEY_VERSION, "
       "redis.VALKEY_VERSION_NUM} end)"},
      {"FCALL", "v", "0"},
      {"FUNCTION", "LOAD", "REPLACE", "#!lua name=lib1\nredis.register_function('echo', function() return 3 end)"},
      {"FCALL", "echo", "0"},
      {"FCALL", "put", "1", "k", "v"},
      {"FUNCTION", "LOAD", "WAT", lib},
      {"SCRIPT", "FLUSH"},
      {"FCALL", "f", "0"},
      {"FUNCTION", "DELETE", "one"},
      {"FUNCTION", "DELETE", "one"},
      {"FUNCTION", "DELETE"},
      {"FUNCTION", "FLUSH", "WAT"},
      {"FUNCTION", "FLUSH"},
      {"FCALL", "echo", "0"},
      {"FUNCTION", "STATS"},
      {"FUNCTION", "KILL"},
      {"FUNCTION", "WAT"},
      {"FCALL"},
      {"EVAL", "return {redis.REDIS_VERSION, redis.REDIS_VERSION_NUM, redis.VALKEY_VERSION, redis.VALKEY_VERSION_NUM}", "0"},
  });
  both({"FUNCTION", "FLUSH"});
}

TEST_F(Differential, ScriptingSandbox) {
  script({
      {"EVAL", "redis.call = nil", "0"},
      {"EVAL", "cjson.encode = nil", "0"},
      {"EVAL", "string.foo = 1", "0"},
      {"EVAL", "rawset(_G, 'x', 1)", "0"},
      {"EVAL", "setmetatable(_G, nil)", "0"},
      {"EVAL", "return x", "0"},
      {"EVAL", "return _G.x", "0"},
      {"EVAL", "return rawget(_G, 'x')", "0"},
      {"EVAL", "return type(rawget(_G, 'pairs'))", "0"},
      {"EVAL", "return type(getmetatable(_G))", "0"},
      {"EVAL", "return getmetatable('').__index == string", "0"},
      {"EVAL", "return ('abc'):upper()", "0"},
      {"EVAL", "local f = loadstring('return 7'); return f()", "0"},
      {"EVAL", "local f = loadstring('y = 1'); return pcall(f)", "0"},
      {"EVAL", "local t = {}; t.x = 1; setmetatable(t, {}); rawset(t, 'y', 2); return t.x + t.y", "0"},
      {"EVAL",
       "local r = {} for _, n in ipairs({'dofile', 'loadfile', 'print', 'io', 'debug', 'package', 'require', 'module', 'getfenv', 'setfenv', 'newproxy', "
       "'coroutine', 'load', 'gcinfo', 'unpack', 'select'}) do r[#r + 1] = n .. '=' .. type(rawget(_G, n)) end return r",
       "0"},
      {"EVAL", "return {type(os.clock), type(os.time)}", "0"},
      {"EVAL", "local n = 0 for k, v in pairs(struct) do n = n + 1 end return n", "0"},
      {"EVAL", "local n = 0 for k, v in pairs(bit) do n = n + 1 end return n", "0"},
      {"EVAL", "local f, s = pairs(string); return s == string", "0"},
      {"EVAL", "local n = 0 for i, v in ipairs({5, 6, 7}) do n = n + v end return n", "0"},
  });
}

TEST_F(Differential, ScriptingLibraries) {
  script({
      {"EVAL", "return cjson.encode({1, 2, {a=1}})", "0"},
      {"EVAL", "return cjson.encode({a=1.5, b='x'})", "0"},
      {"EVAL", "return cjson.decode('[1,2,3.25]')", "0"},
      {"EVAL", "return cjson.encode(cjson.decode('[]'))", "0"},
      {"EVAL", "return cjson.encode(1/3)", "0"},
      {"EVAL", "return cjson.encode({[1]=1, [3]=3})", "0"},
      {"EVAL", "return cjson.encode({[1]=1, [30]=3})", "0"},
      {"EVAL", "return cjson.decode('{\"a\":[1,{\"b\":null}]}').a[1]", "0"},
      {"EVAL", "return cjson.decode('nope')", "0"},
      {"EVAL", "return cjson.encode('a\"b\\\\c/\\n')", "0"},
      {"EVAL", "return cjson.encode(1e300)", "0"},
      {"EVAL", "return bit.band(0xff, 0x0f)", "0"},
      {"EVAL", "return bit.tohex(255)", "0"},
      {"EVAL", "return bit.tobit(4294967296 + 5)", "0"},
      {"EVAL", "return {bit.bor(1, 2), bit.bxor(3, 1), bit.lshift(1, 4), bit.rshift(-1, 28), bit.arshift(-16, 2), bit.bnot(0), bit.rol(1, 33), bit.bswap(1)}",
       "0"},
      {"EVAL", "return cmsgpack.pack(1, 'a', {1, 2})", "0"},
      {"EVAL", "return cmsgpack.pack({a=1}, -1, 300, 70000, 1.5, true, false, string.rep('x', 40))", "0"},
      {"EVAL", "return {cmsgpack.unpack(cmsgpack.pack(1, 'a'))}", "0"},
      {"EVAL", "return struct.pack('HH', 1, 2)", "0"},
      {"EVAL", "return struct.pack('>i4', -2)", "0"},
      {"EVAL", "return {struct.unpack('<HB', '\\1\\0\\7')}", "0"},
      {"EVAL", "return struct.size('i4d')", "0"},
      {"EVAL", "return struct.size('!8bd')", "0"},
      {"EVAL", "return struct.pack('s', 'ab')", "0"},
      {"EVAL", "return {struct.unpack('bc0', struct.pack('bc0', 3, 'xyz'))}", "0"},
      {"EVAL", "return struct.pack('!4bi', 1, 2)", "0"},
      {"EVAL", "return struct.pack('x')", "0"},
      {"EVAL", "return struct.pack('<lLTfd', -3, 3, 4, 0.5, 0.25)", "0"},
      {"EVAL", "return struct.pack('>I3i16', 65536, -1)", "0"},
      {"EVAL", "return {struct.unpack('>I3i16', struct.pack('>I3i16', 65536, -1))}", "0"},
      {"EVAL", "return {struct.unpack('>d', struct.pack('>d', 1.5))}", "0"},
      {"EVAL", "return {struct.unpack('<h', '\\255\\255xx', 3)}", "0"},
      {"EVAL", "return struct.pack('q', 1)", "0"},
      {"EVAL", "return struct.pack('i33', 1)", "0"},
      {"EVAL", "return struct.pack('!3i', 1)", "0"},
      {"EVAL", "return struct.size('s')", "0"},
      {"EVAL", "return struct.unpack('i', 'ab')", "0"},
      {"EVAL", "return struct.unpack('s', 'ab')", "0"},
      {"EVAL", "return struct.unpack('c0', 'ab')", "0"},
      {"EVAL", "return struct.unpack('b', 'ab', 0)", "0"},
      {"EVAL", "return struct.pack('c2', 'a')", "0"},
  });
}

}  // namespace
}  // namespace mimir
