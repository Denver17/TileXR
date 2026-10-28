# TileLang + TileXR two-rank AllGather-GEMM

Historical execution record. The [ABI-v1-only design](../specs/2026-10-19-tilerelay-v1-only-design.md)
supersedes pipeline/v0 implementation and compatibility requirements below.
Those sources are frozen at `7ccaa16768ae2b2cb0da71d47e864352721de8b0` on
`baseline/tilerelay-v0-pipeline-20261019`; use the
[current integration guide](../TILELANG_INTEGRATION.md) for active commands.

## Contract

Target `ssh blue`, two Ascend 910B3 devices, CANN 9.1.0. Pin reference
TileScale to `71e71457d40ed6e6a2dad297936bfdbff21d104b` and TileLang Ascend
to `87e6d1c2f0a9d2ff6a9013d9a1202325c4a619b1`, including submodules.
User explicitly authorizes TileLang's `kernel<<<...>>>` launch for this
experiment. TileXR core remains C++14 and uses its existing launch path.

Each rank owns a row shard of A and a column shard of B. Every rank computes
`concat(A_0, A_1) @ B_rank`. FP16 inputs/output, FP32 accumulation. First
validate a serial baseline, then tile-level overlap; serial alone is not delivery.

## Execution

1. Prepare an isolated user-owned remote build/environment, preserving existing
   installations. Build pinned TileLang Ascend and current TileXR collectives.
2. Add an experimental Python runner under `examples/tilelang/`. Use the
   TileXR collectives ABI, explicitly select AIV peer-memory, and compile GEMM
   with TileLang Ascend. Keep reference sources comparison-only in TileXR builds.
3. Partition each A shard along M. A small C++14 Host-only bridge submits the
   tile loop and invokes the pinned generated three-buffer GEMM callback;
   Python per-tile submission gaps were longer than the initial kernels.
   For each tile enqueue AllGather on a
   communication stream and record a completion event. A compute stream waits
   for that event, runs GEMM and places both rank slices at global row offsets.
   Keep tile buffers/events alive through stream completion. Each tile gets
   an independent collective magic; do not clear shared flags.
4. Compare serial and overlapped output against a deterministic CPU oracle
   across shapes, repeated rounds and delayed ranks. Bound launches and propagate
   rank errors. Collect framework profiler traces showing actual AIV/Cube overlap.
5. Review/simplify changes and document provenance, correctness, timing,
   overlap evidence, reusable pitfalls and hardware coverage limits.

## Boundaries

No TileScale CUDA runtime installation or compiler merger, no UDMA proof on
910B, no system toolkit changes, no production wrapper launch changes.
Retain the existing one-time HCCL 8-device AIV-only record; it does not qualify
the 2-device subset. Do not rerun that environment's HCCL Test.

Acceptance requires both ranks to finish all correctness rounds, framework
trace evidence of tile communication/compute overlap, exact provenance and a
usable command for reproducing the test. A speedup is reported only if measured.

## 2026-10-13 optional-library promotion

The user requests a maintained, opt-in feature instead of an example-only
bridge. Move submission into `src/tilelang`, install `tilexr_tilelang.h` and
`libtilexr-tilelang.so` with `TILEXR_BUILD_TILELANG=ON` and collectives enabled.
Pin compiler/DSL source dependencies as `3rdparty` submodules. Preserve the
generated callback ABI, C++14, stream ordering and the existing runtime paths.
Reject non-two-rank or small peer windows before device submission. Validate
mocked ownership/error contracts, disabled/enabled build configurations,
installed-header/link consumption, and the two-device numerical/overlap matrix
against the new installed library. Retain the original standalone evidence as
a dated baseline and append current module provenance.
