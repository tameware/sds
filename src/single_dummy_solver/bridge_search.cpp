// Search adapted from macroxue/bridge-solver (Apache-2.0 OR MIT):
// https://github.com/macroxue/bridge-solver
// MTD(f) with a shape/pattern bounds cache, for SDS shared-pool positions.

#include "bridge_search.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "play_engine.hpp"

namespace single_dummy_solver
{
namespace
{

using detail::apply_play;
using detail::count_isomorphism_skipped;
using detail::current_player;
using detail::is_opponent_seat;
using detail::is_partnership_seat;
using detail::ordered_legal_moves;
using detail::play_complete;
using detail::remaining_tricks_upper_bound;

struct PatternBounds
{
  int lower = 0;
  int upper = 13;
};

struct ShapeKey
{
  std::array<std::uint8_t, 12> lengths{};  // known,dummy,pool × 4 suits
  std::array<std::uint8_t, kHands> voids{};
  std::uint8_t east_slots = 0;
  std::uint8_t west_slots = 0;
  std::uint8_t leader = 0;
  std::uint8_t denomination = 0;
  std::uint8_t known_seat = 0;
  std::uint64_t pins_hash = 0;
  std::uint64_t win_ranks_hash = 0;

  auto operator==(const ShapeKey& other) const -> bool = default;
};

struct ShapeKeyHash
{
  auto operator()(const ShapeKey& key) const -> std::size_t
  {
    std::size_t hash = 0;
    const auto mix = [&](std::uint64_t value) {
      hash ^= static_cast<std::size_t>(value)
          + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    };
    for (const auto length : key.lengths)
    {
      mix(length);
    }
    for (const auto void_mask : key.voids)
    {
      mix(void_mask);
    }
    mix(key.east_slots);
    mix(key.west_slots);
    mix(key.leader);
    mix(key.denomination);
    mix(key.known_seat);
    mix(key.pins_hash);
    mix(key.win_ranks_hash);
    return hash;
  }
};

// Relative-rank bitmasks per suit for known, dummy, and pool (bridge-solver
// style equivalence over the cards still in play).
using RelativePattern = std::array<std::uint16_t, 12>;

struct PatternEntry
{
  RelativePattern pattern{};
  PatternBounds bounds{};
};

struct PatternCache
{
  std::unordered_map<ShapeKey, std::vector<PatternEntry>, ShapeKeyHash> by_shape;
};

auto suit_length(const std::vector<Card>& cards, int suit) -> int
{
  int count = 0;
  for (const auto& card : cards)
  {
    if (card.suit == suit)
    {
      ++count;
    }
  }
  return count;
}

auto make_shape_key(const GameState& state) -> ShapeKey
{
  ShapeKey key{};
  for (int suit = 0; suit < kSuits; ++suit)
  {
    key.lengths[static_cast<std::size_t>(suit)] = static_cast<std::uint8_t>(
        suit_length(
            state.hands[static_cast<std::size_t>(state.known_seat)], suit));
    key.lengths[static_cast<std::size_t>(4 + suit)] =
        static_cast<std::uint8_t>(suit_length(
            state.hands[static_cast<std::size_t>(state.dummy_seat)], suit));
    key.lengths[static_cast<std::size_t>(8 + suit)] =
        static_cast<std::uint8_t>(suit_length(state.remaining_available, suit));
  }
  for (int seat = 0; seat < kHands; ++seat)
  {
    key.voids[static_cast<std::size_t>(seat)] =
        static_cast<std::uint8_t>(state.void_suits[static_cast<std::size_t>(seat)]);
  }
  key.east_slots = static_cast<std::uint8_t>(state.east_slots);
  key.west_slots = static_cast<std::uint8_t>(state.west_slots);
  key.leader = static_cast<std::uint8_t>(state.leader);
  key.denomination = static_cast<std::uint8_t>(state.denomination);
  key.known_seat = static_cast<std::uint8_t>(state.known_seat);
  for (const auto& pin : state.pins)
  {
    key.pins_hash = key.pins_hash * 1315423911ull
        + static_cast<std::uint64_t>(pin.seat + 1) * 17ull
        + static_cast<std::uint64_t>(pin.suit + 1) * 5ull
        + static_cast<std::uint64_t>(pin.rank);
  }
  for (int suit = 0; suit < kSuits; ++suit)
  {
    key.win_ranks_hash = key.win_ranks_hash * 1315423911ull
        + static_cast<std::uint64_t>(
              state.win_ranks[static_cast<std::size_t>(suit)] + 1);
  }
  return key;
}

auto relative_pattern(const GameState& state) -> RelativePattern
{
  RelativePattern pattern{};
  for (int suit = 0; suit < kSuits; ++suit)
  {
    std::vector<int> ranks;
    const auto collect = [&](const std::vector<Card>& cards) {
      for (const auto& card : cards)
      {
        if (card.suit == suit)
        {
          ranks.push_back(card.rank);
        }
      }
    };
    collect(state.hands[static_cast<std::size_t>(state.known_seat)]);
    collect(state.hands[static_cast<std::size_t>(state.dummy_seat)]);
    collect(state.remaining_available);
    std::sort(ranks.begin(), ranks.end());
    ranks.erase(std::unique(ranks.begin(), ranks.end()), ranks.end());

    const auto encode = [&](const std::vector<Card>& cards) -> std::uint16_t {
      std::uint16_t mask = 0;
      for (const auto& card : cards)
      {
        if (card.suit != suit)
        {
          continue;
        }
        const auto it = std::lower_bound(ranks.begin(), ranks.end(), card.rank);
        const int relative =
            static_cast<int>(std::distance(ranks.begin(), it));
        mask |= static_cast<std::uint16_t>(1u << relative);
      }
      return mask;
    };

    pattern[static_cast<std::size_t>(suit)] =
        encode(state.hands[static_cast<std::size_t>(state.known_seat)]);
    pattern[static_cast<std::size_t>(4 + suit)] =
        encode(state.hands[static_cast<std::size_t>(state.dummy_seat)]);
    pattern[static_cast<std::size_t>(8 + suit)] =
        encode(state.remaining_available);
  }
  return pattern;
}

auto try_pattern_cutoff(
    PatternCache& cache,
    const GameState& state,
    int beta,
    int& out_value) -> bool
{
  if (!state.trick.empty())
  {
    return false;
  }
  const auto shape = make_shape_key(state);
  const auto pattern = relative_pattern(state);
  const auto it = cache.by_shape.find(shape);
  if (it == cache.by_shape.end())
  {
    return false;
  }
  const int base = state.partnership_tricks;
  const int remaining_target = beta - base;
  for (const auto& entry : it->second)
  {
    if (entry.pattern != pattern)
    {
      continue;
    }
    if (entry.bounds.lower >= remaining_target)
    {
      out_value = base + entry.bounds.lower;
      return true;
    }
    if (entry.bounds.upper < remaining_target)
    {
      out_value = base + entry.bounds.upper;
      return true;
    }
  }
  return false;
}

auto store_pattern_bounds(
    PatternCache& cache,
    const GameState& state,
    int tricks,
    int beta) -> void
{
  if (!state.trick.empty())
  {
    return;
  }
  const int remaining = tricks - state.partnership_tricks;
  const int remaining_cap = remaining_tricks_upper_bound(state);
  PatternBounds bounds{};
  if (tricks < beta)
  {
    bounds.lower = 0;
    bounds.upper = remaining;
  }
  else
  {
    bounds.lower = remaining;
    bounds.upper = remaining_cap;
  }

  const auto shape = make_shape_key(state);
  const auto pattern = relative_pattern(state);
  auto& entries = cache.by_shape[shape];
  for (auto& entry : entries)
  {
    if (entry.pattern == pattern)
    {
      entry.bounds.lower = std::max(entry.bounds.lower, bounds.lower);
      entry.bounds.upper = std::min(entry.bounds.upper, bounds.upper);
      if (entry.bounds.lower > entry.bounds.upper)
      {
        // Conflicting probes; drop the entry so search re-proves the node.
        entry.bounds.lower = 0;
        entry.bounds.upper = 13;
      }
      return;
    }
  }
  entries.push_back(PatternEntry{pattern, bounds});
}

auto try_goal_cutoff(SearchStats& stats, int value) -> bool
{
  if (stats.goal.has_value() && value < *stats.goal)
  {
    ++stats.goal_cutoffs;
    return true;
  }
  return false;
}

auto search_alpha_beta(
    GameState state,
    SearchStats& stats,
    PatternCache& cache,
    int alpha,
    int beta) -> int;

auto search_moves(
    GameState state,
    SearchStats& stats,
    PatternCache& cache,
    bool maximize,
    int alpha,
    int beta) -> int
{
  const int seat = current_player(state);
  stats.isomorphism_skipped_moves += count_isomorphism_skipped(state, seat);
  if (state.force_next_play && seat == state.forced_play_seat)
  {
    auto next_state = state;
    next_state.force_next_play = false;
    next_state.forced_play_seat = -1;
    return search_alpha_beta(
        apply_play(next_state, state.forced_play),
        stats,
        cache,
        alpha,
        beta);
  }

  const auto moves = ordered_legal_moves(state, seat);
  if (moves.empty())
  {
    if (is_opponent_seat(state, seat))
    {
      return state.partnership_tricks + remaining_tricks_upper_bound(state);
    }
    throw std::runtime_error("no legal cards for seat " + std::to_string(seat));
  }

  int result = maximize ? std::numeric_limits<int>::min()
                        : std::numeric_limits<int>::max();
  for (const auto& card : moves)
  {
    const int value =
        search_alpha_beta(apply_play(state, card), stats, cache, alpha, beta);
    if (maximize)
    {
      result = std::max(result, value);
      alpha = std::max(alpha, result);
    }
    else
    {
      result = std::min(result, value);
      beta = std::min(beta, result);
    }
    if (alpha >= beta)
    {
      ++stats.alpha_beta_cutoffs;
      break;
    }
    if (!maximize && try_goal_cutoff(stats, result))
    {
      break;
    }
  }
  return result;
}

auto search_alpha_beta(
    GameState state,
    SearchStats& stats,
    PatternCache& cache,
    int alpha,
    int beta) -> int
{
  if (play_complete(state))
  {
    return state.partnership_tricks;
  }

  if (state.partnership_tricks >= beta)
  {
    return state.partnership_tricks;
  }
  const int remaining_cap = remaining_tricks_upper_bound(state);
  if (state.partnership_tricks + remaining_cap < beta)
  {
    return state.partnership_tricks + remaining_cap;
  }

  int cached_value = 0;
  if (try_pattern_cutoff(cache, state, beta, cached_value))
  {
    ++stats.pattern_hits;
    return cached_value;
  }

  const int seat = current_player(state);
  const bool partnership = is_partnership_seat(seat, state.known_seat);
  const int result =
      search_moves(state, stats, cache, partnership, alpha, beta);
  store_pattern_bounds(cache, state, result, beta);
  return result;
}

auto search_with_beta(
    const GameState& state,
    SearchStats& stats,
    PatternCache& cache,
    int beta) -> int
{
  return search_alpha_beta(
      state,
      stats,
      cache,
      beta - 1,
      beta);
}

auto memory_enhanced_test_driver(
    const GameState& state,
    SearchStats& stats,
    PatternCache& cache) -> int
{
  const int already = state.partnership_tricks;
  const int remaining = remaining_tricks_upper_bound(state);
  int lowerbound = already;
  int upperbound = already + remaining;
  int guess = upperbound;
  while (lowerbound < upperbound)
  {
    ++stats.mtd_iterations;
    const int beta = (guess == lowerbound) ? guess + 1 : guess;
    guess = search_with_beta(state, stats, cache, beta);
    if (guess < beta)
    {
      upperbound = guess;
    }
    else
    {
      lowerbound = guess;
    }
  }
  return guess;
}

}  // namespace

auto search_partnership_tricks(GameState state, SearchStats& stats) -> int
{
  PatternCache cache;
  return memory_enhanced_test_driver(state, stats, cache);
}

}  // namespace single_dummy_solver
