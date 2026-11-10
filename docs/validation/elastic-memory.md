# ElasticMemory implementation and A5 validation

This records the single-owner segmented-ring implementation of
[ElasticMemory design](../elastic-memory.en.md) ([中文](../elastic-memory.md)).
The development session owns implementation, independent review, execution and evidence;
the design session checks the submitted results against the design. This is **partial
implementation of the complete design**, not completion of every acceptance item.

The user-approved single-owner segmented ring and 2.5 us allocation target are
implemented and tested on CANN 9.2. The current measurements, retained boundaries
and reproduction commands are in the November 9 section below. The November 10
removal section records the current ABI, fresh regressions and remaining hardware gaps.

## Legacy profiling removal (November 10)

The old communicator capture/encoder, Reserve APIs, ExtraFlag bit, CommArgs
record arrays, private Memory/MoonEP record formats and writers, trace/dump
readers, old CMake/runtime switches, and flat forwarding headers are deleted.
There is no compatibility layer. Public consumers include `<tilexr/profiling.h>`.
Ordinary Memory keeps SDK body markers; ServerDedup writes its business event
catalog through SDK DirectWriter and the existing SDK invocation-slot owner.
ElasticMemory continues to use the #216 Recorder/capture/manifest pipeline.
MoonEP private phase timing has been removed without a replacement stage API;
its framework profiler, Host stage barriers, DFX/status and business timeouts
remain. UDMA registration Profile APIs are unrelated to timing and remain.

Shared CommArgs, ServerDedup tiling and MoonEP native/ctypes layouts changed;
rebuild/deploy Host libraries, external kernels, headers and Python consumers
together, using a clean install prefix. The new SDK slot capacity setting is
strictly decimal `TILEXR_PROFILING_RECORDS_PER_STREAM`, default 8, valid multiples
of 8 in 8..65536. Detailed captures need sufficient capacity and zero drops;
finalization no longer assumes every operator emits exactly two events.

### Current evidence

All operations used the isolated ElasticMemory worktree. Environment: a5 /
rack08os2, aarch64, CANN 9.2.0 (`/home/pkg/b070/cann-9.2.0`), Ascend950DT,
physical 0/1 with identity mapping and no runtime remap, driver 26.2.0.b020 and
firmware 9.2.0.101.200 (fresh board/driver queries retained). CANN 9.2 is the validated
build/run environment; other versions are not promised. The existing HCCL
qualification and its retained per-rank limits were reused, not rerun.

| Check | Result and boundary |
|---|---|
| Full CANN build/install | OFF and ON PASS, EP + ElasticMemory + all MoonEP stages + UniTest; ON uses compact STAGES |
| SDK Host / offline | 5/5 CTest and 23/23 Python PASS |
| EP Host | Release 12/12 PASS; profiling-enabled launch validation 1/1 PASS |
| MoonEP ctypes / diagnostic | FFI 7/7; continuous-iteration 3/3; CPU diagnostic pytest 5/5 PASS |
| CI controller contract | 56/56 PASS |
| A5 Host CTest | OFF and ON each 36/38 PASS; two independently reproduced baseline failures below |
| Native pool | 20 calls, 20 side effects PASS |
| Growth bookkeeping fault | one allocation, one injected failure, one free, one side effect PASS |
| Real Torch / torch_npu | 20 normal cases per rank PASS; both failure-propagation cases PASS, final committed/metadata zero |
| Ordinary Memory OFF | 2 ranks, BF16, bs16/h128/topK2, warmup 20 + measured 100 Dispatch/Combine per rank; output oracle and no-growth checks PASS |
| Installed cleanup | OFF/ON clean prefixes contain no old flat headers; libtile-comm exports no removed Reserve APIs |
| Independent review | Caught/fixed missing SubmitPair signature and stale ABI/config tests; final read-only review found no remaining actionable implementation defect |

Torch used Python 3.12, Torch 2.10.0+cpu plus real torch_npu 2.10.0.post7 at
`/home/l00984991/envs/py312_lzy`. The extension was rebuilt against the new clean
install. Normal cases include FP16/BF16, dropped Pending/inputs, zero receive,
alias/trim, surviving growth, cross-stream consumers, delayed retirement and
close with live Storage. These are real NPU tests, not the CPU diagnostics test.
The latter initially failed because torch_npu auto-loaded without CANN in that
CPU-only command; `TORCH_DEVICE_BACKEND_AUTOLOAD=0` makes its scope explicit.

The default unoptimized EP Host build also segfaulted in host_validation; the
unchanged `origin/main` ce4b9dfd reproduces that failure. Release passes. No claim
is made here about that baseline failure's root cause. The unchanged main also
reproduces Planner V3's exact-fit peer-publication assertions and Combine
schedule's credit-IPC size assertions. Full CTest is therefore not clean. Main's
Dispatch width-8 capacity static_assert prevented compilation after its rank
limit became 256; this change checks the actual default width 16 and rejects
credit group counts above the existing 16-group capacity before launch. It does
not expand the protocol/workspace or prove 256-rank execution.

### Occupancy refusals and missing hardware coverage

The initial launch was refused at 02:39:52 UTC: device 0 PID 520205/python3, 350MB;
device 1 idle. No NPU test had launched. After the process disappeared, a new
combined mapping/proc-mem gate passed at 02:41:34 and the regressions ran. Each
subsequent test required a new gate. The ordinary Memory OFF pair passed its
02:42:46 gate. At02:43:00 the next pair was refused (PID 532229, 370MB); at 02:44:16
the SDK STAGES pair was refused (PID 540073, 370MB). The final bounded observation
02:44:53..02:46:14 contained five busy snapshots of PID 540073. No further NPU
launch followed. No process was killed/reset or bypassed through device remapping.
These snapshots do not prove exclusive occupancy for the entire passing test
intervals or establish the effect of unrelated tasks; that effect is unknown.

**New SDK capture roundtrip, remaining three-mode/two-shape matrix, and real
MoonEP URMA execution were not completed in this removal run.** The Nov9 SDK
records above are historical results from the earlier ABI and do not validate
this new CommArgs layout. ServerDedup requires at least 16 ranks; an 8-card host
cannot verify that runtime path. Its OFF/ON kernels compile, but that is not
hardware correctness, lane-completeness, timer-calibration or performance evidence.

The single completed OFF pair is a correctness/ABI observation, not a performance
improvement or an exclusive-device benchmark. It keeps native Memory transport,
Host AllGather before Dispatch/Combine, and identical msprof task-based AICore /
runtime-API settings. The native C++ driver has no framework NPU profiler to
silently disable. Full Task Duration includes normal device work and excludes
Host submission/allocation and Task Wait Time. No device diagnostics are enabled.

| Stage / rank | Kernel | Samples | Task Duration min / median / max / P95 (us) |
|---|---|---:|---|
| Dispatch/0 | `tilexr_ep_dispatch_memory_kernel` | 100 | 18.080 / 20.700 / 102.163 / 48.822 |
| Combine/0 | `tilexr_ep_combine_memory_kernel` | 100 | 10.041 / 10.439 / 11.089 / 10.814 |
| Dispatch/1 | `tilexr_ep_dispatch_memory_kernel` | 100 | 16.531 / 17.944 / 107.223 / 58.514 |
| Combine/1 | `tilexr_ep_combine_memory_kernel` | 100 | 9.906 / 10.456 / 11.027 / 10.766 |

Dispatch: backend `native_memory`, native status PASS; cross-rank median [min,max] of rank medians **19.322 [17.944,20.700]us**. Logical-byte bandwidth median [min,max] **0.446 [0.414,0.478]GB/s**.

Combine: backend `native_memory`, native status PASS; cross-rank median [min,max] of rank medians **10.447 [10.439,10.456]us**. Logical-byte bandwidth median [min,max] **0.784 [0.783,0.785]GB/s**.

Dispatch logical final output 8576 B/rank (8192 hidden + 384 assist), maximum output
allocation 17152 B; Combine logical input 8192 B and output 4096 B/rank. Bandwidth uses
those logical output/input bytes respectively, not physical fabric traffic.
This maximum-preallocation pair performs no pool allocations: growth 0→0; pool
committed/peak/trim are not being measured. The ordinary output allocation stays
17152 B through all rounds and is freed at driver teardown. The 2 GiB/rank staging
peer window remains; output sizes must not be interpreted as total HBM savings.
There is no paired Device-pool timing from this run and no updated 2.5 us claim.

### Provenance and retained reproduction

Local evidence root:
`/workspace/TileXR-worktrees/elastic-memory/.elastic-memory-evidence/remove-legacy-profiling-20261110/`.
Remote root:
`/home/h30059441/elastic-memory-dev-20261106/remove-legacy-profiling-20261110/`.
`final-code-sha256.json` identifies 1192 transferred code/dependency files; local
and remote hashes match. `binary-sha256.json` records both build/install sets and
the rebuilt Torch extension. `off-small-maximum_capacity/loaded-maps.json`
records actual loaded Host DSO paths and hashes for both native ranks. Build
logs identify the source tree and linked external kernels. The delivered Git
SHA is recorded in the evidence provenance file and PR, avoiding a circular
self-hash inside this document. Previous binary hashes are not reused.

Build command is preserved in `build-remote.sh` (two variants):

```bash
source /home/pkg/b070/cann-9.2.0/set_env.sh
export PATH=/home/l00984991/envs/py312_lzy/bin:$PATH
cmake -S "$R/source" -B "$R/build-OFF" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$R/install-OFF" -DTILEXR_BUILD_EP=ON \
  -DTILEXR_BUILD_ELASTIC_MEMORY=ON -DTILEXR_BUILD_MOONEP=ON \
  -DTILEXR_BUILD_MOONEP_PLANNER=ON -DTILEXR_BUILD_TESTS=ON \
  -DTILEXR_BUILD_UNITEST=ON -DTILEXR_DIAGNOSTICS_SOC_TYPE=Ascend950 \
  -DTILEXR_EP_ENABLE_PROFILING=OFF -DTILEXR_EP_COMPACT_PROFILING=OFF
cmake --build "$R/build-OFF" --parallel 8
cmake --install "$R/build-OFF"
```

ON selects
`TILEXR_EP_ENABLE_PROFILING=ON` and `TILEXR_EP_COMPACT_PROFILING=STAGES` in a
separate build/install tree. Retained commands/logs:

