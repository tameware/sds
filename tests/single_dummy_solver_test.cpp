/*
   DDS, a bridge double dummy solver.

   Copyright (C) 2006-2014 by Bo Haglund /
   2014-2016 by Bo Haglund & Soren Hein.

   See LICENSE and README.
*/

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "single_dummy_solver.hpp"
#include "transposition_table.hpp"

namespace
{

using single_dummy_solver::Card;
using single_dummy_solver::FutureTricks;
using single_dummy_solver::SingleDummyResult;
using single_dummy_solver::TranspositionTable;
using single_dummy_solver::build_transposition_position;
using single_dummy_solver::card_scores_from_future_tricks;
using single_dummy_solver::AdversarialSplitQuery;
using single_dummy_solver::distinct_representative_ranks_in_suit;
using single_dummy_solver::compile_deal_constraint;
using single_dummy_solver::DealConstraint;
using single_dummy_solver::enumerate_constrained_opponent_splits;
using single_dummy_solver::needs_adversarial_split;
using single_dummy_solver::enumerate_unknown_splits;
using single_dummy_solver::kHands;
using single_dummy_solver::kRankBitmaskQueen;
using single_dummy_solver::kRankBitmaskSix;
using single_dummy_solver::parse_card;
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

TEST(SingleDummySolverTest, InitialEastConstraintVoidSpadesReducesCombinations)
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
  EXPECT_EQ(without_constraint.combination_count, 2);
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
  const auto without_constraint = solve_single_dummy(
      parse_hand("K.K.3.A"),
      parse_hand("..AK2.2"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/4,
      parse_hand("A.A.456789."));
  const auto with_constraint = solve_single_dummy(
      parse_hand("K.K.3.A"),
      parse_hand("..AK2.2"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/4,
      parse_hand("A.A.456789."),
      constraint);

  EXPECT_EQ(with_constraint.maximin_value, 4);
  EXPECT_LT(with_constraint.combination_count, without_constraint.combination_count);
  EXPECT_EQ(without_constraint.combination_count, 70);
  EXPECT_EQ(with_constraint.combination_count, 20);
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
  EXPECT_EQ(result.combination_count, 2);
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

TEST(SingleDummySolverTest, OpponentEmptyHandNeedsAdversarialSplit)
{
  // Before opponents are dealt, unseen cards sit in remaining_available with
  // empty opponent hands. An opponent seat about to play must trigger split.
  AdversarialSplitQuery query{};
  query.known_seat = 2;
  query.dummy_seat = 0;
  query.east_seat = 1;
  query.west_seat = 3;
  query.east_slots = 4;
  query.west_slots = 4;
  query.remaining_available = parse_hand("QJT9.QJT9..");

  EXPECT_TRUE(needs_adversarial_split(query, /*seat=*/1));
  EXPECT_TRUE(needs_adversarial_split(query, /*seat=*/3));
  EXPECT_TRUE(needs_adversarial_split(query, /*seat=*/2));

  query.hands[static_cast<std::size_t>(query.east_seat)] =
      parse_hand("QJT9...");
  EXPECT_FALSE(needs_adversarial_split(query, /*seat=*/1));
}

TEST(SingleDummySolverTest, RepartitionsBeforeEveryPartnershipPlayIncludingMidTrickFollows)
{
  // Opposition re-deals remaining unseen cards before each partnership play,
  // including when dummy follows mid-trick. Skipping mid-trick repartition
  // collapses opposition_splits on this deal from ~30k to ~4.5k.
  const auto result = solve_single_dummy(
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q76543.45.."));

  EXPECT_GT(result.opposition_split_evaluations, 10'000);
}

TEST(SingleDummySolverTest, MidTrickRepartitionCapsClubAceLeadOnFiveCardDeal)
{
  // South: ♠T52 ♦2 ♣A   North: ♠J6 ♥5 ♦A4   Available: ♠743 ♥JT873 ♦T3
  //
  // Leading ♣A reaches a mid-trick dummy follow where opposition must be
  // allowed to re-deal remaining unseen cards. Skipping that repartition
  // inflates the ♣A guarantee from three tricks to four.
  const auto result = solve_single_dummy(
      parse_hand("T52..2.A"),
      parse_hand("J6.5.A4."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("743.JT873.T3."));

  const auto spade_two_min = find_min(result, "S2");
  const auto club_ace_min = find_min(result, "CA");
  ASSERT_TRUE(spade_two_min.has_value());
  ASSERT_TRUE(club_ace_min.has_value());
  EXPECT_EQ(*spade_two_min, 4);
  EXPECT_EQ(*club_ace_min, 3);
  EXPECT_EQ(result.maximin_value, 4);
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
  EXPECT_NE(formatted.find("transposition_hits:"), std::string::npos);
  EXPECT_NE(formatted.find("transposition_bound_cutoffs:"), std::string::npos);
  EXPECT_NE(formatted.find("maximin:"), std::string::npos);
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
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q76543.45.."));
  const auto with_goal = solve_single_dummy(
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q76543.45.."),
      {},
      /*goal=*/4);

  EXPECT_EQ(with_goal.maximin_value, without_goal.maximin_value);
  EXPECT_GT(with_goal.goal_cutoffs, 0);
  EXPECT_LT(
      with_goal.opposition_split_evaluations,
      without_goal.opposition_split_evaluations);
}

TEST(SingleDummySolverTest, TranspositionLowerBoundCutsOffWhenAlphaReachesBeta)
{
  std::array<std::vector<Card>, kHands> hands{};
  hands[0] = {card("SA")};
  const std::vector<std::pair<int, Card>> empty_trick;
  std::array<int, kHands> voids{};
  std::array<int, 4> win_ranks{};
  const auto position = build_transposition_position(
      0, 0, 0, 0, hands, empty_trick, voids, win_ranks);

  TranspositionTable table;
  table.store(
      position,
      5,
      /*alpha=*/3,
      /*beta=*/4);

  int alpha = 2;
  int beta = 4;
  const auto lookup = table.lookup(position, alpha, beta);
  ASSERT_TRUE(lookup.has_value());
  EXPECT_EQ(*lookup, 5);
  EXPECT_EQ(table.bound_cutoffs(), 1u);
}

TEST(SingleDummySolverTest, TranspositionUpperBoundCutsOffWhenAlphaReachesBeta)
{
  std::array<std::vector<Card>, kHands> hands{};
  hands[0] = {card("SA")};
  const std::vector<std::pair<int, Card>> empty_trick;
  std::array<int, kHands> voids{};
  std::array<int, 4> win_ranks{};
  const auto position = build_transposition_position(
      0, 0, 0, 0, hands, empty_trick, voids, win_ranks);

  TranspositionTable table;
  table.store(
      position,
      2,
      /*alpha=*/3,
      /*beta=*/10);

  int alpha = 3;
  int beta = 5;
  const auto lookup = table.lookup(position, alpha, beta);
  ASSERT_TRUE(lookup.has_value());
  EXPECT_EQ(*lookup, 2);
  EXPECT_EQ(table.bound_cutoffs(), 1u);
}

TEST(SingleDummySolverTest, TranspositionBoundTightensWindowWithoutCutoff)
{
  std::array<std::vector<Card>, kHands> hands{};
  hands[0] = {card("SA")};
  const std::vector<std::pair<int, Card>> empty_trick;
  std::array<int, kHands> voids{};
  std::array<int, 4> win_ranks{};
  const auto position = build_transposition_position(
      0, 0, 0, 0, hands, empty_trick, voids, win_ranks);

  TranspositionTable table;
  table.store(
      position,
      2,
      /*alpha=*/3,
      /*beta=*/10);

  int alpha = 0;
  int beta = 5;
  const auto lookup = table.lookup(position, alpha, beta);
  EXPECT_FALSE(lookup.has_value());
  EXPECT_EQ(beta, 2);
  EXPECT_EQ(table.bound_cutoffs(), 0u);
}

TEST(SingleDummySolverTest, TranspositionBoundCutoffsDuringSolve)
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
  EXPECT_GT(result.transposition_bound_cutoffs, 0);
}

TEST(SingleDummySolverTest, RelativeSuitEncodingMatchesIsomorphicHoldings)
{
  std::array<std::vector<Card>, kHands> high_label{};
  high_label[0] = {card("SA")};
  high_label[1] = {card("S9")};
  high_label[2] = {card("SQ")};
  high_label[3] = {card("S7")};

  std::array<std::vector<Card>, kHands> low_label{};
  low_label[0] = {card("SK")};
  low_label[1] = {card("S8")};
  low_label[2] = {card("SJ")};
  low_label[3] = {card("S6")};

  const std::vector<std::pair<int, Card>> empty_trick;
  std::array<int, kHands> voids{};
  std::array<int, 4> win_ranks{};

  const auto high_pos = build_transposition_position(
      0, 1, 1, 2, high_label, empty_trick, voids, win_ranks);
  const auto low_pos = build_transposition_position(
      0, 1, 1, 2, low_label, empty_trick, voids, win_ranks);
  EXPECT_EQ(high_pos.order_set, low_pos.order_set);
}

TEST(SingleDummySolverTest, TranspositionTableMatchesRelativePositions)
{
  std::array<std::vector<Card>, kHands> high_label{};
  high_label[0] = {card("SA")};
  high_label[1] = {card("S9")};
  high_label[2] = {card("SQ")};
  high_label[3] = {card("S7")};

  std::array<std::vector<Card>, kHands> low_label{};
  low_label[0] = {card("SK")};
  low_label[1] = {card("S8")};
  low_label[2] = {card("SJ")};
  low_label[3] = {card("S6")};

  const std::vector<std::pair<int, Card>> empty_trick;
  std::array<int, kHands> voids{};
  std::array<int, 4> win_ranks{};

  const auto high_pos = build_transposition_position(
      /*partnership_tricks=*/0,
      /*east_slots=*/1,
      /*west_slots=*/1,
      /*leader=*/2,
      high_label,
      empty_trick,
      voids,
      win_ranks);
  const auto low_pos = build_transposition_position(
      /*partnership_tricks=*/0,
      /*east_slots=*/1,
      /*west_slots=*/1,
      /*leader=*/2,
      low_label,
      empty_trick,
      voids,
      win_ranks);

  TranspositionTable table;
  table.store(
      high_pos,
      7,
      std::numeric_limits<int>::min(),
      std::numeric_limits<int>::max());
  int alpha = std::numeric_limits<int>::min();
  int beta = std::numeric_limits<int>::max();
  const auto lookup = table.lookup(low_pos, alpha, beta);
  ASSERT_TRUE(lookup.has_value());
  EXPECT_EQ(*lookup, 7);
  EXPECT_EQ(table.hits(), 1);
}

TEST(SingleDummySolverTest, TranspositionHitsDuringSolve)
{
  const auto result = solve_single_dummy(
      parse_hand("K98.3.."),
      parse_hand("AJT.2.."),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/0,
      parse_hand("Q76543.45.."));

  EXPECT_EQ(result.maximin_value, 3);
  EXPECT_GT(result.transposition_hits, 0);
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

TEST(SingleDummySolverTest, FiveCardNtClubSevenLeadGuaranteesFourTricks)
{
  // South: ♠T ♥J ♣K73   North: ♥T3 ♦A ♣Q8   Available: ♠KQJ ♥Q85 ♦J82 ♣5
  // Without void tracking during play, C7 drops from 4 to 3 (illegal heart re-deals).
  const auto result = solve_single_dummy(
      parse_hand("T.J..K73"),
      parse_hand(".T3.A.Q8"),
      /*known_seat=*/2,
      /*dummy_seat=*/0,
      /*denomination=*/4,
      parse_hand("KQJ.Q85.J82.5"));

  const auto c3_min = find_min(result, "C3");
  const auto c7_min = find_min(result, "C7");
  const auto king_min = find_min(result, "CK");
  ASSERT_TRUE(c3_min.has_value());
  ASSERT_TRUE(c7_min.has_value());
  ASSERT_TRUE(king_min.has_value());
  EXPECT_EQ(*c3_min, 4);
  EXPECT_EQ(*c7_min, 4);
  EXPECT_EQ(*king_min, 3);
  EXPECT_EQ(result.maximin_value, 4);
  EXPECT_EQ(result.maximin_cards.size(), 2u);
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
  EXPECT_EQ(*king_min, 4);
  EXPECT_EQ(*heart_min, 4);
  EXPECT_EQ(result.maximin_cards.size(), 5u);
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
      /*min_significant_rank=*/7);
  EXPECT_EQ(splits.size(), 2u);
}

TEST(SingleDummySolverTest, MinSignificantRankReducesInitialCombinationCount)
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

  EXPECT_GT(full.combination_count, merged.combination_count);
  EXPECT_EQ(full.maximin_value, merged.maximin_value);
}

}  // namespace
