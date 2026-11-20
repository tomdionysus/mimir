//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/command/acl.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>

#include "dannan/identity/crypto.h"
#include "mimir/command/args.h"
#include "mimir/keyspace/glob.h"

namespace mimir::command {

namespace {

// What a key spec needs of a key pattern: R, W, both, or only its name.
enum KeyNeed : uint8_t { k_none = 0, k_R = 1, k_W = 2, k_RW = 3 };

// Valkey's key specifications: where the keys begin (an index, or after a
// keyword) and how they run on (a range, or a count given in the arguments).
struct BeginSearch {
  enum Kind : uint8_t { index, keyword, unknown } kind;
  int position;
  std::string_view word;
};
struct FindKeys {
  enum Kind : uint8_t { range, keynum, unknown } kind;
  // range: last key, step, limit. keynum: count's index, first key, step.
  int a;
  int b;
  int c;
};
struct KeySpec {
  KeyNeed need;
  // Valkey's own flags for the spec, as COMMAND GETKEYSANDFLAGS answers them.
  std::string_view flags;
  BeginSearch begin;
  FindKeys find;
};

constexpr BeginSearch bs_index(int i) { return {BeginSearch::index, i, {}}; }
constexpr BeginSearch bs_keyword(std::string_view word, int start_from) { return {BeginSearch::keyword, start_from, word}; }
constexpr BeginSearch bs_unknown() { return {BeginSearch::unknown, 0, {}}; }
constexpr FindKeys fk_range(int last, int step, int limit) { return {FindKeys::range, last, step, limit}; }
constexpr FindKeys fk_keynum(int index, int first, int step) { return {FindKeys::keynum, index, first, step}; }
constexpr FindKeys fk_unknown() { return {FindKeys::unknown, 0, 0, 0}; }

struct Entry {
  std::string_view name;
  std::string_view categories;
  std::vector<KeySpec> specs;
};

constexpr std::array<std::string_view, 21> category_names = {"keyspace", "read",   "write",       "set",       "sortedset",  "list",        "hash",
                                                             "string",   "bitmap", "hyperloglog", "geo",       "stream",     "pubsub",      "admin",
                                                             "fast",     "slow",   "blocking",    "dangerous", "connection", "transaction", "scripting"};

// The table, and what is derived from it once: each command's categories as
// bits, its id by name, and a container's subcommands.
struct Table {
  std::vector<Entry> entries;
  std::vector<uint32_t> categories;
  std::unordered_map<std::string, size_t> by_name;
  std::vector<std::vector<size_t>> children;
  std::vector<bool> container;
  std::vector<bool> sub;
};

const Table& table() {
  static const Table* made = [] {
    auto* t = new Table;
    t->entries = {
#include "mimir/command/acl_table.inc"
    };
    const size_t n = t->entries.size();
    t->categories.assign(n, 0);
    t->children.assign(n, {});
    t->container.assign(n, false);
    t->sub.assign(n, false);
    for (size_t i = 0; i < n; ++i) {
      const Entry& e = t->entries[i];
      t->by_name.emplace(std::string(e.name), i);
      std::string_view rest = e.categories;
      while (!rest.empty()) {
        const size_t space = rest.find(' ');
        std::string_view word = rest.substr(0, space);
        rest = space == std::string_view::npos ? std::string_view() : rest.substr(space + 1);
        if (!word.empty() && word[0] == '@') word.remove_prefix(1);
        for (size_t c = 0; c < category_names.size(); ++c)
          if (category_names[c] == word) t->categories[i] |= 1u << c;
      }
    }
    for (size_t i = 0; i < n; ++i) {
      const std::string_view name = t->entries[i].name;
      const size_t bar = name.find('|');
      if (bar == std::string_view::npos) continue;
      const size_t parent = t->by_name.at(std::string(name.substr(0, bar)));
      t->children[parent].push_back(i);
      t->container[parent] = true;
      t->sub[i] = true;
    }
    return t;
  }();
  return *made;
}

std::optional<size_t> id_of(std::string_view name) {
  const auto& by_name = table().by_name;
  auto it = by_name.find(lower(name));
  if (it == by_name.end()) return std::nullopt;
  return it->second;
}

// The id of the command `args` runs: a container's subcommand when it has
// one, none when the command or subcommand is unknown.
std::optional<size_t> command_id(const std::vector<std::string>& args) {
  if (args.empty()) return std::nullopt;
  auto id = id_of(args[0]);
  if (!id || !table().container[*id]) return id;
  if (args.size() < 2) return std::nullopt;
  return id_of(lower(args[0]) + "|" + lower(args[1]));
}

bool has_spaces(std::string_view text) {
  return std::any_of(text.begin(), text.end(), [](char c) { return c == '\0' || std::isspace(static_cast<unsigned char>(c)); });
}

std::string hash_password(std::string_view password) {
  return dannan::identity::hex(dannan::identity::sha256(std::span(reinterpret_cast<const uint8_t*>(password.data()), password.size())));
}

bool valid_hash(std::string_view hash) {
  return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::vector<std::string> split_words(std::string_view text) {
  std::vector<std::string> words;
  size_t at = 0;
  while (at < text.size()) {
    while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at;
    const size_t start = at;
    while (at < text.size() && !std::isspace(static_cast<unsigned char>(text[at]))) ++at;
    if (at > start) words.emplace_back(text.substr(start, at - start));
  }
  return words;
}

// Why a rule was refused, as Valkey's errno says it.
enum class RuleError { none, syntax, unknown, keys_after_all, channels_after_all, no_such_password, bad_hash, sub_first_arg };

std::string_view rule_error_text(RuleError error) {
  switch (error) {
    case RuleError::unknown:
      return "Unknown command or category name in ACL";
    case RuleError::syntax:
      return "Syntax error";
    case RuleError::keys_after_all:
      return "Adding a pattern after the * pattern (or the 'allkeys' flag) is not valid and does not have any effect. Try 'resetkeys' to start with an "
             "empty list of patterns";
    case RuleError::channels_after_all:
      return "Adding a pattern after the * pattern (or the 'allchannels' flag) is not valid and does not have any effect. Try 'resetchannels' to start "
             "with an empty list of channels";
    case RuleError::no_such_password:
      return "The password you are trying to remove from the user does not exist";
    case RuleError::bad_hash:
      return "The password hash must be exactly 64 characters and contain only lowercase hexadecimal characters";
    case RuleError::sub_first_arg:
      return "Allowing first-arg of a subcommand is not supported";
    case RuleError::none:
      break;
  }
  return "Wrong format";
}

Acl::Selector new_selector() {
  Acl::Selector s;
  s.allowed.assign(table().entries.size(), false);
  return s;
}

// A command's permission and its subcommands', as Valkey's
// ACLChangeSelectorPerm: taking any away ends ALLCOMMANDS.
void set_permission(Acl::Selector& s, size_t id, bool allow) {
  s.allowed[id] = allow;
  if (!allow) s.all_commands = false;
  s.first_args.erase(id);
  for (size_t child : table().children[id]) {
    s.allowed[child] = allow;
    if (!allow) s.all_commands = false;
  }
}

// A later rule for a command (or category) replaces an earlier one for it and
// for its subcommands, and goes to the end.
void update_rules(Acl::Selector& s, std::string_view rule, bool allow) {
  const std::string named = lower(rule);
  std::string kept;
  for (const std::string& existing : split_words(s.rules)) {
    std::string_view body = std::string_view(existing).substr(1);
    const bool same = body == named || (body.size() > named.size() && body.substr(0, named.size()) == named && body[named.size()] == '|');
    if (same) continue;
    if (!kept.empty()) kept += ' ';
    kept += existing;
  }
  if (!kept.empty()) kept += ' ';
  kept += (allow ? "+" : "-") + named;
  s.rules = std::move(kept);
}

RuleError set_selector(Acl::Selector& s, std::string_view op) {
  if (iequals(op, "allkeys") || iequals(op, "~*")) {
    s.all_keys = true;
    s.keys.clear();
  } else if (iequals(op, "resetkeys")) {
    s.all_keys = false;
    s.keys.clear();
  } else if (iequals(op, "allchannels") || iequals(op, "&*")) {
    s.all_channels = true;
    s.channels.clear();
  } else if (iequals(op, "resetchannels")) {
    s.all_channels = false;
    s.channels.clear();
  } else if (iequals(op, "allcommands") || iequals(op, "+@all")) {
    std::fill(s.allowed.begin(), s.allowed.end(), true);
    s.all_commands = true;
    s.future = true;
    s.rules.clear();
    s.first_args.clear();
  } else if (iequals(op, "nocommands") || iequals(op, "-@all")) {
    std::fill(s.allowed.begin(), s.allowed.end(), false);
    s.all_commands = false;
    s.future = false;
    s.rules.clear();
    s.first_args.clear();
  } else if (op[0] == '~' || op[0] == '%') {
    if (s.all_keys) return RuleError::keys_after_all;
    bool read = false, write = false;
    size_t offset = 1;
    if (op[0] == '%') {
      for (; offset < op.size(); ++offset) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(op[offset])));
        if (c == 'R' && !read) {
          read = true;
        } else if (c == 'W' && !write) {
          write = true;
        } else if (op[offset] == '~') {
          ++offset;
          break;
        } else {
          return RuleError::syntax;
        }
      }
      if (!read && !write) return RuleError::syntax;
    } else {
      read = write = true;
    }
    const std::string_view pattern = op.substr(std::min(offset, op.size()));
    if (has_spaces(pattern)) return RuleError::syntax;
    auto found = std::find_if(s.keys.begin(), s.keys.end(), [&](const Acl::KeyPattern& k) { return k.pattern == pattern; });
    if (found == s.keys.end()) {
      s.keys.push_back(Acl::KeyPattern{std::string(pattern), read, write});
    } else {
      found->read = found->read || read;
      found->write = found->write || write;
    }
    s.all_keys = false;
  } else if (op[0] == '&') {
    if (s.all_channels) return RuleError::channels_after_all;
    const std::string_view pattern = op.substr(1);
    if (has_spaces(pattern)) return RuleError::syntax;
    if (std::find(s.channels.begin(), s.channels.end(), pattern) == s.channels.end()) s.channels.emplace_back(pattern);
    s.all_channels = false;
  } else if ((op[0] == '+' || op[0] == '-') && op.size() > 1 && op[1] == '@') {
    const bool allow = op[0] == '+';
    const std::string name = lower(op.substr(2));
    const auto at = std::find(category_names.begin(), category_names.end(), name);
    if (at == category_names.end()) return RuleError::unknown;
    const uint32_t bit = 1u << static_cast<uint32_t>(at - category_names.begin());
    update_rules(s, op.substr(1), allow);
    const Table& t = table();
    for (size_t i = 0; i < t.entries.size(); ++i)
      if (t.categories[i] & bit) set_permission(s, i, allow);
  } else if (op[0] == '+') {
    const std::string_view name = op.substr(1);
    const size_t bar = name.rfind('|');
    if (bar == std::string_view::npos) {
      auto id = id_of(name);
      if (!id) return RuleError::unknown;
      set_permission(s, *id, true);
      update_rules(s, table().entries[*id].name, true);
      return RuleError::none;
    }
    auto base = id_of(name.substr(0, bar));
    if (!base) return RuleError::unknown;
    if (table().sub[*base]) return RuleError::sub_first_arg;
    const std::string_view first = name.substr(bar + 1);
    if (first.empty()) return RuleError::syntax;
    if (table().container[*base]) {
      auto id = id_of(name);
      if (!id) return RuleError::unknown;
      set_permission(s, *id, true);
    } else {
      // A first argument of an otherwise refused command (SELECT 0), which
      // Valkey keeps though it calls it a misuse.
      auto& firsts = s.first_args[*base];
      const std::string word = lower(first);
      if (std::find(firsts.begin(), firsts.end(), word) == firsts.end()) firsts.push_back(word);
    }
    update_rules(s, name, true);
  } else if (op[0] == '-') {
    auto id = id_of(op.substr(1));
    if (!id) return RuleError::unknown;
    set_permission(s, *id, false);
    update_rules(s, table().entries[*id].name, false);
  } else {
    return RuleError::syntax;
  }
  return RuleError::none;
}

