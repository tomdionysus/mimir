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

}  // namespace
}  // namespace mimir::test
