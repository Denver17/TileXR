# TileLang Ascend + TileXR two-card validation

This is a chronological evidence record. ABI-v0 and Host pipeline sources are
now frozen at `7ccaa16768ae2b2cb0da71d47e864352721de8b0` on
`baseline/tilerelay-v0-pipeline-20261019`; their earlier results below are not
current API/build instructions. See the [ABI-v1-only design](specs/2026-10-19-tilerelay-v1-only-design.md)
and [current integration guide](TILELANG_INTEGRATION.md).

Validated on 2026-10-12 using `ssh blue`, devices 0 and 1 (Ascend 910B3),
CANN 9.1.0 compiler timestamp `20260520_000325981`, driver 25.5.0,
torch 2.9.0+cpu with torch_npu 2.9.0, and pinned TileLang Ascend 0.1.4.
The board query did not report firmware version; no firmware compatibility
claim is made. See [current benchmark commands](../tools/relay/README.md).

## Result and scope

Both ranks completed a real TileXR AIV peer-memory AllGather followed by
TileLang-generated GEMM, including a tile pipeline on independent NPU streams.
Hardware profiler traces confirm later-tile communication overlaps
earlier-tile computation. This is a native Host submission pipeline with
per-tile events, not a port of the TileScale distributed DSL or a single fused
device kernel. The user explicitly authorized TileLang's `kernel<<<...>>>`
wrapper for this experiment; TileXR production launch paths remain unchanged.

Each rank holds a row shard of A and a column shard of B and computes
`concat(A_0, A_1) @ B_local`. Inputs/output are FP16 with FP32 accumulation.
Every round changes the A payload. Gathered inputs are compared exactly;
GEMM output is compared with CPU FP32 multiplication rounded to FP16,
using `rtol=0.01, atol=0.01`.

## Frozen-source hardware matrix

All three modes (`serial`, `serial_tiled`, `overlap`) ran in each group.
Rounds below include warmup, which also undergoes numerical validation.

| Group | Global M / local N / K | Rows per rank per tile | Rounds per mode | Overlapping hardware task pairs, rank 0 / rank 1 |
| --- | --- | --- | --- | --- |
| final-large | 2048 / 4096 / 4096 | 128 (8 tiles) | 7 (2 warmup + 5 measured) | 69 / 67 |
| final-small | 1024 / 2048 / 2048 | 128 (4 tiles) | 4 (1 + 3) | 10 / 11 |
| final-delay | 1024 / 4096 / 4096 | 128 (4 tiles), rank 1 delayed 10 ms | 4 (1 + 3) | 17 / 17 |

All **90 rank-rounds** passed both numerical checks. Both serial controls
have zero hardware communication/compute overlap. The checker additionally
requires completed numerical result files, complete round coverage and the
correct number of tasks per tile. Intersections are measured only between
`TileXRAllGather_float16_t` (`KERNEL_AIVEC`) and `main_kernel`
(`KERNEL_MIX_AIC`, Cube-bearing), on the same hardware PID and different
physical streams. The communication tile index must exceed the compute tile
index; overlap with a tile's own input is rejected.

Five standard-library unit tests cover cross-device false positives,
consumer-before-ready ordering, missing tasks, serial controls, and
sub-microsecond timestamp precision. Final native bridge compilation and
Python syntax checks also passed. Null native arguments and unsupported
64-row tiles are rejected before device execution. Independent source review found no concrete
defects. All NPU processes exited after the final matrix.

## Measured timing

`final-large`: 8 MiB remote A traffic per rank, 68.72 GFLOP GEMM per rank,
16 MiB output per rank. Both stages are native: AllGather uses TileXR AIV
peer-memory; GEMM uses TileLang Ascend C. Kernel versions are the artifact
hashes recorded below. Framework NPU profiler is enabled in all modes, with
a Gloo rank barrier before every round and explicit stream/event dependencies.
No profiling-disabled baseline is used.

Each rank first takes its five-iteration median. Values below are then
the **median [minimum, maximum] across both ranks**.

| Mode | Pipeline wall time (ms) | AllGather stream intervals (ms) | GEMM stream intervals (ms) | Remote AllGather bandwidth (GB/s) |
| --- | --- | --- | --- | --- |
| Full serial | 1.531 [1.522, 1.539] | 0.507 [0.484, 0.531] | 0.888 [0.885, 0.892] | 16.57 [15.81, 17.34] |
| Serial tiled | 2.717 [2.695, 2.740] | 0.658 [0.634, 0.683] | 1.196 [1.193, 1.199] | 12.76 [12.28, 13.24] |
| Overlapped tiled | 1.353 [1.347, 1.359] | 0.750 [0.746, 0.754] | 1.101 [1.099, 1.102] | 11.18 [11.12, 11.24] |

The measured wall-time ratios are **2.01x versus serial tiled** and
**1.13x versus full serial** for this group. These are short, profiler-enabled
measurements on the tested pair, not a general performance guarantee.
Pipeline wall time excludes event/buffer allocation, event destruction and
CPU output assembly/checking. Stage times are sums of same-stream event
intervals, including stalls and submission gaps; they are not exclusive
kernel execution time. Hardware trace intersections establish overlap
independently of these event measurements. The delayed-rank group is a
correctness/ownership stress test and is not a performance baseline.

TileXR was built with the existing default CMake configuration
(`CMAKE_BUILD_TYPE` unset, Host C++14); the standalone Host bridge used
`g++ -std=c++14 -O2 -shared -fPIC`. TileLang compiler build used Release,
`USE_ASCEND=ON`, `USE_CUDA=OFF`, `USE_LLVM=OFF`, `USE_GTEST=OFF`, with the
upstream TVM patches applied. This is a functional experiment, not a tuned
production Release comparison.

## Reproduction and evidence

Remote scratch root: `/home/d00520898/tilexr-tilelang-20261012.dc9E6j`.
The final bridge is `pipeline-final.so`; final result groups are
`final-large`, `final-small`, and `final-delay`. Logs are `final-<group>.log`.
Use the command in the implementation guide; the large group uses defaults,
small adds `--rows-per-rank 512 --n 2048 --k 2048 --iterations 3 --warmup 1`,
and delay adds `--rows-per-rank 512 --delay-rank-ms 10 --iterations 3 --warmup 1`.
Every launcher had a 300-second process-group timeout with a 15-second kill
deadline. Run `check_overlap.py` on each unique result directory.

Local retained traces, CSVs, generated GEMM, numerical summaries and overlap
records are under ignored `run/tilelang-allgather-gemm-20261012/final-*/`.
Each `summary.json` contains all loaded artifact paths and SHA256 hashes;
`overlap.json` contains every intersecting tile/stream pair. Raw scratch
artifacts and host-specific launch helpers are not committed.

Source snapshot: TileXR core `9d6b97b`; the integration sources and this
record are delivered together. Reference source versions:

- TileScale: `71e71457d40ed6e6a2dad297936bfdbff21d104b`, including recursive submodules.
- TileLang Ascend: `87e6d1c2f0a9d2ff6a9013d9a1202325c4a619b1`, including recursive submodules.

All three final groups recorded identical integration source hashes:

| Artifact | SHA256 |
| --- | --- |
| Python runner | `053b3bf5a66c8c978d0d3bdba38c25bdc6e700f3afeb5bd9ff76001ecdce74e4` |
| Host bridge source | `95858b1b789f5b3d22725615e18e0f787d0581a2fa555d2adbdf4e76856b3aff` |
| Host bridge binary | `3ff24015aaaa1ec806b3bbf04375e0066f83e29b350ea491852f32941e171c36` |
| libtile-comm.so | `abb30173139515c710bb53a20011db51466c86272aade1e5c87540436438e707` |
| libtilexr-collectives.so | `91c23b716037eadb5e1491d33438aa6d9ea944e56b2bd46ee45d6748ab111d3b` |
| External AIV collectives object | `aaa1d06f9a41440e20c5b2834f76dc2fc613935d0d67e4f1a56c61ebe6c7c16a` |
| TileLang compiler library | `d061a88a88056560917338bf9cb32ed510239c45f73ec6fdab8fcf9d216019d7` |
| Large full GEMM library | `500067402508b95b1fde8480c419d6b54a496e6f3d76279fb4f9b9380358721f` |
| Large tiled GEMM library | `65a8def8951e87fb154c7f4af4ed5120261c4dbd75a8e34277a44865745aa551` |
| Bisheng compiler | `12f6e47edb6045f5b070ea67ac8d5af7ae29a02dd447c8a4793d20cf9a963d35` |

The bridge and TileXR Host libraries have no RPATH/RUNPATH entries; driver
HAL resolves through the configured real driver paths, not CANN devlib.

The retained one-time HCCL Test record at
`tilexr-tp8-probe-20261009/evidence/hccl-identity.txt` covers devices 0–7,
single-node, AIV-only, CANN 9.1.0/driver 25.5.0. Its official output reported
success at 8192 and 131072 bytes, with no rank failure lines; positive output
was emitted only by the root. It was not rerun. That record does not qualify
the exact two-device subset; current two-card numerical and trace results
are separate evidence. No UDMA or multi-node data-plane claim is made.

## Lessons retained in the implementation guide

Python submission gaps can eliminate actual overlap despite correct stream
dependencies. Native submission fixes that boundary without changing the
calculation. Timing-only ACL events require the synchronization flag for
cross-stream waits on this tested runtime. Generated callback ABI, collective
ping-pong allocation and binary provenance must be verified explicitly.
The initial zero-overlap trace and the event-flag failure remain retained
under the remote scratch root; neither is presented as a successful delivery.

## 2026-10-13 review and simplification

The overlap checker now parses hardware start timestamps once and performs
one stable sort before filtering rounds and stages. This removes repeated
timestamp conversion and stage sorting while preserving task order, output
records and acceptance conditions. The guide records why JSON emission order
must not be used as tile execution order.

All six checker tests pass, including shuffled hardware-event input. Old and
new analyzers produce exactly equal records for all six retained hardware
traces; all three result groups pass the updated checker. Python syntax and
diff checks pass, and independent review found no concrete defects.
The runner, Host bridge and generated/device code were not changed by this
simplification; their source hashes still match the hardware evidence above.
No new NPU run was required for this trace-analysis-only change.

## 2026-10-13 optional production module

The historical bridge results above are the original example baseline.
The current implementation is `src/tilelang/tilexr_tilelang_pipeline.cpp`,
installed as `libtilexr-tilelang.so` with public `tilexr_tilelang.h` and
`TileXRAllGatherGemmPipeline`. Enable `TILEXR_BUILD_TILELANG=ON` together
with `TILEXR_BUILD_COLLECTIVES=ON`; the feature defaults to OFF.
At this 2026-10-13 snapshot, TileLang Ascend and TileScale were pinned recursive
submodules in `3rdparty/` at the revisions listed above. Neither was implicitly
built or linked into the native library; the pinned TileLang compiler
environment was reused. TileScale has since been moved to comparison-only
`reference/tilescale`; the active compiler dependency remains in `3rdparty/`.

The real CANN 9.1 Release build and installation on `blue` passed. Both
in-tree and standalone installed-header/link configurations passed their two
CTest cases (C++14). Disabled-feature target absence and rejection of the
feature without collectives were checked. Six overlap-checker unit tests pass.

Fresh two-card runs used the installed library's default discovery path,
without `--pipeline-lib`. Each exercised serial, serial-tiled and overlap
modes with framework profiling enabled. Exact gathered inputs and FP32 CPU
GEMM comparisons passed for all 90 rank-rounds:

| Case | Rows / N / K / tile rows | Warmup / measured | Overlap pairs rank 0 / 1 |
| --- | --- | --- | --- |
| Large | 1024 / 4096 / 4096 / 128 | 2 / 5 | 73 / 67 |
| Small | 512 / 2048 / 2048 / 128 | 1 / 3 | 12 / 12 |
| Rank 1 delayed 10 ms | 512 / 4096 / 4096 / 128 | 1 / 3 | 16 / 17 |

All serial controls had zero intersecting communication/compute intervals.
Retained numerical summaries, generated sources, CSVs and traces are under
ignored `run/tilelang-module-20261013/final-{large,small,delay}/`.