RuleError set_user_rule(Acl::User& u, std::string_view op) {
  if (op.empty()) return RuleError::none;
  if (iequals(op, "on")) {
    u.enabled = true;
  } else if (iequals(op, "off")) {
    u.enabled = false;
  } else if (iequals(op, "skip-sanitize-payload")) {
    u.skip_sanitize = true;
  } else if (iequals(op, "sanitize-payload")) {
    u.skip_sanitize = false;
  } else if (iequals(op, "nopass")) {
    u.nopass = true;
    u.passwords.clear();
  } else if (iequals(op, "resetpass")) {
    u.nopass = false;
    u.passwords.clear();
  } else if (op[0] == '>' || op[0] == '#') {
    std::string hash;
    if (op[0] == '>') {
      hash = hash_password(op.substr(1));
    } else {
      if (!valid_hash(op.substr(1))) return RuleError::bad_hash;
      hash = std::string(op.substr(1));
    }
    if (std::find(u.passwords.begin(), u.passwords.end(), hash) == u.passwords.end()) u.passwords.push_back(std::move(hash));
    u.nopass = false;
  } else if (op[0] == '<' || op[0] == '!') {
    std::string hash;
    if (op[0] == '<') {
      hash = hash_password(op.substr(1));
    } else {
      if (!valid_hash(op.substr(1))) return RuleError::bad_hash;
      hash = std::string(op.substr(1));
    }
    auto found = std::find(u.passwords.begin(), u.passwords.end(), hash);
    if (found == u.passwords.end()) return RuleError::no_such_password;
    u.passwords.erase(found);
  } else if (op.size() >= 2 && op.front() == '(' && op.back() == ')') {
    Acl::Selector s = new_selector();
    for (const std::string& word : split_words(op.substr(1, op.size() - 2))) {
      const RuleError error = set_selector(s, word);
      if (error != RuleError::none) return error;
    }
    u.selectors.push_back(std::move(s));
  } else if (iequals(op, "clearselectors")) {
    u.selectors.resize(1);
  } else if (iequals(op, "reset")) {
    for (std::string_view each : {"resetpass", "resetkeys", "resetchannels", "off", "sanitize-payload", "clearselectors", "-@all"}) set_user_rule(u, each);
  } else {
    return set_selector(u.selectors.front(), op);
  }
  return RuleError::none;
}

