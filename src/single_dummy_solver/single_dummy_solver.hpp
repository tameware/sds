/*
   DDS, a bridge double dummy solver.

   Copyright (C) 2006-2014 by Bo Haglund /
   2014-2016 by Bo Haglund & Soren Hein.

   See LICENSE and README.
*/

#pragma once

#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "bridge_types.hpp"

namespace single_dummy_solver
{

struct Card
{
  int suit;  // 0=spades … 3=clubs
  int rank;  // 2..14

  auto operator<=>(const Card&) const = default;
};

struct LeadResult
{
  Card card;
  int min_tricks;
};

struct UnknownSplit
{
  std::vector<Card> first;
  std::vector<Card> second;
};

struct SingleDummyResult
{
  std::vector<LeadResult> leads;
  std::vector<std::map<Card, int>> trick_matrix;
  int combination_count;
  int opposition_split_evaluations;
  int alpha_beta_cutoffs;
  int isomorphism_skipped_moves;
  int transposition_hits;
  int transposition_skipped_inexact;
  int maximin_value;
  std::vector<Card> maximin_cards;
};

auto parse_card(std::string_view text) -> Card;

auto parse_hand(std::string_view text) -> std::vector<Card>;

auto parse_seat(std::string_view text) -> int;

auto parse_denomination(std::string_view text) -> int;

auto parse_void_suits(std::string_view text) -> int;

auto validate_position(
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    const std::vector<Card>& available) -> void;

auto validate_seat_voids(
    const std::array<std::vector<Card>, kHands>& hands,
    const std::array<int, kHands>& seat_void_suits) -> void;

auto enumerate_unknown_splits(
    const std::vector<Card>& pool,
    int hand_size) -> std::vector<UnknownSplit>;

auto enumerate_constrained_opponent_splits(
    const std::vector<Card>& unassigned,
    int east_hand_size,
    int west_hand_size,
    int east_slots,
    int west_slots,
    int east_void_suits = 0,
    int west_void_suits = 0) -> std::vector<UnknownSplit>;

auto card_scores_from_future_tricks(
    const FutureTricks& fut,
    const std::vector<Card>& lead_cards) -> std::map<Card, int>;

auto distinct_representative_ranks_in_suit(
    int hand_mask,
    int unavailable_mask) -> std::vector<int>;

auto solve_single_dummy(
    const std::vector<Card>& known,
    const std::vector<Card>& dummy,
    int known_seat,
    int dummy_seat,
    int denomination,
    const std::vector<Card>& available,
    std::array<int, kHands> seat_void_suits = {}) -> SingleDummyResult;

auto format_result(const SingleDummyResult& result) -> std::string;

}  // namespace single_dummy_solver
