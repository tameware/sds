#pragma once

namespace single_dummy_solver
{

constexpr int kHands = 4;  // N, E, S, W
constexpr int kSuits = 4;  // S, H, D, C

// Rank bitmasks used in FutureTricks.equals (DDS encoding: one bit per rank 2..14,
// then shifted left by 2 when stored in equals[]).
constexpr unsigned kRankBitmaskSix = 0x0010u;
constexpr unsigned kRankBitmaskQueen = 0x0400u;

// Mirrors the FutureTricks layout from the DDS public API. SDS uses this only
// to decode equals-bitmask scores in card_scores_from_future_tricks().
struct FutureTricks
{
  int nodes;
  int cards;
  int suit[13];
  int rank[13];
  int equals[13];
  int score[13];
};

}  // namespace single_dummy_solver
