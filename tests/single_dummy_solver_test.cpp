/*
   DDS, a bridge double dummy solver.

   Copyright (C) 2006-2014 by Bo Haglund /
   2014-2016 by Bo Haglund & Soren Hein.

   See LICENSE and README.
*/

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "single_dummy_solver.hpp"

namespace
{

using single_dummy_solver::Card;
using single_dummy_solver::FutureTricks;
using single_dummy_solver::SingleDummyResult;
using single_dummy_solver::card_scores_from_future_tricks;
using single_dummy_solver::distinct_representative_ranks_in_suit;
using single_dummy_solver::enumerate_constrained_opponent_splits;
using single_dummy_solver::enumerate_unknown_splits;
using single_dummy_solver::kHands;
using single_dummy_solver::kRankBitmaskQueen;
using single_dummy_solver::kRankBitmaskSix;
using single_dummy_solver::parse_card;
using single_dummy_solver::parse_denomination;
using single_dummy_solver::parse_hand;
using single_dummy_solver::parse_seat;
using single_dummy_solver::parse_void_suits;
using single_dummy_solver::solve_single_dummy;
using single_dummy_solver::validate_position;
using single_dummy_solver::validate_seat_voids;

auto card(std::string_view text) -> Card
{
  return parse_card(text);
}

auto find_min(const SingleDummyResult& result, std::string_view text)
    -> std::optional<int>
{
  const Card target = parse_card(text);
  for (const auto& lead : result.leads)
  {
    if (lead.card == target)
    {
      return lead.min_tricks;
    }
  }
  return std::nullopt;
}

TEST(SingleDummySolverTest, ParseHandAcceptsPbnStyleSuits)
{
  const auto hand = parse_hand("AJ32.K..");
  ASSERT_EQ(hand.size(), 5u);
  EXPECT_EQ(hand[0], card("SA"));
  EXPECT_EQ(hand[1], card("SJ"));
  EXPECT_EQ(hand[2], card("S3"));
  EXPECT_EQ(hand[3], card("S2"));
  EXPECT_EQ(hand[4], card("HK"));
}

TEST(SingleDummySolverTest, ParseHandRejectsWrongSuitCount)
{
  EXPECT_THROW(parse_hand("AKQ.JT9"), std::invalid_argument);
}

TEST(SingleDummySolverTest, ParseDenominationAndSeat)
{
  EXPECT_EQ(parse_denomination("NT"), 4);
  EXPECT_EQ(parse_denomination("S"), 0);
  EXPECT_EQ(parse_denomination("h"), 1);
  EXPECT_EQ(parse_seat("N"), 0);
  EXPECT_EQ(parse_seat("w"), 3);
  EXPECT_THROW(parse_denomination("X"), std::invalid_argument);
  EXPECT_THROW(parse_seat("X"), std::invalid_argument);
}

TEST(SingleDummySolverTest, FourCardPoolYieldsSeventySplits)
{
  const auto pool = parse_hand("QJT9.QJT9..");
  ASSERT_EQ(pool.size(), 8u);

  const auto splits = enumerate_unknown_splits(pool, /*hand_size=*/4);
  EXPECT_EQ(splits.size(), 70u);

  std::set<std::pair<std::set<Card>, std::set<Card>>> seen;
  for (const auto& split : splits)
  {
    ASSERT_EQ(split.first.size(), 4u);
    ASSERT_EQ(split.second.size(), 4u);
    const std::set<Card> first(split.first.begin(), split.first.end());
    const std::set<Card> second(split.second.begin(), split.second.end());
    for (const auto& c : pool)
    {
      EXPECT_TRUE(first.count(c) + second.count(c) == 1u);
    }
    EXPECT_TRUE(seen.insert({first, second}).second);
  }
}

TEST(SingleDummySolverTest, ExpandsEqualsBitmaskOntoLeadCards)
{
  const std::vector<Card> lead = {
      card("SK"), card("SQ"), card("S7"), card("S6")};

  FutureTricks fut{};
  fut.cards = 2;
  fut.suit[0] = 0;
  fut.rank[0] = 13;
  fut.equals[0] = static_cast<int>(kRankBitmaskQueen) << 2;
  fut.score[0] = 3;
  fut.suit[1] = 0;
  fut.rank[1] = 7;
  fut.equals[1] = static_cast<int>(kRankBitmaskSix) << 2;
  fut.score[1] = 1;

  const auto scores = card_scores_from_future_tricks(fut, lead);
  ASSERT_EQ(scores.size(), 4u);
  EXPECT_EQ(scores.at(card("SK")), 3);
  EXPECT_EQ(scores.at(card("SQ")), 3);
  EXPECT_EQ(scores.at(card("S7")), 1);
  EXPECT_EQ(scores.at(card("S6")), 1);
}

TEST(SingleDummySolverTest, ValidatePositionRejectsInvalidLayouts)
{
  const auto one = parse_hand("A...");
  const auto two = parse_hand("AK...");
  const auto pool = parse_hand("Q.A..");

  EXPECT_THROW(
      validate_position(one, two, /*known_seat=*/0, /*dummy_seat=*/2, pool),
      std::invalid_argument);
  EXPECT_THROW(
      validate_position(
          parse_hand("A..."),
          parse_hand("K..."),
          /*known_seat=*/0,
          /*dummy_seat=*/1,
          parse_hand("Q.2..")),
      std::invalid_argument);
  EXPECT_THROW(
      validate_position(
          parse_hand("A..."),
          parse_hand("K..."),
          /*known_seat=*/0,
          /*dummy_seat=*/2,
          parse_hand("A.2..")),
      std::invalid_argument);
}

TEST(SingleDummySolverTest, ConstrainedSplitsRequireExactSlotCounts)
{
  const auto pool = parse_hand("Q.A..");
  const auto splits = enumerate_constrained_opponent_splits(
      pool,
      /*east_hand_size=*/0,
      /*west_hand_size=*/0,
      /*east_slots=*/1,
      /*west_slots=*/1);
  EXPECT_EQ(splits.size(), 2u);
}

TEST(SingleDummySolverTest, ParseVoidSuitsAcceptsCommaOrRunTogether)
{
  EXPECT_EQ(parse_void_suits("S"), 1u << 0);
  EXPECT_EQ(parse_void_suits("s,h"), (1u << 0) | (1u << 1));
  EXPECT_EQ(parse_void_suits("SHDC"), (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3));
  EXPECT_EQ(parse_void_suits(""), 0);
  EXPECT_THROW(parse_void_suits("X"), std::invalid_argument);
}

TEST(SingleDummySolverTest, InitialEastVoidSpadesAppliesToRepartitions)
{
  const auto without_void = solve_single_dummy(
      parse_hand("A..."),
      parse_hand("K..."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q.2.."));
  const auto with_void = solve_single_dummy(
      parse_hand("A..."),
      parse_hand("K..."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q.2.."),
      /*seat_void_suits=*/{0, 1u << 0, 0, 0});

  ASSERT_EQ(without_void.leads.size(), 1u);
  ASSERT_EQ(with_void.leads.size(), 1u);
  EXPECT_EQ(without_void.leads[0].min_tricks, 1);
  EXPECT_EQ(with_void.leads[0].min_tricks, 1);
  EXPECT_LT(
      with_void.opposition_split_evaluations,
      without_void.opposition_split_evaluations);
}

TEST(SingleDummySolverTest, ValidateSeatVoidsRejectsKnownHandInVoidSuit)
{
  std::array<std::vector<Card>, kHands> hands{};
  hands[2] = parse_hand("K98.3..");
  EXPECT_THROW(
      validate_seat_voids(hands, {0, 0, 1u << 0, 0}),
      std::invalid_argument);
}

TEST(SingleDummySolverTest, ValidateSeatVoidsRejectsDummyHandInVoidSuit)
{
  std::array<std::vector<Card>, kHands> hands{};
  hands[0] = parse_hand("AJT.2..");
  EXPECT_THROW(
      validate_seat_voids(hands, {1u << 1, 0, 0, 0}),
      std::invalid_argument);
}

TEST(SingleDummySolverTest, SouthVoidAcceptedWhenHandHasNoCardsInVoidSuit)
{
  std::array<int, kHands> seat_void_suits{};
  seat_void_suits[2] = 1u << 3;  // South void clubs; hand has none
  const auto result = solve_single_dummy(
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q76543.45.."),
      seat_void_suits);
  EXPECT_EQ(result.maximin_value, 3);
}

TEST(SingleDummySolverTest, ConstrainedSplitsExcludeVoidSuits)
{
  const auto pool = parse_hand("Q.A..");
  const auto splits = enumerate_constrained_opponent_splits(
      pool,
      /*east_hand_size=*/0,
      /*west_hand_size=*/0,
      /*east_slots=*/1,
      /*west_slots=*/1,
      /*east_void_suits=*/1u << 0,
      /*west_void_suits=*/0);
  ASSERT_EQ(splits.size(), 1u);
  EXPECT_EQ(splits[0].first.front(), card("HA"));
  EXPECT_EQ(splits[0].second.front(), card("SQ"));
}

TEST(SingleDummySolverTest, OneCardEndgameWinsTheOnlyTrickWhenLeadingAce)
{
  const auto result = solve_single_dummy(
      parse_hand("A..."),
      parse_hand("K..."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q.2.."));

  ASSERT_EQ(result.leads.size(), 1u);
  EXPECT_EQ(result.leads[0].card, card("SA"));
  EXPECT_EQ(result.leads[0].min_tricks, 1);
  EXPECT_GT(result.opposition_split_evaluations, 0);
}

TEST(SingleDummySolverTest, FourCardSpadePositionReportsMaximinPerLead)
{
  // North (dummy): ♠AJT ♥2
  // South (lead):  ♠K98 ♥3
  // Available:     ♠Q76543 ♥45
  const auto result = solve_single_dummy(
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q76543.45.."));

  EXPECT_EQ(result.leads.size(), 4u);
  EXPECT_GT(result.opposition_split_evaluations, result.combination_count);

  for (const auto& lead : result.leads)
  {
    EXPECT_GE(lead.min_tricks, 0);
    EXPECT_LE(lead.min_tricks, 4);
  }

  const auto king_min = find_min(result, "SK");
  const auto nine_min = find_min(result, "S9");
  const auto eight_min = find_min(result, "S8");
  const auto heart_min = find_min(result, "H3");
  ASSERT_TRUE(king_min.has_value());
  ASSERT_TRUE(nine_min.has_value());
  ASSERT_TRUE(eight_min.has_value());
  ASSERT_TRUE(heart_min.has_value());

  // Low spade leads let West duck; opposition re-splitting then places SQ to
  // defeat either dummy high or dummy low before East plays.
  EXPECT_EQ(*king_min, 2);
  EXPECT_EQ(*nine_min, 2);
  EXPECT_EQ(*eight_min, 2);
  EXPECT_EQ(*heart_min, 3);
  EXPECT_EQ(result.maximin_value, 3);
  EXPECT_EQ(result.maximin_cards.size(), 1u);
  EXPECT_EQ(result.maximin_cards.front(), card("H3"));
}

TEST(SingleDummySolverTest, DistinctRepresentativeRanksMergeWhenGapUnavailable)
{
  // AQ with king unavailable (played or never dealt) -> only ace is distinct.
  const int ace = 1 << 12;
  const int queen = 1 << 10;
  const int king = 1 << 11;
  const auto reps = distinct_representative_ranks_in_suit(
      ace | queen,
      king);
  ASSERT_EQ(reps.size(), 1u);
  EXPECT_EQ(reps.front(), 14);
}

TEST(SingleDummySolverTest, DistinctRepresentativeRanksKeepSeparateTouchingCards)
{
  // AK stays one group; only ace is searched when leading the suit.
  const int ace = 1 << 12;
  const int king = 1 << 11;
  const auto reps = distinct_representative_ranks_in_suit(ace | king, 0);
  ASSERT_EQ(reps.size(), 1u);
  EXPECT_EQ(reps.front(), 14);
}

TEST(SingleDummySolverTest, CardIsomorphismReducesSearchWithoutChangingResults)
{
  const auto result = solve_single_dummy(
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q76543.45.."));

  EXPECT_EQ(result.maximin_value, 3);
  EXPECT_GT(result.isomorphism_skipped_moves, 0);
}

TEST(SingleDummySolverTest, AlphaBetaPrunesWithoutChangingResults)
{
  const auto result = solve_single_dummy(
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q76543.45.."));

  EXPECT_EQ(result.maximin_value, 3);
  EXPECT_GT(result.alpha_beta_cutoffs, 0);
}

TEST(SingleDummySolverTest, FiveCardClubAceOnlyMaximinOnClubAceLead)
{
  const auto result = solve_single_dummy(
      parse_hand("K98.3..A"),
      parse_hand("AJT.2..K"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q765...234567"));

  const auto club_ace_min = find_min(result, "CA");
  const auto king_min = find_min(result, "SK");
  const auto heart_min = find_min(result, "H3");
  ASSERT_TRUE(club_ace_min.has_value());
  ASSERT_TRUE(king_min.has_value());
  ASSERT_TRUE(heart_min.has_value());
  EXPECT_EQ(*club_ace_min, 4);
  EXPECT_EQ(result.maximin_value, 4);
  EXPECT_LT(*king_min, *club_ace_min);
  EXPECT_LT(*heart_min, *club_ace_min);
}

}  // namespace
