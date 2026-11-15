//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <string_view>

// Glob-style patterns as KEYS, SCAN MATCH and PSUBSCRIBE take them: '*' any
// run, '?' any one byte, "[abc]", "[^abc]" and "[a-z]" a set, and '\' quoting
// the byte after it. Byte-wise, case-sensitive unless asked. A pattern is the
// client's, so matching is bounded: backtracking over runs of '*' is linear in
// the subject for each star, never exponential.
namespace mimir::keyspace {

bool glob_match(std::string_view pattern, std::string_view subject, bool nocase = false) noexcept;

}  // namespace mimir::keyspace
