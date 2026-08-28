# EP Memory 64P Unified Test Runner Design

## Status

- Date: 2026-08-24
- Scope: 64-rank Memory Dispatch and Combine test orchestration
- Entry point: `tests/ep/cluster/run_memory_dispatch_msprof_64p.sh`

## Goal

Provide one parameterized entry point for Dispatch, Combine, or sequential Dispatch and
Combine execution. The runner exposes exactly three user-facing modes:

```text
--op dispatch|combine|both
--mode functional|performance|breakdown
```

The default remains `--op dispatch --mode performance` for compatibility with the
existing no-argument performance workflow.

## Mode Contract

| Mode | msprof | Kernel profiling | Workload | Final result |
| --- | --- | --- | --- | --- |
| `functional` | off | both off | one correctness iteration | per-operator PASS/FAIL |
| `performance` | on | both off | 50 warmup + 150 timed iterations | last 50 rounds, slowest of 64 ranks per round, min/max/avg text |
| `breakdown` | off | selected operator profiling on | 50 warmup + 150 timed iterations | fastest/P50/slowest rank Chrome Trace JSON and elapsed time text |

`both` always runs Dispatch first and Combine second. The two operators are never
launched concurrently.

Performance mode does not publish a final JSON result. Per-node summary JSON files are
internal transport artifacts which preserve the 50 round values needed for cluster
reduction. The user-facing result is text only.

Breakdown mode profiles only the last timed iteration because the fixed DFX record is
overwritten on each launch. Dispatch uses profile buffer 0 and Combine uses profile
buffer 1. Each operator produces three trace files selected from all 64 ranks:
fastest, P50, and slowest. The terminal result includes role, rank, elapsed time, and
the corresponding JSON path.

## Build Matrix

| Selection | Functional/Performance | Breakdown |
| --- | --- | --- |
| Dispatch | Dispatch OFF, Combine OFF | Dispatch ON, Combine OFF |
| Combine | Dispatch OFF, Combine OFF | Dispatch OFF, Combine ON |
| Both | Dispatch OFF, Combine OFF | Dispatch ON, Combine ON |

Before a run, the runner reads `build_ep/CMakeCache.txt`. It rebuilds only when the
required profiling options or installed demo are absent. A rebuild is followed by
parallel artifact distribution to the other seven nodes. This prevents instrumented
Kernel binaries from being used for msprof performance data.

## Data Flow

### Functional

The runner invokes the selected node script on all eight hosts. Each node launches its
eight local ranks in correctness mode. No msprof raw directory or trace is created.

### Performance

Each host runs one `msprof --application` around its eight-rank node script. Every local
`op_summary_*.csv` is filtered by the exact Kernel name. Each host retains the last 50
matched executions per card and reduces each round across its eight cards. The master
then reduces the eight node series per round, yielding the slowest of all 64 cards, and
prints min/max/avg across those 50 values.

Kernel names are fixed:

- Dispatch: `tilexr_ep_dispatch_memory_kernel`
- Combine: `tilexr_ep_combine_memory_kernel`

### Breakdown

The selected node script runs with its existing `profile` test mode and copies the
final DFX record to the rank log. The master gathers 64 rank logs and invokes the
operator-specific trace exporter. msprof is never started in this mode.

## Synchronization and Failure Handling

- Source synchronization from Windows to the master uses the configured Mutagen
  one-way-safe session.
- The master distributes runtime scripts and build artifacts with parallel `scp` or
  `rsync`; Mutagen is not used between servers.
- Before each operator, the runner probes all eight hosts in parallel. Any NPU process
  means busy; probes repeat every 5 seconds and stop the run after 120 seconds.
- A failure on any host stops the current operator and prevents a later operator in
  `both` from starting.
- Raw msprof data, rank logs, traces, and internal summaries use operator-specific run
  IDs and directories.

## Compatibility and Non-goals

- Do not change operator APIs, Kernel logic, profile ABI, trace schema, demo behavior,
  cluster topology, or communication endpoints.
- `run_memory_dispatch_node.sh` and `run_memory_combine_node.sh` retain their existing
  four required positional arguments. Both accept the same optional fifth warmup
  argument.
- Existing environment files remain authoritative for case parameters.
- Node scripts remain focused launch helpers; the unified runner owns the cluster-wide
  NPU availability gate.

## Verification

Local validation covers Bash syntax, CLI validation and help output, Python syntax,
and synthetic CSV reduction for both Kernel names. Server validation covers one mode
at a time and verifies the expected build flags, 64-rank completion, result formatting,
and artifacts. Hardware performance values are reported but are not acceptance
thresholds for this orchestration change.

## Implementation and Validation Record

Implemented on 2026-08-24 in `run_memory_dispatch_msprof_64p.sh`, with the Combine
node warmup interface aligned in `run_memory_combine_node.sh`.

Validation covered:

- Bash syntax, CLI rejection, all six mode-selection mappings, both case environment
  files, generated run ID limits, NPU preflight wiring, and Python syntax;
- synthetic eight-input Dispatch and Combine `op_summary` data, including exact Kernel
  matching, last-50 selection, per-round maximum, and min/max/avg;
- the existing Dispatch trace exporter unit test;
- 64-rank `both + functional`, with all Dispatch and Combine ranks passing;
- 64-rank `both + performance`, with separate text-only Dispatch and Combine results;
- 64-rank `both + breakdown`, including the profiling rebuild and six non-empty,
  syntactically valid Trace JSON files without `displayTimeUnit`.

The final validation run was breakdown mode, so the deployed artifacts have both
profiling switches enabled. A later functional or performance invocation detects this
state, rebuilds both switches as `OFF`, and distributes the non-instrumented artifacts
before launching ranks.
