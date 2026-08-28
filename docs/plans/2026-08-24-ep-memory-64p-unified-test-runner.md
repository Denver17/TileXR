# EP Memory 64P Unified Test Runner Implementation Plan

## Goal and scope

Implement the approved design in
`docs/specs/2026-08-24-ep-memory-64p-unified-test-runner-design.md`. The work is limited
to cluster test orchestration, node-script argument parity, performance reduction, and
documentation. Operator, demo, Host, and Kernel behavior are out of scope.

## Task 1: Generalize the orchestration entry point

**Objective and role:** Turn `run_memory_dispatch_msprof_64p.sh` into the stable runner
for the two operator and three mode dimensions.

**Scope:** `tests/ep/cluster/run_memory_dispatch_msprof_64p.sh`.

**Constraints:** Preserve fixed 64-rank topology, 50 warmup, 150 timed iterations, last
50 performance samples, Chinese stage headings, parallel server distribution, and the
5-second/120-second cluster NPU availability gate. Do not invoke msprof outside performance mode.

**Acceptance:** All six `op x mode` combinations resolve to the correct node script,
Kernel name, build flags, execution mode, and output contract. `both` is sequential.
`bash -n` passes and invalid options fail before remote activity.

**Artifacts and interfaces:** User CLI, operator-specific log/raw/trace directories,
terminal result sections, and the existing node-script positional interface.

## Task 2: Align node-script warmup contracts

**Objective and role:** Allow the orchestrator to enforce identical warmup counts for
Dispatch and Combine.

**Scope:** `tests/ep/cluster/run_memory_combine_node.sh`.

**Constraints:** Preserve all existing environment validation and launch behavior.
Only add the optional fifth warmup argument already supported by Dispatch.

**Acceptance:** Four arguments retain environment-file behavior; five arguments use
the supplied non-negative warmup count; invalid arity or values fail. `bash -n` passes.

## Task 3: Integrate result production

**Objective and role:** Reuse `tools/ep/msprof_kernel_stats.py` for per-node and cluster
performance reduction, and reuse the existing operator trace exporters for breakdown.

**Scope:** Prefer orchestration-only changes. Modify the Python reducer only if a
verified interface gap prevents both Kernel names or text-only final results.

**Constraints:** Performance final output is text only. Internal summaries must retain
all 50 per-round maxima. Breakdown emits three Chrome Trace JSON files per operator.

**Acceptance:** Synthetic data proves exact Kernel filtering, last-50 selection, and
maximum-per-round reduction for both names. Trace invocation uses 64 rank logs and
prints each selected rank's elapsed time and output path.

## Task 4: Validate locally and on the cluster

**Objective and role:** Detect scripting and integration errors before reporting the
runner ready.

**Scope:** Read-only checks plus task-scoped logs/artifacts under the existing test log
directories.

**Constraints:** Use Git Bash and `python`. Synchronize Windows to the master with the
configured Mutagen one-way-safe session, and use parallel `scp`/`rsync` between
servers. Before each operator, check all eight hosts in parallel; any NPU process means
busy, retry every 5 seconds, and stop after 120 seconds.

**Acceptance:** Local Bash and Python checks pass. When the cluster is usable, exercise
the requested runner modes, confirm expected profiling flags, and verify output/artifact
contracts. If any NPU process remains for 120 seconds, stop without launching
ranks and report the boundary.
