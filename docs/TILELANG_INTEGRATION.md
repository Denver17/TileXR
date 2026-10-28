# TileLang integration and TileRelay

TileRelay has one production Host path: the optional C++14
`libtilexr-relay.so` and ABI-v1 Python-defined plans/executions. It does not
require collectives or a runtime compiler. AllGather-GEMM and GEMM-AllReduce
compose generated TileLang compute with TileXR peer-memory communication.

## Source layout and dependency boundaries

| Location | Responsibility |
| --- | --- |
| `src/relay/` | Generic Plan/execution lifecycle, resource binding and registered ABI-v1 launch; no operator math or compiler dependency |
| `src/include/tilexr_relay.h` | Public C ABI, descriptors and borrowed-resource contracts |
| `src/tilelang/python/tilexr_relay/{contracts,planning,runtime}.py` | Compiler/framework-free descriptors, immutable plans and ctypes execution |
| Python `build.py`, `device/`, `ops/` | Offline device compilation, canonical ABI/peer primitives and operator composition; `ops/plans.py` remains compiler-free |
| Python `adapters/` | Optional framework tensor bindings/workspace allocation; not transport ownership |
| `scripts/build_tilexr_relay.py` | Offline ABI-v1 builder CLI |
| `tests/relay/` | Host contracts, installed C/C++ consumers, shared SDK mocks and device qualification |
| `tools/relay/` | External historical-baseline adapter, v1 comparison runner and profiler analysis; no Host pipeline implementation |
| `3rdparty/tilelang-ascend/` | Pinned upstream compiler/templates; comparison-only TileScale stays in ignored `reference/` |

The Python package remains under `src/tilelang`; loading a Plan does not depend
on TileLang. Compiler imports stay in build/device/generator modules, and Torch
stays in the optional framework adapter. Keep timings, CPU references and trace
parsing outside production headers/libraries.

The pinned compiler emits `#include "tl_templates/ascend/common.h"`, whose
unconditional `shmem.h` inclusion requires its SHMEM include directories and
backend define at compile time. Relay communication uses TileXR peer mappings
and `device/peer.py`, not SHMEM calls or a linked SHMEM runtime. Removing those
include flags alone does not remove the upstream header dependency.

## ABI migration and historical baselines

The [ABI-v1-only decision](specs/2026-10-19-tilerelay-v1-only-design.md)
intentionally removes `TileXRAllGatherGemmRelay`, `tilexr_tilelang.h`,
`libtilexr-tilelang.so`, `TILEXR_BUILD_TILELANG` and the 96-byte ABI-v0 generator.
There is no compatibility adapter: rebuild old operators with
`scripts/build_tilexr_relay.py` and use the generic Plan API. Shape-named v0
ELFs cannot be loaded through the 152-byte v1 launch frame.

