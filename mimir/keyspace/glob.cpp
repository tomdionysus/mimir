//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/keyspace/glob.h"

#include <cctype>
#include <utility>

namespace mimir::keyspace {

namespace {

unsigned char fold(unsigned char c, bool nocase) { return nocase ? static_cast<unsigned char>(std::tolower(c)) : c; }

// Whether one pattern element at `p` matches the byte `c`; `p` is moved past
// the element. Precondition: p < pattern.size() and pattern[p] != '*'.
bool match_one(std::string_view pattern, size_t& p, unsigned char c, bool nocase) {
  const unsigned char s = fold(c, nocase);
  switch (pattern[p]) {
    case '?':
      ++p;
      return true;
    case '[': {
      ++p;
      bool negate = false;
      if (p < pattern.size() && pattern[p] == '^') {
        negate = true;
        ++p;
      }
      bool found = false;
      for (;;) {
        if (p >= pattern.size()) {
          // An unclosed set: what was read of it stands, as Valkey reads it.
          break;
        }
        if (pattern[p] == ']') {
          ++p;
          break;
        }
        if (pattern[p] == '\\' && p + 1 < pattern.size()) {
          ++p;
          if (fold(static_cast<unsigned char>(pattern[p]), nocase) == s) found = true;
          ++p;
        } else if (p + 2 < pattern.size() && pattern[p + 1] == '-') {
          unsigned char low = fold(static_cast<unsigned char>(pattern[p]), nocase);
          unsigned char high = fold(static_cast<unsigned char>(pattern[p + 2]), nocase);
          if (low > high) std::swap(low, high);
          if (s >= low && s <= high) found = true;
          p += 3;
        } else {
          if (fold(static_cast<unsigned char>(pattern[p]), nocase) == s) found = true;
          ++p;
        }
      }
      return negate ? !found : found;
    }
    case '\\':
      if (p + 1 < pattern.size()) ++p;
      [[fallthrough]];
    default: {
      const bool same = fold(static_cast<unsigned char>(pattern[p]), nocase) == s;
      ++p;
      return same;
    }
  }
}

}  // namespace

bool glob_match(std::string_view pattern, std::string_view subject, bool nocase) noexcept {
  // Iterative matching with one remembered star: on a mismatch, the most recent
  // star takes one more byte. Each star's retries are bounded by the subject's
  // length, and an earlier star is never revisited, which is sound for globs.
  size_t p = 0;
  size_t s = 0;
  size_t star_p = std::string_view::npos;
  size_t star_s = 0;
  while (s < subject.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      while (p < pattern.size() && pattern[p] == '*') ++p;
      if (p == pattern.size()) return true;
      star_p = p;
      star_s = s;
      continue;
    }
    if (p < pattern.size()) {
      size_t next = p;
      if (match_one(pattern, next, static_cast<unsigned char>(subject[s]), nocase)) {
        p = next;
        ++s;
        continue;
      }
    }
    if (star_p == std::string_view::npos) return false;
    p = star_p;
    s = ++star_s;
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

}  // namespace mimir::keyspace
