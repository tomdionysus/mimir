//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/command/table.h"

#include <array>
#include <string>
#include <unordered_map>

#include "mimir/command/args.h"
#include "mimir/command/handlers.h"

namespace mimir::command {

namespace {

Class classify_expire(const std::vector<std::string>& args) {
  for (size_t i = 3; i < args.size(); ++i) {
    if (iequals(args[i], "NX") || iequals(args[i], "XX") || iequals(args[i], "GT") || iequals(args[i], "LT")) return Class::agreement;
  }
  return Class::merge;
}

// Valkey's arity, flags and key positions for each; Mimir's class.
const std::array specs{
    // Strings.
    Spec{"get", 2, "readonly fast", 1, 1, 1, Class::read, strings::get},
    Spec{"set", -3, "write denyoom", 1, 1, 1, Class::merge, strings::set, strings::classify_set},
    Spec{"setnx", 3, "write denyoom fast", 1, 1, 1, Class::agreement, strings::setnx},
    Spec{"setex", 4, "write denyoom", 1, 1, 1, Class::merge, strings::setex},
    Spec{"psetex", 4, "write denyoom", 1, 1, 1, Class::merge, strings::psetex},
    Spec{"getset", 3, "write denyoom fast", 1, 1, 1, Class::agreement, strings::getset},
    Spec{"getdel", 2, "write fast", 1, 1, 1, Class::agreement, strings::getdel},
    Spec{"getex", -2, "write fast", 1, 1, 1, Class::merge, strings::getex},
    Spec{"mget", -2, "readonly fast", 1, -1, 1, Class::read, strings::mget},
    Spec{"mset", -3, "write denyoom", 1, -1, 2, Class::merge, strings::mset},
    Spec{"msetnx", -3, "write denyoom", 1, -1, 2, Class::agreement, strings::msetnx},
    Spec{"incr", 2, "write denyoom fast", 1, 1, 1, Class::configurable, strings::incr},
    Spec{"decr", 2, "write denyoom fast", 1, 1, 1, Class::configurable, strings::decr},
    Spec{"incrby", 3, "write denyoom fast", 1, 1, 1, Class::configurable, strings::incrby},
    Spec{"decrby", 3, "write denyoom fast", 1, 1, 1, Class::configurable, strings::decrby},
    Spec{"incrbyfloat", 3, "write denyoom fast", 1, 1, 1, Class::configurable, strings::incrbyfloat},
    Spec{"append", 3, "write denyoom fast", 1, 1, 1, Class::configurable, strings::append},
    Spec{"strlen", 2, "readonly fast", 1, 1, 1, Class::read, strings::strlen},
    Spec{"getrange", 4, "readonly", 1, 1, 1, Class::read, strings::getrange},
    Spec{"substr", 4, "readonly", 1, 1, 1, Class::read, strings::getrange},
    Spec{"setrange", 4, "write denyoom", 1, 1, 1, Class::configurable, strings::setrange},
    // Keys.
    Spec{"del", -2, "write", 1, -1, 1, Class::merge, generic::del},
    Spec{"unlink", -2, "write fast", 1, -1, 1, Class::merge, generic::del},
    Spec{"exists", -2, "readonly fast", 1, -1, 1, Class::read, generic::exists},
    Spec{"type", 2, "readonly fast", 1, 1, 1, Class::read, generic::type},
    Spec{"expire", -3, "write fast", 1, 1, 1, Class::merge, generic::expire, classify_expire},
    Spec{"pexpire", -3, "write fast", 1, 1, 1, Class::merge, generic::pexpire, classify_expire},
    Spec{"expireat", -3, "write fast", 1, 1, 1, Class::merge, generic::expireat, classify_expire},
    Spec{"pexpireat", -3, "write fast", 1, 1, 1, Class::merge, generic::pexpireat, classify_expire},
    Spec{"ttl", 2, "readonly fast", 1, 1, 1, Class::read, generic::ttl},
    Spec{"pttl", 2, "readonly fast", 1, 1, 1, Class::read, generic::pttl},
    Spec{"expiretime", 2, "readonly fast", 1, 1, 1, Class::read, generic::expiretime},
    Spec{"pexpiretime", 2, "readonly fast", 1, 1, 1, Class::read, generic::pexpiretime},
    Spec{"persist", 2, "write fast", 1, 1, 1, Class::merge, generic::persist},
    Spec{"rename", 3, "write", 1, 2, 1, Class::agreement, generic::rename},
    Spec{"renamenx", 3, "write fast", 1, 2, 1, Class::agreement, generic::renamenx},
    Spec{"copy", -3, "write denyoom", 1, 2, 1, Class::agreement, generic::copy},
    Spec{"keys", 2, "readonly", 0, 0, 0, Class::read, generic::keys},
    Spec{"scan", -2, "readonly", 0, 0, 0, Class::read, generic::scan},
    Spec{"dbsize", 1, "readonly fast", 0, 0, 0, Class::read, generic::dbsize},
    Spec{"randomkey", 1, "readonly", 0, 0, 0, Class::read, generic::randomkey},
    Spec{"touch", -2, "readonly fast", 1, -1, 1, Class::read, generic::touch},
    Spec{"flushall", -1, "write", 0, 0, 0, Class::admin, generic::flushall},
    Spec{"flushdb", -1, "write", 0, 0, 0, Class::admin, generic::flushall},
    // Connection and server.
    Spec{"ping", -1, "fast", 0, 0, 0, Class::connection, connection::ping},
    Spec{"echo", 2, "fast", 0, 0, 0, Class::connection, connection::echo},
    Spec{"quit", -1, "noscript loading stale fast no_auth allow_busy", 0, 0, 0, Class::connection, connection::quit},
    Spec{"select", 2, "loading stale fast", 0, 0, 0, Class::connection, connection::select},
    Spec{"hello", -1, "noscript loading stale fast no_auth allow_busy", 0, 0, 0, Class::connection, connection::hello},
    Spec{"auth", -2, "noscript loading stale fast no_auth allow_busy", 0, 0, 0, Class::connection, connection::auth},
    Spec{"client", -2, "", 0, 0, 0, Class::connection, connection::client},
    Spec{"command", -1, "loading stale", 0, 0, 0, Class::connection, connection::command},
    Spec{"info", -1, "loading stale", 0, 0, 0, Class::connection, connection::info},
    Spec{"time", 1, "loading stale fast", 0, 0, 0, Class::connection, connection::time},
    Spec{"reset", 1, "noscript loading stale fast no_auth allow_busy", 0, 0, 0, Class::connection, connection::reset},
};

const std::unordered_map<std::string, const Spec*>& by_name() {
  static const auto* map = [] {
    auto* made = new std::unordered_map<std::string, const Spec*>();
    for (const Spec& spec : specs) made->emplace(std::string(spec.name), &spec);
    return made;
  }();
  return *map;
}

}  // namespace

std::string_view class_name(Class cls) noexcept {
  switch (cls) {
    case Class::read:
      return "read";
    case Class::merge:
      return "merge";
    case Class::agreement:
      return "agreement";
    case Class::configurable:
      return "configurable";
    case Class::connection:
      return "connection";
    case Class::admin:
      return "admin";
  }
  return "unknown";
}

const Spec* find_spec(std::string_view name) noexcept {
  if (name.size() > 64) return nullptr;
  const auto& map = by_name();
  auto it = map.find(lower(name));
  return it == map.end() ? nullptr : it->second;
}

std::span<const Spec> all_specs() noexcept { return specs; }

Class class_of(const Spec& spec, const std::vector<std::string>& args) { return spec.classify ? spec.classify(args) : spec.cls; }

std::vector<std::string_view> keys_of(const Spec& spec, const std::vector<std::string>& args) {
  std::vector<std::string_view> keys;
  if (spec.first_key <= 0) return keys;
  const int count = static_cast<int>(args.size());
  const int last = spec.last_key < 0 ? count + spec.last_key : spec.last_key;
  for (int i = spec.first_key; i <= last && i < count; i += spec.step) keys.push_back(args[static_cast<size_t>(i)]);
  return keys;
}

}  // namespace mimir::command
