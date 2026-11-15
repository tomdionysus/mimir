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
  if (table(kind)[name].emplace(id, Subscriber{push, protocol}).second) ++count;
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
      if (found->second.empty()) t.erase(found);
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
  for (Table* t : {&_channels, &_patterns, &_shard}) {
    for (auto it = t->begin(); it != t->end();) {
      it->second.erase(id);
      it = it->second.empty() ? t->erase(it) : std::next(it);
    }
  }
}

size_t PubSub::publish(const std::string& channel, const std::string& text) {
  dannan::contract::Lock lock(_mutex);
  size_t received = 0;
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

size_t PubSub::spublish(const std::string& channel, const std::string& text) {
  dannan::contract::Lock lock(_mutex);
  auto found = _shard.find(channel);
  if (found == _shard.end()) return 0;
  for (const auto& [id, subscriber] : found->second) subscriber.push(message(subscriber.protocol, {"smessage", channel, text}));
  return found->second.size();
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
