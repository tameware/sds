// SDS single-dummy solver tests.

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
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
using single_dummy_solver::AdversarialSplitQuery;
using single_dummy_solver::distinct_representative_ranks_in_suit;
using single_dummy_solver::compile_deal_constraint;
using single_dummy_solver::DealConstraint;
using single_dummy_solver::DealConstraintPin;
using single_dummy_solver::enumerate_constrained_opponent_splits;
using single_dummy_solver::needs_adversarial_split;
using single_dummy_solver::enumerate_unknown_splits;
using single_dummy_solver::kHands;
using single_dummy_solver::kRankBitmaskQueen;
using single_dummy_solver::kRankBitmaskSix;
using single_dummy_solver::OpeningLead;
using single_dummy_solver::LeadResult;
using single_dummy_solver::parse_card;
using single_dummy_solver::parse_opening_lead;
using single_dummy_solver::parse_denomination;
using single_dummy_solver::parse_goal;
using single_dummy_solver::parse_hand;
using single_dummy_solver::parse_min_significant_rank;
using single_dummy_solver::parse_threads;
using single_dummy_solver::default_thread_count;
using single_dummy_solver::parse_seat;
using single_dummy_solver::format_position_diagram;
using single_dummy_solver::format_result;
using single_dummy_solver::solve_single_dummy;
using single_dummy_solver::available_cards_for_opponents;
using single_dummy_solver::validate_position;

auto empty_base_hands() -> std::array<std::vector<Card>, kHands>
{
  return {};
}

auto enumerate_pool_splits(
    const std::vector<Card>& pool,
    int east_slots,
    int west_slots,
    int east_void_suits = 0,
    int west_void_suits = 0,
    const DealConstraint* constraint = nullptr,
    const DealConstraint* pin_constraint = nullptr,
    int min_significant_rank = 2) -> std::vector<single_dummy_solver::UnknownSplit>
{
  return enumerate_constrained_opponent_splits(
      pool,
      /*east_hand_size=*/0,
      /*west_hand_size=*/0,
      east_slots,
      west_slots,
      empty_base_hands(),
      /*east_seat=*/1,
      /*west_seat=*/3,
      east_void_suits,
      west_void_suits,
      constraint,
      pin_constraint != nullptr ? pin_constraint : constraint,
      min_significant_rank);
}

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

