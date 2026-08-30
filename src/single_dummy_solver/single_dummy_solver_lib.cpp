/*
   DDS, a bridge double dummy solver.

   Copyright (C) 2006-2014 by Bo Haglund /
   2014-2016 by Bo Haglund & Soren Hein.

   See LICENSE and README.
*/

#include "single_dummy_solver.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "transposition_table.hpp"

namespace single_dummy_solver
{

auto enumerate_constrained_opponent_splits(
    const std::vector<Card>& unassigned,
    int east_hand_size,
    int west_hand_size,
    int east_slots,
    int west_slots,
    int east_void_suits,
    int west_void_suits,
    int min_significant_rank) -> std::vector<UnknownSplit>;

namespace
{

constexpr std::string_view RankChars = "23456789TJQKA";
constexpr std::string_view SuitChars = "SHDC";
constexpr std::string_view SeatChars = "NESW";

struct PlayedCard
{
  int seat;
  Card card;
};

struct GameState
{
  int denomination;
  int known_seat;
  int dummy_seat;
  int east_seat;
  int west_seat;
  std::array<std::vector<Card>, kHands> hands;
  std::vector<Card> remaining_available;
  int leader;
  std::vector<PlayedCard> trick;
  int partnership_tricks;
  int east_slots;
  int west_slots;
  std::array<int, kHands> void_suits{};
  std::array<int, kSuits> win_ranks{};
  int min_significant_rank = 2;
};

struct EvaluationStats
{
  int opposition_split_evaluations = 0;
  int alpha_beta_cutoffs = 0;
  int isomorphism_skipped_moves = 0;
  int goal_cutoffs = 0;
  std::optional<int> goal;
  TranspositionTable transposition_table;
};

auto try_goal_cutoff(EvaluationStats& stats, int value) -> bool
{
  if (stats.goal.has_value() && value < *stats.goal)
  {
    ++stats.goal_cutoffs;
    return true;
  }
  return false;
}

constexpr int kRankBitmaskAll = 0x1FFF;

auto rank_bit(int rank) -> int
{
  return 1 << (rank - 2);
}

auto held_mask_in_suit(const GameState& state, int suit) -> int
{
  int mask = 0;
  for (const auto& hand : state.hands)
  {
    for (const auto& card : hand)
    {
      if (card.suit == suit)
      {
        mask |= rank_bit(card.rank);
      }
    }
  }
  for (const auto& played : state.trick)
  {
    if (played.card.suit == suit)
    {
      mask |= rank_bit(played.card.rank);
    }
  }
  return mask;
}

auto unavailable_mask_in_suit(const GameState& state, int suit) -> int
{
  return (~held_mask_in_suit(state, suit)) & kRankBitmaskAll;
}

struct SuitGroup
{
  int top_rank;
  int gap;
};

auto suit_groups(int hand_mask) -> std::vector<SuitGroup>
{
  std::vector<SuitGroup> groups;
  std::optional<int> above_bottom;
  for (int top = 14; top >= 2;)
  {
    if ((hand_mask & rank_bit(top)) == 0)
    {
      --top;
      continue;
    }
    int bottom = top;
    while (bottom > 2 && (hand_mask & rank_bit(bottom - 1)) != 0)
    {
      --bottom;
    }
    int gap = 0;
    if (above_bottom.has_value())
    {
      for (int rank = top + 1; rank <= *above_bottom - 1; ++rank)
      {
        gap |= rank_bit(rank);
      }
    }
    groups.push_back(SuitGroup{top, gap});
    above_bottom = bottom;
    top = bottom - 1;
  }
  return groups;
}

auto representative_ranks_in_suit(
    int hand_mask,
    int unavailable_mask) -> std::vector<int>
{
  const auto groups = suit_groups(hand_mask);
  std::vector<int> representatives;
  for (std::size_t index = 0; index < groups.size();)
  {
    std::size_t merge_end = index;
    while (merge_end + 1 < groups.size()
           && (groups[merge_end + 1].gap & unavailable_mask)
               == groups[merge_end + 1].gap)
    {
      ++merge_end;
    }
    representatives.push_back(groups[index].top_rank);
    index = merge_end + 1;
  }
  return representatives;
}

auto hand_mask_in_suit(const std::vector<Card>& hand, int suit) -> int
{
  int mask = 0;
  for (const auto& card : hand)
  {
    if (card.suit == suit)
    {
      mask |= rank_bit(card.rank);
    }
  }
  return mask;
}

auto append_distinct_cards_in_suit(
    const std::vector<Card>& hand,
    int suit,
    int unavailable_mask,
    std::vector<Card>& moves) -> void
{
  const int hand_mask = hand_mask_in_suit(hand, suit);
  if (hand_mask == 0)
  {
    return;
  }
  const auto ranks = representative_ranks_in_suit(hand_mask, unavailable_mask);
  for (const int rank : ranks)
  {
    moves.push_back(Card{suit, rank});
  }
}

auto distinct_legal_cards(const GameState& state, int seat) -> std::vector<Card>
{
  const auto& hand = state.hands[static_cast<std::size_t>(seat)];
  if (hand.empty())
  {
    return {};
  }
  if (state.trick.empty())
  {
    std::vector<Card> moves;
    for (int suit = 0; suit < kSuits; ++suit)
    {
      append_distinct_cards_in_suit(
          hand, suit, unavailable_mask_in_suit(state, suit), moves);
    }
    return moves;
  }

  const int lead_suit = state.trick.front().card.suit;
  const int lead_mask = hand_mask_in_suit(hand, lead_suit);
  if (lead_mask != 0)
  {
    std::vector<Card> moves;
    append_distinct_cards_in_suit(
        hand,
        lead_suit,
        unavailable_mask_in_suit(state, lead_suit),
        moves);
    return moves;
  }

  std::vector<Card> moves;
  for (int suit = 0; suit < kSuits; ++suit)
  {
    append_distinct_cards_in_suit(
        hand, suit, unavailable_mask_in_suit(state, suit), moves);
  }
  return moves;
}

auto legal_cards(const GameState& state, int seat) -> std::vector<Card>;

auto count_isomorphism_skipped(const GameState& state, int seat) -> int
{
  const auto full = legal_cards(state, seat);
  const auto distinct = distinct_legal_cards(state, seat);
  return static_cast<int>(full.size()) - static_cast<int>(distinct.size());
}

auto trick_cards_as_pairs(const GameState& state)
    -> std::vector<std::pair<int, Card>>
{
  std::vector<std::pair<int, Card>> trick;
  trick.reserve(state.trick.size());
  for (const auto& played : state.trick)
  {
    trick.emplace_back(played.seat, played.card);
  }
  return trick;
}

auto transposition_position(const GameState& state) -> TranspositionPosition
{
  return build_transposition_position(
      state.partnership_tricks,
      state.east_slots,
      state.west_slots,
      state.leader,
      state.hands,
      trick_cards_as_pairs(state),
      state.void_suits,
      state.win_ranks);
}

auto char_to_rank(char ch) -> int
{
  const auto upper = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  const auto pos = RankChars.find(upper);
  if (pos == std::string_view::npos)
  {
    throw std::invalid_argument(std::string("invalid rank character: ") + ch);
  }
  return static_cast<int>(pos) + 2;
}

auto equals_lists_rank(int equals_holding, int rank) -> bool
{
  if (rank < 2 || rank > 14)
  {
    return false;
  }
  const unsigned bits = static_cast<unsigned>(equals_holding) >> 2;
  return (bits & (1u << (rank - 2))) != 0;
}

auto opponent_seats(int known_seat, int dummy_seat) -> std::pair<int, int>
{
  std::vector<int> seats;
  for (int seat = 0; seat < kHands; ++seat)
  {
    if (seat != known_seat && seat != dummy_seat)
    {
      seats.push_back(seat);
    }
  }
  return {seats[0], seats[1]};
}

auto is_partnership_seat(int seat, int known_seat, int dummy_seat) -> bool
{
  return seat == known_seat || seat == dummy_seat;
}

auto choose_combinations(
    const std::vector<Card>& pool,
    int choose,
    int start,
    std::vector<Card>& current,
    std::vector<std::vector<Card>>& out) -> void
{
  if (static_cast<int>(current.size()) == choose)
  {
    out.push_back(current);
    return;
  }

  const int need = choose - static_cast<int>(current.size());
  for (int index = start; index + need <= static_cast<int>(pool.size()); ++index)
  {
    current.push_back(pool[index]);
    choose_combinations(pool, choose, index + 1, current, out);
    current.pop_back();
  }
}

auto complement(
    const std::vector<Card>& pool,
    const std::vector<Card>& chosen) -> std::vector<Card>
{
  std::vector<Card> rest;
  for (const auto& card : pool)
  {
    if (std::find(chosen.begin(), chosen.end(), card) == chosen.end())
    {
      rest.push_back(card);
    }
  }
  return rest;
}

auto rank_to_char(int rank) -> char
{
  return RankChars[static_cast<std::size_t>(rank - 2)];
}

auto card_to_string(const Card& card) -> std::string
{
  return std::string(1, SuitChars[static_cast<std::size_t>(card.suit)])
      + rank_to_char(card.rank);
}

auto hand_to_suit_fields(const std::vector<Card>& hand) -> std::array<std::string, kSuits>
{
  std::array<std::vector<int>, kSuits> ranks{};
  for (const auto& card : hand)
  {
    ranks[static_cast<std::size_t>(card.suit)].push_back(card.rank);
  }
  std::array<std::string, kSuits> fields{};
  for (int suit = 0; suit < kSuits; ++suit)
  {
    auto& suit_ranks = ranks[static_cast<std::size_t>(suit)];
    std::sort(suit_ranks.begin(), suit_ranks.end(), std::greater<int>());
    for (const int rank : suit_ranks)
    {
      fields[static_cast<std::size_t>(suit)] += rank_to_char(rank);
    }
  }
  return fields;
}

auto append_void_suffix(int void_mask, std::ostringstream& out) -> void
{
  if (void_mask == 0)
  {
    return;
  }
  out << " [void:";
  for (int suit = 0; suit < kSuits; ++suit)
  {
    if ((void_mask & (1 << suit)) != 0)
    {
      out << ' ' << SuitChars[static_cast<std::size_t>(suit)];
    }
  }
  out << ']';
}

auto append_hand_lines(
    const std::array<std::string, kSuits>& fields,
    std::ostringstream& out) -> void
{
  for (int suit = 0; suit < kSuits; ++suit)
  {
    out << "  " << SuitChars[static_cast<std::size_t>(suit)] << "  "
        << fields[static_cast<std::size_t>(suit)] << '\n';
  }
}

auto current_player(const GameState& state) -> int
{
  return (state.leader + static_cast<int>(state.trick.size())) % kHands;
}

auto all_hands_empty(const GameState& state) -> bool
{
  for (const auto& hand : state.hands)
  {
    if (!hand.empty())
    {
      return false;
    }
  }
  return true;
}

auto legal_cards(const GameState& state, int seat) -> std::vector<Card>
{
  const auto& hand = state.hands[static_cast<std::size_t>(seat)];
  if (hand.empty())
  {
    return {};
  }
  if (state.trick.empty())
  {
    return hand;
  }

  const int lead_suit = state.trick.front().card.suit;
  std::vector<Card> follow;
  for (const auto& card : hand)
  {
    if (card.suit == lead_suit)
    {
      follow.push_back(card);
    }
  }
  if (!follow.empty())
  {
    return follow;
  }
  return hand;
}

auto trick_winner(
    const std::vector<PlayedCard>& trick,
    int trump,
    int leader) -> int
{
  const int lead_suit = trick.front().card.suit;
  int best = 0;
  for (int index = 1; index < static_cast<int>(trick.size()); ++index)
  {
    const auto& candidate = trick[static_cast<std::size_t>(index)];
    const auto& best_card = trick[static_cast<std::size_t>(best)];
    const bool candidate_trump = trump != 4 && candidate.card.suit == trump;
    const bool best_trump = trump != 4 && best_card.card.suit == trump;
    if (candidate_trump && !best_trump)
    {
      best = index;
    }
    else if (candidate_trump && best_trump)
    {
      if (candidate.card.rank > best_card.card.rank)
      {
        best = index;
      }
    }
    else if (!candidate_trump && !best_trump)
    {
      if (candidate.card.suit == lead_suit
          && (best_card.card.suit != lead_suit
              || candidate.card.rank > best_card.card.rank))
      {
        best = index;
      }
    }
  }
  return (leader + best) % kHands;
}

auto winning_rank_mask_in_suit(int suit, int winning_rank, int aggregate) -> int
{
  int mask = 0;
  for (int rank = winning_rank; rank <= 14; ++rank)
  {
    if ((aggregate & rank_bit(rank)) != 0)
    {
      mask |= rank_bit(rank);
    }
  }
  return mask;
}

auto record_trick_win_ranks(GameState& state) -> void
{
  const int winner_index = [&]() {
    const int winner_seat = trick_winner(state.trick, state.denomination, state.leader);
    for (int index = 0; index < static_cast<int>(state.trick.size()); ++index)
    {
      const int seat = (state.leader + index) % kHands;
      if (seat == winner_seat)
      {
        return index;
      }
    }
    return 0;
  }();

  const auto& winning_card = state.trick[static_cast<std::size_t>(winner_index)].card;
  const int lead_suit = state.trick.front().card.suit;
  const bool trump_contract = state.denomination != 4;
  const bool wins_by_trump =
      trump_contract && winning_card.suit == state.denomination;
  const bool wins_by_lead =
      !wins_by_trump && winning_card.suit == lead_suit;

  if (!wins_by_trump && !wins_by_lead)
  {
    return;
  }

  const int suit = winning_card.suit;
  const int aggregate = held_mask_in_suit(state, suit);
  state.win_ranks[static_cast<std::size_t>(suit)] |= winning_rank_mask_in_suit(
      suit,
      winning_card.rank,
      aggregate);
}

auto remove_card(std::vector<Card>& hand, const Card& card) -> void
{
  const auto it = std::find(hand.begin(), hand.end(), card);
  if (it == hand.end())
  {
    throw std::runtime_error("card not in hand: " + card_to_string(card));
  }
  hand.erase(it);
}

auto apply_opponent_split(GameState state, const UnknownSplit& split) -> GameState
{
  state.hands[static_cast<std::size_t>(state.east_seat)] = split.first;
  state.hands[static_cast<std::size_t>(state.west_seat)] = split.second;
  state.remaining_available.clear();
  std::sort(
      state.hands[static_cast<std::size_t>(state.east_seat)].begin(),
      state.hands[static_cast<std::size_t>(state.east_seat)].end());
  std::sort(
      state.hands[static_cast<std::size_t>(state.west_seat)].begin(),
      state.hands[static_cast<std::size_t>(state.west_seat)].end());
  return state;
}

auto gather_opponent_cards(const GameState& state) -> std::vector<Card>
{
  const auto& east_hand = state.hands[static_cast<std::size_t>(state.east_seat)];
  const auto& west_hand = state.hands[static_cast<std::size_t>(state.west_seat)];
  std::vector<Card> cards;
  if (!east_hand.empty() || !west_hand.empty())
  {
    // Opponents were dealt; unplayed cards live in their hands only.
    cards.insert(cards.end(), east_hand.begin(), east_hand.end());
    cards.insert(cards.end(), west_hand.begin(), west_hand.end());
  }
  else
  {
    cards = state.remaining_available;
  }
  std::sort(cards.begin(), cards.end());
  return cards;
}

auto needs_adversarial_split(const GameState& state, int seat) -> bool
{
  if (state.east_slots + state.west_slots == 0)
  {
    return false;
  }
  const auto unknown = gather_opponent_cards(state);
  if (unknown.empty())
  {
    return false;
  }
  if (unknown.size()
      != static_cast<std::size_t>(state.east_slots + state.west_slots))
  {
    return false;
  }
  if (is_partnership_seat(seat, state.known_seat, state.dummy_seat))
  {
    return true;
  }
  return state.hands[static_cast<std::size_t>(seat)].empty();
}

auto mark_void_in_suit(GameState& state, int seat, int suit) -> void
{
  state.void_suits[static_cast<std::size_t>(seat)] |= 1 << suit;
}

auto cards_eligible_for_seat(
    const std::vector<Card>& pool,
    int void_suits) -> std::vector<Card>
{
  std::vector<Card> eligible;
  for (const auto& card : pool)
  {
    if ((void_suits & (1 << card.suit)) == 0)
    {
      eligible.push_back(card);
    }
  }
  return eligible;
}

auto adversarial_splits(const GameState& state) -> std::vector<UnknownSplit>
{
  return enumerate_constrained_opponent_splits(
      gather_opponent_cards(state),
      /*east_hand_size=*/0,
      /*west_hand_size=*/0,
      state.east_slots,
      state.west_slots,
      state.void_suits[static_cast<std::size_t>(state.east_seat)],
      state.void_suits[static_cast<std::size_t>(state.west_seat)],
      state.min_significant_rank);
}

auto evaluate_recursive(
    GameState state,
    EvaluationStats& stats,
    int alpha,
    int beta) -> int;

auto apply_play(GameState state, const Card& card) -> GameState;

auto evaluate_moves(
    GameState state,
    EvaluationStats& stats,
    bool maximize,
    int alpha,
    int beta) -> int
{
  const int seat = current_player(state);
  stats.isomorphism_skipped_moves += count_isomorphism_skipped(state, seat);
  const auto moves = distinct_legal_cards(state, seat);
  if (moves.empty())
  {
    throw std::runtime_error("no legal cards for seat " + std::to_string(seat));
  }

  int result = maximize ? std::numeric_limits<int>::min()
                        : std::numeric_limits<int>::max();
  for (const auto& card : moves)
  {
    const int value =
        evaluate_recursive(apply_play(state, card), stats, alpha, beta);
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

auto evaluate_recursive(
    GameState state,
    EvaluationStats& stats,
    int alpha,
    int beta) -> int
{
  if (all_hands_empty(state) && state.trick.empty())
  {
    return state.partnership_tricks;
  }

  const int alpha_orig = alpha;
  const int beta_orig = beta;
  const auto position = transposition_position(state);
  if (const auto cached = stats.transposition_table.lookup(position, alpha, beta))
  {
    return *cached;
  }

  const int seat = current_player(state);
  const bool partnership =
      is_partnership_seat(seat, state.known_seat, state.dummy_seat);
  int result = 0;
  if (needs_adversarial_split(state, seat))
  {
    const auto splits = adversarial_splits(state);
    if (splits.empty())
    {
      throw std::invalid_argument(
          "no legal opposition split for remaining cards and void suits");
    }
    result = std::numeric_limits<int>::max();
    for (const auto& split : splits)
    {
      ++stats.opposition_split_evaluations;
      const auto split_state = apply_opponent_split(state, split);
      const int value =
          evaluate_moves(split_state, stats, partnership, alpha, beta);
      result = std::min(result, value);
      beta = std::min(beta, result);
      if (alpha >= beta)
      {
        ++stats.alpha_beta_cutoffs;
        break;
      }
      if (try_goal_cutoff(stats, result))
      {
        break;
      }
    }
  }
  else
  {
    result = evaluate_moves(state, stats, partnership, alpha, beta);
  }

  stats.transposition_table.store(position, result, alpha_orig, beta_orig);
  return result;
}

auto apply_play(GameState state, const Card& card) -> GameState
{
  const int seat = current_player(state);
  remove_card(state.hands[static_cast<std::size_t>(seat)], card);
  if (seat == state.east_seat || seat == state.west_seat)
  {
    const auto pool_it = std::find(
        state.remaining_available.begin(),
        state.remaining_available.end(),
        card);
    if (pool_it != state.remaining_available.end())
    {
      state.remaining_available.erase(pool_it);
    }
  }
  if (!state.trick.empty())
  {
    const int lead_suit = state.trick.front().card.suit;
    if (card.suit != lead_suit)
    {
      mark_void_in_suit(state, seat, lead_suit);
    }
  }
  state.trick.push_back(PlayedCard{seat, card});
  if (seat == state.east_seat)
  {
    --state.east_slots;
  }
  else if (seat == state.west_seat)
  {
    --state.west_slots;
  }

  if (state.trick.size() == 4)
  {
    record_trick_win_ranks(state);
    const int winner = trick_winner(state.trick, state.denomination, state.leader);
    if (is_partnership_seat(winner, state.known_seat, state.dummy_seat))
    {
      ++state.partnership_tricks;
    }
    state.leader = winner;
    state.trick.clear();
  }
  return state;
}

auto make_initial_state(
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    int denomination,
    const std::vector<Card>& available,
    const std::array<int, kHands>& seat_void_suits,
    int min_significant_rank) -> GameState
{
  const auto [east_seat, west_seat] = opponent_seats(known_seat, dummy_seat);
  GameState state{};
  state.denomination = denomination;
  state.known_seat = known_seat;
  state.dummy_seat = dummy_seat;
  state.east_seat = east_seat;
  state.west_seat = west_seat;
  state.hands[static_cast<std::size_t>(known_seat)] = known;
  state.hands[static_cast<std::size_t>(dummy_seat)] = dummy;
  state.remaining_available = available;
  state.leader = known_seat;
  state.partnership_tricks = 0;
  state.east_slots = static_cast<int>(known.size());
  state.west_slots = static_cast<int>(known.size());
  state.void_suits = seat_void_suits;
  state.min_significant_rank = min_significant_rank;
  return state;
}

auto needs_adversarial_split_from_query(
    const AdversarialSplitQuery& query,
    int seat) -> bool
{
  GameState state{};
  state.known_seat = query.known_seat;
  state.dummy_seat = query.dummy_seat;
  state.east_seat = query.east_seat;
  state.west_seat = query.west_seat;
  state.east_slots = query.east_slots;
  state.west_slots = query.west_slots;
  state.hands = query.hands;
  state.remaining_available = query.remaining_available;
  return needs_adversarial_split(state, seat);
}

using SplitMixingSignature = std::array<std::pair<int, int>, kSuits>;

auto split_mixing_signature(
    const std::vector<Card>& east_cards,
    int min_significant_rank) -> SplitMixingSignature
{
  SplitMixingSignature signature{};
  for (const auto& card : east_cards)
  {
    if (card.rank >= min_significant_rank)
    {
      signature[static_cast<std::size_t>(card.suit)].first
          |= rank_bit(card.rank);
    }
    else
    {
      ++signature[static_cast<std::size_t>(card.suit)].second;
    }
  }
  return signature;
}

auto deduplicate_splits_by_mixing(
    std::vector<UnknownSplit> splits,
    int min_significant_rank) -> std::vector<UnknownSplit>
{
  if (min_significant_rank <= 2)
  {
    return splits;
  }
  std::map<SplitMixingSignature, UnknownSplit> unique;
  for (auto& split : splits)
  {
    unique.emplace(
        split_mixing_signature(split.first, min_significant_rank),
        std::move(split));
  }
  std::vector<UnknownSplit> deduplicated;
  deduplicated.reserve(unique.size());
  for (auto& entry : unique)
  {
    deduplicated.push_back(std::move(entry.second));
  }
  return deduplicated;
}

}  // namespace

auto needs_adversarial_split(const AdversarialSplitQuery& query, int seat)
    -> bool
{
  return needs_adversarial_split_from_query(query, seat);
}

auto distinct_representative_ranks_in_suit(
    int hand_mask,
    int unavailable_mask) -> std::vector<int>
{
  return representative_ranks_in_suit(hand_mask, unavailable_mask);
}

auto parse_card(std::string_view text) -> Card
{
  if (text.size() != 2)
  {
    throw std::invalid_argument("card must be two characters (suit+rank)");
  }
  const auto suit_pos = SuitChars.find(static_cast<char>(std::toupper(
      static_cast<unsigned char>(text[0]))));
  if (suit_pos == std::string_view::npos)
  {
    throw std::invalid_argument("invalid suit character");
  }
  return Card{
      static_cast<int>(suit_pos),
      char_to_rank(text[1]),
  };
}

auto parse_hand(std::string_view text) -> std::vector<Card>
{
  std::vector<Card> cards;
  int suit = 0;
  std::size_t start = 0;
  while (start <= text.size())
  {
    const auto dot = text.find('.', start);
    const auto field = text.substr(
        start,
        dot == std::string_view::npos ? std::string_view::npos : dot - start);
    if (suit >= kSuits)
    {
      throw std::invalid_argument("hand must have four suit fields separated by dots");
    }
    for (const char ch : field)
    {
      cards.push_back(Card{suit, char_to_rank(ch)});
    }
    ++suit;
    if (dot == std::string_view::npos)
    {
      break;
    }
    start = dot + 1;
  }
  if (suit != kSuits)
  {
    throw std::invalid_argument("hand must have four suit fields separated by dots");
  }
  return cards;
}

auto parse_seat(std::string_view text) -> int
{
  if (text.size() != 1)
  {
    throw std::invalid_argument("seat must be N/E/S/W");
  }
  const auto pos = SeatChars.find(static_cast<char>(std::toupper(
      static_cast<unsigned char>(text[0]))));
  if (pos == std::string_view::npos)
  {
    throw std::invalid_argument("invalid seat (use N/E/S/W)");
  }
  return static_cast<int>(pos);
}

auto parse_goal(std::string_view text) -> int
{
  if (text.empty())
  {
    throw std::invalid_argument("goal must be an integer");
  }
  try
  {
    std::size_t consumed = 0;
    const int value = std::stoi(std::string(text), &consumed);
    if (consumed != text.size())
    {
      throw std::invalid_argument("goal must be an integer");
    }
    return value;
  }
  catch (const std::invalid_argument&)
  {
    throw std::invalid_argument("goal must be an integer");
  }
  catch (const std::out_of_range&)
  {
    throw std::invalid_argument("goal must be an integer");
  }
}

auto parse_threads(std::string_view text) -> int
{
  if (text.empty())
  {
    throw std::invalid_argument("thread count must be an integer");
  }
  if (text == "0")
  {
    return default_thread_count();
  }
  try
  {
    std::size_t consumed = 0;
    const int value = std::stoi(std::string(text), &consumed);
    if (consumed != text.size())
    {
      throw std::invalid_argument("thread count must be an integer");
    }
    if (value < 1)
    {
      throw std::invalid_argument("thread count must be at least 1");
    }
    return value;
  }
  catch (const std::invalid_argument& ex)
  {
    if (std::string_view(ex.what()) == "thread count must be at least 1")
    {
      throw;
    }
    throw std::invalid_argument("thread count must be an integer");
  }
  catch (const std::out_of_range&)
  {
    throw std::invalid_argument("thread count must be an integer");
  }
}

auto default_thread_count() -> int
{
  const unsigned hardware = std::thread::hardware_concurrency();
  return hardware == 0 ? 1 : static_cast<int>(hardware);
}

auto parse_min_significant_rank(std::string_view text) -> int
{
  if (text.empty())
  {
    throw std::invalid_argument("min significant rank must be in 2..14");
  }
  int rank = 0;
  if (text.size() == 1)
  {
    rank = char_to_rank(text[0]);
  }
  else
  {
    try
    {
      std::size_t consumed = 0;
      rank = std::stoi(std::string(text), &consumed);
      if (consumed != text.size())
      {
        throw std::invalid_argument("min significant rank must be in 2..14");
      }
    }
    catch (const std::invalid_argument&)
    {
      throw std::invalid_argument("min significant rank must be in 2..14");
    }
    catch (const std::out_of_range&)
    {
      throw std::invalid_argument("min significant rank must be in 2..14");
    }
  }
  if (rank < 2 || rank > 14)
  {
    throw std::invalid_argument("min significant rank must be in 2..14");
  }
  return rank;
}

auto parse_denomination(std::string_view text) -> int
{
  std::string key(text);
  for (char& ch : key)
  {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  if (key == "S" || key == "0")
  {
    return 0;
  }
  if (key == "H" || key == "1")
  {
    return 1;
  }
  if (key == "D" || key == "2")
  {
    return 2;
  }
  if (key == "C" || key == "3")
  {
    return 3;
  }
  if (key == "N" || key == "NT" || key == "4")
  {
    return 4;
  }
  throw std::invalid_argument("invalid denomination (use S/H/D/C/NT)");
}

auto parse_void_suits(std::string_view text) -> int
{
  int mask = 0;
  for (const char ch : text)
  {
    if (ch == ',' || std::isspace(static_cast<unsigned char>(ch)))
    {
      continue;
    }
    const auto upper = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const auto pos = SuitChars.find(upper);
    if (pos == std::string_view::npos)
    {
      throw std::invalid_argument(
          "invalid void suit (use S/H/D/C, comma-separated)");
    }
    mask |= 1 << static_cast<int>(pos);
  }
  return mask;
}

auto validate_position(
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    const std::vector<Card>& available) -> void
{
  if (known_seat < 0 || known_seat >= kHands || dummy_seat < 0
      || dummy_seat >= kHands)
  {
    throw std::invalid_argument("seats must be in 0..3");
  }
  if (known_seat == dummy_seat)
  {
    throw std::invalid_argument("known and dummy seats must differ");
  }
  if ((known_seat ^ dummy_seat) != 2)
  {
    throw std::invalid_argument("dummy must be partner of the known hand");
  }
  if (known.size() != dummy.size())
  {
    throw std::invalid_argument("known and dummy must have the same number of cards");
  }
  if (known.empty() || known.size() > 13)
  {
    throw std::invalid_argument("each hand must have between 1 and 13 cards");
  }
  if (available.size() != 2 * known.size())
  {
    throw std::invalid_argument(
        "available pool must contain twice the known hand size");
  }

  std::vector<Card> all = known;
  all.insert(all.end(), dummy.begin(), dummy.end());
  all.insert(all.end(), available.begin(), available.end());
  std::sort(all.begin(), all.end());
  if (std::adjacent_find(all.begin(), all.end()) != all.end())
  {
    throw std::invalid_argument(
        "cards must be unique across known, dummy, and available");
  }
}

auto validate_seat_voids(
    const std::array<std::vector<Card>, kHands>& hands,
    const std::array<int, kHands>& seat_void_suits) -> void
{
  for (int seat = 0; seat < kHands; ++seat)
  {
    if (seat_void_suits[static_cast<std::size_t>(seat)] == 0)
    {
      continue;
    }
    for (const auto& card : hands[static_cast<std::size_t>(seat)])
    {
      if ((seat_void_suits[static_cast<std::size_t>(seat)] & (1 << card.suit)) != 0)
      {
        throw std::invalid_argument(
            "hand contains card in declared void suit for seat "
            + std::string(1, SeatChars[static_cast<std::size_t>(seat)]));
      }
    }
  }
}

auto enumerate_unknown_splits(
    const std::vector<Card>& pool,
    int hand_size,
    int min_significant_rank) -> std::vector<UnknownSplit>
{
  return enumerate_constrained_opponent_splits(
      pool,
      /*east_hand_size=*/0,
      /*west_hand_size=*/0,
      /*east_slots=*/hand_size,
      /*west_slots=*/hand_size,
      /*east_void_suits=*/0,
      /*west_void_suits=*/0,
      min_significant_rank);
}

auto enumerate_constrained_opponent_splits(
    const std::vector<Card>& unassigned,
    int east_hand_size,
    int west_hand_size,
    int east_slots,
    int west_slots,
    int east_void_suits,
    int west_void_suits,
    int min_significant_rank) -> std::vector<UnknownSplit>
{
  if (min_significant_rank < 2 || min_significant_rank > 14)
  {
    throw std::invalid_argument("min significant rank must be in 2..14");
  }
  const int east_need = east_slots - east_hand_size;
  const int west_need = west_slots - west_hand_size;
  if (east_need < 0 || west_need < 0)
  {
    throw std::invalid_argument("opponent slot counts are smaller than current hands");
  }
  if (east_need + west_need != static_cast<int>(unassigned.size()))
  {
    throw std::invalid_argument("unassigned pool does not match opponent slot needs");
  }
  if (east_need == 0 && west_need == 0)
  {
    return {UnknownSplit{{}, {}}};
  }

  auto ordered = unassigned;
  std::sort(ordered.begin(), ordered.end());
  const auto east_eligible = cards_eligible_for_seat(ordered, east_void_suits);
  if (static_cast<int>(east_eligible.size()) < east_need)
  {
    return {};
  }

  std::vector<std::vector<Card>> east_choices;
  std::vector<Card> current;
  choose_combinations(east_eligible, east_need, 0, current, east_choices);

  std::vector<UnknownSplit> splits;
  splits.reserve(east_choices.size());
  for (const auto& east_cards : east_choices)
  {
    const auto west_cards = complement(ordered, east_cards);
    if (static_cast<int>(west_cards.size()) != west_need)
    {
      continue;
    }
    if (std::any_of(
            west_cards.begin(),
            west_cards.end(),
            [&](const Card& card) {
              return (west_void_suits & (1 << card.suit)) != 0;
            }))
    {
      continue;
    }
    splits.push_back(UnknownSplit{east_cards, west_cards});
  }
  return deduplicate_splits_by_mixing(std::move(splits), min_significant_rank);
}

auto card_scores_from_future_tricks(
    const FutureTricks& fut,
    const std::vector<Card>& lead_cards) -> std::map<Card, int>
{
  std::map<Card, int> scores;
  for (int index = 0; index < fut.cards; ++index)
  {
    const int suit = fut.suit[index];
    const int score = fut.score[index];
    const int representative = fut.rank[index];

    const Card primary{suit, representative};
    if (std::find(lead_cards.begin(), lead_cards.end(), primary) != lead_cards.end())
    {
      scores[primary] = score;
    }

    for (int rank = 2; rank < representative; ++rank)
    {
      if (!equals_lists_rank(fut.equals[index], rank))
      {
        continue;
      }
      const Card equal_card{suit, rank};
      if (std::find(lead_cards.begin(), lead_cards.end(), equal_card)
          != lead_cards.end())
      {
        scores[equal_card] = score;
      }
    }
  }

  for (const auto& card : lead_cards)
  {
    if (!scores.contains(card))
    {
      throw std::runtime_error(
          "solve result missing score for lead card " + card_to_string(card));
    }
  }
  return scores;
}

struct LeadEvaluation
{
  Card lead_card{};
  int tricks = 0;
  EvaluationStats stats{};
};

auto evaluate_lead(
    const Card& lead_card,
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    int denomination,
    const std::vector<Card>& available,
    const std::array<int, kHands>& seat_void_suits,
    const std::vector<UnknownSplit>& root_splits,
    std::optional<int> goal,
    int min_significant_rank) -> LeadEvaluation
{
  LeadEvaluation result{};
  result.lead_card = lead_card;
  result.stats.goal = goal;
  auto state = make_initial_state(
      known,
      dummy,
      known_seat,
      dummy_seat,
      denomination,
      available,
      seat_void_suits,
      min_significant_rank);
  remove_card(state.hands[static_cast<std::size_t>(known_seat)], lead_card);
  state.trick.push_back(PlayedCard{known_seat, lead_card});
  if (state.trick.size() == 4)
  {
    const int winner = trick_winner(state.trick, state.denomination, state.leader);
    if (is_partnership_seat(winner, state.known_seat, state.dummy_seat))
    {
      ++state.partnership_tricks;
    }
    state.leader = winner;
    state.trick.clear();
  }

  int tricks = std::numeric_limits<int>::max();
  for (const auto& split : root_splits)
  {
    const auto split_state = apply_opponent_split(state, split);
    tricks = std::min(
        tricks,
        evaluate_recursive(
            split_state,
            result.stats,
            std::numeric_limits<int>::min(),
            std::numeric_limits<int>::max()));
    if (try_goal_cutoff(result.stats, tricks))
    {
      break;
    }
  }
  result.tricks = tricks;
  return result;
}

auto solve_single_dummy(
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    int denomination,
    const std::vector<Card>& available,
    std::array<int, kHands> seat_void_suits,
    std::optional<int> goal,
    int thread_count,
    int min_significant_rank) -> SingleDummyResult
{
  validate_position(known, dummy, known_seat, dummy_seat, available);
  if (denomination < 0 || denomination > 4)
  {
    throw std::invalid_argument("denomination must be in 0..4");
  }
  if (goal.has_value() && (*goal < 0 || *goal > 13))
  {
    throw std::invalid_argument("goal must be in 0..13");
  }
  if (min_significant_rank < 2 || min_significant_rank > 14)
  {
    throw std::invalid_argument("min significant rank must be in 2..14");
  }
  if (thread_count == 0)
  {
    thread_count = default_thread_count();
  }
  if (thread_count < 1)
  {
    throw std::invalid_argument("thread count must be at least 1");
  }

  std::array<std::vector<Card>, kHands> hands{};
  hands[static_cast<std::size_t>(known_seat)] = known;
  hands[static_cast<std::size_t>(dummy_seat)] = dummy;
  validate_seat_voids(hands, seat_void_suits);

  const auto [east_seat, west_seat] = opponent_seats(known_seat, dummy_seat);
  const auto root_splits = enumerate_constrained_opponent_splits(
      available,
      /*east_hand_size=*/0,
      /*west_hand_size=*/0,
      static_cast<int>(known.size()),
      static_cast<int>(known.size()),
      seat_void_suits[static_cast<std::size_t>(east_seat)],
      seat_void_suits[static_cast<std::size_t>(west_seat)],
      min_significant_rank);
  if (root_splits.empty())
  {
    throw std::invalid_argument(
        "no legal initial opposition split for available cards and void suits");
  }

  std::map<Card, int> values;
  std::vector<std::map<Card, int>> matrix;
  int total_opposition_splits = 0;
  int total_alpha_beta_cutoffs = 0;
  int total_isomorphism_skipped = 0;
  int total_transposition_hits = 0;
  int total_transposition_bound_cutoffs = 0;
  int total_goal_cutoffs = 0;

  std::vector<LeadEvaluation> lead_results(known.size());
  const auto evaluate_index = [&](std::size_t index) {
    lead_results[index] = evaluate_lead(
        known[index],
        known,
        dummy,
        known_seat,
        dummy_seat,
        denomination,
        available,
        seat_void_suits,
        root_splits,
        goal,
        min_significant_rank);
  };

  if (thread_count == 1 || known.size() <= 1)
  {
    for (std::size_t index = 0; index < known.size(); ++index)
    {
      evaluate_index(index);
    }
  }
  else
  {
    const int workers = std::min(
        thread_count,
        static_cast<int>(known.size()));
    std::atomic<std::size_t> next_index{0};
    std::exception_ptr error;
    std::mutex error_mutex;
    std::vector<std::thread> worker_threads;
    worker_threads.reserve(static_cast<std::size_t>(workers));
    for (int worker = 0; worker < workers; ++worker)
    {
      worker_threads.emplace_back([&]() {
        while (true)
        {
          const std::size_t index = next_index.fetch_add(1);
          if (index >= known.size())
          {
            return;
          }
          try
          {
            evaluate_index(index);
          }
          catch (...)
          {
            const std::lock_guard lock(error_mutex);
            if (!error)
            {
              error = std::current_exception();
            }
          }
        }
      });
    }
    for (auto& worker_thread : worker_threads)
    {
      worker_thread.join();
    }
    if (error)
    {
      std::rethrow_exception(error);
    }
  }

  for (const auto& lead_result : lead_results)
  {
    total_opposition_splits += lead_result.stats.opposition_split_evaluations;
    total_alpha_beta_cutoffs += lead_result.stats.alpha_beta_cutoffs;
    total_isomorphism_skipped += lead_result.stats.isomorphism_skipped_moves;
    total_transposition_hits += lead_result.stats.transposition_table.hits();
    total_transposition_bound_cutoffs +=
        lead_result.stats.transposition_table.bound_cutoffs();
    total_goal_cutoffs += lead_result.stats.goal_cutoffs;
    values[lead_result.lead_card] = lead_result.tricks;
    matrix.push_back({{lead_result.lead_card, lead_result.tricks}});
  }

  SingleDummyResult result{};
  result.combination_count = static_cast<int>(root_splits.size());
  result.opposition_split_evaluations = total_opposition_splits;
  result.alpha_beta_cutoffs = total_alpha_beta_cutoffs;
  result.isomorphism_skipped_moves = total_isomorphism_skipped;
  result.transposition_hits = total_transposition_hits;
  result.transposition_bound_cutoffs = total_transposition_bound_cutoffs;
  result.goal_cutoffs = total_goal_cutoffs;
  result.goal = goal;
  result.trick_matrix = std::move(matrix);

  auto ordered_known = known;
  std::sort(ordered_known.begin(), ordered_known.end());
  result.leads.reserve(ordered_known.size());
  for (const auto& card : ordered_known)
  {
    result.leads.push_back(LeadResult{card, values.at(card)});
  }

  result.maximin_value = 0;
  for (const auto& lead : result.leads)
  {
    result.maximin_value = std::max(result.maximin_value, lead.min_tricks);
  }
  for (const auto& lead : result.leads)
  {
    if (lead.min_tricks == result.maximin_value)
    {
      result.maximin_cards.push_back(lead.card);
    }
  }
  return result;
}

auto format_result(const SingleDummyResult& result) -> std::string
{
  if (result.goal.has_value() && result.maximin_value < *result.goal)
  {
    std::ostringstream out;
    out << *result.goal << " tricks unachieveable\n";
    return out.str();
  }

  std::ostringstream out;
  out << "combinations: " << result.combination_count << '\n';
  out << "opposition_splits: " << result.opposition_split_evaluations << '\n';
  out << "alpha_beta_cutoffs: " << result.alpha_beta_cutoffs << '\n';
  out << "isomorphism_skipped: " << result.isomorphism_skipped_moves << '\n';
  out << "transposition_hits: " << result.transposition_hits << '\n';
  out << "transposition_bound_cutoffs: "
      << result.transposition_bound_cutoffs << '\n';
  if (result.goal.has_value())
  {
    out << "goal_cutoffs: " << result.goal_cutoffs << '\n';
  }
  out << "maximin: " << result.maximin_value << " (";
  for (std::size_t index = 0; index < result.maximin_cards.size(); ++index)
  {
    if (index > 0)
    {
      out << ", ";
    }
    out << card_to_string(result.maximin_cards[index]);
  }
  out << ")\n";
  out << "card  min_tricks\n";
  for (const auto& lead : result.leads)
  {
    out << card_to_string(lead.card);
    if (lead.min_tricks == result.maximin_value)
    {
      out << " *";
    }
    out << "  " << lead.min_tricks << '\n';
  }
  return out.str();
}

auto format_position_diagram(
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    const std::vector<Card>& available,
    int denomination,
    const std::array<int, kHands>& seat_void_suits) -> std::string
{
  static constexpr std::array<std::string_view, kHands> seat_names =
      {"North", "East", "South", "West"};
  static constexpr std::array<std::string_view, 5> denom_labels =
      {"S", "H", "D", "C", "NT"};

  std::ostringstream out;
  out << "Trump: " << denom_labels[static_cast<std::size_t>(denomination)]
      << "\n\n";

  const auto known_fields = hand_to_suit_fields(known);
  const auto dummy_fields = hand_to_suit_fields(dummy);
  const auto available_fields = hand_to_suit_fields(available);

  std::vector<int> opposition;
  for (int seat = 0; seat < kHands; ++seat)
  {
    if (seat != known_seat && seat != dummy_seat)
    {
      opposition.push_back(seat);
    }
  }

  for (int seat = 0; seat < kHands; ++seat)
  {
    out << seat_names[static_cast<std::size_t>(seat)];
    if (seat == known_seat)
    {
      out << " (known)";
    }
    else if (seat == dummy_seat)
    {
      out << " (dummy)";
    }
    else
    {
      out << " (opposition)";
    }
    append_void_suffix(
        seat_void_suits[static_cast<std::size_t>(seat)],
        out);
    out << '\n';

    if (seat == known_seat)
    {
      append_hand_lines(known_fields, out);
    }
    else if (seat == dummy_seat)
    {
      append_hand_lines(dummy_fields, out);
    }
    else
    {
      out << '\n';
    }
  }

  out << '\n';
  out << "Available (";
  for (std::size_t index = 0; index < opposition.size(); ++index)
  {
    if (index > 0)
    {
      out << ", ";
    }
    out << seat_names[static_cast<std::size_t>(opposition[index])];
  }
  out << "):\n";
  append_hand_lines(available_fields, out);
  return out.str();
}

}  // namespace single_dummy_solver
