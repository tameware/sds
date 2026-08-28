/*
   DDS, a bridge double dummy solver.

   Copyright (C) 2006-2014 by Bo Haglund /
   2014-2016 by Bo Haglund & Soren Hein.

   See LICENSE and README.
*/

#include "single_dummy_solver.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace single_dummy_solver
{

auto enumerate_constrained_opponent_splits(
    const std::vector<Card>& unassigned,
    int east_hand_size,
    int west_hand_size,
    int east_slots,
    int west_slots,
    int east_void_suits,
    int west_void_suits) -> std::vector<UnknownSplit>;

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
};

struct EvaluationStats
{
  int opposition_split_evaluations = 0;
  int alpha_beta_cutoffs = 0;
  std::unordered_map<std::string, int> memo;
};

auto state_key(const GameState& state) -> std::string
{
  std::ostringstream key;
  key << state.partnership_tricks << '|' << state.east_slots << '|' << state.west_slots;
  for (const int void_mask : state.void_suits)
  {
    key << '|' << void_mask;
  }
  key << '|' << state.leader << '|' << state.trick.size();
  for (const auto& played : state.trick)
  {
    key << '|' << played.seat << ',' << played.card.suit << ','
        << played.card.rank;
  }
  for (const auto& hand : state.hands)
  {
    key << '|';
    for (const auto& card : hand)
    {
      key << card.suit << ',' << card.rank << ';';
    }
  }
  key << '|';
  for (const auto& card : state.remaining_available)
  {
    key << card.suit << ',' << card.rank << ';';
  }
  return key.str();
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
  return false;
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
      state.void_suits[static_cast<std::size_t>(state.west_seat)]);
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
  const auto moves = legal_cards(state, seat);
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

  const std::string key = state_key(state);
  if (const auto cached = stats.memo.find(key); cached != stats.memo.end())
  {
    return cached->second;
  }

  const int seat = current_player(state);
  const bool partnership =
      is_partnership_seat(seat, state.known_seat, state.dummy_seat);
  if (needs_adversarial_split(state, seat))
  {
    const auto splits = adversarial_splits(state);
    if (splits.empty())
    {
      throw std::invalid_argument(
          "no legal opposition split for remaining cards and void suits");
    }
    int result = std::numeric_limits<int>::max();
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
    }
    stats.memo[key] = result;
    return result;
  }

  const int result = evaluate_moves(state, stats, partnership, alpha, beta);
  stats.memo[key] = result;
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
    const std::array<int, kHands>& seat_void_suits) -> GameState
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
  return state;
}

}  // namespace

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
    int hand_size) -> std::vector<UnknownSplit>
{
  return enumerate_constrained_opponent_splits(
      pool,
      /*east_hand_size=*/0,
      /*west_hand_size=*/0,
      /*east_slots=*/hand_size,
      /*west_slots=*/hand_size);
}

auto enumerate_constrained_opponent_splits(
    const std::vector<Card>& unassigned,
    int east_hand_size,
    int west_hand_size,
    int east_slots,
    int west_slots,
    int east_void_suits,
    int west_void_suits) -> std::vector<UnknownSplit>
{
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
  return splits;
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

auto solve_single_dummy(
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    int denomination,
    const std::vector<Card>& available,
    std::array<int, kHands> seat_void_suits) -> SingleDummyResult
{
  validate_position(known, dummy, known_seat, dummy_seat, available);
  if (denomination < 0 || denomination > 4)
  {
    throw std::invalid_argument("denomination must be in 0..4");
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
      seat_void_suits[static_cast<std::size_t>(west_seat)]);
  if (root_splits.empty())
  {
    throw std::invalid_argument(
        "no legal initial opposition split for available cards and void suits");
  }

  std::map<Card, int> values;
  std::vector<std::map<Card, int>> matrix;
  int total_opposition_splits = 0;
  int total_alpha_beta_cutoffs = 0;

  for (const auto& lead_card : known)
  {
    EvaluationStats stats{};
    auto state = make_initial_state(
        known,
        dummy,
        known_seat,
        dummy_seat,
        denomination,
        available,
        seat_void_suits);
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
              stats,
              std::numeric_limits<int>::min(),
              std::numeric_limits<int>::max()));
    }
    total_opposition_splits += stats.opposition_split_evaluations;
    total_alpha_beta_cutoffs += stats.alpha_beta_cutoffs;
    values[lead_card] = tricks;
    matrix.push_back({{lead_card, tricks}});
  }

  SingleDummyResult result{};
  result.combination_count = static_cast<int>(root_splits.size());
  result.opposition_split_evaluations = total_opposition_splits;
  result.alpha_beta_cutoffs = total_alpha_beta_cutoffs;
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
  std::ostringstream out;
  out << "combinations: " << result.combination_count << '\n';
  out << "opposition_splits: " << result.opposition_split_evaluations << '\n';
  out << "alpha_beta_cutoffs: " << result.alpha_beta_cutoffs << '\n';
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

}  // namespace single_dummy_solver
