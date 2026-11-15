//
// Mimir - The Distributed Database
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
//

// The sorted set commands, as Valkey 8 documents them. A sorted set that
// becomes empty is removed, and a store that makes an empty one removes its
// destination.

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>

#include "mimir/command/args.h"
#include "mimir/command/database.h"
#include "mimir/command/handlers.h"
#include "mimir/keyspace/glob.h"

namespace mimir::command::zsets {

namespace {

using keyspace::SortedSet;

constexpr double infinity = std::numeric_limits<double>::infinity();

SortedSet* zset_value(keyspace::Keyspace& keys, std::string_view key) {
  keyspace::Entry* entry = keys.find(key);
  if (!entry) return nullptr;
  if (entry->type() != keyspace::Type::zset) throw CommandError(wrong_type);
  return &std::get<SortedSet>(entry->value);
}

SortedSet& zset_or_create(keyspace::Keyspace& keys, std::string_view key) {
  if (SortedSet* zset = zset_value(keys, key)) return *zset;
  return std::get<SortedSet>(keys.put(key, keyspace::Entry{SortedSet{}, std::nullopt}).value);
}

void drop_if_empty(keyspace::Keyspace& keys, std::string_view key, const SortedSet& zset) {
  if (zset.empty()) keys.erase(key);
}

// A score as Valkey reads one: strtod's whole text, no leading space, not NaN;
// "inf", "+inf" and "-inf" are scores.
std::optional<double> parse_score(std::string_view text) {
  if (text.empty() || std::isspace(static_cast<unsigned char>(text[0]))) return std::nullopt;
  const std::string copy(text);
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(copy.c_str(), &end);
  if (end != copy.c_str() + copy.size() || std::isnan(value)) return std::nullopt;
  if (errno == ERANGE && !std::isinf(value) && value != 0) return std::nullopt;
  return value;
}

double score_argument(std::string_view text) {
  auto value = parse_score(text);
  if (!value) throw CommandError(not_float);
  return *value;
}

// A score range bound: "(" excludes; "-inf" and "+inf" are open ends.
struct ScoreBound {
  double value;
  bool exclusive;
};

ScoreBound score_bound(std::string_view text) {
  ScoreBound bound{0, false};
  if (!text.empty() && text[0] == '(') {
    bound.exclusive = true;
    text.remove_prefix(1);
  }
  auto value = parse_score(text);
  if (!value) throw CommandError("ERR min or max is not a float");
  bound.value = *value;
  return bound;
}

// A lexical range bound: "[" includes, "(" excludes, "-" and "+" the ends.
struct LexBound {
  std::string value;
  bool exclusive = false;
  bool minimum = false;
  bool maximum = false;
};

LexBound lex_bound(std::string_view text) {
  LexBound bound;
  if (text == "-") {
    bound.minimum = true;
  } else if (text == "+") {
    bound.maximum = true;
  } else if (!text.empty() && (text[0] == '[' || text[0] == '(')) {
    bound.exclusive = text[0] == '(';
    bound.value = std::string(text.substr(1));
  } else {
    throw CommandError("ERR min or max not valid string range item");
  }
  return bound;
}

// [first, last) in rank order of the items within a score range.
std::pair<size_t, size_t> ranks_in_scores(const SortedSet& zset, const ScoreBound& min, const ScoreBound& max) {
  const size_t first = zset.count_below_score(min.value, min.exclusive);
  const size_t last = zset.count_below_score(max.value, !max.exclusive);
  return {first, std::max(first, last)};
}

bool lex_above_min(std::string_view member, const LexBound& min) {
  if (min.minimum) return true;
  if (min.maximum) return false;
  return min.exclusive ? member > std::string_view(min.value) : member >= std::string_view(min.value);
}

bool lex_below_max(std::string_view member, const LexBound& max) {
  if (max.maximum) return true;
  if (max.minimum) return false;
  return max.exclusive ? member < std::string_view(max.value) : member <= std::string_view(max.value);
}

// [first, last) of the items within a lexical range: Valkey assumes equal
// scores, and so does this, scanning by rank.
std::pair<size_t, size_t> ranks_in_lex(const SortedSet& zset, const LexBound& min, const LexBound& max) {
  size_t first = 0;
  // Binary search over ranks for the first member above min, then for the
  // first above max: valid because members are in byte order for one score.
  size_t lo = 0, hi = zset.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (lex_above_min(zset.at(mid).member, min))
      hi = mid;
    else
      lo = mid + 1;
  }
  first = lo;
  hi = zset.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    if (lex_below_max(zset.at(mid).member, max))
      lo = mid + 1;
    else
      hi = mid;
  }
  return {first, std::max(first, lo)};
}

