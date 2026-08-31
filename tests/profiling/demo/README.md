# Profiled Add4 Demo

This single-NPU demo shows the complete producer contract for the generic
profiling tool without adding operator knowledge to `tools/profiling`.

The fixed-shape kernel computes:

```text
output[i] = input0[i] + input1[i] + input2[i] + input3[i]
```

Four Vector cores process one aligned 2,048-element float32 tile each. Every
core writes exactly 20 records to a disjoint device region:

| Event IDs | Stage | Parent | Instances per core |
|---|---|---|---:|
| `1`, `2` | `kernel_total` | root | 1 |
| `10`, `11` | `copy_input` | `kernel_total` | 4 sequential intervals |
| `20`, `21` | `accumulate_window` | `kernel_total` | 1 |
| `100/101`, `110/111`, `120/121` | `partial_sum[0..2]` | `accumulate_window` | 3 overlapping intervals |
| `30`, `31` | `copy_out` | `kernel_total` | 1 |

The per-core hierarchy is:

```text
kernel_total
|-- copy_input x4
|-- accumulate_window
|   |-- partial_sum[0]
|   |-- partial_sum[1]
|   `-- partial_sum[2]
`-- copy_out
```

All three `partial_sum` instances begin before the first vector addition and
end after their corresponding partial result is complete. Their intervals
therefore overlap, like independently tracked per-token work in a pipelined
operator. Parent names and instance order live only in the supplied JSON mapping;
the Kernel still records one timestamp and one numeric event ID per marker.
The Perfetto renderer places each overlapping instance on a synthetic lane
directly after its Core lane because crossing complete slices are invalid on a
single Perfetto thread track.

Repeated copy IDs deliberately demonstrate sequential interval pairing, while
the partial-sum IDs demonstrate overlapping instance pairing. The
marker itself is `TileXR::ProfilingRecorder::RecordMarker` from
`src/include/tilexr_profiling.h`. It records only `GetSystemCycle()` and an
integer event ID into the header-defined recorder's prefixed local arrays. The
Kernel calls the common recorder flush only after `kernel_total` ends. The
operator Host validates only the numerical result. `TileXRCommDestroy` exports
`capture.records.bin`, while `profiled_add4.mapping.json` supplies the event
mapping directly to the tool.

## How To Write The Mapping

The mapping is written by the operator developer who owns the Kernel marker
constants. It is normal JSON, so the real file cannot contain comments. Keep
the file next to the operator demo and make every numeric ID match the ID used
by `RecordMarker` exactly.

The complete Add4 mapping is
[`profiled_add4.mapping.json`](profiled_add4.mapping.json). Its top-level fields
are:

| Field | Meaning | Rule |
|---|---|---|
| `schema` | Mapping protocol version | Must be `tilexr_profiling_mapping.v1` |
| `operator` | Operator name shown in reports | A non-empty display string; it never selects operator code |
| `cycles_per_us` | Device cycles in one microsecond | A finite positive number for the actual timestamp source and platform |
| `stages` | Event meanings and hierarchy | A non-empty array; array order generates internal stage IDs |

The tool converts an interval with:

```text
duration_us = (end_timestamp - begin_timestamp) / cycles_per_us
```

Every stage supports these common fields:

| Field | Meaning | Rule |
|---|---|---|
| `name` | Stage name shown in reports and Perfetto | Must be unique within the mapping |
| `category` | Producer-neutral analysis category | One of the supported categories listed below |
| `parent` | Parent stage name | Optional; must refer to an interval stage and must not create a cycle |
| `critical` | Defines per-core Kernel latency | Optional, default false; exactly one root interval must be critical |

Supported categories are:

```text
total, setup, copy, issue, wait, sync, compute,
reduce, finalize, diagnostic, other
```

Category affects grouping, colors, and generic bottleneck findings. It does not
change Kernel execution.

### Choose One Event Form Per Stage

A stage must use exactly one of the following forms.

Use `begin` and `end` for a normal interval:

```json
{
  "name": "copy_input",
  "category": "copy",
  "parent": "kernel_total",
  "begin": 10,
  "end": 11
}
```

