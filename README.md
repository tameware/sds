# SDS — single-dummy solver

Exhaustive single-dummy maximin analysis for bridge: the two opposition seats
share one pool of unseen cards. When an opponent plays, they may choose any
remaining pool card to best advantage except cards in a suit they have already
shown out of (follow-suit still applies while that suit remains and they are
not void). The maximizing side plays to maximize against that defense.

Search (MTD(f), shape/pattern bounds, relative-rank move collapse, heuristic
ordering) is adapted from
[macroxue/bridge-solver](https://github.com/macroxue/bridge-solver)
(Apache-2.0 OR MIT) for SDS shared-pool positions.

When `--known` and `--dummy` are bridge partners (N–S or E–W), this is classic
declarer single-dummy: both named hands are visible and maximize; the other two
seats share the available pool.

When they are adjacent (for example known South, dummy East), the solver runs
**defender** single-dummy: maximize tricks for known and known’s partner; dummy
is a fully known opponent; the available pool fills the other two seats
(partner and the remaining opponent).

## Setup

```bash
git clone https://github.com/tameware/sds.git
cd sds
```

## Build and test

```bash
bazelisk test //:single_dummy_solver_test
bazelisk run //:single_dummy_solver -- \
  --known K98.3..A --known-seat S \
  --dummy AJT.2..K --dummy-seat N \
  --available Q765...234567 --trump S \
  --diagram
```

Optional opposition constraints use [redeal](https://github.com/anntzer/redeal)-style
accept expressions with `--constraint EXPR`. For example:

```bash
--constraint "len(deal.east.spades) == 4 and deal.west.hcp >= 8"
```

When known and dummy each hold 13 cards, omit `--available`; the remaining 26
cards are inferred automatically.

Expressions may reference `deal.{north,east,south,west}.{spades,hearts,diamonds,clubs}`,
`deal.{seat}.hcp`, and `deal.{seat}.shape[index]`, honor membership (rank `2`–`14`
or `A`/`K`/`Q`/`J`/`T` `in deal.{seat}.{suit}`), with `len(...)`, comparisons, and `and` / `or` / `not`.
Rank membership pins that card to the named seat while it remains unseen; length
constraints such as `len(deal.east.spades) == 0` mark that seat void in the suit
from the start of play.

Add `--diagram` to print the known hands, opposition seats, the shared
available-card pool, and any constraint before the analysis output.

Specify an opening lead as `--lead SEATCARD` (for example `--lead WH4` for
West leads the ♥4). The table lists minimum tricks for each legal card the
first maximizing hand to play may contribute on that trick (following suit
when required). That responding seat must be known or dummy (not the unknown
partner in defender layouts).

After changing `MODULE.bazel`, refresh the lockfile:

```bash
bazelisk mod deps --lockfile_mode=update
```
