/*
   DDS, a bridge double dummy solver.

   Copyright (C) 2006-2014 by Bo Haglund /
   2014-2016 by Bo Haglund & Soren Hein.

   See LICENSE and README.
*/

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

#include "single_dummy_solver.hpp"

namespace
{

void print_usage()
{
  std::cerr
      << "Usage: single_dummy_solver --known HAND --known-seat SEAT "
         "--dummy HAND --dummy-seat SEAT --available HAND --trump DENOM "
         "[--north-void SUITS] [--east-void SUITS] [--south-void SUITS] "
         "[--west-void SUITS] [--diagram] [--goal TRICKS] [--threads N]\n"
      << "  N=0 selects hardware concurrency (default).\n"
      << "  Hands use PBN suit fields (e.g. AJT.2..).\n"
      << "  Seats are N/E/S/W; denomination is S/H/D/C/NT.\n"
      << "  Void suits use S/H/D/C, comma-separated or run together (e.g. S,H "
         "or SH).\n";
}

auto parse_optional_void(std::string_view text) -> int
{
  return text.empty() ? 0 : single_dummy_solver::parse_void_suits(text);
}

}  // namespace

auto main(int argc, char* argv[]) -> int
{
  std::string known_text;
  std::string dummy_text;
  std::string available_text;
  std::string known_seat_text;
  std::string dummy_seat_text;
  std::string denomination_text;
  bool show_diagram = false;
  std::optional<std::string> goal_text;
  int thread_count = 0;
  std::array<std::string, single_dummy_solver::kHands> void_text{};

  for (int index = 1; index < argc; ++index)
  {
    const std::string arg = argv[index];
    if (arg == "--help" || arg == "-h")
    {
      print_usage();
      return 0;
    }
    if (arg == "--known" && index + 1 < argc)
    {
      known_text = argv[++index];
      continue;
    }
    if (arg == "--dummy" && index + 1 < argc)
    {
      dummy_text = argv[++index];
      continue;
    }
    if (arg == "--available" && index + 1 < argc)
    {
      available_text = argv[++index];
      continue;
    }
    if (arg == "--known-seat" && index + 1 < argc)
    {
      known_seat_text = argv[++index];
      continue;
    }
    if (arg == "--dummy-seat" && index + 1 < argc)
    {
      dummy_seat_text = argv[++index];
      continue;
    }
    if ((arg == "--trump" || arg == "--denomination") && index + 1 < argc)
    {
      denomination_text = argv[++index];
      continue;
    }
    if (arg == "--north-void" && index + 1 < argc)
    {
      void_text[0] = argv[++index];
      continue;
    }
    if (arg == "--east-void" && index + 1 < argc)
    {
      void_text[1] = argv[++index];
      continue;
    }
    if (arg == "--south-void" && index + 1 < argc)
    {
      void_text[2] = argv[++index];
      continue;
    }
    if (arg == "--west-void" && index + 1 < argc)
    {
      void_text[3] = argv[++index];
      continue;
    }
    if (arg == "--diagram")
    {
      show_diagram = true;
      continue;
    }
    if (arg == "--goal" && index + 1 < argc)
    {
      goal_text = argv[++index];
      continue;
    }
    if (arg == "--threads" && index + 1 < argc)
    {
      thread_count = single_dummy_solver::parse_threads(argv[++index]);
      continue;
    }
    std::cerr << "Unknown or incomplete argument: " << arg << '\n';
    print_usage();
    return 1;
  }

  if (known_text.empty() || dummy_text.empty() || available_text.empty()
      || known_seat_text.empty() || dummy_seat_text.empty()
      || denomination_text.empty())
  {
    print_usage();
    return 1;
  }

  try
  {
    std::array<int, single_dummy_solver::kHands> seat_void_suits{};
    for (int seat = 0; seat < single_dummy_solver::kHands; ++seat)
    {
      seat_void_suits[static_cast<std::size_t>(seat)] =
          parse_optional_void(void_text[static_cast<std::size_t>(seat)]);
    }
    const auto known = single_dummy_solver::parse_hand(known_text);
    const auto dummy = single_dummy_solver::parse_hand(dummy_text);
    const auto available = single_dummy_solver::parse_hand(available_text);
    const int known_seat = single_dummy_solver::parse_seat(known_seat_text);
    const int dummy_seat = single_dummy_solver::parse_seat(dummy_seat_text);
    const int denomination =
        single_dummy_solver::parse_denomination(denomination_text);
    std::optional<int> goal;
    if (goal_text.has_value())
    {
      goal = single_dummy_solver::parse_goal(*goal_text);
    }
    if (show_diagram)
    {
      std::cout << single_dummy_solver::format_position_diagram(
          known,
          dummy,
          known_seat,
          dummy_seat,
          available,
          denomination,
          seat_void_suits);
      std::cout << '\n';
    }
    const auto result = single_dummy_solver::solve_single_dummy(
        known,
        dummy,
        known_seat,
        dummy_seat,
        denomination,
        available,
        seat_void_suits,
        goal,
        thread_count);
    std::cout << single_dummy_solver::format_result(result);
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