- Host: `ctest --test-dir "$R/build-OFF" --output-on-failure`; ON likewise.
- Real regression: `bash "$R/run-regressions.sh"`; it rechecks identity/proc-mem before each test and refuses busy/unknown.
- Completed pair: `bash "$R/run-pair.sh" NAME OFF maximum_capacity 16 128 100 0`.
- Pending SDK pair: `bash "$R/run-pair.sh" NEW_NAME ON device_pool 16 128 20 1`.
- Pending generic SDK slots: `bash "$R/run-pair.sh" NEW_NAME ON maximum_capacity 16 128 20 0 kernel_perf 1`.

These environment-specific launch scripts remain evidence only, not maintained
repository tools. Every future launch requires a fresh passing gate; these
commands are reproduction references, not a request for the design session to
run tests. Raw profiler output, occupancy JSON/mapping, per-rank logs, snapshots,
source/binary manifests and review are retained in the above roots. The former
Pool occupancy-violation run remains excluded as described in its original section.

## Supported behavior and boundaries

| Design contract | Implementation / evidence |
|---|---|
| Device calculates actual size | Production Memory Dispatch computes receive prefixes, then allocates one hidden+assist bundle on Device. No Planner or Host size oracle is used by this entry. |
| Original kernel waits for real Host growth | M0 resident-kernel experiment and production allocator hardware tests; current kernel consumes a new HBM address without replay. |
| Failed growth terminates the call | Budget failure reaches both ranks; Host bookkeeping fault after successful HBM allocation returns Device RuntimeFailure, frees the unpublished block and does not replay. Runtime/device failures without confirmed completion quarantine ownership; no recovery/retry. |
| FIFO | One native service stream and admission queue per pool; multiple submitted allocation calls complete in order. One active routing handle per Torch buffer. |
| Initial data HBM zero | Pool starts with control metadata only (6,208 reported bytes with mailbox ABI v3). First nonempty call creates a segment. Zero-sized output does not allocate data. |
| Dynamic return | Hot calls reuse; larger calls grow; cache-budget collection and explicit trim invoke real segment free after lease/events finish. Live views keep stable addresses. |
| Ordinary Tensor / Storage | Installed torch_npu `from_blob`, ordinary FP16/BF16 ND tensors, no output copy; alias, registered stream, pending input owners and close-with-live-Storage tests. Inference only; `requires_grad` rejected. |
| Dispatch + Combine | Real two-rank Memory transport and existing Combine with independent routing/value oracle. UDMA direct output, multi-host, quantized/shared-expert/TP paths are outside this implementation. |
| Asynchronous API | Submit runs independently of Python GIL. `WaitMetadata` / Torch `materialize` return stable metadata and establish consumer stream dependencies. Native `Wait` and explicit `synchronize` check full completion/late failures. Combine/discard check the producer before releasing its turn. See the November 9 async validation below. |
| Version support | Native C++14; Torch extension C++17 from installed Torch. This feature is validated on CANN **9.2.0**, A5/Ascend950DT, satisfying the user-scoped version acceptance. Other CANN versions are not promised and are not outstanding requirements. 910B, training/autograd and graph capture are not supported by this delivery. |
| Prior performance baseline | Before the metadata-ready change: owner allocation approximately 1.301–1.671 us; OFF complete pool Dispatch 13.905 / 92.332 us versus maximum output 19.503 / 93.645 us. Full distributions, clock uncertainty and tails are below; Host/growth are separate. |

Core limits are 32 segments, 128 outstanding reservation records (including
retired records blocked behind a live prefix), and 128 registered consumer
streams/events per lease. Exceeding metadata capacity fails explicitly. The
allocator does not relocate live segments to compact memory.
The caller must keep device context and registered streams alive through retirement.

## Standalone profiling SDK migration and retest (November 9, #216)

