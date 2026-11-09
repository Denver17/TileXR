# Profiling formats and statistics

[Overview](../README.md) · [中文入口](../README.zh-CN.md)

Both v1 disk formats remain readable after the SDK memory-layout migration.
Shared boundaries use mapping.v2 with the same records.v1 binary format.
Timer frequency is never inferred from records or the installed toolkit.
Qualified calibration sidecars can supply a separate derived clock view; see the
[calibration contract](ADVANCED.md#clock-calibration).

## manifest.v1: explicit report inputs

`tilexr_profiling_manifest.v1` is a separate JSON sidecar; it does not add fields
to records.v1 or mapping.v1. Supply it with `--manifest` or the API's keyword-only
`manifest=` parameter. Attributes remain self-reported provenance, never an
implicit identity or byte-count interface. Unknown/duplicate structural keys,
invalid numeric types (including bool bytes), conflicting identities or metrics,
and wrong capture/mapping bindings are fatal before report publication.

The document limit is 4 MiB, depth 16 and 262144 JSON nodes. Participant,
metric and event arrays have at most 65536 entries. `mapping_sha256` hashes the
validated input JSON serialized with Python `json.dumps(sort_keys=True,
separators=(',', ':'), ensure_ascii=False, allow_nan=False)`, encoded as UTF-8.
Use `mapping_fingerprint(mapping)` rather than hashing the file's whitespace.
Stage IDs are zero-based mapping array indexes and names must match as well.

| Field | Contract |
|---|---|
| `schema`, `attempt_id`, `capture_id`, `mapping_sha256` | Required schema string, nonempty attempt ID, UUID string or null, exact mapping fingerprint |
| `participants` | Independent pre-execution list of `{stream_id, rank, logical_lane, sampled}`; stream ID must equal `(rank << 16) \| logical_lane` |
| Participant identity | Optional `host`, `device: {id, namespace}`, `acl_stream_label`, `physical_core`; device zero is valid, device identity is caller-declared |
| `stage_metrics` | Entries bind `stream_id`, `stage_id`, `stage` and `source`; known values require uint64 `bytes`, nonempty `byte_semantics`, `scope` |
| Metric scope | `per_interval` multiplies equal bytes by paired sample count; `per_lane_total` is the exact total for that lane/stage, including unequal repeats |
| Metric inapplicability | `status: "not_applicable"` requires `reason` and omits bytes/semantics/scope; do not encode missing information as zero or N/A |
| `environment`, `versions` | Known string fields: `soc`, `cann_version`, `driver_version`; `sdk_version`, `kernel_version`. Additional bounded JSON provenance is retained |
| `execution` | `kind: "compute"` explicitly makes communication backend/native status inapplicable; `"communication"` requires rank/stage declarations for readiness |
| Execution stages | `{rank, stage_id, stage, backend, native_status, kernel_version}`; optional boolean `framework_profiler`, `stage_barrier` are run provenance |
| `events` | Explicit observation list, including an empty list if the caller observed no events; absence means Host observation is unverified |
| `dropped_events` | uint32 cumulative lost diagnostic count, with saturation interpreted as a lower bound |
| `attributes` | Bounded self-reported JSON, retained without overriding records or normalized model facts |

Events require `{event_id, code, severity, phase, scope, count, source, resolved}`.
Severity is `info`, `warning` or `error`; scope is `attempt`, `rank` or `lane`.
Rank/lane scopes require `rank`/`stream_id` consistent with participants. Optional
fields are `message`, `status_code`, `acl_error`, `operation_id`, uint32 `retry`,
and paired `timestamp`/`clock_domain`. Repeated identical event IDs are deduplicated;
resolved state may progress to true, while conflicting event bodies are rejected.
Distinct retries retain distinct event IDs. Untimed and Host-clock diagnostics
remain in metadata, never receive an invented device timestamp or critical track.

`merge_manifest_shards(expected, shards, mapping)` accepts an independently built
expected manifest and an explicit list of `tilexr_profiling_manifest_shard.v1`
documents. A shard has the same fields plus owner `rank`; participants, metrics,
events and execution entries must belong to that rank. Shared metadata conflicts
and duplicate rank shards are rejected. Missing sampled rank shards remain in
`missing_shard_ranks`; policy-excluded ranks do not require a shard. A missing
Host observer is retained in `unobserved_event_ranks` as an observation gap, even if other ranks have events.
No collective, device synchronization or inference from successful files occurs.

`attach_host_events(manifest, snapshot)` consumes the SDK's
`tilexr_profiling_host_events.v1` document: schema, attempt/capture IDs, events and
dropped-events count. IDs must match; cumulative snapshots merge by stable event
ID, monotonic resolution and maximum dropped count. Separate attempts must not
be combined. The final merged manifest is validated by the regular public reader.

All mapped stages and expected lanes remain visible, including absent ones.
Metric states are `known` (including zero), `not_provided`, `not_observed`,
`not_applicable`, `incomplete` and `undefined_denominator`, with reasons. Bytes
and inclusive duration must cover the same samples; incomplete captures suppress
full-stage bandwidth. The unit is decimal GB/s, computed as
`sum(bytes) / sum(lane inclusive_us) / 1000`, named **lane-time effective bandwidth**.
It is not parallel wall-clock, physical-memory or network-counter bandwidth.
Rank details expose min/median/max, coverage and slowest rank without assuming
cross-device clock synchronization. Parent/child bytes are never auto-added.

Capture completeness, Host health and metadata readiness are separate. A unique
critical pair remains timing-usable even if that lane dropped records. A Close
error may leave capture complete but health incomplete. Resolved historical
errors remain visible. No manifest means missing participants are **unverified**.
Valid manifest plus an empty records list creates a diagnostic-only report;
explicit corrupt/missing records are still fatal. CLI returns 1 for fatal input
or publication errors, 2 for incomplete capture/coverage or unresolved Host error,
and 0 otherwise, including clearly labelled policy exclusion (`not_sampled`).
Unverified metadata or a zero exit code is not proof of full metric readiness.

HTML's default Run Metadata has eight entries. Expand provenance and metric
details for full declarations, states and formulas. HTML, Markdown and Perfetto
carry the identical shared report data; Perfetto stores it at `metadata.report`.
In HTML/Markdown stage tables, Bytes and bandwidth show numbers or `missing`;
Byte semantics shows only `yes`/`no`. HTML keeps these three columns on one line.
Keep full semantic labels and missing-value reasons in the folded shared report
data: shortening display text must not change calculations, states or interfaces.

Maintenance lesson: test caller → validation → model → statistics → all reports.
Existing model/renderer fields alone do not establish public support. Keep the
expected set independent, pair byte/time populations, and preserve missing values
instead of silently filling zero. Software fixtures do not prove device timing.

## mapping.v1

Strict JSON top-level fields are `schema="tilexr_profiling_mapping.v1"`,
nonempty `operator`, finite positive `cycles_per_us`, and `stages`.
Unknown or duplicate fields, nonfinite numbers, conflicting event IDs,
invalid parent references/cycles, and contradictory event definitions fail.
Mappings are at most 1 MiB, with at most 4096 stages and 65536 expanded events.

A stage has a unique `name`, a category (`total`, `setup`, `copy`, `issue`,
`wait`, `sync`, `compute`, `reduce`, `finalize`, `diagnostic`, or `other`), and
either a `begin`/`end` event pair or `instant`. Optional `parent` names a stage,
`track` selects a display track, and `critical` identifies the total interval.
`instances` supplies explicit begin/end pairs; `instance_range` expands
`count`, `begin`, `end`, and `stride`. Event IDs are nonzero uint32 values.
Refer to the [Add4 mapping template](../../../examples/profiling/profiled_add4.mapping.template.json)
and parser behavior tests for nested stages and explicit repeated instances.
Protocol metadata such as SoC/backend is supplied as report attributes rather
than adding unrecognized mapping.v1 keys.

| Field | Type and requirement | Default / constraints |
|---|---|---|
| `schema` | Required string | Exactly `tilexr_profiling_mapping.v1` |
| `operator` | Required string | Nonempty; descriptive, not a producer selector |
| `cycles_per_us` | Required number | Finite and positive; booleans rejected; caller-confirmed frequency |
| `stages` | Required array of objects | 1…4096 stages, at most 65536 expanded events |
| Stage `name` | Required string | Nonempty and unique in this mapping |
| Stage `category` | Required string | One of the categories listed above |
| Stage `critical` | Optional boolean | `false`; exactly one interval stage must be critical and have no parent |
| Stage `parent` | Optional string or null | Absent/null means no parent; reference an interval stage by name; no cycles or self-parent |
| Stage `track` | Optional string or null | Absent/null means the renderer's normal stage track; nonempty strings group display tracks |
| Stage `begin`, `end` | Required together for a simple interval | Distinct nonzero uint32 event IDs |
| Stage `instant` | Alternative event form | One nonzero uint32 event ID; cannot be critical |
| Stage `instances` | Alternative nonempty array | Objects containing only required `begin`/`end`; instance IDs are zero-based array indices |
| Stage `instance_range` | Alternative object | Exactly `count`, `begin`, `end`, `stride`; all nonzero uint32; count ≤32768 and within total event budget |

Choose exactly one event form per stage. All event IDs must be unique across
the mapping. A range instance `i` expands to `begin+i*stride` / `end+i*stride`,
with instance ID `i`; expansion must remain within uint32. A simple pair may
repeat sequentially in one critical interval. Overlapping instances need
distinct event pairs; pairing never guesses boundaries from event ordering.

This synthetic example covers all four forms. The frequency `1` is fixture
data, not a hardware recommendation:

```json
{
  "schema": "tilexr_profiling_mapping.v1",
  "operator": "format-example",
  "cycles_per_us": 1,
  "stages": [
    {"name": "total", "category": "total", "critical": true, "begin": 1, "end": 2},
    {"name": "checkpoint", "category": "diagnostic", "parent": "total", "instant": 3},
    {"name": "parts", "category": "compute", "parent": "total", "track": "work",
     "instances": [{"begin": 10, "end": 11}, {"begin": 20, "end": 21}]},
    {"name": "waits", "category": "wait", "parent": "total", "track": "work",
     "instance_range": {"count": 2, "begin": 30, "end": 31, "stride": 2}}
  ]
}
```

## mapping.v2: shared boundaries

Use `schema="tilexr_profiling_mapping.v2"` to let several stage/instance
boundaries reference one physical event. Stage forms, hierarchy, categories,
clock requirements and input bounds remain the same. The 65536-event mapping
limit counts expanded boundary references, including references to shared IDs.
The v1 parser still rejects globally duplicated event IDs.

Simple intervals and explicit `instances` accept numeric begin/end selectors
or objects with exactly `event` and `occurrence`, both nonzero uint32 values.
A number matches every occurrence of that event; an object selects its 1-based
occurrence within one logical stream and capture, in submitted record order.
Each interval must use different begin/end event IDs. `instant` and
`instance_range` retain their numeric forms and positive stride constraints.

The shipped Add4 uses its original slot 9 (event ID 20) to open the compute
parent and three cumulative partial sums. This complete mapping uses synthetic
frequency 1; real captures require a confirmed conversion:

```json
{
  "schema": "tilexr_profiling_mapping.v2",
  "operator": "Add4",
  "cycles_per_us": 1,
  "stages": [
    {"name": "kernel_total", "category": "total", "critical": true, "begin": 1, "end": 2},
    {"name": "copy_input", "category": "copy", "parent": "kernel_total", "begin": 10, "end": 11},
    {"name": "accumulate_window", "category": "compute", "parent": "kernel_total", "begin": 20, "end": 21},
    {"name": "copy_out", "category": "copy", "parent": "kernel_total", "begin": 30, "end": 31},
    {"name": "partial_sum", "category": "compute", "parent": "accumulate_window", "instances": [
      {"begin": 20, "end": 201}, {"begin": 20, "end": 202}, {"begin": 20, "end": 203}
    ]}
  ]
}
```

Each matching record dispatches to all subscribed boundaries, closing intervals
before opening new ones. Each interval closes independently and keeps the
original begin/end record indices. Shared starts do not duplicate raw records.
Unmatched or overlapping edges retain their incomplete diagnostics.

Occurrence selectors count each event ID once per raw record, independently of
other IDs, stages, ranks and measurements. A missing occurrence produces
`selector_not_observed`. If completion is absent or the stream reports drops or
overflow, all intervals using occurrence selectors are suppressed with
`selector_unverifiable`; numeric-selector intervals retain existing behavior.
Unreported producer omissions cannot be detected by records.v1.

Sampling keeps the unique selected event IDs and **every call** for each retained
ID, including predecessors needed to count an occurrence. A declared event that
does not yet match its requested occurrence is ignored after counting it.
Local tests additionally cover using the fourth input-copy end as a shared
boundary; the shipped Add4 has only the slot-9 configuration.

V2 provenance records `mapping_schema` and `pairing_sha256`; samples expose
their selectors in `attributes.pairing`. The pairing fingerprint hashes the
normalized schema, stages and boundary definitions, independently of input
paths and frequency. It supplements the manifest's unchanged raw-JSON mapping
fingerprint. Report comparison includes this boundary contract; changing the
start or occurrence is not automatically a performance regression. Historical
v1-to-v1 comparisons retain their existing contract.

Reusable lesson: a Recorder slot is not an event ID or an exported index.
Size the Recorder for its highest used slot, not only its marker count. Add4
uses consecutive slots 0–16 with capacity 17; sparse slots remain supported and
are compacted by Flush. Consecutive marker calls separately
read the timer and need not have equal timestamps. Share the original record
explicitly rather than merging nearby timestamps. Inclusive sums of cumulative
partial windows count overlap; use the existing union/exclusive interpretation.
Fanout is bounded separately by cumulative pairing-action/sample limits and
memory charges before allocation; raw record count alone is insufficient.

## records.v1

All integers are little-endian. Padding/reserved fields must be zero.

| File header field | Bytes |
|---|---:|
| Magic `TXRPRF1\0` | 8 |
| Major=1, minor=0, header bytes=48, record bytes=16, stream-header bytes=24 | 5 × uint16 |
| Endianness=1, reserved=0 | 2 × uint8 |
| Stream count | uint32 |
| Capture UUID in network UUID byte order (16 raw bytes) | 16 |
| Entire file size | uint64 |

| Each stream block header | Bytes |
|---|---:|
| Stream ID, record count, dropped count, flags | 4 × uint32 |
| Block size including its header | uint64 |

Each following record is `(timestamp:uint64, event_id:uint32, reserved:uint32)`.
Stream ID is `(rank << 16) | logical_lane`. Flag bit 0 is complete, bit 1 overflow;
unknown bits fail. Timestamp zero is valid. Timestamps must be nondecreasing;
equal timestamps retain submitted order. Duplicate stream IDs, mixed capture
UUIDs, truncated blocks, extra bytes, bad sizes or reserved fields are fatal.
The reader permits at most 4096 files, each at most 256 MiB.

New SDK snapshots preserve every configured lane. COMMITTED+count=0 becomes a
complete empty block, whose absent critical events are separately diagnosed.
EMPTY/WRITING become incomplete zero-record blocks; their count/payload is never
interpreted. A dropped prefix is exported with overflow and analyzed incomplete.
INVALID/unknown states or invalid committed counts/IDs fail collection. The
states themselves are an in-memory ABI, not new on-disk fields.

## DeviceView ABI 2

`DeviceView` is an 8-byte-aligned, 88-byte POD. Its first eight uint32 fields are
`abiVersion`, `descriptorBytes`, `enabled`, `logicalStreamCount`,
`recordsPerStream`, `recordStride`, `stateStride`, `reserved`; then five uint64
addresses identify timestamps, event IDs, counts, dropped counts, and states.
At byte offsets 72 and 80, uint64 `logicalLaneIdsAddress` and `generation`
complete ABI 2. A zero logical-ID address means dense lanes; otherwise it points
to a sorted uint32 lookup table with one ID (0…65535) per physical slot.
The optional expected generation rejects stale descriptor use; it is not a lock.
Host and device assert sizes/offsets. Record stride is in elements, at least the
logical capacity, and a multiple of 16. State stride is in uint32 elements and
is a positive multiple of 16. Workspace and array addresses are 64-byte aligned.
These constraints give independent scalar GM writers separate cache lines,
including event IDs when the logical capacity is eight. The Host rounds only
physical record stride; valid capacity/count and records.v1 contain no padding.
The descriptor is ordinary device memory, not a CommArgs or peer mapping.

ABI 1 descriptors are rejected by current writers. Earlier 32-byte-aligned arrays
or eight-element strides are also unsafe. Rebuild Host and Kernel together;
this memory ABI migration does not change disk v1 files. Sparse lane IDs are
restored in encoded stream identities. See the
[workspace contract](ADVANCED.md#workspace-leases) for lease lifetime,
generation and completion snapshots.

Each lane is exclusively owned: EMPTY=0 → WRITING=1 → COMMITTED=2;
INVALID=3 records a protocol error. Writers publish only valid prefix `[0,count)`
and dropped count, then completion. Count need not be a multiple of eight.
New writers cannot reuse a committed lane. Host reads only after successful
bound-stream synchronization. This is not a release/acquire polling API;
hardware scalar-store visibility still requires target CANN/NPU validation.

## Pairing, clock, and completeness

One stream must contain exactly one paired critical interval. Repeated ordinary
subintervals are supported within it; repeated kernel invocations require
separate captures. Unmatched edges and incomplete/overflowed streams retain
diagnostics. A complete supplied-input status does not verify absent rank files
or unlisted participants without a manifest. With a manifest, expected counts
come from its independent scheduled participant list; see manifest.v1 above.

Integer raw timer differences are computed before float conversion. Display
origin is the minimum timestamp within `(launch, rank, device)`; cores in that
group retain relative offsets, ranks are zeroed independently. `clock_domain`
describes an unverified device timer, while `display_origin` describes software
alignment. Unknown device identity remains unknown. `cycles_per_us` comes only
from the caller-confirmed mapping, never the records or a universal 1000 default.

## Statistics

`total_us` and `critical_lane_us` are inclusive sample sums: paired, untrimmed
durations, with overlaps counted repeatedly. Sample minimum/median/maximum and
instance counts keep that interpretation. Existing inclusive
`share_of_critical` uses the critical lane duration and can exceed 100%.

For a lane with unique critical C, stage coverage U is the union of its interval
samples intersected with C. `union_us` sums lengths of U across lanes (unions
are never formed across lanes/ranks). `exclusive_us` subtracts the intersection
of U with the union of explicitly declared direct child stages. Overlapping
parent instances share stage coverage; no implicit parent-instance matching is
invented. Instant samples contribute zero length. Work outside C contributes
only to inclusive statistics. Missing or multiple critical intervals make the
new relative coverage statistics unknown for affected stages.

`critical_lane_union_us` / `critical_lane_exclusive_us` and their shares use
the selected slowest critical lane. Shares are unknown for a zero/missing/
nonunique critical denominator. Sibling stages may overlap, so exclusive
shares need not add to 100%. Rank latency uses each rank's slowest lane;
minimum, median, and maximum summarize those rank durations.

Wait/sync pressure is the union of all wait and sync intervals on the critical
lane, clipped to C. Critical=2 us, wait=1.8 us, and nested sync=1.6 us produces
90% pressure. Its inclusive sample sum can still be 170%, labeled as a sum.
Raw trace samples are preserved by this statistical change. Before publication,
all final statistics must be finite: a finite single-lane duration does not
ensure that a sum, median average, or ratio is representable. Overflow returns
a fatal input error and does not publish a complete report.

Reusable lesson: merge and clip integer timestamps before converting to units.
Summing nested intervals or subtracting each child sample double-counts time;
clamping a wrong percentage to 100% hides that error. Test duplicates, adjacency,
overlap, outside-critical work, zero intervals, and large absolute timestamps.
Synthetic tests establish arithmetic, not target timer frequency/calibration.

## Provenance and extension schemas

Perfetto metadata, Markdown, and HTML share a single validated provenance model:
capture/stream identity, input paths and event IDs, analysis version, mapping
conversion, timer/origin, statistics, completeness scope, caller attributes, and
reported SDK/kernel/SoC/CANN/driver/backend/native status. Unknown environment
values display `unknown`; caller facts are explicitly distinguished from records.
Attributes cannot override protocol facts, timing or completeness. Values must
be finite JSON, depth ≤8, nodes ≤4096, keys 1…256 characters, encoded ASCII JSON
≤65536 bytes, and integers ≤256 bits. Invalid attributes fail before publication.
Text outputs escape untrusted strings and remain self-contained/offline.

API validation distinguishes omitted attributes (`None`) from an explicitly
supplied value: even an empty list, string, or scalar is rejected before any
report is published. Truth-value checks would silently accept malformed input.

Additional independent schemas preserve the strict v1 records/mapping members:
`tilexr_profiling_analysis.v1`, `tilexr_profiling_comparison.v1`,
`tilexr_profiling_artifacts.v1`, `tilexr_profiling_capture_set.v1`,
`tilexr_profiling_sampling.v1`, `tilexr_profiling_environment.v1`,
`tilexr_profiling_calibration.v1`, and `tilexr_profiling_completion.v1`.
See [API reference](API_REFERENCE.md), [machine output contract](ADVANCED.md#machine-outputs-and-comparisons),
[capture-set contract](ADVANCED.md#capture-sets), and
[workspace contract](ADVANCED.md#workspace-leases) for the versioned
fields and validation boundaries. Only a selected set is published when explicitly
requested; the original default three artifacts are unchanged.

Manifest `completion_snapshots` is an optional list of rank-owned SDK snapshots.
Each contains `schema`, exact `capture_id`, uint64 `generation`, and unique
`streams: [{stream_id, completion}]`, where completion is 0/1/2. It supplements,
never overrides, records commit flags. Generations need not match between ranks.

Explicit N/A byte declarations exclude that lane from both byte and bandwidth
time populations, even if its control role observes stage markers. They do not
conflict with another lane's applicable metric. Different actual byte semantics
still suppress aggregation; missing declarations and absent applicable intervals
remain incomplete. Entirely inapplicable ranks appear as `not_applicable`, never 0.
