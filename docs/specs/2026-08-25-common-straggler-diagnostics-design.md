# Common Straggler Diagnostics Design

## Status

Approved direction: move clock synchronization and straggler detection out of
the optional Collectives library into a reusable diagnostics component. The
existing `TileXRCollectiveStraggler*` API is not retained as a compatibility
layer.

## Goals

- Provide one communication-domain clock calibration service shared by
  Collectives, EP, MoonEP, and future communication producers.
- Detect fast and slow ranks for every instrumented communication event without
  making the diagnostics component depend on a producer library.
- Keep the clock synchronization operation independent from business kernels.
- Preserve C++14, CANN 9.1.0, and registered AICore binary launch requirements.
- Keep reports comparable across producers by using one event and rank model.

## Non-Goals

- This migration does not add EP or MoonEP instrumentation to every production
  kernel. It provides and validates their common integration boundary; producer
  coverage can then be added without another ownership change.
- It does not provide external reference-clock accuracy. Reported precision is
  the communication-domain error bound derived from minimum round-trip time.
- It does not retain the unpublished Collective-specific straggler ABI.

## Ownership And Dependencies

Add `src/diagnostics`, producing `libtilexr-diagnostics.so`:

```text
libtile-comm.so
       ^
libtilexr-diagnostics.so
       ^
Collectives / EP / MoonEP
```

`libtilexr-diagnostics.so` owns:

- detector creation, destruction, calibration state, and ownership checks;
- the communication-domain control exchange used by calibration and collection;
- clock sample estimation, event classification, and JSON reporting;
- compilation, embedding, registration, and launch of the standalone clock
  synchronization AICore binary;
- generic producer-side Host observation APIs and device record layout.

The component depends on `libtile-comm.so` for `TileXRCommPtr`, `CommArgs`, peer
memory, magic generation, and bounded Host all-gather. It must not include or
link Collectives, EP, or MoonEP sources.

Producer libraries own the placement of observation calls around their own
communication kernels. They include the common device instrumentation header
and pass the reserved device record address through their existing kernel
argument path.

## Source Layout

```text
src/diagnostics/
  CMakeLists.txt
  cmake/
  host/
    clock_sync.cpp
    kernel_registration.cpp
    straggler_analysis.cpp
    straggler_session.cpp
  kernels/
    CMakeLists.txt
    clock_sync_kernel.cpp

src/include/
  tilexr_straggler.h
  tilexr_straggler_device.h
  tilexr_straggler_layout.h
```

The implementation may keep private helper headers next to their owning source.
No diagnostics implementation remains under `src/collectives` after migration.

## Public Host API

The public handle and control API are producer-neutral:

```cpp
typedef void *TileXRStraggler;

int TileXRStragglerConfigDefault(TileXRStragglerConfig *config);
int TileXRStragglerCreate(TileXRCommPtr comm,
                          const TileXRStragglerConfig *config,
                          TileXRStraggler *detector);
int TileXRStragglerDestroy(TileXRStraggler detector);
int TileXRClockSynchronize(TileXRStraggler detector,
                           aclrtStream stream,
                           TileXRClockSyncResult *result);
int TileXRStragglerCollect(TileXRStraggler detector,
                           aclrtStream stream,
                           TileXRStragglerReport *report);
```

Producer integration is explicit and does not use Collectives-owned thread-local
state:

```cpp
int TileXRStragglerObservationBegin(
    TileXRStraggler detector,
    const TileXRStragglerOperation *operation,
    aclrtStream stream,
    TileXRStragglerObservation *observation);

int TileXRStragglerObservationEnd(
    TileXRStraggler detector,
    const TileXRStragglerObservation *observation,
    aclrtStream stream,
    int launchStatus);
```

`TileXRStragglerOperation` contains stable generic identity fields:

- component kind: Collectives, EP, MoonEP, or user-defined;
- operation kind and optional stage identifier;
- message bytes;
- caller operation identifier when available.

`TileXRStragglerObservation` returns the device record address, launch ID,
generation, rank, and normalized operation metadata needed by the producer's
kernel arguments. A zero device record address means observation is inactive;
producer behavior remains unchanged.

No `TileXRCollectiveStraggler*` type, symbol, or installed header is retained.

## Device Instrumentation

`tilexr_straggler_device.h` exposes producer-independent helpers equivalent to:

```cpp
TileXRStragglerTimingToken TileXRStragglerTimingBegin(
    GM_ADDR record);

void TileXRStragglerTimingEnd(
    TileXRStragglerTimingToken token,
    GM_ADDR record);
```

Each aligned ring slot contains a 64-byte device control block followed by the
80-byte timing record and alignment padding. Observation begin writes the control
block on the producer stream before the business-kernel launch. The producer passes
only the record pointer through its private launch ABI; the device helper derives the
adjacent control address and reads launch, operation, rank, generation, and optional
test-delay metadata from it. This avoids appending producer-specific scalar arguments
whose generated direct-launch metadata may not match an older kernel signature.

The record captures local start, communication-ready, and end cycles. The producer
determines the exact semantic boundary of communication-ready and must call the end
helper only after its communication data movement has completed. The helper uses the
existing MTE3-safe UB-to-GM publication path.

The layout header contains only stable Host/device structures, offsets, and
compile-time size checks. It does not include Collectives types.

## Clock Synchronization

Clock synchronization remains a standalone operator and uses only communicator
peer memory. Each round uses `TileXRCommNextMagic`; protocol slots are not reset
globally.

