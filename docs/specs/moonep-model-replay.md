# MoonEP Model Replay Specification

## Scope

The replay tools under `tools/moonep/model_replay_*` capture routing metadata and
stage configuration from a model-shaped run, replay the same MoonEP stages, and
compare correctness and performance with explicit provenance. They are
validation tools, not an alternative operator implementation.

## Artifact Contract

A replay artifact contains:

- schema/version and workload identity;
- `S`, `K`, `H`, expert/rank counts, dtype, and stage mode;
- compact route data with integrity metadata;
- source, build, CANN, driver, framework, and hardware provenance when known;
- performance samples and profiler/barrier state when results are recorded.

Cache identity includes every field that can change route interpretation or
operator behavior. An artifact whose provenance differs from the current run is
usable only when labeled as checked-in/reference data; it is not presented as a
fresh same-environment capture.

Writers publish metadata and payload atomically so another rank or process never
observes a complete manifest with an incomplete route file. Readers validate
schema, dimensions, checksums, and required provenance before launching.

## Replay Execution

The orchestrator creates the same communicator/stage sequence as the model-side
flow and preserves the public `Buffer`/`MoonEPCommPlan`, async-event, zero-copy,
and return contracts. Required ranks rendezvous before stage execution and use a
stage barrier between Dispatch and Combine.

Dispatch uses `group_credit` by default. `group` is allowed only for an explicit
task. `legacy` is never selected as a default, fallback, qualification, or
diagnostic mode.

The framework NPU profiler and the Dispatch/Combine stage barrier remain enabled on
both model and replay sides during performance comparison. Kernel compile-time
profiling, trace dumps, flag/plan/error dumps, and other diagnostics are disabled
for the baseline unless they are the explicit A/B variable.

## Comparison and Reporting

Correctness comparison identifies the first divergent stage and tensor, then
reports later differences as consequences. Performance comparison uses matching
shape, ranks, mode, profiler/barrier state, warmup, iterations, and artifact
provenance.

Every stage report includes backend, native status, kernel version, algorithm or
communication data volume, human-readable latency, and bandwidth where
applicable. Multi-rank results include median, minimum, and maximum across all
ranks. Missing ranks or incomplete stages invalidate a runnable/model-complete
claim.

## Failure Handling

- Invalid or partial cache artifacts fail before communicator launch.
- Rendezvous and per-stage failures identify the missing rank and first stage.
- A stale installed binary or mismatched source SHA invalidates performance and
  behavior comparison until provenance is corrected.
- A timeout is not evidence of a communication root cause; debugging follows the
  first state, queue, owner, and provenance boundary.

## Verification Boundary

Python tests cover metadata, cache, CLI, comparison, and orchestration behavior.
Model/replay validation must run the full required rank and stage set on the
claimed hardware. A single-operator, partial-rank, Host, or simulator result does
not prove a complete model replay. Detailed debugging workflow and retained
evidence boundaries are maintained in
`docs/moonep/MINDSPEED_DEBUGGING_EXPERIENCE.md`.
