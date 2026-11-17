//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/cluster/agreements.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "dannan/net/codec.h"
#include "mimir/cluster/register.h"

namespace mimir::cluster {

namespace agree = dannan::agree;

namespace {

std::span<const uint8_t> bytes_of(std::string_view text) { return {reinterpret_cast<const uint8_t*>(text.data()), text.size()}; }
std::string text_of(std::span<const uint8_t> bytes) { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }

// A ballot as bytes that order as ballots do: the counter big-endian, then the
// node and the epoch.
std::string version_of(const agree::Ballot& ballot) {
  std::string out;
  for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<char>(ballot.counter >> shift));
  out.append(ballot.node.bytes.begin(), ballot.node.bytes.end());
  out.append(ballot.epoch.bytes.begin(), ballot.epoch.bytes.end());
  return out;
}

inline constexpr size_t version_size = 8 + 16 + 16;
// Valkey's proto-max-bulk-len bounds a key.
inline constexpr size_t max_key = 512ull * 1024 * 1024;

}  // namespace

void Agreements::register_types(dannan::Context& context) {
  dannan::net::Registration registration;
  registration.name = "mimir.commit_notice";
  context.routes().add(commit_notice, registration);
}

Agreements::Agreements(Space& served, std::chrono::milliseconds deadline) : _space(served), _deadline(deadline) {
  _space.context().routes().bind(commit_notice,
                                 [hook = _hook](const dannan::membership::NodeRecord&, dannan::sched::FrameType, const dannan::net::Message& request) {
                                   // Untrusted bytes: every length is bounded before anything is kept.
                                   try {
                                     dannan::net::Reader in(request.payload);
                                     const auto db = in.u16();
                                     const auto key = text_of(in.view_bytes(max_key));
                                     const auto version = text_of(in.view_bytes(version_size));
                                     const bool carried = in.u8() != 0;
                                     std::optional<Record> record;
                                     if (carried)
                                       record = decode_value(in.remaining() ? std::optional<agree::Bytes>(in.bytes(notice_record_limit)) : std::nullopt);
                                     in.finish();
                                     if (version.size() != version_size) return dannan::net::Handled::failed(400, "bad_notice", "a version is 40 bytes");
                                     if (!carried) return dannan::net::Handled::processed();
                                     if (!record) return dannan::net::Handled::failed(400, "bad_notice", "the record cannot be read");
                                     Committed committed;
                                     {
                                       dannan::contract::Lock lock(hook->mutex);
                                       committed = hook->committed;
                                     }
                                     if (committed) committed(db, key, version, *record);
                                     return dannan::net::Handled::processed();
                                   } catch (const dannan::net::DecodeError&) {
                                     return dannan::net::Handled::failed(400, "bad_notice", "the notice cannot be read");
                                   }
                                 });
}

Agreements::~Agreements() {
  _space.context().routes().unbind(commit_notice);
  dannan::contract::Lock lock(_hook->mutex);
  _hook->committed = nullptr;
}

void Agreements::on_committed(Committed committed) {
  dannan::contract::Lock lock(_hook->mutex);
  _hook->committed = std::move(committed);
}

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
      [this, db, key, done = std::move(done)](agree::Outcome outcome) {
        Outcome out;
        out.committed = outcome.committed;
        out.unknown = outcome.unknown;
        out.why = outcome.why;
        if (outcome.committed) {
          const auto record = decode_value(outcome.values.empty() ? std::nullopt : outcome.values[0]);
          if (record) out.record = *record;
          out.result.assign(outcome.result.begin(), outcome.result.end());
          out.version = version_of(outcome.ballot);
          notify(db, key, out.version, out.record);
        }
        done(std::move(out));
      });
}

void Agreements::read(size_t db, const std::string& key, std::function<void(Outcome)> done) {
  auto& agreement = _space.context().agreement();
  const auto& genesis = agreement.table().genesis;
  agreement.read(std::string(space), shard_of(key, _space.shards()), {name_of(static_cast<uint16_t>(db), key)}, genesis.nodes, genesis.version, {}, _deadline,
                 _space.context().time().executor(), [done = std::move(done)](agree::Outcome outcome) {
                   Outcome out;
                   out.committed = outcome.committed;
                   out.why = outcome.why;
                   if (outcome.committed) {
                     const auto record = decode_value(outcome.values.empty() ? std::nullopt : outcome.values[0]);
                     if (record) {
                       out.record = *record;
                     } else {
                       out.committed = false;
                       out.why = "the key's stored record cannot be read";
                     }
                     out.version = version_of(outcome.ballot);
                   }
                   done(std::move(out));
                 });
}

void Agreements::notify(size_t db, const std::string& key, const std::string& version, const Record& record) {
  auto& context = _space.context();
  const auto& replicas = context.agreement().table().genesis.nodes;
  const auto self = context.identity().id();
  const auto bytes = encode(record);
  const bool carried = !bytes || bytes->size() <= notice_record_limit;
  dannan::net::Writer out;
  out.u16(static_cast<uint16_t>(db));
  out.bytes(bytes_of(key));
  out.bytes(bytes_of(version));
  out.u8(carried ? 1 : 0);
  if (carried && bytes) out.bytes(*bytes);
  const auto payload = out.take();
  for (const auto& peer : context.nodes().active()) {
    if (peer.id == self || std::find(replicas.begin(), replicas.end(), peer.id) == replicas.end()) continue;
    // Best effort: a notice lost leaves that replica's view stale (ruling 5).
    (void)context.routes().call(peer, commit_notice, payload, dannan::sched::FrameType::control, std::chrono::seconds(2), context.time().executor(),
                                [](dannan::net::CallOutcome) {});
  }
}

}  // namespace mimir::cluster
