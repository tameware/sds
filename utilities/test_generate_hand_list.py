#!/usr/bin/env python3
"""Tests for generate_hand_list.py."""

import random
import tempfile
import unittest
from pathlib import Path

from generate_hand_list import (
    TRUMPS,
    cards_to_pbn,
    deal_four_hands,
    file_seed,
    format_deal_line,
    generate_deals,
    list_filename,
    merge_opposition,
    parse_pbn_hand,
    validate_deal,
    write_hand_list,
)


class GenerateHandListTest(unittest.TestCase):
    def test_cards_to_pbn_sorts_ranks_high_to_low_within_suit(self):
        hand = [(0, "2"), (0, "A"), (0, "9"), (2, "K"), (3, "T")]
        self.assertEqual(cards_to_pbn(hand), "A92..K.T")

    def test_cards_to_pbn_uses_empty_fields_for_void_suits(self):
        self.assertEqual(cards_to_pbn([(1, "Q"), (1, "J")]), ".QJ..")

    def test_merge_opposition_combines_east_and_west_by_suit(self):
        east = [(0, "Q"), (3, "2"), (3, "3")]
        west = [(0, "7"), (0, "6"), (3, "4")]
        self.assertEqual(merge_opposition(east, west), "Q76...432")

    def test_deal_four_hands_deals_disjoint_hands_of_requested_size(self):
        rng = random.Random(7)
        hands = deal_four_hands(5, rng)
        self.assertEqual(len(hands), 4)
        self.assertTrue(all(len(hand) == 5 for hand in hands))
        flattened = [card for hand in hands for card in hand]
        self.assertEqual(len(flattened), len(set(flattened)))

    def test_format_deal_line_matches_solver_cli(self):
        north = [(0, "A"), (0, "J"), (0, "T"), (1, "2"), (3, "K")]
        east = [(0, "Q"), (0, "7"), (0, "6"), (0, "5"), (3, "2")]
        south = [(0, "K"), (0, "9"), (0, "8"), (1, "3"), (3, "A")]
        west = [(3, "7"), (3, "6"), (3, "5"), (3, "4"), (3, "3")]
        line = format_deal_line(north, east, south, west, "S")
        self.assertEqual(
            line,
            "--known K98.3..A --known-seat S --dummy AJT.2..K --dummy-seat N "
            "--available Q765...765432 --trump S",
        )

    def test_validate_deal_accepts_consistent_five_card_layout(self):
        north = [(0, "A"), (0, "J"), (0, "T"), (1, "2"), (3, "K")]
        east = [(0, "Q"), (0, "7"), (0, "6"), (0, "5"), (3, "2")]
        south = [(0, "K"), (0, "9"), (0, "8"), (1, "3"), (3, "A")]
        west = [(3, "7"), (3, "6"), (3, "5"), (3, "4"), (3, "3")]
        validate_deal(north, east, south, west, cards_per_hand=5)

    def test_validate_deal_rejects_duplicate_cards(self):
        north = [(0, "A"), (0, "A")]
        east = [(1, "2"), (1, "3")]
        south = [(2, "4"), (2, "5")]
        west = [(3, "6"), (3, "7")]
        with self.assertRaises(ValueError):
            validate_deal(north, east, south, west, cards_per_hand=2)

    def test_generate_deals_random_trump_varies_by_deal(self):
        rng = random.Random(file_seed(20, 3))
        deals = generate_deals(20, 3, rng)
        trumps = {line.rsplit("--trump ", 1)[1] for line in deals}
        self.assertTrue(trumps.issubset(set(TRUMPS)))
        self.assertGreater(len(trumps), 1)

    def test_generate_deals_produces_requested_count(self):
        rng = random.Random(11)
        deals = generate_deals(12, 4, rng, trump="H")
        self.assertEqual(len(deals), 12)
        for line in deals:
            self.assertTrue(line.startswith("--known "))
            self.assertIn("--trump H", line)

    def test_list_filename_uses_deal_and_hand_counts(self):
        self.assertEqual(list_filename(10, 5), "list10_5_cards.txt")

    def test_file_seed_is_fourteen_times_deal_count_plus_cards_per_hand(self):
        self.assertEqual(file_seed(10, 5), 145)
        self.assertEqual(file_seed(2, 3), 31)
        self.assertNotEqual(file_seed(10, 5), file_seed(10, 6))

    def test_write_hand_list_creates_comment_header_and_deal_lines(self):
        seed = file_seed(2, 3)
        rng = random.Random(seed)
        deals = generate_deals(2, 3, rng, trump="S")
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / list_filename(2, 3)
            write_hand_list(path, deals, deal_count=2, cards_per_hand=3, seed=seed)
            text = path.read_text(encoding="utf-8")
            self.assertIn("# 2 deals, 3 cards per hand, seed=31", text)
            self.assertEqual(len([line for line in text.splitlines() if line.startswith("--known ")]), 2)

    def test_round_trip_pbn_hand_parser(self):
        hand = [(0, "K"), (0, "9"), (1, "3"), (3, "A")]
        self.assertEqual(parse_pbn_hand(cards_to_pbn(hand)), hand)


if __name__ == "__main__":
    unittest.main()
