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
`deal.{seat}.hcp`, and `deal.{seat}.shape[index]`, with `len(...)`, comparisons,
and `and` / `or` / `not`.

Add `--diagram` to print the known hands, opposition seats, the shared
available-card pool, and any constraint before the analysis output.

After changing `MODULE.bazel`, refresh the lockfile:

```bash
bazelisk mod deps --lockfile_mode=update
```
