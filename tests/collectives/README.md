# TileXR Collectives Tests

This directory builds source/unit checks plus manual multi-process tools for `libtilexr-collectives`.
The API/library split is intentional: `tile-comm` owns communicator infrastructure, while `libtilexr-collectives`
owns `TileXRAllGather`, `TileXRAllToAll`, `TileXRAllReduce`, `TileXRReduceScatter`, `TileXRBroadcast`,
host launch, and CCE registration.

## Build

From the repository root:

```bash
source scripts/common_env.sh
cmake -S . -B build -DTILEXR_BUILD_COLLECTIVES=ON -DTILEXR_BUILD_TESTS=ON -DBUILD_TESTING=OFF
cmake --build build --target test_tilexr_collectives_correctness tilexr_collective_perf -j"$(nproc)"
```

CTest source and CLI smoke checks are hardware-free. They verify headers, split ownership, scripts, docs, and
tool wiring. Physical multi-NPU execution is manual.

Operator-specific profiling metadata is validated in this module. The A5
AllToAll mapping test parses the JSON through the production profiling loader;
fixed-slot capacity is covered separately by the A5 layout unit test. Do not
couple these checks to the profiled Add4 demo or enforce them by matching
production source text.

## Correctness Run

The correctness runner starts one process per rank through the helper script:

```bash
cd tests/collectives
./run_collectives_correctness.sh 2 16 0 ../../build/tests/collectives both
```

Arguments are `rank_size count first_npu bin_dir [op] [extra correctness args...]`. The binary is
`test_tilexr_collectives_correctness` and also accepts `--rank-size`, `--rank`, `--count`, `--first-npu`,
`--root`, and `--op allgather|alltoall|allreduce|reducescatter|broadcast|both`. It initializes ACL, selects
`first_npu + rank`, creates a stream, calls `TileXRCommInitRankLocal`, runs the selected INT32 collective,
synchronizes, copies results back, and validates deterministic rank-specific patterns. `both` runs the
original `TileXRAllGather` plus equal `TileXRAllToAll` checks. The initial equal `TileXRAllToAll` kernel path
is available only when the communicator reports `TOPO_910_93`; other multi-rank topologies are rejected
instead of launching a kernel path that would do no work.

Broadcast correctness should cover multiple root choices, for example:

```bash
./run_collectives_correctness.sh 2 16 0 ../../build/tests/collectives broadcast --root 0
./run_collectives_correctness.sh 2 16 0 ../../build/tests/collectives broadcast --root 1
```

The script writes `collectives_correctness_rank*.log` and tails logs on failure. Multi-rank launches use
`TILEXR_COLLECTIVES_RUN_TIMEOUT_SEC` as a whole-launch timeout, defaulting to 600 seconds. If any rank fails
or the timeout expires, the script kills remaining ranks and tails each rank log.

## Perf Run

`tilexr_collective_perf` is an nccl-tests-style correctness/performance tool:

```bash
cd tests/collectives
../../tools/collectives/run_collective_perf.sh 2 0 ../../build/tests/collectives \
  --op allgather --min-bytes 4 --max-bytes 4096 --step-factor 2 \
  --iters 20 --warmup-iters 5 --datatype int32 --check 1
```

Main options are `--op allgather|alltoall|allreduce|reducescatter|broadcast|noop`, `--min-bytes`, `--max-bytes`, `--step-factor`, `--iters`,
`--warmup-iters`, `--datatype int8|int16|int32|int64|fp16|fp32|bf16`, `--rank-size`, `--rank`,
`--first-npu`, `--check 0|1`, and `--csv <path>`. Optional thresholds `--min-algbw` and
`--max-latency-us` are available but are not set by default.

Use `--comm-mode shared-qp --comm-domain N` to initialize the shared-QP
communicator required by the A5 grouped AllToAll path. That path supports
uniform eight-rank nodes and global rank sizes divisible by eight from 8 through
128; include a 24-rank run when validating non-power-of-two scale support.

