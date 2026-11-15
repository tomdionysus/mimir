//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/keyspace/geo.h"

#include <cmath>
#include <vector>

namespace mimir::keyspace::geo {

namespace {

constexpr double earth_radius = 6372797.560856;
constexpr double mercator_max = 20037726.37;
constexpr double degree = M_PI / 180.0;

double to_radians(double degrees) { return degrees * degree; }
double to_degrees(double radians) { return radians / degree; }

struct Range {
  double min;
  double max;
};

constexpr Range valkey_longitude{longitude_min, longitude_max};
constexpr Range valkey_latitude{latitude_min, latitude_max};

// x's bits into the even places, y's into the odd.
uint64_t interleave(uint32_t x, uint32_t y) {
  static constexpr uint64_t masks[] = {0x5555555555555555ull, 0x3333333333333333ull, 0x0F0F0F0F0F0F0F0Full, 0x00FF00FF00FF00FFull, 0x0000FFFF0000FFFFull};
  static constexpr unsigned shifts[] = {1, 2, 4, 8, 16};
  uint64_t a = x;
  uint64_t b = y;
  for (int i = 4; i >= 0; --i) {
    a = (a | (a << shifts[i])) & masks[i];
    b = (b | (b << shifts[i])) & masks[i];
  }
  return a | (b << 1);
}

// The inverse: the even places in the low half, the odd in the high.
uint64_t deinterleave(uint64_t interleaved) {
  static constexpr uint64_t masks[] = {0x5555555555555555ull, 0x3333333333333333ull, 0x0F0F0F0F0F0F0F0Full,
                                       0x00FF00FF00FF00FFull, 0x0000FFFF0000FFFFull, 0x00000000FFFFFFFFull};
  static constexpr unsigned shifts[] = {0, 1, 2, 4, 8, 16};
  uint64_t x = interleaved;
  uint64_t y = interleaved >> 1;
  for (int i = 0; i < 6; ++i) {
    x = (x | (x >> shifts[i])) & masks[i];
    y = (y | (y >> shifts[i])) & masks[i];
  }
  return x | (y << 32);
}

std::optional<Bits> encode_at(Range lon, Range lat, Point p, unsigned step) {
  if (p.longitude > longitude_max || p.longitude < longitude_min || p.latitude > latitude_max || p.latitude < latitude_min) return std::nullopt;
  if (p.latitude < lat.min || p.latitude > lat.max || p.longitude < lon.min || p.longitude > lon.max) return std::nullopt;
  double lat_offset = (p.latitude - lat.min) / (lat.max - lat.min);
  double lon_offset = (p.longitude - lon.min) / (lon.max - lon.min);
  lat_offset *= static_cast<double>(1ull << step);
  lon_offset *= static_cast<double>(1ull << step);
  return Bits{interleave(static_cast<uint32_t>(lat_offset), static_cast<uint32_t>(lon_offset)), step};
}

struct Area {
  Range longitude;
  Range latitude;
};

Area decode_area(Range lon, Range lat, Bits hash) {
  const uint64_t separated = deinterleave(hash.bits);
  const auto lat_cell = static_cast<uint32_t>(separated);
  const auto lon_cell = static_cast<uint32_t>(separated >> 32);
  const double cells = static_cast<double>(1ull << hash.step);
  Area area;
  area.latitude.min = lat.min + (lat_cell * 1.0 / cells) * (lat.max - lat.min);
  area.latitude.max = lat.min + ((lat_cell + 1) * 1.0 / cells) * (lat.max - lat.min);
  area.longitude.min = lon.min + (lon_cell * 1.0 / cells) * (lon.max - lon.min);
  area.longitude.max = lon.min + ((lon_cell + 1) * 1.0 / cells) * (lon.max - lon.min);
  return area;
}

Point centre_of(const Area& area) {
  Point p{(area.longitude.min + area.longitude.max) / 2, (area.latitude.min + area.latitude.max) / 2};
  p.longitude = std::min(std::max(p.longitude, longitude_min), longitude_max);
  p.latitude = std::min(std::max(p.latitude, latitude_min), latitude_max);
  return p;
}

double latitude_distance(double lat1, double lat2) { return earth_radius * std::fabs(to_radians(lat2) - to_radians(lat1)); }

// Moves a cell one place east (+1) or west (-1): the odd bits.
void move_x(Bits& hash, int d) {
  if (d == 0) return;
  uint64_t x = hash.bits & 0xaaaaaaaaaaaaaaaaull;
  const uint64_t y = hash.bits & 0x5555555555555555ull;
  const uint64_t zz = 0x5555555555555555ull >> (64 - hash.step * 2);
  if (d > 0) {
    x = x + (zz + 1);
  } else {
    x = x | zz;
    x = x - (zz + 1);
  }
  x &= (0xaaaaaaaaaaaaaaaaull >> (64 - hash.step * 2));
  hash.bits = x | y;
}

// One place north (+1) or south (-1): the even bits.
void move_y(Bits& hash, int d) {
  if (d == 0) return;
  const uint64_t x = hash.bits & 0xaaaaaaaaaaaaaaaaull;
  uint64_t y = hash.bits & 0x5555555555555555ull;
  const uint64_t zz = 0xaaaaaaaaaaaaaaaaull >> (64 - hash.step * 2);
  if (d > 0) {
    y = y + (zz + 1);
  } else {
    y = y | zz;
    y = y - (zz + 1);
  }
  y &= (0x5555555555555555ull >> (64 - hash.step * 2));
  hash.bits = x | y;
}

Bits moved(Bits hash, int dx, int dy) {
  move_x(hash, dx);
  move_y(hash, dy);
  return hash;
}

unsigned estimate_step(double range, double latitude) {
  if (range == 0) return full_step;
  int step = 1;
  while (range < mercator_max) {
    range *= 2;
    ++step;
  }
  step -= 2;
  if (latitude > 66 || latitude < -66) {
    --step;
    if (latitude > 80 || latitude < -80) --step;
  }
  if (step < 1) step = 1;
  if (step > 26) step = 26;
  return static_cast<unsigned>(step);
}

}  // namespace

std::optional<uint64_t> encode(Point p) {
  auto bits = encode_at(valkey_longitude, valkey_latitude, p, full_step);
  if (!bits) return std::nullopt;
  return bits->bits;
}

Point decode(uint64_t score) { return centre_of(decode_area(valkey_longitude, valkey_latitude, Bits{score, full_step})); }

std::string geohash_text(uint64_t score) {
  static constexpr char alphabet[] = "0123456789bcdefghjkmnpqrstuvwxyz";
  const Point p = decode(score);
  const auto bits = encode_at(Range{-180, 180}, Range{-90, 90}, p, full_step).value_or(Bits{});
  std::string out(11, '0');
  for (int i = 0; i < 10; ++i) out[i] = alphabet[(bits.bits >> (52 - ((i + 1) * 5))) & 0x1f];
  return out;
}

double distance(Point a, Point b) {
  const double lon1 = to_radians(a.longitude);
  const double lon2 = to_radians(b.longitude);
  const double v = std::sin((lon2 - lon1) / 2);
  if (v == 0.0) return latitude_distance(a.latitude, b.latitude);
  const double lat1 = to_radians(a.latitude);
  const double lat2 = to_radians(b.latitude);
  const double u = std::sin((lat2 - lat1) / 2);
  const double h = u * u + std::cos(lat1) * std::cos(lat2) * v * v;
  return 2.0 * earth_radius * std::asin(std::sqrt(h));
}

std::optional<double> within(const Shape& shape, Point p) {
  if (!shape.box) {
    const double d = distance(shape.centre, p);
    if (d > shape.radius) return std::nullopt;
    return d;
  }
  if (latitude_distance(p.latitude, shape.centre.latitude) > shape.height / 2) return std::nullopt;
  if (distance(Point{p.longitude, p.latitude}, Point{shape.centre.longitude, p.latitude}) > shape.width / 2) return std::nullopt;
  return distance(shape.centre, p);
}

std::vector<std::pair<uint64_t, uint64_t>> search_cells(const Shape& shape) {
  // The bounding box: half the height north and south, half the width east
  // and west, wider towards the pole.
  const double half_height = shape.box ? shape.height / 2 : shape.radius;
  const double half_width = shape.box ? shape.width / 2 : shape.radius;
  const double lat = shape.centre.latitude;
  const double lon = shape.centre.longitude;
  const double lat_delta = to_degrees(half_height / earth_radius);
  const double lon_delta_top = to_degrees(half_width / earth_radius / std::cos(to_radians(lat + lat_delta)));
  const double lon_delta_bottom = to_degrees(half_width / earth_radius / std::cos(to_radians(lat - lat_delta)));
  const bool southern = lat < 0;
  const double min_lon = southern ? lon - lon_delta_bottom : lon - lon_delta_top;
  const double max_lon = southern ? lon + lon_delta_bottom : lon + lon_delta_top;
  const double min_lat = lat - lat_delta;
  const double max_lat = lat + lat_delta;
  const double radius = shape.box ? std::sqrt((shape.width / 2) * (shape.width / 2) + (shape.height / 2) * (shape.height / 2)) : shape.radius;

  unsigned step = estimate_step(radius, lat);
  Bits hash = encode_at(valkey_longitude, valkey_latitude, shape.centre, step).value_or(Bits{});
  auto neighbours = [&](const Bits& h) {
    return std::array<Bits, 8>{moved(h, 0, 1), moved(h, 0, -1), moved(h, 1, 0),  moved(h, -1, 0),
                               moved(h, 1, 1), moved(h, -1, 1), moved(h, 1, -1), moved(h, -1, -1)};
  };
  // north, south, east, west, north-east, north-west, south-east, south-west
  std::array<Bits, 8> around = neighbours(hash);
  Area area = decode_area(valkey_longitude, valkey_latitude, hash);
  // A step too coarse at the edge of what is covered is made one finer.
  const Area north = decode_area(valkey_longitude, valkey_latitude, around[0]);
  const Area south = decode_area(valkey_longitude, valkey_latitude, around[1]);
  const Area east = decode_area(valkey_longitude, valkey_latitude, around[2]);
  const Area west = decode_area(valkey_longitude, valkey_latitude, around[3]);
  const bool finer = north.latitude.max < max_lat || south.latitude.min > min_lat || east.longitude.max < max_lon || west.longitude.min > min_lon;
  if (step > 1 && finer) {
    --step;
    hash = encode_at(valkey_longitude, valkey_latitude, shape.centre, step).value_or(Bits{});
    around = neighbours(hash);
    area = decode_area(valkey_longitude, valkey_latitude, hash);
  }
  // Cells that cannot hold anything in range are left out.
  if (step >= 2) {
    if (area.latitude.min < min_lat) around[1] = around[6] = around[7] = Bits{};
    if (area.latitude.max > max_lat) around[0] = around[4] = around[5] = Bits{};
    if (area.longitude.min < min_lon) around[3] = around[7] = around[5] = Bits{};
    if (area.longitude.max > max_lon) around[2] = around[6] = around[4] = Bits{};
  }
  std::array<Bits, 9> cells{hash, around[0], around[1], around[2], around[3], around[4], around[5], around[6], around[7]};
  std::vector<std::pair<uint64_t, uint64_t>> out;
  size_t last = 0;
  for (size_t i = 0; i < cells.size(); ++i) {
    if (cells[i].zero()) continue;
    if (last && cells[i].bits == cells[last].bits && cells[i].step == cells[last].step) continue;
    const unsigned shift = 52 - cells[i].step * 2;
    out.emplace_back(cells[i].bits << shift, (cells[i].bits + 1) << shift);
    last = i;
  }
  return out;
}

}  // namespace mimir::keyspace::geo
