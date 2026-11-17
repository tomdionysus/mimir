//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/cluster/space.h"

#include <string>

#include "mimir/cluster/register.h"
#include "mimir/version.h"

namespace mimir::cluster {

Space::Space(dannan::Context& context, bool ready) : _context(context), _store(std::make_shared<Store>(context.time().executor(), ready)) {
  const std::string version(mimir::version);
  _context.nodes().advertise(std::string(space), dannan::net::Bytes(version.begin(), version.end()));
  _context.agreement().serve(std::string(space), std::string(space), _store);
}

Space::~Space() { _context.agreement().unserve(std::string(space)); }

void Space::found(std::chrono::milliseconds deadline, std::function<void(dannan::agree::Outcome)> done) {
  _context.agreement().found(std::string(space), deadline, _context.time().executor(), std::move(done));
}

uint32_t Space::shards() const { return _context.agreement().table().slots; }

}  // namespace mimir::cluster
