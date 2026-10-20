# Profiling v1 format and statistics

Both v1 disk formats remain readable after the SDK memory-layout migration.
There is no automatic timestamp-frequency discovery or cross-device calibration.

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
Refer to the [Add4 mapping template](../../examples/profiling/profiled_add4.mapping.template.json)
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

## DeviceView ABI 1

`DeviceView` is an aligned 72-byte POD. Its first eight uint32 fields are
`abiVersion`, `descriptorBytes`, `enabled`, `logicalStreamCount`,
`recordsPerStream`, `recordStride`, `stateStride`, `reserved`; then five uint64
addresses identify timestamps, event IDs, counts, dropped counts, and states.
Host and device assert sizes/offsets. Record stride is in elements; state stride
is in uint32 elements and is at least eight. Each array address is 32-byte
aligned. The descriptor is ordinary device memory, not a CommArgs or peer mapping.

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
or unlisted participants. Expected counts currently describe provided inputs.
An external expected-participant manifest is deferred.

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

## Provenance and deferred features

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

Deferred E01–E09 remain: multi-iteration containers, participant manifests,
generic byte/bandwidth inputs, machine summaries/comparisons, streaming memory
control, richer HTML interaction, selective sampling, automatic timer/calibration,
and generic borrowed-workspace/sparse-lane support. Existing producer-specific
performance-report requirements are unaffected.
