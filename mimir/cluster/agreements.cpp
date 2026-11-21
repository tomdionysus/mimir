//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/cluster/agreements.h"

#include <algorithm>
#include <optional>
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
  dannan::net::Registration notice;
  notice.name = "mimir.commit_notice";
  context.routes().add(commit_notice, notice);
  dannan::net::Registration names;
  names.name = "mimir.shard_names";
  names.lanes = dannan::net::Lanes::both;
  names.frames = dannan::net::Frames::any;
  context.routes().add(shard_names, names);
}

Agreements::Agreements(Space& served, std::chrono::milliseconds deadline, size_t page)
    : _space(served), _deadline(deadline), _page(std::clamp<size_t>(page, 1, shard_names_page)) {
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
  // A restarted peer's catch-up: a page of the names this node holds in a
  // shard, after the last name the peer was given. A page resumes from a name,
  // not an index, so a name removed between pages cannot shift one past the
  // peer. A node catching up itself has none to give.
  _space.context().routes().bind(shard_names, [this](const dannan::membership::NodeRecord&, dannan::sched::FrameType, const dannan::net::Message& request) {
    try {
      dannan::net::Reader in(request.payload);
      const uint32_t slot = in.u32();
      std::optional<agree::Name> after;
      if (in.u8() != 0) after = in.bytes(max_key + 2);
      in.finish();
      if (!_space.store().ready()) return dannan::net::Handled::failed(503, "not_ready", "this node is catching up itself");
      // One past a page: whether there are more.
      std::vector<agree::Name> given;
      _space.store().each_held(slot, after, [this, &given](const agree::Name& name, const agree::AcceptorState&) {
        given.push_back(name);
        return given.size() <= _page;
      });
      const bool more = given.size() > _page;
      if (more) given.pop_back();
      dannan::net::Writer out;
      out.u8(more ? 1 : 0);
      out.u32(static_cast<uint32_t>(given.size()));
      for (const auto& name : given) out.bytes(name);
      return dannan::net::Handled::processed(dannan::net::Message{dannan::net::message::ok, out.take(), {}});
    } catch (const dannan::net::DecodeError&) {
      return dannan::net::Handled::failed(400, "bad_request", "the request cannot be read");
    }
  });
}

struct Agreements::Recovery {
  std::function<void()> done;
  uint32_t shard = 0;
  // The genesis set but this node, and which of them is asked now.
  std::vector<dannan::identity::NodeId> peers;
  size_t peer = 0;
  std::vector<agree::Name> names;
};

void Agreements::recover(std::function<void()> done) {
  auto recovery = std::make_shared<Recovery>();
  recovery->done = std::move(done);
  const auto self = _space.context().identity().id();
  for (const auto& node : _space.context().agreement().table().genesis.nodes)
    if (node != self) recovery->peers.push_back(node);
  if (recovery->peers.empty()) return finish_recovery(recovery);
  recover_shard(std::move(recovery));
}

void Agreements::recover_shard(std::shared_ptr<Recovery> recovery) {
  if (recovery->shard >= _space.shards()) return finish_recovery(recovery);
  recovery->names.clear();
  gather_names(std::move(recovery), 0);
}

void Agreements::gather_names(std::shared_ptr<Recovery> recovery, size_t asked) {
  auto& context = _space.context();
  const auto& peer = recovery->peers[(recovery->peer + asked) % recovery->peers.size()];
  std::optional<dannan::membership::NodeRecord> record;
  for (auto& node : context.nodes().active())
    if (node.id == peer) record = std::move(node);
  const auto retry = [this, recovery] {
    // Another peer, a second later.
    ++recovery->peer;
    _space.context().time().after(std::chrono::seconds(1), [this, recovery] { recover_shard(recovery); });
  };
  if (!record) return retry();
  dannan::net::Writer out;
  out.u32(recovery->shard);
  // After the last name gathered, if any.
  out.u8(recovery->names.empty() ? 0 : 1);
  if (!recovery->names.empty()) out.bytes(recovery->names.back());
  (void)context.routes().call(*record, shard_names, out.take(), dannan::sched::FrameType::control, std::chrono::seconds(5), context.time().executor(),
                              [this, recovery, asked, retry](dannan::net::CallOutcome outcome) {
                                if (!outcome.reply || dannan::net::refusal_of(*outcome.reply)) return retry();
                                try {
                                  dannan::net::Reader in(outcome.reply->payload);
                                  const bool more = in.u8() != 0;
                                  const uint32_t count = in.u32();
                                  if (count > shard_names_page) return retry();
                                  for (uint32_t i = 0; i < count; ++i) recovery->names.push_back(in.bytes(max_key + 2));
                                  in.finish();
                                  if (more) return gather_names(recovery, asked);
                                } catch (const dannan::net::DecodeError&) {
                                  return retry();
                                }
                                // The names are known: read at a majority of the others and kept here.
                                auto& agreement = _space.context().agreement();
                                const auto& genesis = agreement.table().genesis;
                                agreement.catch_up(std::string(space), recovery->shard, recovery->names, genesis.nodes, genesis.version,
                                                   std::chrono::seconds(10), _space.context().time().executor(),
                                                   [this, recovery, retry](agree::Outcome caught) {
                                                     if (!caught.committed) return retry();
                                                     ++recovery->shard;
                                                     recover_shard(recovery);
                                                   });
                              });
}

void Agreements::finish_recovery(const std::shared_ptr<Recovery>& recovery) {
  // This node's view, from what it now holds; then it votes.
  Committed committed;
  {
    dannan::contract::Lock lock(_hook->mutex);
    committed = _hook->committed;
  }
  for (uint32_t shard = 0; shard < _space.shards() && committed; ++shard) {
    // Copied out under the store's lock, applied after it: the view's hook
    // takes the database's lock, never inside the store's.
    std::vector<std::pair<agree::Name, agree::AcceptorState>> held;
    _space.store().each_held(shard, std::nullopt, [&held](const agree::Name& name, const agree::AcceptorState& state) {
      held.emplace_back(name, state);
      return true;
    });
    for (const auto& [name, state] : held) {
      const auto parsed = parse_name(name);
      const auto record = decode_value(state.value);
      if (parsed && record) committed(parsed->first, parsed->second, version_of(state.accepted), *record);
    }
  }
  _space.store().set_ready();
  if (recovery->done) _space.context().time().after(std::chrono::milliseconds(0), recovery->done);
}

Agreements::~Agreements() {
  _space.context().routes().unbind(shard_names);
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

void Agreements::change_keys(std::vector<Name>, ChangeKeys, std::function<void(KeysOutcome)> done) {
  KeysOutcome refused;
  refused.why = "a change of several keys waits for Dannan's atomic commit across replica sets (ruling 35)";
  _space.context().time().after(std::chrono::milliseconds(0), [done = std::move(done), refused] { done(refused); });
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
