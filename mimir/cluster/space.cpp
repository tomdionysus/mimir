//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/cluster/space.h"

#include <string>
#include <utility>

#include "dannan/identity/node_identity.h"
#include "dannan/net/codec.h"
#include "mimir/cluster/register.h"
#include "mimir/version.h"

namespace mimir::cluster {

Space::Space(dannan::Context& context, bool ready) : _context(context), _store(std::make_shared<dannan::agree::MemoryStore>(context.time().executor(), ready)) {
  const std::string version(mimir::version);
  _context.nodes().advertise(std::string(space), dannan::net::Bytes(version.begin(), version.end()));
  _context.agreement().serve(std::string(space), std::string(space), _store);
}

Space::~Space() { _context.agreement().unserve(std::string(space)); }

void Space::found(std::chrono::milliseconds deadline, std::function<void(dannan::agree::Outcome)> done) {
  _context.agreement().found(std::string(space), deadline, _context.time().executor(), std::move(done));
}

uint32_t Space::shards() const { return _context.agreement().table().slots; }

void Space::advertise_clients(const std::string& name, const std::string& host, uint16_t port, uint16_t tls_port) {
  dannan::net::Writer out;
  out.string(name);
  out.string(host);
  out.u16(port);
  out.u16(tls_port);
  _context.nodes().advertise(client_advert, out.take());
}

std::vector<command::NodeAddress> Space::client_addresses() const {
  std::vector<command::NodeAddress> out;
  const auto self = _context.nodes().self();
  std::vector<dannan::membership::NodeRecord> nodes{self};
  for (auto& node : _context.nodes().active())
    if (node.id != self.id) nodes.push_back(std::move(node));
  for (const auto& node : nodes) {
    const auto advert = node.advert(client_advert);
    if (!advert) continue;
    // A peer's advert is untrusted bytes: one that does not read is left out.
    try {
      dannan::net::Reader in(*advert);
      command::NodeAddress address;
      address.name = in.string(256);
      address.host = in.string(256);
      address.port = in.u16();
      address.tls_port = in.u16();
      in.finish();
      if (address.host.empty()) address.host = node.host;
      address.id = dannan::identity::to_string(node.id);
      address.self = node.id == self.id;
      out.push_back(std::move(address));
    } catch (const dannan::net::DecodeError&) {
    }
  }
  return out;
}

}  // namespace mimir::cluster
