//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

// What every command shares: its error, and reading its arguments as Valkey
// reads them.
namespace mimir::command {

// A command that could not run. what() is the whole error reply, its code first
// ("ERR syntax error", "WRONGTYPE ..."). Thrown before a command changes
// anything, so a refused command has no effect.
class CommandError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

inline constexpr const char* wrong_type = "WRONGTYPE Operation against a key holding the wrong kind of value";
inline constexpr const char* not_integer = "ERR value is not an integer or out of range";
inline constexpr const char* not_float = "ERR value is not a valid float";
inline constexpr const char* syntax_error = "ERR syntax error";

bool iequals(std::string_view a, std::string_view b) noexcept;
std::string lower(std::string_view text);
std::string upper(std::string_view text);

// A 64-bit integer as Valkey reads one: an optional '-', then "0" alone or a
// digit 1-9 and more digits, nothing else, in range.
std::optional<int64_t> parse_integer(std::string_view text) noexcept;
// The same, as an argument: throws CommandError(not_integer).
int64_t integer_argument(std::string_view text);
// A long double as Valkey reads one for INCRBYFLOAT: no spaces, not NaN.
std::optional<long double> parse_long_double(std::string_view text) noexcept;
// A long double written as INCRBYFLOAT replies: fixed, 17 places, trailing
// zeros and a trailing point removed.
std::string format_long_double(long double value);
// The decimal text of an integer.
std::string format_integer(int64_t value);

}  // namespace mimir::command
