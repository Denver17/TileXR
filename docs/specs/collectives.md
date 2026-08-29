# Collective Runtime and Profiling Specification

## Scope

The optional collectives component builds `libtilexr-collectives.so` on top of
the core `libtile-comm.so` communicator. It owns collective algorithm dispatch,
embedded AICore binaries, launch validation, and optional performance tracing.
The core communicator remains independently usable.

The public operations in `src/include/tilexr_collectives.h` are AllGather,
AllToAll, AllReduce, ReduceScatter, Broadcast, and the profiling probe. The
library accepts a TileXR communicator and an ACL stream; it does not own either.

## Build and Launch

- `TILEXR_BUILD_COLLECTIVES` controls the optional target.
- AICore binaries are built as device artifacts, embedded in the Host library,
  registered with the runtime, and launched through the registered signature.
- Host dispatch validates datatype, count, topology, rank size, buffer contract,
  and algorithm support before launching.
- Multi-rank AllToAll is supported only on `TOPO_910_93`.
- Sources under `reference/` are comparison-only and are never linked into the
  active target.

## Algorithm Contract

Algorithm selection may depend on operation, datatype, payload size, chip,
topology, and rank count. Each selected kernel must preserve the public buffer
shape and reduction semantics. Unsupported combinations fail at Host validation
instead of silently selecting an incompatible kernel.

Synchronization uses the communicator's reusable magic-tagged flags. A new
round obtains a new value through `TileXRCommNextMagic`; shared flag memory is
not reset between operations.

## Profiling

Profiling is opt-in and must not change collective results or default launch
behavior. `src/include/tilexr_collectives_perf.h` defines the session lifecycle:

1. create a session with output and selection configuration;
2. make it active for launches on the calling process;
3. collect per-core stage records from instrumented kernels;
4. write a complete or explicitly incomplete report;
5. clear and destroy the session.

The generic record schema is defined in `src/include/tilexr_perf_trace.h`.
Records carry versioned headers, chip and stage metadata, per-core timestamps,
and accumulated metrics. Host aggregation preserves rank/core identity, labels
incomplete data, converts cycles with the recorded clock basis, and can emit a
timeline plus machine-readable report data. Offline report generation lives in
`tools/collectives/tilexr_collective_profile_report.py`.

Profiling buffers, report paths, and sessions are Host-owned resources. Kernel
writers own only their assigned fixed-size record and must not write another
core's region.

## Failure and Compatibility

- Missing optional profiling configuration leaves normal execution unchanged.
- Invalid session state or malformed records must produce an explicit report
  error; partial data must not be presented as a complete profile.
- New record fields require a version change or backward-compatible reserved
  space handling.
- Runtime failure is reported through the public return code and must not be
  replaced by an unverified algorithm fallback.

## Verification Boundary

Source and Host tests cover argument validation, algorithm selection, session
lifecycle, report aggregation, and record parsing. A Host or simulator pass does
not prove AICore execution, multi-rank correctness, topology behavior, or device
timing. Build and hardware commands are maintained in
`docs/BUILD_VERIFICATION.md` and profiling procedures in
`docs/COLLECTIVES_MULTIHOST_PROFILING.md`.