Use `--batch-timing 1` to enqueue all measured iterations after warmup and
synchronize the stream once at the end. The reported average is the batch Host
wall time divided by the iteration count. Batch timing is rejected when
`TILEXR_PROFILING_OUTPUT_DIR` enables profiling; because it has no
per-iteration samples, text and CSV output show `N/A` for `min(us)` and
`max(us)`.

Output fields:

```text
op dtype ranks bytes count iters algbw(GB/s) busbw(GB/s) avg(us) min(us) max(us) errors
```

The `bytes` column is the actual send bytes per rank for that operation and size row. Message-size semantics: allgather/allreduce/broadcast: count * dtype_size; alltoall/reducescatter: count * rank_size * dtype_size. `algbw(GB/s)` is
`output_bytes_per_rank / avg_us / 1000`, where `output_bytes_per_rank` is the bytes received by one rank for
the measured message size. `busbw(GB/s)` is `algbw * (rank_size - 1) / rank_size` for the measured operation.
CSV output uses the same fields.

## Collective Straggler Detection and Clock Synchronization

`tilexr_straggler.h` exposes the common diagnostics component used by Collectives,
EP, MoonEP, and future communication producers. Clock calibration is a separate
diagnostics-owned AICore operator. Each standalone collective kernel records its own
start, communication-ready, and completion cycles inside the existing launch; there
are no separate begin/end marker launches and Collectives does not own detector state.

The application lifecycle is:

1. Create one detector per communicator with `TileXRStragglerCreate`.
2. Call `TileXRClockSynchronize` collectively on every rank and retain the accuracy
   evidence.
3. Call supported communication operations. Producers resolve the detector from the
   communicator and reserve observations through `TileXRStragglerObservationBeginForComm`.
4. Call `TileXRStragglerCollect` collectively once for every observed communication,
   in submission order.
5. Recalibrate before `maxCalibrationAgeMs` and call `TileXRStragglerDestroy` after
   all producer streams and observations are drained.

Collection is FIFO, not "collect the most recent launch". Warmups issued after detector
creation are observations too; a test or application that skips their collection will
receive those earlier reports before a later measured launch.

The clock operator collects NTP-style device timestamps (`t0` through `t3`) using
`GetSystemCycle`, requires at least 75 percent and at least four valid samples, selects
the valid sample with minimum round-trip delay, and reports
the synchronization error bound as half of that delay. `precisionNs` is the maximum
per-rank error bound in the communication domain, not a wall-clock accuracy claim. A
terminal operator sample fails the whole calibration round; only non-terminal invalid
timing samples may be rejected under the validity threshold. Clock and collection calls
use a fixed control envelope so operation, sequence, launch ID, generation,
configuration, rank, and error status converge across the full communicator.

Build and run the complete four-NPU AllReduce acceptance test from the repository root:

```bash
source scripts/common_env.sh
cmake -S . -B build \
  -DTILEXR_BUILD_COLLECTIVES=ON -DTILEXR_BUILD_TESTS=ON -DBUILD_TESTING=ON
cmake --build build --target test_collective_straggler_e2e -j"$(nproc)"
export LD_LIBRARY_PATH="$PWD/build/src/collectives:$PWD/build/src/diagnostics:$PWD/build/src/comm:${LD_LIBRARY_PATH:-}"
bash tests/collectives/run_collective_straggler_e2e.sh \
  build/tests/collectives run/collective-straggler 0
```

The optional third script argument is the first physical NPU ID; four consecutive
NPUs are used. The test performs 20 clock rounds with 32 samples per rank, validates
small and large INT32 AllReduce output, requires no fast/slow false positives in the
normal runs, then injects a device-side delay into rank 1 and rank 2 in turn. The
hardware acceptance config uses a 1 ms absolute noise floor for the 5 ms injected
delay; this does not change the public 5 us default. Every
measured injected launch must classify only the injected rank as `SLOW` while
preserving correct AllReduce output.