Large-case measured latency below is the median across the two rank-local
iteration medians, followed by rank minimum and maximum, in microseconds.
The backend is native TileXR AIV peer-memory plus pinned TileLang-generated
Cube GEMM. Each rank transfers 8 MiB remote A and computes 68.72 GFLOP.

| Mode | Total median [min, max] | AllGather median [min, max] | GEMM median [min, max] | Remote GB/s median [min, max] |
| --- | --- | --- | --- | --- |
| Serial | 1614.09 [1545.84, 1682.35] | 509.71 [491.36, 528.06] | 892.18 [891.34, 893.02] | 16.48 [15.89, 17.07] |
| Serial tiled | 2585.27 [2573.42, 2597.13] | 655.38 [604.96, 705.80] | 1185.81 [1175.14, 1196.48] | 12.88 [11.89, 13.87] |
| Overlap | 1397.17 [1386.59, 1407.75] | 819.06 [732.46, 905.66] | 1102.65 [1096.28, 1109.02] | 10.36 [9.26, 11.45] |

These five measured iterations demonstrate overlap for these shapes, not a
general performance guarantee. UDMA and multi-node paths remain unvalidated;
the existing HCCL qualification boundary described above is unchanged.

| Current artifact | SHA256 |
| --- | --- |
| Python runner | `7b426f2b50f726e370679351d0ad6a5508863a07dbd09d7d040b48e3b96d3d10` |
| Production pipeline source | `7110fc6a1b0dd09c9e50aa51012ac6679d7cef3c35509caff8856f800b716a0a` |
| Installed libtilexr-tilelang.so | `ebe87a57df55e1defb4c5df94df6363cd5f0466a2ffc440d75646b83cb896fa1` |
| Installed libtile-comm.so | `474fc17190d19042c0f297b1ae87dd8f72b1da0acb675d3018bdff348b359c1e` |
| Installed libtilexr-collectives.so | `ca4c40ae12079d9b9f56f175dbd1e51ecb40baf11039a1d1c635f76daf175834` |

The external collectives object and reused TileLang compiler retain their
baseline hashes above. The public callback is typed; unsupported rank counts,
missing peer mappings and insufficient collective windows fail before launch.

## 2026-10-13 example-only measurement

The timing-bearing ABI in the preceding snapshot has been replaced:
`TileXRAllGatherGemmTiming` and the timing output parameter are removed.
The native library retains only ready/done dependency events. Host wall-clock
measurement and profiler stage aggregation now live entirely under
`examples/tilelang/`. The example's `total_us` includes runtime event setup
and cleanup, but excludes tensor allocation, output assembly and correctness
checking. `allgather_kernel_us` / `gemm_kernel_us` sum kernel durations;
they are not the earlier event-to-event intervals. Warmup rounds are excluded.

The changed public header/library and in-tree consumer compiled with CANN
9.1 / C++14. In-tree and standalone installed consumers each passed 2/2
CTest cases. Nine Python analyzer/measurement tests passed, including warmup
exclusion. A root installation attempt with only selected test targets built
stopped at an unrelated, unbuilt `data_as_flag` test executable; installing
the TileLang module via `cmake --install build/src/tilelang` succeeded.
This establishes module installation, not a fresh all-target installation.

The large-case rerun used the installed timing-free API and default library
discovery. Each rank again transfers 8 MiB remote A and performs 68.72 GFLOP
using native TileXR AIV peer-memory and the same pinned generated Cube GEMM.
Framework profiling remains enabled. Five measured iteration medians are
aggregated across ranks as median [minimum, maximum], in microseconds:

| Mode | Host total | AllGather kernel | GEMM kernel | Remote GB/s |
| --- | --- | --- | --- | --- |
| Serial | 1723.91 [1707.67, 1740.15] | 486.14 [465.26, 507.02] | 856.64 [854.98, 858.30] | 17.29 [16.54, 18.03] |
| Serial tiled | 2760.46 [2685.21, 2835.70] | 576.09 [554.58, 597.60] | 1068.18 [1065.94, 1070.42] | 14.58 [14.04, 15.13] |
| Overlap | 1602.58 [1552.04, 1653.12] | 755.17 [692.84, 817.50] | 1077.75 [1077.50, 1078.00] | 11.18 [10.26, 12.11] |

Bandwidth uses remote bytes divided by AllGather kernel time. Measurements
are shape-specific; the changed timing boundary prevents direct comparison
with prior tables. The earlier hardware/environment qualification limits
remain unchanged.

| Timing-free artifact | SHA256 |
| --- | --- |
| Python runner | `26c0ef647e13238ccb4544fc417f3511fce778d64a7da68fe58bbddae841a0c6` |
| Pipeline source | `aec6aa473b8ccaebc177f0f4d56b6cd1190c4efd7c379b6a520c9da534a93263` |
| Public header | `43f5724fe37b6ebc14b6ce05acd6137fb20bff80481ddc15ab88817a8cb66013` |
| Installed libtilexr-tilelang.so | `4197710c0c8db49f49ab846bc1134da8856992246602065b7189dff641e651c2` |

Core, collectives, external AIV object and reused compiler hashes match the
preceding snapshot. No generated/device calculation source changed.

The timing-free matrix repeated all three preceding shapes/modes and passed
all 90 rank-round numerical checks. Overlap pairs on rank 0 / rank 1 were
72 / 71 (large), 14 / 14 (small), and 16 / 16 (10 ms delayed rank). All
serial controls had zero overlap. Retained traces, summaries and generated
sources are under ignored
`run/tilelang-module-20261013/timing-example-{large,small,delay}/`.

## 2026-10-14 TileRelay: one device-driven main kernel

The approved [TileRelay spec](specs/2026-10-14-tilerelay-design.md) is implemented
as the optional `TileXRAllGatherGemmRelay` API. Each rank launches one mixed
kernel per generation. Persistent AIV workers publish/pull FP16 peer-memory
rows and generation-tagged readiness; AIC workers execute pinned TileLang
128x256x64 GEMM, local rows first, then remote rows. This is native
device-driven tile scheduling, not TileScale DSL lowering. The original Host
API is retained. Its actual `a17ef9b` library was frozen before rebuilding and
loaded explicitly for all three baseline modes.

Target: the same blue container, physical devices 0/1, 910B3, CANN 9.1.0,
driver 25.5.0, torch/torch_npu 2.9.0, TileLang Ascend 0.1.4 at the pinned
`87e6d1c2f0a9d2ff6a9013d9a1202325c4a619b1` snapshot. The deployed compiler
source copy has no Git metadata: its revision is explicitly attested, and the
actual compiler library is hashed. Firmware was not reported. The retained
one-time eight-device AIV-only HCCL record was not rerun; exact two-device and
mixed-mode qualification remains outside that record's coverage.

### Final functional and launch evidence

All groups use 16 physical Cube worker blocks, FP16 input/output, FP32
accumulation, framework profiling enabled, `TASK_QUEUE_ENABLE=0`, UDMA off,
`TL_CCE_OPT_LEVEL=2`, and `TL_CCE_AUTO_SYNC=on`. The Host library is Release
C++14; pure device ELFs use the pinned generator's C++17 templates with the
same optimization/automatic-sync settings as generated baseline GEMM.
No source/compiler modifications, profiling-disabled baseline or hidden
fallback were used. Each round changes A; gathered rows match exactly and
GEMM matches the CPU FP32 reference rounded to FP16 at rtol/atol 0.01.

| Group | Rows per rank / N / K | Warmup / measured per mode | Correct rank-rounds | Tiled baseline / Relay main launches per rank-round |
| --- | --- | --- | --- | --- |
| qualified-large | 1024 / 4096 / 4096 | 5 / 20 | 200 | 16 / 1 |
| qualified-medium | 512 / 2048 / 2048 | 5 / 20 | 200 | 8 / 1 |
| qualified-delay | 1024 / 4096 / 4096; rank 1 delayed 10 ms | 2 / 3 | 40 | 16 / 1 |
| qualified-diagnostic | 1024 / 4096 / 4096; separate instrumented ELF | 1 / 2 | 24 | 16 / 1 |

All **464 rank-rounds** passed. The trace checker revalidated all eight final
rank traces, including complete round coverage, kernel names and launch counts.
Full serial has two main launches. Framework `aclnn` preparation kernels are
reported separately, not counted as communication/GEMM main launches.
The mixed framework task cannot resolve internal stages. Separate diagnostic
cycles show last-wave producer/local-GEMM intersections on the same group:
rank 0 AIV0/AIV1 = 4001/3912 cycles; rank 1 = 3731/3649 cycles. These intervals
include peer/communication waits, not isolated DMA occupancy. They demonstrate
concurrent producer/compute progress in the instrumented kernel, not exclusive
communication stage time or a profiler-disabled performance result.

The prerequisite mixed handoff test passed one/four blocks, 20 waves and eight
varying generations each (16 device-generations). The final shape-reuse test
alternates 128/4096/4096 and 512/2048/2048 for 12 generations per rank, delaying
alternating ranks by 10 ms. Inputs are prepared beforehand; there are no
per-generation Host barriers or CPU checks. All 24 rank-generations passed
after the sequence, exercising fixed publication/ACK planes and ownership.
Total final distributed numerical checks: **488 rank-generations/rounds**.

### Same-boundary performance comparison

Each rank takes the median of 20 measured rounds; tables show the median
[minimum, maximum] across both rank medians. Host total includes synchronous
native API execution and baseline dependency-event setup/cleanup, excludes
tensor allocation, output assembly and CPU checking for every mode. The
pre-round rank barrier is outside the timer. Remote effective GB/s divides
remote input bytes by total latency, **not isolated AllGather time**. Fused
AllGather/GEMM stage latencies are N/A; no independent stage time is invented.

All stages are native. Baseline communication backend/version is TileXR AIV
peer-memory/external collectives ELF; compute is pinned TileLang Cube GEMM
with the actual full/tiled libraries recorded in each summary. Relay uses
peer-memory plus TileLang Cube within the fixed-plane v1 mixed ELF. Kernel
versions are the artifact hashes below. Each rank transfers 8 MiB remote A
and performs 68.72 GFLOP for large, or 2 MiB and 8.59 GFLOP for medium.

| Shape | Mode | Host total (us), median [min, max] | End-to-end remote GB/s, median [min, max] |
| --- | --- | --- | --- |
| Large | Full serial | 1663.04 [1648.53, 1677.55] | 5.045 [5.001, 5.089] |
| Large | Serial tiled | 2771.80 [2754.92, 2788.69] | 3.027 [3.008, 3.045] |
| Large | Original Host overlap | 1595.94 [1576.15, 1615.72] | 5.257 [5.192, 5.322] |
| Large | TileRelay | 1683.92 [1663.21, 1704.63] | 4.982 [4.921, 5.044] |
| Medium | Full serial | 551.92 [535.05, 568.80] | 3.803 [3.687, 3.920] |
| Medium | Serial tiled | 849.81 [848.97, 850.64] | 2.468 [2.465, 2.470] |
| Medium | Original Host overlap | 475.71 [460.59, 490.83] | 4.413 [4.273, 4.553] |
| Medium | TileRelay | 336.80 [333.44, 340.16] | 6.227 [6.165, 6.289] |

Relay is **1.41x faster** than Host overlap for medium (29.2% lower latency),
but **5.5% slower** for large. Fewer launches therefore do not establish a
universal speedup. Fixed worker gangs, copy/poll costs and the unchanged GEMM
tile schedule are remaining tuning opportunities, not proven causes isolated
by this comparison. Delay and instrumented groups are correctness/diagnostic
stress runs, excluded from the performance conclusion.

### Delivery verification and provenance

Fresh C++14 contract mocks and installed-header/link consumer passed 3/3
CTests; 18 Python measurement/checker tests passed. The final kernel-component
install copied all three tested shape ELFs and JSON files, and installed ELF
bytes matched their qualified originals. Module installation, not a fresh
all-target installation, is the verified boundary. Independent final review
found no concrete blockers. No NPU or communication code outside the optional
TileLang module was changed.

