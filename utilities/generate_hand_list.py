#!/usr/bin/env python3
"""Generate hand-list files for single_dummy_solver."""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

SUITS = "SHDC"
RANKS = "AKQJT98765432"
TRUMPS = ("S", "H", "D", "C", "NT")
Card = tuple[int, str]


def cards_to_pbn(hand: list[Card]) -> str:
    by_suit: list[list[str]] = [[] for _ in range(4)]
    for suit, rank in hand:
        by_suit[suit].append(rank)
    fields = []
    for suit_cards in by_suit:
        suit_cards.sort(key=RANKS.index)
        fields.append("".join(suit_cards))
    return ".".join(fields)


def parse_pbn_hand(pbn: str) -> list[Card]:
    fields = pbn.split(".")
    if len(fields) != 4:
        raise ValueError(f"hand must have four suit fields: {pbn!r}")
    cards: list[Card] = []
    for suit, field in enumerate(fields):
        for rank in field:
            if rank not in RANKS:
                raise ValueError(f"invalid rank {rank!r} in hand {pbn!r}")
            cards.append((suit, rank))
    return cards


def merge_opposition(east: list[Card], west: list[Card]) -> str:
    return cards_to_pbn(east + west)


def deal_four_hands(cards_per_hand: int, rng: random.Random) -> list[list[Card]]:
    if cards_per_hand < 1 or cards_per_hand > 13:
        raise ValueError("cards per hand must be between 1 and 13")
    deck: list[Card] = [(suit, rank) for suit in range(4) for rank in RANKS]
    rng.shuffle(deck)
    return [
        deck[index * cards_per_hand : (index + 1) * cards_per_hand]
        for index in range(4)
    ]


def validate_deal(
    north: list[Card],
    east: list[Card],
    south: list[Card],
    west: list[Card],
    *,
    cards_per_hand: int,
) -> None:
    hands = (north, east, south, west)
    if any(len(hand) != cards_per_hand for hand in hands):
        raise ValueError("each compass hand must have the requested card count")
    flattened = [card for hand in hands for card in hand]
    if len(flattened) != len(set(flattened)):
        raise ValueError("deal contains duplicate cards")


def format_deal_line(
    north: list[Card],
    east: list[Card],
    south: list[Card],
    west: list[Card],
    trump: str,
) -> str:
    known = cards_to_pbn(south)
    dummy = cards_to_pbn(north)
    available = merge_opposition(east, west)
    return (
        f"--known {known} --known-seat S --dummy {dummy} --dummy-seat N "
        f"--available {available} --trump {trump}"
    )


def pick_trump(rng: random.Random, trump: str) -> str:
    if trump == "random":
        return rng.choice(TRUMPS)
    if trump not in TRUMPS:
        raise ValueError(f"trump must be one of {TRUMPS} or 'random'")
    return trump


def generate_deals(
    deal_count: int,
    cards_per_hand: int,
    rng: random.Random,
    *,
    trump: str = "random",
) -> list[str]:
    if deal_count < 1:
        raise ValueError("deal count must be at least 1")
    lines: list[str] = []
    for _ in range(deal_count):
        north, east, south, west = deal_four_hands(cards_per_hand, rng)
        validate_deal(
            north,
            east,
            south,
            west,
            cards_per_hand=cards_per_hand,
        )
        lines.append(
            format_deal_line(
                north,
                east,
                south,
                west,
                pick_trump(rng, trump),
            )
        )
    return lines


def list_filename(deal_count: int, cards_per_hand: int) -> str:
    return f"list{deal_count}_{cards_per_hand}_cards.txt"


def file_seed(deal_count: int, cards_per_hand: int) -> int:
    return 14 * deal_count + cards_per_hand


def write_hand_list(
    path: Path,
    deals: list[str],
    *,
    deal_count: int,
    cards_per_hand: int,
    seed: int,
) -> None:
    header = (
        f"# {deal_count} deals, {cards_per_hand} cards per hand, seed={seed}\n"
        "# One deal per line; pass a line to single_dummy_solver.\n"
    )
    body = "\n".join(deals)
    path.write_text(f"{header}\n{body}\n", encoding="utf-8")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Generate random bridge deals for single_dummy_solver. "
            "Each deal gives South the known hand, North the dummy, and "
            "East/West cards in the available pool."
        ),
    )
    parser.add_argument(
        "deal_count",
        type=int,
        help="number of deals to generate (N in listN_C_cards.txt)",
    )
    parser.add_argument(
        "cards_per_hand",
        type=int,
        help="cards dealt to each compass hand (C in listN_C_cards.txt)",
    )
    parser.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        default=Path("."),
        help="directory for the output file (default: current directory)",
    )
    parser.add_argument(
        "--trump",
        default="random",
        help="contract trump S/H/D/C/NT, or 'random' per deal (default: random)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    seed = file_seed(args.deal_count, args.cards_per_hand)
    rng = random.Random(seed)
    deals = generate_deals(
        args.deal_count,
        args.cards_per_hand,
        rng,
        trump=args.trump,
    )
    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    path = output_dir / list_filename(args.deal_count, args.cards_per_hand)
    write_hand_list(
        path,
        deals,
        deal_count=args.deal_count,
        cards_per_hand=args.cards_per_hand,
        seed=seed,
    )
    print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