// ZRANGE's rank range [start, stop] made [first, last); empty when out of range.
std::pair<size_t, size_t> ranks_in_index(int64_t start, int64_t stop, size_t size) {
  const int64_t len = static_cast<int64_t>(size);
  if (start < 0) start += len;
  if (stop < 0) stop += len;
  if (start < 0) start = 0;
  if (start > stop || start >= len) return {0, 0};
  if (stop >= len) stop = len - 1;
  return {static_cast<size_t>(start), static_cast<size_t>(stop) + 1};
}

// Items replied as members, or with scores: RESP2 flat, RESP3 pairs.
void reply_items(resp::Reply& reply, const std::vector<const SortedSet::Item*>& items, bool with_scores) {
  const bool resp3 = reply.protocol() == resp::Protocol::resp3;
  if (!with_scores) {
    reply.array(items.size());
    for (const auto* item : items) reply.bulk(item->member);
    return;
  }
  if (resp3) {
    reply.array(items.size());
    for (const auto* item : items) {
      reply.array(2);
      reply.bulk(item->member);
      reply.real(item->score);
    }
    return;
  }
  reply.array(items.size() * 2);
  for (const auto* item : items) {
    reply.bulk(item->member);
    reply.real(item->score);
  }
}

enum class By { index, score, lex };

struct RangeRequest {
  By by = By::index;
  bool reverse = false;
  bool with_scores = false;
  bool limited = false;
  int64_t offset = 0;
  int64_t count = -1;
};

// The items a range selects, in reply order.
std::vector<const SortedSet::Item*> select(const SortedSet& zset, const std::string& start, const std::string& stop, const RangeRequest& request) {
  std::vector<const SortedSet::Item*> out;
  std::pair<size_t, size_t> span{0, 0};
  if (request.by == By::index) {
    const int64_t a = integer_argument(start);
    const int64_t b = integer_argument(stop);
    const auto forward = ranks_in_index(a, b, zset.size());
    if (request.reverse) {
      // Ranks counted from the highest.
      if (forward.first < forward.second) span = {zset.size() - forward.second, zset.size() - forward.first};
    } else {
      span = forward;
    }
  } else if (request.by == By::score) {
    // Reversed, the first argument is the maximum.
    const ScoreBound min = score_bound(request.reverse ? stop : start);
    const ScoreBound max = score_bound(request.reverse ? start : stop);
    span = ranks_in_scores(zset, min, max);
  } else {
    const LexBound min = lex_bound(request.reverse ? stop : start);
    const LexBound max = lex_bound(request.reverse ? start : stop);
    span = ranks_in_lex(zset, min, max);
  }
  if (span.first >= span.second) return out;
  const size_t total = span.second - span.first;
  size_t skip = 0;
  size_t take = total;
  if (request.limited) {
    if (request.offset < 0) return out;
    skip = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(request.offset), total));
    take = request.count < 0 ? total - skip : static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(request.count), total - skip));
  }
  out.reserve(take);
  for (size_t i = 0; i < take; ++i) {
    const size_t at = request.reverse ? span.second - 1 - skip - i : span.first + skip + i;
    out.push_back(&zset.at(at));
  }
  return out;
}

// ZRANGE's options, from `from` on.
RangeRequest parse_range(Call& call, size_t from, bool allow_scores) {
  RangeRequest request;
  for (size_t i = from; i < call.args.size(); ++i) {
    const std::string& option = call.args[i];
    if (iequals(option, "BYSCORE") && request.by == By::index) {
      request.by = By::score;
    } else if (iequals(option, "BYLEX") && request.by == By::index) {
      request.by = By::lex;
    } else if (iequals(option, "REV")) {
      request.reverse = true;
    } else if (iequals(option, "WITHSCORES") && allow_scores) {
      request.with_scores = true;
    } else if (iequals(option, "LIMIT") && i + 2 < call.args.size()) {
      request.limited = true;
      request.offset = integer_argument(call.args[i + 1]);
      request.count = integer_argument(call.args[i + 2]);
      i += 2;
    } else {
      throw CommandError(syntax_error);
    }
  }
  if (request.limited && request.by == By::index) throw CommandError("ERR syntax error, LIMIT is only supported in combination with either BYSCORE or BYLEX");
  if (request.with_scores && request.by == By::lex) throw CommandError("ERR syntax error, WITHSCORES not supported in combination with BYLEX");
  return request;
}

