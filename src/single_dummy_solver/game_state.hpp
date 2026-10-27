// Game position for shared-pool single-dummy search.
#pragma once

#include <array>
#include <optional>
#include <vector>

#include "bridge_types.hpp"
#include "deal_constraints.hpp"

namespace single_dummy_solver
{

struct PlayedCard
{
  int seat = 0;
  Card card{};
};

struct GameState
{
  int denomination = 0;
  int known_seat = 0;
  int dummy_seat = 0;
  int east_seat = 0;
  int west_seat = 0;
  std::array<std::vector<Card>, kHands> hands{};
  std::vector<Card> remaining_available;
  int leader = 0;
  std::vector<PlayedCard> trick;
  int partnership_tricks = 0;
  int east_slots = 0;
  int west_slots = 0;
  std::array<int, kHands> void_suits{};
  std::array<int, kSuits> win_ranks{};
  std::vector<DealConstraintPin> pins;
  bool force_next_play = false;
  int forced_play_seat = -1;
  Card forced_play{};
};

struct SearchStats
{
  int opposition_split_evaluations = 0;
  int alpha_beta_cutoffs = 0;
  int isomorphism_skipped_moves = 0;
  int pattern_hits = 0;
  int mtd_iterations = 0;
  int goal_cutoffs = 0;
  std::optional<int> goal;
};

}  // namespace single_dummy_solver