Both ABI-v0 and the Host callback pipeline are frozen on
`baseline/tilerelay-v0-pipeline-20261019`, at commit
`7ccaa16768ae2b2cb0da71d47e864352721de8b0`. The active tree has no pipeline
C++ implementation, public callback API, standalone helper build or dedicated
pipeline tests. The comparison runner only loads an explicitly supplied
historical pipeline DSO; it is never a production dependency or fallback.
The [benchmark guide](../tools/relay/README.md#frozen-baseline-build)
pins the exact commit and uses separate checkout/install directories.

Use a fresh current install prefix during migration. Installing over an old
prefix does not remove stale v0 DSOs/headers/kernels; do not infer the supported
API from leftover files or silently delete user installations. Historical
measurements/specs remain evidence for their recorded revisions, not claims
that removed paths are current or revalidated.

## Device-driven AllGather-GEMM

The v1 operator launches one registered mixed kernel per generation.
Persistent worker groups own row tiles and column workers execute independent
GEMMs. Both AIVs of the first column worker publish their half of the local rows,
pull peer halves and publish local readiness. Other columns wait for those
halves. AIC computes local input first, then waits for the paired AIVs before
remote GEMM. Per-wave ACK bounds cross-core credits; peer pull ACK protects
owner IPC staging across generations. Compute reads private gathered memory,
not owner staging that a peer could overwrite.

`gathered` and `output` are rank-major; the historical Host pipeline uses
tile-major storage. Operator planners supply buffer capacities/alignment and
workspace initialization requirements. Worker blocks cannot exceed physical
Cube capacity, complete column groups are required, and both ranks must agree
on the Plan. Do not run other communicator operations concurrently or free
buffers after a timeout.

CANN 9.1/A2 mode 2 requires both paired AIV arrivals, not a 950-only mode-4
assumption. Bound hardware signal credits with ACKs. Keep communication copies
contiguous and bounded independently of the GEMM K-step: narrow GEMM slices
for GM-to-GM traffic add strided DMA overhead. Publication/ACK planes must stay
at fixed addresses across shape changes. These constraints are unchanged by
removal of the old launcher.

Both Python generators share `device/gemm.py`: two L1 stages batch
`gcd(K, 256)` elements, and two L0 stages feed K64 MMA into one FP32
accumulator. The GCD keeps every supported K multiple of 64 tail-free (K=192
uses 64, K=384 uses 128). At the cap, A/B use 384 KiB L1, 32 KiB L0A and
64 KiB L0B; L0C remains 128 KiB. Initialize and drain credits once per worker,
including idle workers. Release L1 after its last L0 load, L0 after MMA, and
L0C after FIX; a copy being issued does not mean its consumer may reuse storage.
Remote AG input is prefetched only after its gathered-ready notification.
See the [pipeline design](specs/2026-10-27-tilerelay-pipelined-gemm-push-design.md)
for the resource and synchronization contract; performance acceptance is tracked
in the validation guide, not implied by double buffering.

The subsequent two-slot schedule permits next-wave production alongside
current-wave consumption: AIV produces gathered tiles for AG; AIC produces
FP32 partials for AR. Before wave `w >= 2`, its producer waits for consumption
of `w - 2`, then drains the final one or two releases before exit. Logical
slots 0/1 map to AIV-to-AIC flags 0/1 and AIC-to-AIV flags 2/3, with both AIV
participants. Full per-tile GM staging and peer ACKs remain unchanged; this is
not a two-buffer GM ring. The bound is per mixed worker, not across all AG
column workers. `Channel.max_credits=1` still describes each peer flag slot,
not the two local hardware-notification credits. ABI-v1, Plan capacities and
synchronous native launch are unchanged. See the
[lookahead evidence and limits](TILELANG_ALLGATHER_GEMM_VALIDATION.md#2026-10-26-two-slot-lookahead).

## TileRelay Core: Python-defined operators

`TILEXR_BUILD_RELAY=ON` builds the only production Relay Host library,
`libtilexr-relay.so`, independently of collectives. It owns typed Plan validation,
communicator binding, epoch allocation, external ELF registration and synchronous
launch. It contains no operator enum, GEMM shape formula, Python interpreter or
runtime compiler. The option is off by default; native code remains C++14.

```bash
git submodule update --init --recursive 3rdparty/spdlog
source scripts/common_env.sh
cmake -S . -B build-relay -DCMAKE_BUILD_TYPE=Release -DTILEXR_BUILD_RELAY=ON \
  -DTILEXR_BUILD_COLLECTIVES=OFF -DTILEXR_INSTALL_RELAY_PYTHON=ON \
  -DCMAKE_INSTALL_PREFIX="$PWD/install-relay"
cmake --build build-relay
cmake --install build-relay
```

Python installation is opt-in and copies the package/build script to
`share/tilexr/tilelang`; it installs no compiler/framework dependencies.
`contracts`, `planning` and `runtime` load without TileLang/TVM/Torch. The
`device`/operator generators explicitly require the pinned compiler environment;
the optional `adapters.torch` module validates tensors on the current NPU.

Build an operator in the pinned CANN/TileLang environment:

```bash
python scripts/build_tilexr_relay.py --operator allgather_gemm \
  --rows 256 --n 512 --k 128 --blocks 2 \
  --compiler-revision 87e6d1c2f0a9d2ff6a9013d9a1202325c4a619b1 \
  --output /path/to/artifacts
```

`--operator gemm_allreduce` selects the Python-only second operator, with
FP16 inputs, FP32 partial GEMM/exchange/SUM, then FP16 output. It is **GEMM then
AllReduce**, not input AllReduce then GEMM: each rank owns an equal K shard
and both receive `A_0 @ B_0 + A_1 @ B_1`. Cube writes owner peer memory;
Vector pulls the peer's FP32 partial and reduces it locally. Shared GEMM does
not change slots, peer flags or workspace requirements. The experimental push
layout was rejected on measured performance and is not a production option;
see the [acceptance results](TILELANG_ALLGATHER_GEMM_VALIDATION.md#2026-10-27-gemm-pipeline-and-fp32-push-acceptance-experiment).
Adding another supported fused operator
means implementing its Python math/layout/plan/generator and build registration,
plus tests/artifacts; it does not mean adding a native dispatch case. New ranks,
transports, tails or device ABIs still require a separate design/qualification.

Operator planners take only a `KernelArtifact`: `allgather_gemm_plan(artifact)`
or `gemm_allreduce_plan(artifact)`. They check the operator ID and derive validated
`rows/n/k/blocks` from its specialization. Do not pass a separate shape: a smaller
Plan paired with a larger compiled shape can pass generic capacity checks while
the ELF accesses beyond those buffers; a different operator changes slot meanings.
These checks belong in Python's operator layer, not the generic native runtime.
Host contract tests cover rejection and derived capacities; they do not verify
ELF contents or device execution. Build/installation must still keep metadata
consistent with the compiled ELF.

Communication artifacts also record an immutable `PeerMemoryLayout`: tile-slot
count and the owner/peer flag argument positions. `ops/plans.py` derives it from
the validated specialization before compilation; planner and generator consume
the same value. Publication begins at slot zero, ACK uses the second fixed 1 MiB
plane, and each slot is 64 bytes. The supported Python contract is one channel,
two ranks and one credit, not a general multi-channel allocator.

`RelayPlan` checks channels and flag bindings against this artifact layout on
construction and JSON loading. Merely checking declared channels for overlap
cannot establish that kernel accesses use those ranges: a changed declaration
used to leave the generated flag addresses unchanged. Nonzero starts, altered
counts, extra/missing channels or shifted/swapped flag arguments now fail before
native Create. Operator entrypoints additionally check the layout against their
shape and canonical argument positions. Existing communicating JSON manifests
without layout metadata must be regenerated with the updated builder; ABI echo
and other no-communication plans may omit it. The device/native ABI stays v1.
This is metadata consistency, not ELF authentication: build/install remain
trusted, and direct native callers still own descriptor/kernel correspondence.

The builder emits a device-only content-addressed
`tilexr_relay_<ELF_SHA256>.o` and a typed Plan `.json`. Configure
`TILEXR_RELAY_KERNEL_DIR=/path/to/artifacts` and install the
`TileRelayKernels` component to place ELF/manifest pairs below the owning DSO's
`lib[64]/tilexr/kernels`. Build and installation are the trust boundary: the
runtime uses the filename as the artifact identity and does not rehash ELF bytes.
Installed files must remain immutable; publish changed code under a new filename.
Relay does not independently verify file contents against their names.
Keep file I/O/size checks in the common loader and device-memory/ABI checks at
public API boundaries; callers should rely on successful internal operations'
contracts rather than repeat their checks. Measurement-only artifact hashes
remain useful provenance, not production launch requirements.

The C API in `tilexr_relay.h` deep-copies versioned descriptors. A Plan is
immutable, with concrete byte requirements and sixteen finite pointer bindings;
it owns no tensor, communicator, stream or epoch. The canonical mixed signature
is sixteen pointers, int64 epoch, int32 rank, int32 rank count, final compiler
FFTS uint64: a 152-byte launch frame. Every formal slot, including unused null
slots, must survive code generation. `device/abi.py` emits this entrypoint for
all operators; `device/peer.py` supplies inlined DMA publication/polling/ACK and
paired handoff with caller-owned scratch, not a device interpreter or per-tile
Host launches. `tests/relay/abi_echo.py` + registered hardware probe verifies
unused slot retention, subsequent pointers/scalars and real mixed handoff.

Operator macros declare meaningful tensor arguments and only the scalars they
use: `canonical_main(body, specs, scalars=("epoch", "rank"))` for AllGather-GEMM,
or `scalars=("epoch",)` for GEMM-AllReduce. Specs describe the used leading
pointer slots in Plan order. The ABI helper pads the remaining slots and
forwards arguments at Python build time; operators need no sixteen-argument
adapter macro. Keep the explicit sixteen-pointer entrypoint in `device/abi.py`:
removing unused formals would change the native launch frame. Changes here need
fresh compiler output and the existing ABI echo/operator hardware probes, not
source-text checks or Python-only mocks.

Typical framework use after communicator initialization:

```python
from pathlib import Path
from tilexr_relay.planning import RelayPlan
from tilexr_relay.runtime import RelayRuntime
from tilexr_relay.adapters.torch import bind_tensors, allocate_workspace

description = RelayPlan.from_json(Path(manifest_path).read_text())
runtime = RelayRuntime(library_path)
workspace = allocate_workspace(description)  # initialize once, outside timing
bindings = bind_tensors(description, [a, b, gathered, output])  # AG-GEMM order
with runtime.create_plan(description) as plan:
    prepared = plan.prepare_bindings(bindings, workspace.data_ptr(), workspace.numel())
    with plan.prepare_execution(comm_ptr, prepared) as execution:
        execution.launch(stream.npu_stream)
        # Update input contents in-place, synchronize those writes, then reuse:
        execution.launch(stream.npu_stream)
```

Keep tensor/workspace/library/communicator ownership until synchronous completion.
Both ranks must agree on Plan identity and generation order; no cross-rank plan
negotiation or communicator concurrency is promised. On a post-launch error or
timeout, terminate the process before device storage reuse/free. The Python
adapter conservatively poisons on any native launch error because the C return
code alone does not expose failure stage. Destroy does not make borrowed memory
safe after a timeout and context-manager cleanup must preserve the first error.

For repeated launches, immutable `PreparedBindings` validates/packs static
addresses, capacities and layout tags once at setup. It belongs to one live
RuntimePlan and owns only the Host ctypes array, not device storage; it captures
no communicator, stream or epoch. Each launch still validates those call-time
resources and executes the full native memory/capability checks. The existing
one-shot `launch` remains available. Do not hide repeated Python descriptor
packing in the hot loop or remove native validation to recover its cost.

For stable addresses, `prepare_execution(comm, prepared)` additionally performs
the complete native binding/capability validation and external-code registration
once. The returned `RuntimeExecution` owns a Host launch frame, not device memory.
Its synchronous `launch(stream)` checks current device/context and communicator
rank/mappings/capacity, obtains fresh FFTS state and a new communicator epoch, and
launches one registered kernel. It caches neither stream nor epoch and never
silently rebinds. The C equivalents are `TileXRRelayExecutionCreate`,
`TileXRRelayExecutionLaunch` and `TileXRRelayExecutionDestroy`.

Keep allocations, communicator and context alive and address-stable until
execution destruction, not just until the last call returns. Input contents can
change between calls; new addresses require a new execution. Pointer checks do
not detect freed/reallocated memory or reused communicator addresses. Context
reset/destruction while executions are live is unsupported. Parent and sibling
executions share serialization and poison state; destroying the parent invalidates
all child launches, but their Host handles remain safely destroyable. There is
no automatic teardown or protection for concurrent use of separate plans sharing
a communicator. Python loads the new symbols only on preparation, so existing
launch APIs continue to work with older native libraries.

Performance provenance must include Host optimization flags as well as the
TileLang compiler/artifact identity. An empty CMake build type is not a Release
build: controlled Host-only tests found that it materially increases binding and
registration overhead without changing the NPU ELF. Compare old/new paths in one
Release library before attributing a gain to preparation. Do not subtract absolute
Host/device trace timestamps to estimate Host phases; profiler clock alignment can
place device completion outside a synchronous Host marker. Use same-clock Host
instrumentation when phase attribution is required.

An activated venv does not guarantee that a global `torchrun` executable uses
its interpreter. Use `python -m torch.distributed.run ... -- script.py ...`
and retain `sys.executable`/framework/compiler versions when compiling and
benchmarking; import failures before device setup are not kernel failures.

Publication/ACK planes stay fixed at the first/second 1 MiB of peer memory;
payload begins after 2 MiB. Builder-assigned channels are disjoint. Do not move
planes with shape changes or reset shared flags per generation. Reuse private
workspace only with compatible layout/initialization; otherwise allocate a
separate zero-once workspace. Generalization initially kept one outstanding
wave; the subsequent two-slot local schedule above retains full staging and
the same fixed peer-memory contract.

Reusable Host lessons: use a private function address as the binary-loader DSO
anchor, since exported-symbol interposition can resolve a different owner.
Registration identity includes ACL context/device, not only filename/shape.
Allocate the cache node before runtime registration so a later allocation
failure cannot release a live function stub. Keep descriptor arrays/strings
alive until native Create has deep-copied them. These are Host ownership checks,
not proof of device protocol correctness.

Focused checks:

```bash
cmake -S tests/relay -B build/relay-host
cmake --build build/relay-host
ctest --test-dir build/relay-host --output-on-failure
python3 -m unittest discover -s tests/relay -p 'test_*.py'
```

`scripts/ci/host_checks.sh` includes these compiler-free Python contracts,
native mocks and trace-analysis tests in its maintained `relay-host` case.
Configure installed C/C++ consumers separately against the fresh prefix.
Host CI performs no NPU launch.

Two-rank device qualification uses `tests/relay/hardware.py` under torchrun with
`TASK_QUEUE_ENABLE=0` and an outer timeout. Select `--probe abi`, `ag`, `ar` or
`mixed`; the legacy probe is removed. It covers changed inputs, delayed
ranks, shape/operator alternation without per-generation Host barriers, exact
gathered data and one mixed main launch per invocation. A constructed FP32
partial case deliberately distinguishes premature FP16 rounding; loose random
GEMM tolerances alone would miss it. Host/simulator checks do not prove NPU
ordering, UDMA or multi-node support.
Add `--execution` to run the same probes through the native prepared path,
including borrowed-descriptor release and parent-before-child teardown checked
through both the native and Python interfaces. Each prepared execution is launched
twice, on different streams, with the profiler checking both launches. Without it,
probes alternate one-shot and prepared-binding calls. Both paths alternate
streams. For complete matrix qualification, choose enough rounds to execute
every loaded Plan, then repeat for reuse coverage. Short probes only
validate the generations they execute; `plan_identities` lists the agreed plan
set, not proof that all of it ran.

Keep normal-path coverage in these E2Es rather than duplicating the ABI, layout,
shape reuse and lifecycle in mocks or separate experimental probes. Retain Host
tests for malformed contracts, overflow/aliasing, registration failures,
allocation failures after registration, timeout cleanup and shared poison: a
successful NPU run cannot demonstrate these rejection and ownership boundaries.
The C/C++ installed-consumer checks share a source but must compile as separate
languages. Trace-parser rejection tests protect the evidence checker itself.

For warmed comparison, supply `--baseline-lib`, `--generic-manifest` and
`--generic-runtime` to `tools/relay/compare_relay.py`. The three historical
pipeline controls are serial, serial-tiled and Host overlap; current modes are
generic Relay and optional prepared execution. The archived specialized Relay
is not a current comparison mode. Use at least `--warmup 5 --iterations 20`,
preserve profiling/timing boundaries and keep raw rank samples.
`--reverse-mode-order` supports order-sensitivity checks; near ties are not
stable speedups. `--prepared-execution` adds the native prepared path alongside
prepared bindings in the same DSO; `--interleave-seed N` shuffles modes
deterministically within each round. Repeat with another seed and record
`--host-build-flags` for the compared libraries. Setup/preparation and input
copies remain outside the timed interval. See the
[benchmark guide](../tools/relay/README.md) for the frozen checkout/build
and invocation, and the [validation record](TILELANG_ALLGATHER_GEMM_VALIDATION.md)
for historical results and current qualification.