void range_reply(Call& call, const std::string& key, const std::string& start, const std::string& stop, const RangeRequest& request) {
  // Arguments are checked before the key is read, as Valkey checks them.
  if (request.by == By::score) {
    score_bound(start);
    score_bound(stop);
  } else if (request.by == By::lex) {
    lex_bound(start);
    lex_bound(stop);
  } else {
    integer_argument(start);
    integer_argument(stop);
  }
  SortedSet* zset = zset_value(call.keys, key);
  if (!zset) {
    call.reply.array(0);
    return;
  }
  reply_items(call.reply, select(*zset, start, stop, request), request.with_scores);
}

// The old-style range commands, as ZRANGE with their implied options.
void legacy_range(Call& call, By by, bool reverse, bool allow_limit) {
  RangeRequest request;
  request.by = by;
  request.reverse = reverse;
  for (size_t i = 4; i < call.args.size(); ++i) {
    const std::string& option = call.args[i];
    if (iequals(option, "WITHSCORES") && by != By::lex) {
      request.with_scores = true;
    } else if (allow_limit && iequals(option, "LIMIT") && i + 2 < call.args.size()) {
      request.limited = true;
      request.offset = integer_argument(call.args[i + 1]);
      request.count = integer_argument(call.args[i + 2]);
      i += 2;
    } else {
      throw CommandError(syntax_error);
    }
  }
  range_reply(call, call.args[1], call.args[2], call.args[3], request);
}

// A popped or chosen pair: RESP3 nests it, RESP2 flattens it.
void pop_generic(Call& call, bool min) {
  if (call.args.size() > 3) throw CommandError(syntax_error);
  std::optional<int64_t> count;
  if (call.args.size() == 3) {
    auto parsed = parse_integer(call.args[2]);
    if (!parsed || *parsed < 0) throw CommandError("ERR value is out of range, must be positive");
    count = *parsed;
  }
  SortedSet* zset = zset_value(call.keys, call.args[1]);
  const bool resp3 = call.reply.protocol() == resp::Protocol::resp3;
  if (!zset || (count && *count == 0)) {
    call.reply.array(0);
    return;
  }
  const size_t n = count ? static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(*count), zset->size())) : 1;
  std::vector<SortedSet::Item> taken;
  for (size_t i = 0; i < n; ++i) {
    taken.push_back(zset->at(min ? 0 : zset->size() - 1));
    zset->erase(taken.back().member);
  }
  if (resp3 && count) {
    call.reply.array(taken.size());
    for (const auto& item : taken) {
      call.reply.array(2);
      call.reply.bulk(item.member);
      call.reply.real(item.score);
    }
  } else {
    call.reply.array(taken.size() * 2);
    for (const auto& item : taken) {
      call.reply.bulk(item.member);
      call.reply.real(item.score);
    }
  }
  drop_if_empty(call.keys, call.args[1], *zset);
}

enum class Aggregate { sum, min, max };
enum class SetOp { onion, inter, diff };

// The members and scores a ZUNION, ZINTER or ZDIFF makes, with the index
// after its options' end. Sets are accepted as inputs with a score of 1.
struct Combined {
  std::vector<std::pair<std::string, double>> items;
  bool with_scores = false;
};

