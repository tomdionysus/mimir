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
    std::string request = "*" + std::to_string(args.size()) + "\r\n";
    for (const auto& arg : args) request += "$" + std::to_string(arg.size()) + "\r\n" + arg + "\r\n";
    asio::write(_socket, asio::buffer(request));
    return read_reply();
  }

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
      // Not compared: COPY ... DB 1. Mimir has one database, as Valkey's
      // cluster mode does; standalone Valkey has sixteen (for Tom).
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

}  // namespace
}  // namespace mimir
