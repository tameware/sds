# SDS — single-dummy solver

Exhaustive single-dummy maximin analysis for bridge: before each partnership
play, opposition adversarially repartitions remaining unseen cards between East
and West, then minimizes; partnership plays maximize.

This repository contains the SDS utility. [dds-bridge/dds](https://github.com/dds-bridge/dds)
is included as a git submodule at `dds/` and linked via a Bazel `local_path_override`.

## Setup

```bash
git clone https://github.com/tameware/sds.git
cd sds
git submodule update --init --recursive
```

## Build and test

```bash
bazelisk test //:single_dummy_solver_test
bazelisk run //:single_dummy_solver -- \
  --known K98.3.. --known-seat S \
  --dummy AJT.2.. --dummy-seat N \
  --available Q76543.45.. --trump S
```

Void suits for mid-deal positions: `--north-void`, `--east-void`, `--south-void`,
and `--west-void` (suit letters `S/H/D/C`, comma-separated or run together).

After changing `MODULE.bazel`, refresh the lockfile:

```bash
bazelisk mod deps --lockfile_mode=update
```

## Updating DDS

```bash
cd dds
git fetch origin
git checkout <commit-or-tag>
cd ..
git add dds
git commit -m "Bump dds submodule."
```