Combined combine(Call& call, size_t numkeys_at, SetOp op, bool allow_scores, std::string_view name) {
  auto numkeys = parse_integer(call.args[numkeys_at]);
  if (!numkeys) throw CommandError(not_integer);
  if (*numkeys < 1) throw CommandError("ERR at least 1 input key is needed for '" + std::string(name) + "' command");
  if (static_cast<uint64_t>(*numkeys) > call.args.size() - numkeys_at - 1) throw CommandError(syntax_error);
  const size_t first = numkeys_at + 1;
  const size_t end = first + static_cast<size_t>(*numkeys);
  std::vector<double> weights(static_cast<size_t>(*numkeys), 1.0);
  Aggregate aggregate = Aggregate::sum;
  Combined result;
  for (size_t i = end; i < call.args.size(); ++i) {
    const std::string& option = call.args[i];
    if (op != SetOp::diff && iequals(option, "WEIGHTS") && i + static_cast<size_t>(*numkeys) < call.args.size()) {
      for (size_t w = 0; w < weights.size(); ++w) {
        auto weight = parse_score(call.args[i + 1 + w]);
        if (!weight) throw CommandError("ERR weight value is not a float");
        weights[w] = *weight;
      }
      i += weights.size();
    } else if (op != SetOp::diff && iequals(option, "AGGREGATE") && i + 1 < call.args.size()) {
      const std::string& how = call.args[++i];
      if (iequals(how, "SUM"))
        aggregate = Aggregate::sum;
      else if (iequals(how, "MIN"))
        aggregate = Aggregate::min;
      else if (iequals(how, "MAX"))
        aggregate = Aggregate::max;
      else
        throw CommandError(syntax_error);
    } else if (allow_scores && iequals(option, "WITHSCORES")) {
      result.with_scores = true;
    } else {
      throw CommandError(syntax_error);
    }
  }
  // Each input as (member, score) pairs; a missing key is empty.
  std::vector<std::vector<std::pair<std::string_view, double>>> inputs;
  for (size_t k = first; k < end; ++k) {
    std::vector<std::pair<std::string_view, double>> input;
    keyspace::Entry* entry = call.keys.find(call.args[k]);
    if (entry) {
      if (entry->type() == keyspace::Type::zset) {
        const auto& zset = std::get<SortedSet>(entry->value);
        for (size_t r = 0; r < zset.size(); ++r) input.emplace_back(zset.at(r).member, zset.at(r).score);
      } else if (entry->type() == keyspace::Type::set) {
        for (auto member : std::get<keyspace::Set>(entry->value).ordered()) input.emplace_back(member, 1.0);
      } else {
        throw CommandError(wrong_type);
      }
    }
    inputs.push_back(std::move(input));
  }
  const auto weighted = [](double score, double weight) {
    const double value = score * weight;
    return std::isnan(value) ? 0.0 : value;
  };
  const auto fold = [&](double a, double b) {
    switch (aggregate) {
      case Aggregate::sum: {
        const double sum = a + b;
        return std::isnan(sum) ? 0.0 : sum;
      }
      case Aggregate::min:
        return std::min(a, b);
      case Aggregate::max:
        return std::max(a, b);
    }
    return a;
  };
  std::map<std::string_view, double> acc;
  if (op == SetOp::onion) {
    for (size_t k = 0; k < inputs.size(); ++k) {
      for (const auto& [member, score] : inputs[k]) {
        const double value = weighted(score, weights[k]);
        auto [it, fresh] = acc.emplace(member, value);
        if (!fresh) it->second = fold(it->second, value);
      }
    }
  } else if (op == SetOp::inter) {
    std::vector<std::map<std::string_view, double>> maps(inputs.size());
    for (size_t k = 0; k < inputs.size(); ++k)
      for (const auto& [member, score] : inputs[k]) maps[k].emplace(member, score);
    for (const auto& [member, score] : inputs.front()) {
      double value = weighted(score, weights[0]);
      bool everywhere = true;
      for (size_t k = 1; k < maps.size() && everywhere; ++k) {
        auto found = maps[k].find(member);
        if (found == maps[k].end())
          everywhere = false;
        else
          value = fold(value, weighted(found->second, weights[k]));
      }
      if (everywhere) acc.emplace(member, value);
    }
  } else {
    std::vector<std::map<std::string_view, double>> maps(inputs.size());
    for (size_t k = 1; k < inputs.size(); ++k)
      for (const auto& [member, score] : inputs[k]) maps[k].emplace(member, score);
    for (const auto& [member, score] : inputs.front()) {
      bool elsewhere = false;
      for (size_t k = 1; k < maps.size() && !elsewhere; ++k) elsewhere = maps[k].count(member) > 0;
      if (!elsewhere) acc.emplace(member, score);
    }
  }
  for (const auto& [member, score] : acc) result.items.emplace_back(std::string(member), score);
  std::sort(result.items.begin(), result.items.end(),
            [](const auto& a, const auto& b) { return a.second != b.second ? a.second < b.second : a.first < b.first; });
  return result;
}

void combine_reply(Call& call, SetOp op, std::string_view name) {
  Combined combined = combine(call, 1, op, true, name);
  std::vector<SortedSet::Item> items;
  for (auto& [member, score] : combined.items) items.push_back(SortedSet::Item{score, member});
  std::vector<const SortedSet::Item*> pointers;
  for (const auto& item : items) pointers.push_back(&item);
  reply_items(call.reply, pointers, combined.with_scores);
}

