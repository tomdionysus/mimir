//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The GEO commands, as Valkey 8 documents and answers them: positions in a
// sorted set, GEOADD as ZADD over their geohashes, and the searches by radius
// or box from a position or a member, sorted or in Valkey's own order.

#include "mimir/keyspace/geo.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"

namespace mimir::command::geo {

namespace geo_math = keyspace::geo;

namespace {

using keyspace::SortedSet;

// A double as Valkey reads one: strtod's whole text, no leading space, not NaN.
std::optional<double> parse_double(const std::string& text) {
  if (text.empty() || std::isspace(static_cast<unsigned char>(text[0]))) return std::nullopt;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end != text.c_str() + text.size() || std::isnan(value)) return std::nullopt;
  return value;
}

double double_or(const std::string& text, const char* error) {
  auto value = parse_double(text);
  if (!value) throw CommandError(error);
  return *value;
}

geo_math::Point point_argument(const std::string& lon, const std::string& lat) {
  const double x = double_or(lon, not_float);
  const double y = double_or(lat, not_float);
  if (x < geo_math::longitude_min || x > geo_math::longitude_max || y < geo_math::latitude_min || y > geo_math::latitude_max) {
    char text[160];
    std::snprintf(text, sizeof(text), "ERR invalid longitude,latitude pair %f,%f", x, y);
    throw CommandError(text);
  }
  return geo_math::Point{x, y};
}

// Metres in one unit.
double unit_argument(const std::string& unit) {
  if (iequals(unit, "m")) return 1;
  if (iequals(unit, "km")) return 1000;
  if (iequals(unit, "ft")) return 0.3048;
  if (iequals(unit, "mi")) return 1609.34;
  throw CommandError("ERR unsupported unit provided. please use M, KM, FT, MI");
}

SortedSet* zset_of(Call& call, const std::string& key) {
  keyspace::Entry* entry = call.keys.find(key);
  if (!entry) return nullptr;
  if (entry->type() != keyspace::Type::zset) throw CommandError(wrong_type);
  return &std::get<SortedSet>(entry->value);
}

std::string four_places(double value) {
  char text[64];
  std::snprintf(text, sizeof(text), "%.4f", value);
  return text;
}

struct Found {
  std::string member;
  double distance;
  uint64_t score;
  geo_math::Point at;
};

enum Kind : unsigned { by_coords = 1, by_member = 2, no_store = 4, search = 8, search_store = 16 };

void search_generic(Call& call, size_t key_at, unsigned kind) {
  std::optional<std::string> store;
  bool store_dist = false;
  SortedSet* zset = zset_of(call, call.args[key_at]);
  geo_math::Shape shape;
  double conversion = 1;
  size_t base = 0;
  if (kind & by_coords) {
    base = 6;
    shape.centre = point_argument(call.args[2], call.args[3]);
    const double radius = double_or(call.args[4], "ERR need numeric radius");
    if (radius < 0) throw CommandError("ERR radius cannot be negative");
    conversion = unit_argument(call.args[5]);
    shape.radius = radius * conversion;
  } else if (kind & by_member) {
    base = 5;
    if (zset) {
      auto score = zset->score(call.args[2]);
      if (!score) throw CommandError("ERR could not decode requested zset member");
      shape.centre = geo_math::decode(static_cast<uint64_t>(*score));
      const double radius = double_or(call.args[3], "ERR need numeric radius");
      if (radius < 0) throw CommandError("ERR radius cannot be negative");
      conversion = unit_argument(call.args[4]);
      shape.radius = radius * conversion;
    }
  } else {
    base = 2;
    if (kind & search_store) {
      base = 3;
      store = call.args[1];
    }
  }
  bool with_dist = false, with_hash = false, with_coord = false, any = false;
  bool from_member = false, from_lonlat = false, by_radius = false, by_box = false;
  int sort = 0;  // 1 ascending, -1 descending
  int64_t count = 0;
  const size_t remaining = call.args.size() > base ? call.args.size() - base : 0;
  for (size_t i = 0; i < remaining; ++i) {
    const std::string& arg = call.args[base + i];
    if (iequals(arg, "WITHDIST")) {
      with_dist = true;
    } else if (iequals(arg, "WITHHASH")) {
      with_hash = true;
    } else if (iequals(arg, "WITHCOORD")) {
      with_coord = true;
    } else if (iequals(arg, "ANY")) {
      any = true;
    } else if (iequals(arg, "ASC")) {
      sort = 1;
    } else if (iequals(arg, "DESC")) {
      sort = -1;
    } else if (iequals(arg, "COUNT") && i + 1 < remaining) {
      count = integer_argument(call.args[base + i + 1]);
      if (count <= 0) throw CommandError("ERR COUNT must be > 0");
      ++i;
    } else if ((iequals(arg, "STORE") || iequals(arg, "STOREDIST")) && i + 1 < remaining && !(kind & no_store) && !(kind & search)) {
      store = call.args[base + i + 1];
      store_dist = iequals(arg, "STOREDIST");
      ++i;
    } else if (iequals(arg, "STOREDIST") && (kind & search) && (kind & search_store)) {
      store_dist = true;
    } else if (iequals(arg, "FROMMEMBER") && i + 1 < remaining && (kind & search) && !from_lonlat) {
      if (zset) {
        auto score = zset->score(call.args[base + i + 1]);
        if (!score) throw CommandError("ERR could not decode requested zset member");
        shape.centre = geo_math::decode(static_cast<uint64_t>(*score));
      }
      from_member = true;
      ++i;
    } else if (iequals(arg, "FROMLONLAT") && i + 2 < remaining && (kind & search) && !from_member) {
      shape.centre = point_argument(call.args[base + i + 1], call.args[base + i + 2]);
      from_lonlat = true;
      i += 2;
    } else if (iequals(arg, "BYRADIUS") && i + 2 < remaining && (kind & search) && !by_box) {
      const double radius = double_or(call.args[base + i + 1], "ERR need numeric radius");
      if (radius < 0) throw CommandError("ERR radius cannot be negative");
      conversion = unit_argument(call.args[base + i + 2]);
      shape.box = false;
      shape.radius = radius * conversion;
      by_radius = true;
      i += 2;
    } else if (iequals(arg, "BYBOX") && i + 3 < remaining && (kind & search) && !by_radius) {
      const double width = double_or(call.args[base + i + 1], "ERR need numeric width");
      const double height = double_or(call.args[base + i + 2], "ERR need numeric height");
      if (height < 0 || width < 0) throw CommandError("ERR height or width cannot be negative");
      conversion = unit_argument(call.args[base + i + 3]);
      shape.box = true;
      shape.width = width * conversion;
      shape.height = height * conversion;
      by_box = true;
      i += 3;
    } else {
      throw CommandError(syntax_error);
    }
  }
  if (store && (with_dist || with_hash || with_coord)) {
    throw CommandError(std::string("ERR ") + ((kind & search_store) ? "GEOSEARCHSTORE" : "STORE option in GEORADIUS") +
                       " is not compatible with WITHDIST, WITHHASH and WITHCOORD options");
  }
  if ((kind & search) && !(from_member || from_lonlat)) {
    throw CommandError("ERR exactly one of FROMMEMBER or FROMLONLAT can be specified for " + lower(call.args[0]));
  }
  if ((kind & search) && !(by_radius || by_box)) {
    throw CommandError("ERR exactly one of BYRADIUS and BYBOX can be specified for " + lower(call.args[0]));
  }
  if (any && !count) throw CommandError("ERR the ANY argument requires COUNT argument");
  if (!zset) {
    if (store) {
      if (call.keys.erase(*store)) call.notify('g', "del", *store);
      call.reply.integer(0);
    } else {
      call.reply.array(0);
    }
    return;
  }
  if (count != 0 && sort == 0 && !any) sort = 1;

  // The cells in Valkey's order, each read in score order.
  std::vector<Found> found;
  const size_t limit = any ? static_cast<size_t>(count) : 0;
  for (const auto& [min, max] : geo_math::search_cells(shape)) {
    if (limit && found.size() >= limit) break;
    const size_t first = zset->count_below_score(static_cast<double>(min), false);
    for (size_t r = first; r < zset->size(); ++r) {
      const auto& item = zset->at(r);
      if (item.score >= static_cast<double>(max)) break;
      const auto score = static_cast<uint64_t>(item.score);
      const geo_math::Point at = geo_math::decode(score);
      auto d = geo_math::within(shape, at);
      if (!d) continue;
      found.push_back(Found{item.member, *d, score, at});
      if (limit && found.size() >= limit) break;
    }
  }
  if (found.empty() && !store) {
    call.reply.array(0);
    return;
  }
  const size_t returned = count == 0 || found.size() < static_cast<size_t>(count) ? found.size() : static_cast<size_t>(count);
  if (sort != 0) {
    std::stable_sort(found.begin(), found.end(), [&](const Found& a, const Found& b) { return sort > 0 ? a.distance < b.distance : a.distance > b.distance; });
  }
  if (!store) {
    const size_t options = (with_dist ? 1 : 0) + (with_coord ? 1 : 0) + (with_hash ? 1 : 0);
    call.reply.array(returned);
    for (size_t i = 0; i < returned; ++i) {
      const Found& f = found[i];
      if (options) call.reply.array(options + 1);
      call.reply.bulk(f.member);
      if (with_dist) call.reply.bulk(four_places(f.distance / conversion));
      if (with_hash) call.reply.integer(static_cast<int64_t>(f.score));
      if (with_coord) {
        call.reply.array(2);
        call.reply.bulk(format_long_double(f.at.longitude));
        call.reply.bulk(format_long_double(f.at.latitude));
      }
    }
    return;
  }
  if (returned) {
    SortedSet made;
    for (size_t i = 0; i < returned; ++i) made.set(found[i].member, store_dist ? found[i].distance / conversion : static_cast<double>(found[i].score));
    // Its encoding as its size gives, as Valkey's store makes it.
    made.settle_new([&call]() DANNAN_NO_THREAD_SAFETY_ANALYSIS { return call.database.limits_locked(); }());
    call.keys.put(*store, keyspace::Entry{std::move(made), std::nullopt});
    call.notify('z', (kind & search) ? "geosearchstore" : "georadiusstore", *store);
  } else if (call.keys.erase(*store)) {
    call.notify('g', "del", *store);
  }
  call.reply.integer(static_cast<int64_t>(returned));
}

}  // namespace

