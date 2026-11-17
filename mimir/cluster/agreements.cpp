//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/cluster/agreements.h"

#include <utility>
#include <vector>

#include "mimir/cluster/register.h"

namespace mimir::cluster {

namespace agree = dannan::agree;

Agreements::Agreements(Space& served, std::chrono::milliseconds deadline) : _space(served), _deadline(deadline) {}

std::string Agreements::operation_id() {
  // The node, this process's durability epoch, and a count: never the same
  // twice, across the cluster and across restarts (40 bytes).
  auto& identity = _space.context().identity();
  const auto node = identity.id();
  const auto epoch = identity.durability_epoch();
  std::string id(node.bytes.begin(), node.bytes.end());
  id.append(epoch.bytes.begin(), epoch.bytes.end());
  const uint64_t count = _next.fetch_add(1);
  for (int shift = 56; shift >= 0; shift -= 8) id.push_back(static_cast<char>(count >> shift));
  return id;
}

void Agreements::change(size_t db, const std::string& key, Change change, std::function<void(Outcome)> done) {
  auto& agreement = _space.context().agreement();
  const auto& genesis = agreement.table().genesis;
  agreement.change(
      std::string(space), shard_of(key, _space.shards()), {name_of(static_cast<uint16_t>(db), key)}, genesis.nodes, genesis.version,
      [change = std::move(change)](std::span<const std::optional<agree::Bytes>> current) {
        agree::Changed out;
        // Every one of Mimir's changes finds its own operation in the record.
        out.repeatable = true;
        const auto record = decode_value(current.empty() ? std::nullopt : current[0]);
        if (!record) {
          // Not a record: left as it is, and said so.
          out.values.push_back(current.empty() ? std::nullopt : current[0]);
          const std::string refusal = "-ERR this key's stored record cannot be read\r\n";
          out.result.assign(refusal.begin(), refusal.end());
          return out;
        }
        auto changed = change(*record);
        out.values.push_back(encode(changed.record));
        out.result.assign(changed.result.begin(), changed.result.end());
        return out;
      },
      {}, _deadline, _space.context().time().executor(),
      [done = std::move(done)](agree::Outcome outcome) {
        Outcome out;
        out.committed = outcome.committed;
        out.unknown = outcome.unknown;
        out.why = outcome.why;
        if (outcome.committed) {
          const auto record = decode_value(outcome.values.empty() ? std::nullopt : outcome.values[0]);
          if (record) out.record = *record;
          out.result.assign(outcome.result.begin(), outcome.result.end());
        }
        done(std::move(out));
      });
}

}  // namespace mimir::cluster
