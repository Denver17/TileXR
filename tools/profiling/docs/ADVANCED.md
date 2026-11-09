# Profiling advanced usage

[Overview](../README.md) · [中文入口](../README.zh-CN.md) · [User guide](USER_GUIDE.md)

This document groups optional workflows. Start with ordinary `Recorder` /
`AclCapture` integration; use [API_REFERENCE.md](API_REFERENCE.md) for signatures
and lifecycle contracts, and [FORMAT.md](FORMAT.md) for schemas and statistics.

## Architecture

The header-only SDK records one kernel invocation per capture. The Host owns the
ACL context/stream, launch, rank coordination and completion point. The SDK owns
capture storage or a borrowed reservation, validates readback and exports records.
The independent Python package pairs events and produces reports without CANN.

Each lane has one writer. DeviceView ABI 2 is 88 bytes; deploy matching Host and
Kernel headers. Records remain `records.v1`; mappings can use v1 or v2. Shared
begin/end events use [mapping.v2](FORMAT.md#mappingv2-shared-boundaries) without
duplicating raw records. Logical capacity excludes physical cache-line padding.
The `tilexr/` include name does not require the TileXR communication runtime.

`Collect` seals a capture before waiting on its stream. Successful collection is
idempotent; `Export` can retry from its retained snapshot without ACL work.
`Save` combines them. Check `Close` before releasing borrowed ACL resources;
failed synchronization retains storage. Destruction neither exports nor replaces
explicit cleanup checks. Disabled device instrumentation and Host capture are
independent: align `CaptureConfig.enabled` with the device build switch.

## Manifests and health

Prepare the expected rank/lane population before execution. Supply it with
`--manifest capture.manifest.json` or `manifest=` to `generate_reports`, together
with known device identities, execution/shape/version information and per-stage
bytes or justified N/A. Without a manifest, complete covers supplied inputs only.
See [manifest.v1](FORMAT.md#manifestv1-explicit-report-inputs).

Each rank writes its own metadata shard. Merge an explicit returned shard list
with `merge_manifest_shards(expected, shards, mapping)` against the original
expected population; never infer expectations from successful files or edit one
shared manifest concurrently. SDK `ManifestProducer` / `ExportManifestShard`
bind the planned attempt and mapping fingerprint to a capture.

`AclCapture::Events()` exposes bounded cumulative Host observations. Attach them
with `attach_host_events`; preserve the original SDK/ACL status if observing or
exporting health fails. Export a fresh snapshot after retries. Records publication
and diagnostic publication are separate retry boundaries.

`ExportCompletion` emits raw `EMPTY=0`, `WRITING=1`, `COMMITTED=2` states and a
rank-local generation; Python `attach_completion` checks capture/participant
bindings. EMPTY/WRITING mean unsubmitted, not proof of kernel start. Noncommitted
payload/counts are not interpreted. Timing usability, completeness and metadata
readiness remain independent; missing observations stay unverified. Without an
expected manifest, missing rank/lane counts are unknown rather than zero.

## Machine outputs and comparisons

Default publication produces Perfetto, Markdown and HTML. Select machine views
with `--artifacts json csv`; explicit selection also writes `artifacts.json`.
All views share statistics, units, missing-value reasons, coverage and provenance.
CSV protects formula-like text; JSON retains lossless values. Publication is
atomic for the selected output set and never overwrites existing reports.

```bash
python3 -m tilexr_profiling --mapping operator.mapping.json \
  --records baseline.records.bin --manifest baseline.manifest.json \
  --artifacts json csv --output-dir baseline-report
python3 -m tilexr_profiling --mapping operator.mapping.json \
  --records candidate.records.bin --manifest candidate.manifest.json \
  --baseline baseline-report/analysis.json \
  --threshold-relative 0.05 --threshold-absolute-us 0.5 \
  --artifacts json html --output-dir candidate-report
```

`compare_reports` accepts analysis documents or bounded `analysis.json` files.
A regression must exceed both absolute and relative tolerances; equality passes.
The relative tolerance is a fraction of the baseline. A zero baseline has no
relative-change ratio. `Threshold` also supports explicit metric, stage and
higher/lower-is-better direction. Comparison publishes `comparison.json`:

| Exit | Meaning |
|---|---|
| 0 | Pass |
| 1 | Fatal input/output error |
| 2 | Missing/incomplete data; CLI usage errors also return 2 without reports |
| 3 | Regression |
| 4 | Incomparable known contexts |

Complete coverage, healthy observations and known identity/shape/clock context
are required. SoC, CANN/driver, participant/device namespace, conversion and its
basis, execution, sampling, workload semantics, stages and statistics must agree.
For communication, profiler/barrier settings also agree. Unknown provenance is
`missing_data`; different known context is `incomparable`, not a regression.
Comparison does not authenticate caller declarations. Bandwidth uses declared
bytes divided by matching inclusive lane time, not network counters or parallel
wall-clock throughput.

`--max-input-bytes`, `--max-records` and `--max-memory-bytes` tighten cumulative
admission limits, not process RSS. Pairing actions and generated samples are also
bounded before allocation, including shared-boundary fanout and capture sets.
See the [API budget defaults](API_REFERENCE.md#machine-outputs-budgets-and-measurements).
HTML search/filter/sort/folding changes visible rows only; global health and
coverage remain visible and statistics never change. Perfetto reserves every
main stream ID before allocating grouped/instance tracks; streaming and
in-memory output use the same event order and track identities. Host diagnostics
stay in metadata because uncalibrated host/device clocks have no shared axis.

## Capture sets

Use a new capture for every invocation. A `tilexr_profiling_capture_set.v1`
container groups 1–1024 explicitly bound measurements without joining their
events. IDs, attempt IDs and non-null capture UUIDs must be unique.

```json
{
  "schema": "tilexr_profiling_capture_set.v1",
  "experiment_id": "add4-repeat",
  "measurements": [
    {
      "measurement_id": "iteration-0-attempt-0",
      "iteration": 0,
      "attempt_id": "attempt-0",
      "capture_id": "018f6f5e-41a0-7d31-9ad1-82d61fc0a001",
      "warmup": false,
      "mapping": "operator.mapping.json",
      "records": ["rank0.records.bin", "rank1.records.bin"],
      "manifest": "attempt0.manifest.json"
    }
  ]
}
```

```bash
python3 -m tilexr_profiling --capture-set captures/set.json \
  --artifacts json csv --output-dir repeat-report
```

Member paths are relative POSIX paths from the container directory; absolute
paths, traversal and escaping symlinks fail. Object input requires `base_dir`.
Do not combine `--capture-set` with `--mapping`, `--records`, `--manifest`,
`--attribute` or `--baseline`. The container owns these declarations.

Every member is validated, including warmup. Missing files are fatal; missing
expected rank records yield an incomplete member/set. Repeat groups require
matching mapping, environment, execution, versions, participants, sampling and
byte contracts. Iteration identity is excluded from the grouping key. Warmup and
incomplete members remain visible but are excluded from complete distributions;
each sample uses that member's slowest usable lane. Warmup-only sets are
`not_sampled`. One cumulative input/model/output budget covers the whole set.
Member reports, the set overview and selected artifacts publish atomically.

## Workspace leases

`WorkspacePool` registers caller-owned ordinary device allocations by exact
size/context. It owns reservations, never allocations. Ranges are disjoint,
addressable and 64-byte aligned. `Acquire(bytes, context, &lease)` returns a
move-only reservation with a nonwrapping generation. Initialize through
`AclCapture::InitializeBorrowed(config, stream, std::move(lease))`; do not launch
through a lease's raw address. Initialization clears the layout and uploads its
descriptor and immutable sparse lane lookup.

Keep the allocation, pool, context and stream alive until `Close` succeeds and
no leases are busy. Failed safe-close retains the reservation; unsafe destruction
quarantines it. Generation checks detect stale handles but do not replace stream
completion or ownership. Each initialization retry must recheck retained
size/context/generation, including retries through ordinary `Initialize`.

`logicalLaneIds` is empty for dense IDs or a sorted unique uint32 list of values
0–65535 with exactly `logicalStreamCount` entries. This count is physical slots;
writers resolve logical IDs and skip unselected IDs. Export restores
`(rank << 16) | logical_lane`. Cache-line isolation applies to physical slots.
The pool serializes reservations; a single capture/lease is not concurrent.

## Selective sampling

`SamplingPolicy.from_document` accepts `tilexr_profiling_sampling.v1`, a mandatory
source and optional unique `iterations`, `stream_ids`, and `stages`. An omitted
selector means all; an empty one means none. `select(mapping, participants,
iteration)` keeps the full expected population, marks excluded lanes
`sampled=false`, preserves the critical stage and selected ancestors, and returns
sparse IDs, a filtered mapping and `event_ids`.

Consume this selection before execution: configure sparse lanes and guard
markers with the predicate from `cpp_header()` before recording. Use
`apply_manifest` to rebind metrics to the returned stage IDs/names and mapping
fingerprint. Filtering the mapping of an already unfiltered capture is invalid.
The policy does not rewrite a compiled kernel or infer exclusions from missing
records. Deliberate exclusion is `not_sampled`; resource/budget failure is a
`capture_skipped` observation. Shared event IDs retain every occurrence, including predecessors required by
one-based occurrence selectors. The minimal Add4 records every declared event
and uses only the SDK compile switch.

## Clock calibration

`collect_environment` reads bounded Host/toolkit metadata and optional
caller-owned runtime observations. Each fact keeps its source/status;
`reconcile_environment` rejects conflicting manual values. It does not initialize
ACL, infer a runtime SoC from the compiler target, or discover timer frequency.

`CalibrationSession` brackets an actual device timer callback with a qualified
reference clock. Collect at least three samples before/after the measured
operation, then call `finish`. The `tilexr_profiling_calibration.v1` sidecar binds
capture UUID, host/device namespace, clock domain, reference clock, method/source,
validity and error/drift limits to 3–4096 raw `{cycles,before_ns,after_ns}` samples.
Brackets progress without overlap; validity is at most one hour. The parser fits
from raw observations, retaining integer anchors for large uint64 cycles; it does
not trust supplied coefficients.

`load_calibration` / `convert_timestamp` enforce binding, validity, sampled range,
error and drift and refuse extrapolation. `derive_global_coordinates` requires
manifest identity on every lane, a shared qualified reference clock and mapping
frequency consistency, while preserving original local records. Overlapping
uncertainty intervals yield `indeterminate_within_error` for global order.

The optional [Add4 timer probe](../../../examples/profiling/calibrate.py) uses an actual
Add4 launch inside each reference bracket, validates device/record identity,
and brackets an explicit measurement command. Process startup widens uncertainty;
precision is never assumed. The default reference is host/process-scoped
monotonic time and does not synchronize hosts. A documented timer conversion or
aligned timeline alone does not qualify global ordering or calibration accuracy.

## Collectives integration

TileXR collectives use the same SDK with per-launch capture ownership. Enable
`TILEXR_COLLECTIVES_ENABLE_PROFILING`, set `TILEXR_PROFILING_OUTPUT_DIR`, and share
a fresh nonzero `TILEXR_PROFILING_CAPTURE_ID` seed across ranks. The bounded queue
prepares up to four captures / 64 MiB. Acquisition does not synchronize or drain;
resource exhaustion records a skip and business execution continues with null.
Drain at the existing safe cleanup point before ACL teardown. Failed items stay
owned and successful publication/close is skipped on retry.

Choose the actual [coarse mapping](../../../src/collectives/kernels/tilexr_collectives_coarse.mapping.json)
or [A5 mapping](../../../src/collectives/kernels/tilexr_alltoall_a5.mapping.json),
confirm its conversion and bind its fingerprint through
`TILEXR_PROFILING_COARSE_MAPPING_SHA256` / `TILEXR_PROFILING_A5_MAPPING_SHA256`
(or shared `TILEXR_PROFILING_MAPPING_SHA256`). Optional CANN/driver/clock-basis
variables add caller provenance; runtime identity comes from the Host/ACL.
Missing identity/fingerprint cannot claim full producer readiness.

`ConfigureProducer` provides a default; each `Acquire(..., producer)` deep-copies
that launch's actual shape, workload and execution information. Do not reuse the
first launch's mutable metadata for later shapes. Drain emits rank-owned shards,
completion and Host events without replacing already published records.

Before execution, use `collective_capture_id(seed, ordinal)` (ordinals 0–3) and
`build_collective_expected(mapping, capture_id, rank_size, lane_count)` to prepare
independent expectations. Merge only explicit returned shards. Ordinary kernels
use their actual AIV lane count; grouped A5 uses 48 logical lanes (32 send + 16
receive), distinct from blockDim 33. Missing ranks remain expected and incomplete.
Warmup has no capture; matching launch order and sampling across ranks is required.

Ordinary stages declare algorithm input bytes, apportioned as
`input_bytes / lanes + (i < input_bytes % lanes)`; ReduceScatter expands receive
count by rank count. This models logical input, not physical traffic. For grouped
A5 with R ranks and B bytes per peer:

| Stage | Planned bytes per rank |
|---|---|
| `send_all`, `send-put-signal` | `(R - 1) * B` |
| `receive_loop`, `receive-copy`, `sdma-submit` | `(R - 1) * B` |
| `self_copy` | `B` on logical lane 32 |
| `kernel_total` | `(2 * R - 1) * B` |

Use `per_lane_total` for variable per-peer intervals; control/wait roles declare
N/A with a reason. Execution metadata has rank/stage scope, including N/A lanes:
total `UDMA+SDMA`, send `UDMA`, receive/setup/copy `SDMA`. Validate exporter output
through the strict Python consumer; in-memory vectors alone miss scope conflicts.
This accounting does not prove device transfer, UDMA/SDMA correctness or timing.