void geoadd(Call& call) {
  size_t at = 2;
  bool nx = false, xx = false;
  while (at < call.args.size()) {
    if (iequals(call.args[at], "NX"))
      nx = true;
    else if (iequals(call.args[at], "XX"))
      xx = true;
    else if (!iequals(call.args[at], "CH"))
      break;
    ++at;
  }
  if ((call.args.size() - at) % 3 != 0 || (xx && nx)) throw CommandError(syntax_error);
  // ZADD key [options] score member ..., each score a position's geohash.
  std::vector<std::string> args(call.args.begin(), call.args.begin() + static_cast<std::ptrdiff_t>(at));
  args[0] = "zadd";
  for (size_t i = at; i < call.args.size(); i += 3) {
    const geo_math::Point p = point_argument(call.args[i], call.args[i + 1]);
    args.push_back(std::to_string(*geo_math::encode(p)));
    args.push_back(call.args[i + 2]);
  }
  Call zadd_call{call.session, call.database, call.keys, call.reply, args};
  zadd_call.db = call.db;
  zsets::zadd(zadd_call);
}

void geodist(Call& call) {
  if (call.args.size() > 5) throw CommandError(syntax_error);
  const double conversion = call.args.size() == 5 ? unit_argument(call.args[4]) : 1;
  SortedSet* zset = zset_of(call, call.args[1]);
  auto a = zset ? zset->score(call.args[2]) : std::nullopt;
  auto b = zset ? zset->score(call.args[3]) : std::nullopt;
  if (!a || !b) {
    call.reply.null();
    return;
  }
  const double d = geo_math::distance(geo_math::decode(static_cast<uint64_t>(*a)), geo_math::decode(static_cast<uint64_t>(*b)));
  call.reply.bulk(four_places(d / conversion));
}