std::string pattern_text(const Acl::KeyPattern& k) {
  if (k.read && k.write) return "~" + k.pattern;
  return (k.read ? "%R~" : "%W~") + k.pattern;
}

struct KeyRef {
  size_t position;
  uint8_t need;
  std::string_view flags;
};

// The keys `args` names and what each needs, by the command's key specs as
// Valkey's getKeysUsingKeySpecs finds them; none when a spec does not fit the
// arguments (the command then refuses them itself).
std::vector<KeyRef> keys_by_specs(const Entry& entry, const std::vector<std::string>& args) {
  std::vector<KeyRef> keys;
  const int argc = static_cast<int>(args.size());
  for (const KeySpec& spec : entry.specs) {
    int first = 0;
    if (spec.begin.kind == BeginSearch::index) {
      first = spec.begin.position;
    } else if (spec.begin.kind == BeginSearch::keyword) {
      const int from = spec.begin.position;
      const int start = from > 0 ? from : argc + from;
      const int end = from > 0 ? argc - 1 : 1;
      for (int i = start; i != end; i = start <= end ? i + 1 : i - 1) {
        if (i >= argc || i < 1) break;
        if (iequals(args[static_cast<size_t>(i)], spec.begin.word)) {
          first = i + 1;
          break;
        }
      }
      if (!first) continue;
    } else {
      return {};
    }
    int last = 0;
    int step = 1;
    if (spec.find.kind == FindKeys::range) {
      step = spec.find.b;
      if (spec.find.a >= 0) {
        last = first + spec.find.a;
      } else if (!spec.find.c) {
        last = argc + spec.find.a;
      } else {
        last = first + ((argc - first) / spec.find.c + spec.find.a);
      }
    } else if (spec.find.kind == FindKeys::keynum) {
      step = spec.find.c;
      if (spec.find.a >= argc - first) return {};
      auto count = parse_integer(args[static_cast<size_t>(first + spec.find.a)]);
      if (!count || *count < 0 || *count > argc) return {};
      first += spec.find.b;
      last = first + static_cast<int>(*count) - 1;
    } else {
      return {};
    }
    if (last >= argc || last < first || first >= argc) return {};
    for (int i = first; i <= last; i += std::max(step, 1)) keys.push_back(KeyRef{static_cast<size_t>(i), spec.need, spec.flags});
  }
  return keys;
}

