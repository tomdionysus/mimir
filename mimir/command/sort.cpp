//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// SORT and SORT_RO, as Valkey 8 documents and answers them: the elements of a
// list, set or sorted set, as numbers or (ALPHA) as bytes, by themselves or
// by the values a BY pattern names, a LIMIT of them, each answered as itself
// or as what GET patterns name, or stored as a list.

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"

namespace mimir::command::sorting {

namespace {

// What a BY or GET pattern names for `element`: the pattern with its first
// '*' replaced by the element, a string key's value or, with "->field", a
// hash's field; "#" is the element itself. None when the pattern has no '*',
// or names nothing, or a value of another type.
std::optional<std::string> look_up(Call& call, const std::string& pattern, const std::string& element) {
  if (pattern == "#") return element;
  const size_t star = pattern.find('*');
  if (star == std::string::npos) return std::nullopt;
  const size_t arrow = pattern.find("->", star);
  const bool field = arrow != std::string::npos && arrow + 2 < pattern.size();
  const std::string key_pattern = field ? pattern.substr(0, arrow) : pattern;
  std::string key = key_pattern.substr(0, star) + element + key_pattern.substr(star + 1);
  keyspace::Entry* entry = call.keys.find(key);
  if (!entry) return std::nullopt;
  if (field) {
    if (entry->type() != keyspace::Type::hash) return std::nullopt;
    const std::string* value = std::get<keyspace::Hash>(entry->value).get(std::string_view(pattern).substr(arrow + 2));
    if (!value) return std::nullopt;
    return *value;
  }
  if (entry->type() != keyspace::Type::string) return std::nullopt;
  return std::get<std::string>(entry->value);
}

struct Item {
  std::string element;
  double score = 0;
  std::optional<std::string> by;
};

int compare_bytes(std::string_view a, std::string_view b) {
  const int c = a.compare(b);
  return c < 0 ? -1 : c > 0 ? 1 : 0;
}

void sort_generic(Call& call, bool read_only) {
  const std::string& key = call.args[1];
  bool desc = false;
  bool alpha = false;
  int64_t limit_start = 0;
  int64_t limit_count = -1;
  std::optional<std::string> by;
  bool dontsort = false;
  std::vector<std::string> gets;
  std::optional<std::string> store;
  for (size_t j = 2; j < call.args.size(); ++j) {
    const size_t left = call.args.size() - j - 1;
    const std::string& word = call.args[j];
    if (iequals(word, "ASC")) {
      desc = false;
    } else if (iequals(word, "DESC")) {
      desc = true;
    } else if (iequals(word, "ALPHA")) {
      alpha = true;
    } else if (iequals(word, "LIMIT") && left >= 2) {
      limit_start = integer_argument(call.args[j + 1]);
      limit_count = integer_argument(call.args[j + 2]);
      j += 2;
    } else if (iequals(word, "STORE") && left >= 1 && !read_only) {
      store = call.args[++j];
    } else if (iequals(word, "BY") && left >= 1) {
      by = call.args[++j];
      // A pattern with no '*' sorts nothing: the elements keep their order.
      if (by->find('*') == std::string::npos) dontsort = true;
    } else if (iequals(word, "GET") && left >= 1) {
      gets.push_back(call.args[++j]);
    } else {
      throw CommandError(syntax_error);
    }
  }

  keyspace::Entry* entry = call.keys.find(key);
  if (entry && entry->type() != keyspace::Type::list && entry->type() != keyspace::Type::set && entry->type() != keyspace::Type::zset) {
    throw CommandError(wrong_type);
  }
  const keyspace::Type type = entry ? entry->type() : keyspace::Type::list;
  // A set left unsorted is sorted anyway where the answer must not depend on
  // its order: into STORE, or in a script.
  if (dontsort && type == keyspace::Type::set && (store || call.session.scripted)) {
    dontsort = false;
    alpha = true;
    by.reset();
  }

  // The elements, in their own order.
  std::vector<Item> items;
  if (entry) {
    switch (type) {
      case keyspace::Type::list:
        for (const auto& e : std::get<keyspace::List>(entry->value)) items.push_back(Item{e, 0, std::nullopt});
        break;
      case keyspace::Type::set:
        for (auto m : std::get<keyspace::Set>(entry->value).ordered()) items.push_back(Item{std::string(m), 0, std::nullopt});
        break;
      default: {
        const auto& zset = std::get<keyspace::SortedSet>(entry->value);
        for (size_t r = 0; r < zset.size(); ++r) items.push_back(Item{zset.at(r).member, 0, std::nullopt});
        break;
      }
    }
  }
  const int64_t length = static_cast<int64_t>(items.size());
  int64_t start = limit_start < 0 ? 0 : limit_start;
  int64_t end = limit_count < 0 ? length - 1 : start + limit_count - 1;
  if (start >= length) {
    start = length - 1;
    end = length - 2;
  }
  if (end >= length) end = length - 1;

  if (dontsort) {
    // A list or a sorted set keeps its order in the direction asked (the
    // differential gate's finding for a list); a set its own.
    if (desc && (type == keyspace::Type::zset || type == keyspace::Type::list)) {
      std::reverse(items.begin(), items.end());
    }
  } else {
    bool unconvertible = false;
    for (auto& item : items) {
      std::optional<std::string> value;
      if (by) {
        value = look_up(call, *by, item.element);
        if (!value) continue;
      } else {
        value = item.element;
      }
      if (alpha) {
        if (by) item.by = std::move(value);
      } else {
        errno = 0;
        char* end_of = nullptr;
        item.score = std::strtod(value->c_str(), &end_of);
        if (*end_of != '\0' || errno == ERANGE || std::isnan(item.score)) unconvertible = true;
      }
    }
    if (unconvertible) throw CommandError("ERR One or more scores can't be converted into double");
    const bool use_by = by.has_value();
    std::stable_sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
      int cmp = 0;
      if (!alpha) {
        cmp = a.score > b.score ? 1 : a.score < b.score ? -1 : compare_bytes(a.element, b.element);
      } else if (use_by) {
        if (!a.by && !b.by)
          cmp = 0;
        else if (!a.by)
          cmp = -1;
        else if (!b.by)
          cmp = 1;
        else
          cmp = compare_bytes(*a.by, *b.by);
      } else {
        cmp = compare_bytes(a.element, b.element);
      }
      return desc ? cmp > 0 : cmp < 0;
    });
  }

