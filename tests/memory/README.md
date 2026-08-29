# TileXR Peer-Memory DataCopy Demo

This directory contains a small memory-semantics example modeled after the reference-only `ascend-transformer-boost/src/kernels/lcal/src/kernels/lcal_allgather.cce` source.

The example uses TileXR `CommArgs::peerMems[]` as shared peer-memory windows and moves data through UB with Ascend C `DataCopy`. It intentionally does not use `TileXRUDMARegister`, `tilexr_udma.h`, or UDMA put/get APIs.

## Build

Build TileXR first:

```bash
cd /path/to/TileXR
source scripts/common_env.sh
cmake -S . -B /tmp/tilexr-build-memory -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build /tmp/tilexr-build-memory --target tile-comm -j"$(nproc)"
cmake --install /tmp/tilexr-build-memory
```

Then build this example:

```bash
cd /path/to/TileXR/tests/memory
bash build.sh
```

If `bisheng` is unavailable, the AICore demo is skipped.

## Run

```bash
cd /path/to/TileXR/tests/memory
bash demo/run_tilexr_memory_demo.sh 2 16 2 0
```

Arguments:

```text
run_tilexr_memory_demo.sh <rank_size> <elements_per_rank> <npu_count> <first_npu>
```

`elements_per_rank` is the number of `int32_t` values per rank. Keep each segment 32-byte aligned for this simple `DataCopy` example, for example `16`.

Each run writes per-rank logs under `tests/memory/logs/tilexr_memory_demo_*`.