void combine_store(Call& call, SetOp op, std::string_view name) {
  Combined combined = combine(call, 2, op, false, name);
  call.reply.integer(static_cast<int64_t>(combined.items.size()));
  if (combined.items.empty()) {
    call.keys.erase(call.args[1]);
    return;
  }
  SortedSet made;
  for (const auto& [member, score] : combined.items) made.set(member, score);
  call.keys.put(call.args[1], keyspace::Entry{std::move(made), std::nullopt});
}

}  // namespace

// A plain ZADD sets scores, last writer winning per member: it merges. Its
// conditions depend on the score before it; INCR is a read-modify-write.
Class classify_zadd(const std::vector<std::string>& args) {
  for (size_t i = 2; i < args.size(); ++i) {
    if (iequals(args[i], "INCR")) return Class::configurable;
    if (iequals(args[i], "NX") || iequals(args[i], "XX") || iequals(args[i], "GT") || iequals(args[i], "LT")) return Class::agreement;
  }
  return Class::merge;
}

void zadd(Call& call) {
  bool nx = false, xx = false, gt = false, lt = false, ch = false, incr = false;
  size_t i = 2;
  for (; i < call.args.size(); ++i) {
    const std::string& option = call.args[i];
    if (iequals(option, "NX"))
      nx = true;
    else if (iequals(option, "XX"))
      xx = true;
    else if (iequals(option, "GT"))
      gt = true;
    else if (iequals(option, "LT"))
      lt = true;
    else if (iequals(option, "CH"))
      ch = true;
    else if (iequals(option, "INCR"))
      incr = true;
    else
      break;
  }
  const size_t pairs_start = i;
  const size_t remaining = call.args.size() - pairs_start;
  if (remaining == 0 || remaining % 2 != 0) throw CommandError(syntax_error);
  if (nx && xx) throw CommandError("ERR XX and NX options at the same time are not compatible");
  if ((gt && nx) || (lt && nx) || (gt && lt)) throw CommandError("ERR GT, LT, and/or NX options at the same time are not compatible");
  if (incr && remaining > 2) throw CommandError("ERR INCR option supports a single increment-element pair");
  std::vector<double> scores;
  for (size_t p = pairs_start; p < call.args.size(); p += 2) scores.push_back(score_argument(call.args[p]));

  SortedSet* existing = zset_value(call.keys, call.args[1]);
  if (!existing && xx) {
    if (incr)
      call.reply.null();
    else
      call.reply.integer(0);
    return;
  }
  SortedSet& zset = existing ? *existing : zset_or_create(call.keys, call.args[1]);
  int64_t added = 0;
  int64_t changed = 0;
  std::optional<double> incr_result;
  for (size_t p = 0; p < scores.size(); ++p) {
    const std::string& member = call.args[pairs_start + 2 * p + 1];
    double score = scores[p];
    auto current = zset.score(member);
    if (current) {
      if (nx) continue;
      if (incr) {
        score += *current;
        if (std::isnan(score)) {
          drop_if_empty(call.keys, call.args[1], zset);
          throw CommandError("ERR resulting score is not a number (NaN)");
        }
      }
      if ((gt && score <= *current) || (lt && score >= *current)) continue;
      if (score != *current) {
        zset.set(member, score);
        ++changed;
      }
      if (incr) incr_result = score;
    } else {
      if (xx) continue;
      zset.set(member, score);
      ++added;
      if (incr) incr_result = score;
    }
  }
  drop_if_empty(call.keys, call.args[1], zset);
  if (incr) {
    if (incr_result)
      call.reply.real(*incr_result);
    else
      call.reply.null();
    return;
  }
  call.reply.integer(ch ? added + changed : added);
}

void zincrby(Call& call) {
  const double by = score_argument(call.args[2]);
  SortedSet& zset = zset_or_create(call.keys, call.args[1]);
  const double next = zset.score(call.args[3]).value_or(0) + by;
  if (std::isnan(next)) {
    drop_if_empty(call.keys, call.args[1], zset);
    throw CommandError("ERR resulting score is not a number (NaN)");
  }
  zset.set(call.args[3], next);
  call.reply.real(next);
}

void zscore(Call& call) {
  SortedSet* zset = zset_value(call.keys, call.args[1]);
  auto score = zset ? zset->score(call.args[2]) : std::nullopt;
  if (score)
    call.reply.real(*score);
  else
    call.reply.null();
}

