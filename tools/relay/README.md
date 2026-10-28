# TileRelay benchmark tools and frozen Host-pipeline comparison

Two-rank FP16 AllGather-GEMM on 910B: each rank holds an M-row A shard and an
N-column B shard and computes `concat(A_rank0, A_rank1) @ B_local`, with FP32
accumulation and FP16 output. The current implementation uses one registered
mixed ABI-v1 kernel per rank/generation, combining TileXR AIV peer-memory
communication and TileLang Ascend-generated Cube computation.

The old ABI-v0 Relay and Host pipeline implementation are archived, not built
from this tree. `compare_relay.py` retains only a foreign-function adapter and
standalone GEMM generation needed to compare the current runtime with an
explicitly supplied historical pipeline library. Neither is a production
dependency. The [integration guide](../../docs/TILELANG_INTEGRATION.md) covers
current APIs; the [validation record](../../docs/TILELANG_ALLGATHER_GEMM_VALIDATION.md)
keeps historical measurements labelled with their original provenance.

These are maintained benchmark/trace-validation tools, not a minimal API example
or production dependency. Run their offline checker tests with
`python3 -m unittest discover -s tools/relay -p 'test_check_*.py'`.
When moving a current runner, update its CI discovery path and current commands,
but preserve paths inside the pinned historical checkout: its directory layout
is part of the baseline provenance, not the current repository layout.

## Preparation

Use an isolated Python 3.10+ environment with matching torch/torch_npu and CANN
9.1.0. Initialize and build the pinned Ascend compiler separately:

```bash
git submodule update --init --recursive 3rdparty/tilelang-ascend
```

See [third-party dependencies](../../3rdparty/README.md) for its revision and
upstream build instructions. Required compiler switches are `USE_ASCEND=ON`,
`USE_CUDA=OFF`, `USE_LLVM=OFF` and `USE_GTEST=OFF`; apply its TVM patches.
Source its `set_env.sh` after selecting CANN and the Python environment.
TileScale is comparison-only source under ignored `reference/tilescale`, not a
dependency. Do not install both projects' conflicting `tilelang` packages into
the same environment.

Build current Relay into a fresh prefix, without collectives:

```bash
git submodule update --init --recursive 3rdparty/spdlog
source scripts/common_env.sh
python scripts/build_tilexr_relay.py --operator allgather_gemm \
  --rows 1024 --n 4096 --k 4096 --blocks 16 \
  --compiler-revision 87e6d1c2f0a9d2ff6a9013d9a1202325c4a619b1 \
  --output build/relay-kernels
cmake -S . -B build-relay -DCMAKE_BUILD_TYPE=Release \
  -DTILEXR_BUILD_RELAY=ON -DTILEXR_BUILD_COLLECTIVES=OFF \
  -DTILEXR_RELAY_KERNEL_DIR="$PWD/build/relay-kernels" \
  -DCMAKE_INSTALL_PREFIX="$PWD/install-relay"
cmake --build build-relay -j"$(nproc)"
cmake --install build-relay
```

The builder emits the manifest and content-addressed ELF. Record its manifest
path for the command below; the runtime loads the matching ELF from
`lib[64]/tilexr/kernels` beside `libtilexr-relay.so`. The manifest's operator,
shape and blocks must match the comparison arguments; incompatible descriptions
are rejected before device allocation/launch. Existing 96-byte ABI-v0 artifacts
cannot be reused as v1 kernels.

## Frozen baseline build

The immutable baseline branch is `baseline/tilerelay-v0-pipeline-20261019`,
fixed at `7ccaa16768ae2b2cb0da71d47e864352721de8b0`. Always pin that commit,
not a moving branch name. From the current repository, create a separate
checkout at an unused path:

```bash
git fetch origin baseline/tilerelay-v0-pipeline-20261019
git worktree add --detach /path/to/TileXR-baseline \
  7ccaa16768ae2b2cb0da71d47e864352721de8b0
```

Run these commands in the historical checkout, not the current source tree:

```bash
cd /path/to/TileXR-baseline
git submodule update --init --recursive 3rdparty/spdlog
source scripts/common_env.sh
cmake -S . -B build-baseline -DCMAKE_BUILD_TYPE=Release \
  -DTILEXR_BUILD_COLLECTIVES=ON \
  -DCMAKE_INSTALL_PREFIX="$PWD/install-baseline"
cmake --build build-baseline -j"$(nproc)"
cmake --install build-baseline
cmake -S examples/tilelang -B build/pipeline-baseline \
  -DTILEXR_INSTALL_PREFIX="$PWD/install-baseline" -DCMAKE_BUILD_TYPE=Release
cmake --build build/pipeline-baseline -j"$(nproc)"
```

The external helper is
`/path/to/TileXR-baseline/build/pipeline-baseline/libtilexr-tilelang-benchmark.so`.
Its historical callback/header/tests remain in that checkout. Reproducing
ABI-v0-specific results also uses that checkout's documentation and runner;
the current comparison no longer supports a specialized Relay mode.