The same IDs may appear repeatedly in one core's records when intervals are
strictly sequential. Add4 therefore records `10, 11` four times to create four
`copy_input` samples. A second `begin` before the matching `end` is incomplete
instead of being guessed.

Use `instant` for a timestamp with no duration:

```json
{
  "name": "checkpoint",
  "category": "diagnostic",
  "parent": "kernel_total",
  "instant": 40
}
```

Use `instances` only when occurrences of the same logical stage may overlap:

```json
{
  "name": "partial_sum",
  "category": "compute",
  "parent": "accumulate_window",
  "instances": [
    {"begin": 100, "end": 101},
    {"begin": 110, "end": 111},
    {"begin": 120, "end": 121}
  ]
}
```

The array positions automatically become `instance_id` values `0`, `1`, and
`2`. These are instances inside every individual core stream, not different
cores. Add4 uses separate pairs because all three `partial_sum` intervals are
open at the same time. The corresponding Kernel constants are:

```cpp
constexpr uint32_t kEventPartialSum0Begin = 100U;
constexpr uint32_t kEventPartialSum0End = 101U;
constexpr uint32_t kEventPartialSum1Begin = 110U;
constexpr uint32_t kEventPartialSum1End = 111U;
constexpr uint32_t kEventPartialSum2Begin = 120U;
constexpr uint32_t kEventPartialSum2End = 121U;
```

All event IDs must be globally unique, nonzero `uint32` values within one
mapping. The Kernel may record a declared ID repeatedly, but two mapping entries
must never assign the same ID to different meanings.

### Add4 Mapping Walkthrough

| Mapping entry | Why it is written this way |
|---|---|
| `kernel_total`, IDs `1/2` | The only critical root interval; its duration is the per-core Kernel latency |
| `copy_input`, IDs `10/11` | One child definition reused for four non-overlapping input copies |
| `accumulate_window`, IDs `20/21` | Parent interval covering all partial additions |
| `copy_out`, IDs `30/31` | Output-copy child interval inside `kernel_total` |
| `partial_sum.instances` | Three separately identified children that overlap inside `accumulate_window` |

Write a new operator mapping in this order:

1. List every `RecordMarker` event constant used by the Kernel.
2. Choose one root interval that represents total Kernel latency and mark it `critical`.
3. Group the remaining IDs into normal intervals, instant events, or explicit overlapping instances.
4. Add `parent` names to describe containment without writing numeric stage IDs.
5. Check that every event ID is nonzero, unique in the mapping, and identical to its Kernel constant.
6. Set `cycles_per_us` from the target platform's timestamp conversion.
7. Generate a report and treat unknown IDs, unmatched pairs, or containment diagnostics as mapping or instrumentation errors.

Do not add capture IDs, run IDs, streams, ranks, cores, launches, Host names,
device names, metrics, or diagnostics to the mapping. The records file supplies
capture and stream identity; the tool derives rank/core from `stream_id` and
uses fixed version-1 timing and launch semantics.

## Run On Ascend950

Use a genuinely idle device. The script does not select or reserve hardware:

```bash
TILEXR_ADD4_DEVICE_ID=0 tests/profiling/demo/run_demo.sh
```

An optional first argument chooses a new output directory. Existing paths are
rejected so a previous capture cannot be overwritten. Passing `1` as the
second argument makes the runner check `npu-smi` immediately before launch and
refuse to use a device that already has a process. The build uses two jobs by
default; set `TILEXR_ADD4_BUILD_JOBS` only when the host has spare CPU.

The capture directory contains:

```text
capture.mapping.json
capture.records.bin
report/perfetto_trace.json
report/analysis.md
report/report.html
```

The runner enables the communication-domain capture with
`TILEXR_PROFILING_RECORDS_PATH`. The capture UUID is stored in the binary
records and does not need to be duplicated in the mapping. The runner also
disables IPC, UDMA, and SDMA because this single-rank arithmetic demo does not
exercise communication. The demo currently targets Ascend950 and uses the
platform conversion of 1,000 system cycles per microsecond. Production JSON
mappings must publish the conversion for their actual device and timestamp
source.

See [`tools/profiling/OPERATOR_PROFILING_GUIDE.md`](../../../tools/profiling/OPERATOR_PROFILING_GUIDE.md)
for the reusable operator instrumentation architecture.
