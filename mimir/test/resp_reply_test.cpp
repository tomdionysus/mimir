//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The reply side of RESP, from the protocol's specification: each RESP3 type
// as it is written in RESP3, and as RESP2 carries it to a client that has not
// said HELLO 3.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <limits>

#include "mimir/resp/reply.h"

namespace mimir::resp {
namespace {

template <typename Write>
std::pair<std::string, std::string> both(Write write) {
  Reply two(Protocol::resp2);
  Reply three(Protocol::resp3);
  write(two);
  write(three);
  return {two.bytes(), three.bytes()};
}

TEST(RespReply, TheSimpleTypesAreTheSameInBoth) {
  EXPECT_EQ(both([](Reply& r) { r.simple("OK"); }), std::make_pair(std::string("+OK\r\n"), std::string("+OK\r\n")));
  EXPECT_EQ(both([](Reply& r) { r.error("ERR syntax error"); }).first, "-ERR syntax error\r\n");
  EXPECT_EQ(both([](Reply& r) { r.integer(-42); }).second, ":-42\r\n");
  EXPECT_EQ(both([](Reply& r) { r.bulk("hi"); }).first, "$2\r\nhi\r\n");
  EXPECT_EQ(both([](Reply& r) { r.bulk(""); }).second, "$0\r\n\r\n");
  EXPECT_EQ(both([](Reply& r) { r.array(2); }).first, "*2\r\n");
}

TEST(RespReply, AnErrorIsOneLine) {
  Reply r;
  r.error("ERR a\r\nb\nc");
  EXPECT_EQ(r.bytes(), "-ERR a  b c\r\n");
}

TEST(RespReply, ASimpleStringIsOneLine) {
  Reply r;
  r.simple("a\r\nb");
  EXPECT_EQ(r.bytes(), "+a  b\r\n");
}

TEST(RespReply, NullsDifferByProtocol) {
  EXPECT_EQ(both([](Reply& r) { r.null(); }), std::make_pair(std::string("$-1\r\n"), std::string("_\r\n")));
  EXPECT_EQ(both([](Reply& r) { r.null_array(); }), std::make_pair(std::string("*-1\r\n"), std::string("_\r\n")));
}

TEST(RespReply, AMapIsAFlatArrayInResp2) {
  EXPECT_EQ(both([](Reply& r) { r.map(2); }), std::make_pair(std::string("*4\r\n"), std::string("%2\r\n")));
}

TEST(RespReply, ASetAndAPushAreArraysInResp2) {
  EXPECT_EQ(both([](Reply& r) { r.set(3); }), std::make_pair(std::string("*3\r\n"), std::string("~3\r\n")));
  EXPECT_EQ(both([](Reply& r) { r.push(3); }), std::make_pair(std::string("*3\r\n"), std::string(">3\r\n")));
}

TEST(RespReply, ADoubleIsABulkStringInResp2) {
  EXPECT_EQ(both([](Reply& r) { r.real(1.5); }), std::make_pair(std::string("$3\r\n1.5\r\n"), std::string(",1.5\r\n")));
  EXPECT_EQ(both([](Reply& r) { r.real(std::numeric_limits<double>::infinity()); }).second, ",inf\r\n");
  EXPECT_EQ(both([](Reply& r) { r.real(-std::numeric_limits<double>::infinity()); }).first, "$4\r\n-inf\r\n");
}

TEST(RespReply, ABooleanIsAnIntegerInResp2) {
  EXPECT_EQ(both([](Reply& r) { r.boolean(true); }), std::make_pair(std::string(":1\r\n"), std::string("#t\r\n")));
  EXPECT_EQ(both([](Reply& r) { r.boolean(false); }), std::make_pair(std::string(":0\r\n"), std::string("#f\r\n")));
}

TEST(RespReply, VerbatimTextIsABulkStringInResp2) {
  EXPECT_EQ(both([](Reply& r) { r.verbatim("txt", "abc"); }), std::make_pair(std::string("$3\r\nabc\r\n"), std::string("=7\r\ntxt:abc\r\n")));
}

TEST(RespReply, DoublesAreTheShortestThatReadsBack) {
  EXPECT_EQ(format_double(0.1), "0.1");
  EXPECT_EQ(format_double(3.0), "3");
  EXPECT_EQ(format_double(-2.5), "-2.5");
  EXPECT_EQ(format_double(1e300), "1e+300");
  EXPECT_EQ(format_double(std::nan("")), "nan");
  for (double v : {0.1, 1.0 / 3.0, 123456.789, 5e-324, 1.7976931348623157e308}) {
    // strtod, not stod: stod throws on a subnormal such as 5e-324.
    EXPECT_EQ(std::strtod(format_double(v).c_str(), nullptr), v) << v;
  }
}

}  // namespace
}  // namespace mimir::resp
