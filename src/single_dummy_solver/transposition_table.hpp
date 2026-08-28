#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "bridge_types.hpp"

namespace single_dummy_solver
{

struct Card;

/// Snapshot of a search position for DDS-style transposition-table lookup.
struct TranspositionPosition
{
  int partnership_tricks = 0;
  int east_slots = 0;
  int west_slots = 0;
  int leader = 0;
  int trick_size = 0;
  std::array<int, kHands> void_suits{};
  std::array<std::array<int, kHands>, kSuits> cards_per_suit{};
  std::array<int, kSuits> aggregate{};
  std::array<unsigned, kSuits> order_set{};
  std::array<unsigned, kSuits> win_mask{};
  std::array<unsigned, kSuits> stored_order_set{};
  std::array<int, 4> trick_seats{};
  std::array<int, 4> trick_suits{};
  std::array<int, 4> trick_relative_ranks{};
};

class TranspositionTable
{
 public:
  static constexpr std::size_t kBucketCount = 4096;
  auto lookup(const TranspositionPosition& position) -> std::optional<int>;

  auto store(const TranspositionPosition& position, int value) -> void;

  auto hits() const -> int { return hits_; }

  auto stores() const -> int { return stores_; }

 private:
  struct Entry
  {
    std::uint64_t suit_lengths = 0;
    std::uint32_t context_hash = 0;
    std::array<unsigned, kSuits> win_mask{};
    std::array<unsigned, kSuits> order_set{};
    int value = 0;
  };

  static auto suit_lengths_key(const TranspositionPosition& position) -> std::uint64_t;

  static auto context_hash(const TranspositionPosition& position) -> std::uint32_t;

  static auto bucket_index(const TranspositionPosition& position) -> std::size_t;

  static auto matches(const Entry& entry, const TranspositionPosition& position)
      -> bool;

  std::vector<std::vector<Entry>> buckets_{kBucketCount};
  int hits_ = 0;
  int stores_ = 0;
};

auto build_transposition_position(
    int partnership_tricks,
    int east_slots,
    int west_slots,
    int leader,
    const std::array<std::vector<Card>, kHands>& hands,
    const std::vector<std::pair<int, Card>>& trick,
    const std::array<int, kHands>& void_suits,
    const std::array<int, kSuits>& win_ranks) -> TranspositionPosition;

auto encode_suit_order_set(
    int suit,
    int aggregate,
    const std::array<std::vector<Card>, kHands>& hands,
    const std::vector<std::pair<int, Card>>& trick) -> unsigned;

auto relative_rank_in_suit(int aggregate, int absolute_rank) -> int;

auto encoding_win_mask(int aggregate, int lowest_winning_absolute_rank) -> unsigned;

}  // namespace single_dummy_solver
