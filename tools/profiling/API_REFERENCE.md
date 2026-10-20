# Profiling API reference

Include `<tilexr/profiling.h>`. Namespace: `TileXR::Profiling`. SDK version,
DeviceView ABI, and disk protocol versions are independent. Host code is
C++14; ACL capture and file publication support Linux. The pure types/layout/
encoder layer needs only the C++ standard library. The AICore branch parses
neither Host STL nor ACL. Applications link their own CANN Host libraries and
compile/register/launch their own kernels.

`TileXR::profiling` is an optional installed CMake INTERFACE target. It
propagates include paths and C++14, without adding communication or ACL libraries.

## Configuration and capture

`CaptureConfig` has `enabled=true`, `rank=0`, `logicalStreamCount=1`,
`recordsPerStream=4096`, a 16-byte `captureId`, and workspace/file byte budgets.
Rank is signed and must be 0…65535. Lane count is 1…65536, lane indices 0…65535.
Capacity is 8…65536, a multiple of eight. Count is an actual valid prefix and
can be unaligned. Budgets and encoding limits are checked before allocation.

| Method | Contract |
|---|---|
| `Initialize(stream, logicalStreamCount)` | Default config; generate a fresh random UUID. Fail if entropy is unavailable. Borrow the current context and stream. |
| `Initialize(config, stream)` | Same lifecycle; caller distributes a shared nonzero UUID for a distributed capture. Initialization allocates/zeros ordinary device memory and synchronously uploads the descriptor. |
| `DeviceArgs()` | GM descriptor address for one kernel invocation; null if disabled, sealed, or closed. Never pass a Host stack DeviceView address. |
| `Collect()` | Seal submissions; synchronize only the bound stream, D2H, validate, and own a read-only Host snapshot. Successful collection is idempotent. |
| `Snapshot()` | `const CaptureSnapshot*`, null before collection or if disabled. Remains valid through Close, until reinitialization/destruction. |
| `Export(path)` | Encode the collected snapshot and publish without overwriting; no ACL work. Can retry another path after failure, including after Close or from a different current context. |
| `Save(path)` | `Collect()` followed by `Export(path)`. Export failure preserves the snapshot. |
| `Close()` | Seal, wait if necessary, free once. Idempotent; synchronization/free failure retains ownership for retry. Keep the context/stream alive until successful Close. |
| `Config()`, `WorkspaceBytes()` | Inspect effective configuration/UUID and retained allocation bytes; bytes are zero after successful Close, failed allocation cleanup, or moving ownership away. |

Empty/Closed → Initialize → Ready → Collect → Collected → Close → Closed.
Export does not consume the snapshot. Close without Collect permits safe cleanup
but creates no snapshot. Initialize while Ready/Collected is invalid; Close first.
The first Collect/Close seals submissions even when it fails. Do not enqueue
further work with an old descriptor. One capture is one kernel measurement,
with a single writer per lane; it is not a communicator or a multi-launch buffer.

All Host methods are serialized by the caller. ACL methods require the saved
context to be current on the calling thread; they return an error on mismatch.
Copy and move assignment are deleted. Move construction transfers ownership.
Destruction performs no export and never throws; if safe completion cannot be
confirmed, it retains GM and writes a cleanup diagnostic. Explicitly check Close.

With `enabled=false`, Initialize allocates/uploads nothing, DeviceArgs is null,
and Collect/Export/Save/Close succeed without creating files. Device compilation
is independent: undefined/0 `TILEXR_ENABLE_PROFILING` disables instrumentation,
1 enables it, other values fail compilation. Compiled markers may still read
the timer when the runtime descriptor is null.

| StatusCode | Meaning |
|---|---|
| `Ok` | Operation succeeded. |
| `InvalidArgument`, `InvalidState` | Invalid config or lifecycle/context. |
| `Acl` | CANN operation failed; inspect `aclError` and `message`. |
| `Snapshot` | Invalid committed state, count, event, or encoding. |
| `Io` | UUID generation or file operation failed; no successful publication. |
| `PublishedCleanupWarning` | Target was published; only temporary-name cleanup failed. `ok()`/bool is true. Do not re-publish that path. |

## Device writers and lower-level Host API

`Recorder<N>` supports 1…256 compile-time slots. `RecordMarker<Slot,EventId>()`
requires a nonzero event ID and valid Slot. Construct before measurement.
Assign Slots in execution order; conditional branches may skip slots. Flush
compacts valid slots in Slot order, reads no skipped timestamp, and commits
even an empty measurement: `Flush(profileGM, logicalStream)`.

`DirectWriter.Initialize(profileGM, logicalStream)` first resets local state.
`RecordAt(eventId, timestamp)` appends explicit timestamps (including zero).
`Publish()` commits once and is idempotent on the same writer. Event ID zero
invalidates the lane. Capacity overflow retains the prefix and saturates dropped
at UINT32_MAX. Reusing a committed lane with another writer invalidates it.
Null descriptors and disabled compilation are safe no-ops. Lane state is not a
lock and does not support concurrent writers. Marker calls insert no business
pipeline barriers; the operator defines the completion boundary it measures.

`BuildLayout(config, &layout)`, `MakeDeviceView(config, layout, workspace)`,
`BuildSnapshot(...)`, `EncodeV1(snapshot, &bytes)`, and
`ExportSnapshot(snapshot, path)` are the same public components used by
AclCapture. BuildSnapshot receives full, strided state/payload vectors.
Caller-managed workspaces must satisfy the checked layout/address contract;
the caller owns allocation, context, completion, and staging lifetime. AclCapture
itself only owns its allocation; borrowing/pooling arbitrary external memory is
deferred.

Linux publication uses a unique same-directory O_CREAT|O_EXCL temporary,
complete write/fsync/close, hard link to create the destination without replacement,
then cleanup of its own temporary. Unsupported filesystems return I/O error.
This does not promise directory persistence across power loss.

## TileXR asynchronous integration

The collectives resource owner holds ordinary captures and passes their GM
descriptors to ordinary/A5 kernels. Initial preparation has allocation/upload
cost; later acquisition neither collects, exports, synchronizes, nor destroys a
capture. Capture resources are drained during existing communicator resource
cleanup, before context/stream teardown. Local per-entry drain has no rank
collective; fixed common cleanup phases agree status across ranks. Failed items
remain owned, and successful published/closed entries are skipped on retry.

Build collectives with `TILEXR_COLLECTIVES_ENABLE_PROFILING=ON`. Set
`TILEXR_PROFILING_OUTPUT_DIR` and distribute one fresh
`TILEXR_PROFILING_CAPTURE_ID` UUID to all ranks in the same communication group.
Each prepared capture derives a distinct UUID and writes
`OUTPUT_DIR/<capture_uuid>/rank-<rank>.records.bin`. Use a new seed for another
owner/run; launch order and sampling configuration must match across ranks.
The bounded owner prepares at most four captures and 64 MiB; exhaustion skips
sampling with a diagnostic and still launches the business kernel with null.
A5 configures 48 logical lanes (32 send + 16 receive), distinct from blockDim 33.
Ordinary kernels use the actual participating AIV block count.

Warmup must run without a capture. The perf driver suppresses the profiling
output setting during warmup. One report consumes one capture directory; do not
merge several UUIDs or infer iterations from event order. Old communicator
auto-aggregation and `TILEXR_PROFILING_RECORDS_PATH` are retired. Reserved
CommArgs fields retain ABI space but have no collection semantics.
