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
  --available Q765...234567 --trump S
```

Void suits for mid-deal positions: `--north-void`, `--east-void`, `--south-void`,
and `--west-void` (suit letters `S/H/D/C`, comma-separated or run together).

After changing `MODULE.bazel`, refresh the lockfile:

```bash
bazelisk mod deps --lockfile_mode=update
```