The retained `summary.json` contains NPU IDs, p50/p95/max domain precision, last-round
offset/RTT/error bounds, and all scenario classifications. Per-rank clock and launch
JSON files plus `rank0.log` through `rank3.log` remain under the output directory.
Clock precision claims must name the exact hardware subset, CANN/driver versions,
sample count, and retained summary; Host tests or target compilation are not device
precision evidence.

Implementation pitfalls: keep every 64-bit direct-launch ABI field naturally aligned
and guard offsets with `static_assert`; an unaligned legacy device ABI can fail at the
kernel entry with a vector-core scalar-instruction exception. Changes to a CCE object
must also make the final AICore link and external-artifact staging rerun; a rebuilt
temporary object alone does not update the staged kernel artifact. Validate both conditions by a
clean target build and a repeated hardware clock run. The timing record must be
published after a device pipeline completion boundary, because a Host stream launch
return does not prove that collective MTE work or the record write has finished. Host
collection must synchronize the stream that produced each ring slot, not merely the
stream passed to the collection API. A failed clock-stream synchronization remains
owned by the detector and is retried by the next calibration or destroy call; do not
discard it or clear unrelated producer streams.
Likewise, a failed collection synchronization keeps the launch in its ring slot and
prevents reuse until a later collective collect call completes successfully.

For a synchronized collective, aligned completion and local duration alone may show
that the domain waited without identifying the rank that initiated the wait. The
in-kernel ready timestamp separates rank-local pre-communication time from the real
collective body. Reports retain `pre_communication_ns`, its robust score, and the
`PRE_COMMUNICATION` reason so a positive stall can be attributed without an auxiliary
marker operator.

The process exits nonzero if communicator destruction or subsequent ACL cleanup
fails, even when collective correctness and timing completed successfully.

`--check=1` validates outputs. INT32 is checked element-by-element with operation-specific expected values;
other dtypes use deterministic byte-pattern checks for allgather, alltoall, and broadcast. Checked
allreduce/reducescatter runs require `--datatype int32`; `--check=0` measures any supported dtype.

### Operator-Internal Profiling

Completion sidecar publication can fail when its target already exists. After
removing the conflicting target, a successful `Drain()` retry resolves the same
`completion_export` event while preserving its failure history. Check the latest
manifest revision when assessing health; the earlier revision records the failed
attempt. The mock capture queue check covers this retry, not NPU execution.

Collectives uses the [independent profiling SDK](../../tools/profiling/README.md).
Enable the build flag, set `TILEXR_PROFILING_OUTPUT_DIR` and distribute a fresh
`TILEXR_PROFILING_CAPTURE_ID` seed across ranks. The perf driver suppresses
profiling during warmup; the first prepared pool supports up to four sampled
kernel invocations, each with a separate UUID. Capture exhaustion is logged and
business execution continues. After communicator cleanup, analyze rank files
from one UUID directory with `python3 -m tilexr_profiling` and
`src/collectives/kernels/tilexr_collectives_coarse.mapping.json` for ordinary AIV,
or `src/collectives/kernels/tilexr_alltoall_a5.mapping.json` for grouped A5.
Never combine different capture directories into one report. Reported timer
conversion comes from the mapping; verify its frequency for the actual target.
Use that adjusted mapping's canonical fingerprint in
`TILEXR_PROFILING_COARSE_MAPPING_SHA256` / `TILEXR_PROFILING_A5_MAPPING_SHA256`
(shared fallback: `TILEXR_PROFILING_MAPPING_SHA256`). The producer binds actual
launch workload, host/device identity and execution metadata, and exports rank
manifest shards, completion and Host events at cleanup. Prepare expected ranks
and lanes before launch with `build_collective_expected`, then merge explicit
shards with `merge_manifest_shards`. Never derive the expected participant set
from successful output files: otherwise missing ranks disappear from coverage.
Use `collective_capture_id(seed, ordinal)` for ordinals 0 through 3 to predict
the shared queue capture ID before launch. Without a manifest, status 0 covers
only the supplied files. If mapping identity is unavailable, capture falls back
to records without producer metadata and makes no completeness assertion.

