//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <memory>

#include "dannan/context.h"
#include "mimir/command/pubsub.h"

// Pub/sub across the cluster (the design's "Pub/sub", agreed with Dannan and
// Lir): a message published on one node, PUBLISH's or SPUBLISH's or a keyspace
// notification's, rides Dannan's notify channel `mimir.pubsub` to the other
// nodes, at most once and in no order across nodes, and each delivers it to
// its own subscribers without carrying it on. Each node announces interest in
// every topic for now; per-channel interest is a refinement.
namespace mimir::cluster {

class Fanout {
 public:
  static constexpr const char* channel = "mimir.pubsub";

  // Opens the channel, subscribes this node, and hands `pubsub` its fan-out.
  Fanout(dannan::Context& context, command::PubSub& pubsub);
  ~Fanout();
  Fanout(const Fanout&) = delete;
  Fanout& operator=(const Fanout&) = delete;

 private:
  dannan::Context& _context;
  command::PubSub& _pubsub;
  uint64_t _subscription = 0;
};

}  // namespace mimir::cluster