void zmscore(Call& call) {
  SortedSet* zset = zset_value(call.keys, call.args[1]);
  call.reply.array(call.args.size() - 2);
  for (size_t i = 2; i < call.args.size(); ++i) {
    auto score = zset ? zset->score(call.args[i]) : std::nullopt;
    if (score)
      call.reply.real(*score);
    else
      call.reply.null();
  }
}

void zcard(Call& call) {
  SortedSet* zset = zset_value(call.keys, call.args[1]);
  call.reply.integer(zset ? static_cast<int64_t>(zset->size()) : 0);
}

void zcount(Call& call) {
  const ScoreBound min = score_bound(call.args[2]);
  const ScoreBound max = score_bound(call.args[3]);
  SortedSet* zset = zset_value(call.keys, call.args[1]);
  if (!zset) {
    call.reply.integer(0);
    return;
  }
  auto [first, last] = ranks_in_scores(*zset, min, max);
  call.reply.integer(static_cast<int64_t>(last - first));
}

void zlexcount(Call& call) {
  const LexBound min = lex_bound(call.args[2]);
  const LexBound max = lex_bound(call.args[3]);
  SortedSet* zset = zset_value(call.keys, call.args[1]);
  if (!zset) {
    call.reply.integer(0);
    return;
  }
  auto [first, last] = ranks_in_lex(*zset, min, max);
  call.reply.integer(static_cast<int64_t>(last - first));
}

void zrange(Call& call) {
  RangeRequest request = parse_range(call, 4, true);
  range_reply(call, call.args[1], call.args[2], call.args[3], request);
}

void zrangestore(Call& call) {
  RangeRequest request = parse_range(call, 5, false);
  // Arguments first, then the source.
  if (request.by == By::score) {
    score_bound(call.args[3]);
    score_bound(call.args[4]);
  } else if (request.by == By::lex) {
    lex_bound(call.args[3]);
    lex_bound(call.args[4]);
  } else {
    integer_argument(call.args[3]);
    integer_argument(call.args[4]);
  }
  SortedSet* source = zset_value(call.keys, call.args[2]);
  SortedSet made;
  if (source)
    for (const auto* item : select(*source, call.args[3], call.args[4], request)) made.set(item->member, item->score);
  call.reply.integer(static_cast<int64_t>(made.size()));
  if (made.empty())
    call.keys.erase(call.args[1]);
  else
    call.keys.put(call.args[1], keyspace::Entry{std::move(made), std::nullopt});
}

void zrevrange(Call& call) { legacy_range(call, By::index, true, false); }
void zrangebyscore(Call& call) { legacy_range(call, By::score, false, true); }
void zrevrangebyscore(Call& call) { legacy_range(call, By::score, true, true); }
void zrangebylex(Call& call) { legacy_range(call, By::lex, false, true); }
void zrevrangebylex(Call& call) { legacy_range(call, By::lex, true, true); }

namespace {
void rank_generic(Call& call, bool reverse) {
  bool with_score = false;
  if (call.args.size() == 4) {
    if (!iequals(call.args[3], "WITHSCORE")) throw CommandError(syntax_error);
    with_score = true;
  } else if (call.args.size() > 4) {
    throw CommandError(syntax_error);
  }
  SortedSet* zset = zset_value(call.keys, call.args[1]);
  auto rank = zset ? zset->rank(call.args[2]) : std::nullopt;
  if (!rank) {
    if (with_score)
      call.reply.null_array();
    else
      call.reply.null();
    return;
  }
  const size_t value = reverse ? zset->size() - 1 - *rank : *rank;
  if (!with_score) {
    call.reply.integer(static_cast<int64_t>(value));
    return;
  }
  call.reply.array(2);
  call.reply.integer(static_cast<int64_t>(value));
  call.reply.real(*zset->score(call.args[2]));
}
}  // namespace

void zrank(Call& call) { rank_generic(call, false); }
void zrevrank(Call& call) { rank_generic(call, true); }

void zrem(Call& call) {
  SortedSet* zset = zset_value(call.keys, call.args[1]);
  if (!zset) {
    call.reply.integer(0);
    return;
  }
  int64_t removed = 0;
  for (size_t i = 2; i < call.args.size(); ++i) removed += zset->erase(call.args[i]) ? 1 : 0;
  drop_if_empty(call.keys, call.args[1], *zset);
  call.reply.integer(removed);
}