TEST(SingleDummySolverTest, ParseGoalRejectsNonIntegerText)
{
  EXPECT_EQ(parse_goal("4"), 4);
  EXPECT_THROW(parse_goal("abc"), std::invalid_argument);
  EXPECT_THROW(parse_goal("4x"), std::invalid_argument);
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
  EXPECT_NO_THROW(
      validate_position(
          parse_hand("A..."),
          parse_hand("K..."),
          /*known_seat=*/0,
          /*dummy_seat=*/1,
          parse_hand("Q.2..")));
  EXPECT_THROW(
      validate_position(
          parse_hand("A..."),
          parse_hand("K..."),
          /*known_seat=*/0,
          /*dummy_seat=*/0,
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

TEST(SingleDummySolverTest, ValidatePositionAcceptsAdjacentKnownAndDummy)
{
  EXPECT_NO_THROW(
      validate_position(
          parse_hand("AKQ2..."),
          parse_hand("3456..."),
          /*known_seat=*/2,
          /*dummy_seat=*/1,
          parse_hand("JT9.23456..")));
}

TEST(SingleDummySolverTest, DefenderModeSolveReturnsKnownHandLeads)
{
  const auto result = solve_single_dummy(
      parse_hand("AKQ2..."),
      parse_hand("3456..."),
      /*known_seat=*/2,
      /*dummy_seat=*/1,
      /*denomination=*/4,
      parse_hand("JT9.23456.."));

  ASSERT_FALSE(result.leads.empty());
  // AKQ collapse to one representative; deuce stays separate.
  EXPECT_EQ(result.leads.size(), 2u);
  for (const auto& lead : result.leads)
  {
    EXPECT_EQ(lead.card.suit, 0);
    EXPECT_GE(lead.min_tricks, 0);
    EXPECT_LE(lead.min_tricks, 4);
  }
  EXPECT_GE(result.maximin_value, 0);
  EXPECT_LE(result.maximin_value, 4);
  EXPECT_FALSE(result.maximin_cards.empty());
}

TEST(SingleDummySolverTest, AvailableCardsForOpponentsRequiresThirteenCardHands)
{
  EXPECT_THROW(
      available_cards_for_opponents(parse_hand("A..."), parse_hand("K...")),
      std::invalid_argument);
}

TEST(SingleDummySolverTest, AvailableCardsForOpponentsReturnsRemainingDeckCards)
{
  const auto known = parse_hand("AKQJT98765432...");
  const auto dummy = parse_hand(".AKQJT98765432..");
  const auto available = available_cards_for_opponents(known, dummy);
  const auto expected = parse_hand("..AKQJT98765432.AKQJT98765432");

  ASSERT_EQ(available.size(), 26u);
  auto sorted_available = available;
  auto sorted_expected = expected;
  std::sort(sorted_available.begin(), sorted_available.end());
  std::sort(sorted_expected.begin(), sorted_expected.end());
  EXPECT_EQ(sorted_available, sorted_expected);
  EXPECT_NO_THROW(
      validate_position(known, dummy, /*known_seat=*/2, /*dummy_seat=*/0, available));
}

TEST(SingleDummySolverTest, AvailableCardsForOpponentsRejectsDuplicateCards)
{
  const auto hand = parse_hand("AKQJT98765432...");
  EXPECT_THROW(
      available_cards_for_opponents(hand, hand),
      std::invalid_argument);
}

TEST(SingleDummySolverTest, ConstrainedSplitsRequireExactSlotCounts)
{
  const auto pool = parse_hand("Q.A..");
  const auto splits = enumerate_pool_splits(pool, /*east_slots=*/1, /*west_slots=*/1);
  EXPECT_EQ(splits.size(), 2u);
}

TEST(SingleDummySolverTest, DealConstraintEvaluatesRedealStyleExpressions)
{
  const auto constraint = compile_deal_constraint("len(deal.east.spades) == 0");
  std::array<std::vector<Card>, kHands> hands{};
  hands[2] = parse_hand("A...");
  EXPECT_TRUE(constraint.evaluate(hands));
  hands[1] = parse_hand("Q...");
  EXPECT_FALSE(constraint.evaluate(hands));
}

TEST(SingleDummySolverTest, DealConstraintSupportsHcpComparison)
{
  const auto constraint = compile_deal_constraint("deal.west.hcp >= 8");
  std::array<std::vector<Card>, kHands> hands{};
  hands[3] = parse_hand("AKQ...");
  EXPECT_TRUE(constraint.evaluate(hands));
  hands[3] = parse_hand(".2..");
  EXPECT_FALSE(constraint.evaluate(hands));
}

TEST(SingleDummySolverTest, DealConstraintSupportsBooleanAnd)
{
  const auto constraint = compile_deal_constraint(
      "len(deal.east.spades) == 0 and len(deal.west.hearts) == 0");
  std::array<std::vector<Card>, kHands> hands{};
  hands[1] = parse_hand("..K.");
  hands[3] = parse_hand("...K");
  EXPECT_TRUE(constraint.evaluate(hands));
  hands[1] = parse_hand("Q...");
  EXPECT_FALSE(constraint.evaluate(hands));
}

TEST(SingleDummySolverTest, DealConstraintRankInSuitDetectsMembership)
{
  const auto constraint = compile_deal_constraint("A in deal.east.spades");
  std::array<std::vector<Card>, kHands> hands{};
  hands[1] = parse_hand("A...");
  EXPECT_TRUE(constraint.evaluate(hands));
  hands[1] = parse_hand("K...");
  EXPECT_FALSE(constraint.evaluate(hands));
}

TEST(SingleDummySolverTest, DealConstraintRankInSuitSupportsHonorConstants)
{
  std::array<std::vector<Card>, kHands> hands{};
  hands[2] = parse_hand("KQTJ...");
  EXPECT_TRUE(compile_deal_constraint("K in deal.south.spades").evaluate(hands));
  EXPECT_TRUE(compile_deal_constraint("Q in deal.south.spades").evaluate(hands));
  EXPECT_TRUE(compile_deal_constraint("T in deal.south.spades").evaluate(hands));
  EXPECT_TRUE(compile_deal_constraint("J in deal.south.spades").evaluate(hands));
  EXPECT_FALSE(compile_deal_constraint("A in deal.south.spades").evaluate(hands));
}

TEST(SingleDummySolverTest, DealConstraintRankInSuitSupportsNumericRanks)
{
  std::array<std::vector<Card>, kHands> hands{};
  hands[1] = parse_hand("9...");
  EXPECT_TRUE(compile_deal_constraint("9 in deal.east.spades").evaluate(hands));
  hands[1] = parse_hand("2...");
  EXPECT_TRUE(compile_deal_constraint("2 in deal.east.spades").evaluate(hands));
  hands[1] = parse_hand("T...");
  EXPECT_TRUE(compile_deal_constraint("10 in deal.east.spades").evaluate(hands));
  hands[1] = parse_hand("K...");
  EXPECT_FALSE(compile_deal_constraint("9 in deal.east.spades").evaluate(hands));
}

TEST(SingleDummySolverTest, DealConstraintRankInSuitCombinesWithAnd)
{
  const auto constraint = compile_deal_constraint(
      "A in deal.east.hearts and K in deal.west.clubs");
  std::array<std::vector<Card>, kHands> hands{};
  hands[1] = parse_hand(".A..");
  hands[3] = parse_hand("...K");
  EXPECT_TRUE(constraint.evaluate(hands));
  hands[3] = parse_hand("...Q");
  EXPECT_FALSE(constraint.evaluate(hands));
}

TEST(SingleDummySolverTest, DealConstraintRejectsInOnNonSuitAttribute)
{
  EXPECT_THROW(compile_deal_constraint("A in deal.east.hcp"), std::invalid_argument);
}

TEST(SingleDummySolverTest, ConstrainedSplitsFilterByRankInSuit)
{
  const auto pool = parse_hand("Q.A..");
  const auto constraint = compile_deal_constraint("A in deal.east.hearts");
  const auto splits = enumerate_pool_splits(
      pool,
      /*east_slots=*/1,
      /*west_slots=*/1,
      /*east_void_suits=*/0,
      /*west_void_suits=*/0,
      &constraint);
  ASSERT_EQ(splits.size(), 1u);
  EXPECT_EQ(splits[0].first.front(), card("HA"));
  EXPECT_EQ(splits[0].second.front(), card("SQ"));
}

TEST(SingleDummySolverTest, DealConstraintExposesPinnedCards)
{
  const auto constraint = compile_deal_constraint(
      "9 in deal.east.spades and A in deal.west.clubs");
  const auto pins = constraint.pinned_cards();
  ASSERT_EQ(pins.size(), 2u);
  EXPECT_EQ(pins[0].seat, 1);
  EXPECT_EQ(pins[0].suit, 0);
  EXPECT_EQ(pins[0].rank, 9);
  EXPECT_EQ(pins[1].seat, 3);
  EXPECT_EQ(pins[1].suit, 3);
  EXPECT_EQ(pins[1].rank, 14);
}

TEST(SingleDummySolverTest, DealConstraintExposesLengthZeroVoids)
{
  const auto constraint = compile_deal_constraint(
      "len(deal.east.spades) == 0 and len(deal.west.hearts) == 0");
  const auto voids = constraint.void_suits();
  ASSERT_EQ(voids.size(), 2u);
  EXPECT_EQ(voids[0].seat, 1);
  EXPECT_EQ(voids[0].suit, 0);
  EXPECT_EQ(voids[1].seat, 3);
  EXPECT_EQ(voids[1].suit, 1);
}

TEST(SingleDummySolverTest, DealConstraintRankPinRequiresCardOnEverySplit)
{
  const auto pool = parse_hand("92...");
  const auto constraint = compile_deal_constraint("9 in deal.east.spades");

  const auto without_constraint = enumerate_pool_splits(pool, 1, 1);
  ASSERT_EQ(without_constraint.size(), 2u);

  const auto with_constraint =
      enumerate_pool_splits(pool, 1, 1, 0, 0, &constraint);
  ASSERT_EQ(with_constraint.size(), 1u);
  EXPECT_EQ(with_constraint[0].first.front(), card("S9"));
  EXPECT_EQ(with_constraint[0].second.front(), card("S2"));
}

TEST(SingleDummySolverTest, DealConstraintRankPinAppliesWithoutFullConstraintEvaluation)
{
  const auto pool = parse_hand("92...K");
  const auto constraint = compile_deal_constraint("9 in deal.east.spades");

  const auto splits = enumerate_constrained_opponent_splits(
      pool,
      /*east_hand_size=*/0,
      /*west_hand_size=*/0,
      /*east_slots=*/1,
      /*west_slots=*/2,
      empty_base_hands(),
      /*east_seat=*/1,
      /*west_seat=*/3,
      /*east_void_suits=*/0,
      /*west_void_suits=*/0,
      /*constraint=*/nullptr,
      &constraint);
  ASSERT_EQ(splits.size(), 1u);
  EXPECT_EQ(splits[0].first.front(), card("S9"));
}

TEST(SingleDummySolverTest, DealConstraintRankPinIgnoredWhenCardNotInPool)
{
  const auto pool = parse_hand("2...");
  const auto constraint = compile_deal_constraint("9 in deal.east.spades");

  const auto splits = enumerate_pool_splits(pool, 1, 0, 0, 0, nullptr, &constraint);
  ASSERT_EQ(splits.size(), 1u);
  EXPECT_EQ(splits[0].first.front(), card("S2"));
}

TEST(SingleDummySolverTest, DealConstraintRankPinSurvivesMidDealRepartition)
{
  const auto result = solve_single_dummy(
      parse_hand("8.3.92.K64"),
      parse_hand("A.J.K43.Q3"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/2,
      parse_hand("96432.AQ86.QT.A97"),
      std::string("9 in deal.east.spades and A in deal.west.clubs"));

  EXPECT_EQ(result.leads.size(), 6u);
  EXPECT_GT(result.combination_count, 0);
}

TEST(SingleDummySolverTest, InitialEastConstraintVoidSpadesKeepsAceWinning)
{
  const auto without_constraint = solve_single_dummy(
      parse_hand("A..."),
      parse_hand("K..."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q.2.."));
  const auto with_constraint = solve_single_dummy(
      parse_hand("A..."),
      parse_hand("K..."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q.2.."),
      std::string("len(deal.east.spades) == 0"));

  ASSERT_EQ(without_constraint.leads.size(), 1u);
  ASSERT_EQ(with_constraint.leads.size(), 1u);
  EXPECT_EQ(without_constraint.leads[0].min_tricks, 1);
  EXPECT_EQ(with_constraint.leads[0].min_tricks, 1);
  EXPECT_EQ(without_constraint.combination_count, 1);
  EXPECT_EQ(with_constraint.combination_count, 1);
}

TEST(SingleDummySolverTest, ConstrainedSplitsFilterByDealConstraint)
{
  const auto pool = parse_hand("Q.A..");
  const auto constraint = compile_deal_constraint("len(deal.east.spades) == 0");
  const auto splits = enumerate_pool_splits(
      pool,
      /*east_slots=*/1,
      /*west_slots=*/1,
      /*east_void_suits=*/0,
      /*west_void_suits=*/0,
      &constraint);
  ASSERT_EQ(splits.size(), 1u);
  EXPECT_EQ(splits[0].first.front(), card("HA"));
  EXPECT_EQ(splits[0].second.front(), card("SQ"));
}

TEST(SingleDummySolverTest, ConstrainedSplitsExcludePlayTimeVoidSuits)
{
  const auto pool = parse_hand("Q.A..");
  const auto splits = enumerate_pool_splits(
      pool,
      /*east_slots=*/1,
      /*west_slots=*/1,
      /*east_void_suits=*/1u << 0,
      /*west_void_suits=*/0);
  ASSERT_EQ(splits.size(), 1u);
  EXPECT_EQ(splits[0].first.front(), card("HA"));
  EXPECT_EQ(splits[0].second.front(), card("SQ"));
}

TEST(SingleDummySolverTest, RecessedMenaceConstraintMatchesVoidFlagsBehavior)
{
  const auto constraint =
      "len(deal.east.spades) == 0 and len(deal.west.hearts) == 0";
  const auto with_constraint = solve_single_dummy(
      parse_hand("K.K.3.A"),
      parse_hand("..AK2.2"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/4,
      parse_hand("A.A.456789."),
      constraint);

  EXPECT_EQ(with_constraint.maximin_value, 4);
  EXPECT_EQ(find_min(with_constraint, "CA"), std::optional<int>{4});
  EXPECT_EQ(find_min(with_constraint, "D3"), std::optional<int>{3});
  EXPECT_EQ(with_constraint.combination_count, 1);
  EXPECT_EQ(with_constraint.opposition_split_evaluations, 0);
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
  EXPECT_EQ(result.combination_count, 1);
}

TEST(SingleDummySolverTest, SharedPoolUnseenHandsReportsMaximinPerLead)
{
  // North (dummy): ♠AJT ♥2
  // South (lead):  ♠K98 ♥3
  // Available:     ♠Q76543 ♥45
  //
  // Shared-pool opposition play (no deal enumeration). Spade leads are held
  // to two; the heart lead guarantees three.
  const auto result = solve_single_dummy(
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q76543.45.."));

  EXPECT_EQ(result.leads.size(), 3u);
  EXPECT_EQ(result.opposition_split_evaluations, 0);
  EXPECT_EQ(result.combination_count, 1);

  const auto king_min = find_min(result, "SK");
  const auto nine_min = find_min(result, "S9");
  const auto heart_min = find_min(result, "H3");
  ASSERT_TRUE(king_min.has_value());
  ASSERT_TRUE(nine_min.has_value());
  ASSERT_TRUE(heart_min.has_value());
  EXPECT_FALSE(find_min(result, "S8").has_value());

  EXPECT_EQ(*king_min, 2);
  EXPECT_EQ(*nine_min, 2);
  EXPECT_EQ(*heart_min, 3);
  EXPECT_EQ(result.maximin_value, 3);
  EXPECT_EQ(result.maximin_cards.size(), 1u);
  EXPECT_EQ(result.maximin_cards.front(), card("H3"));
}

TEST(SingleDummySolverTest, SharedPoolNeverNeedsAdversarialSplit)
{
  AdversarialSplitQuery query{};
  query.known_seat = 2;
  query.dummy_seat = 0;
  query.east_seat = 1;
  query.west_seat = 3;
  query.east_slots = 4;
  query.west_slots = 4;
  query.remaining_available = parse_hand("QJT9.QJT9..");

  EXPECT_FALSE(needs_adversarial_split(query, /*seat=*/1));
  EXPECT_FALSE(needs_adversarial_split(query, /*seat=*/3));
  EXPECT_FALSE(needs_adversarial_split(query, /*seat=*/2));
}

TEST(SingleDummySolverTest, SharedPoolSkipsOppositionDealEnumeration)
{
  const auto result = solve_single_dummy(
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q76543.45.."));

  EXPECT_EQ(result.opposition_split_evaluations, 0);
  EXPECT_EQ(result.combination_count, 1);
}

TEST(SingleDummySolverTest, SharedPoolFiveCardDealClubAceCappedBelowSpadeLeads)
{
  // South: ♠T52 ♦2 ♣A   North: ♠J6 ♥5 ♦A4   Available: ♠743 ♥JT873 ♦T3
  const auto result = solve_single_dummy(
      parse_hand("T52..2.A"),
      parse_hand("J6.5.A4."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("743.JT873.T3."));

  const auto club_ace_min = find_min(result, "CA");
  const auto spade_five_min = find_min(result, "S5");
  ASSERT_TRUE(club_ace_min.has_value());
  ASSERT_TRUE(spade_five_min.has_value());
  EXPECT_EQ(*club_ace_min, 4);
  EXPECT_EQ(*spade_five_min, 5);
  EXPECT_EQ(result.maximin_value, 5);
  EXPECT_EQ(result.opposition_split_evaluations, 0);
}

TEST(SingleDummySolverTest, FormatResultHidesAlgorithmCountsByDefault)
{
  const auto result = solve_single_dummy(
      parse_hand("K98.3..A"),
      parse_hand("AJT.2..K"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q765...234567"));

  const std::string formatted = format_result(result);

  EXPECT_EQ(formatted.find("combinations:"), std::string::npos);
  EXPECT_EQ(formatted.find("opposition_splits:"), std::string::npos);
  EXPECT_EQ(formatted.find("alpha_beta_cutoffs:"), std::string::npos);
  EXPECT_EQ(formatted.find("maximin:"), std::string::npos);
  EXPECT_NE(formatted.find("card  min_tricks"), std::string::npos);
}

TEST(SingleDummySolverTest, FormatResultShowsAlgorithmCountsWithInfo)
{
  const auto result = solve_single_dummy(
      parse_hand("K98.3..A"),
      parse_hand("AJT.2..K"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q765...234567"));

  const std::string formatted = format_result(result, /*show_info=*/true);

  EXPECT_NE(formatted.find("combinations:"), std::string::npos);
  EXPECT_NE(formatted.find("opposition_splits:"), std::string::npos);
  EXPECT_NE(formatted.find("alpha_beta_cutoffs:"), std::string::npos);
  EXPECT_NE(formatted.find("isomorphism_skipped:"), std::string::npos);
  EXPECT_NE(formatted.find("pattern_hits:"), std::string::npos);
  EXPECT_NE(formatted.find("mtd_iterations:"), std::string::npos);
  EXPECT_NE(formatted.find("maximin:"), std::string::npos);
}

TEST(SingleDummySolverTest, FormatResultShowsUpperBoundWhenGoalCutoffStopsShort)
{
  SingleDummyResult result{};
  result.maximin_value = 5;
  result.maximin_cards = {card("S9")};
  result.goal = 5;
  result.leads = {
      LeadResult{card("S9"), 5, /*min_tricks_upper_bound=*/false},
      LeadResult{card("SQ"), 4, /*min_tricks_upper_bound=*/true},
  };

  const std::string formatted = format_result(result);
  EXPECT_NE(formatted.find("S9 *  5\n"), std::string::npos);
  EXPECT_NE(formatted.find("SQ  <=4\n"), std::string::npos);
  EXPECT_EQ(formatted.find("SQ  4\n"), std::string::npos);
}

TEST(SingleDummySolverTest, GoalCutoffFormatsSubGoalLeadsAsUpperBound)
{
  // Maximin is 5 on CA/SK/S9; H3 is held to 4. With --goal 5 that heart
  // result is only an upper bound once goal pruning stops early.
  const auto result = solve_single_dummy(
      parse_hand("K98.3..A"),
      parse_hand("AJT.2..K"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q765...234567"),
      {},
      /*goal=*/5);

  ASSERT_EQ(result.maximin_value, 5);
  ASSERT_TRUE(result.goal.has_value());
  EXPECT_GT(result.goal_cutoffs, 0);

  bool saw_upper_bound = false;
  for (const auto& lead : result.leads)
  {
    if (lead.min_tricks < *result.goal)
    {
      EXPECT_TRUE(lead.min_tricks_upper_bound);
      saw_upper_bound = true;
    }
    else
    {
      EXPECT_FALSE(lead.min_tricks_upper_bound);
    }
  }
  EXPECT_TRUE(saw_upper_bound);

  const std::string formatted = format_result(result);
  EXPECT_NE(formatted.find("H3  <=4\n"), std::string::npos);
  EXPECT_NE(formatted.find("<="), std::string::npos);
}

TEST(SingleDummySolverTest, GoalReportsUnachievableWhenMaximinBelowTarget)
{
  const auto result = solve_single_dummy(
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q76543.45.."),
      {},
      /*goal=*/4);

  EXPECT_EQ(result.maximin_value, 3);
  ASSERT_TRUE(result.goal.has_value());
  EXPECT_EQ(*result.goal, 4);
  EXPECT_EQ(format_result(result), "4 tricks unachieveable\n");
}

TEST(SingleDummySolverTest, GoalAchievableMatchesFullSolveResults)
{
  const auto full = solve_single_dummy(
      parse_hand("K98.3..A"),
      parse_hand("AJT.2..K"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q765...234567"));
  const auto with_goal = solve_single_dummy(
      parse_hand("K98.3..A"),
      parse_hand("AJT.2..K"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q765...234567"),
      {},
      /*goal=*/4);

  EXPECT_EQ(with_goal.maximin_value, full.maximin_value);
  EXPECT_EQ(with_goal.maximin_cards, full.maximin_cards);
  EXPECT_EQ(with_goal.leads.size(), full.leads.size());
  for (std::size_t index = 0; index < full.leads.size(); ++index)
  {
    EXPECT_EQ(with_goal.leads[index].card, full.leads[index].card);
    EXPECT_EQ(with_goal.leads[index].min_tricks, full.leads[index].min_tricks);
  }
  EXPECT_EQ(format_result(with_goal).find("4 tricks unachieveable"), std::string::npos);
}

TEST(SingleDummySolverTest, GoalPruningDiscardsUnfruitfulPaths)
{
  const auto without_goal = solve_single_dummy(
      parse_hand("K98.3..A"),
      parse_hand("AJT.2..K"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q765...234567"));
  const auto with_goal = solve_single_dummy(
      parse_hand("K98.3..A"),
      parse_hand("AJT.2..K"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q765...234567"),
      {},
      /*goal=*/5);

  EXPECT_EQ(with_goal.maximin_value, without_goal.maximin_value);
  EXPECT_GT(with_goal.goal_cutoffs, 0);
}

TEST(SingleDummySolverTest, PatternAndMtdStatsDuringSolve)
{
  const auto result = solve_single_dummy(
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q76543.45.."));

  EXPECT_EQ(result.maximin_value, 3);
  EXPECT_GT(result.mtd_iterations, 0);
  EXPECT_GT(result.pattern_hits, 0);
  EXPECT_GT(result.alpha_beta_cutoffs, 0);
}

TEST(SingleDummySolverTest, VoidConstraintStillUsesPatternAndMtdSearch)
{
  const auto result = solve_single_dummy(
      parse_hand("A..."),
      parse_hand("K..."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q.2.."),
      std::string("len(deal.east.spades) == 0"));

  EXPECT_EQ(result.maximin_value, 1);
  EXPECT_GT(result.mtd_iterations, 0);
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

TEST(SingleDummySolverTest, RootLeadTableUsesDistinctLegalMovesLikeInTreeSearch)
{
  // South ♠K98 ♥3: S9 and S8 are isomorphic on lead, so the root table should
  // score only the distinct representative (S9), matching mid-tree move gen.
  const auto result = solve_single_dummy(
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q76543.45.."));

  EXPECT_TRUE(find_min(result, "SK").has_value());
  EXPECT_TRUE(find_min(result, "S9").has_value());
  EXPECT_TRUE(find_min(result, "H3").has_value());
  EXPECT_FALSE(find_min(result, "S8").has_value());
  EXPECT_EQ(result.leads.size(), 3u);
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

TEST(SingleDummySolverTest, FiveCardNtLowClubLeadGuaranteesFourTricks)
{
  // South: ♠T ♥J ♣K73   North: ♥T3 ♦A ♣Q8   Available: ♠KQJ ♥Q85 ♦J82 ♣5
  const auto result = solve_single_dummy(
      parse_hand("T.J..K73"),
      parse_hand(".T3.A.Q8"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/4,
      parse_hand("KQJ.Q85.J82.5"));

  const auto c7_min = find_min(result, "C7");
  const auto c3_min = find_min(result, "C3");
  const auto king_min = find_min(result, "CK");
  ASSERT_TRUE(c7_min.has_value());
  ASSERT_TRUE(c3_min.has_value());
  ASSERT_TRUE(king_min.has_value());
  EXPECT_EQ(*c7_min, 4);
  EXPECT_EQ(*c3_min, 4);
  EXPECT_EQ(*king_min, 3);
  EXPECT_EQ(result.maximin_value, 4);
}

TEST(SingleDummySolverTest, FiveCardClubAceAndSpadeLeadsAreMaximin)
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
  EXPECT_EQ(*club_ace_min, 5);
  EXPECT_EQ(*king_min, 5);
  EXPECT_EQ(*heart_min, 4);
  EXPECT_EQ(result.maximin_value, 5);
  // S8 is isomorphic with S9, so three distinct maximin leads (S9, SK, CA).
  EXPECT_EQ(result.maximin_cards.size(), 3u);
}

TEST(SingleDummySolverTest, FormatPositionDiagramShowsKnownHandsAndAvailablePool)
{
  const auto diagram = format_position_diagram(
      parse_hand("K98.3..A"),
      parse_hand("AJT.2..K"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      parse_hand("Q765...234567"),
      /*denomination=*/0,
      {});

  EXPECT_NE(diagram.find("Trump: S"), std::string::npos);
  EXPECT_NE(diagram.find("North (dummy)"), std::string::npos);
  EXPECT_NE(diagram.find("South (known)"), std::string::npos);
  EXPECT_EQ(diagram.find("(opposition)"), std::string::npos);
  EXPECT_NE(diagram.find("S  AJT"), std::string::npos);
  EXPECT_NE(diagram.find("S  K98"), std::string::npos);
  EXPECT_NE(diagram.find("C  A"), std::string::npos);
  EXPECT_NE(diagram.find("C  K"), std::string::npos);
  EXPECT_NE(
      diagram.find("Available (East, West): Q765...765432"),
      std::string::npos);
  EXPECT_EQ(diagram.find("Available (East, West):\n  S"), std::string::npos);
}

TEST(SingleDummySolverTest, FormatPositionDiagramShowsConstraint)
{
  const auto diagram = format_position_diagram(
      parse_hand("K98.3..A"),
      parse_hand("AJT.2..K"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      parse_hand("Q765...234567"),
      /*denomination=*/0,
      std::string_view("len(deal.east.spades) == 4"));

  EXPECT_NE(diagram.find("Constraint: len(deal.east.spades) == 4"), std::string::npos);
}

TEST(SingleDummySolverTest, ParseThreadsAcceptsZeroForHardwareConcurrency)
{
  EXPECT_EQ(parse_threads("0"), default_thread_count());
  EXPECT_EQ(parse_threads("1"), 1);
  EXPECT_EQ(parse_threads("4"), 4);
  EXPECT_THROW(parse_threads("abc"), std::invalid_argument);
  EXPECT_THROW(parse_threads("-1"), std::invalid_argument);
}

TEST(SingleDummySolverTest, DefaultThreadCountMatchesHardwareConcurrencyOrOne)
{
  EXPECT_EQ(default_thread_count(), parse_threads("0"));
  EXPECT_GE(default_thread_count(), 1);
}

TEST(SingleDummySolverTest, DefaultThreadCountSolveMatchesSingleThreadedResults)
{
  const auto known = parse_hand("K98.3..");
  const auto dummy = parse_hand("AJT.2..");
  const auto available = parse_hand("Q76543.45..");
  const auto single = solve_single_dummy(
      known,
      dummy,
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      available,
      {},
      std::nullopt,
      /*thread_count=*/1);
  const auto multi = solve_single_dummy(
      known,
      dummy,
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      available,
      {},
      std::nullopt,
      /*thread_count=*/0);

  EXPECT_EQ(multi.maximin_value, single.maximin_value);
  EXPECT_EQ(multi.maximin_cards, single.maximin_cards);
  ASSERT_EQ(multi.leads.size(), single.leads.size());
  for (const auto& lead : single.leads)
  {
    const auto it = std::find_if(
        multi.leads.begin(),
        multi.leads.end(),
        [&](const auto& candidate) { return candidate.card == lead.card; });
    ASSERT_NE(it, multi.leads.end());
    EXPECT_EQ(it->min_tricks, lead.min_tricks);
  }
  EXPECT_EQ(
      multi.opposition_split_evaluations,
      single.opposition_split_evaluations);
}

TEST(SingleDummySolverTest, ParseMinSignificantRankAcceptsValidRanks)
{
  EXPECT_EQ(parse_min_significant_rank("2"), 2);
  EXPECT_EQ(parse_min_significant_rank("7"), 7);
  EXPECT_EQ(parse_min_significant_rank("T"), 10);
  EXPECT_EQ(parse_min_significant_rank("A"), 14);
  EXPECT_THROW(parse_min_significant_rank(""), std::invalid_argument);
  EXPECT_THROW(parse_min_significant_rank("1"), std::invalid_argument);
  EXPECT_THROW(parse_min_significant_rank("15"), std::invalid_argument);
}

TEST(SingleDummySolverTest, MinSignificantRankDefaultKeepsLowCardSplitsDistinct)
{
  const auto pool = parse_hand("32...");
  const auto splits = enumerate_pool_splits(pool, /*east_slots=*/1, /*west_slots=*/1);
  EXPECT_EQ(splits.size(), 2u);
}

TEST(SingleDummySolverTest, MinSignificantRankMergesInsignificantLowCardSplits)
{
  const auto pool = parse_hand("32...");
  const auto splits = enumerate_pool_splits(
      pool,
      /*east_slots=*/1,
      /*west_slots=*/1,
      /*east_void_suits=*/0,
      /*west_void_suits=*/0,
      /*constraint=*/nullptr,
      /*pin_constraint=*/nullptr,
      /*min_significant_rank=*/7);
  ASSERT_EQ(splits.size(), 1u);
  EXPECT_TRUE(
      splits[0].first.front().rank == 2 || splits[0].first.front().rank == 3);
}

TEST(SingleDummySolverTest, MinSignificantRankKeepsSignificantCardSplitsDistinct)
{
  const auto pool = parse_hand("72...");
  const auto splits = enumerate_pool_splits(
      pool,
      /*east_slots=*/1,
      /*west_slots=*/1,
      /*east_void_suits=*/0,
      /*west_void_suits=*/0,
      /*constraint=*/nullptr,
      /*pin_constraint=*/nullptr,
      /*min_significant_rank=*/7);
  EXPECT_EQ(splits.size(), 2u);
}

TEST(SingleDummySolverTest, MinSignificantRankPreservesMaximinUnderSharedPool)
{
  const auto available = parse_hand("Q76543.45..");
  const auto full = solve_single_dummy(
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      available,
      {},
      std::nullopt,
      /*thread_count=*/1,
      /*min_significant_rank=*/2);
  const auto merged = solve_single_dummy(
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      available,
      {},
      std::nullopt,
      /*thread_count=*/1,
      /*min_significant_rank=*/7);

  EXPECT_EQ(full.combination_count, 1);
  EXPECT_EQ(merged.combination_count, 1);
  EXPECT_EQ(full.maximin_value, merged.maximin_value);
}

TEST(SingleDummySolverTest, ParseOpeningLeadAcceptsSeatAndCard)
{
  const auto lead = parse_opening_lead("WH4");
  EXPECT_EQ(lead.seat, 3);
  EXPECT_EQ(parse_opening_lead("WH4").card, card("H4"));
  EXPECT_EQ(parse_opening_lead("SsK").seat, 2);
  EXPECT_EQ(parse_opening_lead("SsK").card, card("SK"));
}

TEST(SingleDummySolverTest, ParseOpeningLeadRejectsInvalidText)
{
  EXPECT_THROW(parse_opening_lead("W"), std::invalid_argument);
  EXPECT_THROW(parse_opening_lead("XXY"), std::invalid_argument);
}

TEST(SingleDummySolverTest, OpeningLeadFromWestEvaluatesDummyFirstResponse)
{
  const OpeningLead west_leads_low_heart{3, card("H4")};
  const auto after_lead = solve_single_dummy(
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q76543.45.."),
      {},
      std::nullopt,
      /*thread_count=*/0,
      /*min_significant_rank=*/2,
      west_leads_low_heart);

  EXPECT_EQ(after_lead.leads.size(), 1u);
  EXPECT_EQ(after_lead.combination_count, 1);
  EXPECT_FALSE(find_min(after_lead, "SK").has_value());
  EXPECT_FALSE(find_min(after_lead, "SA").has_value());
  const auto heart_two_min = find_min(after_lead, "H2");
  ASSERT_TRUE(heart_two_min.has_value());
  EXPECT_EQ(*heart_two_min, 3);
  EXPECT_EQ(after_lead.maximin_value, 3);
}

TEST(SingleDummySolverTest, OpeningLeadFromEastEvaluatesKnownFirstResponse)
{
  const OpeningLead east_leads_low_heart{1, card("H5")};
  const auto after_lead = solve_single_dummy(
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q76543.45.."),
      {},
      std::nullopt,
      /*thread_count=*/0,
      /*min_significant_rank=*/2,
      east_leads_low_heart);

  EXPECT_EQ(after_lead.leads.size(), 1u);
  const auto heart_three_min = find_min(after_lead, "H3");
  ASSERT_TRUE(heart_three_min.has_value());
  EXPECT_FALSE(find_min(after_lead, "SA").has_value());
}

TEST(SingleDummySolverTest, OpeningLeadFromSouthClubAceShowsDummyClubTwoFollow)
{
  const OpeningLead south_leads_club_ace{2, card("CA")};
  const auto after_lead = solve_single_dummy(
      parse_hand("K.K.3.A"),
      parse_hand("..AK2.2"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/4,
      parse_hand("A.A.456789."),
      "len(deal.east.spades) == 0 and len(deal.west.hearts) == 0",
      std::nullopt,
      /*thread_count=*/0,
      /*min_significant_rank=*/2,
      south_leads_club_ace);

  ASSERT_EQ(after_lead.leads.size(), 1u);
  EXPECT_EQ(after_lead.leads[0].card, card("C2"));
  EXPECT_EQ(after_lead.leads[0].min_tricks, 4);
  EXPECT_EQ(after_lead.maximin_value, 4);
}

TEST(SingleDummySolverTest, OpeningLeadFromDummyClubTwoShowsKnownClubAceFollow)
{
  const OpeningLead north_leads_club_two{0, card("C2")};
  const auto after_lead = solve_single_dummy(
      parse_hand("K.K.3.A"),
      parse_hand("..AK2.2"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/4,
      parse_hand("A.A.456789."),
      "len(deal.east.spades) == 0 and len(deal.west.hearts) == 0",
      std::nullopt,
      /*thread_count=*/0,
      /*min_significant_rank=*/2,
      north_leads_club_two);

  ASSERT_EQ(after_lead.leads.size(), 1u);
  EXPECT_EQ(after_lead.leads[0].card, card("CA"));
  EXPECT_EQ(after_lead.leads[0].min_tricks, 4);
}

TEST(SingleDummySolverTest, OpeningLeadRejectsCardNotInAvailablePool)
{
  const OpeningLead west_leads_missing_card{3, card("S2")};
  EXPECT_THROW(
      solve_single_dummy(
          parse_hand("K98.3.."),
          parse_hand("AJT.2.."),
          /*known_seat=*/2,
          /*dummy_seat=*/0,
          /*denomination=*/0,
          parse_hand("Q76543.45.."),
          {},
          std::nullopt,
          /*thread_count=*/0,
          /*min_significant_rank=*/2,
          west_leads_missing_card),
      std::invalid_argument);
}

TEST(SingleDummySolverTest, DefenderOpeningLeadRejectsWhenNextMaximizeSeatIsUnknownPartner)
{
  // Known South, dummy East: after West leads, North (unknown partner) is next.
  const OpeningLead west_leads_heart{3, card("H2")};
  EXPECT_THROW(
      solve_single_dummy(
          parse_hand("AKQ2..."),
          parse_hand("3456..."),
          /*known_seat=*/2,
          /*dummy_seat=*/1,
          /*denomination=*/4,
          parse_hand("JT9.23456.."),
          {},
          std::nullopt,
          /*thread_count=*/0,
          /*min_significant_rank=*/2,
          west_leads_heart),
      std::invalid_argument);
}

TEST(SingleDummySolverTest, DefenderOpeningLeadFromDummyEvaluatesKnownResponse)
{
  const OpeningLead east_leads_spade{1, card("S3")};
  const auto after_lead = solve_single_dummy(
      parse_hand("AKQ2..."),
      parse_hand("3456..."),
      /*known_seat=*/2,
      /*dummy_seat=*/1,
      /*denomination=*/4,
      parse_hand("JT9.23456.."),
      {},
      std::nullopt,
      /*thread_count=*/0,
      /*min_significant_rank=*/2,
      east_leads_spade);

  ASSERT_FALSE(after_lead.leads.empty());
  for (const auto& lead : after_lead.leads)
  {
    EXPECT_EQ(lead.card.suit, 0);
  }
}

}  // namespace

TEST(SingleDummySolverTest, RecessedMenaceClubAceMakesFourDiamondThreeMakesThree)
{
  // East void spades / West void hearts forces SA with West and HA with East.
  // On every such deal, CA makes four (cash the freed king after an ace-pitch,
  // or walk D2 if both keep their aces). D3 is held to three.
  const auto constraint =
      "len(deal.east.spades) == 0 and len(deal.west.hearts) == 0";
  const auto result = solve_single_dummy(
      parse_hand("K.K.3.A"),
      parse_hand("..AK2.2"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/4,
      parse_hand("A.A.456789."),
      constraint);

  EXPECT_EQ(find_min(result, "D3"), std::optional<int>{3});
  EXPECT_EQ(find_min(result, "CA"), std::optional<int>{4});
  EXPECT_EQ(result.maximin_value, 4);
}

TEST(SingleDummySolverTest, RecessedMenaceClubAceLeadMatchesOpeningLeadPath)
{
  const auto constraint =
      "len(deal.east.spades) == 0 and len(deal.west.hearts) == 0";
  const auto from_root = solve_single_dummy(
      parse_hand("K.K.3.A"),
      parse_hand("..AK2.2"),
      2,
      0,
      4,
      parse_hand("A.A.456789."),
      constraint,
      std::nullopt,
      /*thread_count=*/1);
  const auto after_lead = solve_single_dummy(
      parse_hand("K.K.3.A"),
      parse_hand("..AK2.2"),
      2,
      0,
      4,
      parse_hand("A.A.456789."),
      constraint,
      std::nullopt,
      /*thread_count=*/1,
      2,
      OpeningLead{2, card("CA")});

  ASSERT_TRUE(find_min(from_root, "CA").has_value());
  ASSERT_EQ(after_lead.leads.size(), 1u);
  EXPECT_EQ(*find_min(from_root, "CA"), 4);
  EXPECT_EQ(after_lead.leads[0].min_tricks, 4);
}