Keep historical and current installation prefixes separate. Never copy baseline
DSOs or kernels into the current installation. The comparison uses the baseline
prefix's compatible core/collectives dependencies for both paths in one process,
and explicitly selects the current Relay DSO. Build both against the same CANN,
driver and compiler environment, verify the actually loaded libraries, and
record their hashes. Pure ABI-v1 use needs neither this checkout nor collectives.

## Run the comparison

Return to the current checkout. With CANN, the isolated Python environment and
the pinned compiler loaded, select the baseline dependencies and current
manifest/runtime explicitly (use `lib` instead of `lib64` if configured):

```bash
export TILEXR_INSTALL_PREFIX=/path/to/TileXR-baseline/install-baseline
export LD_LIBRARY_PATH="$TILEXR_INSTALL_PREFIX/lib64:${LD_LIBRARY_PATH:-}"
export TASK_QUEUE_ENABLE=0
export TILEXR_ENABLE_UDMA=0
export OMP_NUM_THREADS=4
export TL_CCE_OPT_LEVEL=2
export TL_CCE_AUTO_SYNC=on
timeout --signal=TERM --kill-after=15s 480s \
  python -m torch.distributed.run --standalone --nproc_per_node=2 \
  -- tools/relay/compare_relay.py \
  --rows 1024 --n 4096 --k 4096 --blocks 16 --warmup 5 --iterations 20 \
  --baseline-lib /path/to/TileXR-baseline/build/pipeline-baseline/libtilexr-tilelang-benchmark.so \
  --generic-manifest "/path/to/current/build/relay-kernels/<manifest>.json" \
  --generic-runtime /path/to/current/install-relay/lib64/libtilexr-relay.so \
  --compiler-revision 87e6d1c2f0a9d2ff6a9013d9a1202325c4a619b1 \
  --host-build-flags "baseline=-O3 -DNDEBUG; current=-O3 -DNDEBUG" \
  --output /path/to/unique/relay-results
```

Check devices 0/1 are free before starting; this runner uses the two local ranks.
Replace the sample Host flags with the actual flags from both builds.
The modes are:

- `serial`: historical full AllGather, then full GEMM.
- `serial_tiled`: historical per-tile AllGather/GEMM, completing each tile first.
- `overlap`: historical Host pipeline, separate communication/computation streams.
- `generic_relay`: current ABI-v1 launch with prepared bindings.
- `generic_execution`: optional `--prepared-execution`, freezing native bindings.

All modes retain framework profiling, numerical checks and complete launch-count
checks. Use at least five warmups and twenty measured iterations. Add
`--delay-rank-ms 10` for delayed-rank correctness. Use `--reverse-mode-order`
or `--interleave-seed N` and repeat seeds to test order sensitivity; near ties
are not stable speedups. Record actual compiler/library/kernel hashes, raw rank
samples, Host flags and framework versions. `--compiler-revision` is an explicit
source attestation, not automatic Git discovery.

Host total time excludes allocations, input copies, output assembly and CPU
checking. Effective remote bandwidth divides remote input bytes by total time;
it is not isolated communication bandwidth. The trace checker counts a
Cube-bearing mixed launch once, not again for its paired vector subtask.
Framework preparation tasks are reported separately; unexpected non-framework
kernels and missing/duplicate rounds remain errors.

## Validation boundaries and pitfalls

- The historical pipeline uses tile-major buffers; current AllGather-GEMM uses
  rank-major buffers. The adapter must preserve both layouts and its callback's
  `void(uint8_t*, uint8_t*, uint8_t*, aclrtStream)` signature.
- Keep generated callback libraries, device storage, communicator and streams
  alive through synchronous completion. After timeout, terminate the process
  before freeing device storage. Do not run concurrent communicator operations.
- `TASK_QUEUE_ENABLE=0` preserves ordering between direct ctypes launches and
  framework streams. Use `python -m torch.distributed.run`, not a global
  `torchrun` from another environment. Put `--` before the script path so
  torchrun does not interpret the runner's `--n` as its own abbreviated option.
- Extra streams and correct numbers do not prove overlap. Require same-device
  intersections of later-tile AllGather and earlier-tile GEMM tasks, with serial
  controls. Sort profiler hardware timestamps before assigning tile indices.
- The historical baseline needs full collectives peer windows; the small
  memory-only domain is not interchangeable. Its synchronization events need
  `ACL_EVENT_TIME_LINE | ACL_EVENT_SYNC` on the validated CANN 9.1/910B path.
- Pipeline fault/ordering tests belong to the frozen checkout. Current Host
  contracts and ABI/operator/lifecycle probes live under `tests/relay`; ordinary
  positive coverage stays in the E2Es, not duplicated in benchmark mocks.
- Historical results are not automatically revalidated by code removal. Keep
  their commit, artifact identity, compiler and hardware boundaries intact.
- These tests cover two-rank, single-node 910B peer memory, not UDMA, multi-node
  execution or concurrent collectives. Reuse the retained one-time HCCL
  qualification record; do not claim it covers an untested device subset.
