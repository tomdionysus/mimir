//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/keyspace/rdb.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mimir/resp/reply.h"

namespace mimir::keyspace::rdb {

namespace {

// The RDB value types (Valkey's rdb.h numbers).
enum : uint8_t {
  type_string = 0,
  type_list = 1,
  type_set = 2,
  type_zset = 3,
  type_hash = 4,
  type_zset_2 = 5,
  type_hash_zipmap = 9,
  type_list_ziplist = 10,
  type_set_intset = 11,
  type_zset_ziplist = 12,
  type_hash_ziplist = 13,
  type_list_quicklist = 14,
  type_stream_listpacks = 15,
  type_hash_listpack = 16,
  type_zset_listpack = 17,
  type_list_quicklist_2 = 18,
  type_stream_listpacks_2 = 19,
  type_set_listpack = 20,
  type_stream_listpacks_3 = 21,
  opcode_function2 = 245,
};

// Encodings of a length's first byte.
enum : uint8_t { enc_int8 = 0, enc_int16 = 1, enc_int32 = 2, enc_lzf = 3 };

constexpr size_t max_string = 512u * 1024u * 1024u;
constexpr size_t list_node_bytes = 8192;

// Anything that does not parse: caught where a payload is read.
struct Malformed {};

// ---------------------------------------------------------------- writing

void put_le(std::string& out, uint64_t value, size_t bytes) {
  for (size_t i = 0; i < bytes; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

void put_be(std::string& out, uint64_t value, size_t bytes) {
  for (size_t i = bytes; i-- > 0;) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

void put_length(std::string& out, uint64_t length) {
  if (length < (1u << 6)) {
    out.push_back(static_cast<char>(length));
  } else if (length < (1u << 14)) {
    out.push_back(static_cast<char>(0x40 | (length >> 8)));
    out.push_back(static_cast<char>(length & 0xff));
  } else if (length <= UINT32_MAX) {
    out.push_back(static_cast<char>(0x80));
    put_be(out, length, 4);
  } else {
    out.push_back(static_cast<char>(0x81));
    put_be(out, length, 8);
  }
}

// A decimal integer exactly as it would print: no sign but '-', no leading
// zero, in range. Valkey's string2ll.
std::optional<int64_t> canonical_integer(std::string_view text) {
  if (text.empty() || text.size() > 20) return std::nullopt;
  if (text == "0") return 0;
  size_t i = 0;
  bool negative = false;
  if (text[0] == '-') {
    negative = true;
    i = 1;
    if (text.size() == 1) return std::nullopt;
  }
  if (text[i] < '1' || text[i] > '9') return std::nullopt;
  uint64_t value = 0;
  for (; i < text.size(); ++i) {
    if (text[i] < '0' || text[i] > '9') return std::nullopt;
    const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
    if (value > (UINT64_MAX - digit) / 10) return std::nullopt;
    value = value * 10 + digit;
  }
  if (negative) {
    if (value > static_cast<uint64_t>(INT64_MAX) + 1) return std::nullopt;
    return static_cast<int64_t>(0 - value);
  }
  if (value > static_cast<uint64_t>(INT64_MAX)) return std::nullopt;
  return static_cast<int64_t>(value);
}

// A string as Valkey's rdbSaveRawString writes it: a small integer as an
// integer, a long compressible string in LZF, anything else as its bytes.
void put_string(std::string& out, std::string_view s) {
  if (s.size() <= 11) {
    if (auto v = canonical_integer(s)) {
      if (*v >= INT8_MIN && *v <= INT8_MAX) {
        out.push_back(static_cast<char>(0xC0 | enc_int8));
        put_le(out, static_cast<uint64_t>(*v), 1);
        return;
      }
      if (*v >= INT16_MIN && *v <= INT16_MAX) {
        out.push_back(static_cast<char>(0xC0 | enc_int16));
        put_le(out, static_cast<uint64_t>(*v), 2);
        return;
      }
      if (*v >= INT32_MIN && *v <= INT32_MAX) {
        out.push_back(static_cast<char>(0xC0 | enc_int32));
        put_le(out, static_cast<uint64_t>(*v), 4);
        return;
      }
    }
  }
  if (s.size() > 20) {
    if (auto compressed = lzf_compress(s, s.size() - 4)) {
      out.push_back(static_cast<char>(0xC0 | enc_lzf));
      put_length(out, compressed->size());
      put_length(out, s.size());
      out += *compressed;
      return;
    }
  }
  put_length(out, s.size());
  out.append(s);
}

// A listpack, built an element at a time.
class Listpack {
 public:
  void add(std::string_view s) {
    if (auto v = canonical_integer(s))
      add_integer(*v);
    else
      add_bytes(s);
  }
  void add_integer(int64_t v) {
    std::string e;
    if (v >= 0 && v <= 127) {
      e.push_back(static_cast<char>(v));
    } else if (v >= -4096 && v <= 4095) {
      const uint64_t u = v < 0 ? static_cast<uint64_t>((1 << 13) + v) : static_cast<uint64_t>(v);
      e.push_back(static_cast<char>(0xC0 | (u >> 8)));
      e.push_back(static_cast<char>(u & 0xff));
    } else if (v >= INT16_MIN && v <= INT16_MAX) {
      e.push_back(static_cast<char>(0xF1));
      put_le(e, static_cast<uint64_t>(v), 2);
    } else if (v >= -8388608 && v <= 8388607) {
      e.push_back(static_cast<char>(0xF2));
      put_le(e, static_cast<uint64_t>(v), 3);
    } else if (v >= INT32_MIN && v <= INT32_MAX) {
      e.push_back(static_cast<char>(0xF3));
      put_le(e, static_cast<uint64_t>(v), 4);
    } else {
      e.push_back(static_cast<char>(0xF4));
      put_le(e, static_cast<uint64_t>(v), 8);
    }
    finish_entry(e);
  }
  void add_bytes(std::string_view s) {
    std::string e;
    if (s.size() < 64) {
      e.push_back(static_cast<char>(0x80 | s.size()));
    } else if (s.size() < 4096) {
      e.push_back(static_cast<char>(0xE0 | (s.size() >> 8)));
      e.push_back(static_cast<char>(s.size() & 0xff));
    } else {
      e.push_back(static_cast<char>(0xF0));
      put_le(e, s.size(), 4);
    }
    e.append(s);
    finish_entry(e);
  }
  // The bytes the listpack takes with `s` added, before adding it.
  size_t size_with(std::string_view s) const {
    Listpack probe;
    probe.add(s);
    return bytes() + probe._body.size();
  }
  size_t bytes() const noexcept { return 6 + _body.size() + 1; }
  size_t count() const noexcept { return _count; }
  std::string take() const {
    std::string out;
    put_le(out, bytes(), 4);
    put_le(out, _count < 65535 ? _count : 65535, 2);
    out += _body;
    out.push_back(static_cast<char>(0xFF));
    return out;
  }

 private:
  void finish_entry(const std::string& e) {
    _body += e;
    const uint64_t l = e.size();
    if (l <= 127) {
      _body.push_back(static_cast<char>(l));
    } else if (l < 16383) {
      _body.push_back(static_cast<char>(l >> 7));
      _body.push_back(static_cast<char>((l & 127) | 128));
    } else if (l < 2097151) {
      _body.push_back(static_cast<char>(l >> 14));
      _body.push_back(static_cast<char>(((l >> 7) & 127) | 128));
      _body.push_back(static_cast<char>((l & 127) | 128));
    } else if (l < 268435455) {
      _body.push_back(static_cast<char>(l >> 21));
      _body.push_back(static_cast<char>(((l >> 14) & 127) | 128));
      _body.push_back(static_cast<char>(((l >> 7) & 127) | 128));
      _body.push_back(static_cast<char>((l & 127) | 128));
    } else {
      _body.push_back(static_cast<char>(l >> 28));
      _body.push_back(static_cast<char>(((l >> 21) & 127) | 128));
      _body.push_back(static_cast<char>(((l >> 14) & 127) | 128));
      _body.push_back(static_cast<char>(((l >> 7) & 127) | 128));
      _body.push_back(static_cast<char>((l & 127) | 128));
    }
    ++_count;
  }

  std::string _body;
  size_t _count = 0;
};

bool fits_listpack(size_t count, size_t longest) { return count <= 128 && longest <= 64; }

void put_id_raw(std::string& out, const StreamId& id) {
  put_be(out, id.ms, 8);
  put_be(out, id.seq, 8);
}

void dump_list(std::string& out, const List& list) {
  // Nodes of at most 8 KB of listpack, as Valkey's quicklist holds a list
  // built by pushes.
  std::vector<std::string> nodes;
  Listpack node;
  for (const auto& element : list) {
    if (node.count() > 0 && node.size_with(element) > list_node_bytes) {
      nodes.push_back(node.take());
      node = Listpack();
    }
    node.add(element);
  }
  if (node.count() > 0) nodes.push_back(node.take());
  out.push_back(static_cast<char>(type_list_quicklist_2));
  put_length(out, nodes.size());
  for (const auto& bytes : nodes) {
    put_length(out, 2);  // a packed node
    put_string(out, bytes);
  }
}

void dump_hash(std::string& out, const Hash& hash) {
  size_t longest = 0;
  for (const auto& [f, v] : hash) longest = std::max({longest, f.size(), v.size()});
  if (fits_listpack(hash.size(), longest)) {
    Listpack lp;
    for (const auto& [f, v] : hash) {
      lp.add(f);
      lp.add(v);
    }
    out.push_back(static_cast<char>(type_hash_listpack));
    put_string(out, lp.take());
    return;
  }
  out.push_back(static_cast<char>(type_hash));
  put_length(out, hash.size());
  for (const auto& [f, v] : hash) {
    put_string(out, f);
    put_string(out, v);
  }
}

void dump_set(std::string& out, const Set& set) {
  const auto members = set.ordered();
  switch (set.shape()) {
    case Set::Shape::integers: {
      std::vector<int64_t> values;
      size_t width = 2;
      for (auto m : members) {
        const int64_t v = *canonical_integer(m);
        values.push_back(v);
        if (v < INT16_MIN || v > INT16_MAX) width = std::max<size_t>(width, 4);
        if (v < INT32_MIN || v > INT32_MAX) width = 8;
      }
      std::string intset;
      put_le(intset, width, 4);
      put_le(intset, values.size(), 4);
      for (int64_t v : values) put_le(intset, static_cast<uint64_t>(v), width);
      out.push_back(static_cast<char>(type_set_intset));
      put_string(out, intset);
      return;
    }
    case Set::Shape::small: {
      Listpack lp;
      for (auto m : members) lp.add(m);
      out.push_back(static_cast<char>(type_set_listpack));
      put_string(out, lp.take());
      return;
    }
    case Set::Shape::large:
      out.push_back(static_cast<char>(type_set));
      put_length(out, members.size());
      for (auto m : members) put_string(out, m);
      return;
  }
}

void dump_zset(std::string& out, const SortedSet& zset) {
  size_t longest = 0;
  for (size_t r = 0; r < zset.size(); ++r) longest = std::max(longest, zset.at(r).member.size());
  if (fits_listpack(zset.size(), longest)) {
    Listpack lp;
    for (size_t r = 0; r < zset.size(); ++r) {
      lp.add(zset.at(r).member);
      lp.add(resp::format_double(zset.at(r).score));
    }
    out.push_back(static_cast<char>(type_zset_listpack));
    put_string(out, lp.take());
    return;
  }
  // The skiplist from its tail, as Valkey writes it.
  out.push_back(static_cast<char>(type_zset_2));
  put_length(out, zset.size());
  for (size_t r = zset.size(); r-- > 0;) {
    const auto& item = zset.at(r);
    put_string(out, item.member);
    uint64_t bits = 0;
    std::memcpy(&bits, &item.score, sizeof(bits));
    put_le(out, bits, 8);
  }
}

void dump_stream(std::string& out, const Stream& stream) {
  // Each node of live entries as one listpack under its first entry's id.
  std::vector<std::pair<StreamId, std::string>> nodes;
  auto it = stream.entries.begin();
  while (it != stream.entries.end()) {
    const uint64_t node = stream.node_of.count(it->first) ? stream.node_of.at(it->first) : 0;
    const StreamId master = it->first;
    const std::vector<std::string>& master_values = it->second;
    std::vector<std::pair<StreamId, const std::vector<std::string>*>> in_node;
    while (it != stream.entries.end() && (stream.node_of.count(it->first) ? stream.node_of.at(it->first) : 0) == node) {
      in_node.emplace_back(it->first, &it->second);
      ++it;
    }
    Listpack lp;
    lp.add_integer(static_cast<int64_t>(in_node.size()));
    lp.add_integer(0);
    const size_t master_fields = master_values.size() / 2;
    lp.add_integer(static_cast<int64_t>(master_fields));
    for (size_t f = 0; f < master_fields; ++f) lp.add(master_values[2 * f]);
    lp.add_integer(0);
    for (const auto& [id, fields] : in_node) {
      bool same = fields->size() == master_values.size();
      for (size_t f = 0; same && f < master_fields; ++f) same = (*fields)[2 * f] == master_values[2 * f];
      lp.add_integer(same ? 2 : 0);
      lp.add_integer(static_cast<int64_t>(id.ms - master.ms));
      lp.add_integer(static_cast<int64_t>(id.seq - master.seq));
      const size_t count = fields->size() / 2;
      if (same) {
        for (size_t f = 0; f < count; ++f) lp.add((*fields)[2 * f + 1]);
        lp.add_integer(static_cast<int64_t>(count + 3));
      } else {
        lp.add_integer(static_cast<int64_t>(count));
        for (const auto& value : *fields) lp.add(value);
        lp.add_integer(static_cast<int64_t>(2 * count + 4));
      }
    }
    nodes.emplace_back(master, lp.take());
  }
  out.push_back(static_cast<char>(type_stream_listpacks_3));
  put_length(out, nodes.size());
  for (const auto& [master, bytes] : nodes) {
    std::string key;
    put_id_raw(key, master);
    put_string(out, key);
    put_string(out, bytes);
  }
  put_length(out, stream.entries.size());
  put_length(out, stream.last_id.ms);
  put_length(out, stream.last_id.seq);
  const StreamId first = stream.first_id();
  put_length(out, first.ms);
  put_length(out, first.seq);
  put_length(out, stream.max_deleted.ms);
  put_length(out, stream.max_deleted.seq);
  put_length(out, stream.entries_added);
  put_length(out, stream.groups.size());
  for (const auto& [name, group] : stream.groups) {
    put_string(out, name);
    put_length(out, group.last_delivered.ms);
    put_length(out, group.last_delivered.seq);
    put_length(out, group.entries_read.value_or(UINT64_MAX));
    put_length(out, group.pending.size());
    for (const auto& [id, pending] : group.pending) {
      put_id_raw(out, id);
      put_le(out, pending.delivered_ms, 8);
      put_length(out, pending.deliveries);
    }
    put_length(out, group.consumers.size());
    for (const auto& [consumer_name, consumer] : group.consumers) {
      put_string(out, consumer_name);
      put_le(out, consumer.seen_ms, 8);
      put_le(out, consumer.active_ms, 8);
      put_length(out, consumer.pending.size());
      for (const auto& id : consumer.pending) put_id_raw(out, id);
    }
  }
}

void seal(std::string& out) {
  put_le(out, version, 2);
  put_le(out, crc64(0, out), 8);
}

// ---------------------------------------------------------------- reading

class Reader {
 public:
  explicit Reader(std::string_view in) : _in(in) {}

  bool done() const noexcept { return _at == _in.size(); }
  size_t left() const noexcept { return _in.size() - _at; }

  uint8_t byte() {
    if (_at >= _in.size()) throw Malformed{};
    return static_cast<uint8_t>(_in[_at++]);
  }
  std::string_view raw(size_t n) {
    if (n > left()) throw Malformed{};
    std::string_view out = _in.substr(_at, n);
    _at += n;
    return out;
  }
  uint64_t le(size_t n) {
    const std::string_view bytes = raw(n);
    uint64_t v = 0;
    for (size_t i = n; i-- > 0;) v = (v << 8) | static_cast<uint8_t>(bytes[i]);
    return v;
  }
  uint64_t be(size_t n) {
    const std::string_view bytes = raw(n);
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v = (v << 8) | static_cast<uint8_t>(bytes[i]);
    return v;
  }

  // A length, or (with `encoded`) one of the special string encodings.
  uint64_t length(bool* encoded = nullptr) {
    if (encoded) *encoded = false;
    const uint8_t first = byte();
    switch (first >> 6) {
      case 0:
        return first & 0x3f;
      case 1:
        return (static_cast<uint64_t>(first & 0x3f) << 8) | byte();
      case 2:
        if (first == 0x80) return be(4);
        if (first == 0x81) return be(8);
        throw Malformed{};
      default:
        if (!encoded) throw Malformed{};
        *encoded = true;
        return first & 0x3f;
    }
  }
  // A count of things each at least one byte long.
  uint64_t count() {
    const uint64_t n = length();
    if (n > left()) throw Malformed{};
    return n;
  }

  std::string string() {
    bool encoded = false;
    const uint64_t n = length(&encoded);
    if (!encoded) {
      if (n > max_string) throw Malformed{};
      return std::string(raw(static_cast<size_t>(n)));
    }
    switch (n) {
      case enc_int8:
        return std::to_string(static_cast<int8_t>(byte()));
      case enc_int16:
        return std::to_string(static_cast<int16_t>(le(2)));
      case enc_int32:
        return std::to_string(static_cast<int32_t>(le(4)));
      case enc_lzf: {
        const uint64_t compressed = length();
        const uint64_t expanded = length();
        if (compressed > left() || expanded > max_string || expanded == 0) throw Malformed{};
        // LZF expands at most a few hundred times; a claim beyond is not LZF.
        if (expanded > compressed * 300 + 64) throw Malformed{};
        auto out = lzf_decompress(raw(static_cast<size_t>(compressed)), static_cast<size_t>(expanded));
        if (!out) throw Malformed{};
        return std::move(*out);
      }
      default:
        throw Malformed{};
    }
  }

  StreamId id_raw() {
    StreamId id;
    id.ms = be(8);
    id.seq = be(8);
    return id;
  }

 private:
  std::string_view _in;
  size_t _at = 0;
};

// A listpack's elements, as strings (integers in decimal).
std::vector<std::string> read_listpack(std::string_view bytes) {
  Reader r(bytes);
  const uint64_t total = r.le(4);
  if (total != bytes.size() || total < 7) throw Malformed{};
  const uint64_t claimed = r.le(2);
  std::vector<std::string> out;
  for (;;) {
    const uint8_t first = r.byte();
    if (first == 0xFF) break;
    size_t encoded = 1;
    if ((first & 0x80) == 0) {
      out.push_back(std::to_string(first & 0x7f));
    } else if ((first & 0xC0) == 0x80) {
      const size_t n = first & 0x3f;
      out.emplace_back(r.raw(n));
      encoded += n;
    } else if ((first & 0xE0) == 0xC0) {
      uint64_t u = (static_cast<uint64_t>(first & 0x1f) << 8) | r.byte();
      const int64_t v = u >= (1u << 12) ? static_cast<int64_t>(u) - (1 << 13) : static_cast<int64_t>(u);
      out.push_back(std::to_string(v));
      encoded = 2;
    } else if ((first & 0xF0) == 0xE0) {
      const size_t n = (static_cast<size_t>(first & 0x0f) << 8) | r.byte();
      out.emplace_back(r.raw(n));
      encoded = 2 + n;
    } else {
      switch (first) {
        case 0xF0: {
          const size_t n = static_cast<size_t>(r.le(4));
          out.emplace_back(r.raw(n));
          encoded = 5 + n;
          break;
        }
        case 0xF1:
          out.push_back(std::to_string(static_cast<int16_t>(r.le(2))));
          encoded = 3;
          break;
        case 0xF2: {
          uint64_t u = r.le(3);
          const int64_t v = (u & 0x800000) ? static_cast<int64_t>(u) - (1 << 24) : static_cast<int64_t>(u);
          out.push_back(std::to_string(v));
          encoded = 4;
          break;
        }
        case 0xF3:
          out.push_back(std::to_string(static_cast<int32_t>(r.le(4))));
          encoded = 5;
          break;
        case 0xF4:
          out.push_back(std::to_string(static_cast<int64_t>(r.le(8))));
          encoded = 9;
          break;
        default:
          throw Malformed{};
      }
    }
    // The back-length: as many bytes as the entry's size takes.
    const size_t back = encoded <= 127 ? 1 : encoded < 16383 ? 2 : encoded < 2097151 ? 3 : encoded < 268435455 ? 4 : 5;
    r.raw(back);
  }
  if (!r.done()) throw Malformed{};
  if (claimed != 65535 && claimed != out.size()) throw Malformed{};
  return out;
}

// A ziplist's elements (the encoding before listpack).
std::vector<std::string> read_ziplist(std::string_view bytes) {
  Reader r(bytes);
  const uint64_t total = r.le(4);
  if (total != bytes.size() || total < 11) throw Malformed{};
  r.le(4);  // the tail's offset
  const uint64_t claimed = r.le(2);
  std::vector<std::string> out;
  for (;;) {
    uint8_t first = r.byte();
    if (first == 0xFF) break;
    if (first == 0xFE) r.raw(4);  // a five-byte previous length
    const uint8_t enc = r.byte();
    switch (enc >> 6) {
      case 0:
        out.emplace_back(r.raw(enc & 0x3f));
        continue;
      case 1:
        out.emplace_back(r.raw((static_cast<size_t>(enc & 0x3f) << 8) | r.byte()));
        continue;
      case 2:
        if (enc != 0x80) throw Malformed{};
        out.emplace_back(r.raw(static_cast<size_t>(r.be(4))));
        continue;
      default:
        break;
    }
    switch (enc) {
      case 0xC0:
        out.push_back(std::to_string(static_cast<int16_t>(r.le(2))));
        break;
      case 0xD0:
        out.push_back(std::to_string(static_cast<int32_t>(r.le(4))));
        break;
      case 0xE0:
        out.push_back(std::to_string(static_cast<int64_t>(r.le(8))));
        break;
      case 0xF0: {
        const uint64_t u = r.le(3);
        out.push_back(std::to_string((u & 0x800000) ? static_cast<int64_t>(u) - (1 << 24) : static_cast<int64_t>(u)));
        break;
      }
      case 0xFE:
        out.push_back(std::to_string(static_cast<int8_t>(r.byte())));
        break;
      default:
        if (enc >= 0xF1 && enc <= 0xFD) {
          out.push_back(std::to_string((enc & 0x0f) - 1));
          break;
        }
        throw Malformed{};
    }
  }
  if (!r.done()) throw Malformed{};
  if (claimed != 65535 && claimed != out.size()) throw Malformed{};
  return out;
}

std::vector<int64_t> read_intset(std::string_view bytes) {
  Reader r(bytes);
  const uint64_t width = r.le(4);
  if (width != 2 && width != 4 && width != 8) throw Malformed{};
  const uint64_t n = r.le(4);
  if (n * width != r.left()) throw Malformed{};
  std::vector<int64_t> out;
  for (uint64_t i = 0; i < n; ++i) {
    const uint64_t u = r.le(width);
    out.push_back(width == 2 ? static_cast<int16_t>(u) : width == 4 ? static_cast<int32_t>(u) : static_cast<int64_t>(u));
  }
  return out;
}

double parse_score(const std::string& text) {
  if (text.empty()) throw Malformed{};
  char* end = nullptr;
  const double score = std::strtod(text.c_str(), &end);
  if (end != text.c_str() + text.size() || std::isnan(score)) throw Malformed{};
  return score;
}

Entry read_list(Reader& r, uint8_t type) {
  List list;
  if (type == type_list) {
    for (uint64_t n = r.count(); n > 0; --n) list.push_back(r.string());
  } else if (type == type_list_ziplist) {
    for (auto& e : read_ziplist(r.string())) list.push_back(std::move(e));
  } else {
    for (uint64_t nodes = r.count(); nodes > 0; --nodes) {
      uint64_t container = 2;
      if (type == type_list_quicklist_2) container = r.length();
      const std::string bytes = r.string();
      if (container == 1) {
        list.push_back(bytes);
      } else if (container == 2 || type == type_list_quicklist) {
        auto elements = type == type_list_quicklist ? read_ziplist(bytes) : read_listpack(bytes);
        if (elements.empty()) throw Malformed{};
        for (auto& e : elements) list.push_back(std::move(e));
      } else {
        throw Malformed{};
      }
    }
  }
  if (list.empty()) throw Malformed{};
  return Entry{std::move(list), std::nullopt};
}

Entry read_set(Reader& r, uint8_t type) {
  Set set;
  if (type == type_set) {
    for (uint64_t n = r.count(); n > 0; --n) set.add(r.string());
  } else if (type == type_set_intset) {
    for (int64_t v : read_intset(r.string())) set.add(std::to_string(v));
  } else {
    for (const auto& m : read_listpack(r.string())) set.add(m);
  }
  if (set.empty()) throw Malformed{};
  return Entry{std::move(set), std::nullopt};
}

Entry read_zset(Reader& r, uint8_t type) {
  SortedSet zset;
  if (type == type_zset || type == type_zset_2) {
    for (uint64_t n = r.count(); n > 0; --n) {
      std::string member = r.string();
      double score = 0;
      if (type == type_zset_2) {
        const uint64_t bits = r.le(8);
        std::memcpy(&score, &bits, sizeof(score));
        if (std::isnan(score)) throw Malformed{};
      } else {
        const uint8_t len = r.byte();
        if (len == 253) throw Malformed{};
        if (len == 254)
          score = INFINITY;
        else if (len == 255)
          score = -INFINITY;
        else
          score = parse_score(std::string(r.raw(len)));
      }
      zset.set(member, score);
    }
  } else {
    const auto elements = type == type_zset_ziplist ? read_ziplist(r.string()) : read_listpack(r.string());
    if (elements.size() % 2 != 0) throw Malformed{};
    for (size_t i = 0; i < elements.size(); i += 2) zset.set(elements[i], parse_score(elements[i + 1]));
  }
  if (zset.empty()) throw Malformed{};
  return Entry{std::move(zset), std::nullopt};
}

Entry read_hash(Reader& r, uint8_t type) {
  Hash hash;
  if (type == type_hash) {
    for (uint64_t n = r.count(); n > 0; --n) {
      std::string field = r.string();
      if (!hash.set(field, r.string())) throw Malformed{};
    }
  } else {
    const auto elements = type == type_hash_ziplist ? read_ziplist(r.string()) : read_listpack(r.string());
    if (elements.size() % 2 != 0) throw Malformed{};
    for (size_t i = 0; i < elements.size(); i += 2) {
      if (!hash.set(elements[i], elements[i + 1])) throw Malformed{};
    }
  }
  if (hash.empty()) throw Malformed{};
  return Entry{std::move(hash), std::nullopt};
}

int64_t integer_of(const std::string& text) {
  auto v = canonical_integer(text);
  if (!v) throw Malformed{};
  return *v;
}

Entry read_stream(Reader& r, uint8_t type) {
  Stream stream;
  uint64_t node = 0;
  // The last node's entries, deleted ones included: an append fills it until
  // it holds node_entries, as Valkey's does after a RESTORE.
  uint64_t last_filled = 0;
  for (uint64_t nodes = r.count(); nodes > 0; --nodes, ++node) {
    const std::string key = r.string();
    if (key.size() != 16) throw Malformed{};
    Reader key_reader(key);
    const StreamId master = key_reader.id_raw();
    const auto lp = read_listpack(r.string());
    // The master entry: count, deleted, the master fields, then a zero.
    size_t at = 0;
    const auto next = [&]() -> const std::string& {
      if (at >= lp.size()) throw Malformed{};
      return lp[at++];
    };
    const int64_t live = integer_of(next());
    const int64_t deleted = integer_of(next());
    const int64_t master_count = integer_of(next());
    if (live < 0 || deleted < 0 || master_count < 0 || static_cast<uint64_t>(master_count) > lp.size()) throw Malformed{};
    last_filled = static_cast<uint64_t>(live) + static_cast<uint64_t>(deleted);
    std::vector<std::string> master_fields;
    for (int64_t f = 0; f < master_count; ++f) master_fields.push_back(next());
    if (integer_of(next()) != 0) throw Malformed{};
    int64_t seen = 0;
    while (at < lp.size()) {
      const int64_t flags = integer_of(next());
      const int64_t ms_diff = integer_of(next());
      const int64_t seq_diff = integer_of(next());
      StreamId id{master.ms + static_cast<uint64_t>(ms_diff), master.seq + static_cast<uint64_t>(seq_diff)};
      std::vector<std::string> fields;
      if (flags & 2) {
        for (const auto& f : master_fields) {
          fields.push_back(f);
          fields.push_back(next());
        }
      } else {
        const int64_t count = integer_of(next());
        if (count < 0 || static_cast<uint64_t>(count) > lp.size()) throw Malformed{};
        for (int64_t f = 0; f < 2 * count; ++f) fields.push_back(next());
      }
      next();                   // lp-count
      if (flags & 1) continue;  // a deleted entry
      if (stream.entries.count(id)) throw Malformed{};
      stream.entries.emplace(id, std::move(fields));
      stream.node_of.emplace(id, node);
      ++seen;
    }
    if (seen != live) throw Malformed{};
  }
  stream.appended = node == 0 ? 0 : (node - 1) * Stream::node_entries + std::min(last_filled, Stream::node_entries);
  const uint64_t length = r.length();
  if (length != stream.entries.size()) throw Malformed{};
  stream.last_id.ms = r.length();
  stream.last_id.seq = r.length();
  if (type >= type_stream_listpacks_2) {
    r.length();  // the first id: the first entry's
    r.length();
    stream.max_deleted.ms = r.length();
    stream.max_deleted.seq = r.length();
    stream.entries_added = r.length();
  } else {
    stream.entries_added = length;
  }
  if (!stream.entries.empty() && stream.last_id < stream.entries.rbegin()->first) throw Malformed{};
  for (uint64_t groups = r.count(); groups > 0; --groups) {
    std::string name = r.string();
    StreamGroup group;
    group.last_delivered.ms = r.length();
    group.last_delivered.seq = r.length();
    if (type >= type_stream_listpacks_2) {
      const uint64_t read = r.length();
      if (read != UINT64_MAX) group.entries_read = read;
    }
    for (uint64_t n = r.count(); n > 0; --n) {
      const StreamId id = r.id_raw();
      StreamPending pending;
      pending.delivered_ms = r.le(8);
      pending.deliveries = r.length();
      if (!group.pending.emplace(id, std::move(pending)).second) throw Malformed{};
    }
    for (uint64_t consumers = r.count(); consumers > 0; --consumers) {
      std::string consumer_name = r.string();
      StreamConsumer consumer;
      consumer.seen_ms = r.le(8);
      consumer.active_ms = type >= type_stream_listpacks_3 ? r.le(8) : consumer.seen_ms;
      for (uint64_t n = r.count(); n > 0; --n) {
        const StreamId id = r.id_raw();
        auto found = group.pending.find(id);
        if (found == group.pending.end() || !found->second.consumer.empty()) throw Malformed{};
        found->second.consumer = consumer_name;
        consumer.pending.insert(id);
      }
      if (!group.consumers.emplace(std::move(consumer_name), std::move(consumer)).second) throw Malformed{};
    }
    // Every pending entry belongs to a consumer.
    for (const auto& [id, pending] : group.pending) {
      if (pending.consumer.empty()) throw Malformed{};
    }
    if (!stream.groups.emplace(std::move(name), std::move(group)).second) throw Malformed{};
  }
  return Entry{std::move(stream), std::nullopt};
}

// The body of a payload whose version and checksum hold, or none.
std::optional<std::string_view> verified_body(std::string_view payload) {
  if (payload.size() < 10) return std::nullopt;
  const std::string_view body = payload.substr(0, payload.size() - 10);
  Reader footer(payload.substr(payload.size() - 10));
  const uint64_t found_version = footer.le(2);
  const uint64_t crc = footer.le(8);
  if (found_version > version) return std::nullopt;
  if (crc64(0, payload.substr(0, payload.size() - 8)) != crc) return std::nullopt;
  return body;
}

}  // namespace

uint64_t crc64(uint64_t crc, std::string_view bytes) noexcept {
  static const std::array<uint64_t, 256> table = [] {
    // Jones's polynomial, reflected.
    constexpr uint64_t reflected = 0x95AC9329AC4BC9B5ull;
    std::array<uint64_t, 256> t{};
    for (uint64_t i = 0; i < 256; ++i) {
      uint64_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ reflected : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  for (unsigned char b : bytes) crc = table[(crc ^ b) & 0xff] ^ (crc >> 8);
  return crc;
}

std::optional<std::string> lzf_compress(std::string_view in, size_t limit) {
  constexpr unsigned hash_log = 16;
  constexpr uint32_t hash_size = 1u << hash_log;
  constexpr size_t max_literal = 32;
  constexpr size_t max_offset = 8192;
  constexpr size_t max_reference = (1 << 8) + (1 << 3);
  const size_t n = in.size();
  if (n == 0 || limit == 0) return std::nullopt;
  const auto at = [&](size_t i) { return static_cast<uint32_t>(static_cast<unsigned char>(in[i])); };
  const auto slot_of = [](uint32_t h) { return ((h >> (3 * 8 - hash_log)) - h * 5) & (hash_size - 1); };
  // Where each three-byte prefix was last seen; zero is the start, which a
  // match may never reference.
  std::vector<uint32_t> seen(hash_size, 0);
  std::string out(limit, '\0');
  size_t ip = 0;
  size_t op = 1;  // a literal run's control byte is out[0]
  size_t literal = 0;
  uint32_t h = n >= 2 ? (at(0) << 8) | at(1) : 0;
  while (ip + 2 < n) {
    h = (h << 8) | at(ip + 2);
    uint32_t& slot = seen[slot_of(h)];
    const size_t ref = slot;
    slot = static_cast<uint32_t>(ip);
    if (ref > 0 && ref < ip && ip - ref - 1 < max_offset && at(ref + 2) == at(ip + 2) && at(ref) == at(ip) && at(ref + 1) == at(ip + 1)) {
      const size_t offset = ip - ref - 1;
      size_t len = 2;
      size_t max_len = n - ip - len;
      if (max_len > max_reference) max_len = max_reference;
      if (op + 3 + 1 >= limit && op - (literal == 0 ? 1 : 0) + 3 + 1 >= limit) return std::nullopt;
      out[op - literal - 1] = static_cast<char>(literal - 1);
      if (literal == 0) --op;
      bool stopped = false;
      if (max_len > 16) {
        for (int k = 0; k < 16 && !stopped; ++k) {
          ++len;
          if (at(ref + len) != at(ip + len)) stopped = true;
        }
      }
      if (!stopped) {
        do {
          ++len;
        } while (len < max_len && at(ref + len) == at(ip + len));
      }
      len -= 2;
      ++ip;
      if (len < 7) {
        out[op++] = static_cast<char>((offset >> 8) + (len << 5));
      } else {
        out[op++] = static_cast<char>((offset >> 8) + (7 << 5));
        out[op++] = static_cast<char>(len - 7);
      }
      out[op++] = static_cast<char>(offset & 0xff);
      literal = 0;
      ++op;
      ip += len + 1;
      if (ip + 2 >= n) break;
      ip -= 2;
      h = (at(ip) << 8) | at(ip + 1);
      h = (h << 8) | at(ip + 2);
      seen[slot_of(h)] = static_cast<uint32_t>(ip);
      ++ip;
      h = (h << 8) | at(ip + 2);
      seen[slot_of(h)] = static_cast<uint32_t>(ip);
      ++ip;
    } else {
      if (op >= limit) return std::nullopt;
      ++literal;
      out[op++] = in[ip++];
      if (literal == max_literal) {
        out[op - literal - 1] = static_cast<char>(literal - 1);
        literal = 0;
        ++op;
      }
    }
  }
  if (op + 3 > limit) return std::nullopt;
  while (ip < n) {
    ++literal;
    out[op++] = in[ip++];
    if (literal == max_literal) {
      out[op - literal - 1] = static_cast<char>(literal - 1);
      literal = 0;
      ++op;
    }
  }
  out[op - literal - 1] = static_cast<char>(literal - 1);
  if (literal == 0) --op;
  out.resize(op);
  return out;
}

std::optional<std::string> lzf_decompress(std::string_view in, size_t length) {
  std::string out;
  out.reserve(length);
  size_t ip = 0;
  while (ip < in.size()) {
    const unsigned ctrl = static_cast<unsigned char>(in[ip++]);
    if (ctrl < (1 << 5)) {
      const size_t run = ctrl + 1;
      if (run > in.size() - ip || out.size() + run > length) return std::nullopt;
      out.append(in.substr(ip, run));
      ip += run;
      continue;
    }
    size_t len = ctrl >> 5;
    if (len == 7) {
      if (ip >= in.size()) return std::nullopt;
      len += static_cast<unsigned char>(in[ip++]);
    }
    if (ip >= in.size()) return std::nullopt;
    const size_t back = ((ctrl & 0x1f) << 8) + static_cast<unsigned char>(in[ip++]) + 1;
    len += 2;
    if (back > out.size() || out.size() + len > length) return std::nullopt;
    // Byte by byte: a reference may overlap what it is writing.
    for (size_t i = 0; i < len; ++i) out.push_back(out[out.size() - back]);
  }
  if (out.size() != length) return std::nullopt;
  return out;
}

std::string dump(const Entry& entry) {
  std::string out;
  switch (entry.type()) {
    case Type::string:
      out.push_back(static_cast<char>(type_string));
      put_string(out, std::get<std::string>(entry.value));
      break;
    case Type::list:
      dump_list(out, std::get<List>(entry.value));
      break;
    case Type::hash:
      dump_hash(out, std::get<Hash>(entry.value));
      break;
    case Type::set:
      dump_set(out, std::get<Set>(entry.value));
      break;
    case Type::zset:
      dump_zset(out, std::get<SortedSet>(entry.value));
      break;
    case Type::stream:
      dump_stream(out, std::get<Stream>(entry.value));
      break;
  }
  seal(out);
  return out;
}

std::variant<Entry, Refusal> restore(std::string_view payload) {
  auto body = verified_body(payload);
  if (!body) return Refusal::checksum;
  try {
    Reader r(*body);
    const uint8_t type = r.byte();
    Entry entry;
    switch (type) {
      case type_string:
        entry = Entry{r.string(), std::nullopt};
        break;
      case type_list:
      case type_list_ziplist:
      case type_list_quicklist:
      case type_list_quicklist_2:
        entry = read_list(r, type);
        break;
      case type_set:
      case type_set_intset:
      case type_set_listpack:
        entry = read_set(r, type);
        break;
      case type_zset:
      case type_zset_2:
      case type_zset_ziplist:
      case type_zset_listpack:
        entry = read_zset(r, type);
        break;
      case type_hash:
      case type_hash_ziplist:
      case type_hash_listpack:
        entry = read_hash(r, type);
        break;
      case type_stream_listpacks:
      case type_stream_listpacks_2:
      case type_stream_listpacks_3:
        entry = read_stream(r, type);
        break;
      default:
        return Refusal::format;
    }
    if (!r.done()) return Refusal::format;
    return entry;
  } catch (const Malformed&) {
    return Refusal::format;
  }
}

std::string dump_functions(const std::vector<std::string>& codes) {
  std::string out;
  for (const auto& code : codes) {
    out.push_back(static_cast<char>(opcode_function2));
    put_string(out, code);
  }
  seal(out);
  return out;
}

std::variant<std::vector<std::string>, Refusal> restore_functions(std::string_view payload) {
  auto body = verified_body(payload);
  if (!body) return Refusal::checksum;
  try {
    Reader r(*body);
    std::vector<std::string> codes;
    while (!r.done()) {
      if (r.byte() != opcode_function2) return Refusal::format;
      codes.push_back(r.string());
    }
    return codes;
  } catch (const Malformed&) {
    return Refusal::format;
  }
}

}  // namespace mimir::keyspace::rdb
