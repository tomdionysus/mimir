//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "dannan/context.h"
#include "dannan/contract/thread_safety.h"
#include "mimir/command/pubsub.h"

// Pub/sub across the cluster (the design's "Pub/sub", agreed with Dannan and
// Lir): a message published on one node, PUBLISH's or SPUBLISH's or a keyspace
// notification's, rides Dannan's notify channel `mimir.pubsub` to the nodes
// whose subscribers want it, at most once and in no order across nodes, and
// each delivers it to its own subscribers without carrying it on.
//
// A node subscribes on the channel by topic prefix, and Dannan gossips the
// prefixes so a publish goes only where they match. A topic is "c:" or "s:"
// and the channel's name. A channel's prefix is its topic; a pattern's is
// "c:" and its literal part before the first glob character, so "news.*" is
// "c:news." and "*" is every channel. A prefix may match more than is
// wanted; each node's hub matches exactly.
//
// A subscription's interest is pushed to every active peer, and a SUBSCRIBE
// is confirmed only once they acknowledge it (or the push's deadline
// passes), as Lir holds SUBACK: so a PUBLISH made after the confirmation, on
// any node, reaches the subscriber, as Valkey promises.
namespace mimir::cluster {

class Fanout {
 public:
  static constexpr const char* channel = "mimir.pubsub";

  // Opens the channel, and hands `pubsub` its fan-out and its interest.
  Fanout(dannan::Context& context, command::PubSub& pubsub);
  ~Fanout();
  Fanout(const Fanout&) = delete;
  Fanout& operator=(const Fanout&) = delete;

  // The topic prefix a subscription of `kind` to `name` needs.
  static std::string prefix_of(command::PubSub::Kind kind, const std::string& name);
  // The prefixes of `wanted` that no other of them covers: a topic matches at
  // most one, so no message is delivered twice.
  static std::vector<std::string> minimal(const std::map<std::string, size_t>& wanted);

 private:
  // What a push's acknowledgement reaches: this fan-out while it lives.
  struct Alive {
    dannan::contract::Mutex mutex;
    Fanout* fanout DANNAN_GUARDED_BY(mutex) = nullptr;
  };
  // A SUBSCRIBE held until its prefixes are routed here.
  struct Waiting {
    std::vector<std::string> prefixes;
    std::function<void()> done;
  };

  void interest(command::PubSub::Kind kind, const std::string& name, bool held);
  bool prepare(command::PubSub::Kind kind, const std::vector<std::string>& names, std::function<void()> done);
  // The subscription `id` holds is acknowledged: the SUBSCRIBEs it completes
  // are told, outside the lock.
  void pushed(const std::shared_ptr<uint64_t>& id);
  void resubscribe_locked() DANNAN_REQUIRES(_mutex);
  // Whether a subscription acknowledged by the peers covers `prefix`.
  bool routed_locked(const std::string& prefix) DANNAN_REQUIRES(_mutex);

  dannan::Context& _context;
  command::PubSub& _pubsub;
  dannan::identity::NodeId _self;
  std::shared_ptr<Alive> _alive = std::make_shared<Alive>();
  // Lock order: the database's lock, then the hub's, then this. A push's
  // acknowledgement takes Alive's lock, then this.
  dannan::contract::Mutex _mutex;
  // How many of this node's subscribed names need each prefix, and the
  // channel subscriptions made for the minimal set of them.
  std::map<std::string, size_t> _wanted DANNAN_GUARDED_BY(_mutex);
  std::map<std::string, uint64_t> _subscribed DANNAN_GUARDED_BY(_mutex);
  // The subscriptions whose interest every peer has acknowledged (or whose
  // push's deadline passed).
  std::set<uint64_t> _pushed DANNAN_GUARDED_BY(_mutex);
  // Prefixes routed ahead of their subscriber, for a held SUBSCRIBE: kept
  // until it subscribes, or for a while after it was told (a connection that
  // went meanwhile never does). time_point::max() while it waits.
  std::map<std::string, std::chrono::steady_clock::time_point> _warm DANNAN_GUARDED_BY(_mutex);
  std::vector<Waiting> _waiting DANNAN_GUARDED_BY(_mutex);
};

}  // namespace mimir::cluster