  // The answer: each element in range as itself, or as each GET names it.
  std::vector<std::optional<std::string>> out;
  for (int64_t j = start; j <= end; ++j) {
    const std::string& element = items[static_cast<size_t>(j)].element;
    if (gets.empty()) {
      out.emplace_back(element);
    } else {
      for (const auto& pattern : gets) out.push_back(look_up(call, pattern, element));
    }
  }
  if (!store) {
    call.reply.array(out.size());
    for (const auto& value : out) {
      if (value)
        call.reply.bulk(*value);
      else
        call.reply.null();
    }
    return;
  }
  call.reply.integer(static_cast<int64_t>(out.size()));
  if (out.empty()) {
    if (call.keys.erase(*store)) call.notify('g', "del", *store);
    return;
  }
  keyspace::List stored;
  for (auto& value : out) stored.push_back(value ? std::move(*value) : std::string());
  call.keys.put(*store, keyspace::Entry{std::move(stored), std::nullopt});
  call.notify('l', "sortstore", *store);
}

}  // namespace

void sort(Call& call) { sort_generic(call, false); }
void sort_ro(Call& call) { sort_generic(call, true); }

// SORT's keys: the key sorted and STORE's destination.
std::vector<std::string_view> sort_keys(const std::vector<std::string>& args) {
  std::vector<std::string_view> keys;
  if (args.size() < 2) return keys;
  keys.push_back(args[1]);
  for (size_t j = 2; j + 1 < args.size(); ++j) {
    if (iequals(args[j], "LIMIT")) {
      j += 2;
    } else if (iequals(args[j], "BY") || iequals(args[j], "GET")) {
      ++j;
    } else if (iequals(args[j], "STORE")) {
      keys.push_back(args[++j]);
    }
  }
  return keys;
}

}  // namespace mimir::command::sorting
