//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/cluster/fanout.h"

#include <string>

namespace mimir::cluster {

namespace {

// A message's topic: its kind, then the channel it was published on.
std::string topic_of(command::PubSub::Kind kind, const std::string& name) { return (kind == command::PubSub::Kind::shard ? "s:" : "c:") + name; }

}  // namespace

Fanout::Fanout(dannan::Context& context, command::PubSub& pubsub) : _context(context), _pubsub(pubsub) {
  _context.channels().open(channel, dannan::net::Delivery::notify);
  const auto self = _context.identity().id();
  _subscription = _context.channels().subscribe(channel, "", _context.time().executor(), [this, self](dannan::net::ChannelMessage message) {
    // This node's own publications were delivered here when made.
    if (message.origin == self || message.topic.size() < 2) return;
    const auto kind = message.topic[0] == 's' ? command::PubSub::Kind::shard : command::PubSub::Kind::channel;
    _pubsub.deliver(kind, message.topic.substr(2), std::string(message.payload.begin(), message.payload.end()));
  });
  _pubsub.set_fanout([this](command::PubSub::Kind kind, const std::string& name, const std::string& text) {
    (void)_context.channels().publish(channel, topic_of(kind, name), dannan::net::Bytes(text.begin(), text.end()));
  });
}

Fanout::~Fanout() {
  _pubsub.set_fanout(nullptr);
  _context.channels().unsubscribe(_subscription);
}

}  // namespace mimir::cluster
