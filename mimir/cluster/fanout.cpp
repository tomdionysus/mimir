//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/cluster/fanout.h"

#include <chrono>
#include <set>
#include <string>

namespace mimir::cluster {

namespace {

// A message's topic: its kind, then the channel it was published on.
std::string topic_of(command::PubSub::Kind kind, const std::string& name) { return (kind == command::PubSub::Kind::shard ? "s:" : "c:") + name; }

}  // namespace

std::string Fanout::prefix_of(command::PubSub::Kind kind, const std::string& name) {
  if (kind != command::PubSub::Kind::pattern) return topic_of(kind, name);
  // A pattern matches channels: those starting with its literal part.
  return "c:" + name.substr(0, std::min(name.find_first_of("*?[\\"), name.size()));
}

std::vector<std::string> Fanout::minimal(const std::map<std::string, size_t>& wanted) {
  // In sorted order a covering prefix comes before every prefix it covers.
  std::vector<std::string> out;
  for (const auto& [prefix, count] : wanted) {
    if (count == 0) continue;
    if (!out.empty() && prefix.compare(0, out.back().size(), out.back()) == 0) continue;
    out.push_back(prefix);
  }
  return out;
}

Fanout::Fanout(dannan::Context& context, command::PubSub& pubsub) : _context(context), _pubsub(pubsub), _self(context.identity().id()) {
  _context.channels().open(channel, dannan::net::Delivery::notify);
  _pubsub.set_interest([this](command::PubSub::Kind kind, const std::string& name, bool held) { interest(kind, name, held); });
  _pubsub.set_fanout([this](command::PubSub::Kind kind, const std::string& name, const std::string& text) {
    (void)_context.channels().publish(channel, topic_of(kind, name), dannan::net::Bytes(text.begin(), text.end()));
  });
}

Fanout::~Fanout() {
  _pubsub.set_fanout(nullptr);
  _pubsub.set_interest(nullptr);
  dannan::contract::Lock lock(_mutex);
  for (const auto& [prefix, id] : _subscribed) _context.channels().unsubscribe(id);
  _subscribed.clear();
}

void Fanout::interest(command::PubSub::Kind kind, const std::string& name, bool held) {
  dannan::contract::Lock lock(_mutex);
  const std::string prefix = prefix_of(kind, name);
  if (held) {
    ++_wanted[prefix];
  } else if (auto found = _wanted.find(prefix); found != _wanted.end() && --found->second == 0) {
    _wanted.erase(found);
  }
  resubscribe_locked();
}

void Fanout::resubscribe_locked() {
  const auto wanted = minimal(_wanted);
  const std::set<std::string> keep(wanted.begin(), wanted.end());
  // Those no longer wanted go first, so no topic is ever matched twice.
  for (auto it = _subscribed.begin(); it != _subscribed.end();) {
    if (keep.count(it->first)) {
      ++it;
      continue;
    }
    _context.channels().unsubscribe(it->second);
    it = _subscribed.erase(it);
  }
  for (const auto& prefix : wanted) {
    if (_subscribed.count(prefix)) continue;
    const auto self = _self;
    // The interest is pushed to every active peer at once, rather than
    // waiting for gossip, so a publish soon after reaches this subscriber.
    _subscribed[prefix] = _context.channels().subscribe(
        channel, prefix, _context.time().executor(),
        [this, self](dannan::net::ChannelMessage message) {
          // This node's own publications were delivered here when made.
          if (message.origin == self || message.topic.size() < 2) return;
          const auto kind = message.topic[0] == 's' ? command::PubSub::Kind::shard : command::PubSub::Kind::channel;
          _pubsub.deliver(kind, message.topic.substr(2), std::string(message.payload.begin(), message.payload.end()));
        },
        std::chrono::seconds(2), _context.time().executor(), [](dannan::membership::Pushed) {});
  }
}

}  // namespace mimir::cluster
