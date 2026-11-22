//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Valkey's access control lists (Valkey 8, "ACL"): users, each with passwords
// and selectors saying which commands, keys and channels it may use, the rules
// that change them, the descriptions ACL LIST and GETUSER answer, the check
// every command passes, and the log of refusals. At the single-node stage the
// users live in memory, seeded from the settings (ruling 27); in the cluster
// SETUSER and DELUSER commit by agreement. Not thread-safe: the database holds
// it under its lock.
namespace mimir::command {

// Why a command was refused, in Valkey's order of relevance: when no selector
// allows a command, the refusal reported is the most relevant one found.
enum class Denial : uint8_t { none, command, key, auth, channel };

struct AclVerdict {
  Denial denial = Denial::none;
  // The argument refused (a key or a channel), by its index.
  size_t position = 0;
};

class Acl {
 public:
  struct KeyPattern {
    std::string pattern;
    bool read = false;
    bool write = false;
  };

  struct Selector {
    // Valkey's ALLCOMMANDS: every command passes, until one is taken away.
    bool all_commands = false;
    // +@all's reserved bit: the description starts "+@all", not "-@all".
    bool future = false;
    bool all_keys = false;
    bool all_channels = false;
    // Per command of the ACL table (acl_table.inc), whether it is allowed.
    std::vector<bool> allowed;
    // A command otherwise refused, allowed with these first arguments.
    std::map<size_t, std::vector<std::string>> first_args;
    std::vector<KeyPattern> keys;
    std::vector<std::string> channels;
    // The command rules as they were given, a later rule for a command
    // replacing an earlier one: what the description shows after +@all/-@all.
    std::string rules;
  };

  struct User {
    std::string name;
    bool enabled = false;
    bool nopass = false;
    bool skip_sanitize = false;
    // SHA-256 of each password, in lower-case hex, in the order added.
    std::vector<std::string> passwords;
    // The root selector first, then those added in parentheses.
    std::vector<Selector> selectors;
  };

  // The default user, as Valkey makes it: on, nopass, every key, channel and
  // command.
  Acl();

  // ACL SETUSER: the rules applied in order to the user (made when new), all
  // or none. The error reply when one is refused.
  std::optional<std::string> set_user(std::string_view name, std::span<const std::string> rules);
  // One line of an ACL file ("user <name> <rules>..."), for the settings.
  std::optional<std::string> load_line(std::string_view line);
  // ACL DELUSER of one user; false when there is none. The default user
  // cannot be removed (the caller refuses it).
  bool delete_user(std::string_view name);
  const User* find(std::string_view name) const;
  const std::map<std::string, User, std::less<>>& users() const noexcept { return _users; }
  // ACL LIST's lines, one a user; replace_users takes them back. In a cluster
  // they are the users' record (ruling 27).
  std::vector<std::string> lines() const;
  // Every user replaced by those `lines` describe, all or none (the error
  // when one does not load). The log is kept.
  std::optional<std::string> replace_users(std::span<const std::string> lines);

  // Whether `password` opens `name`: the user exists, is on, and is nopass or
  // has the password's hash.
  bool authenticate(std::string_view name, std::string_view password) const;
  // A connection that has not authenticated must before any command but those
  // that need none: the default user is off or has passwords.
  bool auth_required() const;

  // What ACL LIST shows after "user <name> ".
  static std::string describe(const User& user);
  // ACL GETUSER's parts of one selector.
  static std::string describe_commands(const Selector& selector);
  static std::string describe_keys(const Selector& selector);
  static std::string describe_channels(const Selector& selector);
  static std::vector<std::string> flags_of(const User& user);

  // Whether `user` may run `args` (its name first, its arity checked).
  static AclVerdict check(const User& user, const std::vector<std::string>& args);
  // Whether a user can never be refused: one selector allowing everything.
  static bool unrestricted(const User& user);
  // The refusal's words: "User u has no permissions to run the 'get'
  // command", "No permissions to access a key" (verbose: "... the 'k' key").
  static std::string message(const AclVerdict& verdict, std::string_view user, const std::vector<std::string>& args, bool verbose);
  // A command's full name as ACL names it: "get", "client|list".
  static std::string full_name(const std::vector<std::string>& args);
  // Whether ACL knows the command `args` names (with its subcommand, for a
  // container): DRYRUN's "Command not found" otherwise.
  static bool known(const std::vector<std::string>& args);
  // COMMAND GETKEYS: whether the command takes keys at all, and the keys
  // `args` names (by index), each with Valkey's flags for it ("RW access
  // update"), found by Valkey's key specifications.
  static bool has_keys(const std::vector<std::string>& args);
  static std::vector<std::pair<size_t, std::string_view>> keys_with_flags(const std::vector<std::string>& args);

  // The categories in Valkey's order, and the commands in one (by full name);
  // none for a category that does not exist.
  static std::span<const std::string_view> categories();
  static std::optional<std::vector<std::string>> commands_in(std::string_view category);
  // COMMAND INFO's categories of a command ("@write", ...).
  static std::vector<std::string> categories_of(std::string_view command);
  // Every command and subcommand by full name ("config", "config|get").
  static std::vector<std::string> command_names();

  // The log of refusals, newest first (ACL LOG).
  struct LogEntry {
    uint64_t count = 1;
    std::string reason;
    std::string context;
    std::string object;
    std::string username;
    uint64_t created_ms = 0;
    uint64_t updated_ms = 0;
    std::string client_info;
    uint64_t id = 0;
  };
  void log(std::string reason, std::string context, std::string object, std::string username, std::string client_info, uint64_t now_ms);
  const std::deque<LogEntry>& log_entries() const noexcept { return _log; }
  void reset_log() { _log.clear(); }

 private:
  std::map<std::string, User, std::less<>> _users;
  std::deque<LogEntry> _log;
  uint64_t _next_log_id = 0;
};

// Valkey's words for a refused AUTH.
inline constexpr const char* wrong_pass = "WRONGPASS invalid username-password pair or user is disabled.";

}  // namespace mimir::command
