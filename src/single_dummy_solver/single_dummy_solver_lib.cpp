// SDS single-dummy solver (shared-pool opposition play).
// Search algorithms adapted from macroxue/bridge-solver (Apache-2.0 OR MIT).

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

#include "bridge_search.hpp"
#include "deal_constraints.hpp"
#include "game_state.hpp"
#include "play_engine.hpp"

namespace single_dummy_solver
{

auto enumerate_constrained_opponent_splits(
    const std::vector<Card>& unassigned,
    int east_hand_size,
    int west_hand_size,
    int east_slots,
    int west_slots,
    const std::array<std::vector<Card>, kHands>& base_hands,
    int east_seat,
    int west_seat,
    int east_void_suits,
    int west_void_suits,
    const DealConstraint* constraint,
    const DealConstraint* pin_constraint,
    int min_significant_rank,
    const std::vector<DealConstraintPin>& extra_pins) -> std::vector<UnknownSplit>;

namespace
{

constexpr std::string_view RankChars = "23456789TJQKA";
constexpr std::string_view SuitChars = "SHDC";
constexpr std::string_view SeatChars = "NESW";

auto split_satisfies_pins(
    const std::vector<DealConstraintPin>& pins,
    const std::vector<Card>& pool,
    int east_seat,
    int west_seat,
    const std::vector<Card>& east_cards,
    const std::vector<Card>& west_cards) -> bool
{
  for (const auto& pin : pins)
  {
    const Card pinned{pin.suit, pin.rank};
    if (!std::binary_search(pool.begin(), pool.end(), pinned))
    {
      continue;
    }
    const auto& seat_cards =
        pin.seat == east_seat ? east_cards : west_cards;
    if (!std::binary_search(seat_cards.begin(), seat_cards.end(), pinned))
    {
      return false;
    }
  }
  return true;
}

auto pins_from_constraint(const DealConstraint* pin_constraint)
    -> std::vector<DealConstraintPin>
{
  if (pin_constraint == nullptr || !pin_constraint->is_active())
  {
    return {};
  }
  return pin_constraint->pinned_cards();
}

using detail::apply_play;
using detail::is_partnership_seat;
using detail::ordered_legal_moves;

auto rank_bit(int rank) -> int
{
  return 1 << (rank - 2);
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

auto erase_card(std::vector<Card>& hand, const Card& card) -> void
{
  const auto it = std::find(hand.begin(), hand.end(), card);
  if (it == hand.end())
  {
    throw std::runtime_error("card not in hand: " + card_to_string(card));
  }
  hand.erase(it);
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

auto format_pbn_suit_fields(
    const std::array<std::string, kSuits>& fields) -> std::string
{
  std::ostringstream out;
  for (int suit = 0; suit < kSuits; ++suit)
  {
    if (suit > 0)
    {
      out << '.';
    }
    out << fields[static_cast<std::size_t>(suit)];
  }
  return out.str();
}

auto make_initial_state(
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    int denomination,
    const std::vector<Card>& available,
    const DealConstraint& deal_constraint,
    int /*min_significant_rank*/) -> GameState
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
  state.pins = deal_constraint.pinned_cards();
  for (const auto& void_suit : deal_constraint.void_suits())
  {
    state.void_suits[static_cast<std::size_t>(void_suit.seat)] |=
        1 << void_suit.suit;
  }
  return state;
}

auto needs_adversarial_split_from_query(
    const AdversarialSplitQuery& query,
    int seat) -> bool
{
  (void)query;
  (void)seat;
  // Shared-pool play never repartitions opposition hands.
  return false;
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

auto parse_opening_lead(std::string_view text) -> OpeningLead
{
  if (text.size() < 3)
  {
    throw std::invalid_argument("opening lead must be seat plus card (e.g. WH4)");
  }
  OpeningLead lead{};
  lead.seat = parse_seat(text.substr(0, 1));
  lead.card = parse_card(text.substr(1));
  return lead;
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

auto available_cards_for_opponents(
    const std::vector<Card>& known,
    const std::vector<Card>& dummy) -> std::vector<Card>
{
  constexpr int kFullHandSize = 13;
  if (known.size() != kFullHandSize || dummy.size() != kFullHandSize)
  {
    throw std::invalid_argument(
        "available pool is implicit only when known and dummy each have 13 cards");
  }

  std::vector<Card> remaining;
  remaining.reserve(26);
  for (int suit = 0; suit < kSuits; ++suit)
  {
    for (int rank = 2; rank <= 14; ++rank)
    {
      remaining.push_back(Card{suit, rank});
    }
  }

  const auto remove_card = [](std::vector<Card>& cards, const Card& target) -> bool {
    const auto it = std::find(cards.begin(), cards.end(), target);
    if (it == cards.end())
    {
      return false;
    }
    cards.erase(it);
    return true;
  };

  for (const auto& card : known)
  {
    if (!remove_card(remaining, card))
    {
      throw std::invalid_argument(
          "known and dummy must contain 26 distinct cards from a full deck");
    }
  }
  for (const auto& card : dummy)
  {
    if (!remove_card(remaining, card))
    {
      throw std::invalid_argument(
          "known and dummy must contain 26 distinct cards from a full deck");
    }
  }
  if (remaining.size() != 26)
  {
    throw std::invalid_argument(
        "known and dummy must contain 26 distinct cards from a full deck");
  }
  return remaining;
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

auto validate_opening_lead(
    const OpeningLead& opening_lead,
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    const std::vector<Card>& available) -> void
{
  const auto contains = [](const std::vector<Card>& cards, const Card& target) {
    return std::find(cards.begin(), cards.end(), target) != cards.end();
  };
  if (opening_lead.seat == known_seat)
  {
    if (!contains(known, opening_lead.card))
    {
      throw std::invalid_argument("opening lead card is not in the known hand");
    }
    return;
  }
  if (opening_lead.seat == dummy_seat)
  {
    if (!contains(dummy, opening_lead.card))
    {
      throw std::invalid_argument("opening lead card is not in the dummy hand");
    }
    return;
  }
  if (contains(known, opening_lead.card) || contains(dummy, opening_lead.card))
  {
    throw std::invalid_argument(
        "opening lead card must come from the available pool for opposition seats");
  }
  if (!contains(available, opening_lead.card))
  {
    throw std::invalid_argument("opening lead card is not in the available pool");
  }
}

auto apply_opening_lead(GameState state, const OpeningLead& opening_lead)
    -> GameState
{
  state.leader = opening_lead.seat;
  return apply_play(state, opening_lead.card);
}

auto hands_without_opening_lead(
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    const OpeningLead& opening_lead)
    -> std::pair<std::vector<Card>, std::vector<Card>>
{
  auto known_hand = known;
  auto dummy_hand = dummy;
  if (opening_lead.seat == known_seat)
  {
    erase_card(known_hand, opening_lead.card);
  }
  else if (opening_lead.seat == dummy_seat)
  {
    erase_card(dummy_hand, opening_lead.card);
  }
  return {known_hand, dummy_hand};
}

auto hands_for_opponent_split(
    const std::array<std::vector<Card>, kHands>& base_hands,
    int east_seat,
    int west_seat,
    const std::vector<Card>& east_cards,
    const std::vector<Card>& west_cards) -> std::array<std::vector<Card>, kHands>
{
  auto hands = base_hands;
  hands[static_cast<std::size_t>(east_seat)] = east_cards;
  hands[static_cast<std::size_t>(west_seat)] = west_cards;
  return hands;
}

auto enumerate_unknown_splits(
    const std::vector<Card>& pool,
    int hand_size,
    int min_significant_rank) -> std::vector<UnknownSplit>
{
  std::array<std::vector<Card>, kHands> base_hands{};
  return enumerate_constrained_opponent_splits(
      pool,
      /*east_hand_size=*/0,
      /*west_hand_size=*/0,
      /*east_slots=*/hand_size,
      /*west_slots=*/hand_size,
      base_hands,
      /*east_seat=*/1,
      /*west_seat=*/3,
      /*east_void_suits=*/0,
      /*west_void_suits=*/0,
      /*constraint=*/nullptr,
      /*pin_constraint=*/nullptr,
      min_significant_rank,
      {});
}

auto enumerate_constrained_opponent_splits(
    const std::vector<Card>& unassigned,
    int east_hand_size,
    int west_hand_size,
    int east_slots,
    int west_slots,
    const std::array<std::vector<Card>, kHands>& base_hands,
    int east_seat,
    int west_seat,
    int east_void_suits,
    int west_void_suits,
    const DealConstraint* constraint,
    const DealConstraint* pin_constraint,
    int min_significant_rank,
    const std::vector<DealConstraintPin>& extra_pins) -> std::vector<UnknownSplit>
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
  auto pins = pins_from_constraint(pin_constraint);
  pins.insert(pins.end(), extra_pins.begin(), extra_pins.end());
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
    if (!pins.empty()
        && !split_satisfies_pins(
            pins, ordered, east_seat, west_seat, east_cards, west_cards))
    {
      continue;
    }
    if (constraint != nullptr)
    {
      const auto hands = hands_for_opponent_split(
          base_hands, east_seat, west_seat, east_cards, west_cards);
      if (!constraint->evaluate(hands))
      {
        continue;
      }
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
  bool tricks_upper_bound = false;
  SearchStats stats{};
};

auto evaluate_lead(
    const Card& lead_card,
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    int denomination,
    const std::vector<Card>& available,
    const DealConstraint& deal_constraint,
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
      deal_constraint,
      min_significant_rank);

  state.force_next_play = true;
  state.forced_play_seat = known_seat;
  state.forced_play = lead_card;
  const int tricks = search_partnership_tricks(state, result.stats);
  result.tricks = tricks;
  result.tricks_upper_bound =
      goal.has_value() && result.stats.goal_cutoffs > 0 && tricks < *goal;
  return result;
}

auto seat_after_lead(int leader_seat) -> int
{
  return (leader_seat + 1) % kHands;
}

auto first_partnership_seat_to_play(int leader_seat, int known_seat) -> int
{
  int seat = seat_after_lead(leader_seat);
  for (int step = 0; step < kHands; ++step)
  {
    if (is_partnership_seat(seat, known_seat))
    {
      return seat;
    }
    seat = seat_after_lead(seat);
  }
  throw std::invalid_argument("no partnership seat plays after opening lead");
}

auto probe_state_after_opening_lead(
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    int denomination,
    const std::vector<Card>& available,
    const DealConstraint& deal_constraint,
    const OpeningLead& opening_lead,
    int min_significant_rank) -> GameState
{
  const auto [known_hand, dummy_hand] = hands_without_opening_lead(
      known,
      dummy,
      known_seat,
      dummy_seat,
      opening_lead);
  auto state = make_initial_state(
      known_hand,
      dummy_hand,
      known_seat,
      dummy_seat,
      denomination,
      available,
      deal_constraint,
      min_significant_rank);
  state.leader = opening_lead.seat;
  state.trick.push_back(PlayedCard{opening_lead.seat, opening_lead.card});
  return state;
}

auto legal_response_cards_after_opening_lead(
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    int denomination,
    const std::vector<Card>& available,
    const DealConstraint& deal_constraint,
    const OpeningLead& opening_lead,
    int response_seat,
    int min_significant_rank) -> std::vector<Card>
{
  const auto probe = probe_state_after_opening_lead(
      known,
      dummy,
      known_seat,
      dummy_seat,
      denomination,
      available,
      deal_constraint,
      opening_lead,
      min_significant_rank);
  return ordered_legal_moves(probe, response_seat);
}

auto hand_for_seat(
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    int seat) -> const std::vector<Card>&
{
  if (seat == known_seat)
  {
    return known;
  }
  if (seat == dummy_seat)
  {
    return dummy;
  }
  throw std::invalid_argument("seat is not a partnership hand");
}

auto evaluate_after_opening_lead(
    const Card& play_card,
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    int denomination,
    const std::vector<Card>& available,
    const DealConstraint& deal_constraint,
    const OpeningLead& opening_lead,
    int response_seat,
    std::optional<int> goal,
    int min_significant_rank) -> LeadEvaluation
{
  LeadEvaluation result{};
  result.lead_card = play_card;
  result.stats.goal = goal;

  auto state = make_initial_state(
      known,
      dummy,
      known_seat,
      dummy_seat,
      denomination,
      available,
      deal_constraint,
      min_significant_rank);
  state = apply_opening_lead(state, opening_lead);

  state.force_next_play = true;
  state.forced_play_seat = response_seat;
  state.forced_play = play_card;
  const int tricks = search_partnership_tricks(state, result.stats);
  result.tricks = tricks;
  result.tricks_upper_bound =
      goal.has_value() && result.stats.goal_cutoffs > 0 && tricks < *goal;
  return result;
}

auto solve_single_dummy(
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    int denomination,
    const std::vector<Card>& available,
    std::optional<std::string> constraint_text,
    std::optional<int> goal,
    int thread_count,
    int min_significant_rank,
    std::optional<OpeningLead> opening_lead) -> SingleDummyResult
{
  validate_position(known, dummy, known_seat, dummy_seat, available);
  if (opening_lead.has_value())
  {
    validate_opening_lead(
        *opening_lead,
        known,
        dummy,
        known_seat,
        dummy_seat,
        available);
  }
  const DealConstraint deal_constraint = constraint_text.has_value()
      ? compile_deal_constraint(*constraint_text)
      : DealConstraint{};
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

  std::map<Card, int> values;
  std::map<Card, bool> upper_bounds;
  std::vector<std::map<Card, int>> matrix;
  int total_opposition_splits = 0;
  int total_alpha_beta_cutoffs = 0;
  int total_isomorphism_skipped = 0;
  int total_pattern_hits = 0;
  int total_mtd_iterations = 0;
  int total_goal_cutoffs = 0;

  std::vector<Card> cards_to_evaluate;
  int response_seat = known_seat;
  if (opening_lead.has_value())
  {
    response_seat = first_partnership_seat_to_play(opening_lead->seat, known_seat);
    if (response_seat != known_seat && response_seat != dummy_seat)
    {
      throw std::invalid_argument(
          "opening lead response seat is the unknown partner hand; "
          "lead so a known or dummy seat plays first for the maximizing side");
    }
    cards_to_evaluate = legal_response_cards_after_opening_lead(
        known,
        dummy,
        known_seat,
        dummy_seat,
        denomination,
        available,
        deal_constraint,
        *opening_lead,
        response_seat,
        min_significant_rank);
    if (cards_to_evaluate.empty())
    {
      throw std::invalid_argument(
          "no legal partnership response to the opening lead");
    }
  }
  else
  {
    const auto probe = make_initial_state(
        known,
        dummy,
        known_seat,
        dummy_seat,
        denomination,
        available,
        deal_constraint,
        min_significant_rank);
    cards_to_evaluate = ordered_legal_moves(probe, known_seat);
    if (cards_to_evaluate.empty())
    {
      throw std::invalid_argument("no legal cards for known seat to lead");
    }
  }

  std::vector<LeadEvaluation> lead_results(cards_to_evaluate.size());
  const auto evaluate_index = [&](std::size_t index) {
    if (opening_lead.has_value())
    {
      lead_results[index] = evaluate_after_opening_lead(
          cards_to_evaluate[index],
          known,
          dummy,
          known_seat,
          dummy_seat,
          denomination,
          available,
          deal_constraint,
          *opening_lead,
          response_seat,
          goal,
          min_significant_rank);
      return;
    }
    lead_results[index] = evaluate_lead(
        cards_to_evaluate[index],
        known,
        dummy,
        known_seat,
        dummy_seat,
        denomination,
        available,
        deal_constraint,
        goal,
        min_significant_rank);
  };

  if (thread_count == 1 || cards_to_evaluate.size() <= 1)
  {
    for (std::size_t index = 0; index < cards_to_evaluate.size(); ++index)
    {
      evaluate_index(index);
    }
  }
  else
  {
    const int workers = std::min(
        thread_count,
        static_cast<int>(cards_to_evaluate.size()));
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
          if (index >= cards_to_evaluate.size())
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
    total_pattern_hits += lead_result.stats.pattern_hits;
    total_mtd_iterations += lead_result.stats.mtd_iterations;
    total_goal_cutoffs += lead_result.stats.goal_cutoffs;
    values[lead_result.lead_card] = lead_result.tricks;
    upper_bounds[lead_result.lead_card] = lead_result.tricks_upper_bound;
    matrix.push_back({{lead_result.lead_card, lead_result.tricks}});
  }

  SingleDummyResult result{};
  // Shared-pool search uses one unseen-card pool rather than enumerated deals.
  result.combination_count = 1;
  result.opposition_split_evaluations = total_opposition_splits;
  result.alpha_beta_cutoffs = total_alpha_beta_cutoffs;
  result.isomorphism_skipped_moves = total_isomorphism_skipped;
  result.pattern_hits = total_pattern_hits;
  result.mtd_iterations = total_mtd_iterations;
  result.goal_cutoffs = total_goal_cutoffs;
  result.goal = goal;
  result.trick_matrix = std::move(matrix);

  auto ordered_known = cards_to_evaluate;
  std::sort(ordered_known.begin(), ordered_known.end());
  result.leads.reserve(ordered_known.size());
  for (const auto& card : ordered_known)
  {
    result.leads.push_back(
        LeadResult{card, values.at(card), upper_bounds.at(card)});
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

auto format_result(const SingleDummyResult& result, bool show_info) -> std::string
{
  if (result.goal.has_value() && result.maximin_value < *result.goal)
  {
    std::ostringstream out;
    out << *result.goal << " tricks unachieveable\n";
    return out.str();
  }

  std::ostringstream out;
  if (show_info)
  {
    out << "combinations: " << result.combination_count << '\n';
    out << "opposition_splits: " << result.opposition_split_evaluations << '\n';
    out << "alpha_beta_cutoffs: " << result.alpha_beta_cutoffs << '\n';
    out << "isomorphism_skipped: " << result.isomorphism_skipped_moves << '\n';
    out << "pattern_hits: " << result.pattern_hits << '\n';
    out << "mtd_iterations: " << result.mtd_iterations << '\n';
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
  }
  out << "card  min_tricks\n";
  for (const auto& lead : result.leads)
  {
    out << card_to_string(lead.card);
    if (lead.min_tricks == result.maximin_value)
    {
      out << " *";
    }
    out << "  ";
    if (lead.min_tricks_upper_bound)
    {
      out << "<=";
    }
    out << lead.min_tricks << '\n';
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
    std::optional<std::string_view> constraint,
    std::optional<OpeningLead> opening_lead) -> std::string
{
  static constexpr std::array<std::string_view, kHands> seat_names =
      {"North", "East", "South", "West"};
  static constexpr std::array<std::string_view, 5> denom_labels =
      {"S", "H", "D", "C", "NT"};

  std::ostringstream out;
  out << "Trump: " << denom_labels[static_cast<std::size_t>(denomination)]
      << "\n\n";
  if (opening_lead.has_value())
  {
    out << "Opening lead: "
        << seat_names[static_cast<std::size_t>(opening_lead->seat)] << " "
        << card_to_string(opening_lead->card) << "\n\n";
  }

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
    if (seat != known_seat && seat != dummy_seat)
    {
      continue;
    }
    out << seat_names[static_cast<std::size_t>(seat)];
    if (seat == known_seat)
    {
      out << " (known)";
    }
    else
    {
      out << " (dummy)";
    }
    out << '\n';

    if (seat == known_seat)
    {
      append_hand_lines(known_fields, out);
    }
    else
    {
      append_hand_lines(dummy_fields, out);
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
  out << "): " << format_pbn_suit_fields(available_fields) << '\n';
  if (constraint.has_value())
  {
    out << "Constraint: " << *constraint << '\n';
  }
  return out.str();
}

}  // namespace single_dummy_solver
