#pragma once

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bridge_types.hpp"

namespace single_dummy_solver
{

struct DealConstraintPin
{
  int seat = 0;
  int suit = 0;
  int rank = 0;
};

struct DealConstraintVoid
{
  int seat = 0;
  int suit = 0;
};

class DealConstraint
{
public:
  DealConstraint();
  explicit DealConstraint(std::string_view expression);

  auto evaluate(const std::array<std::vector<Card>, kHands>& hands) const -> bool;

  auto is_active() const -> bool;

  auto pinned_cards() const -> std::vector<DealConstraintPin>;

  // Suits forced empty at the start of play by length-zero comparisons such as
  // `len(deal.east.spades) == 0`.
  auto void_suits() const -> std::vector<DealConstraintVoid>;

private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
};

auto compile_deal_constraint(std::string_view expression) -> DealConstraint;

}  // namespace single_dummy_solver
