//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <map>
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
  void interest(command::PubSub::Kind kind, const std::string& name, bool held);
  void resubscribe_locked() DANNAN_REQUIRES(_mutex);

  dannan::Context& _context;
  command::PubSub& _pubsub;
  dannan::identity::NodeId _self;
  // Lock order: the hub's lock, then this.
  dannan::contract::Mutex _mutex;
  // How many of this node's subscribed names need each prefix, and the
  // channel subscriptions made for the minimal set of them.
  std::map<std::string, size_t> _wanted DANNAN_GUARDED_BY(_mutex);
  std::map<std::string, uint64_t> _subscribed DANNAN_GUARDED_BY(_mutex);
};

}  // namespace mimir::cluster