std::vector<KeyRef> keys_of_command(size_t id, const std::vector<std::string>& args) {
  const Entry& entry = table().entries[id];
  // The commands whose keys' needs depend on their arguments, found as
  // Valkey's own getkeys procedures find them.
  if (entry.name == "set") {
    for (size_t i = 3; i < args.size(); ++i)
      if (iequals(args[i], "GET")) return {KeyRef{1, k_RW, "RW access update"}};
    return {KeyRef{1, k_W, "OW update"}};
  }
  if (entry.name == "bitfield") {
    bool read_only = true;
    for (size_t i = 2; i < args.size(); ++i) {
      const size_t more = args.size() - i - 1;
      if (iequals(args[i], "GET") && more >= 2) {
        i += 2;
      } else if ((iequals(args[i], "SET") || iequals(args[i], "INCRBY")) && more >= 3) {
        read_only = false;
        break;
      } else if (iequals(args[i], "OVERFLOW") && more >= 1) {
        i += 1;
      } else {
        read_only = false;
        break;
      }
    }
    if (read_only) return {KeyRef{1, k_R, "RO access"}};
    return {KeyRef{1, k_RW, "RW access update"}};
  }
  if (entry.name == "sort_ro") return {KeyRef{1, k_R, "RO access"}};
  if (entry.name == "migrate") {
    // Valkey's migrateGetKeys: the key at 3, or those after KEYS, passing
    // over AUTH's and AUTH2's arguments.
    for (size_t i = 6; i < args.size(); ++i) {
      if (iequals(args[i], "AUTH")) {
        i += 1;
      } else if (iequals(args[i], "AUTH2")) {
        i += 2;
      } else if (iequals(args[i], "KEYS")) {
        std::vector<KeyRef> keys;
        for (size_t j = i + 1; j < args.size(); ++j) keys.push_back(KeyRef{j, k_RW, "RW access delete"});
        return keys;
      }
    }
    return {KeyRef{3, k_RW, "RW access delete"}};
  }
  if (entry.name == "sort") {
    std::vector<KeyRef> keys{KeyRef{1, k_R, "RO access"}};
    std::optional<size_t> store;
    for (size_t i = 2; i < args.size(); ++i) {
      if (iequals(args[i], "LIMIT")) {
        i += 2;
      } else if (iequals(args[i], "STORE") && i + 1 < args.size()) {
        store = i + 1;
      } else if (iequals(args[i], "GET") || iequals(args[i], "BY")) {
        i += 1;
      }
    }
    if (store) keys.push_back(KeyRef{*store, k_W, "OW update"});
    return keys;
  }
  return keys_by_specs(entry, args);
}

