#include "transposition_table.hpp"

#include "single_dummy_solver.hpp"

#include <algorithm>
#include <functional>

namespace single_dummy_solver
{

namespace
{

constexpr int kRankBitmaskAll = 0x1FFF;

auto rank_bit(int rank) -> int
{
  return 1 << (rank - 2);
}

auto lowest_set_rank(int mask) -> int
{
  if (mask == 0)
  {
    return 0;
  }
  for (int rank = 2; rank <= 14; ++rank)
  {
    if ((mask & rank_bit(rank)) != 0)
    {
      return rank;
    }
  }
  return 0;
}

auto seat_holds_rank(
    int suit,
    int rank,
    const std::array<std::vector<Card>, kHands>& hands,
    const std::vector<std::pair<int, Card>>& trick) -> int
{
  for (int seat = 0; seat < kHands; ++seat)
  {
    for (const auto& card : hands[static_cast<std::size_t>(seat)])
    {
      if (card.suit == suit && card.rank == rank)
      {
        return seat;
      }
    }
  }
  for (const auto& [seat, card] : trick)
  {
    if (card.suit == suit && card.rank == rank)
    {
      return seat;
    }
  }
  return -1;
}

auto aggregate_for_suit(
    int suit,
    const std::array<std::vector<Card>, kHands>& hands,
    const std::vector<std::pair<int, Card>>& trick) -> int
{
  int aggregate = 0;
  for (const auto& hand : hands)
  {
    for (const auto& card : hand)
    {
      if (card.suit == suit)
      {
        aggregate |= rank_bit(card.rank);
      }
    }
  }
  for (const auto& [seat, card] : trick)
  {
    if (card.suit == suit)
    {
      aggregate |= rank_bit(card.rank);
    }
  }
  return aggregate;
}

}  // namespace

auto relative_rank_in_suit(int aggregate, int absolute_rank) -> int
{
  if ((aggregate & rank_bit(absolute_rank)) == 0)
  {
    return 0;
  }
  int relative = 0;
  for (int rank = 14; rank >= 2; --rank)
  {
    if ((aggregate & rank_bit(rank)) == 0)
    {
      continue;
    }
    ++relative;
    if (rank == absolute_rank)
    {
      return relative;
    }
  }
  return 0;
}

auto encoding_win_mask(int aggregate, int lowest_winning_absolute_rank) -> unsigned
{
  if (lowest_winning_absolute_rank == 0)
  {
    return 0;
  }
  unsigned mask = 0;
  for (int rank = 14; rank >= 2; --rank)
  {
    if ((aggregate & rank_bit(rank)) == 0)
    {
      continue;
    }
    mask = (mask << 2) | 0x3u;
    if (rank == lowest_winning_absolute_rank)
    {
      break;
    }
  }
  return mask;
}

auto encode_suit_order_set(
    int suit,
    int aggregate,
    const std::array<std::vector<Card>, kHands>& hands,
    const std::vector<std::pair<int, Card>>& trick) -> unsigned
{
  unsigned encoding = 0;
  for (int rank = 14; rank >= 2; --rank)
  {
    if ((aggregate & rank_bit(rank)) == 0)
    {
      continue;
    }
    const int seat = seat_holds_rank(suit, rank, hands, trick);
    if (seat < 0)
    {
      continue;
    }
    encoding = (encoding << 2) | static_cast<unsigned>(seat);
  }
  return encoding;
}

auto build_transposition_position(
    int partnership_tricks,
    int east_slots,
    int west_slots,
    int leader,
    const std::array<std::vector<Card>, kHands>& hands,
    const std::vector<std::pair<int, Card>>& trick,
    const std::array<int, kHands>& void_suits,
    const std::array<int, kSuits>& win_ranks,
    const std::vector<Card>& remaining_available) -> TranspositionPosition
{
  TranspositionPosition position{};
  position.partnership_tricks = partnership_tricks;
  position.east_slots = east_slots;
  position.west_slots = west_slots;
  position.leader = leader;
  position.trick_size = static_cast<int>(trick.size());
  position.void_suits = void_suits;

  for (int seat = 0; seat < kHands; ++seat)
  {
    for (const auto& card : hands[static_cast<std::size_t>(seat)])
    {
      ++position.cards_per_suit[static_cast<std::size_t>(card.suit)]
                                [static_cast<std::size_t>(seat)];
    }
  }

  for (const auto& card : remaining_available)
  {
    position.pool_aggregate[static_cast<std::size_t>(card.suit)] |=
        rank_bit(card.rank);
  }

  for (int suit = 0; suit < kSuits; ++suit)
  {
    position.aggregate[static_cast<std::size_t>(suit)] =
        aggregate_for_suit(suit, hands, trick)
        | position.pool_aggregate[static_cast<std::size_t>(suit)];
    position.order_set[static_cast<std::size_t>(suit)] = encode_suit_order_set(
        suit,
        position.aggregate[static_cast<std::size_t>(suit)],
        hands,
        trick);
    const int lowest_win = lowest_set_rank(
        win_ranks[static_cast<std::size_t>(suit)] & kRankBitmaskAll);
    position.win_mask[static_cast<std::size_t>(suit)] = encoding_win_mask(
        position.aggregate[static_cast<std::size_t>(suit)],
        lowest_win);
    position.stored_order_set[static_cast<std::size_t>(suit)] =
        position.order_set[static_cast<std::size_t>(suit)]
        & position.win_mask[static_cast<std::size_t>(suit)];
  }

  for (std::size_t index = 0; index < trick.size(); ++index)
  {
    const auto& [seat, card] = trick[index];
    position.trick_seats[index] = seat;
    position.trick_suits[index] = card.suit;
    position.trick_relative_ranks[index] = relative_rank_in_suit(
        position.aggregate[static_cast<std::size_t>(card.suit)],
        card.rank);
  }

  return position;
}

auto TranspositionTable::suit_lengths_key(const TranspositionPosition& position)
    -> std::uint64_t
{
  std::uint64_t key = 0;
  for (int seat = 0; seat < kHands; ++seat)
  {
    std::uint64_t seat_lengths = 0;
    for (int suit = 0; suit < kSuits; ++suit)
    {
      seat_lengths =
          (seat_lengths << 4)
          | static_cast<std::uint64_t>(
                position.cards_per_suit[static_cast<std::size_t>(suit)]
                                       [static_cast<std::size_t>(seat)]);
    }
    key = (key << 16) | (seat_lengths & 0xFFFFu);
  }
  return key;
}

auto TranspositionTable::context_hash(const TranspositionPosition& position)
    -> std::uint32_t
{
  std::uint32_t hash = static_cast<std::uint32_t>(position.partnership_tricks);
  hash = hash * 31u + static_cast<std::uint32_t>(position.east_slots);
  hash = hash * 31u + static_cast<std::uint32_t>(position.west_slots);
  hash = hash * 31u + static_cast<std::uint32_t>(position.leader);
  hash = hash * 31u + static_cast<std::uint32_t>(position.trick_size);
  for (const int void_mask : position.void_suits)
  {
    hash = hash * 31u + static_cast<std::uint32_t>(void_mask);
  }
  for (const int pool_mask : position.pool_aggregate)
  {
    hash = hash * 31u + static_cast<std::uint32_t>(pool_mask);
  }
  for (int index = 0; index < position.trick_size; ++index)
  {
    hash = hash * 31u + static_cast<std::uint32_t>(position.trick_seats[index]);
    hash = hash * 31u + static_cast<std::uint32_t>(position.trick_suits[index]);
    hash = hash * 31u
        + static_cast<std::uint32_t>(position.trick_relative_ranks[index]);
  }
  return hash;
}

auto TranspositionTable::bucket_index(const TranspositionPosition& position)
    -> std::size_t
{
  std::size_t hash = std::hash<std::uint64_t>{}(suit_lengths_key(position));
  hash ^= static_cast<std::size_t>(context_hash(position)) << 1;
  for (unsigned order_set : position.order_set)
  {
    hash ^= static_cast<std::size_t>(order_set * 1315423911u);
  }
  return hash % kBucketCount;
}

auto TranspositionTable::matches(
    const Entry& entry,
    const TranspositionPosition& position) -> bool
{
  if (entry.suit_lengths != suit_lengths_key(position))
  {
    return false;
  }
  if (entry.context_hash != context_hash(position))
  {
    return false;
  }
  if (entry.pool_aggregate != position.pool_aggregate)
  {
    return false;
  }
  const bool shared_pool = std::any_of(
      position.pool_aggregate.begin(),
      position.pool_aggregate.end(),
      [](int mask) { return mask != 0; });
  for (int suit = 0; suit < kSuits; ++suit)
  {
    if (shared_pool)
    {
      // Relative-rank soft matching is unsafe while unseen cards sit in a shared
      // pool rather than in assigned hands.
      if (entry.order_set[static_cast<std::size_t>(suit)]
          != position.order_set[static_cast<std::size_t>(suit)])
      {
        return false;
      }
      continue;
    }
    if ((entry.win_mask[static_cast<std::size_t>(suit)]
         & position.order_set[static_cast<std::size_t>(suit)])
        != entry.order_set[static_cast<std::size_t>(suit)])
    {
      return false;
    }
  }
  return true;
}

auto TranspositionTable::lookup(
    const TranspositionPosition& position,
    int& alpha,
    int& beta) -> std::optional<int>
{
  const bool shared_pool = std::any_of(
      position.pool_aggregate.begin(),
      position.pool_aggregate.end(),
      [](int mask) { return mask != 0; });
  const auto& bucket = buckets_[bucket_index(position)];
  for (const auto& entry : bucket)
  {
    if (!matches(entry, position))
    {
      continue;
    }
    ++hits_;
    switch (entry.bound)
    {
      case TranspositionBound::Exact:
        return entry.value;
      case TranspositionBound::Lower:
        if (shared_pool)
        {
          // Bound cutoffs under shared-pool keys are not yet trustworthy.
          return std::nullopt;
        }
        alpha = std::max(alpha, entry.value);
        break;
      case TranspositionBound::Upper:
        if (shared_pool)
        {
          return std::nullopt;
        }
        beta = std::min(beta, entry.value);
        break;
    }
    if (alpha >= beta)
    {
      ++bound_cutoffs_;
      return entry.value;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

auto TranspositionTable::store(
    const TranspositionPosition& position,
    int value,
    int alpha,
    int beta) -> void
{
  TranspositionBound bound = TranspositionBound::Exact;
  if (value <= alpha)
  {
    bound = TranspositionBound::Upper;
  }
  else if (value >= beta)
  {
    bound = TranspositionBound::Lower;
  }

  Entry entry{};
  entry.suit_lengths = suit_lengths_key(position);
  entry.context_hash = context_hash(position);
  entry.pool_aggregate = position.pool_aggregate;
  entry.win_mask = position.win_mask;
  const bool shared_pool = std::any_of(
      position.pool_aggregate.begin(),
      position.pool_aggregate.end(),
      [](int mask) { return mask != 0; });
  // Shared-pool positions store the full order set for exact matching; dealt
  // hands keep the DDS relative-rank projection under win_mask.
  entry.order_set =
      shared_pool ? position.order_set : position.stored_order_set;
  entry.bound = bound;
  entry.value = value;

  auto& bucket = buckets_[bucket_index(position)];
  for (auto& existing : bucket)
  {
    if (matches(existing, position))
    {
      existing = entry;
      ++stores_;
      return;
    }
  }
  bucket.push_back(entry);
  ++stores_;
}

}  // namespace single_dummy_solver