The producer's byte counts describe planned algorithm payload, not measured
physical traffic. Ordinary AIV input bytes are apportioned across lanes; grouped
A5 accounts for send/receive/self-copy payload and declares control or wait stages
not applicable. Profiling metadata is inspected manually when needed, without
producer round-trip tests or CI jobs. The optional queue mock protects capture
exhaustion, asynchronous resource lifetime and cleanup retry. CPU substitutes
do not prove ACL execution, NPU cache visibility or UDMA data-plane transfer.

## Skip Behavior

Manual scripts are strict by default. If `npu-smi info -l` or `TILEXR_AVAILABLE_NPUS` reports too few devices,
they exit nonzero. Set `TILEXR_SKIP_IF_INSUFFICIENT_NPUS=1` to skip cleanly instead:

```bash
TILEXR_SKIP_IF_INSUFFICIENT_NPUS=1 ./run_collectives_correctness.sh 8 16 0 ../../build/tests/collectives
```

Set `TILEXR_COLLECTIVES_RUN_TIMEOUT_SEC=N` to override the default 600-second launch timeout for either
manual script.

Common failure reasons include missing CANN environment, too few visible NPUs, devices already in use,
driver/runtime version mismatch, unsupported topology for the selected collective kernel, or `LD_LIBRARY_PATH`
not including the TileXR install/build library directories.

## vllm-ascend Shim Smoke on Remote NPU Host

The Phase 3 vllm-ascend shim validation keeps all remote state under a scratch directory and does not modify
the remote system Python or shell startup files. The TileXR collective path is opt-in: set
`VLLM_ASCEND_TILEXR_COLLECTIVES=1` to enable the adapter path used by the smoke script. Without that flag,
`TileXRVllmCollectivesAdapter` reports fallback so a vllm-ascend caller can keep its existing HCCL or
`torch.distributed` behavior.

```bash
TILEXR_VLLM_REMOTE=blue \
TILEXR_VLLM_REMOTE_BASE=/path/to/remote/tilexr_vllm_collectives_$(date +%Y%m%d_%H%M%S) \
bash tests/collectives/deploy_and_run_vllm_remote.sh
```

If the remote login shell does not already expose CANN Toolkit or the CCE compiler, pass the remote paths
explicitly:

```bash
TILEXR_VLLM_REMOTE_ASCEND_HOME_PATH=/path/to/remote/ascend-toolkit \
TILEXR_VLLM_REMOTE_ASCEND_DRIVER_PATH=/path/to/remote/ascend-driver \
TILEXR_VLLM_REMOTE_CMAKE_CCE_COMPILER=/path/to/remote/ascend-toolkit/bin/ccec \
bash tests/collectives/deploy_and_run_vllm_remote.sh
```

If the remote default Python does not provide `torch_npu`, select a Python environment explicitly. A direct Python
path takes precedence over a conda environment name:

```bash
TILEXR_VLLM_REMOTE_PYTHON=/path/to/remote/python \
bash tests/collectives/deploy_and_run_vllm_remote.sh
```

```bash
TILEXR_VLLM_REMOTE_CONDA_ENV=tt4 \
bash tests/collectives/deploy_and_run_vllm_remote.sh
```

Set `TILEXR_VLLM_REMOTE_CONDA_SH` when the remote conda activation script is not available at
`/home/miniconda3/etc/profile.d/conda.sh`.

Set `TILEXR_VLLM_REMOTE_PYTHONPATH` to prepend an isolated dependency directory or source checkout to the vLLM
environment probe and Python shim smoke without installing packages into the selected Python environment.

