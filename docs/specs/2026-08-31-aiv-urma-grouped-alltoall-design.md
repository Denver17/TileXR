# A5 AllToAll Internal Optimization Design

> **Packaging update (2026-09-03):** The embedding details in this historical
> design are superseded by
> `2026-09-03-external-aicore-kernel-packaging-design.md`. The A5 objects remain
> owned by Collectives but are installed under `tilexr/kernels` and loaded on
> first use.

## Goal

Integrate the validated grouped AIV URMA algorithm as an internal A5/Ascend950
optimization of the existing `TileXRAllToAll()` API. The implementation belongs
to `src/collectives` and does not introduce a public prepared-handle lifecycle
or a separate Host shared library.

Callers continue to use:

```cpp
TileXRAllToAll(sendBuf, recvBuf, sendCount, dataType, comm, stream);
```

The existing generic AllToAll remains authoritative for every unsupported
combination and must retain its current behavior.

## Selection Contract

The A5 path is a candidate only when all of the following are true:

- the communicator reports Ascend950/A5 capability;
- `dataType` is `int32`;
- rank size is between 8 and 128 inclusive and divisible by eight;
- local rank size is eight;
- UDMA, shared QP, SDMA, and Credit resources are available;
- the runtime owns a complete grouped profile with exactly 39 QPs, the expected
  profile kind, one workspace region, all required bindings, and valid Host and
  device registry views.

The fixed algorithm is multi-route, single-pass, `quietBatch=1`, Credit enabled,
receive progress enabled, and SDMA copyout enabled. These properties are private
algorithm constraints, not environment variables or public API options.

Static ineligibility and explicit `TILEXR_ERROR_NOT_SUPPORT` before collective
resource initialization select the generic path. Once collective profile
registration begins, allocation, registration, cross-rank agreement, or profile
validation failures are returned to every rank. They must not cause a local-only
fallback that could launch different kernels across ranks.

## Host Ownership

`src/collectives/host/tilexr_collectives.cpp` retains public argument validation,
loopback handling, rank/count overflow checks, and generic/A5 selection.

`collective_launcher.*` owns the private per-communicator A5 runtime:

```cpp
struct A5AllToAllRuntime {
    void *workspace;
    uint64_t workspaceCapacity;
    TileXRUDMAProfileHandle profile;
    TileXRUDMAProfileView profileView;
    int64_t previousMagic;
    bool launched;
    uint32_t profilingLaunchSlot;
};
```

These are the stable algorithm fields. The private implementation may also
retain retry-only cleanup state for a profile/workspace that could not be
released during growth; that state is not part of a launch or public ABI.

The concrete structure stays in a `.cpp` private namespace. The launcher:

- computes the checked layout for the current `sendCount`;
- creates and registers workspace on first use;
- reuses workspace and profile for equal or smaller requests;
- replaces them safely when a larger request requires more capacity;
- takes `sendBuf` and `recvBuf` from every call instead of caching them;
- binds the implementation to the current single-stream collective contract by
  serializing Host mutation and rejecting a different stream after first use;
- carries `previousMagic`, first-invocation state, and the fixed profiling slot.

Steady-state launches do not allocate, synchronize, register, query the UDMA
profile registry, or rebuild the profile. Reading the communicator's already
agreed grouped-route capability is a local check. Profile validation happens
when the cached runtime is created or replaced.

`collective_kernel.*` owns registration and launch of the A5 binaries. The
generic binary and the A5 64P/128P binaries are external artifacts owned by the
same Host library and launched with `rtKernelLaunchWithFlagV2`.

## Communicator Lifetime

`libtile-comm.so` provides an internal, non-installed opaque resource slot API.
The slot stores only an opaque pointer and a destroy callback; it does not know
the A5 runtime type and does not depend on `libtilexr-collectives.so`.

The collectives launcher attaches one A5 runtime to each communicator. During
`TileXRCommDestroy()`, the communicator invokes the callback before UDMA and
SDMA teardown. The callback unregisters the profile, frees the workspace, and
deletes the private runtime. Registration failures and partial construction
must unwind in the reverse order. No global map keyed by communicator is used.
If callback cleanup fails, destroy returns the error while retaining the
communicator and callback state for an explicit retry; transport teardown does
not run ahead of registered workspace cleanup.

