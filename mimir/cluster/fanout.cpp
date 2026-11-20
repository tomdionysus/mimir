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

// How long a prefix routed ahead of its subscriber is kept once the held
// SUBSCRIBE was told, for it to run again and subscribe.
constexpr auto warm_for = std::chrono::seconds(30);

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
  {
    dannan::contract::Lock lock(_alive->mutex);
    _alive->fanout = this;
  }
  _context.channels().open(channel, dannan::net::Delivery::notify);
  _pubsub.set_interest([this](command::PubSub::Kind kind, const std::string& name, bool held) { interest(kind, name, held); });
  _pubsub.set_prepare(
      [this](command::PubSub::Kind kind, const std::vector<std::string>& names, std::function<void()> done) { return prepare(kind, names, std::move(done)); });
  _pubsub.set_fanout([this](command::PubSub::Kind kind, const std::string& name, const std::string& text) {
    (void)_context.channels().publish(channel, topic_of(kind, name), dannan::net::Bytes(text.begin(), text.end()));
  });
}

Fanout::~Fanout() {
  {
    // An acknowledgement still on its way finds nothing.
    dannan::contract::Lock lock(_alive->mutex);
    _alive->fanout = nullptr;
  }
  _pubsub.set_fanout(nullptr);
  _pubsub.set_prepare(nullptr);
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
    _warm.erase(prefix);
  } else if (auto found = _wanted.find(prefix); found != _wanted.end() && --found->second == 0) {
    _wanted.erase(found);
  }
  resubscribe_locked();
}

bool Fanout::prepare(command::PubSub::Kind kind, const std::vector<std::string>& names, std::function<void()> done) {
  dannan::contract::Lock lock(_mutex);
  const auto now = std::chrono::steady_clock::now();
  // Prefixes routed ahead for a SUBSCRIBE that never came back go.
  bool expired = false;
  for (auto it = _warm.begin(); it != _warm.end();) {
    if (it->second != std::chrono::steady_clock::time_point::max() && now - it->second > warm_for) {
      it = _warm.erase(it);
      expired = true;
    } else {
      ++it;
    }
  }
  std::vector<std::string> needed;
  for (const auto& name : names) {
    const auto prefix = prefix_of(kind, name);
    if (!routed_locked(prefix)) needed.push_back(prefix);
  }
  if (needed.empty()) {
    if (expired) resubscribe_locked();
    return true;
  }
  for (const auto& prefix : needed) _warm[prefix] = std::chrono::steady_clock::time_point::max();
  resubscribe_locked();
  _waiting.push_back(Waiting{std::move(needed), std::move(done)});
  return false;
}

void Fanout::pushed(const std::shared_ptr<uint64_t>& id) {
  std::vector<std::function<void()>> ready;
  {
    dannan::contract::Lock lock(_mutex);
    // Only a subscription still held: one dropped meanwhile routes nothing.
    bool held = false;
    for (const auto& [prefix, subscription] : _subscribed)
      if (subscription == *id) held = true;
    if (!held) return;
    _pushed.insert(*id);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = _waiting.begin(); it != _waiting.end();) {
      bool all = true;
      for (const auto& prefix : it->prefixes)
        if (!routed_locked(prefix)) all = false;
      if (!all) {
        ++it;
        continue;
      }
      for (const auto& prefix : it->prefixes)
        if (auto warm = _warm.find(prefix); warm != _warm.end()) warm->second = now;
      ready.push_back(std::move(it->done));
      it = _waiting.erase(it);
    }
  }
  for (auto& done : ready) done();
}

bool Fanout::routed_locked(const std::string& prefix) {
  // The minimal set holds at most one subscription covering a prefix.
  for (const auto& [subscribed, id] : _subscribed)
    if (prefix.compare(0, subscribed.size(), subscribed) == 0) return _pushed.count(id) > 0;
  return false;
}

void Fanout::resubscribe_locked() {
  auto wanting = _wanted;
  for (const auto& [prefix, since] : _warm) ++wanting[prefix];
  const auto wanted = minimal(wanting);
  const std::set<std::string> keep(wanted.begin(), wanted.end());
  // Those no longer wanted go first, so no topic is ever matched twice.
  for (auto it = _subscribed.begin(); it != _subscribed.end();) {
    if (keep.count(it->first)) {
      ++it;
      continue;
    }
    _context.channels().unsubscribe(it->second);
    _pushed.erase(it->second);
    it = _subscribed.erase(it);
  }
  for (const auto& prefix : wanted) {
    if (_subscribed.count(prefix)) continue;
    const auto self = _self;
    // The interest is pushed to every active peer at once, rather than
    // waiting for gossip; its acknowledgement (or deadline) completes the
    // SUBSCRIBEs held for it. The id is set below, under this lock, before
    // the acknowledgement can read it.
    auto id = std::make_shared<uint64_t>(0);
    *id = _context.channels().subscribe(
        channel, prefix, _context.time().executor(),
        [this, self](dannan::net::ChannelMessage message) {
          // This node's own publications were delivered here when made.
          if (message.origin == self || message.topic.size() < 2) return;
          const auto kind = message.topic[0] == 's' ? command::PubSub::Kind::shard : command::PubSub::Kind::channel;
          _pubsub.deliver(kind, message.topic.substr(2), std::string(message.payload.begin(), message.payload.end()));
        },
        std::chrono::seconds(2), _context.time().executor(),
        [alive = _alive, id](dannan::membership::Pushed) {
          dannan::contract::Lock lock(alive->mutex);
          if (alive->fanout) alive->fanout->pushed(id);
        });
    _subscribed[prefix] = *id;
  }
}

}  // namespace mimir::cluster
