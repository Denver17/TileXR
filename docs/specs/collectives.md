# Collective Runtime and Profiling Specification

## Scope

The optional collectives component builds `libtilexr-collectives.so` on top of
the core `libtile-comm.so` communicator. It owns collective algorithm dispatch,
external AICore binaries, launch validation, and generic profiling markers.
The core communicator remains independently usable.

The public operations in `src/include/tilexr_collectives.h` are AllGather,
AllToAll, AllReduce, ReduceScatter, and Broadcast. The
library accepts a TileXR communicator and an ACL stream; it does not own either.

## Build and Launch

- `TILEXR_BUILD_COLLECTIVES` controls the optional target.
- AICore binaries are built as device artifacts, installed under the library's
  `tilexr/kernels` directory, loaded on first use, registered with the runtime,
  and launched through the registered signature.
- Host dispatch validates datatype, count, topology, rank size, buffer contract,
  and algorithm support before launching.
- Multi-rank AllToAll retains the generic `TOPO_910_93` implementation. On
  Ascend950/A5, `TileXRAllToAll` may internally select the grouped kernel for
  `int32`, rank sizes from 8 through 128 that are divisible by 8, and eight
  local ranks. Rank sizes through 64 use the 64-rank binary; larger sizes use
  the 128-rank binary. there is no separate grouped public API or library.
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

The A5 path requires UDMA shared-QP, SDMA, and Credit readiness plus the exact
validated 39-QP grouped route descriptor. A raw QP count is not an algorithm
selector. Its registered workspace profile is explicitly classified as the A5
grouped AllToAll profile and binds all 39 QPs to the single workspace region.
The eight-rank divisibility requirement preserves complete, uniform local
nodes; non-multiples such as 63 remain unsupported because the communicator
does not represent uneven per-node rank counts.

The communicator owns the private A5 runtime. The first launch allocates and
registers its workspace; equal or smaller calls reuse it without Host
synchronization or re-registration. Growth installs a new profile
transactionally, synchronizes the bound stream on that rare path, and retires
the old profile/workspace through rank-agreed, retryable cleanup. Current send
and receive addresses are always per-call. Host mutation is serialized and the
runtime rejects a stream different from the one used on first launch.
Communicator destruction runs this cleanup before UDMA and SDMA shutdown.
If internal cleanup fails, `TileXRCommDestroy` returns that error without
tearing down the communicator so the caller can retry destruction.

## Profiling

Collectives uses `<tilexr/profiling.h>` with the same Recorder/DirectWriter and
AclCapture API available to external operators. Its internal Host/Kernel
arguments pass an independent DeviceView. CommArgs profiling fields remain
reserved ABI slots only; no SDK code interprets them.

The common resource owner keeps asynchronous captures alive, with at most four
prepared captures and a 64 MiB budget. `TILEXR_PROFILING_OUTPUT_DIR` enables
sampling; multi-rank callers distribute a fresh `TILEXR_PROFILING_CAPTURE_ID`
seed and match launch order. Each capture writes a separate UUID directory.
First preparation costs allocation/upload; subsequent acquisition performs no
collection, I/O, free, or stream wait. Budget exhaustion skips profiling.
Existing resource cleanup drains locally before its fixed rank status phases.
A5 retains every owner until its common drain phase succeeds, so retries do not
change the rank collective sequence. Generic optional owners can be absent;
communicator destruction always performs the outer status phase on every rank.

Use `python3 -m tilexr_profiling` and the operator-owned mapping after cleanup.
See [the API reference](../../tools/profiling/API_REFERENCE.md). Warmup does not
record, and one stream in each capture has one critical interval; repeated
kernels are never accumulated into one stream.

The installed `tilexr_collectives_perf.h` and `tilexr_perf_trace.h` files are
declaration-free compatibility headers. Keep the filenames in the install
manifest for existing consumers and the sealed v4 CI controller, but do not
restore the removed session API or trace schema in them.

The A5 writer derives every record address from a fixed invocation slot. It
does not append through a GM counter shared across kernels. A launch that would
exceed the configured records-per-core capacity fails before kernel launch.

## Failure and Compatibility

- Missing optional profiling configuration leaves normal execution unchanged.
- Malformed or incomplete records are rejected by the common profiling tool.
- Runtime failure is reported through the public return code and must not be
  replaced by an unverified algorithm fallback.

## Verification Boundary

Source and Host tests cover argument validation, algorithm selection, profile
classification, workspace layout, resource teardown, and record parsing. A Host or simulator pass does
not prove AICore execution, multi-rank correctness, topology behavior, or device
timing. Build and hardware commands are maintained in
`docs/BUILD_VERIFICATION.md`. A real hardware profiling check must retain the
common records file and the generated `perfetto_trace.json`.