Based on merged `main` `ce4b9dfd` (including profiling #216 `4d3e5276` and
ElasticMemory #215), this consumer uses SDK 1.0.0, DeviceView ABI 2 and
`mapping.v2`. Both Host and kernel were rebuilt. Each capture has an explicit
`generation=ordinal+1`; SDK records, completion and rank-owned Host-event manifest
shards are exported after `Pending::Wait`, on the creating context. Initialize
uses the actual Pool service stream; Collect does not execute inside its launch
callback or block resident-kernel growth.

The maintained `tests/elastic_memory/profile_compact.py` prepares the two-rank,
64-lane/rank manifests **before launch**, using the declared route workload,
then uses the #216 APIs `attach_completion`, `merge_manifest_shards` and
`generate_capture_set_reports`. The SDK emits per-capture Markdown/HTML/Perfetto/
JSON/CSV and capture-set repeat distributions. No separate timer, encoder or
stage-pairing implementation was added. SDK header dependencies are explicit in
the compact kernel CMake target so an SDK change rebuilds the device consumer.

Environment: `a5` / `rack08os2`, aarch64, Ascend950DT V120, physical 0/1 identity
mapping, CANN **9.2.0** (`/home/pkg/b070/cann-9.2.0`), driver `26.2.0.b020`,
firmware `9.2.0.101.200` from this run's board query. Torch 2.10.0+cpu plus real
`torch_npu` 2.10.0.post7 / Python 3.12. Other CANN versions are not promised.
All launches passed a fresh combined mapping + proc-mem gate, without remapping.
No HCCL qualification was repeated; the retained qualification and its existing
per-rank evidence limits remain applicable.

### SDK stage evidence and instrumentation boundary

Four STAGES pairs cover bs16/h128 and bs256/h4096, ordinary output retirement
and retaining one prior output to force nonempty ring wrap. Each rank runs
20 warmups without capture, then 20 measured captures with unchanged growthCount
1→1. All 80 shared captures have 128 expected/present/usable lanes, complete
publication and cleanup health, matching generation and zero dropped/overflow:
**10,240 lane instances, 67,200 markers, 160 owner allocator intervals**.

Owner is lane 63. Values below are approximate microseconds, derived from raw
single-lane event 6→21 differences; there is no cross-lane/rank timestamp
subtraction. P95 uses the nearest-rank definition.

| Case | Rank | Min | Median | Max | P95 |
|---|---:|---:|---:|---:|---:|
| stages-small-final | 0 | 1.559 | 1.571 | 1.791 | 1.582 |
| stages-small-final | 1 | 1.538 | 1.548 | 1.558 | 1.552 |
| stages-large | 0 | 1.567 | 1.578 | 1.758 | 1.624 |
| stages-large | 1 | 1.548 | 1.557 | 1.790 | 1.589 |
| stages-ring-small | 0 | 1.442 | 1.459 | 1.470 | 1.469 |
| stages-ring-small | 1 | 1.435 | 1.441 | 1.666 | 1.464 |
| stages-ring-large | 0 | 1.454 | 1.461 | 1.645 | 1.501 |
| stages-ring-large | 1 | 1.433 | 1.455 | 1.478 | 1.467 |

All 160 samples span 1.433–1.791 us, median 1.540 us, P95 1.592 us; no observed
sample exceeds 2.5 us under the retained approximate scale. This is sampled
measurement, not a hard latency guarantee. The explicit scale is
1000.8934146536867 ticks/us, retained from same-target empirical GetSystemCycle
vs msprof duration sweeps (`optimization-20261108-resume/timer-calibration/`).
It is not an independently identified timer frequency or a new #216 calibration
session. Short-interval absolute error, marker overhead and cross-lane epochs
remain unqualified. Raw ticks and clock basis are preserved. Every longest
same-lane kernel-body interval is shorter than its matching whole-task duration;
that consistency check alone does not calibrate the clock.

SDK capture-set `critical_lane_union_us` describes the slowest critical lane;
that lane may not own allocation. Owner stages come from the owner lane's paired
samples, not a zero/absent allocator column on another lane. Stage nesting and
parallel work prohibit summing these stages into a serial critical path.

The STAGES versus OFF Dispatch medians of rank medians are 14.625 versus
12.596 us (small), and 93.627 versus 92.010 us (large). Different batches include
rank arrival variance; STAGES ring-small rank medians are 23.991 / 13.611 us.
These differences expose diagnostic disturbance and scheduling skew, not an
isolated recorder-cost estimate; no subtraction correction is applied.

### Complete operator comparison with device diagnostics OFF

Both ranks use BF16, topK=2, experts=4, route `(token*2+k+source)%4`, native Memory
transport and 64 AIV blocks. Host AllGather barriers remain before Dispatch and
Combine. All ten native paired measurements use identical msprof task-based
AICore/runtime-API profiling; no Python/framework profiler exists in this native
driver, so none is silently toggled between native modes. Torch regression is
functional coverage, not this timing source. The OFF comparison has 20 warmups
and **100 measured Dispatch/Combine tasks per rank/mode/shape**, with no growth.
Host allocation, Host queue/wall time and Task Wait Time are excluded; normal
Device allocation, metadata and in-kernel synchronization remain included.

All kernel times below are named msprof **Task Duration**, in microseconds.
Cross-rank summary is median [minimum, maximum] of per-rank medians, not a pooled
sample median.

| Shape | Mode | Rank | Dispatch min / median / max / P95 | Combine median |
|---|---|---:|---|---:|
| small | maximum_capacity | 0 | 17.941 / 20.867 / 93.503 / 52.532 | 10.529 |
| small | maximum_capacity | 1 | 16.748 / 17.918 / 98.567 / 66.819 | 10.997 |
| small | host_actual_reference | 0 | 17.716 / 19.733 / 80.129 / 42.786 | 10.893 |
| small | host_actual_reference | 1 | 16.378 / 18.952 / 120.877 / 65.301 | 10.463 |
| small | device_pool | 0 | 10.559 / 13.925 / 48.216 / 22.729 | 10.568 |
| small | device_pool | 1 | 10.552 / 11.267 / 21.475 / 15.610 | 10.904 |
| large | maximum_capacity | 0 | 94.324 / 97.997 / 99.719 / 99.225 | 76.166 |
| large | maximum_capacity | 1 | 86.600 / 88.885 / 90.118 / 89.667 | 75.332 |
| large | host_actual_reference | 0 | 94.139 / 98.505 / 100.146 / 99.552 | 76.834 |
| large | host_actual_reference | 1 | 84.466 / 88.984 / 90.107 / 89.799 | 74.355 |
| large | device_pool | 0 | 88.869 / 95.031 / 99.048 / 97.715 | 76.236 |
| large | device_pool | 1 | 84.998 / 88.990 / 99.698 / 96.789 | 75.293 |

| Shape | Mode | Dispatch cross-rank us | Combine cross-rank us | Logical output GB/s cross-rank |
|---|---|---|---|---|
| small | maximum_capacity | 19.392 [17.918, 20.867] | 10.763 [10.529, 10.997] | 0.445 [0.411, 0.479] |
| small | host_actual_reference | 19.343 [18.952, 19.733] | 10.678 [10.463, 10.893] | 0.444 [0.435, 0.453] |
| small | device_pool | 12.596 [11.267, 13.925] | 10.736 [10.568, 10.904] | 0.689 [0.616, 0.761] |
| large | maximum_capacity | 93.441 [88.885, 97.997] | 75.749 [75.332, 76.166] | 45.060 [42.863, 47.257] |
| large | host_actual_reference | 93.745 [88.984, 98.505] | 75.594 [74.355, 76.834] | 44.923 [42.642, 47.205] |
| large | device_pool | 92.010 [88.990, 95.031] | 75.764 [75.293, 76.236] | 45.701 [44.201, 47.201] |

Every Dispatch/Combine stage is backend `native_memory`, native status `PASS`.
Pool uses `tilexr_ep_dispatch_compact_kernel`; the two preallocated references
use `tilexr_ep_dispatch_memory_kernel`; all use
`tilexr_ep_combine_memory_kernel`. Dispatch bandwidth is logical hidden+assist
output bytes divided by the same device-task duration, not physical fabric or
HBM bandwidth. Combine logical input is hidden payload 8,192 / 4,194,304 B,
output is 4,096 / 2,097,152 B per rank (small / large); its full distributions
are in `audit.json`. No UDMA bandwidth is claimed.

The measured OFF compact binary SHA256 is unchanged from #215
(`6e46c01e...b5993c`). This migration changes profiling evidence and analysis;
latency differences from prior batches do not demonstrate a kernel optimization.
Current pool medians are below the two references in this batch, while tails and
rank imbalance remain visible. No stronger hardware-wide claim follows.

Actual pool bundles are **8,576 / 4,200,448 B**; counts add 32 B per rank outside
the pool, so complete output totals are 8,608 / 4,200,480 B. Maximum-capacity
bundles reserve 17,152 / 8,400,896 B. Ordinary pool committed/peak is
8,639 / 4,200,511 B (63 B alignment headroom), unchanged throughout sampling;
retained-output ring committed/peak is 21,440 / 10,501,120 B. Successful final
trim returns every data segment; the native test asserts committed=0 before
Close. Control metadata is 6,208 B before Close, zero after the last owners are
dropped. The **2 GiB staging + 4 MiB flags + 1 MiB guard per rank remain**;
output reduction is not an equivalent reduction in total HBM.

### Regression, provenance and reproduction

- CANN9.2 STAGES/OFF full build+install and fresh Torch extension build: PASS.
- Standalone SDK Host/protocol: 5/5 PASS; offline Python suite: 23/23 PASS;
  C++14 allocator CTest: PASS.
- Native pool: 20 calls / 20 side effects, FIFO/ring/zero/metadata-early/late-error/
  delayed-consumer lifetime checks PASS.
- Post-HBM-allocation Host fault: 1 allocation / 1 injected exception / 1 real
  free / 1 side effect, failed-domain admission rejected. The first command
  omitted the fixture's required byte-selector environment and failed its
  **pre-ACL argument check**; the preserved error log is not a failed kernel run.
  The corrected, freshly gated invocation passes.
- Torch: 20 normal cases/rank and paired rank-failure propagation/cleanup PASS;
  real ordinary Tensor, alias, cross-stream, zero-output and early-drop paths.
- Independent read-only code review and independent raw CSV/records statistics
  audit: no confirmed defect/inconsistency; no reviewer NPU launch.

The last workload completed both ranks with exit0. At **12:56:00 UTC**, the
post-test gate rejected card0 (PID2874292, 2368 MB; process name unavailable),
while card1 was idle. The command batch exits90 for this gate, not for a failed
workload. No subsequent NPU launch occurred. Ownership of that process and any
relation to completed tests are unconfirmed; do not claim the card is free or
that no external task was affected. No kill/reset/remap was used.

Local evidence root:
`/workspace/TileXR-worktrees/elastic-memory/.elastic-memory-evidence/profiling-v216-20261109/`.
Remote root:
`/home/h30059441/elastic-memory-dev-20261106/profiling-v216-20261109/`.
`final-code-sha256.json` + `final-code-verification.log` bind the unchanged 1,210
non-documentation source/dependency files to the deployed snapshot. Build logs,
`binary-sha256.json`, per-run `binary-sha256.txt` and actual `/proc/PID/maps`
DSO hashes bind the launched executables, Host libraries and external kernels.
The OFF/STAGES compact kernel hashes are respectively:

```
6e46c01ec1258b8adab56300d2695bf87b96dcd359cd9b017b199137e4b5993c
eb583930723800bb7883d6a90e832f42742e6522af1add076827ca407307a336
```

Each measured directory retains proc-mem/mapping preflight, logs, msprof raw
records/CSV and loaded maps; SDK directories also retain planned manifests,
records/completion/shards, all per-capture reports and `repeat-report/`.
`audit.json`, `owner-raw-samples.json`, `independent-audit.md/json` retain both
machine summaries and independent checks. Environment-specific launch/gate/
monitor scripts stay ignored and are not project tools.

Build from the repository root after sourcing the actual CANN9.2 environment:

```bash
cmake -S . -B build-STAGES -DCMAKE_BUILD_TYPE=Release \
  -DTILEXR_BUILD_EP=ON -DTILEXR_BUILD_ELASTIC_MEMORY=ON \
  -DTILEXR_BUILD_TESTS=ON -DTILEXR_BUILD_UNITEST=OFF \
  -DTILEXR_DIAGNOSTICS_SOC_TYPE=Ascend950 \
  -DTILEXR_EP_COMPACT_PROFILING=STAGES
cmake --build build-STAGES --parallel 8
export PYTHONPATH="$PWD/tools/profiling/python"
python3 tests/elastic_memory/profile_compact.py prepare /fresh/capture-directory \
  --bs 16 --h 128 --lanes 64 --devices 0 1 --host "$(hostname)" \
  --soc Ascend950DT --cann 9.2.0 --driver 26.2.0.b020 \
  --source-id SOURCE_COMMIT_AND_MANIFEST \
  --cycles-per-us CONFIRMED_OR_EXPLICIT_APPROXIMATE_SCALE \
  --clock-basis 'evidence path and accuracy boundary'
```

Export the three `TILEXR_ELASTIC_*` values printed by prepare to **both ranks**.
After a new strict combined idle/identity gate, run the following concurrently
with rank=device=0 and 1, a shared unused `TILEXR_COMM_ID` port and UDMA disabled:

```bash
msprof --output=/fresh/prof-rankRANK --task-time=on --ai-core=on \
  --aic-mode=task-based --runtime-api=on \
  --application="$PWD/build-STAGES/tests/elastic_memory/test_elastic_compact_ep RANK DEVICE kernel_perf device_pool 16 128 20 20"
python3 tests/elastic_memory/profile_compact.py analyze /fresh/capture-directory
```

Repeat as separate freshly gated pairs with `kernel_ring_perf`, and with
bs256/h4096. For the formal comparison, rebuild a separate `build-OFF` with
`TILEXR_EP_COMPACT_PROFILING=OFF`, unset the capture environment, use
`kernel_perf MODE BS H 20 100`, and compare all three modes under identical
msprof/Host-barrier settings. Never reuse the post-test occupancy snapshot as
launch permission. Existing reports can be read/reanalyzed using the public SDK
capture-set API into a new output directory; the preparation/analysis helper
publishes files exclusively and does not overwrite a completed run.

## Metadata-ready materialization and 5 us active polling (November 9)

The new implementation is based on merged PR #212 (`5836b343`). Native
`Pending::Wait()` remains data-complete. `WaitMetadata()` publishes a validated
immutable allocation only after recording the producer event; Torch `materialize`
queues that event on its consumer stream and returns ordinary Tensor Storage.
Explicit `synchronize()` checks late runtime/protocol errors. Output metadata is
never rewritten: ABI3 adds a separate 64B finalStatus at offset 320. Rebuild the
Host libraries, external kernels, and Torch extension together. Same-pool FIFO
still waits for full producer completion; this is not concurrent Device allocation.

The active service loop requests `sleep_for(5us)` instead of 50us. This is an OS
sleep request, **not a 5us wakeup guarantee**. A short Dispatch may complete before
the Host observes its metadata; early return is permitted, not guaranteed on every
call. A producer event establishes ordering, not a promise that no late error will
occur. Failed outputs and downstream results derived from them are invalid.

Environment: A5 `rack08os2`, physical 0/1 identity mapping, Ascend950DT,
CANN 9.2.0, driver 26.2.0.b020, Torch 2.10.0+cpu with torch_npu 2.10.0.post7.
Each launch uses a fresh combined proc-mem/mapping gate. Busy snapshots at
08:04, 08:28 and 08:33 UTC rejected launches; no external process was killed,
reset or remapped. Native tests ran after the 08:25 and subsequent fresh idle
gates; paired Torch normal tests followed the 08:30:11 idle gate. HCCL was not
rerun; the retained qualification limitations below still apply.

Evidence root on A5:
`/home/h30059441/elastic-memory-dev-20261106/async-materialize-20261109/`.
Local evidence root:
`/workspace/TileXR-worktrees/elastic-memory/.elastic-memory-evidence/async-materialize-20261109/`.

| Check | Result / retained log |
|---|---|
| CANN 9.2 OFF build, install and real Torch extension build | PASS; `build-updated.log`, `build-close-fix.log`, `build-final-tests.log`, `torch-build.log` |
| C++14 allocator, `-O2 -Wall -Wextra -Werror` | PASS ABI3; `allocator-strict-final.log` |
| Native metadata and existing pool contracts | PASS; `pool-final.log`, 20 calls / 20 side effects, FIFO leader 3872B / follower 128B, hot reuse/wrap/segment switch/trim |
| Actual early readiness | Held producer confirms metadata while producerDone=0; consumer stays blocked until release, then verifies payload. Four cases cover 704B, early drop, zero bytes and late error; 129 repeated same-stream waits are idempotent |
| Zero-output Close | No external allocation: Close waits delayed consumer boundary and metadata becomes 0. Live zero allocation: Close returns with 6208B control metadata; final drop/Close clears it |
| Host bookkeeping failure after growth | PASS; `growth-fault.log`: allocations=1 / injected=1 / frees=1 / side_effect=1 |
| Real paired Torch Dispatch/Combine | PASS both ranks exit0; `torch-normal2-rank{0,1}.log`: immediate ordinary ops, repeated materialize/new stream, zero count dependencies, FP16/BF16, aliases, old view across growth, delayed consumers, close |
| Single-rank failure | PASS both ranks exit0; `torch-failure-final-rank{0,1}.log`: rank0 allocation error 2, rank1 finalStatus error 2, domain rejects later requests; dropping failed owners and Close leaves committed/metadata0 |

Maintainer lesson: producer completion is not sufficient to destroy its event.
The installed CANN 9.2 `acl_rt.h` requires the associated StreamWaitEvent tasks to
have completed. Retain per-stream wait-boundary events, including zero-byte
allocations, and drain unowned boundaries in waiting trim/Close. Keep live zero
allocations valid across Close. Also preserve immutable early metadata and report
late errors through a separate slot. The held-producer and delayed-zero-boundary
regressions exercise these distinct boundaries directly.

Pipeline timing and final artifact provenance are recorded with this section's
result package; earlier allocation/Dispatch numbers below are separate baseline
measurements and must not be claimed as the async change's speedup.

### Matched pipeline profiles

The same binary compares `pending.synchronize()` (full_wait) with
`pending.materialize()` (metadata). Both use BF16/topK2/two ranks, routing
`(token*2+k+source_rank)%4`, ordinary Memory backend, native
`tilexr_ep_dispatch_compact_kernel` (ABI3, Device diagnostics OFF), an immediate
ordinary Torch mul, CANN 9.2/torch_npu Level0 CPU+NPU profiling, and the identical
pre-round device synchronization plus CPU Gloo stage barrier. Each rank/mode has
20 warmups and 100 measured rounds. CPU correctness checks and retirement occur
outside the timed pipeline. These are separate process runs with the same
configuration, not a randomized old/new-version or 50us/5us polling A/B.

All times below are **us**. Each metric cell is **median / min / max / P95**
for 100 measured samples on that rank. Dispatch and mul timestamps come from
hardware tasks on the same device; gap is mul start minus Dispatch end; span is
Dispatch start to mul end. No Host/device clock subtraction or cycle-frequency
assumption is used.

| Shape | Mode | Rank | Dispatch kernel | Dispatch→mul gap | Device pipeline span |
|---|---|---:|---|---|---|
| 16/128 | full_wait | 0 | 22.747 / 10.167 / 53.901 / 34.697 | 202.995 / 169.031 / 236.842 / 218.838 | 226.530 / 207.927 / 252.326 / 240.389 |
| 16/128 | full_wait | 1 | 10.537 / 9.972 / 59.307 / 23.436 | 206.790 / 170.138 / 225.597 / 219.505 | 219.718 / 209.308 / 274.306 / 233.405 |
| 16/128 | metadata | 0 | 24.137 / 10.500 / 74.012 / 39.870 | 182.205 / 152.862 / 202.855 / 196.602 | 208.131 / 190.759 / 245.165 / 220.104 |
| 16/128 | metadata | 1 | 10.784 / 10.118 / 35.386 / 11.219 | 186.293 / 153.374 / 206.793 / 199.354 | 198.792 / 165.742 / 219.124 / 211.424 |
| 256/4096 | full_wait | 0 | 91.523 / 77.157 / 128.297 / 97.999 | 207.822 / 186.387 / 251.090 / 236.887 | 302.533 / 284.806 / 353.580 / 323.737 |
| 256/4096 | full_wait | 1 | 91.439 / 77.472 / 170.165 / 132.826 | 194.474 / 164.143 / 276.927 / 220.116 | 293.663 / 274.601 / 401.937 / 351.976 |
| 256/4096 | metadata | 0 | 90.539 / 76.904 / 98.098 / 97.114 | 114.063 / 92.145 / 182.848 / 143.152 | 207.967 / 194.521 / 284.609 / 232.482 |
| 256/4096 | metadata | 1 | 93.902 / 80.635 / 158.575 / 122.914 | 147.830 / 107.125 / 194.308 / 174.323 | 255.496 / 198.369 / 321.910 / 288.031 |

Cross-rank gap: rank medians aggregated as **median [min,max]**; pooled P95
uses all 200 measured rank samples and is distinct from the rank-median aggregate.

| Shape | Mode | Rank-median gap [min,max] | Pooled gap median/P95 | Rank-median pipeline span |
|---|---|---|---|---|
| small | full_wait | 204.892 [202.995,206.790] | 205.321/219.211 | 223.124 |
| small | metadata | 184.249 [182.205,186.293] | 184.098/199.354 | 203.462 |
| large | full_wait | 201.148 [194.474,207.822] | 202.455/231.350 | 298.098 |
| large | metadata | 130.946 [114.063,147.830] | 123.220/170.565 | 231.731 |

The observed rank-median gap decreases by 20.643us (small) and 70.202us (large).
This includes full_wait's extra Python/native Synchronize and scheduling costs;
**it does not establish a pure kernel speedup, isolated polling benefit, or a
universal asynchronous-overlap saving**. In all 100 measured samples for each
rank/mode/shape, CPU `aten::mul` begins after Dispatch hardware completion. This
does not exclude earlier overlap of metadata retrieval or Tensor construction,
but it does not show early downstream operator submission either. The held
producer test, separately, proves that metadata may actually return while the
producer remains resident. There is no per-call early-return latency guarantee.

Both shapes grow once during cold startup; every measured round is growth 1→1.
Each rank retains 16MiB committed/peak pool HBM during the measurements and
returns all 16MiB at final trim. Small output: hidden 8192B, pooled hidden+assist
bundle 8576B, plus 32B of separately allocated counts (all output tensors 8608B).
Large output: hidden 4194304B, pooled bundle 4200448B, plus 32B counts
(all output tensors 4200480B). Pool output savings are not total-HBM savings:
each rank still has 2GiB peer data staging, 4MiB flags and 1MiB guard, as well as
inputs, Torch allocations and 6208B pool control metadata before final Close.

Effective hidden-output throughput (hidden bytes / Dispatch kernel duration)
is an output-rate metric, not wire or HBM traffic bandwidth. At per-rank medians:
- small: full_wait rank0 0.360 GB/s, rank1 0.777 GB/s; metadata rank0 0.339 GB/s, rank1 0.760 GB/s.
- large: full_wait rank0 45.828 GB/s, rank1 45.870 GB/s; metadata rank0 46.326 GB/s, rank1 44.667 GB/s.

Cold ordinal 0 contains growth and is excluded from the hot tables. Raw cold/hot
samples and Host enqueue/completion distributions remain in `rounds.jsonl` and
`result.json`; Host intervals include Python, allocation, queueing and completion
and are not kernel durations. `pipeline-{small,large}/{full_wait,metadata}/`
contains per-rank raw CANN records, framework trace, kernel CSV, environment and
binary hashes. Independent `independent-audit.md/json` recomputes counts, pairing,
units and distributions:120 Dispatch + 120 mul per trace, 20 excluded warmups,
no missing target task or ambiguous pairing; CSV units agree with trace. Profiler
logs warn that the CPU Gloo group has no NPU backend for parallel-group metadata;
the device trace still contains the complete target tasks. No HCCL qualification
was launched by this benchmark.

Cold ordinal0 is a single observation per rank, includes growth and framework
first-use work, and is not a distribution or a steady-state latency estimate.

| Shape | Mode | Rank | Cold Dispatch kernel us | Cold Dispatch→mul gap us | Cold Host-complete us |
|---|---|---:|---:|---:|---:|
| small | full_wait | 0 | 2879.010 | 2765.075 | 228292.450 |
| small | full_wait | 1 | 2963.314 | 2753.655 | 228284.000 |
| small | metadata | 0 | 2363.186 | 3082.258 | 223994.140 |
| small | metadata | 1 | 2547.203 | 2904.780 | 223796.200 |
| large | full_wait | 0 | 2465.514 | 2963.327 | 226685.630 |
| large | full_wait | 1 | 2877.470 | 2959.172 | 226699.160 |
| large | metadata | 0 | 2543.294 | 2801.608 | 224015.680 |
| large | metadata | 1 | 3104.952 | 2740.795 | 223960.950 |

### Async artifacts and reproduction

The PR commit contains the final source; `final-code-sha256.json` pins 749
non-documentation files and is checked against the remote source used for these
artifacts. Manifest SHA-256: `cb65ce42e8c36e7887bca95267b7d18fa07878461ee326f5de5d3a191e909372`. The original broader build
snapshot is retained separately as `source-code-sha256.json`. The benchmark
runner recorded in each result is preserved as `measured-benchmark_pipeline.py`;
the final runner only clarifies its module docstring about caller-selected devices.
An AST comparison excluding that docstring confirms identical executable Python.

These final artifact hashes match the libraries/kernel recorded in all eight
per-rank pipeline profiles:

| Artifact | SHA-256 |
|---|---|
| `install/lib64/libtilexr-elastic-memory.so` | `bb820fd86b973951f20a5342fc0f4ba23c56968f6cf6dd12124aba321d3f3cad` |
| `install/lib64/libtilexr-ep.so` | `bfa3aad0e335d82f500a02970c9f0f5a906e0e4c12474f32ff788f6c3d79e39e` |
| `install/lib64/libtile-comm.so` | `8f6e585988308213e9c22a33e075cb355d8c0b63633710aa13e653212931c961` |
| `install/lib64/tilexr/kernels/tilexr_ep_dispatch_compact_kernel.o` | `6e46c01ec1258b8adab56300d2695bf87b96dcd359cd9b017b199137e4b5993c` |
| `install/lib64/tilexr/kernels/tilexr_ep_combine_memory_kernel.o` | `e77def8099fc6014327f31cb037060353489d5b77e3eb7d23aa7ac7392a7cc7b` |
| `build/tests/elastic_memory/test_elastic_pool` | `d40f9e50c9454dcbd05a5ba5984b0428b110678a09b280d0402b57f735af69db` |
| `build/tests/elastic_memory/elastic_pool_kernel.o` | `02ce6bc31d100e5c9b673a76e635045fd041c31facd6d813fd7b79af2b698a21` |
| `build/tests/elastic_memory/test_elastic_growth_fault` | `1784fe8b1003b206687cf2a2f55af89baf2e4591156ff8855b4626721664ed3f` |
| `build/tests/elastic_memory/libelastic_growth_fault.so` | `cf4e51025f3f9f8700da950d5421d92c9a2a219e3485d1ba31ca9aaaecb22f3e` |
| `source/integrations/elastic_memory_torch/tilexr_elastic/_C.cpython-312-aarch64-linux-gnu.so` | `28360b328f183f99681feadad286cf7d780018a6f22f36d94b5253e6bc8782a4` |

Exact local logs, raw traces, source manifests, independent review and audit are
under the evidence root above. The native and Torch builds use the same separate
remote source/build/install tree; no old installation is substituted. A final
read-only handoff snapshot at08:51:39 UTC showed0/1 idle. It is not authorization
for another launch: rerun the full gate immediately before every test.

Reproduction uses the retained A5 tree and environment; choose unused ports and
communicator domains for paired runs. Do not run these commands on busy cards:

```bash
source /home/pkg/b070/cann-9.2.0/set_env.sh
export PATH=/home/g00623548/test20261102/tools/cmake/bin:$PATH
cd /home/h30059441/elastic-memory-dev-20261106/async-materialize-20261109
cmake -S source -B build -DTILEXR_BUILD_ELASTIC_MEMORY=ON -DTILEXR_BUILD_EP=ON \
  -DTILEXR_BUILD_UNITEST=OFF -DBUILD_TESTING=ON -DTILEXR_EP_COMPACT_PROFILING=OFF \
  -DTILEXR_DIAGNOSTICS_SOC_TYPE=Ascend950DT -DCMAKE_INSTALL_LIBDIR=lib64 \
  -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build -j6
cmake --install build
export LD_LIBRARY_PATH="$PWD/install/lib64:$LD_LIBRARY_PATH"
export TILEXR_INSTALL_PREFIX="$PWD/install"
export PYTHONPATH="$PWD/source/integrations/elastic_memory_torch"
(cd source/integrations/elastic_memory_torch && MAX_JOBS=2 \
  /home/l00984991/envs/py312_lzy/bin/python setup.py build_ext --inplace)
ctest --test-dir build -R '^elastic_allocator$' --output-on-failure
python3 ../idle_gate.py 0 1 && timeout 90 build/tests/elastic_memory/test_elastic_pool \
  0 "$PWD/build/tests/elastic_memory/elastic_pool_kernel.o"
python3 ../idle_gate.py 0 1 && env TILEXR_TEST_GROWTH_FAULT_BYTES=1048593 \
  LD_PRELOAD="$PWD/build/tests/elastic_memory/libelastic_growth_fault.so" \
  timeout 90 build/tests/elastic_memory/test_elastic_growth_fault \
  0 "$PWD/build/tests/elastic_memory/elastic_pool_kernel.o"
```

For paired Torch, after a combined0/1 gate, set `TILEXR_COMM_ID=127.0.0.1:PORT`
and launch `test_npu.py --rank RANK --device RANK --comm-domain DOMAIN` once for
each rank concurrently with the real Python environment above. Repeat with a
fresh domain/port and `--failure`. Require both exit codes and per-rank PASS
records; failure tests finish with committed/metadata0 after dropping owners.
The [pipeline benchmark commands](../../integrations/elastic_memory_torch/README.md#pipeline-profiling)
retain the matched framework profiler and stage barrier. Do not compare their
Host wall times against the complete-kernel-only baseline below.

## Device header layout and file descriptions

The Device allocator and profiling helpers are now installed as
`include/device/tilexr_elastic_memory_device.h` and
`include/device/tilexr_elastic_memory_profile.h`; consumers include them with the
`device/` prefix. Host API and shared ABI stay directly under `include/`.
CMake kernel dependencies and installation rules follow the moved source paths.
The module's file descriptions explain callers and confirmed ownership without
language labels; the repository-wide convention is in `AGENTS.md`.

Layout source **`fc8499455e22690329b49a6f85bccb8a47da2986`** changes comments,
include paths and packaging only. The complete operator measurements below are
evidence for **`d847c22956966e4e0805981ba5039fad894e047b`**; they are not new timing
measurements for the layout revision. Build and install checks use a separate
A5 CANN 9.2 tree under
`/home/h30059441/elastic-memory-dev-20261106/headers-20261109/`;
local logs are under
`/workspace/TileXR-worktrees/elastic-memory/.elastic-memory-evidence/headers-20261109/`.
Validation uses CANN OFF full build, STAGES compact-kernel build, a clean install,
and external Host/Device consumers compiled with only the installed TileXR include
root plus CANN headers. The strict C++14 allocator test and Python AST comparison
check the unaffected implementation. No NPU task or HCCL qualification was run.
`configure-off-final.log`, `build-off-final.log`, `build-stages.log`, `install.log`,
`installed-header-smoke-final.log` and `strict-allocator.log` retain the commands
or outputs; `provenance.json` pins this layout revision separately from performance.
The layout manifest contains 426 files; its SHA-256 is
`d0e1324b46d899a60e06d05acbe0a95f73a0e39060b0697bf8a473c0004bf09e`.

## Segmented-ring Device and complete-kernel measurements (November 9)

The final Device pool Dispatch rank-median aggregate is **13.905 / 92.332 us**,
compared with the same-batch maximum-output reference **19.503 / 93.645 us**.
Owner allocation is approximately **1.301–1.671 us** across the retained normal
and nonempty-wrap samples. These are two-shape measured results, not bounds for
arbitrary shapes or every latency sample.

### Allocation and Dispatch integration

The final count core already owns the final prefix and now performs the single-owner
Device allocation. Each core completes its original send/count work. Only the
leading `min(receiveStatusCount, blockDim)` output lanes receive count rows/flags;
only those lanes consume and clear those flags. Prefix/expert-map/count-output
preparation overlaps allocation. A copy lane waits for the immutable local
address descriptor immediately before output copy. The owner publishes the body,
completes MTE3, then publishes the round magic; readers observe magic before
fetching the body. Peer failure aggregation overlaps other lanes' local copies.
In this earlier measured revision, a peer failure patches the Host output error and
Host waits for complete kernel completion before exporting a Tensor. The later ABI3
async revision separates immutable Output from finalStatus, as documented above.
No Host size Planner, precomputed output address, growth replay or retry was added.
The two preallocated reference entries retain their scheduling behavior.

Extended owner-copy regression exposed a pre-existing low-blockDim UB sizing bug:
`sumCoreBuf_`/`sumLocalBuf_` reserved `blockDim*32` bytes, while Duplicate writes
whole 256B repeats. With 2/4 cores the write exceeds the reserved scratch span; a non-multiple
of eight also needs rounding. Both buffers now reserve `ceil(blockDim/8)*256`.
The original two-core capacity-failure reproduction timed out on both ranks;
a change limited to this sizing removed that timeout and both ranks returned
Device error 2. Matching PID logs were retained; they contain no definitive
kernel-PC fault, so the exact corrupted value is not claimed. The Host UB model
now matches these two aligned buffers; the strict C++14 Host layout test passes.
The 64-core capacity is unchanged. The failure and subsequent final regression
are recorded separately.

### Final complete-kernel comparison

A5 `rack08os2`, physical 0/1 with identity mapping; CANN 9.2.0, driver
26.2.0.b020, firmware 9.2.0.101.200. Native Memory backend, BF16/topK2/four experts,
64 AIV blocks, identical deterministic routes, bs16/h128 and bs256/h4096.
`TILEXR_EP_COMPACT_PROFILING=OFF`; generic EP/Combine diagnostics OFF.
The Host AllGather stage barrier and msprof/framework configuration stay enabled
and identical across all three modes. No CPU affinity modification is used in
this final batch. Each rank has 20 warmups and 100 measured samples, no outliers
removed. Task Duration excludes Host wall and Task Wait Time; normal Device
allocation, publication and synchronization remain inside Dispatch.

All rows have backend `native_memory`, native status `PASS`. Reference Dispatch
kernel is `tilexr_ep_dispatch_memory_kernel`; pool Dispatch is
`tilexr_ep_dispatch_compact_kernel`; all Combine rows use
`tilexr_ep_combine_memory_kernel`. Kernel hashes below identify these versions.
The independent audit joined 2,880 tasks, including 2,400 measured tasks; pool
measurement growthCount is 1→1 throughout. Cross-rank median/min/max summarize
the two per-rank medians. P95 uses nearest rank `ceil(0.95*n)` over the applicable
samples; pooled P95 uses all 200 measured tasks for each stage/case. The Host
actual-size mode remains a comparison-only routing oracle, not a production Planner.

| Shape / mode | Rank | Dispatch median / min / max / P95 us | Cold us | Combine median / min / max / P95 us | Cold us |
|---|---:|---|---:|---|---:|
| small-maximum_capacity | 0 | 21.008 / 18.627 / 72.814 / 58.134 | 70.358 | 10.835 / 10.135 / 12.150 / 11.461 | 11.854 |
| small-maximum_capacity | 1 | 17.998 / 16.298 / 176.084 / 77.928 | 117.777 | 10.448 / 9.913 / 11.164 / 10.829 | 343.644 |
| small-host_actual_reference | 0 | 20.064 / 18.070 / 77.035 / 45.012 | 44.833 | 10.483 / 10.063 / 11.637 / 10.868 | 12.947 |
| small-host_actual_reference | 1 | 18.106 / 15.920 / 138.307 / 65.427 | 35.466 | 10.885 / 10.157 / 11.991 / 11.362 | 246.532 |
| small-device_pool | 0 | 16.845 / 10.649 / 36.187 / 28.580 | 82.433 | 11.139 / 10.405 / 13.352 / 12.592 | 12.324 |
| small-device_pool | 1 | 10.966 / 10.442 / 14.120 / 11.734 | 66.977 | 10.460 / 9.926 / 11.067 / 10.872 | 216.886 |
| large-maximum_capacity | 0 | 98.552 / 89.580 / 146.555 / 100.059 | 127.422 | 77.066 / 75.291 / 97.784 / 79.918 | 264.770 |
| large-maximum_capacity | 1 | 88.738 / 82.695 / 98.227 / 89.976 | 235.641 | 71.791 / 58.564 / 76.564 / 75.332 | 61.982 |
| large-host_actual_reference | 0 | 98.111 / 87.008 / 100.638 / 99.384 | 140.915 | 76.382 / 66.918 / 101.574 / 78.191 | 142.109 |
| large-host_actual_reference | 1 | 89.145 / 86.683 / 107.874 / 91.739 | 151.152 | 74.349 / 56.901 / 87.405 / 81.728 | 61.166 |
| large-device_pool | 0 | 96.175 / 77.853 / 102.234 / 98.928 | 1265.786 | 76.520 / 67.621 / 88.584 / 79.176 | 61.387 |
| large-device_pool | 1 | 88.489 / 84.290 / 114.182 / 101.574 | 1493.821 | 74.168 / 62.194 / 87.257 / 81.192 | 334.677 |

Cold is the first retained task, excluded from the warm statistics. Pool cold
tasks include the resident Host-growth wait and are not judged against the hot
2.5 us allocation target.

| Shape / mode | Stage | Rank medians: median / min / max us | Pooled P95 us | Effective output GB/s: median / min / max |
|---|---|---|---:|---|
| small-maximum_capacity | dispatch | 19.503 / 17.998 / 21.008 | 63.970 | 0.442 / 0.408 / 0.477 |
| small-maximum_capacity | combine | 10.641 / 10.448 / 10.835 | 11.245 | 0.385 / 0.378 / 0.392 |
| small-host_actual_reference | dispatch | 19.085 / 18.106 / 20.064 | 54.255 | 0.451 / 0.427 / 0.474 |
| small-host_actual_reference | combine | 10.684 / 10.483 / 10.885 | 11.340 | 0.384 / 0.376 / 0.391 |
| small-device_pool | dispatch | 13.905 / 10.966 / 16.845 | 23.523 | 0.646 / 0.509 / 0.782 |
| small-device_pool | combine | 10.800 / 10.460 / 11.139 | 12.118 | 0.380 / 0.368 / 0.392 |
| large-maximum_capacity | dispatch | 93.645 / 88.738 / 98.552 | 99.621 | 44.979 / 42.622 / 47.336 |
| large-maximum_capacity | combine | 74.428 / 71.791 / 77.066 | 78.218 | 28.212 / 27.213 / 29.212 |
| large-host_actual_reference | dispatch | 93.628 / 89.145 / 98.111 | 99.294 | 44.966 / 42.813 / 47.119 |
| large-host_actual_reference | combine | 75.365 / 74.349 / 76.382 | 80.944 | 27.831 / 27.456 / 28.207 |
| large-device_pool | dispatch | 92.332 / 88.489 / 96.175 | 99.106 | 45.572 / 43.675 / 47.468 |
| large-device_pool | combine | 75.344 / 74.168 / 76.520 | 81.192 | 27.841 / 27.406 / 28.276 |

The same-batch pool Dispatch increments over maximum output are
**-5.598 / -1.312 us**. Rank-arrival and scheduling tails remain visible in the
table; this does not guarantee a per-sample latency bound. In particular, large
rank 1 has pool P95 101.574 us versus maximum-output 89.976 us (+11.598 us): the
median result does not establish uniformly improved tail latency. No unfavorable
final sample is removed or replaced with another batch.

Effective bandwidth is logical output bytes / Task Duration, not link or HBM bus
bandwidth. Dispatch output is 8,576 / 4,200,448 B (hidden 8,192 / 4,194,304 B,
assist 384 / 6,144 B; count control 32 B separately); Combine output is
4,096 / 2,097,152 B and its logical input equals the hidden payload.

### SDK stages and remaining measurement boundaries

Four final STAGES runs use the repository `TileXR::Profiling::Recorder<64>` and
real pool service-stream capture. Collect runs only after completion so it cannot
block the Host growth service. Each shape has normal and retained-output ring
runs, each with 20 warmups and 20 measured calls per rank. The owner is final
count core 63. All 10,240 lane streams and 67,200 markers are complete without
overflow/drop. The independent parser audit checks raw intervals, mapping,
normal/wrap classification and zero measured growth. The allocator itself still
reads the compact 128B snapshot, commits one 64B ring and publishes the result.

| Run | Rank | Allocation median / min / max / P95, approximate us |
|---|---:|---|
| stages-verified-small | 0 | 1.451 / 1.441 / 1.671 / 1.670 |
| stages-verified-small | 1 | 1.431 / 1.421 / 1.448 / 1.444 |
| stages-verified-large | 0 | 1.308 / 1.302 / 1.466 / 1.322 |
| stages-verified-large | 1 | 1.321 / 1.314 / 1.426 / 1.337 |
| stages-verified-ring-small | 0 | 1.459 / 1.450 / 1.503 / 1.467 |
| stages-verified-ring-small | 1 | 1.305 / 1.301 / 1.557 / 1.396 |
| stages-verified-ring-large | 0 | 1.352 / 1.347 / 1.589 / 1.399 |
| stages-verified-ring-large | 1 | 1.311 / 1.306 / 1.454 / 1.453 |

All 160 owner intervals (120 normal / 40 nonempty wrap) are approximately
**1.301–1.671 us**, with 0 samples above 2.5 us.
The same-target empirical scale is 1000.8934146536867 ticks/us relative to
msprof Task Duration, not an independently identified GetSystemCycle frequency.
Its calibration records remain under
`optimization-20261108-resume/timer-calibration/` in the local evidence root;
this retained clock calibration is the conversion basis for the current SDK results.
No HWTS/AIV/sysClock frequency is silently substituted. Short-stage absolute
accuracy, cross-lane epochs and diagnostic disturbance remain limitations.
`output_setup` includes count waiting plus prefix/expert-map/count preparation;
`copy` now covers WaitAndFormatOutput only. These intervals overlap allocator
work on another core and must not be added as a serial path. Separate STAGES/OFF
task differences mix rank arrival with instrumentation; no subtraction correction
is used. Raw records, current mappings and official SDK HTML/Perfetto reports
are retained for every final capture.


Diagnostic perturbation check (separate STAGES/OFF batches, identical stage barrier):

| Shape | Rank | STAGES Task median us | OFF Task median us | Owner send/count median approx. us | Owner rank-gate median approx. us | Slowest-lane body median approx. us |
|---|---:|---:|---:|---:|---:|---:|
| small | 0 | 13.438 | 16.845 | 3.660 | 2.083 | 8.730 |
| small | 1 | 22.541 | 10.966 | 13.157 | 2.097 | 17.312 |
| large | 0 | 97.848 | 96.175 | 45.954 | 5.524 | 89.337 |
| large | 1 | 88.593 | 88.489 | 44.402 | 3.231 | 81.257 |

Slowest lane means the longest same-lane body interval, not an established
common-epoch completion critical path. Per-lane raw ticks and all substage
min/median/max/P95 remain in the SDK reports. These differing task distributions
do not establish negligible instrumentation cost; use only OFF for the comparison.

### Functional regression and retained failures

The final binary passes the strict C++14 allocator test, native pool/FIFO/ring,
post-HBM Host-bookkeeping fault, two-rank EP normal/zero/fail and real Torch
normal/failure tests. Torch uses 2.10.0+cpu with torch_npu 2.10.0.post7 on real NPUs,
not a mock: each rank passes 20 normal cases plus distributed failure. Seven
same-communicator transitions cover copy×count dimensions
32×2 → 2×1 → 4×1 → 4×1 → 9×2 → 32×2 → 4×1, including owner copy,
non-eight-multiple blockDim and shrinking/expanding flag layouts. Each checks all
four Dispatch outputs and Combine values, then retires ownership. Existing alias,
view across growth, delayed consumer, registered stream, dropped owners, FP16/BF16,
zero receive and close-with-live-Storage cases remain checked. Two-core zero and
capacity failure are additionally exercised directly.

FIFO still reserves a growing head request's new space: the retained native test
queues both requests before launch, observes one growth, then leader 3,872 B at
offset 0 and follower 128 B at offset 3,904. Pool tests retain one side effect per
call (14/14), nonempty wrap without growth, 864 blocked bytes, 160 padding bytes,
segment switch without growth and final trim-to-zero. Fault injection records
one allocation, one failure, one free and one side effect; poisoned domains reject
new requests. Output memory is unchanged: actual bundles 8,576 / 4,200,448 B,
committed/peak 8,639 / 4,200,511 B per rank, metadata 6,208 B. Empty segments return
on trim. The 2 GiB staging remains; Torch additionally retains 4 MiB flags and a
1 MiB guard. Output savings are not equal to total HBM savings.

Every actual launch has a fresh combined proc-mem/identity gate; busy or unknown
stops it. Retained rejected gates include the pre-overlap diagnostic and post-test
cleanup windows. PID 3470491 in the first Torch rejection matches the preceding
native failure test's plog; other ownership is not inferred without records.
No card reset, mapping change, HCCL requalification or external-task termination
was performed. The historical November 6 occupied-card run remains excluded and
its external impact remains unknown.

Two diagnostic fixture failures are retained and excluded: an old hardcoded end
event (28 versus the new 32), fixed by using the shared enum, and an invalid
`ring_kernel_perf` argument rejected before ACL initialization (correct name:
`kernel_ring_perf`). The two-core timeout and its sizing fix are also retained;
no timeout or msprof exit alone is counted as PASS.

### Memory and lifecycle accounting

Per rank, both shapes start at zero data HBM. The warm single-output comparison
allocates actual hidden+assist bytes, while retaining the existing staging:

| Quantity | bs16/h128 | bs256/h4096 |
|---|---:|---:|
| Actual output bundle B | 8,576 | 4,200,448 |
| Maximum-capacity reference B | 17,152 | 8,400,896 |
| Pool committed = peak B | 8,639 | 4,200,511 |
| Control metadata B | 6,208 | 6,208 |
| Configured staging data B | 2,147,483,648 | 2,147,483,648 |
| Pool committed after final trim B | 0 | 0 |

The 6,208 B metadata counter is 4,096 B mapped mailbox + 2,112 B Device
PoolState. It does not count Host map/deque, lease/event or service-thread object
overhead and is not a measurement of total Host control memory.

Torch additionally reports 4 MiB flags and a 1 MiB memory-domain guard; the peer
window is 2,151,677,952 B. Output reduction is **not total HBM reduction** and
does not remove staging, inputs, Torch intermediates or metadata. Ring blocked
free bytes are a subset of `freeBytes` unavailable until their prefix retires;
padding belongs to live/pending reservations. The dedicated pool fixture measures
864 B blocked and 160 B padding, then frees all segments. The retained-output SDK
fixture intentionally holds two overlapping lifetimes and reports each offset,
generation, padding and committed capacity in its JSON records; it must not be
compared with the single-output memory table as an equivalent retention pattern.

`Pending::Wait/materialize` still waits full data completion; metadata-only
materialization is not implemented. The version scope is CANN 9.2; other versions
are not promised. Inference-only, one owner per pool, no training/graph capture,
UDMA direct output or multi-host qualification are claimed.

### Final provenance and reproduction

Local evidence root:
`/workspace/TileXR-worktrees/elastic-memory/.elastic-memory-evidence/kernel-increment-20261109/`.
Remote root:
`/home/h30059441/elastic-memory-dev-20261106/kernel-increment-20261109/` on a5.
The ignored `provenance.json` pins source commit
**`d847c22956966e4e0805981ba5039fad894e047b`** on `codex/elastic-memory`,
[PR #212](https://github.com/LingquLab/TileXR/pull/212). `source-sha256.txt`
(SHA-256 `3b8bad6af21fbc44867af2cbecf154a6c1862a413769df9515a7fae0eddbccd8`)
and `verified-source-verification.log` verify the same 422 first-party build/test files
against the deployed source tree. `verified-provenance.log` and
`verified-torch-loaded-provenance.log` identify the actual build and installed libraries.
Install RPATH rewriting explains build/install Host-library hash differences.
The unchanged Torch extension is dynamically linked to the updated installed
native libraries/kernel package. The remote-root `verified-binaries.tar.gz`
preserves final OFF binaries and CMakeCache; its SHA-256 is
`579cbdfae5e4aee2f813bde64330ef5f1e64a36d9a68bc19906df114f019a513`.
STAGES run folders pin their instrumented binary hashes separately; they are not the OFF performance binaries.

Artifact paths below are relative to
`/home/h30059441/elastic-memory-dev-20261106/`, unless absolute:

| Final artifact | SHA-256 |
|---|---|
| `build/tests/elastic_memory/test_elastic_compact_ep` | `b9ebc52af641122e8caeb57ead3d495c33a214e1faf508abf8d3b99bace43ace` |
| `build/src/elastic_memory/libtilexr-elastic-memory.so` | `c6bf2d9a109a6e81e29aa1c5eb8965fb362044cae46f5a379ca535bded659ee2` |
| `install/lib64/libtilexr-elastic-memory.so` | `48a5944d5d8a53f482962d27cd5bc3c7e739f21d40177dff399808210eedf338` |
| `build/src/ep/libtilexr-ep.so` | `833d9d02dcf48456af911bf09bff61476052f770d5f6664493a0beda205e5d69` |
| `install/lib64/libtilexr-ep.so` | `b2b3ce3cd682c4d3c02b13af0f5ed6b25b29c3191d765182df1ca0dba7b0f820` |
| `install/lib64/libtile-comm.so` | `ada88ea138ed62612cf7906c85692a9d2cd5f0de142eeab17fda7106a5ebd351` |
| `install/lib64/libtilexr-diagnostics.so` | `7385814a3660f5e05cdbf992216401bbe99ebecdcd3bf7a5b8c756adddda6dc5` |
| `install/lib64/tilexr/kernels/tilexr_ep_dispatch_compact_kernel.o` | `e56a432d4fb21774f613d3641cba79030d73610d7056ea5ef7890727c18ceaa2` |
| `install/lib64/tilexr/kernels/tilexr_ep_dispatch_memory_kernel.o` | `d38560c3e1f5df38d6eb5ceb7f49446f623469a6ed06149a22239dab9353528f` |
| `install/lib64/tilexr/kernels/tilexr_ep_combine_memory_kernel.o` | `e77def8099fc6014327f31cb037060353489d5b77e3eb7d23aa7ac7392a7cc7b` |
| `build/tests/elastic_memory/elastic_pool_kernel.o` | `261d31e4b043468921d2bc2149145c9b06d344e796c32ed25527101e2ced6fdc` |
| `source/integrations/elastic_memory_torch/tilexr_elastic/_C.cpython-312-aarch64-linux-gnu.so` | `cc8f7e5452330909a44d7712561162c2c9a0593db04c607adf6bc0e5dc305c7d` |
| `/home/pkg/b070/cann-9.2.0/aarch64-linux/lib64/libascendcl.so` | `1c06b9c4a8910cfb585eab4ed0066bc09e28a0cf273dfcb15458f7cc49a78cea` |
| `/home/pkg/b070/cann-9.2.0/aarch64-linux/lib64/libruntime.so` | `945f49b024b192412d5021bc12a47c9a115064059e62940f9fc8c0f7342e82c5` |
| `/home/pkg/b070/cann-9.2.0/aarch64-linux/lib64/libruntime_common.so` | `6688f0dfaa0e02965896384ae2f30f962057fab6ad79a3c7d36ad95e4bf04597` |
| `/usr/local/Ascend/driver/lib64/driver/libascend_hal.so` | `ab759e7165f18ca68640583585650327b9ec34893fd611e9af3820b44d213450` |

`delivery-off-*` holds each final paired gate, JSON correctness/memory logs and
raw msprof CSV. `verified-summary.json` / `verified-joined-samples.json` are the
main audit; `independent-verified-off-audit/` reparses original CSV/logs.
`stages-verified-*` holds raw SDK records, mapping and per-capture official reports;
`independent-verified-stage-audit/` independently checks every lane.
`regressions-verified/` contains the post-fix functional results; earlier failing
and diagnostic cases remain under their original names. Source/ABI/CMake and
failure/lifetime ordering received independent review, followed by a final
main-agent diff review. No environment-only orchestration is committed.

Reproduction is performed by development; the design session is not asked to run
commands. Use the environment and build commands below, choose unused communication
ports, then run the fresh gate immediately before each paired launch. The retained
ignored `run-pair.sh` records the exact equivalent msprof command; its final calls
use `delivery-off-SHAPE-MODE MODE BS H 100 0`. Diagnostic calls use STAGES and
`stages-verified-SHAPE device_pool BS H 20 1`, with `kernel_ring_perf` for retained
ring calls. Set no NPU remap and no CPU affinity override. Host tests require no NPU.
Torch uses the existing `tests/test_npu.py` command with the updated transitions.

CANN 9.2 alone is the feature's validated version. Pending::Wait/materialize still
waits full data completion; metadata-only readiness remains unimplemented.
More ranks, multi-host, UDMA direct output, training/autograd and graph capture
are not established by these results.

## Environment and evidence roots

The November 9 results use `ssh a5`, host `rack08os2`, aarch64, without a compute
container. Native pool tests use physical NPU 0; paired EP/Torch tests use physical
NPUs 0 and 1, with identity mapping confirmed before each launch.

- Ascend950DT; driver `26.2.0.b020`, firmware `9.2.0.101.200`.
- Toolkit `/home/pkg/b070/cann-9.2.0`, compiler build `20260921_000125261`;
  bisheng reports clang 15.0.5 build 2026-09-18.
- CMake 3.31.10 at `/home/g00623548/test20261102/tools/cmake/bin/cmake`;
  host GCC 12, native C++14.
- Python `/home/l00984991/envs/py312_lzy/bin/python`, Torch `2.10.0+cpu`,
  torch_npu `2.10.0.post7`, CXX11 ABI enabled. Despite the Torch package suffix,
  actual tensors and kernels ran on NPU through torch_npu; no mock was used.
- Runtime resolves real driver libraries in `/usr/local/Ascend/driver/lib64/driver`
  and `/usr/local/Ascend/driver/lib64`, never toolkit `devlib` stubs.
- Source/build/install/log root: `/home/h30059441/elastic-memory-dev-20261106/`.
  Source is in `source/`, products in `build/` and `install/lib64/`.
- M0 experiment root: `/home/h30059441/elastic-memory-m0-20261106/`.
- Local evidence copy: `/workspace/TileXR-worktrees/elastic-memory/.elastic-memory-evidence/`.
  Environment-specific gate/probe scripts and raw logs are retained outside Git.

HCCL Test was **not rerun**. Existing records under
`/home/h30059441/unitest-pr199/logs/{hccl-attempt-started.txt,hccl-aiv-only.txt,hccl-exit-status.txt}`
and `unitest-8254c28b-a5-20261106/logs/qualification-reuse.json` identify the
November 3 all-eight-device AIV-only attempt. Only rank-0 correctness evidence
survives. It is not complete qualification of every rank or the current two-card
subset; that boundary remains explicit despite successful operator tests.

## Occupancy violation and repaired gate

At **2026-11-06 11:14:58 UTC (19:14:58 +08)**, the first mature pool run
(`pool-test1.log`) proceeded on device 0 although proc-mem already showed
external `VLLMWorker_DP` PID **2686943**, **85,155 MB**. The preflight observation
preceded our test in that log. The faulty gate relied on an empty `fuser` result
instead of rejecting the proc-mem occupancy. That PASS is **only a functional
observation with an occupancy violation**: excluded from formal acceptance,
performance and exclusivity evidence. No external task was killed or reset;
**impact on the external workload is unknown**. Absence of kill/reset does not
establish absence of interference. The exact external workload start/end and
performance effect were not measured.

Further launch was paused. The replacement gate refuses any target-card
external process, failed query, malformed/truncated output, or unconfirmed
logical/physical mapping. Only explicit `No process in device.` for every target
card passes; `fuser` cannot override it. It also rejects runtime device remapping.
Retained occupied output was replayed without launching NPU work:

| Fixture | Gate result |
|---|---|
| Retained VLLM proc-mem output | busy / refused |
| Nonzero query status | unknown / refused |
| NPU ID mismatch | unknown / refused |
| Truncated or empty output | unknown / refused |
| Explicit idle response | idle |

Evidence: local `idle-gate-fixtures.log`, `occupancy-raw.log`,
`mapping-and-incident.log`; remote `idle_gate.py`, each `*-preflight.json`.
Live all-device queries subsequently reported idle at 11:29 UTC. Formal tests
started only after fresh checks immediately before each launch: production pool
11:45 UTC; EP normal 11:47, zero/failure 11:50; Torch 11:56 and later final runs.
These are point-in-time availability checks, not a reservation system or a claim
that another user could not start work later.

## Resident-kernel growth gate (M0)

The retained M0 experiment establishes the running-kernel growth prerequisite;
current segmented-ring functional coverage is recorded above.

M0 `growth-volatile.log`: Device request 2,272 bytes; Host deliberately delayed
100 ms, then real HBM allocation took 26 us with `done_at_allocation=0`;
pre-allocation `side_effect=1`; every output word correct; stream sync 0.
`failure-injection.log`: Host failure 207001, Device exits with failure,
side effect remains 1, sync 0. The earlier failed poll is retained in `growth.log`:
the compiler hoisted `GetValue` outside the polling loop; volatile GM reload plus
DCCI fixed the observed hang. This is CANN 9.2 evidence only.

Read the retained originals without launching device work:

```bash
sed -n '1,180p' /home/h30059441/elastic-memory-m0-20261106/growth-volatile.log
sed -n '1,180p' /home/h30059441/elastic-memory-m0-20261106/failure-injection.log
sed -n '1,180p' /home/h30059441/elastic-memory-m0-20261106/growth.log
```

The current native pool and fault-injection commands below recheck real growth,
once-only side effects and failure propagation through the segmented-ring path.

## Reproduction commands

Choose genuinely idle devices first. Capture `npu-smi info -m` and
`npu-smi info -t proc-mem -i DEVICE` for **every** selected device immediately
before launch; stop if the query/mapping is uncertain or any external process
appears. Do not kill/reset another workload. The retained environment-specific
`idle_gate.py` implements these checks; it is not a repository deployment tool.

```bash
source /home/pkg/b070/cann-9.2.0/set_env.sh
export PATH=/home/g00623548/test20261102/tools/cmake/bin:$PATH
export TILEXR_SOC_NAME=Ascend950DT
cd /home/h30059441/elastic-memory-dev-20261106
cmake -S source -B build -DTILEXR_BUILD_ELASTIC_MEMORY=ON -DTILEXR_BUILD_EP=ON \
  -DTILEXR_BUILD_UNITEST=OFF -DBUILD_TESTING=ON -DTILEXR_BUILD_TESTS=OFF \
  -DTILEXR_EP_COMPACT_PROFILING=OFF -DTILEXR_EP_ENABLE_PROFILING=OFF \
  -DTILEXR_EP_COMBINE_MEMORY_ENABLE_PROFILING=OFF -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build -j8
cmake --install build
export LD_LIBRARY_PATH="$PWD/install/lib64:/usr/local/Ascend/driver/lib64/driver:/usr/local/Ascend/driver/lib64:$LD_LIBRARY_PATH"
ctest --test-dir build -R elastic_allocator --output-on-failure
python3 idle_gate.py 0 && timeout -k 5 90 build/tests/elastic_memory/test_elastic_pool \
  0 "$PWD/build/tests/elastic_memory/elastic_pool_kernel.o"
python3 idle_gate.py 0 && env TILEXR_TEST_GROWTH_FAULT_BYTES=1048593 \
  LD_PRELOAD="$PWD/build/tests/elastic_memory/libelastic_growth_fault.so" \
  timeout -k 5 90 build/tests/elastic_memory/test_elastic_growth_fault \
  0 "$PWD/build/tests/elastic_memory/elastic_pool_kernel.o"
```

For paired EP, set `TILEXR_COMM_ID=127.0.0.1:UNUSED_PORT`,
`TILEXR_ENABLE_UDMA=0`, and launch `test_elastic_compact_ep RANK DEVICE MODE`
concurrently after the **combined** idle check. Run `normal`, `zero` and `fail`
in separate pairs, with an outer bounded timeout; verify both exit codes and
both ranks' expected PASS output. The final Torch test includes owner-copy and
changing copy/count dimensions as well as the lifecycle cases listed above.

```bash
cd source/integrations/elastic_memory_torch
export TILEXR_INSTALL_PREFIX=/home/h30059441/elastic-memory-dev-20261106/install
MAX_JOBS=4 /home/l00984991/envs/py312_lzy/bin/python setup.py build_ext --inplace
export PYTHONPATH="$PWD"
# After a combined idle gate, run the following concurrently with rank/device 0 and 1:
/home/l00984991/envs/py312_lzy/bin/python tests/test_npu.py \
  --rank RANK --device DEVICE --comm-domain UNUSED_DOMAIN
# Repeat as a fresh paired call with --failure for one-rank budget failure.
```

### Device kernel profiling command

With the environment above, build `test_elastic_compact_ep`. For each fresh
process pair, select MODE from `maximum_capacity`, `host_actual_reference`,
`device_pool` and BS/H from `16/128`, `256/4096`, choose a fresh communication port,
and repeat the combined idle gate. The actual per-rank invocation was:

```bash
cmake --build build --target test_elastic_compact_ep -j8
export TILEXR_ENABLE_UDMA=0 TILEXR_COMM_ID=127.0.0.1:UNUSED_PORT
python3 idle_gate.py 0 1 || exit 1  # never reuse an earlier snapshot
# Run concurrently for RANK=0 and RANK=1, DEVICE equal to RANK:
timeout -k 5 240 msprof --output="$OUT/rank$RANK" --task-time=on --ai-core=on \
  --aic-mode=task-based --runtime-api=on \
  --application="$PWD/build/tests/elastic_memory/test_elastic_compact_ep $RANK $RANK kernel_perf $MODE $BS $H 20 100"
# Read-only reproduction of the retained analysis; no device execution:
python3 kernel-increment-20261109/summarize_delivery_off.py
```

Verify both process exit codes, both final PASS lines, each kernel's 120-row count,
the first-20 exclusion, unchanged growth counters and full-value oracles. Do not
substitute Host wall time or add task-wait time to the profiler task duration.

### Repository SDK capture command

The following is the historical pre-#216 reproduction. Current captures require
the prepared mapping fingerprint and completion/manifest sidecars described in
the standalone SDK migration section above. Use `TILEXR_EP_COMPACT_PROFILING=STAGES`
for diagnostic capture. The retained
`kernel-increment-20261109/run-pair.sh` supplies `TILEXR_ELASTIC_PROFILE_DIR` and
a shared capture seed to both ranks, binds the capture to the actual service
stream, and collects after completion. After a fresh combined gate, its paired
application commands are:

```bash
# RANK/DEVICE are 0/0 and 1/1. Use the same unused communication port for the pair.
build/tests/elastic_memory/test_elastic_compact_ep RANK DEVICE kernel_perf device_pool 16 128 20 20
build/tests/elastic_memory/test_elastic_compact_ep RANK DEVICE kernel_ring_perf device_pool 16 128 20 20
# Repeat as separate fresh-gated pairs for bs=256, h=4096.
```

Rebuild and install with `TILEXR_EP_COMPACT_PROFILING=OFF` before the complete-kernel
comparison. Diagnose with complete SDK lanes/records; only the OFF named msprof
Task Duration supports the final kernel comparison.

## Maintainer lessons

- Keep hot metadata proportional to the current operation. Preserve immutable
  Host snapshots and Device commit ownership, bound all reservations including
  retired records blocked by an earlier lease, and account for padding separately.
  Rebuild native and Torch consumers together when statistics or launch ABI changes.

- When moving a public Device header, update kernel build dependencies and install
  destinations as well as includes. Compile a consumer against a fresh install;
  an old prefix can hide a missed move by retaining obsolete headers.

- A compact allocator alone does not remove integration barriers or unnecessary count
  fanout. Prove the actual consumer set before reducing flags; skipped consumers must
  have no published flag to clear. Test both dimensions changing across completed
  rounds and the owner also performing copy. Size vector scratch for the complete
  repeat span, not only the bytes eventually copied to GM; tiny and non-eight-multiple
  blockDim expose overlaps that 64-core performance tests miss.

- A full Host oracle between stages can make the earlier rank wait inside the
  next communication kernel. Use the same pre-stage rank barrier in all modes,
  keep normal in-kernel waits, and derive latency from the named profiler task.
  Verify ordinal/sample counts and growth stability before computing statistics;
  separate cross-rank median statistics from a pooled sample P95.

- proc-mem is the occupancy gate; absence of file owners is not evidence of an
  idle NPU. Fail closed on parsing/query/mapping uncertainty and repeat directly
  before the actual launch. Preserve a violation and its unknown impact even
  after compliant runs succeed.
- A mapped mailbox does not make an ordinary accessor a polling load. Inspect
  generated instructions when a reload disappears; combine volatile access with
  the required Device cache invalidation and ordered publication. This fix was
  proven on CANN 9.2; do not infer compatibility with other versions.
- Publish growth failure before cleanup that might wait on Device. HBM success
  followed by Host bookkeeping failure must still unblock the resident kernel;
  the fault test covers this boundary without a production injection switch.
- Torch task queues may still contain uses after the Python Storage reference
  disappears. Flush registered submission queues on the native retirement worker,
  outside the pool mutex, then record/query completion events. Do not synchronously
  flush from a Storage deleter that may itself execute on the Torch queue worker.
- Keep runtime waits/malloc/free outside the statistics mutex. Account for an
  in-flight producer reservation so a snapshot cannot advertise its output as
  free or reclaimable. Segment size is a preference, not a reason to reject a
  smaller request that fits the remaining committed budget.
- Build all install targets before `cmake --install`; on this aarch64 system
  GNUInstallDirs selects `lib64`. Torch setup detects this and permits an explicit
  `TILEXR_INSTALL_LIBDIR` override. Device headers and their allocator dependency
  must be installed alongside the Host API.
