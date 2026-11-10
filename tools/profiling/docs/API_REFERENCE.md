# Profiling API reference

[Overview](../README.md) · [中文入口](../README.zh-CN.md)

## Public report and manifest APIs

`generate_reports` and `mapping_fingerprint` accept mapping.v1 and mapping.v2.
`MAPPING_SCHEMA` remains the v1 string; `MAPPING_SCHEMA_V2` selects shared
boundaries and `SUPPORTED_MAPPING_SCHEMAS` lists both. See
[shared-boundary selectors](FORMAT.md#mappingv2-shared-boundaries) for numeric
and explicit-occurrence endpoints. Both use records.v1 and the same analyzer.
V2 adds parser-owned `mapping_schema`/`pairing_sha256` provenance and per-sample
`attributes.pairing`; caller attributes cannot override these facts.

```python
from pathlib import Path
from tilexr_profiling import generate_reports, mapping_fingerprint

mapping = Path("operator.mapping.json")
manifest = {
    "schema": "tilexr_profiling_manifest.v1",
    "attempt_id": "my-attempt-1",
    "capture_id": "018f6f5e-41a0-7d31-9ad1-82d61fc0a001",  # actual records UUID
    "mapping_sha256": mapping_fingerprint(mapping),
    "participants": [{"stream_id": 0, "rank": 0, "logical_lane": 0,
                      "sampled": True, "host": "worker-a",
                      "device": {"id": 0, "namespace": "host-local ACL ordinal"}}],
    # Use the actual stage ID/name and independently known workload bytes.
    "stage_metrics": [{"stream_id": 0, "stage_id": 0, "stage": "total",
                       "bytes": 40960, "byte_semantics": "algorithm read+write bytes",
                       "scope": "per_lane_total", "source": "workload allocation plan"}],
    "execution": {"kind": "compute"},
}
result = generate_reports(mapping, [Path("capture.records.bin")],
                          Path("new-report"), manifest=manifest)
print(result.capture_status, result.health_status, result.metadata_readiness, result.coverage)
```

This illustrative UUID/stage/workload must be replaced with actual input facts.
Prepare participants before execution; never derive them from received files.
Add real environment/version declarations and attach an observed Host snapshot
for full readiness. Missing fields retain specific reasons. The old four-argument
`generate_reports(mapping, records, output_dir, attributes=None)` call remains
valid. `manifest=` accepts a Path or JSON object. Empty records require a valid
manifest; a null capture ID denotes an attempt that never established a capture.

`load_manifest(value, mapping)` validates and returns an owned JSON document.
`mapping_fingerprint(mapping)` accepts the same Path/object mapping input.
`attach_host_events(manifest, snapshot)` attaches matching cumulative SDK snapshots.
`merge_manifest_shards(expected, shards, mapping)` joins explicit rank-owned
shards without shrinking the independent expected set. These helpers do no device
work. See [FORMAT.md](FORMAT.md#manifestv1-explicit-report-inputs) for schema,
limits, state semantics and all three output contracts.

## Host health events

`HostEvents(capacity=128)` is a Host-only bounded observer (capacity at most 4096).
`Begin(attempt_id)` starts one attempt; `BindCapture(uuid)` binds only an actual
capture. `Observe(operation, phase, status, code="")` records errors and successes
without throwing, resolves earlier failures of the same operation on success,
and preserves distinct retry IDs. `Snapshot()` and `DroppedEvents()` expose
observations; `Json()` serializes them and `Export(path)` uses no-overwrite file
publication. Diagnostic buffer saturation is an independent lower-bound counter.

`AclCapture::Events()` exposes automatic Initialize/Collect/Export/Close events.
`AclCapture(HostEvents&)` optionally borrows an observer that must outlive the
capture, allowing the caller to export final destructor cleanup/retry evidence.
The default constructor owns its observer. These are serialized Host APIs, not
concurrent callbacks. They do not add device synchronization, alter the device ABI,
or replace the returned SDK/ACL error when observation fails. Read and retain the
original status even if event export fails. Export a fresh snapshot after a retry;
never overwrite a previously published snapshot or combine unrelated attempts.

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
The Host pads physical record stride to a multiple of 16 and uses 16 uint32
elements per status slot so independent lanes never share a 64-byte cache line.
Logical capacity and encoded file budget exclude padding; workspace budget
includes it. Caller-owned workspaces passed to checked `MakeDeviceView` require
64-byte alignment. Current device writers reject unsafe addresses or strides;
rebuild Host and Kernel together when upgrading an older layout.

| Method | Contract |
|---|---|
| `Initialize(stream, logicalStreamCount)` | Default config; generate a fresh random UUID. Fail if entropy is unavailable. Borrow the current context and stream. |
| `Initialize(config, stream)` | Same lifecycle; caller distributes a shared nonzero UUID for a distributed capture. Initialization allocates/zeros ordinary device memory and synchronously uploads the descriptor. |
| `DeviceArgs()` | GM descriptor address for one kernel invocation; null if disabled, sealed, or closed. Never pass a Host stack DeviceView address. |
| `Collect()` | Seal submissions; synchronize only the bound stream, D2H, validate, and own a read-only Host snapshot. Successful collection is idempotent. |
| `Snapshot()` | `const CaptureSnapshot*`, null before collection or if disabled. Remains valid through Close, until reinitialization/destruction. |
| `Export(path)` | Encode the collected snapshot and publish without overwriting; no ACL work. Can retry another path after failure, including after Close or from a different current context. |
| `Save(path)` | `Collect()` followed by `Export(path)`. Export failure preserves the snapshot. |
| `Close()` | Seal, wait if necessary, free owned GM or return the borrowed lease. Idempotent; synchronization/free failure retains the resource for retry. Keep the context/stream alive until successful Close. |
| `Config()`, `WorkspaceBytes()` | Inspect effective configuration/UUID and retained workspace bytes, including borrowed storage. Bytes are zero when no workspace is retained, including successful Close, completed initialization rollback or move-out. |

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
supports both owned allocation and `InitializeBorrowed(config, stream, std::move(lease))`.
Register caller-owned ordinary device allocations with `WorkspacePool::Add`, then
`Acquire(bytes, context, &lease)`. The pool owns reservations, never allocations.
Keep allocation/context/stream alive until successful Close and zero busy leases.
Failed synchronization keeps the lease busy; an unsafe destructor quarantines it.
Sparse `CaptureConfig.logicalLaneIds` uses uint32 values in 0…65535; IDs must be
sorted, unique, and match physical slot count.
Both `Recorder::Flush` and `DirectWriter::Initialize` accept an optional
`expectedGeneration`: zero disables the check, while a nonzero value must match
the descriptor. Passing the original generation does not replace stream completion
or extend a descriptor's lifetime.

DeviceView ABI **2** is 88 bytes: rebuild Host and Kernel together; v1 disk files
remain unchanged. See [workspace contract](ADVANCED.md#workspace-leases).

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
merge several UUIDs or infer iterations from event order. Collectives does not
use the EP invocation-slot plan; each path owns distinct SDK captures.

### EP invocation-slot integration

Ordinary Memory and ServerDedup use `TILEXR_EP_ENABLE_PROFILING=ON`. Both include
`<tilexr/profiling.h>`; the forwarding include, old reserve APIs, compatibility
writer/owner/encoder and private 512B dump format have been removed. Ordinary
Memory emits two `Recorder<2>` body events (IDs 1/2) per participating lane.
ServerDedup uses `DirectWriter` with its existing Dispatch/Combine business event
catalogs and one terminal Publish per lane. Its record count depends on the path;
it is not constrained to two events.

The communicator's `profilingDescriptorsGM`, magic base, slot count and lane-count
table are the retained SDK integration. Before communicator initialization,
set `TILEXR_PROFILING_SLOTS_FILE` to a managed invocation plan. The first line is
`total_invocations workspace_budget_bytes`; each selected line is
`one_based_ordinal uuid32hex "output_path"`. Total count includes unselected calls
and warmup. Each selected ordinal gets its own AclCapture; unselected entries are
null. Distribute matching invocation order and UUIDs across ranks, with separate
rank output paths. Do not reuse a committed descriptor for the next invocation.
UniTest prepares plans for its ordinary Memory capture flow; another driver owns
its own plan and the mapping appropriate to its operator.

`TILEXR_PROFILING_RECORDS_PER_STREAM` configures this integration, default **8**.
With a plan enabled, values are strictly decimal, **8…65536**, and multiples of
**8**; invalid values fail initialization. This is distinct from the standalone
CaptureConfig default of 4096. Detailed ServerDedup can start with **512 or more**
records/lane, subject to the workspace budget. Check observed valid counts,
complete lanes and zero dropped records; 512 is not a completeness guarantee.
Setting capacity alone does not enable a capture.

The owner checks the consumed invocation count and waits for safe device completion
before collecting/exporting through SDK APIs. It uses actual launched lane counts,
requires complete nonempty records with no drops for each participating lane,
and writes independent records.v1 files to the planned paths. This is a table of
one-invocation captures, not latest-slot aggregation. Changed CommArgs/tiling layouts
require a coordinated rebuild of Host, all consuming kernels and downstream code.
Old runner `profile`/`breakdown` modes are rejected; normal perf keeps warmup,
stage barriers and external profiler measurement.

## Machine outputs, budgets and measurements

`generate_reports(..., *, manifest=None, artifacts=None, limits=None)` keeps the
original default three files. Explicit `artifacts=("json", "csv")` publishes
`analysis.json`, `analysis.csv` and `artifacts.json`; choices are `perfetto`,
`markdown`, `html`, `json`, `csv`. Only selected renderers run. Publication is
atomic for the selected set, and errors leave no partial published directory.

`InputLimits` configures cumulative files, bytes, records, streams (including
manifest-only placeholders), metrics, events, pairing actions, samples and estimated retained-model memory.
Defaults: 4096 files, 512 MiB input, 1,000,000 records, 65,536 streams/metrics/events,
1 GiB model estimate, `max_pairing_actions=1000000`, `max_samples=1000000`.
Each matched boundary is an action; every generated interval/instant is a sample.
Both limits accumulate across streams and capture-set members. Fanout, samples
and diagnostics are charged before allocation. Existing per-file 256 MiB and manifest 4 MiB bounds still
apply. The estimate is an admission bound, not an OS RSS guarantee. The parser
decodes and pairs one stream before the next, without retaining encoded input
copies. Statistics retain the bounded immutable run. Selected trace, JSON,
Markdown, HTML and CSV writers emit chunks/rows; legacy renderer functions still
return complete strings for callers that explicitly request them. This is not an
out-of-core statistics engine or hard process-memory isolation.

`build_analysis_document(run, analysis)` / `render_analysis_json` /
`render_analysis_csv` serialize the shared statistics, units, missing-value reasons,
coverage and provenance. `compare_reports(baseline, candidate, thresholds=None)`
accepts documents/paths and returns `pass`, `regression`, `incomparable` or
`missing_data`. `Threshold` has explicit metric/direction/absolute/relative and
optional stage ID; degradation must exceed both tolerances. Unknown/incomplete
context never becomes a regression. CLI `--baseline analysis.json` uses latency
median with `--threshold-relative` (default 0.05) and
`--threshold-absolute-us` (default 0.0); it also publishes `comparison.json` and
`artifacts.json`. Exit statuses
are 0 pass, 2 missing data, 3 regression, 4 incomparable, 1 fatal input/output.

`load_capture_set`, `analyze_capture_set` and `generate_capture_set_reports`
accept `tilexr_profiling_capture_set.v1` with explicit experiment/measurement,
iteration, attempt, UUID, mapping/records/manifest membership. File members are
relative to the set document. CLI uses `--capture-set set.json` in place of
`--mapping`; it rejects `--records`, `--manifest`, `--attribute` and
`--baseline` because the container owns member inputs. Warmup is retained but excluded from repeat distributions; captures
never pair together. See [capture-set contract](ADVANCED.md#capture-sets).

`SamplingPolicy.from_document(...).select(mapping, participants, iteration)` is
a pre-launch decision. `SamplingSelection.apply_manifest` preserves the expected
participants, remaps stage IDs and binds the filtered mapping; `cpp_header()`
provides the C++14/Ascend C marker guard. Producers consume selected sparse IDs
and this guard before launch. Reinterpreting old unfiltered records is invalid.

`collect_environment` conservatively reads host/toolkit facts, with an optional
caller-owned runtime probe; `reconcile_environment` rejects conflicting manual
claims. `CalibrationSession(read_cycles, ...)` brackets a caller-owned device
timer probe with a reference clock. Call `sample()` before and after the measured
operation, then `finish()` to obtain a validated sidecar (at least three samples).
The default monotonic reference is scoped to the host/process; a custom clock
requires its qualified ID. The probe must return the same device timer as records,
not ACL event duration. `load_calibration`, `convert_timestamp`, `derive_global_coordinates` and
`compare_global_order` consume qualified paired observations, check expiry,
error/drift/identity and refuse extrapolation or ambiguous ordering. They preserve
raw local records. A toolkit version does not discover timer frequency; automatic
target probe qualification and cross-device calibration still require hardware.
`examples/profiling/calibrate.py` provides the explicit Ascend950 Add4 probe and
pre/post measurement orchestration; its process-wide brackets may be imprecise
and must satisfy the caller's error/drift limits before a sidecar is published.

`attach_completion(manifest, snapshot)` imports SDK raw completion evidence into
`completion_snapshots`, preserving rank-local generation. EMPTY/WRITING mean only
unsubmitted. Conflicting capture IDs, participant IDs or commit flags fail before
publication. Rank shards may carry separate completion snapshots.

`collective_capture_id(seed, ordinal)` and
`build_collective_expected(mapping, capture_id, rank_size, logical_lane_count)`
construct the capture identity and independent expected set before execution.
They do not inspect successful shards. See the
[collectives producer contract](ADVANCED.md#collectives-integration)
for runtime identity, per-launch workload, mapping fingerprints and byte semantics.
