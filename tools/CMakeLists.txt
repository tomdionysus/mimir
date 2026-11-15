#
# Mimir - The Distributed Database
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
#

# mimir-node: a node that serves RESP.
add_executable(mimir-node mimir_node.cpp)
target_link_libraries(mimir-node PRIVATE mimir mimir_warnings)
