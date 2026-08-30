#pragma once

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bridge_types.hpp"

namespace single_dummy_solver
{

class DealConstraint
{
public:
  DealConstraint();
  explicit DealConstraint(std::string_view expression);

  auto evaluate(const std::array<std::vector<Card>, kHands>& hands) const -> bool;

  auto is_active() const -> bool;

private:
  struct Impl;
  std::shared_ptr<const Impl> impl_;
};

auto compile_deal_constraint(std::string_view expression) -> DealConstraint;

}  // namespace single_dummy_solver
