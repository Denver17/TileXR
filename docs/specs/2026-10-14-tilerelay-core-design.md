# TileRelay Core: stable runtime and Python-defined fused operators

Superseded packaging/compatibility requirements: the approved
[ABI-v1-only design](2026-10-19-tilerelay-v1-only-design.md) archives ABI-v0 and
the Host pipeline at `7ccaa16768ae2b2cb0da71d47e864352721de8b0` on
`baseline/tilerelay-v0-pipeline-20261019` and removes their implementations from
the active tree. ABI-v1 contracts remain unchanged; preservation requirements
below are historical. See the [current integration guide](../TILELANG_INTEGRATION.md).

## Status and objective

The user approved the concise incremental design on 2026-10-14, including
single responsibilities, dependency inversion, explicit contracts and lifecycle
ownership. The user confirmed this written spec on 2026-10-14. This increment
is implemented and qualified within the scope below; concrete evidence and
remaining limits are recorded in the
[validation guide](../TILELANG_ALLGATHER_GEMM_VALIDATION.md#tilerelay-core-qualification-2026-10-14).

Within a supported device ABI, transport and target, a new fused operator must
be implementable by adding Python operator/generator code, generated artifacts
and tests, without changing the C++ runtime or adding a native operator enum.
The concrete acceptance witness is AllGather-GEMM followed by GEMM-AllReduce
using the same installed generic runtime binary.

This extends, rather than replaces, the
[original TileRelay design](2026-10-14-tilerelay-design.md). Existing public APIs
and their buffer, synchronous-return and failure contracts remain supported.

## Scope and non-goals

- First target: two same-node 910B ranks, CANN 9.1, Host C++14, pinned
  `tilelang-ascend`, external pure AICore binaries and one mixed main launch per
  rank/generation. Worker groups persist for that launch, not process lifetime.
- Preserve optional/default-OFF builds, the original Host pipeline and the
  specialized Relay interface. No implicit compiler invocation at runtime.
- Initially preserve full staging and one outstanding wave per synchronization
  channel. Do not combine generalization with lookahead, ring-buffer or polling
  performance changes. The separately qualified two-slot extension below changes
  local AIC/AIV scheduling only, after this initial baseline.
- No asynchronous public execution, concurrent operations on one communicator,
  arbitrary ranks, UDMA, multi-node, new hardware, tail handling or arbitrary
  device function signatures in this increment.
- TileScale remains ignored comparison-only source under `reference/`.
  Active builds/generators must not include or link it.

## Design principles and module ownership

| Module | Responsibility | Excluded responsibility |
| --- | --- | --- |
| Native Relay runtime | Capability/binding validation, communicator resource binding, epoch allocation, artifact registration, launch and completion | Operator mathematics, shape-specific workspace formulas, Python/TVM/PyTorch execution |
| Python contracts and Plan Builder | Immutable specialization, typed bindings, checked workspace/peer layouts, logical synchronization channels | Live device allocations, streams, mutable generations |
| Python artifact builder | Canonical ABI emission, target compilation/linking, manifest and content identity | Runtime compilation, implicit dependency installation |
| Python device primitives | Ordered peer copy, publication, DMA polling, peer-read ACK and Cube/Vector handoff | GEMM semantics, communicator initialization, device memory allocation |
| Python operators | Data partition, mathematical semantics, computation/communication composition and schedule selection | Binary registration, transport ownership, raw hardware flag allocation |
| Framework adapter | Tensor validation, binding conversion, workspace allocation/initialization and stream selection | Core transport/protocol implementation |
| Tests/examples | Reference results, traces, diagnostics, timing and provenance | Production timing types or hidden execution fallback |

Dependency direction is one-way: operators depend on contracts and primitives;
the artifact builder depends on contracts and the pinned compiler; framework
adapters depend on the public runtime/binding contracts. The native runtime does
not import operators or depend on a framework. Loading an already-built plan
must not import TileLang/TVM or require the compiler environment.

Suggested placement: native code in `src/relay`, public `tilexr_relay.h`, and a
cohesive Python package under `src/tilelang/python/tilexr_relay/` with
`contracts`, `planning`, `build`, `device`, `ops`, `runtime` and optional
`adapters`. The existing `src/tilelang` Host module remains a compatibility
consumer. Public Python entrypoints use lazy imports to separate build-time
compiler dependencies from runtime-only consumption.

Use Builder for validated plans, compile-time Strategy for scheduling/transport,
Adapter for legacy interfaces, and immutable value objects for specifications.
RAII applies to Host ownership and safe cleanup; it must not free storage or
unregister code still referenced by incomplete device work. Define narrow
backend interfaces, but implement only the existing peer-memory/910B path now.
Do not introduce device virtual calls, a general graph interpreter, or empty
future transport implementations merely to demonstrate a pattern.

## Domain model

### KernelArtifact

An immutable identity containing operator ID, specialization, target/backend,
compiler provenance, ABI version, communication protocol version, measurement
mode, entry symbol and actual ELF digest. A canonical manifest records these
values and the plan requirements. Identity includes all code-generation-affecting
options; shape alone is not sufficient.

Generated ELF filenames are content-addressed, safe basenames under the owning
library's `tilexr/kernels` directory. A new operator or diagnostic variant must
not overwrite an existing registered filename. SHA256 is computed at build time
for naming, not rechecked during native registration. Build/installation are
trusted and installed artifacts are immutable; runtime cache identity uses the
filename, entry and device/context. Relay does not detect contents inconsistent
with their filename or establish trust in third-party executable code.

### RelayPlan

An immutable, validated description of the artifact, concrete binding
requirements, workspace regions, peer payload regions, launch geometry and
logical channel assignments. It contains no tensor addresses, communicator,
stream, allocation ownership or mutable epoch.

Required sizes/offsets are concrete checked integers, not expressions evaluated
by C++. The Builder rejects overflow, invalid alignment, overlapping live
regions, unavailable synchronization resources and inconsistent participants.
Operator semantic/shape rules remain in Python; native validation independently
checks the concrete memory/capability contract on every launch.

### LaunchBindings and execution state

Bindings supply caller-owned addresses, byte capacities and dtype/layout tags
for this invocation, plus workspace, communicator and stream. The framework
adapter derives these from actual tensors. Raw C callers remain responsible for
truthful allocation capacities and the current device/context.

Runtime state is separate from the immutable plan: an invocation progresses
from validation to running and then completed or failed/poisoned. Prelaunch
rejection performs no launch. An error after launch must not be treated as safe
workspace reuse; a completion timeout requires process termination before
storage release, matching the current contract.

## Native interface and standard device ABI

Proposed public operations are `TileXRRelayPlanCreate`,
`TileXRRelayPlanGetWorkspaceSize`, `TileXRRelayLaunch` and
`TileXRRelayPlanDestroy`. They expose a C ABI usable from C++14 and ctypes.
Public descriptors carry structure-size/version fields; creation deep-copies
borrowed descriptor data. JSON is decoded by the Python facade or another
caller into typed descriptors; the Host library does not gain a general JSON
or expression interpreter.

`Launch` receives a plan, communicator, user bindings, workspace pointer/bytes
and stream. It validates all bindings/capacities/alignment and current target
requirements before launch, supplies peer mappings, obtains one fresh
`TileXRCommNextMagic` epoch, launches once and synchronizes with a bounded
timeout. It returns ordinary TileXR/runtime errors, not timing information.
Concurrent communicator operations remain unsupported, including calls through
other libraries; no cross-library concurrency guarantee is implied.

The proposed device ABI v1 is a fixed flat signature with sixteen pointer
slots followed by `int64 epoch`, `int32 rank`, `int32 rank_count`, and the
compiler-required final `uint64 ffts` argument. On the supported 64-bit Host
ABI its packed launch frame is 152 bytes:

| Field | Byte offset | Size |
| --- | ---: | ---: |
| Pointer slots 0..15 | 0 | 128 |
| Epoch | 128 | 8 |
| Rank | 136 | 4 |
| Rank count | 140 | 4 |
| FFTS address | 144 | 8 |

Each slot is described as one of a finite set of sources: user binding,
workspace region, owner/peer payload region, owner/peer flag region, or unused.
The runtime performs only these generic bindings; slot names and operator IDs
do not select native business logic. Scalar operator inputs can use typed
buffer bindings; specialization constants stay compile-time. Unused slots are
null and must never be accessed.

Tensor shape/dtype annotations vary by specialization while pointer/scalar
order stays fixed. The generator must retain every formal slot and emit the
canonical entrypoint without rewriting generated C++ text. This is a target
compile/hardware acceptance gate, not a presumed compiler capability. Verify
all offsets, unused-slot retention and FFTS placement with an argument-echo
kernel and native mocks before migration. If the pinned compiler cannot
support this ABI, stop and revise the ABI design rather than changing the
compiler or silently admitting arbitrary signatures.

The current specialized 96-byte ABI and shape-named binaries remain distinct;
they must never be registered/launched as ABI v1.

## Memory and synchronization contracts

The communicator remains the owner of peer mappings/allocation. A plan borrows
validated regions; it never registers a remote mapping or creates another
communication runtime. Required payload bytes derive from the declared
operation: A tiles for AllGather-GEMM, partial-output tiles for GEMM-AllReduce.
Native checks use declared byte requirements, not GEMM dimension formulas.

Publication and peer-read ACK remain in fixed first/second 1 MiB flag planes,
with payload after `IPC_DATA_OFFSET`. Logical channels receive disjoint,
aligned slot ranges within the planes; changing a shape must not move a plane
or alias publication and ACK roles. Channel ownership, number of tile slots
and maximum live credits are explicit plan requirements. Exhausted capacity
is an error, not a fallback to smaller fictitious storage.

Private workspace regions specify alignment, size, lifetime and one-time
initialization requirements. Workspace is allocated/initialized outside the
steady-state launch. Ready state is retained across completed invocations;
shared flags are not cleared each generation. Workspace reuse across different
plans requires compatible initialization/layout or a new workspace. Ownership
and old-generation peer-read ACKs remain valid through plan/shape changes.

Device primitives express obligations, not raw stores:

- Publish only after referenced payload writes have completed.
- Poll remote readiness through the verified DMA-to-UB path, not cached scalar
  GM reads. Match the current epoch before consuming data.
- Publish peer-read ACK only after the remote payload read has completed and
  the peer publication for that generation has been observed.
- Release/reuse producer storage only after all relevant peer-read and local
  consumer completion obligations have been satisfied.
- AIC/AIV handoff accounts for both paired AIVs on A2 mode 2. All participants
  in an active group execute matching signal/wait counts.
- Keep outstanding credits bounded; the initial strategy allows one wave.

Macros receive explicit scratch buffers and Builder-assigned logical channels.
They do not allocate a fresh hidden UB buffer at every call or expose flag IDs
to operator code. Backend lowering owns hardware event/flag assignment and its
coexistence with compiler automatic synchronization. No launch-wide barrier is
introduced to coordinate inactive worker groups.

Both ranks must execute matching plans and generation order. Initial native
runtime does not promise to discover arbitrary cross-rank plan mismatches;
the caller must agree on artifact/plan identity, and tests verify it before
launch. Outer process-group deadlines bound peer failures/mismatches. Adding
a production cross-rank negotiation protocol is a separate design decision.

## Operator contract and extensibility witness

An operator supplies its mathematical definition, partition/layout, supported
dtype/shape rules, typed binding specifications, workspace/peer requirements,
schedule strategy and mixed-kernel composition. The common Builder/backend
supplies resource allocation, protocol lowering, canonical entrypoint and
artifact emission. Registration/discovery is Python-side; adding an operator
must not add a C++ dispatch case.

AllGather-GEMM retains `concat(A_0, A_1) @ B_rank`, FP16 inputs/output and FP32
accumulation, rank-major gathered/output layouts and the current local-first
schedule. Its numerical and synchronization semantics must not change during
generalization.

The second witness is explicitly **GEMM then AllReduce SUM**, not input
AllReduce then GEMM. Each rank owns an equal K shard `A_rank[M,K]` and
`B_rank[K,N]`. The output on both ranks is
`C = A_0 @ B_0 + A_1 @ B_1`, equivalent mathematically to the global K-sharded
GEMM. AIC produces FP32 partial tiles; AIVs exchange and sum FP32 partials,
then produce FP16 final output. There is no intermediate FP16 rounding of
partial GEMMs. Layouts and reference/tolerances are explicit operator contracts.

This reverses the primary handoff direction and changes communication payload
from input tiles to output tiles, making it a meaningful generality test.
Use bounded copy tiles and declared staging layouts; do not silently add a
separate layout-conversion/reduction kernel. FP32 exchange/reduction and final
stores require target compile and numerical proof before qualification.

## Two-slot lookahead pipeline

This user-approved extension is implemented and qualified on two 910B3 ranks
with the pinned toolchain. Its purpose is to overlap communication for one tile
with computation for an adjacent tile inside the existing single mixed kernel.
The [validation record](../TILELANG_ALLGATHER_GEMM_VALIDATION.md#2026-10-26-two-slot-lookahead)
covers 22 artifacts, tail/epoch regression and separate stage-overlap diagnostics.
Paired measurements show repeatable AR multi-wave gains, not a uniform speedup:
AG remains approximately unchanged, and small-shape differences are recorded.
It preserves the [artifact-bound fixed peer layout](2026-10-21-tilerelay-fixed-peer-layout-design.md)
and the current ABI-v1-only runtime. Full staging remains; this is a two-slot
notification protocol, not a two-buffer GM ring or a general task scheduler.

### Scope and ownership

- Target remains two same-node 910B ranks with CANN 9.1 and the pinned compiler.
  No extra kernel, stream, Host scheduling loop or asynchronous public API.
- `device/peer.py` owns the mapping of logical local slots to hardware flags and
  the four paired AIC/AIV handoff macros. Operators supply only logical slot
  0 or 1; raw flag IDs do not enter operator code.
- `ops/allgather_gemm.py` and `ops/gemm_allreduce.py` own tile traversal,
  producer/consumer ordering, local outstanding-work bounds and final draining.
  Use the existing macros with explicit slot arguments; do not introduce a
  scheduling class, runtime queue, configurable depth or alternate launcher.
- `ops/plans.py`, `contracts.py`, the canonical signature and native runtime do
  not change. Buffer capacities, binding slots, flag planes and workspace
  initialization remain the same. The new generated ELF has its own content
  identity; there is no second independently configurable schedule in the Plan.
- Preserve `gcd(K, 256)` L1 batches, `kL0Size=64`, FP32 accumulation/partials and
  the single final FP16 conversion. Do not combine this change with GEMM L1
  prefetch, AG communication-group reassignment, reduction-copy tuning or
  compressed peer staging.

### Distinguish local credits from peer channels

Each tile already has its own peer payload range and publication/ACK slot.
AG additionally has per-tile private gathered data and readiness. These remain
indexed by the full tile index, never by local slot parity. No tensor-sized
allocation is doubled, and existing UB/L1/L0C buffers remain single allocations.

The two local slots are hardware-notification credits within one AIC and its
paired AIVs. Each hardware flag is reused only after its previous notification
has been consumed and acknowledged. Two different tiles may be in flight, but
each peer flag slot still has one generation outstanding. Consequently
`Channel.max_credits` remains 1, `PeerMemoryLayout` is unchanged, and neither
the device ABI nor the peer protocol version changes.

The bound is per mixed worker, not a gang-wide or rank-wide two-tile bound.
An AG leader can advance ahead of slower column workers: full per-tile gathered
storage and retained readiness make that safe. Do not replace those allocations
with two physical buffers without a separate all-consumer lifetime design.

### Local handoff state machine

Let `w` be a worker's valid local wave ordinal and `s = w % 2`. The
backend assignment keeps the two directions disjoint:

| Macro pair | Producer pipe | Hardware IDs for logical slots 0 / 1 |
| --- | --- | --- |
| `vector_ready(s)` / `cube_wait_ready(s)` | AIV MTE3 | 0 / 1 |
| `cube_done(s)` / `vector_wait_done(s)` | AIC FIX | 2 / 3 |

Use A2 mode 2 with both paired AIV participants. Keep the existing required
MTE/scalar/FIX ordering; a notification instruction being issued is not proof
that its preceding asynchronous transfer has completed. Confirm these IDs do
not collide with compiler-generated cross-core synchronization in the actual
compiled artifact. Do not change third-party code to make the mapping work.

For each slot the logical lifetime is free, produced, consumed, released, then
free again. No initial hardware release signals are sent:

1. The producer may start waves 0 and 1 without a release wait. Before starting
   any wave `w >= 2`, it consumes the release for wave `w - 2` on slot `s`.
2. The producer completes the relevant data writes and sends exactly one
   handoff for the current tile. The consumer waits for that tile's slot before
   accessing dependent data, then completes consumption and sends its release.
3. After producing its last valid wave, the producer consumes the releases for
   local ordinals `max(0, L - 2)` through `L - 1`, where `L` is that worker's
   actual valid wave count. Earlier releases were consumed by step 1.
4. Thus every produced handoff and release has a matching wait, including at
   kernel exit. Both AIV halves must follow the same valid-wave/slot sequence.

This permits at most two produced-but-not-released tiles per mixed worker.
It must not rely on accumulating repeated signals on a single hardware flag.
Zero-work workers emit/wait nothing; one-wave workers drain exactly once.

For AG, `L` counts valid tiles `w * gangs + gang`; all column workers in a gang
share that count. For AR, it counts valid tiles `w * blocks + cid`. Do not use
the rounded launch-wide wave count to drain inactive/tail workers. Odd counts
and a partially occupied final wave are supported without a global barrier.

### AllGather-GEMM schedule

The paired AIVs are producers; their AIC is the consumer. Before communication
for wave `w >= 2`, each AIV waits for `cube_done(s)` from wave `w - 2`.
The first column worker then performs the existing publish/pull protocol and
publishes the peer-read ACK and private readiness after copying its half.
Other column workers wait on that tile's private readiness as before.

Each AIV sends `vector_ready(s)` but does not immediately wait for that same
wave's Cube completion. The leader still observes the peer ACK for this tile
before advancing to its next communication wave. The next tile can therefore
be communicated while the current tile is being computed. Finish by draining
the last one or two Cube completions on every active column worker.

The AIC retains local-first GEMM. It waits on `vector_ready(s)` before remote
GEMM and sends `cube_done(s)` only after completing all output columns assigned
to that worker for this row tile. Credit reuse must not precede those final
stores. The two half-row arrivals and all column-wave work remain required.

### GEMM-AllReduce schedule

The AIC is the producer; both AIVs are consumers. Before computing wave
`w >= 2`, the AIC waits for `vector_ready(s)` releasing wave `w - 2`. It computes
the current FP32 partial, copies it to its distinct owner payload tile and sends
`cube_done(s)`, then can compute the next tile without waiting for the current
reduction. L0C reuse still requires the preceding FIX read to finish; retain
the necessary local pipeline dependencies rather than treating notification
issuance or distinct GM destinations as sufficient protection for L0C.

Each AIV waits on `cube_done(s)`, publishes its half's peer readiness, observes
peer publication, reads and reduces both FP32 partials, and stores FP16 output.
It publishes the peer-read ACK and observes the peer ACK before sending
`vector_ready(s)`. AIC drains the final one or two releases before exit, so
reduction, output stores and peer-read obligations outlive neither the kernel
nor their borrowed buffers. No standalone reduction kernel is introduced.

### Lifetime, compatibility and failure invariants

Within a launch, tile GM ranges are disjoint and private readiness is retained
until all local consumers finish. Across launches, keep the fixed peer planes,
fresh `TileXRCommNextMagic` epoch, per-tile publication-before-ACK order and
completed peer-read handshake. Every tile's required peer ACK is observed
before exit; local notification slots are completely drained. Do not clear
shared flags or manufacture an extra Host barrier to hide reuse defects.

The native launch remains synchronous on return; alternating streams in tests
does not authorize simultaneous communicator operations. Both ranks must use
matching plans/artifacts and generation order. Existing ABI-v1 artifacts remain
loadable by the unchanged runtime, but mixed old/new artifacts between ranks
are not a supported validation configuration.

Keep existing error propagation, poisoned execution state and bounded launcher
timeouts. A timeout is not permission to reuse/free in-flight storage or silently
fall back to the old schedule. If compiler lowering, event availability or
progress constraints invalidate this schedule, stop and revise this design.

### Acceptance and evidence

- Compile both directions with the pinned CANN/TileLang toolchain. Check actual
  AIC/AIV lowering, notification pairing, automatic-sync interaction and local
  buffer reuse. Host arithmetic or source-text matching is not device proof.
- Use the existing mixed E2E rather than a new scheduler unit-test framework.
  Exercise workers with 0, 1, 2, 3 and many valid waves, odd/even final slots,
  partial final waves, AG column waves, nontrivial K divisors and K=32768.
  Preserve exact gather checks, FP32-partial precision, random references,
  changed inputs/shapes, delayed ranks and repeated epochs on alternating
  streams without per-generation Host barriers. Verify launch counts and
  parent-before-child teardown. Keep existing Python contract tests.
- Establish internal overlap with a separate diagnostic artifact/run if the
  framework profiler shows only the fused task: next-tile producer work must
  overlap current-tile consumer work on a suitable multi-wave shape. A single
  kernel event or lower total latency alone does not prove internal overlap.
  Diagnostics must not enter the baseline or add production timing interfaces.
- Compare both operators against the preceding K-batched implementation and
  installed official fused operators on the same device subset and unchanged
  Host/core DSOs. Use 10 warmups plus at least 30 measured rounds per mode,
  repeated/reordered with two seeds, identical profiler state and input data.
  Keep numerical checks and distinguish official AR's rounding behavior.
  Report Host and device latency, rank statistics, raw samples and provenance;
  do not infer stage bandwidth from fused total latency.
- Qualify correctness before accepting performance. Investigate repeatable
  small-shape regressions and require reproducible benefit on the intended
  multi-wave workloads before replacing the baseline. Do not retain a new
  production mode switch, per-shape table or duplicate implementation to hide
  a failed optimization. Record results and limitations in the maintained
  integration/validation guides; no speedup is guaranteed by this design.
- Check occupancy before hardware workloads and reuse existing HCCL environment
  qualification within its recorded scope. No new transport/hardware claim is
  established by these two-card tests.

## Incremental migration and packaging

1. Extract device macros and contracts while retaining the specialized ABI,
   artifact names, schedule and public functions. Revalidate behavior before
   adding the new launcher.
2. Add generic runtime/Plan and canonical ABI v1, then generate a v1
   AllGather-GEMM artifact using the same primitives/algorithm.
3. Preserve the old entrypoint as a compatibility adapter. Existing legacy
   artifacts remain usable through their explicit ABI-v0 path. New generic
   entrypoints use v1 artifacts; no ABI guessing or implicit fallback.
4. Add Python GEMM-AllReduce and tests without rebuilding/changing the generic
   C++ library. Record the identical runtime library hash for both operators.
5. Treat bounded lookahead, double buffering and polling reduction as later
   isolated performance work, after the generalization baseline is established.

The generic `libtilexr-relay.so` depends on core communication, common binary
loading and CANN, not collectives, Python or TVM. It is optional/default OFF;
the TileLang compatibility option may enable it as an internal dependency.
The existing Host pipeline still requires collectives. Native code/binaries
are installed separately under the normal lib/include/kernel directories.
The Python package/build tools are opt-in and do not install system packages.

Successful registration stubs and binary bytes initially retain process-lifetime
storage as in the current validated path. Plan destruction releases plan
ownership, not borrowed device memory or code still usable by another plan.
Failed registration is cleaned up safely. Live binary eviction, hot replacement
and runtime teardown ordering are not part of this increment.

## Verification and acceptance

- Native unit/mock and installed-consumer tests: canonical ABI frame, every
  binding source, alignment/capacity/overflow, rank/target/block rejection,
  descriptor lifetime, first-error propagation, launch count and timeout rules.
- Builder/unit tests: concrete typed layouts, channel collisions, live-region
  alias rejection, deterministic identity, distinct operations/specializations,
  measurement-mode separation and malformed/version-incompatible descriptors.
- Compile/link and argument-echo hardware probe: actual canonical argument
  order, all sixteen slots including unused ones, scalar widths and FFTS.
  No source-text/token/regex implementation guards.
- Two-card AllGather-GEMM: unchanged golden comparison, repeated changed
  inputs, delayed rank, shape/plan alternation without per-generation Host
  barriers, complete framework traces and one main launch per generation.
- Two-card GEMM-AllReduce: FP32 partial/SUM behavior and final FP16 CPU
  reference, repeated inputs, delayed rank and alternation with AllGather-GEMM.
  Prove the same C++ runtime hash and no new native operator dispatch.
- Performance comparisons preserve profiler/dtype/shape/compiler settings and
  timing boundaries, use at least five warmups and twenty measured rounds,
  report rank median/min/max and retain raw samples. Plan creation, compilation
  and workspace initialization are outside the steady-state timer. Near-tie
  results require repeated/reordered trials and must not imply stable speedup.
- Preserve exact artifact/compiler/runtime provenance, numerical results,
  bounded launcher logs and post-test device occupancy. Reuse retained HCCL
  qualification with its original coverage boundary; do not rerun it merely
  because this module was refactored.

Host tests or DSL macro availability do not prove device correctness. The
ABI-v1 compiler gate, FP32 reduction support and mixed-operator flag lifetimes
are the main unresolved implementation risks. Broader ABI, rank, transport,
asynchronous or hardware support requires a separate explicit decision.
