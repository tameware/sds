/*
   DDS, a bridge double dummy solver.

   Copyright (C) 2006-2014 by Bo Haglund /
   2014-2016 by Bo Haglund & Soren Hein.

   See LICENSE and README.
*/

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
         "--dummy HAND --dummy-seat SEAT [--available HAND] --trump DENOM "
         "[--constraint EXPR] [--diagram] [--info] [--goal TRICKS] [--threads N] "
         "[--min-significant-rank RANK] [--lead SEATCARD]\n"
      << "  N=0 selects hardware concurrency (default).\n"
      << "  RANK=2..14 or rank character; default 2 treats every card as significant.\n"
      << "  Hands use PBN suit fields (e.g. AJT.2..).\n"
      << "  Omit --available when known and dummy each hold 13 cards.\n"
      << "  Seats are N/E/S/W; denomination is S/H/D/C/NT.\n"
      << "  CONSTRAINT is a redeal-style Python expression on deal.north/east/"
         "south/west (e.g. A in deal.east.spades and deal.west.hcp >= 8).\n"
      << "  LEAD is opening lead as seat plus card (e.g. WH4). Results list legal "
         "follows for the first partnership hand to play on that trick.\n";
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
  bool show_info = false;
  std::optional<std::string> goal_text;
  std::optional<std::string> constraint_text;
  std::optional<std::string> lead_text;
  int thread_count = 0;
  int min_significant_rank = 2;

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
    if (arg == "--constraint" && index + 1 < argc)
    {
      constraint_text = argv[++index];
      continue;
    }
    if (arg == "--diagram")
    {
      show_diagram = true;
      continue;
    }
    if (arg == "--info")
    {
      show_info = true;
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
    if (arg == "--lead" && index + 1 < argc)
    {
      lead_text = argv[++index];
      continue;
    }
    if (arg == "--min-significant-rank" && index + 1 < argc)
    {
      min_significant_rank =
          single_dummy_solver::parse_min_significant_rank(argv[++index]);
      continue;
    }
    std::cerr << "Unknown or incomplete argument: " << arg << '\n';
    print_usage();
    return 1;
  }

  if (known_text.empty() || dummy_text.empty() || known_seat_text.empty()
      || dummy_seat_text.empty() || denomination_text.empty())
  {
    print_usage();
    return 1;
  }

  try
  {
    const auto known = single_dummy_solver::parse_hand(known_text);
    const auto dummy = single_dummy_solver::parse_hand(dummy_text);
    const auto available = available_text.empty()
        ? single_dummy_solver::available_cards_for_opponents(known, dummy)
        : single_dummy_solver::parse_hand(available_text);
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
      std::optional<std::string_view> constraint_view;
      if (constraint_text.has_value())
      {
        constraint_view = *constraint_text;
      }
      std::optional<single_dummy_solver::OpeningLead> opening_lead;
      if (lead_text.has_value())
      {
        opening_lead = single_dummy_solver::parse_opening_lead(*lead_text);
      }
      std::cout << single_dummy_solver::format_position_diagram(
          known,
          dummy,
          known_seat,
          dummy_seat,
          available,
          denomination,
          constraint_view,
          opening_lead);
      std::cout << '\n';
    }
    std::optional<single_dummy_solver::OpeningLead> opening_lead;
    if (lead_text.has_value())
    {
      opening_lead = single_dummy_solver::parse_opening_lead(*lead_text);
    }
    const auto result = single_dummy_solver::solve_single_dummy(
        known,
        dummy,
        known_seat,
        dummy_seat,
        denomination,
        available,
        constraint_text,
        goal,
        thread_count,
        min_significant_rank,
        opening_lead);
    std::cout << single_dummy_solver::format_result(result, show_info);
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