namespace {
void remove_span(Call& call, std::pair<size_t, size_t> span, SortedSet& zset) {
  std::vector<std::string> members;
  for (size_t r = span.first; r < span.second; ++r) members.push_back(zset.at(r).member);
  for (const auto& member : members) zset.erase(member);
  call.reply.integer(static_cast<int64_t>(members.size()));
  drop_if_empty(call.keys, call.args[1], zset);
}
}  // namespace

void zremrangebyrank(Call& call) {
  const int64_t start = integer_argument(call.args[2]);
  const int64_t stop = integer_argument(call.args[3]);
  SortedSet* zset = zset_value(call.keys, call.args[1]);
  if (!zset) {
    call.reply.integer(0);
    return;
  }
  remove_span(call, ranks_in_index(start, stop, zset->size()), *zset);
}

void zremrangebyscore(Call& call) {
  const ScoreBound min = score_bound(call.args[2]);
  const ScoreBound max = score_bound(call.args[3]);
  SortedSet* zset = zset_value(call.keys, call.args[1]);
  if (!zset) {
    call.reply.integer(0);
    return;
  }
  remove_span(call, ranks_in_scores(*zset, min, max), *zset);
}

void zremrangebylex(Call& call) {
  const LexBound min = lex_bound(call.args[2]);
  const LexBound max = lex_bound(call.args[3]);
  SortedSet* zset = zset_value(call.keys, call.args[1]);
  if (!zset) {
    call.reply.integer(0);
    return;
  }
  remove_span(call, ranks_in_lex(*zset, min, max), *zset);
}

void zpopmin(Call& call) { pop_generic(call, true); }
void zpopmax(Call& call) { pop_generic(call, false); }

void zmpop(Call& call) {
  auto numkeys = parse_integer(call.args[1]);
  if (!numkeys || *numkeys <= 0) throw CommandError("ERR numkeys should be greater than 0");
  if (static_cast<uint64_t>(*numkeys) > call.args.size() - 3) throw CommandError(syntax_error);
  const size_t where_at = 2 + static_cast<size_t>(*numkeys);
  bool min;
  if (iequals(call.args[where_at], "MIN"))
    min = true;
  else if (iequals(call.args[where_at], "MAX"))
    min = false;
  else
    throw CommandError(syntax_error);
  int64_t count = 1;
  bool have_count = false;
  for (size_t i = where_at + 1; i < call.args.size(); ++i) {
    if (iequals(call.args[i], "COUNT") && i + 1 < call.args.size() && !have_count) {
      auto parsed = parse_integer(call.args[++i]);
      if (!parsed || *parsed <= 0) throw CommandError("ERR count should be greater than 0");
      count = *parsed;
      have_count = true;
    } else {
      throw CommandError(syntax_error);
    }
  }
  for (size_t k = 2; k < where_at; ++k) {
    SortedSet* zset = zset_value(call.keys, call.args[k]);
    if (!zset) continue;
    const size_t n = std::min(static_cast<size_t>(count), zset->size());
    call.reply.array(2);
    call.reply.bulk(call.args[k]);
    call.reply.array(n);
    for (size_t i = 0; i < n; ++i) {
      SortedSet::Item item = zset->at(min ? 0 : zset->size() - 1);
      zset->erase(item.member);
      call.reply.array(2);
      call.reply.bulk(item.member);
      call.reply.real(item.score);
    }
    drop_if_empty(call.keys, call.args[k], *zset);
    return;
  }
  call.reply.null_array();
}

void zrandmember(Call& call) {
  if (call.args.size() == 2) {
    SortedSet* zset = zset_value(call.keys, call.args[1]);
    if (!zset) {
      call.reply.null();
      return;
    }
    call.reply.bulk(zset->at(call.database.random() % zset->size()).member);
    return;
  }
  const int64_t count = integer_argument(call.args[2]);
  bool with_scores = false;
  if (call.args.size() == 4) {
    if (!iequals(call.args[3], "WITHSCORES")) throw CommandError(syntax_error);
    with_scores = true;
  } else if (call.args.size() > 4) {
    throw CommandError(syntax_error);
  }
  if (with_scores && (count < -(LLONG_MAX / 2) || count > LLONG_MAX / 2)) throw CommandError("ERR value is out of range");
  SortedSet* zset = zset_value(call.keys, call.args[1]);
  if (!zset || count == 0) {
    call.reply.array(0);
    return;
  }
  std::vector<const SortedSet::Item*> chosen;
  if (count < 0) {
    const uint64_t n = 0 - static_cast<uint64_t>(count);
    for (uint64_t i = 0; i < n; ++i) chosen.push_back(&zset->at(call.database.random() % zset->size()));
  } else if (static_cast<uint64_t>(count) >= zset->size()) {
    // The whole set, highest first, as Valkey answers a small one (the
    // differential gate's finding).
    for (size_t r = zset->size(); r > 0; --r) chosen.push_back(&zset->at(r - 1));
  } else {
    std::vector<size_t> ranks(zset->size());
    for (size_t r = 0; r < ranks.size(); ++r) ranks[r] = r;
    for (size_t i = 0; i < static_cast<size_t>(count); ++i) {
      const size_t j = i + static_cast<size_t>(call.database.random() % (ranks.size() - i));
      std::swap(ranks[i], ranks[j]);
      chosen.push_back(&zset->at(ranks[i]));
    }
  }
  reply_items(call.reply, chosen, with_scores);
}

