//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The version rule: the tag, project(... VERSION ...) and what the library
// reports are one number, and a consumer built against other headers is told.

#include "mimir/version.h"

#include <gtest/gtest.h>

#include <string>

namespace mimir {
namespace {

TEST(Version, TheLibraryReportsTheVersionItsHeadersSay) {
  EXPECT_EQ(std::string(library_version()), std::string(version));
  EXPECT_EQ(std::string(version), std::to_string(version_major) + "." + std::to_string(version_minor) + "." + std::to_string(version_patch));
}

}  // namespace
}  // namespace mimir
