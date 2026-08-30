# SDS — single-dummy solver

Exhaustive single-dummy maximin analysis for bridge: before each partnership
play, opposition adversarially repartitions remaining unseen cards between East
and West, then minimizes; partnership plays maximize.

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

Expressions may reference `deal.{north,east,south,west}.{spades,hearts,diamonds,clubs}`,
`deal.{seat}.hcp`, and `deal.{seat}.shape[index]`, honor membership (rank `2`–`14`
or `A`/`K`/`Q`/`J`/`T` `in deal.{seat}.{suit}`), with `len(...)`, comparisons, and `and` / `or` / `not`.
Rank membership pins that card to the seat on every opposition repartition while it
remains unseen; length constraints such as `len(deal.east.spades) == 0` apply at the
initial split and are enforced on later repartitions via void-suit tracking.

Add `--diagram` to print the known hands, opposition seats, the shared
available-card pool, and any constraint before the analysis output.

After changing `MODULE.bazel`, refresh the lockfile:

```bash
bazelisk mod deps --lockfile_mode=update
```
