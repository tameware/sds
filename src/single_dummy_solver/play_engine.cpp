#include "play_engine.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace single_dummy_solver::detail
{
namespace
{

constexpr int kRankBitmaskAll = 0x1FFF;

auto rank_bit(int rank) -> int
{
  return 1 << (rank - 2);
}

auto card_to_string(const Card& card) -> std::string
{
  constexpr char suits[] = "SHDC";
  constexpr char ranks[] = "23456789TJQKA";
  return std::string{suits[card.suit], ranks[card.rank - 2]};
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
  for (const auto& card : state.remaining_available)
  {
    if (card.suit == suit)
    {
      mask |= rank_bit(card.rank);
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
  int top_rank = 0;
  int gap = 0;
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

auto representative_ranks_in_suit(int hand_mask, int unavailable_mask)
    -> std::vector<int>
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
  for (const int rank : representative_ranks_in_suit(hand_mask, unavailable_mask))
  {
    moves.push_back(Card{suit, rank});
  }
}

auto opponent_eligible_cards(const GameState& state, int seat) -> std::vector<Card>
{
  const int voids = state.void_suits[static_cast<std::size_t>(seat)];
  std::vector<Card> eligible;
  for (const auto& card : state.remaining_available)
  {
    if ((voids & (1 << card.suit)) != 0)
    {
      continue;
    }
    bool pinned_elsewhere = false;
    for (const auto& pin : state.pins)
    {
      if (pin.suit == card.suit && pin.rank == card.rank && pin.seat != seat)
      {
        pinned_elsewhere = true;
        break;
      }
    }
    if (!pinned_elsewhere)
    {
      eligible.push_back(card);
    }
  }
  return eligible;
}

auto opposition_slots_feasible(const GameState& state) -> bool
{
  if (state.east_slots + state.west_slots
      != static_cast<int>(state.remaining_available.size()))
  {
    return false;
  }
  return static_cast<int>(opponent_eligible_cards(state, state.east_seat).size())
      >= state.east_slots
      && static_cast<int>(opponent_eligible_cards(state, state.west_seat).size())
          >= state.west_slots;
}

auto holding_for_legal_moves(const GameState& state, int seat) -> std::vector<Card>
{
  if (is_opponent_seat(state, seat))
  {
    return opponent_eligible_cards(state, seat);
  }
  return state.hands[static_cast<std::size_t>(seat)];
}

auto distinct_legal_cards(const GameState& state, int seat) -> std::vector<Card>
{
  const auto hand = holding_for_legal_moves(state, seat);
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
  if (hand_mask_in_suit(hand, lead_suit) != 0)
  {
    std::vector<Card> moves;
    append_distinct_cards_in_suit(
        hand, lead_suit, unavailable_mask_in_suit(state, lead_suit), moves);
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

auto legal_cards(const GameState& state, int seat) -> std::vector<Card>
{
  const auto hand = holding_for_legal_moves(state, seat);
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

auto winning_rank_mask_in_suit(int winning_rank, int aggregate) -> int
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
  const int winner_seat =
      trick_winner(state.trick, state.denomination, state.leader);
  int winner_index = 0;
  for (int index = 0; index < static_cast<int>(state.trick.size()); ++index)
  {
    if ((state.leader + index) % kHands == winner_seat)
    {
      winner_index = index;
      break;
    }
  }
  const auto& winning_card =
      state.trick[static_cast<std::size_t>(winner_index)].card;
  const int lead_suit = state.trick.front().card.suit;
  const bool trump_contract = state.denomination != 4;
  const bool wins_by_trump =
      trump_contract && winning_card.suit == state.denomination;
  const bool wins_by_lead = !wins_by_trump && winning_card.suit == lead_suit;
  if (!wins_by_trump && !wins_by_lead)
  {
    return;
  }
  const int suit = winning_card.suit;
  state.win_ranks[static_cast<std::size_t>(suit)] |= winning_rank_mask_in_suit(
      winning_card.rank, held_mask_in_suit(state, suit));
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

auto mark_void_in_suit(GameState& state, int seat, int suit) -> void
{
  state.void_suits[static_cast<std::size_t>(seat)] |= 1 << suit;
}

// Relative strength of a card vs current trick (for move ordering).
auto beats_current_winner(const GameState& state, const Card& card) -> bool
{
  if (state.trick.empty())
  {
    return true;
  }
  std::vector<PlayedCard> probe = state.trick;
  probe.push_back(PlayedCard{current_player(state), card});
  const int winner =
      trick_winner(probe, state.denomination, state.leader);
  return is_partnership_seat(winner, state.known_seat)
      == is_partnership_seat(current_player(state), state.known_seat);
}

}  // namespace

auto partner_seat(int seat) -> int
{
  return seat ^ 2;
}

auto is_partnership_seat(int seat, int known_seat) -> bool
{
  return seat == known_seat || seat == partner_seat(known_seat);
}

auto is_opponent_seat(const GameState& state, int seat) -> bool
{
  return seat == state.east_seat || seat == state.west_seat;
}

auto current_player(const GameState& state) -> int
{
  return (state.leader + static_cast<int>(state.trick.size())) % kHands;
}

auto play_complete(const GameState& state) -> bool
{
  if (!state.trick.empty() || !state.remaining_available.empty())
  {
    return false;
  }
  for (const auto& hand : state.hands)
  {
    if (!hand.empty())
    {
      return false;
    }
  }
  return true;
}

auto remaining_tricks_upper_bound(const GameState& state) -> int
{
  // Cards still in the current trick are still "in play" for this trick.
  const int cards_in_play =
      static_cast<int>(state.remaining_available.size())
      + static_cast<int>(
            state.hands[static_cast<std::size_t>(state.known_seat)].size())
      + static_cast<int>(
            state.hands[static_cast<std::size_t>(state.dummy_seat)].size())
      + static_cast<int>(state.trick.size());
  return (cards_in_play + 3) / 4;
}

auto apply_play(GameState state, const Card& card) -> GameState
{
  const int seat = current_player(state);
  if (is_opponent_seat(state, seat))
  {
    remove_card(state.remaining_available, card);
  }
  else
  {
    remove_card(state.hands[static_cast<std::size_t>(seat)], card);
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
    const int winner =
        trick_winner(state.trick, state.denomination, state.leader);
    if (is_partnership_seat(winner, state.known_seat))
    {
      ++state.partnership_tricks;
    }
    state.leader = winner;
    state.trick.clear();
  }
  return state;
}

auto ordered_legal_moves(const GameState& state, int seat) -> std::vector<Card>
{
  auto moves = distinct_legal_cards(state, seat);
  if (is_opponent_seat(state, seat))
  {
    std::vector<Card> feasible;
    for (const auto& card : moves)
    {
      if (opposition_slots_feasible(apply_play(state, card)))
      {
        feasible.push_back(card);
      }
    }
    moves = std::move(feasible);
  }

  // Heuristic ordering inspired by bridge-solver OrderCards: prefer cards that
  // currently win the trick for the side to play; otherwise prefer low cards.
  const bool maximize = is_partnership_seat(seat, state.known_seat);
  std::stable_sort(
      moves.begin(),
      moves.end(),
      [&](const Card& left, const Card& right) {
        const bool left_wins = beats_current_winner(state, left);
        const bool right_wins = beats_current_winner(state, right);
        if (left_wins != right_wins)
        {
          return maximize ? left_wins : !left_wins;
        }
        if (left.suit != right.suit)
        {
          return left.suit < right.suit;
        }
        return maximize ? left.rank > right.rank : left.rank < right.rank;
      });
  return moves;
}

auto count_isomorphism_skipped(const GameState& state, int seat) -> int
{
  return static_cast<int>(legal_cards(state, seat).size())
      - static_cast<int>(distinct_legal_cards(state, seat).size());
}

auto distinct_representative_ranks_in_suit_impl(
    int hand_mask,
    int unavailable_mask) -> std::vector<int>
{
  return representative_ranks_in_suit(hand_mask, unavailable_mask);
}

}  // namespace single_dummy_solver::detail

namespace single_dummy_solver
{

auto distinct_representative_ranks_in_suit(
    int hand_mask,
    int unavailable_mask) -> std::vector<int>
{
  return detail::distinct_representative_ranks_in_suit_impl(
      hand_mask, unavailable_mask);
}

}  // namespace single_dummy_solver
