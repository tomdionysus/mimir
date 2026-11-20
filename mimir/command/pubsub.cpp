//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/command/pubsub.h"

#include "mimir/keyspace/glob.h"

namespace mimir::command {

PubSub::Table& PubSub::table(Kind kind) {
  switch (kind) {
    case Kind::channel:
      return _channels;
    case Kind::pattern:
      return _patterns;
    case Kind::shard:
      return _shard;
  }
  return _channels;
}

std::string PubSub::message(resp::Protocol protocol, std::initializer_list<std::string_view> parts) {
  resp::Reply reply(protocol);
  reply.push(parts.size());
  for (auto part : parts) reply.bulk(part);
  return reply.take();
}

namespace {
std::string confirmation(resp::Protocol protocol, std::string_view verb, const std::string* name, size_t count) {
  resp::Reply reply(protocol);
  reply.push(3);
  reply.bulk(verb);
  if (name)
    reply.bulk(*name);
  else
    reply.null();
  reply.integer(static_cast<int64_t>(count));
  return reply.take();
}
}  // namespace

void PubSub::subscribe(Kind kind, int64_t id, const std::string& name, const Push& push, resp::Protocol protocol, std::string_view verb, size_t& count) {
  dannan::contract::Lock lock(_mutex);
  auto& subscribers = table(kind)[name];
  const bool first = subscribers.empty();
  if (subscribers.emplace(id, Subscriber{push, protocol}).second) ++count;
  if (first && _interest) _interest(kind, name, true);
  push(confirmation(protocol, verb, &name, count));
}

void PubSub::unsubscribe(Kind kind, int64_t id, const std::optional<std::string>& name, const Push& push, resp::Protocol protocol, std::string_view verb,
                         size_t& count) {
  dannan::contract::Lock lock(_mutex);
  if (name) {
    auto& t = table(kind);
    auto found = t.find(*name);
    if (found != t.end()) {
      if (found->second.erase(id) > 0 && count > 0) --count;
      if (found->second.empty()) {
        t.erase(found);
        if (_interest) _interest(kind, *name, false);
      }
    }
  }
  push(confirmation(protocol, verb, name ? &*name : nullptr, count));
}

std::vector<std::string> PubSub::names_of(Kind kind, int64_t id) {
  dannan::contract::Lock lock(_mutex);
  std::vector<std::string> out;
  for (const auto& [name, subscribers] : table(kind))
    if (subscribers.count(id)) out.push_back(name);
  return out;
}

size_t PubSub::count_of(Kind kind, int64_t id) {
  dannan::contract::Lock lock(_mutex);
  size_t count = 0;
  for (const auto& [name, subscribers] : table(kind)) count += subscribers.count(id);
  return count;
}

size_t PubSub::count_of(int64_t id) { return count_of(Kind::channel, id) + count_of(Kind::pattern, id) + count_of(Kind::shard, id); }

void PubSub::set_protocol(int64_t id, resp::Protocol protocol) {
  dannan::contract::Lock lock(_mutex);
  for (Table* t : {&_channels, &_patterns, &_shard})
    for (auto& [name, subscribers] : *t) {
      auto found = subscribers.find(id);
      if (found != subscribers.end()) found->second.protocol = protocol;
    }
}

void PubSub::drop(int64_t id) {
  dannan::contract::Lock lock(_mutex);
  for (const Kind kind : {Kind::channel, Kind::pattern, Kind::shard}) {
    Table& t = table(kind);
    for (auto it = t.begin(); it != t.end();) {
      it->second.erase(id);
      if (!it->second.empty()) {
        ++it;
        continue;
      }
      if (_interest) _interest(kind, it->first, false);
      it = t.erase(it);
    }
  }
}

void PubSub::set_interest(Interest interest) {
  dannan::contract::Lock lock(_mutex);
  _interest = std::move(interest);
}

void PubSub::set_prepare(Prepare prepare) {
  dannan::contract::Lock lock(_mutex);
  _prepare = std::move(prepare);
}

bool PubSub::prepare(Kind kind, const std::vector<std::string>& names, std::function<void()> done) {
  Prepare prepare;
  {
    dannan::contract::Lock lock(_mutex);
    prepare = _prepare;
  }
  // Asked outside the hub's lock: the fan-out takes its own.
  return !prepare || prepare(kind, names, std::move(done));
}

size_t PubSub::publish(const std::string& channel, const std::string& text) {
  Fanout fanout;
  size_t received = 0;
  {
    dannan::contract::Lock lock(_mutex);
    received = deliver_locked(Kind::channel, channel, text);
    fanout = _fanout;
  }
  if (fanout) fanout(Kind::channel, channel, text);
  return received;
}

size_t PubSub::spublish(const std::string& channel, const std::string& text) {
  Fanout fanout;
  size_t received = 0;
  {
    dannan::contract::Lock lock(_mutex);
    received = deliver_locked(Kind::shard, channel, text);
    fanout = _fanout;
  }
  if (fanout) fanout(Kind::shard, channel, text);
  return received;
}

size_t PubSub::deliver(Kind kind, const std::string& channel, const std::string& text) {
  dannan::contract::Lock lock(_mutex);
  return deliver_locked(kind, channel, text);
}

void PubSub::set_fanout(Fanout fanout) {
  dannan::contract::Lock lock(_mutex);
  _fanout = std::move(fanout);
}

size_t PubSub::deliver_locked(Kind kind, const std::string& channel, const std::string& text) {
  size_t received = 0;
  if (kind == Kind::shard) {
    auto found = _shard.find(channel);
    if (found == _shard.end()) return 0;
    for (const auto& [id, subscriber] : found->second) subscriber.push(message(subscriber.protocol, {"smessage", channel, text}));
    return found->second.size();
  }
  auto found = _channels.find(channel);
  if (found != _channels.end()) {
    for (const auto& [id, subscriber] : found->second) {
      subscriber.push(message(subscriber.protocol, {"message", channel, text}));
      ++received;
    }
  }
  for (const auto& [pattern, subscribers] : _patterns) {
    if (!keyspace::glob_match(pattern, channel)) continue;
    for (const auto& [id, subscriber] : subscribers) {
      subscriber.push(message(subscriber.protocol, {"pmessage", pattern, channel, text}));
      ++received;
    }
  }
  return received;
}

std::vector<std::string> PubSub::channels(Kind kind, const std::optional<std::string>& pattern) {
  dannan::contract::Lock lock(_mutex);
  std::vector<std::string> out;
  for (const auto& [name, subscribers] : table(kind))
    if (!pattern || keyspace::glob_match(*pattern, name)) out.push_back(name);
  return out;
}

size_t PubSub::subscribers(Kind kind, const std::string& name) {
  dannan::contract::Lock lock(_mutex);
  auto& t = table(kind);
  auto found = t.find(name);
  return found == t.end() ? 0 : found->second.size();
}

size_t PubSub::patterns() {
  dannan::contract::Lock lock(_mutex);
  return _patterns.size();
}

}  // namespace mimir::command