Remote scratch: `/home/d00520898/tilexr-tilerelay-20261014.ChO1rt`, with final
`qualified-{large,medium,delay,diagnostic}` and `shape-reuse` directories.
Retained local artifacts: ignored `run/tilerelay-20261014/`. Each summary
contains rank samples, actual loaded library/kernel paths and hashes. Main
matrix launchers were bounded by 480 seconds with a 15-second kill deadline.
See the [archived example commands](https://github.com/LingquLab/TileXR/blob/7ccaa16768ae2b2cb0da71d47e864352721de8b0/examples/tilelang/README.md#tilerelay-comparison).

| Final artifact | SHA256 |
| --- | --- |
| Comparison runner | `7327cdbce8e9a722681372a66d6318f62bc7f47438f1f346663e41e086308f89` |
| Trace checker | `756c94f80a279fd630239d1ac071ec4b5da578b3fd8a56049bf07d872c8a365d` |
| Relay generator | `011e9c1ab4ae9d79b61db34a912f77bbf42945fcdc437366b44a5fe6368c6149` |
| Relay Host source | `8b4e9bfc670e714d49b21436ccfea5690f935ce59b8ea79010a11238ae7df0b0` |
| Frozen baseline library, a17ef9b | `4197710c0c8db49f49ab846bc1134da8856992246602065b7189dff641e651c2` |
| Installed native library | `b33c3bed35dd7f8c8f1308d69ab5fcfd392cbfe2fdf83ab767d201acd821f030` |
| Large Relay ELF | `6c33e42ac9f36f63127d56d0f88434f43a806add732e545aaa2216dda52f42ad` |
| Medium Relay ELF | `3dd24f0d8ee7f5ae05e5d0df7340b4ff127fc971fa6a243651fac96b6e728700` |
| Separate diagnostic Relay ELF | `d08fb7992825e52d3fb3b514cd533167efbbf6c9494c67043920e35fb016d66e` |

The compiler, comm/collectives libraries and external collectives ELF retain
the hashes in the earlier tables. Final summaries also record actual baseline
GEMM libraries; source hashes were checked against the delivered source.
This validates same-node two-card 910B peer-memory only, not arbitrary shapes,
950/UDMA, multi-node, other worker configurations or concurrent communicator
operations. Reusable synchronization, shape-reuse, metadata and DMA lessons
are retained in [the archived integration guide](https://github.com/LingquLab/TileXR/blob/7ccaa16768ae2b2cb0da71d47e864352721de8b0/docs/TILELANG_INTEGRATION.md#tilerelay-device-driven-alternative).

## 2026-10-14 dependency boundary cleanup

TileScale is not used by native builds, offline Relay generation or execution.
Its gitlink and `.gitmodules` entry are removed; comparison source is retained
under ignored `reference/tilescale`. TileLang Ascend remains the pinned active
compiler submodule. The dependency/example guides no longer require fetching
TileScale. Comparison-only checkouts must not enter dependency initialization
or active include/link paths merely because their algorithms informed a design.

Validation is scoped to the submodule manifest/index, relocated recursive
checkout integrity, ignored reference paths and unchanged Python tests. No
Host/device source or build target changed; no new NPU run is required for this
dependency-metadata cleanup, and earlier hardware hashes/results remain scoped
to their original artifacts.

## TileRelay Core qualification (2026-10-14)

The approved [Core design](specs/2026-10-14-tilerelay-core-design.md) is now
implemented as an optional generic runtime and Python-defined operators. This
record is distinct from the earlier specialized 96-byte Relay qualification.

### Host and packaging

On blue/CANN 9.1, GCC 10.3.1 builds the generic Host library as C++14 with
`-Wall -Wextra -Werror`. Native mocks and installed C++14/C11 consumers passed
3/3. Actual full-repository CMake configure/build/install passed with Relay ON
and collectives/TileLang OFF; configured Host CTests passed 6/6 and standalone
installed consumers passed 3/3. A fresh default configure kept all three
optional features OFF and exposed only the core runtime target.

Opt-in Python installation passed system Python 3.9 `-S` imports without
TileLang/Torch, installed builder `--help`, and verification of seven external
ELF/typed-manifest pairs. The separate kernel component installed no DSO. The
generic DSO has no collectives/Python dependency or RPATH/RUNPATH.

The maintained Host CI passed all seven cases, including native Relay mocks,
legacy Host regressions, Python contracts and trace validation. After adding
explicit prepared bindings, the current Python suite passed 22/22 on blue
(local 21/22 with the actual-header compiler test skipped because this container
has no C++ compiler). Current trace tests passed 19/19; CI behavior tests 56/56.

The first default driver include path was unreadable for this account. Using
the previously qualified driver shim in CMake resolved it without source
changes. Logs are retained under ignored `run/relay-cmake-20261014/`; this is a
build-environment boundary, not a production fallback.

### Device/ABI and Python-only extensibility

The canonical entrypoint retains sixteen pointer slots, int64 epoch, int32 rank
and rank count, with final uint64 FFTS (152-byte Host frame). The registered
argument-echo probe leaves slot 7 null/unused, observes slots after it and all
three scalars, and exercises paired Cube/Vector handoff. No compiler patch,
generated-C++ rewriting or Host binary embedding was used.

The final prepared-bindings path used Python 3.10, torch/torch_npu 2.9.0, pinned
TileLang Ascend 0.1.4, two same-node 910B3 devices 0/1 and CANN 9.1. Its matrix:

| Probe | Cases (rows / N / local K / blocks) | Rounds per rank |
| --- | --- | ---: |
| Canonical ABI echo | All used slots after an unused/null slot | 6 |
| AllGather-GEMM | 128/256/64/4; 256/512/128/2; 384/768/128/2 | 18 |
| GEMM-AllReduce SUM | Same three cases, FP32 partial/exchange/SUM | 18 |
| Alternating both operators and shapes | Six plans, no per-round Host barrier | 24 |

All 132 rank-rounds passed, including changed inputs, delayed ranks, inactive
groups and uneven column waves. Every trace contained one mixed main launch per
generation. Gathered inputs are exact; normal final FP16 GEMMs use FP32 CPU
references with rtol/atol 0.01. A separate exact AllReduce case uses partials
`1 + 2^-11` and `2^-12`, requiring final FP16 `1 + 2^-10`; it rejects premature
FP16 partial rounding that the loose random tolerance could miss.

The same generic runtime was frozen before adding the second Python operator:
`aed647dbc80c4ae37b9cc6c9838dbce229cb624d6e7d82ff67abc04368791e66`.
It remained unchanged through both operators, centralized generation, prepared
bindings and the final matrix. No native operator enum/formula/dispatch was
added for GEMM-AllReduce. The actual CMake-installed runtime is separately
hashed `f973090fd9190473d8b2a3ad06c24022a14f68e9c219a507cace6a225dec682d`
(different build flags/stripping); that installed DSO and installed core also
passed a final prepared-bindings regression of 24 mixed rounds per rank under
the pinned Python 3.10 environment, with one main launch per round and unchanged
runtime hash before/after. Both devices were idle afterward. External binary
names derive from actual ELF SHA256, not operator/shape aliases. The extracted
legacy generator also compiled its optional diagnostic variant
(512/256/64, 4 blocks); this final diagnostic check was compile-only, not a
new diagnostic performance measurement.

Evidence is retained under ignored `run/relay-core-20261014/` and the bounded
blue task output. The existing one-time HCCL environment qualification was
reused, not rerun; its original subset/mode coverage does not independently
qualify this exact two-card mixed path. No UDMA, multi-node, 950 or asynchronous
correctness claim follows from these tests.

### Warmed performance comparison

Five final comparison groups, each with 5 validated warmups + 20 measured
rounds on both ranks, passed all numerical/launch checks (1250 rank-rounds).
All use 16 blocks, framework profiling enabled, identical TileLang compiler settings,
and synchronous Host wall time excluding allocation, plan/binding preparation
and CPU reference checks. No concurrent compilation ran during these groups.
The final large group reverses all five mode blocks. Values below are the
median of rank medians, with `[minimum, maximum]` across those rank medians,
in microseconds; raw 20-sample arrays per rank remain in the retained JSON.

| Per-rank rows / N / K | Original serial AG→GEMM | Serial tiled | Host overlap | Frozen specialized Relay | Generic prepared Relay |
| --- | ---: | ---: | ---: | ---: | ---: |
| 128 / 256 / 64 | 201.95 [196.71,207.19] | 196.06 [188.32,203.80] | 175.91 [169.27,182.56] | 82.26 [79.71,84.81] | 102.56 [99.73,105.39] |
| 256 / 2048 / 2048 | 401.35 [396.78,405.93] | 488.35 [486.47,490.24] | 353.42 [353.12,353.73] | 237.49 [233.00,241.97] | 281.44 [279.92,282.95] |
| 512 / 2048 / 2048 | 545.73 [539.65,551.80] | 936.70 [922.67,950.72] | 501.66 [488.62,514.70] | 332.51 [328.61,336.41] | 376.37 [374.19,378.55] |
| 1024 / 4096 / 4096 | 1717.76 [1710.80,1724.71] | 2813.61 [2797.52,2829.70] | 1570.71 [1549.77,1591.66] | 1652.89 [1631.86,1673.92] | 1655.94 [1637.16,1674.72] |
| Same large, reverse mode order | 1665.27 [1618.94,1711.60] | 2778.53 [2742.35,2814.70] | 1545.43 [1540.66,1550.21] | 1639.77 [1586.38,1693.17] | 1635.03 [1614.36,1655.70] |

The serial control is AllGather **then** GEMM, not computation then
communication; reversing that mathematical operation was not measured as an
AG-GEMM baseline. Frozen baseline DSO hash is
`4197710c0c8db49f49ab846bc1134da8856992246602065b7189dff641e651c2`
(`a17ef9b`), specialized Relay DSO
`b33c3bed35dd7f8c8f1308d69ab5fcfd392cbfe2fdf83ab767d201acd821f030`,
and compiler library
`d061a88a88056560917338bf9cb32ed510239c45f73ec6fdab8fcf9d216019d7`.
Each final group records these same hashes plus the frozen generic runtime,
its actual ELF/manifest, generator, primitives, canonical entrypoint and traces.

Native status is registered external mixed ELF, ABI/protocol v1; computation is
TileLang Ascend Cube GEMM and communication is TileXR AIV peer-memory with
FP16 payload `rows*K*2` bytes per rank. Generic effective end-to-end remote
bandwidth is 0.160 [0.155,0.164], 3.726 [3.706,3.746],
5.572 [5.540,5.604], 5.066 [5.009,5.124] and
5.131 [5.066,5.196] GB/s respectively. These include GEMM and Host overhead;
they are not isolated transport bandwidth. Fused communication/compute stage
latencies are not independently observable in this baseline.

Generic prepared Relay improves over Host overlap on the first three cases by
41.7%, 20.4% and 25.0%, but costs 20–44 us more than specialized Relay.
Large generic/specialized results differ by +0.2% and -0.3% in the two orders:
they are a near tie, not a stable speedup. Both are about 5–6% slower than Host
overlap on this large shape. This increment establishes extensibility, not a
new device lookahead or load/polling optimization.

### Fixed Host overhead investigation

A provisional one-shot Python comparison showed small generic latency around
153 us versus specialized 78 us. A bounded two-rank diagnostic held the same
kernel/inputs and compared legacy, generic one-shot Python and generic raw C
ABI, then reversed their order: specialized 82–91 us, generic Python 153–167
us, generic raw C 92–99 us. Direct SoC/context queries took only about 0.9/1.7
us. Small mixed-kernel medians were roughly 10–12 us for both implementations.
This isolates a substantial repeated Python validation/ctypes packing cost,
not a demonstrated arithmetic slowdown or slow SoC query.

Explicit immutable prepared bindings move that static work outside the hot
loop while preserving dynamic call guards and all native checks. The final
small result is 102.56 us; residual 20–44 us end-to-end differences are not
attributed entirely to one component without another isolated A/B. Do not
remove native validation or claim the generic wrapper is faster than the
specialized path. The one-shot interface remains supported, not the warmed
repeated-execution performance path.

An initial launcher failure used a system Python 3.9 `torchrun` executable
despite an active Python 3.10 venv, failing at the TileLang import before device
setup. The qualified comparisons use `python -m torch.distributed.run` from
the pinned environment. Another preliminary large run overlapped Host CI
compilation; it is excluded from this final performance table. Retain such
provenance boundaries instead of treating warmup/repetition alone as immunity
to environment or scheduling differences.

## 2026-10-16: native prepared execution

The additive `TileXRRelayExecution` / Python `RuntimeExecution` path freezes
validated addresses, communicator mappings, context/device and the registered
stub. It shares parent serialization/poison and preserves fresh FFTS/epoch per
launch. No device kernel, canonical ABI, communication primitive or generated
operator changed. The plan and lifetime contract is recorded in
[the execution plan](plans/2026-10-16-tilerelay-prepared-execution.md).

### Build and regression evidence

On the same blue 910B devices 0/1 and CANN 9.1 environment, a new isolated CMake
Release installation built generic and specialized Host libraries with
`-O2 -DNDEBUG -fPIC`. The earlier performance record did not establish matching
Host optimization flags: an empty CMake build type was unoptimized, even with
the same TileLang compiler. Do not interpret that earlier comparison as a
controlled Host-O2 comparison.

- Six focused CTest cases passed: native mock, C/C++ linkage and legacy pipeline/
  Relay regression; three standalone installed-consumer CTest cases also passed.
- Python contracts/operator-plan tests passed 31/31 on blue, including the native
  header layout comparison; trace tests passed 20/20. Native mocks also passed
  a separate C++14 `-Wall -Wextra -Werror` build.
- The maintained Host CI script passed all seven cases. An initial invocation
  used the NPU venv, which lacks pytest; rerunning the unchanged checks with the
  system Python containing pytest passed. ASan/UBSan was unavailable because the
  server lacks the sanitizer libraries; it is not counted as passing.
- Installed Python imports/new symbols were verified with TileLang/TVM/Torch
  blocked; loading the old native DSO through the new Python runtime also passed.
- Prepared execution passed ABI echo (8), AG-GEMM (24), GEMM-AllReduce (24) and
  mixed-plan (30) rounds per rank. Old prepared bindings passed 12 mixed rounds,
  and the specialized entry passed 6: **208 passing rank-rounds** total, with one
  mixed main launch per round. The numerical checks include exact gathered data,
  increasing ABI epochs and the exact FP32-partial discriminator. Shape matrix
  remains 128/256/64, 256/512/128 and 384/768/128, with delayed ranks and no
  per-generation Host barrier. Real execution handles were destroyed after their
  parent; attempted Python launches after parent destruction were rejected.

An initial specialized probe selected an old DSO directory missing the
256/512/128/B2 ELF, rejecting before kernel launch. The retained matching ELF was
installed beside the new specialized DSO and the full probe passed. No production
code workaround or repeated HCCL qualification was used. Evidence and failed
setup attempts are retained under ignored `run/relay-execution-20261016/` and
blue's isolated `tilexr-relay-execution-20261016.CyrRE1` directory.

Installed DSO SHA256 identities:

| Artifact | SHA256 |
| --- | --- |
| Generic Relay | `6e60a11ab634722a3fc639c3317134a22cca67875fd0adf3af0598d2e39ba071` |
| Specialized Relay | `0e38bde938a40d6acf16a60bfdc2c460a6a2b25034f92bd13a74e6777ce334dc` |
| Core | `6f731165492d56681ce859546353a8efdd578174fc8f6891c7bb993f6573716a` |
| Collectives | `7a5fea857664dd92ffcb0d2a4c53945e25a01f01108fe2e312a1b80e4456081c` |

No runtime RPATH/RUNPATH was present in the installed generic DSO. The same
borrowed-resource restrictions remain: buffers/communicator/context must stay
alive until execution destruction; address checks cannot detect freed-and-reused
objects. These tests do not qualify asynchronous, UDMA, 950 or multi-node use.

### Same-batch prepared-execution performance

All eight groups passed numerical and launch-count checks: four shapes, seeds
41/97, six modes, 5 warmups + 20 measured rounds, two ranks = **2400 passing
rank-rounds**. Each round shuffles mode order identically on both ranks. The
framework profiler and `record_shapes=True` remain enabled; allocations, native
preparation, input copies and CPU references are outside the synchronous Host
timer. No concurrent compilation ran. Both devices were idle after completion.

Old prepared bindings and new execution use the **same Release generic DSO**;
the specialized control is also rebuilt Release. Thus “old generic” below means
the preserved full-validation entry in that DSO, not a previously unoptimized
binary. Original serial/serial-tiled/overlap still use frozen Host baseline
`4197710c…`, with the common new Release core/collectives. Generic and specialized
device ELFs are unchanged retained artifacts. Compiler identity remains
`d061a88a…`; each result records complete DSO/ELF hashes and Host build flags.

Values are microseconds: median of the two rank medians, followed by
`[minimum, maximum]` across those rank medians. Each seed remains separate;
raw 20-sample rank arrays and complete profiler traces are retained.

| Rows / N / K; seed | Original serial AG→GEMM | Serial tiled | Host overlap | Specialized Relay | Old generic bindings | New generic execution |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 128 / 256 / 64; 41 | 197.30 [187.68,206.92] | 199.74 [192.23,207.26] | 182.96 [173.93,191.99] | 83.03 [82.74,83.32] | 98.30 [98.01,98.60] | 73.90 [73.17,74.63] |
| 128 / 256 / 64; 97 | 219.60 [214.16,225.03] | 219.05 [213.38,224.73] | 196.64 [195.79,197.50] | 84.40 [82.32,86.49] | 100.11 [99.73,100.48] | 76.64 [76.36,76.92] |
| 256 / 2048 / 2048; 41 | 441.39 [436.00,446.77] | 571.38 [571.38,571.38] | 405.86 [402.05,409.66] | 244.61 [243.42,245.79] | 287.08 [284.04,290.11] | 225.46 [223.79,227.14] |
| 256 / 2048 / 2048; 97 | 401.41 [401.20,401.62] | 520.88 [514.41,527.36] | 361.31 [359.24,363.39] | 217.00 [216.91,217.08] | 245.31 [244.82,245.79] | 204.53 [203.12,205.93] |
| 512 / 2048 / 2048; 41 | 586.43 [580.01,592.86] | 925.10 [924.40,925.79] | 517.34 [498.26,536.42] | 366.66 [364.60,368.71] | 412.82 [407.73,417.90] | 341.70 [340.64,342.76] |
| 512 / 2048 / 2048; 97 | 566.12 [558.29,573.96] | 874.12 [863.09,885.15] | 502.72 [497.59,507.85] | 350.84 [348.32,353.36] | 375.95 [374.80,377.09] | 323.50 [322.31,324.68] |
| 1024 / 4096 / 4096; 41 | 1769.52 [1726.66,1812.39] | 2911.63 [2862.50,2960.76] | 1578.69 [1575.51,1581.87] | 1652.70 [1644.25,1661.14] | 1861.37 [1849.95,1872.80] | 1649.20 [1637.14,1661.27] |
| 1024 / 4096 / 4096; 97 | 1747.25 [1723.38,1771.13] | 2905.26 [2876.81,2933.71] | 1598.57 [1576.87,1620.27] | 1729.71 [1714.19,1745.22] | 1735.78 [1728.54,1743.01] | 1656.18 [1647.17,1665.19] |

Relative to old generic bindings, new execution reduces end-to-end median
latency by 23.4–24.8%, 16.6–21.5%, 14.0–17.2% and 4.6–11.4% respectively.
Small/medium cases also beat specialized Relay in both seeds. The large case is
near-tied with specialized in seed 41 and faster in seed 97, but remains
3.6–4.5% slower than Host overlap: this is not a universal winning schedule.

These differences are not isolated CPU time. For example, the unchanged large
generic ELF's measured mixed-kernel rank medians range from 1481.61–1625.19 us
on the old entry and 1464.09–1512.19 us on new execution across the two seeds.
Host preparation can change peer arrival/wait behavior; do not equate the full
end-to-end reduction with a fixed per-call CPU cost or claim faster GEMM math.

Both Relay controls use native registered external mixed kernels: specialized
ABI v0 versus generic ABI/protocol v1, TileLang Cube GEMM plus TileXR AIV
peer-memory FP16 traffic. Per-rank remote volumes are 16 KiB, 1 MiB, 2 MiB and
8 MiB. New execution's effective end-to-end remote bandwidth (GB/s, rank median
and `[min,max]`) is respectively 0.22 [0.22,0.22] / 0.21 [0.21,0.21],
4.65 [4.62,4.69] / 5.13 [5.09,5.16], 6.14 [6.12,6.16] /
6.48 [6.46,6.51], and 5.09 [5.05,5.12] / 5.07 [5.04,5.09] for seeds 41/97.
This includes GEMM and Host costs, not isolated communication bandwidth;
communication and computation stage latency remain unseparated.

## 2026-10-16 review and coverage consolidation

The review removed duplicate normal-path mocks, the standalone mixed-handoff
and legacy shape-reuse experiments, and the duplicate C++ consumer source.
Normal coverage now lives in the production E2Es; C/C++ consumer compilation,
invalid-contract and fault-injection checks remain. The legacy registration
cache now allocates its node before registering a stub. A post-registration
allocation-failure test fails on the preceding implementation (`836cb3d`, exit
134) and passes after the fix; this ownership failure is not safely injectable
on an NPU. The generic Plan no longer retains an unused channel-array copy.

Fresh Release validation on the same blue devices 0/1 and CANN 9.1 passed:
Host CI 7/7, native CTests 6/6, standalone installed consumer CTests 3/3,
Python contracts 23/23, and trace checks 19/19. Both native mock suites also
build and pass with `-Wall -Wextra -Werror`.

| E2E probe | Generations per rank | Main launches per rank |
| --- | ---: | ---: |
| ABI, native execution | 8 | 16 |
| Mixed AG-GEMM/GEMM-AllReduce, native execution | 18 | 36 |
| ABI, alternating one-shot/prepared bindings | 8 | 8 |
| Mixed operators, alternating one-shot/prepared bindings | 18 | 18 |
| Specialized Relay, three alternating shapes | 9 | 9 |

Every execution was reused on two streams after releasing its borrowed binding
descriptors. Parent-before-child teardown was checked through both native and
Python APIs. All probes passed numerical and launch-count checks, including the
FP32-partial discriminator. The current pipeline DSO separately passed all
three modes at rows/N/K = 512/2048/2048, tile rows 128, 5 warmups + 20 measured
rounds per mode: 150 correct rank-rounds, with 78/73 valid overlapping task pairs
on ranks 0/1. This is regression evidence, not a new six-mode speedup comparison.

Raw logs, per-rank JSON and profiler traces are retained under blue
`/home/d00520898/tilexr-relay-review-20261016.MSqgbC` and locally under ignored
`run/relay-review-20261016/`. Installed DSO SHA256:

- Generic: `f1ac51b088fb90afd0c180852512227fcaa1eb4feb8151b02a9e0b5f37f1dc0d`
- Specialized/pipeline: `df985de79d1182310636d651d980a46a54402b7466c5d3761cdc783f602bf48c`

Both devices were idle after the matrix. Relay device ELFs are unchanged retained
artifacts; the earlier performance table
remains tied to its original Host hashes. No new transport/platform qualification
is implied by this cleanup.

### Trusted-artifact simplification

Native registration now trusts immutable installed filenames rather than
recomputing SHA256. Its existing mock success path deliberately supplies bytes
unrelated to the naming digest: it fails against `492ee0c` and passes after the
change. The SHA implementation and its dedicated cases are removed. Duplicate
loader-result checks, smaller-range overflow checks, successful-Create handle
checks and an unreachable legacy row-limit check are also removed; public ABI,
memory bounds, context/device and failure-state checks remain.

Fresh blue/CANN 9.1 Release validation passed Host CI 7/7, native CTests 6/6,
installed consumers 3/3, Python 23/23, trace checks 19/19 and both native mock
suites with `-Wall -Wextra -Werror`. Two-card mixed-operator probes ran 12
generations per rank on each generic entry path; legacy ran 6 alternating-shape
generations: 60 checked rank-generations and 84 mixed launches, all passing.
No new performance comparison was made. Evidence is retained under blue
`/home/d00520898/tilexr-relay-trust-20261016.gJfD3k` and ignored local
`run/relay-trust-20261016/`. Installed generic/specialized DSO SHA256:

- `963ef71a415254c1287af473e47a06e1ea2ea156bdb173f3e119e4881f0d6924`
- `bf463a9c81d95eeb6321b769d4bd6ef809be44fbeb90737bc4c62f462b50a8bf`

### Canonical argument adapter simplification

Moving pointer padding and scalar selection into `device/abi.py` removes both
operator-specific sixteen-slot adapter macros without changing the native ABI.
Fresh-cache compilation passed for ABI echo and both operators at rows/N/K/blocks
256/512/128/2, 128/4096/4096/16 and 512/2048/2048/16. Echo and both 256-row
generated sources are byte-identical to retained originals; the two operator
ELF identities also match. Python contracts passed 23/23.

On blue devices 0/1 with CANN 9.1, existing ABI probes ran 8 generations per rank
and mixed-operator probes ran 12, on both prepared-execution and alternating
one-shot/prepared-binding paths: 80 checked rank-generations, 120 mixed launches.
Numerical, FP32-partial, slot/scalar, launch-count and teardown checks passed.
The generic Host DSO is unchanged from the trusted-artifact validation above.
No tests were added and no new performance claim is made. Logs and per-rank
traces are retained under blue
`/home/d00520898/tilexr-relay-abi-20261016.0euRis` and ignored local
`run/relay-abi-20261016/`.

## 2026-10-19: Host pipeline moved to the benchmark

`TileXRAllGatherGemmPipeline` and its callback type are no longer public or
exported by the production library. The unchanged implementation and existing
failure tests moved to `examples/tilelang`, with a separately built, non-installed
`libtilexr-tilelang-benchmark.so`. The example requires an explicit helper path;
comparison results retain actual baseline paths/hashes instead of assuming every
baseline is the historical `a17ef9b` binary.

Fresh blue/CANN 9.1 Release build/install with `TILEXR_BUILD_COLLECTIVES=OFF`,
`TILEXR_BUILD_TILELANG=ON` and `TILEXR_BUILD_RELAY=ON` passed 8/8 CTests;
standalone installed-consumer configuration passed 2/2 and Host CI passed 7/7,
including the relocated benchmark test. ELF inspection confirmed the production
specialized DSO exports Relay but not Pipeline and has no collectives dependency.
The independent helper builds against installed core/collectives; it is absent
from the production install. Both Python runner imports/CLI help passed.

On devices 0/1, rows/N/K = 512/2048/2048 and tile rows 128, all three pipeline
modes passed 5 warmups + 20 measured rounds (150 checked rank-rounds). Hardware
traces showed 76/73 cross-tile overlapping task pairs and no serial overlap.
The comparison runner then passed all four modes with the new helper and newly
built specialized Relay DSO (16 blocks, retained device ELF): another 200 correct
rank-rounds with complete launch-count checks. Profiler/correctness checks stayed
enabled; both devices were idle afterward. This is relocation regression evidence,
not a new speedup claim or broader transport/hardware qualification.

Evidence: blue `/home/d00520898/tilexr-pipeline-benchmark-20261019.JZjYJ8`, locally
ignored `run/pipeline-benchmark-20261019/`. Production specialized/helper SHA256:

- `90cef2911526027397f1531fe5b032614a88251cf8285582fe264942d98da704` (Host `-O2 -DNDEBUG -fPIC`)
- `b381502001e974a8a09f965a0248713b0b82b700ab6f46be3471eb388d030651` (helper Release `-O3 -DNDEBUG`)

## 2026-10-19: Artifact-derived operator plans

Both Python operator planners now derive shape/blocks only from the artifact
specialization and reject the wrong operator. The 25 Python contract tests pass,
including wrong-operator, missing/invalid-specialization and old-argument
rejection. Full Host CI passed 7/7 on blue; the initial local attempt could not
run compiler/pytest cases because those tools were unavailable.
Rebuilding plans from 10 installed manifests (7 AllGather-GEMM, 3 GEMM-AllReduce)
produced identical canonical JSON, including resource layouts and plan identity.
Evidence: blue `/home/d00520898/tilexr-artifact-plan-20261019.431HYg/host-ci.log`;
manifests came from `tilexr-relay-trust-20261016.gJfD3k/install/lib64/tilexr/kernels`.
No ELF, native runtime or launch path changed. No new NPU/performance run was
performed; this validates the planner boundary, not ELF/metadata authenticity.

## 2026-10-19: ABI-v1-only runtime and external historical baseline

The archival remote branch was verified at the full commit above before deleting
v0/pipeline implementations. The current runtime, 152-byte ABI and registration
policy are unchanged. The AllGather-GEMM generator now emits only the canonical
entry; the removed v0 diagnostics and compatibility path are historical only.

On blue with the same CANN 9.1/910B setup, a fresh Release build/install with
collectives OFF and Relay ON passed 10/10 CTests and 3/3 installed C/C++ consumer
tests. Final Host CI passed 7/7 (Python contracts 25/25, trace checks 12/12).
ELF inspection found only v1 Relay exports, no collectives dependency or runtime
RPATH, and no specialized/pipeline libraries or header in the fresh install.
Both operator builders and ABI echo compiled with the pinned TileLang compiler;
the revised comparison and hardware runner CLI imports passed in that environment.

Devices 0/1 were checked idle before each test group. ABI echo ran 8 generations
per rank and mixed AllGather-GEMM/GEMM-AllReduce ran 12, on both alternating
one-shot/prepared-binding and prepared-execution paths: 80 checked rank-generations
and 120 mixed launches. Slot/scalar/epoch, FP32-partial precision, numerical,
launch-count and parent-before-child teardown checks passed. AllGather-GEMM used
rows/N/K/blocks 512/2048/2048/16; GEMM-AllReduce used 256/512/128/2.
Profiler remained enabled. Some parser logs report unavailable ACL-to-NPU flow
edges; checked hardware task events and complete launch counts were present.
No device reset or repeat HCCL qualification was performed.

The external pipeline helper was rebuilt from the exact frozen commit into its
own build directory, using retained installed core/collectives from
`tilexr-relay-trust-20261016.gJfD3k/install`. It is not installed with current
Relay. Actual Host options: current `-O2 -DNDEBUG -fPIC`, baseline
`-O3 -DNDEBUG`. Evidence is retained on blue under
`/home/d00520898/tilexr-v1-only-20261019.fDR6Al`.

The revised comparison passed serial, serial-tiled, overlap, generic bindings
and prepared execution at 512/2048/2048/16 with 5 warmups + 20 measured rounds
per mode, shuffled each round with seed 19: 250 correct rank-rounds. Launch
counts were respectively 2/8/8/1/1 per round; each rank retained 20 measured
samples per mode. Hardware traces showed 90/87 cross-tile overlapping task
pairs and no serial overlap. Profiler/correctness checks remained enabled;
these results qualify the refactored runner, not a new speedup claim.
The first command omitted torchrun's `--` separator and exited during argument
parsing before a workload started; the corrected documented command completed.

Artifact SHA256:

- Current Host DSO: `4a7c5cfd684878c1da02411ddb0e3b58eb7936587e1ae7fc32800d05e480d4ba`.
- Historical pipeline DSO: `b381502001e974a8a09f965a0248713b0b82b700ab6f46be3471eb388d030651`.
- AllGather-GEMM ELF: `97b22855ebb54a359f681c87b7413937980aff353f90818f36f7929d7ebcef3e`.
- GEMM-AllReduce ELF: `9374f7420756677142fb14750f6dadd942e7bc10a707ece6410c162d209f3c05`.
- ABI echo ELF: `086cf6339462eed1b68b396188c9267f56a54f9ae5234210f0a3d5c7ea29c1e2`.

## 2026-10-26: Artifact-bound fixed peer layout

The [approved fixed-layout design](specs/2026-10-21-tilerelay-fixed-peer-layout-design.md)
ties channel and flag bindings to the immutable artifact layout consumed by both
planner and generator. Both operators now reject the reported `Channel(100, 1)`
replacement. Existing Python tests cover direct/JSON mismatches, extra/missing
channels, shifted/swapped flag arguments, missing old metadata, operator-layout
disagreement and canonical round trips: **29/29 pass**. Capacity negatives retain
valid flag bindings so the new layout check cannot mask their intended boundary.
No source-text guards or separate arithmetic tests were added.

On blue, CANN 9.1.0 and driver 25.5.0, fresh Release build/install with Relay ON,
collectives OFF and `-O2 -DNDEBUG -fPIC` passed **10/10 CTests**; final Host CI
passed **7/7**. The pinned TileLang revision
`87e6d1c2f0a9d2ff6a9013d9a1202325c4a619b1` rebuilt both operators and ABI echo.
Their three ELF digests are identical to the 2026-10-19 values immediately above;
the new JSON manifests round-trip with explicit communication layouts. Scheduling,
native code and the 152-byte device ABI are unchanged. Fresh Host DSO SHA256:
`cecb93576af5367cf0939e6f76c9b5d512b255e8fc2ebd488503d9bbc691c0ee`.

Devices 0/1 were checked idle before each test group. Existing profiled ABI echo
ran 8 generations per rank and mixed operators ran 12, each through alternating
one-shot/prepared bindings and through prepared execution on two streams. All
**80 checked rank-generations / 120 launches** passed, including numerical and
FP32-partial precision, slot/scalar/epoch, launch-count and parent-before-child
teardown checks. AG shape rows/N/K/blocks was 512/2048/2048/16; AR was
256/512/128/2. Commands used `tests/relay/hardware.py --probe abi --rounds 8`
or `--probe mixed --rounds 12`, each repeated with `--execution`, under two-rank
torchrun and a 240-second timeout. Profiler remained enabled; these are correctness
regressions, not warmed-up performance measurements or a new speedup claim.
Some profiler parser logs report missing ACL-to-NPU flow edges; numerical checks
and hardware task-event counts passed, but complete Host-to-device flow attribution
is not claimed. Both devices were idle after the test groups.

Evidence is retained under blue
`/home/d00520898/tilexr-peer-layout-20261026.GrdzHi` (build/compile/Host CI logs,
fresh kernels/install, per-rank JSON and traces). Historical installations and
the frozen baseline branch were not modified. No HCCL qualification was repeated;
existing evidence retains its previously documented topology/mode limits. This
does not qualify multi-channel composition, other hardware/transports or metadata
authenticity. Old communicating manifests must regenerate; no-communication
manifests remain accepted without a layout. Independent review found no remaining
concrete defect in the final diff.

## 2026-10-26: Benchmark relocation and main integration

The seven files formerly in `examples/tilelang` now live in `tools/relay`;
benchmark and checker behavior is unchanged. Current CLI/documentation/CI paths
were updated, while commands inside the frozen historical checkout retain its
original `examples/tilelang` path. Upstream `examples/profiling` remains intact.
Main `67fd80f5` was integrated without rewriting branch history; both Relay and
profiling Host CI suites are retained (nine cases). A stale shell-source assertion
from main was replaced by a behavior test that rejects invalid syntax in the
second tracked script, instead of requiring a particular loop implementation.

Validation: 57 CI control tests, 12 offline trace tests, 29 Relay Python contracts,
and **9/9 Host CI cases** passed. Profiling package-build checks were skipped by
the existing runner because `build/setuptools/wheel` were not all available;
this is not wheel/sdist qualification. CANN 9.1 successfully built the Relay
library and passed **3/3 Relay CTests**, including C/C++ linked consumers.
Both relocated CLI `--help` commands imported successfully in the pinned
TileLang/torch_npu environment. Independent relocation/conflict review found no
remaining defect. This directory/CI change did not rerun NPU workloads or timing.

The full top-level CANN build did **not** pass: upstream
`profiling_sdk_roundtrip` fails to link `aclrtMalloc`/`aclrtMemcpy` when built
alongside the real CANN headers. The same target failed identically in a separate
unmodified `67fd80f5` checkout, so it was not repaired in this relocation change.
Standalone CANN-free profiling tests passed through Host CI; they do not cover
that top-level include/link configuration. Logs and the clean-main reproduction
are retained on blue under
`/home/d00520898/tilexr-relay-tools-20261026.ZpLwhO`.

## 2026-10-26: GEMM K-batch tuning

This optimization changes only the two Python operator generators, not ABI-v1,
Plan/channel resources, registered launch, peer flags, epochs or FP32 partials.
The previous GEMM loaded K=64 into L1 and drained `gemm_v0` around each outer
iteration. Larger L1 K batches amortize those copies/barriers and let the pinned
helper reuse its internal L0 ping-pong across more K work. Explicit
`kL0Size=64` also avoids the old default 128 splitting N=256 into two L0B tiles.
These are two interacting changes; the combined gain is not attributable to
just one of them. This is not cross-wave communication double buffering.

### Measurement and provenance

The unchanged baseline is `64feb3cb`; all variants use the same Host/core DSOs
and the pinned TileLang revision `87e6d1c2f0a9d2ff6a9013d9a1202325c4a619b1`
(0.1.4), CANN 9.1.0, `-O2`, Ascend910B3 devices 0/1 on blue over HCCS,
driver 25.5.0, torch 2.9.0+cpu and torch_npu 2.9.0. Pure AICore ELFs are built
and registered separately; no reference checkout participates in production.
The official comparison uses installed ACLNN AllGatherMatmulV2 AIV mode and
MatmulAllReduce v1, not a newly built `ops-transformer` source checkout.
Host DSO SHA256 is `fb28eaba08ed465adfcd32a12432bd10d77b8af38d14153d4b1c5e6a8fa075a7`;
core DSO is `9f0ae2036b5979d3b93ab4c18a67a68d4a025a6cc6960a7d16924a2838c10fbe`.
The local and deployed generator source digests agree:
AG `b97332851b01e4e517697b4378f46d13fc93c26277bf98cabf94fc225a6e3f8b`,
AR `09ec77f82ed415f60f0121c83e2e4f46de05e28860da6c4911873c1335a66100`.

Each full comparison uses 10 warmups + 30 measured calls per mode, repeated
with seeds 19 and 37. Mode order is shuffled identically on both ranks in every
round. Old/new/official share input tensors, while Tile executions have separate
plans/workspaces; outputs are poisoned before each call. Every round checks
against a CPU FP32 reference (FP16 output, rtol/atol 0.01), and AG gathered data
is checked exactly. Profiler stays enabled for every mode. A Gloo barrier precedes
each timed invocation, and Host time includes launch and stream completion.
Official executor/workspace-query preparation is measured separately, outside
that timer; Tile prepared-execution setup is also excluded. These are warmed
prepared-launch timings, not full application throughput or concurrent queues.

For each iteration, take the slower rank, then report the median of the 60
measured samples. Retained summaries also contain min/max and per-rank statistics.
Device time is the task span; official AR includes overlapping AICore/AICPU tasks.
The profiler's CPU and device clock domains must not be directly compared:
associate kernels by mode order and device chronology, checking exact counts and
nonoverlap between calls, rather than trusting drifted Step IDs or CPU timestamp
windows. All artifacts, runner hashes and rank samples are retained with traces.

Changing only `kL0Size` while retaining outer K=64 was insufficient: a 10+20
screen measured large AG 1639.84 vs old 1582.80 us and AR 1363.82 vs 1356.56 us.
It was not accepted as a standalone optimization. K=256 and K=512 L1 caps were
then compared with the old version and official operators in the same process.

The AR precision probe still returns 1.0009765625 for Tile versus 1.0 for the
installed official operator. The precision paths therefore differ; performance
numbers are not evidence that the two implementations have identical rounding.

### K=256 results

The following full two-seed comparison measures Host launch-to-completion in us.
Rows are per rank. Small uses one worker block; the other shapes use 16.
The percent column is latency reduction, not throughput increase.

| Operator | Rows / N / K | Previous | K=256 cap | Reduction | Installed official |
| --- | --- | ---: | ---: | ---: | ---: |
| AllGather-GEMM | 128 / 256 / 256 | 81.24 | 77.94 | 4.1% | 148.68 |
| AllGather-GEMM | 256 / 2048 / 2048 | 212.61 | 185.43 | 12.8% | 231.27 |
| AllGather-GEMM | 512 / 2048 / 2048 | 341.65 | 296.06 | 13.3% | 327.27 |
| AllGather-GEMM | 1024 / 4096 / 4096 | 1597.07 | 1202.71 | 24.7% | 716.63 |
| GEMM-AllReduce | 128 / 256 / 256 | 83.93 | 81.33 | 3.1% | 154.64 |
| GEMM-AllReduce | 256 / 2048 / 2048 | 218.28 | 208.86 | 4.3% | 232.82 |
| GEMM-AllReduce | 512 / 2048 / 2048 | 365.47 | 344.31 | 5.8% | 314.21 |
| GEMM-AllReduce | 1024 / 4096 / 4096 | 1334.53 | 1161.79 | 12.9% | 737.15 |

Large AG device median decreases from 1491.95 to 1095.89 us; large AR from
1244.07 to 1073.01 us. Their official device medians are 587.52 and 626.00 us.
Thus the improvement is visible on device, but large shapes still trail official
kernels. Host ordering does not necessarily imply the same device ordering:
for AR rows=256, Tile's Host time is lower but device time is 146.60 vs official
140.78 us. Per-run medians vary (for example optimized AG rows=512 is 321.14
and 284.44 us), so small differences should not be treated as universal wins.

### Selected cap and K=512 comparison

Keep `block_k = gcd(K, 256)` in both operators. It consumes at most 192 KiB
of L1 versus 384 KiB for the 512 cap. In the separate four-mode, two-seed run,
K=512 improves large-shape device time by only 1.3% (AG) / 2.1% (AR), while
the intermediate shapes are slightly slower. This does not justify doubling L1
usage or introducing a per-shape tuning table. Device task-span medians in us:

| Operator | Rows / N / K | K=256 cap | K=512 cap |
| --- | --- | ---: | ---: |
| AllGather-GEMM | 256 / 2048 / 2048 | 98.95 | 100.66 |
| AllGather-GEMM | 512 / 2048 / 2048 | 193.08 | 195.47 |
| AllGather-GEMM | 1024 / 4096 / 4096 | 1065.58 | 1052.10 |
| GEMM-AllReduce | 256 / 2048 / 2048 | 140.08 | 141.46 |
| GEMM-AllReduce | 512 / 2048 / 2048 | 270.64 | 271.17 |
| GEMM-AllReduce | 1024 / 4096 / 4096 | 1078.49 | 1056.20 |

These are separately collected paired samples, not interchangeable with the
three-mode table above. No K=512 candidate or new tuning/configuration interface
is added to production. Neither candidate changes communication scheduling:
AG still has fewer communication groups as N grows, and AR still waits for the
current reduction/ACK before the next wave. Cross-tile buffering/prefetch remains
a separate protocol/lifetime design, not a claimed result of this optimization.

### Final regression and limits

All 18 selected-cap operator/shape artifacts compiled on the pinned toolchain.
In addition to the four performance shapes above, both operators cover
rows/N/K/blocks = 128/256/64/1, 256/512/128/2, 384/768/192/2,
128/256/384/1 and 128/256/32768/1. This exercises small K, GCD fallbacks,
incomplete worker waves and the supported K upper bound.

The existing mixed hardware probe ran all 18 plans twice: 36 generations per
rank, two prepared launches per generation on alternating streams, with delayed
rank arrivals and no per-generation Host barrier. Both rank JSONs report
correctness and exactly 72 mixed kernel launches: **72 checked rank-generations
/ 144 launches** total. FP32-partial precision, exact AG gathering, random
numerics, workspace/epoch reuse and parent-before-child teardown passed.
Profiler remained enabled. The task-local runner changes only communicator
construction/destruction to call the core directly because the optional
collectives DSO is absent; the existing E2E assertions are unchanged. It is not
a new production test or qualification of the collectives wrapper.

Command: two-rank torchrun under a 360-second timeout, existing hardware probe
`--probe mixed --rounds 36 --execution`, using `k256/tilexr/kernels` and
`k256/libtilexr-relay.so`. Existing Python contracts passed **29/29**; no new
arithmetic unit test or source-text guard was added. Native code is unchanged,
so this tuning does not rerun the full top-level CANN build or resolve the
separately reproduced baseline profiling link failure documented above.
No official HCCL qualification was repeated. Results cover these two 910B3 cards,
not other devices, transports, unmeasured shapes or application throughput.

Raw evidence is retained on blue under
`/home/d00520898/tilexr-gemm-tuning-20261026.1C0Jc0` (compile logs, generated
sources, ELFs/manifests, rank JSON and profiler traces), with local downloaded
results under ignored `run/relay-gemm-tuning-20261026`. Before compilation,
finish transferring the source snapshot and verify generated templates/ELF
identity: compiling while an overlay is still arriving can silently benchmark
the previous generator. Two early stale K64 AG builds were detected this way
and rebuilt; only the verified rebuilds were used in the screen above.

## 2026-10-26: Two-slot lookahead

The approved extension changes only `device/peer.py` and the two Python
generators. Two alternating local notification slots replace the immediate
same-wave producer wait. The producer acquires the release for wave `w-2`
before wave `w`, and drains its actual final one or two releases before exit.
Peer payload/readiness/ACK slots remain per tile; full staging, FP32 partials,
`Channel.max_credits=1`, ABI-v1 and the synchronous native runtime are unchanged.
There is no runtime switch, extra kernel, GM ring or per-shape tuning table.

### Provenance

The paired baseline is K-batched commit `6154b449`, not the earlier K=64 or
historical Host pipeline. Hardware, CANN, pinned TileLang, framework, compile
flags and Host/core DSO hashes are unchanged from the preceding section.
Local and deployed source SHA256 agree:

- `device/peer.py`: `76f490750a4a01c322c9f214788c87791fbdfc6c4c390f9d07630ae1ec896b6c`.
- AG generator: `67c1c937d1bb1fecdd90f5bfccf503e618b31b83b37c439acc47a22f7317fded`.
- AR generator: `72ff1f05f8ec1fbabcabbc1f421a51417fc912907f1cd8f5a5fb3f7a44dc7e06`.

Large production ELF identities are AG
`d68c89090928c60010990e853129283a1129aaa284f1a39bc148124b861fef23` and AR
`e4cd0a113dda5ec76e4f2f9afacd006c490898bc1bc3b0ddaa3fb7cc88415a6e`.
The separate diagnostic identities are AG
`6f88e15265799062524030baf95132142f6e8061f03f55c5eb428947d24908e2` and AR
`20eba9ee2cd7393cfb9dfd8e4eb5b968201bb8696703a511ca1b272cea177b9f`.
Their manifests retain generated-source/compiler/binary hashes and explicitly
mark diagnostic measurement mode; diagnostic provenance is not baseline timing.

### Correctness and internal overlap

All 22 production artifacts compiled with the preceding pinned toolchain and
unchanged Host/core DSOs. The 18-artifact K-batch matrix is extended with both
operators at rows/N/K/blocks = 128/256/64/2 (zero/one-wave workers) and
640/256/256/2 (three/two-wave tails). The existing mixed hardware probe ran
`--rounds 44 --execution`: each plan twice, two launches per generation on
alternating streams, delayed ranks, no per-generation Host barrier. Both ranks
passed 44 generations and exactly 88 mixed launches: **88 checked
rank-generations / 176 launches**. Exact gathered data, random GEMM references,
FP32-partial precision, shape/epoch/workspace reuse and parent-before-child
teardown passed. The direct-core task-local communicator adapter is the same
as in K-batch validation. Existing Python contracts passed **29/29**; no new
scheduler unit test or source-text guard was added.

Separate diagnostic ELFs sampled device system cycles around producer/consumer
stages at 1024/4096/4096/16, using the unused ABI slot 15 for an explicit extra
diagnostic binding. Production artifacts have no such binding or instrumentation.
Each core/AIC-or-AIV role owns disjoint 64-byte records and its own counter;
two launches each record eight waves, without cross-launch interval pairing.
Both operators/ranks passed numerical checks, 16-record counts and
`producer[w+2].start >= consumer[w].end` within each launch. AG accepts overlap
only from core 0, the actual peer-copy leader at this shape: other columns'
readiness waits are not communication evidence. All 28 eligible leader/half
pairs per AG rank-generation and all 448 AR pairs had positive cross-wave
intersection, across two generations per operator/rank. This establishes
bounded adjacent-wave **stage lifetime** overlap, not isolated DMA occupancy
or sustained Cube utilization: intervals include peer waits, barriers and
sampling overhead. Diagnostic timings are excluded from performance comparisons.

Two task-local diagnostic issues were resolved separately from production:
raw instrumentation had to live inside explicit C/V scopes to satisfy the
compiler verifier; a Python loop variable shadowed the communication DSO handle
during cleanup. The corrected `diagnostic-final` run exited 0, including cleanup.

### Paired performance and remaining bottlenecks

All 16 comparisons completed: four shapes, two operators and seeds 19/37,
each with 10 warmups + 30 measured calls per mode. Baseline/new/installed official
run interleaved in one process with the same inputs, poisoned outputs, correctness
checks, framework profiler, Gloo pre-call barrier and prepared-launch timing
boundary described in the K-batch section. No diagnostic kernel participates.
Official operators are installed ACLNN AllGatherMatmulV2 AIV and MatmulAllReduce
v1, not a rebuilt `ops-transformer` checkout. AR's precision probe remains
1.0009765625 for Tile versus 1.0 for official; their rounding paths differ.

Host launch-to-completion medians in us, taking the slower rank each round and
then the median of 60 samples. Rows are per rank; the smallest shape has one
worker, all others 16. Positive reduction means lower latency.

| Operator | Rows / N / K | K-batch baseline | Two-slot | Reduction | Installed official |
| --- | --- | ---: | ---: | ---: | ---: |
| AllGather-GEMM | 128 / 256 / 256 | 84.90 | 85.34 | -0.5% | 152.43 |
| AllGather-GEMM | 256 / 2048 / 2048 | 180.87 | 177.32 | 2.0% | 223.87 |
| AllGather-GEMM | 512 / 2048 / 2048 | 276.53 | 275.53 | 0.4% | 303.34 |
| AllGather-GEMM | 1024 / 4096 / 4096 | 1188.21 | 1193.71 | -0.5% | 712.64 |
| GEMM-AllReduce | 128 / 256 / 256 | 78.03 | 78.00 | 0.0% | 155.75 |
| GEMM-AllReduce | 256 / 2048 / 2048 | 206.37 | 206.60 | -0.1% | 232.10 |
| GEMM-AllReduce | 512 / 2048 / 2048 | 337.27 | 318.07 | 5.7% | 298.28 |
| GEMM-AllReduce | 1024 / 4096 / 4096 | 1160.17 | 1005.26 | 13.4% | 727.23 |

Device task-span medians in us, using the same slower-rank aggregation. Official
AR spans its overlapping AICore/AICPU tasks. Kernel count/order checks passed
for both ranks of every run; no cross-clock CPU/device timestamp subtraction
or profiler Step-ID association is used.

| Operator | Rows / N / K | K-batch baseline | Two-slot | Installed official |
| --- | --- | ---: | ---: | ---: |
| AllGather-GEMM | 128 / 256 / 256 | 28.12 | 30.16 | 74.09 |
| AllGather-GEMM | 256 / 2048 / 2048 | 105.40 | 105.05 | 130.49 |
| AllGather-GEMM | 512 / 2048 / 2048 | 195.12 | 195.58 | 204.45 |
| AllGather-GEMM | 1024 / 4096 / 4096 | 1084.87 | 1094.41 | 605.27 |
| GEMM-AllReduce | 128 / 256 / 256 | 17.97 | 18.29 | 77.49 |
| GEMM-AllReduce | 256 / 2048 / 2048 | 144.95 | 146.20 | 153.87 |
| GEMM-AllReduce | 512 / 2048 / 2048 | 265.17 | 249.32 | 198.64 |
| GEMM-AllReduce | 1024 / 4096 / 4096 | 1070.98 | 913.21 | 601.35 |

AR's multi-wave improvement repeats in both runs: large Host time is
1160.62 -> 1007.93 us and 1159.09 -> 998.72 us; device time is
1067.55 -> 913.74 us and 1071.96 -> 913.05 us. Its previous AIC waited for
each reduction/peer ACK before beginning another GEMM; two slots remove that
same-wave dependency while preserving the completion bound. The large device
reduction is 14.7%, but it remains about 1.52x the installed official device
time. This does not close the fused-operator latency gap.

AG has **no reproducible latency gain** here despite measured internal stage
overlap. Large device time improves 1114.94 -> 1102.56 us in seed 19, but
changes 1074.90 -> 1077.04 us in seed 37. The combined median is slightly worse.
The implementation already computes local rows while its current peer copy
runs; allowing the next copy earlier does not accelerate the unchanged GEMM.
At N=4096/blocks=16 there is still only one communicating column leader for
the 16 compute workers. This schedule neither adds communication groups nor
prefetches L1 GEMM operands, so those costs remain. Stage intersection is not
proof that communication was on the critical path.

Small/one-wave shapes have no adjacent tile to overlap and are not claimed as
wins. Their Host medians are essentially unchanged; the pooled device medians
are somewhat higher (AG +2.04 us, AR +0.32 us). Examining matched rounds instead
of subtracting pooled medians gives median new-minus-old device differences
of +0.16/-0.38 us for AG and +0.17/+0.43 us for AR across the two seeds; both
operators' paired differences include negative and positive quartiles.
Generated single-wave sources retain the same data copies/GEMM work, with
changed notification IDs/drain placement. These noisy samples do not establish
a uniform small-shape win or strict non-regression; keep the observed costs
visible rather than adding a shape-specific fallback. The demonstrated benefit
of this increment is AR multi-wave throughput and bounded in-kernel scheduling,
not a blanket speedup for both operators.

Raw rank samples, per-rank and slower-rank min/median/max, preparation timing,
artifact/runner hashes and profiler traces are retained in `measurements/` on
blue and locally; local `summary.json` contains both runs and all metrics.
Comparisons should use this paired baseline, not subtract separately collected
medians from the preceding section. Post-run devices 0/1 were idle.

### Lowering lesson and validation boundary

Inspect generated tail-drain arithmetic before launching inactive workers.
An initial count expression using floor division was simplified to C++
`(0-cid)/2`; for `cid=1`, truncation toward zero could invent an unmatched wait.
That compile-only candidate was rejected before NPU execution. The final code
selects between launch-wide `waves` and `waves-1` using the last-wave validity
predicate, avoiding negative division entirely. Generated zero/one and three/two
counts, notification IDs 0–3 and local FIX/L0C dependencies were inspected;
hardware E2E covers both boundary shapes. Python arithmetic alone would not
prove the lowered synchronization sequence.

Evidence is retained on blue under
`/home/d00520898/tilexr-lookahead-20261026.bj77hA`: `qualified/` contains final
production artifacts, `diagnostic/` separate instrumented artifacts,
`qualification/full` and `qualification/diagnostic-final` the passing runs.
`candidate/` is an unused compile-only predecessor, not qualification evidence.
Local downloads live under ignored `run/relay-lookahead-20261026`; one-off
instrumentation/orchestration is not shipped as a production API or test suite.
No official HCCL qualification was repeated. These checks cover blue's two
910B3 cards, not UDMA, other devices/rank counts or simultaneous communicator
operations. Native code is unchanged; the previously established top-level
profiling link failure was not rerun or repaired by this Python-only change.

## 2026-10-26 fine-grained profiling

This is diagnostic evidence for the
[proposed GEMM/push extension](specs/2026-10-27-tilerelay-pipelined-gemm-push-design.md),
not qualification of that implementation. Production remained at
`3994dd34dc822a59d42c9834d24143ff7073ec60`. The environment was blue 910B3
devices 0/1, CANN 9.1.0, driver 25.5.0, with the same pinned compiler and native
libraries as the preceding section. Official kernels came from the installed
opapi package, not a rebuild of the latest comparison checkout.

For rows/N/K = 1024/4096/4096, separate PipeUtilization, Memory and MemoryL0
runs used 10 warmups and 30 measured iterations per mode. Paired core-count,
chunk-size and payload-layout experiments each used seeds 19/37. Across the
24 rank reports there are 2160 effective mode/rank samples, with numerical
checks on every iteration. Instrumented phase runs were separate artifacts;
the retained final AIC, fenced-AIV and split-read runs all passed numerical
checks and completed normally. Instrumented distributions are not unperturbed
production time decompositions.

- AG steady-state AIC cycles were approximately 65% copy-plus-barrier and 33%
  gemm-helper-plus-barrier; gathered-ready wait was below 0.1%. The actual
  gemm_v0 helper drains its internal events at each K-batch return. L0 ping-pong
  alone does not establish inter-batch L1 prefetch.
- AR AIC local-credit wait was about 26% in the AIC-phase run. In a further
  diagnostic with separate completion fences for owner/peer reads, remote
  MTE2 reads accounted for 59–64% of AIV cycles and local reads about 12%.
  This localizes the expensive reads under the instrumented schedule, without
  proving which link/cache/controller mechanism dominates or that push wins.
- Increasing AR workers from 16 to 20 did not accelerate either paired run;
  increasing its reduction chunk from 8 to 32 rows increased device medians
  by 9.86%/8.90%; changing partial storage to tile-packed layout had no stable
  gain (-0.13%/+1.56%). Do not turn these hypotheses into production changes
  without fresh evidence. Layout changes affect both FIX stores and local/peer
  reads, not only the remote transfer.

### Measurement lessons

GetSystemCycle samples the scalar pipeline. An MTE3-to-MTE2 dependency does
not by itself make an immediately following scalar sample a store-completion
boundary. The initial AIV store timing was discarded; the retained fenced
variant explicitly completes the store before sampling. Such extra fences
perturb overlap, so report their presence and keep baseline timing separate.

Direct-registered Tile kernels lacked BlockNum and AIV PMU metadata in this
profiler path. Zero fields do not prove zero work. Installed metric formulas
confirm that MAC/MTE ratios do not depend on block_num; do not use incomplete
core-normalized time/bandwidth fields to reconstruct absolute phase timings.
Pipeline ratios can overlap and must not be added as elapsed-time shares.

Official AR exposes AICore, MC2 AICPU, SDMA and notification tasks. Its
op_summary AICore duration alone is not the complete fused device span.
The precision probe still produced Tile 1.0009765625 versus official 1.0,
consistent with the different FP32/FP16 partial-rounding paths; the Tile
FP32 contract must not be relaxed to claim an equivalent speedup.

Task-local source, raw reports, traces, artifact hashes and retained ELFs live
under ignored `run/relay-deep-profile-20261026/`; `summary.json` is at that
directory's root. Remote records are in the preceding task root's
`deep-profile/`, with separate `phase/`, `vectorfenced/`, `splitread/`,
`core20/`, `chunk32/` and `packed/` artifact directories. Independent review
recomputed the summary tables and split-read distributions. No production
optimization or new HCCL environment qualification was performed by this
investigation; no one-off diagnostic scripts are promoted into the project.

## 2026-10-27 GEMM pipeline and FP32 push acceptance experiment

This experiment implements the approved shared L1/L0 GEMM pipeline and FP32
AR push separately and together. Shared GEMM improves performance; **push failed
the incremental-performance gate and is not delivered**. The user approved
shipping shared GEMM with the original pull, Plan and peer layout. Push-only
and combined measurements below remain rejected-experiment evidence. Final
GEMM-only full-matrix qualification passed on both cards, as detailed below.

Environment and baseline are unchanged from the preceding investigation:
blue 910B3 devices 0/1, CANN 9.1.0, driver 25.5.0, baseline `3994dd34`, and
compiler `87e6d1c2f0a9d2ff6a9013d9a1202325c4a619b1`. Native runtime/ABI and
installed official kernels are unchanged. The existing HCCL qualification was
reused. No UDMA, A5 or additional ranks are covered.

### Correctness and method

The combined candidate passed both ranks for 24 specializations (12 per
operator), 48 mixed rounds with prepared execution (96 launches/rank), and a
separate 48-round one-shot/prepared-bindings sequence (48 launches/rank).
Coverage includes K=64/128/192/256/384/32768, inactive workers, odd tails,
two streams, delayed ranks, repeated epochs, parent-before-child destruction,
NaN output coverage and the strict FP32-partial probe. There is no new
per-generation Host barrier in this lifecycle qualification. Shared-GEMM-only
small/large smoke runs and all push-ablation numerical checks also passed;
these intermediate checks are separate from final qualification.

After removing push, the final shared-GEMM/original-pull artifacts passed the
same full 24-specialization, 48-round sequences again: 96 execution launches
per rank and 48 one-shot/prepared-bindings launches per rank. Both ranks passed
all numerical and lifecycle checks, including the FP32-partial probe, with the
framework profiler enabled and no per-generation Host barrier. Metadata asserts
24 matched plan identities and the exact mixed-kernel counts. Final local
GEMM, AG/AR generators, planner and builder source SHA256 values match the
GEMM-only build source used for the formal performance cohort, so that cohort
measures the final implementation rather than the rejected combined candidate.

Final `python3 -m unittest discover -s tests/relay -p 'test_*.py'` passed 29
tests; the profiler-analysis suite passed 12 tests; the standalone C++14
`tests/relay` Host-mock CMake build/ctest passed 1/1 on blue. No new redundant
unit tests or production algorithm switches were retained. Independent final
review found no actionable defects. Native libraries, Plan, builder and peer
primitives are unchanged; this is not a new top-level native build claim.

Hardware runs reuse the task-local copy of `tests/relay/hardware.py` with only
its communicator bootstrap adapted to the core library via ctypes. Arithmetic,
launch sequence and assertions are unchanged. The initial direct invocation
of the maintained runner lacked `TILEXR_INSTALL_PREFIX` and stopped before
communicator/kernel launch; the successful runs use the existing core adapter,
not the collectives-package bootstrap. This boundary is not hidden as a passing
unmodified runner. Logs are `final-gemm-core-{execution,bindings}.log`, metadata
and traces `qualification/final-gemm-{execution,bindings}`. Post-run devices
0/1 were idle. Final runtime SHA256 is
`fb28eaba08ed465adfcd32a12432bd10d77b8af38d14153d4b1c5e6a8fa075a7`.

Formal performance uses 10 warmups + 30 measured iterations, randomized
variant order with seeds 19 and 37, correctness on every iteration, the same
framework Level0 profiler and per-call Host barrier, and no kernel diagnostic
instrumentation or simultaneous compilation. Statistics first take the maximum
over the two ranks for each iteration, then the median/min/max over iterations.
Host time covers launch through synchronization, excluding ACLNN preparation;
device time covers the whole Tile mixed kernel or the full official fused span,
including AR's MC2 AICPU, SDMA control and notifications, not just its AICore.
Raw per-rank arrays, minimum/maximum, hashes and precision probes are retained.

Tile retains FP32 partial SUM followed by one FP16 conversion. Its probe yields
1.0009765625; the installed official AR yields 1.0. Performance comparisons do
not establish equivalent intermediate precision.

### AG performance

Cells are microseconds, seed 19 / seed 37. All rows use blocks=16 except
128/256/256, which uses blocks=1. Historical serial values are **Host** latency
from a different, 20-sample median-of-rank-medians cohort, not remeasured here;
do not compute paired speedups against them. That historical AG baseline is
AllGather then GEMM. There is no matching historical small K=256 result.

| rows/N/K | Historical serial Host | Baseline Host | GEMM Host | Official Host | Baseline device | GEMM device | Official device |
|---|---:|---:|---:|---:|---:|---:|---:|
| 128/256/256 | — | 79.36 / 72.94 | 78.68 / 72.50 | 147.08 / 140.91 | 16.25 / 13.07 | 16.10 / 13.06 | 63.62 / 60.50 |
| 256/2048/2048 | 401.35 | 165.40 / 174.21 | 152.01 / 162.60 | 209.79 / 217.10 | 99.71 / 99.68 | 86.99 / 88.38 | 120.01 / 122.89 |
| 512/2048/2048 | 545.73 | 264.94 / 271.04 | 231.74 / 237.47 | 291.15 / 293.54 | 184.99 / 186.75 | 153.71 / 154.00 | 195.07 / 192.12 |
| 1024/4096/4096 | 1717.76 | 1199.87 / 1507.72 | 927.81 / 1129.16 | 761.90 / 723.27 | 1080.09 / 1400.16 | 806.26 / 1020.40 | 649.55 / 583.19 |

Large AG device time improves by 25.4% / 27.1% within the paired seeds. The
absolute seed-to-seed variation remains visible; neither seed is discarded.
AG still trails the installed official fusion at the large shape.

### AR performance and rejected push hypotheses

Same units and paired-seed convention. `GEMM` retains the original FP32 pull;
`push only` retains the baseline GEMM; `combined` changes both. No historical
serial AR measurement is available (historical serial column: —).

| rows/N/K | Baseline device | GEMM device | Push-only device | Combined device | Official device |
|---|---:|---:|---:|---:|---:|
| 128/256/256 | 21.48 / 21.06 | 21.04 / 20.83 | 25.31 / 22.76 | 23.56 / 23.05 | 93.59 / 92.09 |
| 256/2048/2048 | 147.71 / 143.34 | 135.50 / 134.79 | 155.18 / 156.97 | 155.43 / 144.43 | 161.37 / 170.74 |
| 512/2048/2048 | 255.63 / 255.44 | 253.52 / 243.16 | 290.11 / 277.84 | 270.40 / 268.99 | 231.06 / 231.01 |
| 1024/4096/4096 | 904.27 / 970.08 | 880.29 / 927.20 | 1003.26 / 1056.78 | 963.55 / 1030.50 | 620.65 / 702.88 |

| rows/N/K | Baseline Host | GEMM Host | Push-only Host | Combined Host | Official Host |
|---|---:|---:|---:|---:|---:|
| 128/256/256 | 82.17 / 80.12 | 79.27 / 81.82 | 82.74 / 83.87 | 83.01 / 83.64 | 159.67 / 164.83 |
| 256/2048/2048 | 211.28 / 210.69 | 201.73 / 200.75 | 222.31 / 224.46 | 215.71 / 213.80 | 234.12 / 242.33 |
| 512/2048/2048 | 321.84 / 323.39 | 316.99 / 312.29 | 356.74 / 350.04 | 337.27 / 336.59 | 308.04 / 305.91 |
| 1024/4096/4096 | 989.96 / 1057.25 | 969.76 / 1006.10 | 1087.24 / 1139.21 | 1050.88 / 1118.70 | 703.29 / 790.46 |

A separate paired large-AR experiment tried retaining the sent local partial
in UB (remove its reread), merging eight sends into one 64-row send, and then
merging eight incoming reads into one 64-row read. All preserve FP32 and passed
numerical checks. Production has no selector for these diagnostic variants.

| Large AR device median, us | Seed 19 | Seed 37 |
|---|---:|---:|
| Shared GEMM + original pull | 888.54 | 885.12 |
| Initial push (8-row chunks) | 992.32 | 983.44 |
| Retain local UB | 992.23 | 983.85 |
| Also batch sends | 990.76 | 988.92 |
| Also batch incoming reads | 1002.34 | 988.17 |
| Official full device span | 650.27 | 637.01 |

The extra local reread and small DMA transaction count are therefore not a
sufficient explanation of the regression: removing them does not recover the
gap. Expensive remote reads in a fenced diagnostic do not prove remote writes
are cheaper. Do not introduce extra workspace/layout contracts on that inference
alone. Link saturation, cache behavior and MTE contention remain hypotheses,
not established root causes from these timings.

### PMU interpretation and retained evidence

Separate PipeUtilization/Memory cohorts again use 10 warmups + 30 samples.
Rank-0 large AG median is 1063.62 us before versus 813.04 us with shared GEMM;
MAC ratio rises from 0.334 to 0.423 and MTE2 ratio from 0.581 to 0.929. This
supports improved compute overlap, not a claim of measured HBM saturation.
AR pull/push medians are 869.76/967.37 us; MAC ratios are 0.176/0.153.
Direct-registered Tile AIV PMU and BlockNum metadata remain unavailable; zeros
must not be interpreted as inactivity, and incomplete normalized bandwidths
must not be used to infer physical link bandwidth. Official AR's AICore PMU
duration still excludes its other fused tasks and is not its full device span.

Separate fenced phase artifacts subsequently passed four generations, two
launches/generation, on both ranks for each direction. Every core/role recorded
16 wave samples per generation and all eight mixed launches/rank were present.
The FP32 precision generation passed. For steady-state analysis, exclude the
first (sparse precision) generation and the first two waves of each launch
(records 0/1/8/9, before two-slot backpressure starts). This leaves 576 AIC
and 1152 AIV records per rank/variant. Percentages below divide summed phase
ticks by summed sampled interval ticks; they are not medians of percentages.

| Fenced diagnostic phase | Rank 0 pull | Rank 1 pull | Rank 0 push | Rank 1 push |
|---|---:|---:|---:|---:|
| AIC whole GEMM + fence | 27.92% | 30.76% | 17.00% | 16.29% |
| AIC local-credit wait + fence | 70.89% | 67.59% | 81.94% | 82.78% |
| AIV local partial read | 13.27% | 12.48% | — | — |
| AIV remote partial read | 64.72% | 66.53% | — | — |
| AIV completed local-read + remote-write send | — | — | 92.30% | 61.32% |
| AIV local partial/incoming pair reads | — | — | 1.33% | 1.33% |
| AIV publication/ACK waits + fences | 19.59% | 18.19% | 4.63% | 35.60% |
| AIV FP32 add / convert / output + fences | 2.33% | 2.71% | 1.68% | 1.68% |

Push moves the long phase to the completed send and peer wait, rather than
eliminating it. The rank asymmetry is important: the faster sender waits longer
for its peer; do not report one rank's send duration as independent link cost.
AIV sampled-interval medians are 4844.5/4934 ticks for pull and 5743/5733 ticks
for push, with similar math/output ticks (97/98 versus 96/96). Additional AIC
credit waiting confirms backpressure from AIV under this instrumented schedule.
The send measurement includes both local reads and remote writes; it does not
isolate a hardware MTE3, cache or physical-link cause. Extra PIPE_ALL fences
alter overlap and inflate waiting: neither the percentage change nor these tick
medians is a quantitative decomposition of the uninstrumented ~10% regression.

Reusable conclusion: profile the completed communication protocol, including
peer wait and producer credit return. A remote-read hotspot alone is not a
direction-selection rule. Retain a push design only after its full paired
latency beats pull at the required precision and resource cost.

Remote evidence root is
`/home/d00520898/tilexr-gemm-push-20261027.jK6c3Z`: `formal/`, `tuning/`,
`pmu-v2/`, `qualification/full-execution`, `qualification/full-bindings`,
and their summaries retain raw results. `gemm/`, `combined/`, `push-only/`,
`retained/`, `batched/`, `bulk/` isolate artifacts. Local downloaded summaries
are under ignored `run/relay-gemm-push-20261027/`. Phase instrumentation is a
separate diagnostic artifact, not part of production or the timing cohorts.
Its artifacts are `phase-pull/` (ELF hash `605f62ab…`) and `phase-push/`
(`77ce93a1…`), with full manifest hashes, raw per-wave ticks and run metadata
under `phase-results/{pull,push}`. Post-run device occupancy was idle on 0/1.

For temporary Python diagnostics, avoid filenames such as `profile.py` that
shadow the standard library: the first PMU attempt failed importing torch's
`cProfile` before any NPU launch. Renaming the task script to `pmu_compare.py`
resolved that import failure; only the subsequent complete cohort is reported.