struct ChannelRef {
  size_t position;
  bool pattern;
};

std::vector<ChannelRef> channels_of(size_t id, const std::vector<std::string>& args) {
  const std::string_view name = table().entries[id].name;
  std::vector<ChannelRef> refs;
  if (name == "publish" || name == "spublish") {
    if (args.size() > 1) refs.push_back(ChannelRef{1, false});
  } else if (name == "subscribe" || name == "ssubscribe" || name == "psubscribe") {
    for (size_t i = 1; i < args.size(); ++i) refs.push_back(ChannelRef{i, name == "psubscribe"});
  }
  return refs;
}

bool channel_allowed(const Acl::Selector& s, std::string_view channel, bool pattern) {
  for (const std::string& allowed : s.channels) {
    if (pattern ? allowed == channel : keyspace::glob_match(allowed, channel)) return true;
  }
  return false;
}

bool no_auth(std::string_view name) { return name == "auth" || name == "hello" || name == "quit" || name == "reset"; }

AclVerdict check_selector(const Acl::Selector& s, size_t id, const std::vector<std::string>& args) {
  const Table& t = table();
  const size_t base = t.sub[id] ? t.by_name.at(lower(args[0])) : id;
  if (!s.all_commands && !no_auth(t.entries[base].name) && !s.allowed[id]) {
    auto firsts = s.first_args.find(id);
    if (firsts == s.first_args.end() || args.size() < 2) return {Denial::command, 0};
    const std::string& first = args[t.sub[id] ? 2 : 1];
    if (std::none_of(firsts->second.begin(), firsts->second.end(), [&](const std::string& f) { return iequals(f, first); })) return {Denial::command, 0};
  }
  if (!s.all_keys) {
    for (const KeyRef& ref : keys_of_command(id, args)) {
      const std::string& key = args[ref.position];
      const bool read = ref.need & k_R;
      const bool write = ref.need & k_W;
      const bool ok = std::any_of(s.keys.begin(), s.keys.end(),
                                  [&](const Acl::KeyPattern& k) { return (!read || k.read) && (!write || k.write) && keyspace::glob_match(k.pattern, key); });
      if (!ok) return {Denial::key, ref.position};
    }
  }
  if (!s.all_channels) {
    for (const ChannelRef& ref : channels_of(base, args)) {
      if (!channel_allowed(s, args[ref.position], ref.pattern)) return {Denial::channel, ref.position};
    }
  }
  return {};
}

}  // namespace

