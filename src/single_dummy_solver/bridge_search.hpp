// Search adapted from macroxue/bridge-solver (Apache-2.0 OR MIT): MTD(f) with
// shape/pattern bounds, for SDS shared-pool single-dummy positions.
#pragma once

#include "game_state.hpp"

namespace single_dummy_solver
{

// Exact partnership-trick count from `state` under shared-pool opposition play.
auto search_partnership_tricks(GameState state, SearchStats& stats) -> int;

}  // namespace single_dummy_solver