Each peer calibration uses a `READY -> REQUEST -> RESPONSE -> TIMESTAMP -> ACK`
four-timestamp exchange. Rank 0 publishes `READY` for exactly one peer and
sequence before that peer records `t0` and pushes `REQUEST` into rank 0 memory;
rank 0 then records `t1` and pushes `RESPONSE` into peer memory, after which the
peer records `t3`. Rank 0 measures the midpoint of its completed RESPONSE MTE3
window and publishes that value as `t2` through a separate TIMESTAMP slot; the
peer then pushes `ACK` into rank 0 memory. Timestamp delivery occurs after `t3`
and is outside the measured interval. Rank 0 does not advance to another peer
until it observes the ACK. Time spent waiting for rank 0 to service other peers
is therefore outside the measured `t0 -> t3` interval.

REQUEST, ACK, and TIMESTAMP use separate per-rank 64-byte slots. Reusing or
placing states in one remotely updated cache line can leave a receiver polling
a stale state even after MTE completion. Receivers fetch the complete 64-byte
slot with MTE2. The REQUEST write, REQUEST read, RESPONSE write, and RESPONSE
read timestamps use the midpoint of their respective MTE operation windows,
reducing the systematic endpoint bias from assigning the complete transfer
latency to one side. The estimator remains `((t1 - t0) + (t2 - t3)) / 2`, and
the conservative error bound remains half of the minimum corrected round trip.

Calibration runs configurable warmup exchanges before configurable measured
samples. Warmups exercise the identical device path but are never written to the
sample buffer or considered by the estimator. The default is 32 warmups followed
by 128 measured samples. `TileXRClockSyncResult::sampleCount` counts measured
samples only and `warmupSampleCount` records the discarded exchanges.

The diagnostics build embeds a pure AICore object and registers it with
`rtDevBinaryRegister` and `rtFunctionRegister`. Host launch uses
`rtKernelLaunchWithFlagV2` with a fixed argument ABI.

SoC selection uses a common cache variable derived from `TILEXR_SOC_NAME` and
must fail configuration when no supported target is known for an enabled
diagnostics build. It must not silently compile Ascend950 for `dav-c220-vec`.

## Session And Concurrency

- A detector belongs to exactly one `TileXRCommPtr` and one creating Host
  thread unless the public contract is extended later.
- Only one detector may own a communicator at a time.
- Calibration, observation reservation, observation completion, and collection
  are generation-tagged.
- Destroy rejects or drains outstanding observations according to the existing
  bounded timeout policy; it never frees device records still owned by a
  producer stream.
- Producer launch failure is recorded as observation failure and does not
  overwrite a prior device-side failure.

## Reporting

`TileXRStragglerReport` describes a generic communication event. It includes:

- component kind, operation kind, stage ID, and operation ID;
- communication bytes and calibration generation;
- median and MAD duration and pre-communication latency;
- clock precision and calibration age;
- per-rank aligned timestamps, duration, deltas, classification, and reasons.

The clock report retains per-rank offset, minimum and median RTT, valid/rejected
sample counts, and error bound. Precision remains the maximum accepted per-rank
`min_rtt / 2` bound for the domain.

## Build And Installation

Add `TILEXR_BUILD_DIAGNOSTICS`. It is enabled automatically when Collectives,
EP, or MoonEP enables straggler instrumentation, and can be built independently.
The installed artifacts are:

- `libtilexr-diagnostics.so`;
- `tilexr_straggler.h`;
- `tilexr_straggler_device.h`;
- `tilexr_straggler_layout.h`.

Collectives removes the migrated Host sources, clock kernel, registration entry,
and installed Collective-specific header. It links diagnostics only for the
observation adapter. EP and MoonEP source/compile tests link the same public
integration surface without any dependency on `libtilexr-collectives.so`.

## Migration Sequence

1. Add diagnostics build, public headers, clock binary, Host session, and
   analysis by moving the existing implementation without behavior changes.
2. Rename all public and internal Collective-specific types to generic types.
3. Replace active Collectives TLS coupling with explicit observation begin/end
   data passed through the Collective launch context.
4. Remove migrated sources and the clock entry from the Collectives binary.
5. Move unit and hardware tests under `tests/diagnostics`; keep one Collectives
   integration test as a producer adapter test.
6. Add EP and MoonEP compile/link boundary tests.
7. Remove `tilexr_collective_straggler.h` and all old symbols.

## Verification

- Header ABI and C++14 compile tests for all public structures and functions.
- Unit tests for clock estimation, classification, config validation, lifecycle,
  generation reuse, ownership, launch failure, and incomplete observations.
- Source guards proving diagnostics does not depend on producer modules and EP /
  MoonEP do not depend on Collectives for diagnostics.
- Independent diagnostics build and install-prefix consumer test.
- Collectives aggregate communication correctness plus fast/slow rank detection.
- Existing 64-rank, eight-node clock-only test on Ascend950 with the explicitly
  recorded SoC binary provenance.
- The 64-rank test must enforce a caller-supplied precision ceiling against p50,
  p95, and maximum communication-domain error bounds; the production acceptance
  ceiling for the reference-ready protocol is strictly below 1000 ns.

The retained 64-rank HCCL AIV-only result qualifies the current environment and
must be reused rather than rerun unless the environment identity changes.

## Reusable Build Lesson

An empty SoC cache variable previously selected `dav-c220-vec` for Ascend950 and
caused every rank to fail at the same scalar instruction PC. Any standalone
AICore component must make its SoC selection explicit, record the selected
architecture in build provenance, and reject an unknown target instead of
silently choosing a different hardware generation.