Acl::Acl() {
  const std::vector<std::string> rules{"+@all", "~*", "&*", "on", "nopass"};
  set_user("default", rules);
}

std::optional<std::string> Acl::set_user(std::string_view name, std::span<const std::string> rules) {
  if (has_spaces(name)) return std::string("ERR Usernames can't contain spaces or null characters");
  // A selector's rules may come as several arguments: "(~x" "+get)".
  std::vector<std::string> merged;
  std::optional<size_t> open;
  for (size_t i = 0; i < rules.size(); ++i) {
    const std::string& op = rules[i];
    if (!open && !op.empty() && op.front() == '(' && op.back() != ')') {
      open = i;
      merged.push_back(op);
      continue;
    }
    if (open) {
      merged.back() += " " + op;
      if (!op.empty() && op.back() == ')') open.reset();
      continue;
    }
    merged.push_back(op);
  }
  if (open) return "ERR Unmatched parenthesis in acl selector starting at '" + rules[*open] + "'.";
  User user;
  if (const User* existing = find(name)) {
    user = *existing;
  } else {
    user.name = std::string(name);
    user.selectors.push_back(new_selector());
  }
  for (const std::string& op : merged) {
    const RuleError error = set_user_rule(user, op);
    if (error != RuleError::none) return "ERR Error in ACL SETUSER modifier '" + op + "': " + std::string(rule_error_text(error));
  }
  _users.insert_or_assign(std::string(name), std::move(user));
  return std::nullopt;
}

std::optional<std::string> Acl::load_line(std::string_view line) {
  std::vector<std::string> words = split_words(line);
  if (words.empty()) return std::nullopt;
  if (words.size() < 2 || words[0] != "user") return "a user line is \"user <name> <rules>...\": " + std::string(line);
  return set_user(words[1], std::span<const std::string>(words).subspan(2));
}

