//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "mimir/keyspace/zset.h"

#include <vector>

namespace mimir::keyspace {

struct SortedSet::Node {
  Item item;
  uint64_t priority;
  size_t count = 1;
  std::unique_ptr<Node> left;
  std::unique_ptr<Node> right;
};

namespace {

// Whether (score, member) orders before `item`.
bool before(double score, std::string_view member, const SortedSet::Item& item) {
  if (score != item.score) return score < item.score;
  return member < std::string_view(item.member);
}

}  // namespace

size_t SortedSet::size_of(const Node* node) noexcept { return node ? node->count : 0; }

void SortedSet::fix(Node* node) noexcept { node->count = 1 + size_of(node->left.get()) + size_of(node->right.get()); }

std::unique_ptr<SortedSet::Node> SortedSet::merge(std::unique_ptr<Node> left, std::unique_ptr<Node> right) {
  if (!left) return right;
  if (!right) return left;
  if (left->priority > right->priority) {
    left->right = merge(std::move(left->right), std::move(right));
    fix(left.get());
    return left;
  }
  right->left = merge(std::move(left), std::move(right->left));
  fix(right.get());
  return right;
}

void SortedSet::split(std::unique_ptr<Node> node, double score, std::string_view member, std::unique_ptr<Node>& left, std::unique_ptr<Node>& right) {
  if (!node) {
    left.reset();
    right.reset();
    return;
  }
  // Items ordering before (score, member) go left.
  const bool goes_left = node->item.score < score || (node->item.score == score && std::string_view(node->item.member) < member);
  if (goes_left) {
    split(std::move(node->right), score, member, node->right, right);
    fix(node.get());
    left = std::move(node);
  } else {
    split(std::move(node->left), score, member, left, node->left);
    fix(node.get());
    right = std::move(node);
  }
}

std::unique_ptr<SortedSet::Node> SortedSet::copy(const Node* node) {
  if (!node) return nullptr;
  auto made = std::make_unique<Node>(Node{node->item, node->priority, node->count, nullptr, nullptr});
  made->left = copy(node->left.get());
  made->right = copy(node->right.get());
  return made;
}

SortedSet::SortedSet(const SortedSet& other)
    : _root(copy(other._root.get())), _scores(other._scores), _seed(other._seed), _order(other._order), _skiplist(other._skiplist) {}

SortedSet& SortedSet::operator=(const SortedSet& other) {
  if (this == &other) return *this;
  _root = copy(other._root.get());
  _scores = other._scores;
  _seed = other._seed;
  _order = other._order;
  _skiplist = other._skiplist;
  return *this;
}

bool SortedSet::insert_member(std::string_view member, double score, const Limits& limits) {
  const bool created = set(member, score);
  // Only a new member converts: past the entry limit, or longer than the
  // value limit. A score updated never does.
  if (created && !_skiplist && (size() > limits.zset_max_listpack_entries || member.size() > limits.zset_max_listpack_value)) _skiplist = true;
  return created;
}

void SortedSet::settle_new(const Limits& limits) {
  size_t longest = 0;
  for (const auto& [member, entry] : _scores) longest = std::max(longest, member.size());
  _skiplist = size() > limits.zset_max_listpack_entries || longest > limits.zset_max_listpack_value;
}

SortedSet::SortedSet() = default;
SortedSet::SortedSet(SortedSet&&) noexcept = default;
SortedSet& SortedSet::operator=(SortedSet&&) noexcept = default;

SortedSet::~SortedSet() {
  // Iteratively, so a deep tree never exhausts the stack.
  std::vector<std::unique_ptr<Node>> pending;
  if (_root) pending.push_back(std::move(_root));
  while (!pending.empty()) {
    auto node = std::move(pending.back());
    pending.pop_back();
    if (node->left) pending.push_back(std::move(node->left));
    if (node->right) pending.push_back(std::move(node->right));
  }
}

void SortedSet::insert(double score, std::string_view member) {
  // xorshift64: priorities need only be independent of the keys.
  _seed ^= _seed << 13;
  _seed ^= _seed >> 7;
  _seed ^= _seed << 17;
  std::unique_ptr<Node> left, right;
  split(std::move(_root), score, member, left, right);
  auto node = std::make_unique<Node>(Node{Item{score, std::string(member)}, _seed, 1, nullptr, nullptr});
  _root = merge(merge(std::move(left), std::move(node)), std::move(right));
}

void SortedSet::remove(double score, std::string_view member) {
  // Walk to the node, keeping the owning pointer, and merge its children in its place.
  std::unique_ptr<Node>* link = &_root;
  std::vector<Node*> path;
  while (*link) {
    Node* node = link->get();
    if (node->item.score == score && node->item.member == member) break;
    path.push_back(node);
    link = before(score, member, node->item) ? &node->left : &node->right;
  }
  if (!*link) return;
  *link = merge(std::move((*link)->left), std::move((*link)->right));
  for (auto it = path.rbegin(); it != path.rend(); ++it) fix(*it);
}

bool SortedSet::set(std::string_view member, double score) {
  auto found = _scores.find(std::string(member));
  if (found != _scores.end()) {
    if (found->second.score == score) return false;
    remove(found->second.score, member);
    found->second.score = score;
    insert(score, member);
    return false;
  }
  const uint64_t number = _order.next();
  auto [made, inserted] = _scores.emplace(std::string(member), Entry{score, number});
  _order.added(number, &made->first);
  insert(score, member);
  return true;
}

std::optional<double> SortedSet::score(std::string_view member) const {
  auto found = _scores.find(std::string(member));
  if (found == _scores.end()) return std::nullopt;
  return found->second.score;
}

bool SortedSet::erase(std::string_view member) {
  auto found = _scores.find(std::string(member));
  if (found == _scores.end()) return false;
  remove(found->second.score, member);
  _order.removed(found->second.number);
  _scores.erase(found);
  return true;
}

std::pair<std::vector<SortedSet::Item>, uint64_t> SortedSet::page(uint64_t cursor, size_t count) {
  auto [members, next] = _order.page(cursor, count, [this](auto visit) {
    for (const auto& [name, entry] : _scores) visit(entry.number, &name);
  });
  std::vector<Item> items;
  items.reserve(members.size());
  for (const std::string* member : members) items.push_back(Item{_scores.at(*member).score, *member});
  return {std::move(items), next};
}

std::optional<size_t> SortedSet::rank(std::string_view member) const {
  auto found = score(member);
  if (!found) return std::nullopt;
  return count_below(*found, member, false);
}

const SortedSet::Item& SortedSet::at(size_t rank) const {
  const Node* node = _root.get();
  for (;;) {
    const size_t left = size_of(node->left.get());
    if (rank < left) {
      node = node->left.get();
    } else if (rank == left) {
      return node->item;
    } else {
      rank -= left + 1;
      node = node->right.get();
    }
  }
}

size_t SortedSet::count_below(double score, std::string_view member, bool inclusive) const {
  size_t count = 0;
  const Node* node = _root.get();
  while (node) {
    const bool equal = node->item.score == score && node->item.member == member;
    const bool node_below = !equal && !before(score, member, node->item);
    if (node_below || (inclusive && equal)) {
      count += size_of(node->left.get()) + 1;
      node = node->right.get();
    } else {
      node = node->left.get();
    }
  }
  return count;
}

size_t SortedSet::count_below_score(double score, bool inclusive) const {
  size_t count = 0;
  const Node* node = _root.get();
  while (node) {
    const bool below = inclusive ? node->item.score <= score : node->item.score < score;
    if (below) {
      count += size_of(node->left.get()) + 1;
      node = node->right.get();
    } else {
      node = node->left.get();
    }
  }
  return count;
}

void SortedSet::adopt_numbers(const SortedSet& before) {
  _order.resume_from(before._order.reached());
  for (auto& [member, entry] : _scores) {
    const auto found = before._scores.find(member);
    entry.number = found != before._scores.end() ? found->second.number : _order.next();
  }
}

}  // namespace mimir::keyspace
