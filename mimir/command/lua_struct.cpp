//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// struct.pack, struct.unpack and struct.size, as Valkey's scripts see them.
// A format is a sequence of options: '>' and '<' set the byte order, '!n' the
// largest alignment (a power of 2; '!' alone the platform's), ' ' is ignored;
// 'b' 'B' 'h' 'H' 'l' 'L' 'T' and 'i[n]' 'I[n]' are integers (lower case
// signed), 'f' and 'd' floating point, 'x' a zero byte, 's' a zero-terminated
// string and 'c[n]' a fixed one ('c0': as long as the string when packing,
// the previous number's length when unpacking).
//
// Every error here is raised by Lua (longjmp), so no function holds an object
// with a destructor.

#include "mimir/command/lua_struct.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace mimir::command {

namespace {

constexpr int max_int_size = 32;
struct AlignProbe {
  char c;
  double d;
};
constexpr int max_align = offsetof(AlignProbe, d);

enum Endian : int { big = 0, little = 1 };

int native_endian() {
  const uint16_t one = 1;
  unsigned char first = 0;
  std::memcpy(&first, &one, 1);
  return first == 1 ? little : big;
}

struct Header {
  int endian;
  int align;
};

void default_options(Header& h) {
  h.endian = native_endian();
  h.align = 1;
}

bool is_power_of_2(int x) { return x > 0 && (x & (x - 1)) == 0; }

int get_number(const char** fmt, int otherwise) {
  if (!std::isdigit(static_cast<unsigned char>(**fmt))) return otherwise;
  int a = 0;
  do {
    a = a * 10 + (*((*fmt)++) - '0');
  } while (std::isdigit(static_cast<unsigned char>(**fmt)) && a <= (INT32_MAX - 9) / 10);
  return a;
}

size_t option_size(lua_State* L, char opt, const char** fmt) {
  switch (opt) {
    case 'B':
    case 'b':
      return sizeof(char);
    case 'H':
    case 'h':
      return sizeof(short);
    case 'L':
    case 'l':
      return sizeof(long);
    case 'T':
      return sizeof(size_t);
    case 'f':
      return sizeof(float);
    case 'd':
      return sizeof(double);
    case 'x':
      return 1;
    case 'c':
      return static_cast<size_t>(get_number(fmt, 1));
    case 'i':
    case 'I': {
      const int size = get_number(fmt, sizeof(int));
      if (size > max_int_size) luaL_error(L, "integral size %d is larger than limit of %d", size, max_int_size);
      return static_cast<size_t>(size);
    }
    default:
      return 0;
  }
}

size_t to_align(size_t length, const Header& h, char opt, size_t size) {
  if (size == 0 || opt == 'c') return 0;
  if (size > static_cast<size_t>(h.align)) size = static_cast<size_t>(h.align);
  return (size - (length & (size - 1))) & (size - 1);
}

void control_options(lua_State* L, char opt, const char** fmt, Header& h) {
  switch (opt) {
    case ' ':
      return;
    case '>':
      h.endian = big;
      return;
    case '<':
      h.endian = little;
      return;
    case '!': {
      const int a = get_number(fmt, max_align);
      if (!is_power_of_2(a)) luaL_error(L, "alignment %d is not a power of 2", a);
      h.align = a;
      return;
    }
    default: {
      const char* message = lua_pushfstring(L, "invalid format option '%c'", opt);
      luaL_argerror(L, 1, message);
    }
  }
}

void put_integer(lua_State* L, luaL_Buffer* b, int arg, int endian, size_t size) {
  const lua_Number n = luaL_checknumber(L, arg);
  uint64_t value = n < 0 ? static_cast<uint64_t>(static_cast<int64_t>(n)) : static_cast<uint64_t>(n);
  char buffer[max_int_size];
  if (endian == little) {
    for (size_t i = 0; i < size; ++i) {
      buffer[i] = static_cast<char>(value & 0xff);
      value >>= 8;
    }
  } else {
    for (size_t i = size; i-- > 0;) {
      buffer[i] = static_cast<char>(value & 0xff);
      value >>= 8;
    }
  }
  luaL_addlstring(b, buffer, size);
}

void correct_bytes(char* bytes, size_t size, int endian) {
  if (endian == native_endian() || size == 0) return;
  size_t i = 0;
  size_t j = size - 1;
  while (i < j) {
    const char held = bytes[i];
    bytes[i++] = bytes[j];
    bytes[j--] = held;
  }
}

int pack(lua_State* L) {
  luaL_Buffer b;
  const char* fmt = luaL_checkstring(L, 1);
  Header h;
  default_options(h);
  int arg = 2;
  size_t total = 0;
  lua_pushnil(L);  // separates the arguments from the buffer
  luaL_buffinit(L, &b);
  while (*fmt != '\0') {
    const char opt = *fmt++;
    size_t size = option_size(L, opt, &fmt);
    size_t align = to_align(total, h, opt, size);
    total += align;
    while (align-- > 0) luaL_addchar(&b, '\0');
    switch (opt) {
      case 'b':
      case 'B':
      case 'h':
      case 'H':
      case 'l':
      case 'L':
      case 'T':
      case 'i':
      case 'I':
        put_integer(L, &b, arg++, h.endian, size);
        break;
      case 'x':
        luaL_addchar(&b, '\0');
        break;
      case 'f': {
        float f = static_cast<float>(luaL_checknumber(L, arg++));
        char bytes[sizeof(float)];
        std::memcpy(bytes, &f, sizeof(f));
        correct_bytes(bytes, size, h.endian);
        luaL_addlstring(&b, bytes, size);
        break;
      }
      case 'd': {
        double d = luaL_checknumber(L, arg++);
        char bytes[sizeof(double)];
        std::memcpy(bytes, &d, sizeof(d));
        correct_bytes(bytes, size, h.endian);
        luaL_addlstring(&b, bytes, size);
        break;
      }
      case 'c':
      case 's': {
        size_t length = 0;
        const char* s = luaL_checklstring(L, arg++, &length);
        if (size == 0) size = length;
        luaL_argcheck(L, length >= size, arg, "string too short");
        luaL_addlstring(&b, s, size);
        if (opt == 's') {
          luaL_addchar(&b, '\0');
          ++size;
        }
        break;
      }
      default:
        control_options(L, opt, &fmt, h);
    }
    total += size;
  }
  luaL_pushresult(&b);
  return 1;
}

lua_Number get_integer(const char* bytes, int endian, bool is_signed, size_t size) {
  uint64_t value = 0;
  if (endian == big) {
    for (size_t i = 0; i < size; ++i) value = (value << 8) | static_cast<unsigned char>(bytes[i]);
  } else {
    for (size_t i = size; i-- > 0;) value = (value << 8) | static_cast<unsigned char>(bytes[i]);
  }
  if (!is_signed) return static_cast<lua_Number>(value);
  if (size < 8) {
    const uint64_t mask = ~uint64_t{0} << (size * 8 - 1);
    if (value & mask) value |= mask;
  }
  return static_cast<lua_Number>(static_cast<int64_t>(value));
}

int unpack(lua_State* L) {
  Header h;
  const char* fmt = luaL_checkstring(L, 1);
  size_t length = 0;
  const char* data = luaL_checklstring(L, 2, &length);
  const lua_Integer start = luaL_optinteger(L, 3, 1);
  luaL_argcheck(L, start > 0, 3, "offset must be 1 or greater");
  size_t pos = static_cast<size_t>(start) - 1;
  int n = 0;
  default_options(h);
  while (*fmt) {
    const char opt = *fmt++;
    size_t size = option_size(L, opt, &fmt);
    pos += to_align(pos, h, opt, size);
    luaL_argcheck(L, size <= length && pos <= length - size, 2, "data string too short");
    luaL_checkstack(L, 2, "too many results");
    switch (opt) {
      case 'b':
      case 'B':
      case 'h':
      case 'H':
      case 'l':
      case 'L':
      case 'T':
      case 'i':
      case 'I':
        lua_pushnumber(L, get_integer(data + pos, h.endian, std::islower(static_cast<unsigned char>(opt)) != 0, size));
        ++n;
        break;
      case 'x':
        break;
      case 'f': {
        char bytes[sizeof(float)];
        std::memcpy(bytes, data + pos, size);
        correct_bytes(bytes, sizeof(bytes), h.endian);
        float f = 0;
        std::memcpy(&f, bytes, sizeof(f));
        lua_pushnumber(L, f);
        ++n;
        break;
      }
      case 'd': {
        char bytes[sizeof(double)];
        std::memcpy(bytes, data + pos, size);
        correct_bytes(bytes, sizeof(bytes), h.endian);
        double d = 0;
        std::memcpy(&d, bytes, sizeof(d));
        lua_pushnumber(L, d);
        ++n;
        break;
      }
      case 'c': {
        if (size == 0) {
          if (n == 0 || !lua_isnumber(L, -1)) luaL_error(L, "format 'c0' needs a previous size");
          size = static_cast<size_t>(lua_tonumber(L, -1));
          lua_pop(L, 1);
          --n;
          luaL_argcheck(L, size <= length && pos <= length - size, 2, "data string too short");
        }
        lua_pushlstring(L, data + pos, size);
        ++n;
        break;
      }
      case 's': {
        const void* end = std::memchr(data + pos, '\0', length - pos);
        if (end == nullptr) luaL_error(L, "unfinished string in data");
        size = static_cast<size_t>(static_cast<const char*>(end) - (data + pos)) + 1;
        lua_pushlstring(L, data + pos, size - 1);
        ++n;
        break;
      }
      default:
        control_options(L, opt, &fmt, h);
    }
    pos += size;
  }
  lua_pushinteger(L, static_cast<lua_Integer>(pos + 1));
  return n + 1;
}

int size_of(lua_State* L) {
  Header h;
  const char* fmt = luaL_checkstring(L, 1);
  size_t pos = 0;
  default_options(h);
  while (*fmt) {
    const char opt = *fmt++;
    const size_t size = option_size(L, opt, &fmt);
    pos += to_align(pos, h, opt, size);
    if (opt == 's') {
      luaL_argerror(L, 1, "option 's' has no fixed size");
    } else if (opt == 'c' && size == 0) {
      luaL_argerror(L, 1, "option 'c0' has no fixed size");
    }
    if (!std::isalnum(static_cast<unsigned char>(opt))) control_options(L, opt, &fmt, h);
    pos += size;
  }
  lua_pushinteger(L, static_cast<lua_Integer>(pos));
  return 1;
}

}  // namespace

int open_struct(lua_State* L) {
  const luaL_Reg functions[] = {{"pack", pack}, {"unpack", unpack}, {"size", size_of}, {nullptr, nullptr}};
  lua_newtable(L);
  for (const luaL_Reg* f = functions; f->name; ++f) {
    lua_pushcfunction(L, f->func);
    lua_setfield(L, -2, f->name);
  }
  return 1;
}

}  // namespace mimir::command