std::vector<std::string> Acl::lines() const {
  std::vector<std::string> out;
  out.reserve(_users.size());
  for (const auto& [name, user] : _users) out.push_back("user " + name + " " + describe(user));
  return out;
}

std::optional<std::string> Acl::replace_users(std::span<const std::string> lines) {
  Acl loaded;
  loaded._users.clear();
  for (const std::string& line : lines)
    if (auto error = loaded.load_line(line)) return error;
  _users = std::move(loaded._users);
  return std::nullopt;
}

bool Acl::delete_user(std::string_view name) {
  auto found = _users.find(name);
  if (found == _users.end()) return false;
  _users.erase(found);
  return true;
}

const Acl::User* Acl::find(std::string_view name) const {
  auto found = _users.find(name);
  return found == _users.end() ? nullptr : &found->second;
}

bool Acl::authenticate(std::string_view name, std::string_view password) const {
  const User* user = find(name);
  if (!user || !user->enabled) return false;
  if (user->nopass) return true;
  const std::string hash = hash_password(password);
  bool matched = false;
  for (const std::string& kept : user->passwords) {
    matched = dannan::identity::constant_time_equal(std::span(reinterpret_cast<const uint8_t*>(kept.data()), kept.size()),
                                                    std::span(reinterpret_cast<const uint8_t*>(hash.data()), hash.size())) ||
              matched;
  }
  return matched;
}

bool Acl::auth_required() const {
  const User* user = find("default");
  return !user || !user->nopass || !user->enabled;
}

std::vector<std::string> Acl::flags_of(const User& user) {
  std::vector<std::string> flags{user.enabled ? "on" : "off"};
  if (user.nopass) flags.emplace_back("nopass");
  flags.emplace_back(user.skip_sanitize ? "skip-sanitize-payload" : "sanitize-payload");
  return flags;
}

std::string Acl::describe_commands(const Selector& s) {
  std::string out = s.future ? "+@all" : "-@all";
  if (!s.rules.empty()) out += " " + s.rules;
  return out;
}

std::string Acl::describe_keys(const Selector& s) {
  if (s.all_keys) return "~*";
  std::string out;
  for (const KeyPattern& k : s.keys) {
    if (!out.empty()) out += ' ';
    out += pattern_text(k);
  }
  return out;
}

std::string Acl::describe_channels(const Selector& s) {
  if (s.all_channels) return "&*";
  std::string out;
  for (const std::string& c : s.channels) {
    if (!out.empty()) out += ' ';
    out += "&" + c;
  }
  return out;
}

std::string Acl::describe(const User& user) {
  std::string out;
  for (const std::string& flag : flags_of(user)) out += flag + " ";
  for (const std::string& hash : user.passwords) out += "#" + hash + " ";
  const auto selector_text = [](const Selector& s) {
    std::string text;
    if (s.all_keys) {
      text += "~* ";
    } else {
      for (const KeyPattern& k : s.keys) text += pattern_text(k) + " ";
    }
    if (s.all_channels) {
      text += "&* ";
    } else {
      text += "resetchannels ";
      for (const std::string& c : s.channels) text += "&" + c + " ";
    }
    return text + describe_commands(s);
  };
  for (size_t i = 0; i < user.selectors.size(); ++i) {
    if (i == 0)
      out += selector_text(user.selectors[i]);
    else
      out += " (" + selector_text(user.selectors[i]) + ")";
  }
  return out;
}

bool Acl::unrestricted(const User& user) {
  return std::any_of(user.selectors.begin(), user.selectors.end(), [](const Selector& s) { return s.all_commands && s.all_keys && s.all_channels; });
}

AclVerdict Acl::check(const User& user, const std::vector<std::string>& args) {
  if (unrestricted(user)) return {};
  auto id = command_id(args);
  // An unknown command or subcommand is the command's own to refuse.
  if (!id) return {};
  AclVerdict relevant{Denial::command, 0};
  bool first = true;
  for (const Selector& s : user.selectors) {
    const AclVerdict verdict = check_selector(s, *id, args);
    if (verdict.denial == Denial::none) return verdict;
    if (first || verdict.denial > relevant.denial || (verdict.denial == relevant.denial && verdict.position > relevant.position)) relevant = verdict;
    first = false;
  }
  return relevant;
}

