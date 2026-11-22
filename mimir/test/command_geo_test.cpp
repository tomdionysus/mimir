//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The GEO commands, from Valkey 8's documentation: its examples (Palermo and
// Catania in "Sicily") with the answers the documentation prints.

#include <gtest/gtest.h>

#include "mimir/test/fixtures/bed.h"

namespace mimir::test {
namespace {

std::string bulks(const std::vector<std::string>& items) {
  std::vector<std::string> out;
  for (const auto& item : items) out.push_back(bulk(item));
  return array(out);
}

void sicily(Bed& bed) { ASSERT_EQ(bed.run({"GEOADD", "Sicily", "13.361389", "38.115556", "Palermo", "15.087269", "37.502669", "Catania"}), integer(2)); }

TEST(CommandGeo, DistanceAndUnits) {
  Bed bed;
  sicily(bed);
  EXPECT_EQ(bed.run({"GEODIST", "Sicily", "Palermo", "Catania"}), bulk("166274.1516"));
  EXPECT_EQ(bed.run({"GEODIST", "Sicily", "Palermo", "Catania", "km"}), bulk("166.2742"));
  EXPECT_EQ(bed.run({"GEODIST", "Sicily", "Palermo", "Catania", "mi"}), bulk("103.3182"));
  EXPECT_EQ(bed.run({"GEODIST", "Sicily", "Foo", "Bar"}), nil());
  EXPECT_EQ(bed.run({"GEODIST", "Sicily", "Palermo", "Catania", "parsecs"}), error("ERR unsupported unit provided. please use M, KM, FT, MI"));
}

TEST(CommandGeo, HashAndPosition) {
  Bed bed;
  sicily(bed);
  EXPECT_EQ(bed.run({"GEOHASH", "Sicily", "Palermo", "Catania"}), bulks({"sqc8b49rny0", "sqdtr74hyu0"}));
  EXPECT_EQ(bed.run({"GEOPOS", "Sicily", "Palermo", "Catania", "NonExisting"}),
            array({bulks({"13.36138933897018433", "38.11555639549629859"}), bulks({"15.08726745843887329", "37.50266842333162032"}), "*-1\r\n"}));
}

TEST(CommandGeo, Search) {
  Bed bed;
  sicily(bed);
  bed.run({"GEOADD", "Sicily", "12.758489", "38.788135", "edge1", "17.241510", "38.788135", "edge2"});
  EXPECT_EQ(bed.run({"GEORADIUS", "Sicily", "15", "37", "200", "km", "WITHDIST"}),
            array({array({bulk("Palermo"), bulk("190.4424")}), array({bulk("Catania"), bulk("56.4413")})}));
  EXPECT_EQ(bed.run({"GEORADIUS", "Sicily", "15", "37", "200", "km", "WITHDIST", "ASC"}),
            array({array({bulk("Catania"), bulk("56.4413")}), array({bulk("Palermo"), bulk("190.4424")})}));
  EXPECT_EQ(bed.run({"GEOSEARCH", "Sicily", "FROMLONLAT", "15", "37", "BYBOX", "400", "400", "km", "ASC", "WITHDIST"}),
            array({array({bulk("Catania"), bulk("56.4413")}), array({bulk("Palermo"), bulk("190.4424")}), array({bulk("edge2"), bulk("279.7403")}),
                   array({bulk("edge1"), bulk("279.7405")})}));
  EXPECT_EQ(bed.run({"GEORADIUSBYMEMBER", "Sicily", "Palermo", "200", "km", "ASC"}), bulks({"Palermo", "edge1", "Catania"}));
  EXPECT_EQ(bed.run({"GEOSEARCHSTORE", "out", "Sicily", "FROMMEMBER", "Palermo", "BYRADIUS", "200", "km", "COUNT", "2", "ASC"}), integer(2));
  EXPECT_EQ(bed.run({"ZSCORE", "out", "Catania"}), nil());
  EXPECT_EQ(bed.run({"ZCARD", "out"}), integer(2));
}

TEST(CommandGeo, Errors) {
  Bed bed;
  EXPECT_EQ(bed.run({"GEOADD", "k", "1", "2"}), error("ERR wrong number of arguments for 'geoadd' command"));
  EXPECT_EQ(bed.run({"GEOADD", "k", "NX", "XX", "1", "2", "m"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"GEOADD", "k", "x", "2", "m"}), error("ERR value is not a valid float"));
  EXPECT_EQ(bed.run({"GEOSEARCH", "k", "FROMLONLAT", "1", "2", "BYRADIUS", "1", "km", "ANY"}), error("ERR the ANY argument requires COUNT argument"));
  EXPECT_EQ(bed.run({"GEOSEARCH", "k", "BYRADIUS", "1", "km", "ASC", "x"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"GEOSEARCH", "k", "BYRADIUS", "1", "km", "ASC", "WITHDIST"}),
            error("ERR exactly one of FROMMEMBER or FROMLONLAT can be specified for geosearch"));
  EXPECT_EQ(bed.run({"GEORADIUS", "k", "1", "2", "-1", "km"}), error("ERR radius cannot be negative"));
  EXPECT_EQ(bed.run({"GEORADIUS", "k", "1", "2", "1", "km"}), array({}));
}

// Valkey 8.1.1's answers (probed) for the rest of the refusals and the less
// used forms: the replies' extra fields, the read-only variants, and a store
// that finds nothing.
TEST(CommandGeo, RefusalsAndFormsAsValkeyAnswers) {
  Bed bed;
  sicily(bed);
  EXPECT_EQ(bed.run({"GEOADD", "Sicily", "200", "100", "bad"}), error("ERR invalid longitude,latitude pair 200.000000,100.000000"));
  EXPECT_EQ(bed.run({"GEORADIUSBYMEMBER", "Sicily", "nosuch", "10", "km"}), error("ERR could not decode requested zset member"));
  EXPECT_EQ(bed.run({"GEOSEARCH", "Sicily", "FROMMEMBER", "nosuch", "BYRADIUS", "10", "km"}), error("ERR could not decode requested zset member"));
  EXPECT_EQ(bed.run({"GEOSEARCH", "Sicily", "FROMLONLAT", "15", "37", "BYBOX", "-1", "10", "km"}), error("ERR height or width cannot be negative"));
  EXPECT_EQ(bed.run({"GEOSEARCH", "Sicily", "FROMLONLAT", "15", "37", "BYBOX", "x", "10", "km"}), error("ERR need numeric width"));
  EXPECT_EQ(bed.run({"GEOSEARCH", "Sicily", "FROMLONLAT", "15", "37", "BYBOX", "10", "x", "km"}), error("ERR need numeric height"));
  EXPECT_EQ(bed.run({"GEOSEARCH", "Sicily", "FROMLONLAT", "15", "37", "BYRADIUS", "-1", "km"}), error("ERR radius cannot be negative"));
  EXPECT_EQ(bed.run({"GEOSEARCHSTORE", "d", "Sicily", "FROMLONLAT", "15", "37", "BYRADIUS", "200", "km", "WITHDIST"}),
            error("ERR GEOSEARCHSTORE is not compatible with WITHDIST, WITHHASH and WITHCOORD options"));
  EXPECT_EQ(bed.run({"GEORADIUS", "Sicily", "15", "37", "200", "km", "STORE", "d", "WITHDIST"}),
            error("ERR STORE option in GEORADIUS is not compatible with WITHDIST, WITHHASH and WITHCOORD options"));
  EXPECT_EQ(bed.run({"GEORADIUS", "Sicily", "15", "37", "200", "km", "COUNT", "0"}), error("ERR COUNT must be > 0"));
  EXPECT_EQ(bed.run({"GEORADIUS", "Sicily", "15", "37", "200", "km", "BOGUS"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"GEORADIUS", "Sicily", "15", "37", "x", "km"}), error("ERR need numeric radius"));
  EXPECT_EQ(bed.run({"GEORADIUS", "Sicily", "15", "37", "200", "parsec"}), error("ERR unsupported unit provided. please use M, KM, FT, MI"));
  EXPECT_EQ(bed.run({"GEORADIUS_RO", "Sicily", "15", "37", "200", "km", "STORE", "d"}), error("ERR syntax error"));

  EXPECT_EQ(bed.run({"GEORADIUS", "Sicily", "15", "37", "200", "km", "COUNT", "1", "ANY"}), bulks({"Palermo"}));
  EXPECT_EQ(bed.run({"GEORADIUS", "Sicily", "15", "37", "200", "km", "WITHCOORD", "WITHHASH", "WITHDIST", "ASC"}),
            array({array({bulk("Catania"), bulk("56.4413"), integer(3479447370796909), bulks({"15.08726745843887329", "37.50266842333162032"})}),
                   array({bulk("Palermo"), bulk("190.4424"), integer(3479099956230698), bulks({"13.36138933897018433", "38.11555639549629859"})})}));
  EXPECT_EQ(bed.run({"GEORADIUS", "Sicily", "15", "37", "200", "km", "DESC"}), bulks({"Palermo", "Catania"}));
  EXPECT_EQ(bed.run({"GEORADIUS", "Sicily", "0", "0", "1", "km"}), array({}));
  EXPECT_EQ(bed.run({"GEORADIUSBYMEMBER_RO", "Sicily", "Palermo", "200", "km"}), bulks({"Palermo", "Catania"}));

  // A store that finds nothing, or reads a missing key, removes its destination.
  bed.run({"SET", "d", "x"});
  EXPECT_EQ(bed.run({"GEORADIUS", "nokey", "15", "37", "200", "km", "STORE", "d"}), integer(0));
  EXPECT_EQ(bed.run({"EXISTS", "d"}), integer(0));
  bed.run({"SET", "d", "x"});
  EXPECT_EQ(bed.run({"GEORADIUS", "Sicily", "0", "0", "1", "km", "STORE", "d"}), integer(0));
  EXPECT_EQ(bed.run({"EXISTS", "d"}), integer(0));
  EXPECT_EQ(bed.run({"GEOSEARCHSTORE", "d", "Sicily", "FROMLONLAT", "15", "37", "BYRADIUS", "200", "km", "STOREDIST"}), integer(2));
  // Valkey's scores were 56.44125787015819 and 190.44242984775792 on arm64;
  // their last digits follow whether the compiler fused multiply-adds (an
  // optimised build does, a debug build and x86-64's baseline do not), in
  // Valkey as here, so they are compared to a part in 10^12.
  const std::string stored = bed.run({"ZRANGE", "d", "0", "-1", "WITHSCORES"});
  ASSERT_EQ(stored.substr(0, 17), "*4\r\n$7\r\nCatania\r\n") << stored;
  const auto score_after = [&](const std::string& member) {
    const size_t at = stored.find(member + "\r\n$") + member.size() + 2;
    const size_t text = stored.find("\r\n", at) + 2;
    return std::stod(stored.substr(text, stored.find("\r\n", text) - text));
  };
  EXPECT_NEAR(score_after("Catania"), 56.44125787015819, 56.44125787015819 * 1e-12);
  EXPECT_NEAR(score_after("Palermo"), 190.44242984775792, 190.44242984775792 * 1e-12);

  EXPECT_EQ(bed.run({"GEODIST", "Sicily", "Palermo", "Catania", "km", "extra"}), error("ERR syntax error"));
  EXPECT_EQ(bed.run({"GEOHASH", "Sicily", "Palermo", "nosuch"}), array({bulk("sqc8b49rny0"), nil()}));
  EXPECT_EQ(bed.run({"GEOHASH", "nokey", "a"}), array({nil()}));
  EXPECT_EQ(bed.run({"GEOPOS", "nokey", "a"}), array({"*-1\r\n"}));
}

TEST(CommandGeo, SearchNeedsAShape) {
  Bed bed;
  EXPECT_EQ(bed.run({"GEOSEARCH", "g", "FROMLONLAT", "0", "0", "ASC", "WITHDIST"}),
            error("ERR exactly one of BYRADIUS and BYBOX can be specified for geosearch"));
}

}  // namespace
}  // namespace mimir::test