If vLLM and vllm-ascend are available as source trees rather than installed packages, pass their remote source
roots to make the environment probe add them to `PYTHONPATH`:

```bash
TILEXR_VLLM_REMOTE_VLLM_SOURCE=/path/to/remote/vllm \
TILEXR_VLLM_REMOTE_VLLM_ASCEND_SOURCE=/path/to/remote/vllm-ascend \
bash tests/collectives/deploy_and_run_vllm_remote.sh
```

Set `TILEXR_VLLM_REMOTE_DUMMY_MODEL` to a remote local HuggingFace config directory to additionally run a minimal
vllm-ascend inference probe with `skip_tokenizer_init=True` and `load_format="dummy"`. This avoids HuggingFace network
access and checks that the opt-in `sitecustomize` hook is loaded by the real vLLM engine and worker processes. The
probe prints `PASS TileXR vllm dummy inference probe` when `LLM.generate()` returns token ids. Set
`TILEXR_VLLM_REMOTE_VLLM_PLUGINS` only when the remote environment needs a plugin selection other than the default
`ascend`.
Set `TILEXR_VLLM_REMOTE_NNAL_ATB_SET_ENV` when the selected vllm-ascend worker path needs an NNAL/ATB environment
script to resolve `libatb.so`, for example `/usr/local/Ascend/nnal/atb/set_env.sh`.

The probe imports vLLM modules in child Python processes, including
`vllm.distributed.device_communicators.base_device_communicator` and
`vllm_ascend.distributed.device_communicators.npu_communicator`, so an import crash or missing dependency is
reported without aborting the TileXR shim validation. After CANN setup, the remote script also runs an
`NPUCommunicator` patch probe against the real vllm-ascend communicator class. That probe replaces the TileXR
adapter with a lightweight stub, verifies that `all_reduce` and `broadcast` route through the TileXR hook, verifies
that `all_gather` falls back when the adapter returns `None`, and prints TileXR route counts. This proves the
opt-in hook reaches the vllm-ascend communicator class; it still does not replace full vllm-ascend inference
validation with a model.

The script syncs the current TileXR commit, initializes submodules from local worktrees, dumps the NPU/CANN/Python
environment, probes whether `vllm` and `vllm_ascend` are importable before and after CANN setup, builds
`tile-comm` and `tilexr-collectives`, runs the standalone 2-rank AllGather correctness check, and runs Python
torch-npu shim smoke for `int32` and `fp16`.

Current shim coverage is:

- `allgather`
- `allreduce`
- `reducescatter`
- `broadcast`
- equal-split `alltoall` when the selected topology supports the standalone TileXR AllToAll path

The framework adapter owns a native communicator per CPU group. Unsupported
non-NPU/empty/dtype contracts and AllToAll use the original framework path;
non-contiguous supported inputs are materialized on the NPU. Missing native
artifacts fail group startup, and native operation failures propagate rather
than replaying through the original backend. Standalone shim smoke validates
the Python ABI and pointer path; it does not qualify framework group ownership.
See [the integration contract](../../docs/specs/vllm-ascend-collectives.md).

On an already qualified single-node 910B environment with vLLM and vllm-ascend
0.18.0 loaded, run the group integration check with the selected Python:

```bash
export PYTHONPATH="$PWD/integrations/vllm_ascend:$PYTHONPATH"
export VLLM_ASCEND_TILEXR_COLLECTIVES=1 TILEXR_INSTALL_PREFIX="$PWD/install"
export TILEXR_COMM_ID=127.0.0.1:29688
python -m torch.distributed.run --nproc_per_node=8 --master_port=29686 \
  tests/collectives/integration/vllm_group_npu.py
```