std::string Acl::full_name(const std::vector<std::string>& args) {
  auto id = command_id(args);
  if (id) return std::string(table().entries[*id].name);
  return args.empty() ? std::string() : lower(args[0]);
}

bool Acl::known(const std::vector<std::string>& args) { return command_id(args).has_value(); }

bool Acl::has_keys(const std::vector<std::string>& args) {
  auto id = command_id(args);
  if (!id) return false;
  const Entry& entry = table().entries[*id];
  return !entry.specs.empty() || entry.name == "set" || entry.name == "bitfield" || entry.name == "sort" || entry.name == "sort_ro" || entry.name == "migrate";
}

std::vector<std::pair<size_t, std::string_view>> Acl::keys_with_flags(const std::vector<std::string>& args) {
  std::vector<std::pair<size_t, std::string_view>> out;
  auto id = command_id(args);
  if (!id) return out;
  for (const KeyRef& ref : keys_of_command(*id, args)) out.emplace_back(ref.position, ref.flags);
  return out;
}

std::string Acl::message(const AclVerdict& verdict, std::string_view user, const std::vector<std::string>& args, bool verbose) {
  switch (verdict.denial) {
    case Denial::command:
      return "User " + std::string(user) + " has no permissions to run the '" + full_name(args) + "' command";
    case Denial::key:
      if (verbose) return "User " + std::string(user) + " has no permissions to access the '" + args[verdict.position] + "' key";
      return "No permissions to access a key";
    case Denial::channel:
      if (verbose) return "User " + std::string(user) + " has no permissions to access the '" + args[verdict.position] + "' channel";
      return "No permissions to access a channel";
    case Denial::auth:
    case Denial::none:
      break;
  }
  return {};
}

std::span<const std::string_view> Acl::categories() { return category_names; }

std::optional<std::vector<std::string>> Acl::commands_in(std::string_view category) {
  const std::string name = lower(category);
  const auto at = std::find(category_names.begin(), category_names.end(), name);
  if (at == category_names.end()) return std::nullopt;
  const uint32_t bit = 1u << static_cast<uint32_t>(at - category_names.begin());
  std::vector<std::string> names;
  const Table& t = table();
  for (size_t i = 0; i < t.entries.size(); ++i)
    if (t.categories[i] & bit) names.emplace_back(t.entries[i].name);
  return names;
}

std::vector<std::string> Acl::categories_of(std::string_view command) {
  std::vector<std::string> out;
  auto id = id_of(command);
  if (!id) return out;
  const uint32_t bits = table().categories[*id];
  for (size_t c = 0; c < category_names.size(); ++c)
    if (bits & (1u << c)) out.push_back("@" + std::string(category_names[c]));
  return out;
}

void Acl::log(std::string reason, std::string context, std::string object, std::string username, std::string client_info, uint64_t now_ms) {
  // Valkey looks at the ten newest for one to count again, within a minute.
  constexpr size_t looked_at = 10;
  constexpr uint64_t grouping_ms = 60000;
  constexpr size_t kept = 128;
  for (size_t i = 0; i < _log.size() && i < looked_at; ++i) {
    LogEntry& e = _log[i];
    if (e.reason != reason || e.context != context || e.object != object || e.username != username) continue;
    if (now_ms - std::min(now_ms, e.updated_ms) > grouping_ms) continue;
    LogEntry moved = std::move(e);
    _log.erase(_log.begin() + static_cast<std::ptrdiff_t>(i));
    ++moved.count;
    moved.updated_ms = now_ms;
    moved.client_info = std::move(client_info);
    _log.push_front(std::move(moved));
    return;
  }
  _log.push_front(
      LogEntry{1, std::move(reason), std::move(context), std::move(object), std::move(username), now_ms, now_ms, std::move(client_info), _next_log_id++});
  while (_log.size() > kept) _log.pop_back();
}

}  // namespace mimir::command
