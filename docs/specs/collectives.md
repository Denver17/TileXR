# Collective Runtime and Profiling Specification

## Scope

The optional collectives component builds `libtilexr-collectives.so` on top of
the core `libtile-comm.so` communicator. It owns collective algorithm dispatch,
embedded AICore binaries, launch validation, and generic profiling markers.
The core communicator remains independently usable.

The public operations in `src/include/tilexr_collectives.h` are AllGather,
AllToAll, AllReduce, ReduceScatter, and Broadcast. The
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

Collectives uses the common recorder from `src/include/tilexr_profiling.h` at
the original coarse kernel begin/end locations. It adds no Host launch argument,
session API, records schema, or report implementation. The existing `commArgs`
kernel argument supplies the common profiling GM state, and runtime enablement
is controlled by `TILEXR_PROFILING_RECORDS_PATH` and the other common profiling
environment variables.

`tools/collectives/tilexr_collectives.mapping.json` gives the two events their
operator-owned meaning. After communicator destruction exports records, the
unchanged `tools/profiling` command validates and renders them.

Do not remove `ExtraFlag::PROFILING` or `CommArgs::tilexrProfiling*` while
cleaning legacy Collectives profiling. Those fields belong to the common
runtime and are required by every PR #143 producer.

The installed `tilexr_collectives_perf.h` and `tilexr_perf_trace.h` files are
declaration-free compatibility headers. Keep the filenames in the install
manifest for existing consumers and the sealed v4 CI controller, but do not
restore the removed session API or trace schema in them.

## Failure and Compatibility

- Missing optional profiling configuration leaves normal execution unchanged.
- Malformed or incomplete records are rejected by the common profiling tool.
- Runtime failure is reported through the public return code and must not be
  replaced by an unverified algorithm fallback.

## Verification Boundary

Source and Host tests cover argument validation and algorithm selection. A Host or simulator pass does
not prove AICore execution, multi-rank correctness, topology behavior, or device
timing. Build and hardware commands are maintained in
`docs/BUILD_VERIFICATION.md`. A real hardware profiling check must retain the
common records file and the generated `perfetto_trace.json`.