Optional `TILEXR_VALIDATION_OUTPUT` names an existing directory for per-rank
JSON results. The test requires all eight requested devices to be available.
It checks two overlapping TP8 groups, even/odd TP4 groups with local-root
broadcast, fp16/bf16 arithmetic, layout/axis semantics, cross-stream ordering,
and two creation/destruction cycles. Require eight `correct=true` records and
zero fallback/error for the four exercised operations. Its total route counts
are rank-local. It does not qualify AllToAll, multi-node, graph or fusion paths.

Expected success lines include:

```text
PASS TileXR vllm collectives smoke rank_size=2 op=allgather dtype=int32
PASS TileXR vllm collectives smoke rank_size=2 op=allgather dtype=fp16
PASS TileXR vllm collectives smoke rank_size=2 op=allreduce dtype=int32
PASS TileXR vllm collectives smoke rank_size=2 op=allreduce dtype=fp16
PASS TileXR vllm collectives smoke rank_size=2 op=reducescatter dtype=int32
PASS TileXR vllm collectives smoke rank_size=2 op=reducescatter dtype=fp16
PASS TileXR vllm collectives smoke rank_size=2 op=broadcast dtype=int32
PASS TileXR vllm collectives smoke rank_size=2 op=broadcast dtype=fp16
PASS TileXR vllm NPUCommunicator patch probe
TileXR vllm NPUCommunicator route counts: ...
PASS TileXR vllm dummy inference probe
```

If `torch` or `torch-npu` is missing in the selected Python environment, the preflight fails before the multi-rank
shim smoke. Missing or non-importable `vllm` / `vllm-ascend` is recorded in the environment dump and isolated import
probe but does not fail this shim phase. On `blue`, the older `tt4` environment records `libhccl.so` missing during
default pre-CANN vLLM import, then exposed missing `zmq` and incompatible torch versions for the available vLLM and
vllm-ascend source trees. The isolated `tilexr-vllm29` environment uses `torch==2.9.0`, `torch-npu==2.9.0`, and
`triton-ascend==3.2.0`; after CANN setup, its `vllm.distributed.device_communicators.base_device_communicator` and
`vllm_ascend.distributed.device_communicators.npu_communicator` probes import with `rc=0`, and the manual
`NPUCommunicator` patch probe printed route counts showing `all_reduce` and `broadcast` through TileXR plus
`all_gather` fallback. A manual `TILEXR_VLLM_REMOTE_DUMMY_MODEL=/home/<remote-user>/vllm-ascend/tests/ut/fake_weight`
probe reached the real vLLM V1 engine and spawned workers with `NPUCommunicator patched` printed in each process, then
stopped on a vllm-ascend worker import dependency (`torchvision`) before `LLM.generate()` completed. After installing
`torchvision` / `torchaudio` and sourcing NNAL ATB to resolve `libatb.so`, the same probe reached HCCL/Gloo
initialization and model weight loading, then stopped on a vLLM/vllm-ascend source mismatch:
`set_forward_context() got an unexpected keyword argument 'virtual_engine'`. That historical source mismatch was resolved in the retained 2026-10-09
qualification by using matching official vLLM/vllm-ascend 0.18.0 source trees,
an isolated triton-ascend 3.2.0 backend with the official CANN 9.x compatibility
patches, and a task-local toolkit path compatibility layout. Shared framework
and toolkit installations were not changed.

The resulting 910B3 x8 / CANN 9.1 / driver 25.5.0 eager qualification used real
Qwen3.5-4B weights with TP8. Three generation calls with two prompts per call
and 16 output tokens per prompt matched the original backend's token IDs.
Each of eight ranks recorded request deltas of 6,240 TileXR AllReduce and 96
AllGather calls, with zero fallback/error. Engine startup counts were excluded
from those deltas. The group integration check additionally passed two cycles
of overlapping TP8 and odd/even TP4 groups and the one-rank-missing-library
startup agreement check. All eight cards were idle after exit. These are
correctness/ownership results, not performance or graph/fusion qualification.