void zscan(Call& call) {
  {
    const std::string& text = call.args[2];
    uint64_t cursor = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), cursor);
    if (text.empty() || error != std::errc() || end != text.data() + text.size()) throw CommandError("ERR invalid cursor");
  }
  std::optional<std::string> pattern;
  bool noscores = false;
  for (size_t i = 3; i < call.args.size(); ++i) {
    const bool has_next = i + 1 < call.args.size();
    if (iequals(call.args[i], "MATCH") && has_next) {
      pattern = call.args[++i];
    } else if (iequals(call.args[i], "COUNT") && has_next) {
      if (integer_argument(call.args[++i]) < 1) throw CommandError(syntax_error);
    } else if (iequals(call.args[i], "NOSCORES")) {
      noscores = true;
    } else {
      throw CommandError(syntax_error);
    }
  }
  SortedSet* zset = zset_value(call.keys, call.args[1]);
  std::vector<const SortedSet::Item*> kept;
  if (zset)
    for (size_t r = 0; r < zset->size(); ++r)
      if (!pattern || keyspace::glob_match(*pattern, zset->at(r).member)) kept.push_back(&zset->at(r));
  // The whole sorted set in one page, as Valkey answers for a small one; a
  // score is a bulk string here in either protocol, as Valkey's scan replies.
  call.reply.array(2);
  call.reply.bulk("0");
  call.reply.array(noscores ? kept.size() : kept.size() * 2);
  for (const auto* item : kept) {
    call.reply.bulk(item->member);
    if (!noscores) call.reply.bulk(resp::format_double(item->score));
  }
}

void zunion(Call& call) { combine_reply(call, SetOp::onion, "zunion"); }
void zinter(Call& call) { combine_reply(call, SetOp::inter, "zinter"); }
void zdiff(Call& call) { combine_reply(call, SetOp::diff, "zdiff"); }
void zunionstore(Call& call) { combine_store(call, SetOp::onion, "zunionstore"); }
void zinterstore(Call& call) { combine_store(call, SetOp::inter, "zinterstore"); }
void zdiffstore(Call& call) { combine_store(call, SetOp::diff, "zdiffstore"); }

void zintercard(Call& call) {
  auto numkeys = parse_integer(call.args[1]);
  if (!numkeys || *numkeys <= 0) throw CommandError("ERR numkeys should be greater than 0");
  if (static_cast<uint64_t>(*numkeys) > call.args.size() - 2) throw CommandError("ERR Number of keys can't be greater than number of args");
  const size_t end = 2 + static_cast<size_t>(*numkeys);
  uint64_t limit = 0;
  for (size_t i = end; i < call.args.size(); ++i) {
    if (iequals(call.args[i], "LIMIT") && i + 1 < call.args.size()) {
      auto parsed = parse_integer(call.args[++i]);
      if (!parsed) throw CommandError(not_integer);
      if (*parsed < 0) throw CommandError("ERR LIMIT can't be negative");
      limit = static_cast<uint64_t>(*parsed);
    } else {
      throw CommandError(syntax_error);
    }
  }
  // As ZINTER over the keys alone, counted.
  std::vector<std::string> args{"zinter", call.args[1]};
  for (size_t k = 2; k < end; ++k) args.push_back(call.args[k]);
  resp::Reply scratch(call.reply.protocol());
  Call inner{call.session, call.database, call.keys, scratch, args};
  Combined combined = combine(inner, 1, SetOp::inter, false, "zintercard");
  uint64_t size = combined.items.size();
  if (limit > 0 && size > limit) size = limit;
  call.reply.integer(static_cast<int64_t>(size));
}

}  // namespace mimir::command::zsets
