//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dannan/contract/thread_safety.h"
#include "mimir/resp/reply.h"

// Publish and subscribe on one node: who listens to which channel, pattern
// and shard channel, and the delivery of a published message to each, at most
// once, as Valkey's pub/sub promises. A message is pushed through each
// subscriber's own sender, which queues it on that connection without waiting
// (Dannan's Stream::send), so a publisher never waits on a slow subscriber;
// a subscriber too slow to keep up is closed by its stream's outbound bound.
// In the cluster stage a message also crosses Dannan's notify channels to the
// nodes with announced interest; here it reaches this node's subscribers.
namespace mimir::command {

class PubSub {
 public:
  using Push = std::function<void(std::string bytes)>;
  enum class Kind : uint8_t { channel, pattern, shard };

  // Subscribes connection `id` to `name`, counts it in `count` when new, and
  // pushes the confirmation [verb, name, count], all under the hub's lock, so
  // no message on `name` can reach the connection before its confirmation.
  // `push` and `protocol` say how it is told. Thread-safe.
  void subscribe(Kind kind, int64_t id, const std::string& name, const Push& push, resp::Protocol protocol, std::string_view verb, size_t& count);
  // Ends it, and pushes [verb, name, count] the same way; a null name when
  // `name` is none (an UNSUBSCRIBE of nothing).
  void unsubscribe(Kind kind, int64_t id, const std::optional<std::string>& name, const Push& push, resp::Protocol protocol, std::string_view verb,
                   size_t& count);
  // The names connection `id` holds of `kind`, sorted.
  std::vector<std::string> names_of(Kind kind, int64_t id);
  // How many subscriptions of every kind connection `id` holds.
  size_t count_of(int64_t id);
  size_t count_of(Kind kind, int64_t id);
  // A connection's protocol changed (HELLO).
  void set_protocol(int64_t id, resp::Protocol protocol);
  // Ends every subscription of connection `id` (its close, RESET).
  void drop(int64_t id);

  // Delivers `message` on `channel` to its subscribers and the patterns that
  // match it; how many received it. Waits on nothing but the hub's lock.
  size_t publish(const std::string& channel, const std::string& message);
  size_t spublish(const std::string& channel, const std::string& message);

  // The cluster's fan-out (stage 3): told, once the lock is let go, of every
  // message published here (keyspace notifications included), to carry it to
  // the other nodes. Set once, before the first publish.
  using Fanout = std::function<void(Kind kind, const std::string& channel, const std::string& message)>;
  void set_fanout(Fanout fanout);
  // The cluster's interest: told, under the hub's lock, when a name of a kind
  // gains its first subscriber here (`held`) or loses its last, so the node
  // announces only what its own subscribers want. Set once, before the first
  // subscription; it must not call back into the hub.
  using Interest = std::function<void(Kind kind, const std::string& name, bool held)>;
  void set_interest(Interest interest);
  // The cluster's routing of a subscription: true when every other node
  // already routes `names` of `kind` here. Otherwise the fan-out starts
  // routing them and calls `done` once, later, on its own thread and never
  // under the hub's lock, when the other nodes acknowledge it or its deadline
  // passes. With no fan-out every name is routed. Set once, before the first
  // subscription. Thread-safe.
  using Prepare = std::function<bool(Kind kind, const std::vector<std::string>& names, std::function<void()> done)>;
  void set_prepare(Prepare prepare);
  bool prepare(Kind kind, const std::vector<std::string>& names, std::function<void()> done);
  // A message published on another node: delivered here, never carried on.
  size_t deliver(Kind kind, const std::string& channel, const std::string& message);

  // PUBSUB's answers.
  std::vector<std::string> channels(Kind kind, const std::optional<std::string>& pattern);
  size_t subscribers(Kind kind, const std::string& name);
  size_t patterns();

 private:
  struct Subscriber {
    Push push;
    resp::Protocol protocol;
  };
  using Table = std::map<std::string, std::map<int64_t, Subscriber>>;
  Table& table(Kind kind) DANNAN_REQUIRES(_mutex);
  size_t deliver_locked(Kind kind, const std::string& channel, const std::string& message) DANNAN_REQUIRES(_mutex);
  static std::string message(resp::Protocol protocol, std::initializer_list<std::string_view> parts);

  dannan::contract::Mutex _mutex;
  Table _channels DANNAN_GUARDED_BY(_mutex);
  Table _patterns DANNAN_GUARDED_BY(_mutex);
  Table _shard DANNAN_GUARDED_BY(_mutex);
  Fanout _fanout DANNAN_GUARDED_BY(_mutex);
  Interest _interest DANNAN_GUARDED_BY(_mutex);
  Prepare _prepare DANNAN_GUARDED_BY(_mutex);
};

}  // namespace mimir::command