void geohash(Call& call) {
  SortedSet* zset = zset_of(call, call.args[1]);
  call.reply.array(call.args.size() - 2);
  for (size_t i = 2; i < call.args.size(); ++i) {
    auto score = zset ? zset->score(call.args[i]) : std::nullopt;
    if (score)
      call.reply.bulk(geo_math::geohash_text(static_cast<uint64_t>(*score)));
    else
      call.reply.null();
  }
}

void geopos(Call& call) {
  SortedSet* zset = zset_of(call, call.args[1]);
  call.reply.array(call.args.size() - 2);
  for (size_t i = 2; i < call.args.size(); ++i) {
    auto score = zset ? zset->score(call.args[i]) : std::nullopt;
    if (!score) {
      call.reply.null_array();
      continue;
    }
    const geo_math::Point p = geo_math::decode(static_cast<uint64_t>(*score));
    call.reply.array(2);
    call.reply.bulk(format_long_double(p.longitude));
    call.reply.bulk(format_long_double(p.latitude));
  }
}

void georadius(Call& call) { search_generic(call, 1, by_coords); }
void georadius_ro(Call& call) { search_generic(call, 1, by_coords | no_store); }
void georadiusbymember(Call& call) { search_generic(call, 1, by_member); }
void georadiusbymember_ro(Call& call) { search_generic(call, 1, by_member | no_store); }
void geosearch(Call& call) { search_generic(call, 1, search); }
void geosearchstore(Call& call) { search_generic(call, 2, search | search_store); }

// GEORADIUS reads, but with STORE or STOREDIST it replaces a key, by agreement.
Class classify_georadius(const std::vector<std::string>& args) {
  for (size_t i = 2; i < args.size(); ++i) {
    if (iequals(args[i], "STORE") || iequals(args[i], "STOREDIST")) return Class::agreement;
  }
  return Class::read;
}

// GEORADIUS's keys: the key searched and STORE's or STOREDIST's.
std::vector<std::string_view> georadius_keys(const std::vector<std::string>& args) {
  std::vector<std::string_view> keys;
  if (args.size() < 2) return keys;
  keys.push_back(args[1]);
  for (size_t i = 2; i + 1 < args.size(); ++i) {
    if (iequals(args[i], "STORE") || iequals(args[i], "STOREDIST")) keys.push_back(args[++i]);
  }
  return keys;
}

}  // namespace mimir::command::geo
