// Shared-pool play and legal-move helpers used by bridge-style search.
#pragma once

#include <vector>

#include "bridge_types.hpp"
#include "game_state.hpp"

namespace single_dummy_solver::detail
{

auto partner_seat(int seat) -> int;
auto is_partnership_seat(int seat, int known_seat) -> bool;
auto is_opponent_seat(const GameState& state, int seat) -> bool;
auto current_player(const GameState& state) -> int;
auto play_complete(const GameState& state) -> bool;
auto apply_play(GameState state, const Card& card) -> GameState;
auto ordered_legal_moves(const GameState& state, int seat) -> std::vector<Card>;
auto count_isomorphism_skipped(const GameState& state, int seat) -> int;
auto remaining_tricks_upper_bound(const GameState& state) -> int;

}  // namespace single_dummy_solver::detail