Communicator destruction follows the existing caller contract that enqueued
work is quiesced before destroy. The optimization adds no synchronization to
the steady-state launch path.

## Profile And Routing Contract

Grouped routing is selected by an explicit profile kind carried in the existing
versioned UDMA profile descriptor/registry contract. Generic profiles use the
default kind. The grouped kind is accepted only when all ranks agree and the
registry contains exactly the validated 39-QP shape and bindings.

The transport must not select grouped remote-QP mapping from `qpCount == 32` or
`qpCount == 39`. Route creation and remote-QP mapping receive the validated
profile classification explicitly. Mismatched kind, count, route descriptor,
region, or binding data is rejected before launch.

## Kernel And Trace Contract

The kernel implementation lives under `src/collectives/kernels` as
`tilexr_alltoall_a5_kernel.cpp` with shared Host/device layout in
`alltoall_a5_common.h`. It preserves the validated schedule:

- 32 send AIV cores and one SDMA receive/copyout core;
- ordered payload and signal publication, Credit closure, and bounded waits;
- dedicated 64P and 128P variants, with rank sizes through 64 using the former
  and larger rank sizes using the latter;
- the established balanced 128-rank partition schedule.

Profiling uses a fixed per-invocation slot from the communicator runtime. It
must not depend on a cross-kernel GM append count. Validation requires every
active core to emit the complete expected round set without overlap.

## Build And Public Surface

`TILEXR_BUILD_COLLECTIVES` is the only option controlling this implementation.
The build produces and installs only `libtilexr-collectives.so` and the existing
collectives headers.

Remove the standalone grouped option, shared library, public headers, installed
demo, and these symbols:

- `TileXRGroupedAllToAllGetWorkspaceSize`;
- `TileXRGroupedAllToAllPrepare`;
- `TileXRGroupedAllToAllRun`;
- `TileXRGroupedAllToAllGetKernelStatus`;
- `TileXRGroupedAllToAllDestroy`.

Grouped layout and lifecycle tests move under `tests/collectives`; there is no
standalone grouped source or test ownership tree.

## Failure Handling

Checked Host arithmetic rejects overflow, zero sizes, invalid ranks, and
impossible workspace layouts. Capability absence preserves the generic path.
Runtime construction is transactional: a failed expansion leaves an active
valid runtime or no attached runtime. Old resources that fail local cleanup are
retained in a rank-agreed retry queue and are not freed while registered.
Because launches are asynchronous, growth first synchronizes the bound stream
before unregistering or freeing the old profile/workspace. This synchronization
is limited to growth and does not affect steady-state reuse.

The kernel records bounded device failures in private workspace state for
diagnostics. The public API continues to report Host and launch errors through
the existing `TileXRAllToAll()` return value; no grouped-specific status API is
exposed.

## Validation

Host tests cover generic dispatch regression, A5 eligibility, exact 39-QP
profile classification, layout overflow, first creation, steady-state reuse,
growth replacement, failure unwind, communicator cleanup, launch ABI, schedule
coverage, Credit closure, and fixed trace slots.

Target validation then covers:

1. CANN 9.1 Host and both A5 kernel builds with staged external objects;
2. generic AllToAll regression on supported existing topology;
3. A5 correctness and repeated-call reuse at 2x8, 3x8, 4x8, and 8x8 ranks;
4. a larger request after reuse to exercise workspace/profile expansion;
5. communicator destruction with resource-release evidence;
6. trace completeness for every active core;
7. performance with profiler and collective barrier state recorded consistently.

Host, compile, simulator, and 910B evidence do not prove A5 UDMA data-plane
correctness or performance.

## Non-Goals

- a new public API, public runtime type, standalone library, or build option;
- caller-controlled grouped modes, quiet batching, Credit, receive, or SDMA;
- 910B fallback claims, framework registration, or ACLNN packaging;
- multi-stream or concurrent calls on one communicator;
- changes to the validated payload schedule without separate evidence.
