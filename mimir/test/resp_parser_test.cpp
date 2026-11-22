//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The request side of RESP, from the protocol's specification ("Sending
// commands to a server": an array of bulk strings, or an inline command) and
// Valkey's documented protocol errors. A request may arrive in any number of
// pieces; a piece may hold several requests (pipelining).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "mimir/resp/parser.h"

namespace mimir::resp {
namespace {

using Args = std::vector<std::string>;

std::string error_of(RequestParser& parser) {
  try {
    parser.next();
  } catch (const ProtocolError& e) {
    return e.what();
  }
  return "";
}

TEST(RespParser, AnArrayOfBulkStringsIsOneCommand) {
  RequestParser parser;
  parser.feed("*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n");
  EXPECT_EQ(parser.next(), (Args{"GET", "key"}));
  EXPECT_EQ(parser.next(), std::nullopt);
  EXPECT_EQ(parser.buffered(), 0u);
}

TEST(RespParser, ACommandArrivesInAnyPieces) {
  const std::string request = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhello\r\n";
  for (size_t cut = 1; cut < request.size(); ++cut) {
    RequestParser parser;
    parser.feed(request.substr(0, cut));
    EXPECT_EQ(parser.next(), std::nullopt) << cut;
    parser.feed(request.substr(cut));
    EXPECT_EQ(parser.next(), (Args{"SET", "k", "hello"})) << cut;
  }
}

TEST(RespParser, ByteAtATime) {
  const std::string request = "*2\r\n$4\r\nECHO\r\n$0\r\n\r\n";
  RequestParser parser;
  std::optional<Args> got;
  for (char c : request) {
    EXPECT_FALSE(got.has_value());
    parser.feed(std::string(1, c));
    got = parser.next();
  }
  EXPECT_EQ(got, (Args{"ECHO", ""}));
}

TEST(RespParser, PipelinedCommandsComeOutInOrder) {
  RequestParser parser;
  parser.feed("*1\r\n$4\r\nPING\r\n*2\r\n$4\r\nECHO\r\n$2\r\nhi\r\nPING x\r\n");
  EXPECT_EQ(parser.next(), (Args{"PING"}));
  EXPECT_EQ(parser.next(), (Args{"ECHO", "hi"}));
  EXPECT_EQ(parser.next(), (Args{"PING", "x"}));
  EXPECT_EQ(parser.next(), std::nullopt);
}

TEST(RespParser, ABulkStringIsBinarySafe) {
  RequestParser parser;
  const std::string value("a\r\nb\0c", 6);
  parser.feed("*2\r\n$4\r\nECHO\r\n$6\r\n" + value + "\r\n");
  EXPECT_EQ(parser.next(), (Args{"ECHO", value}));
}

TEST(RespParser, AnInlineCommandIsSplitOnSpaces) {
  RequestParser parser;
  parser.feed("SET  key   value\r\nGET key\n");
  EXPECT_EQ(parser.next(), (Args{"SET", "key", "value"}));
  EXPECT_EQ(parser.next(), (Args{"GET", "key"}));
}

TEST(RespParser, EmptyLinesAndEmptyArraysAreSkipped) {
  RequestParser parser;
  // Valkey skips any count below one, not only -1.
  parser.feed("\r\n\n*0\r\n*-1\r\n*-7\r\nPING\r\n");
  EXPECT_EQ(parser.next(), (Args{"PING"}));
}

TEST(RespParser, InlineQuotingFollowsValkey) {
  EXPECT_EQ(split_inline("SET k \"a b\""), (Args{"SET", "k", "a b"}));
  EXPECT_EQ(split_inline("SET k \"\\x41\\n\\t\\\\\\\"\""), (Args{"SET", "k", "A\n\t\\\""}));
  EXPECT_EQ(split_inline("SET k 'it\\'s'"), (Args{"SET", "k", "it's"}));
  EXPECT_EQ(split_inline("SET k 'a\\nb'"), (Args{"SET", "k", "a\\nb"}));
  EXPECT_EQ(split_inline("SET k \"\""), (Args{"SET", "k", ""}));
  EXPECT_EQ(split_inline("  "), Args{});
  // Upper-case hex digits, and the rest of sdssplitargs's escapes.
  EXPECT_EQ(split_inline("SET k \"\\xAb\\x0F\\r\\t\\b\\a\\q\""), (Args{"SET", "k", "\xab\x0f\r\t\b\aq"}));
}

TEST(RespParser, AnInlineLineWithoutItsEndWaits) {
  RequestParser parser;
  parser.feed("PIN");
  EXPECT_FALSE(parser.next());
  parser.feed("G\r\n");
  EXPECT_EQ(parser.next(), (std::optional<Args>(Args{"PING"})));
}

TEST(RespParser, UnbalancedQuotesAreRefused) {
  for (const char* line : {"SET k \"abc", "SET k 'abc", "SET k \"a\"b", "SET k 'a'b"}) {
    EXPECT_THROW(split_inline(line), ProtocolError) << line;
  }
  RequestParser parser;
  parser.feed("SET k \"abc\r\n");
  EXPECT_EQ(error_of(parser), "Protocol error: unbalanced quotes in request");
}

TEST(RespParser, AnArrayCountMustBeANumberInBounds) {
  ParserLimits limits;
  limits.max_arguments = 4;
  for (const char* bytes : {"*x\r\n", "*5\r\n", "*1x\r\n", "*\r\n"}) {
    RequestParser parser(limits);
    parser.feed(bytes);
    EXPECT_EQ(error_of(parser), "Protocol error: invalid multibulk length") << bytes;
  }
}

// An inline command is held to the same count as an array (found by the
// request parser's fuzz target, 2026-11-15: a line of control bytes split into
// more words than the limit).
TEST(RespParser, AnInlineCommandIsBoundedAsAnArrayIs) {
  ParserLimits limits;
  limits.max_arguments = 4;
  limits.restricted_arguments = 2;
  {
    RequestParser parser(limits);
    parser.feed("a\rb\tc\vd\n");
    EXPECT_EQ(parser.next(), (std::vector<std::string>{"a", "b", "c", "d"}));
    parser.feed("a b c d e\n");
    EXPECT_EQ(error_of(parser), "Protocol error: invalid multibulk length");
  }
  {
    RequestParser parser(limits);
    parser.restrict(true);
    parser.feed("a b c\n");
    EXPECT_EQ(error_of(parser), "Protocol error: unauthenticated multibulk length");
  }
}

TEST(RespParser, AnArrayHoldsOnlyBulkStrings) {
  RequestParser parser;
  parser.feed("*1\r\n:1\r\n");
  EXPECT_EQ(error_of(parser), "Protocol error: expected '$', got ':'");
}

TEST(RespParser, ABulkLengthMustBeANumberInBounds) {
  ParserLimits limits;
  limits.max_bulk = 8;
  for (const char* bytes : {"*1\r\n$x\r\n", "*1\r\n$9\r\n", "*1\r\n$-1\r\n"}) {
    RequestParser parser(limits);
    parser.feed(bytes);
    EXPECT_EQ(error_of(parser), "Protocol error: invalid bulk length") << bytes;
  }
}

TEST(RespParser, ABulkStringEndsWithALineEnd) {
  RequestParser parser;
  parser.feed("*1\r\n$4\r\nPINGxx");
  EXPECT_EQ(error_of(parser), "Protocol error: expected '\\r\\n' after bulk string");
}

TEST(RespParser, LinesWithoutAnEndAreBounded) {
  ParserLimits limits;
  limits.max_inline = 16;
  {
    RequestParser parser(limits);
    parser.feed(std::string(17, 'a'));
    EXPECT_EQ(error_of(parser), "Protocol error: too big inline request");
  }
  {
    RequestParser parser(limits);
    parser.feed("*" + std::string(17, '1'));
    EXPECT_EQ(error_of(parser), "Protocol error: too big mbulk count string");
  }
  {
    RequestParser parser(limits);
    parser.feed("*1\r\n$" + std::string(17, '1'));
    EXPECT_EQ(error_of(parser), "Protocol error: too big bulk count string");
  }
}

TEST(RespParser, ALongInlineLineWithItsEndIsStillTooBig) {
  ParserLimits limits;
  limits.max_inline = 16;
  RequestParser parser(limits);
  parser.feed(std::string(20, 'a') + "\r\n");
  EXPECT_EQ(error_of(parser), "Protocol error: too big inline request");
}

TEST(RespParser, ARestrictedConnectionHasSmallLimits) {
  RequestParser parser;
  parser.restrict(true);
  parser.feed("*11\r\n");
  EXPECT_EQ(error_of(parser), "Protocol error: unauthenticated multibulk length");

  RequestParser second;
  second.restrict(true);
  second.feed("*2\r\n$4\r\nAUTH\r\n$16385\r\n");
  EXPECT_EQ(error_of(second), "Protocol error: unauthenticated bulk length");

  RequestParser third;
  third.restrict(true);
  third.feed("*2\r\n$4\r\nAUTH\r\n$6\r\nsecret\r\n");
  EXPECT_EQ(third.next(), (Args{"AUTH", "secret"}));
}

TEST(RespParser, ASpentParserStaysSpent) {
  RequestParser parser;
  parser.feed("*x\r\n");
  EXPECT_THROW(parser.next(), ProtocolError);
  parser.feed("*1\r\n$4\r\nPING\r\n");
  EXPECT_THROW(parser.next(), ProtocolError);
}

TEST(RespParser, NothingIsAllocatedForALengthNotYetReceived) {
  // A client that announces a large bulk and sends nothing holds only what it
  // sent: the parser does not reserve the announced length up front.
  RequestParser parser;
  parser.feed("*1\r\n$536870912\r\n");
  EXPECT_EQ(parser.next(), std::nullopt);
  EXPECT_LT(parser.buffered(), 64u);
}

}  // namespace
}  // namespace mimir::resp
