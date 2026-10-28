# TileRelay implementation plan

Historical execution record. The [ABI-v1-only design](../specs/2026-10-19-tilerelay-v1-only-design.md)
supersedes pipeline/v0 implementation and compatibility requirements below.
Those sources are frozen at `7ccaa16768ae2b2cb0da71d47e864352721de8b0` on
`baseline/tilerelay-v0-pipeline-20261019`; use the
[current integration guide](../TILELANG_INTEGRATION.md) for active commands.

Approved scope: [spec](../specs/2026-10-14-tilerelay-design.md).

## Ordered work

1. Inspect CANN 9.1 handoff headers and pinned TileLang extern/source import.
   Main agent owns the mixed handoff probe and resource layout. Acceptance:
   bounded real-device producer/consumer correctness with repeated rounds.
2. Implement generation-tagged peer-memory publish/pull and local readiness.
   Main agent owns `src/tilelang` device helpers. Preserve ordinary collectives
   and MoonEP. Acceptance: exact data on both ranks and delayed-rank progress.
3. Integrate persistent local-first TileLang GEMM; define validated launch ABI,
   staging sizing and optional binary installation. Main agent owns the public
   API/build path. Acceptance: C++14 compile, installed consumer and one main
   launch per generation without Host per-tile events.
4. Extend examples and focused tests. Main agent owns profiler/correctness and
   same-boundary baseline comparison. Acceptance: large/small/delayed cases,
   changed payloads, rank statistics and source/binary provenance.
5. Simplify, independently review, record reusable lessons and delivery evidence
   in maintained integration/validation docs, then commit/push task scope.

## Independent read-only work

A reviewer may inspect target-version synchronization and compiler interfaces
without modifying files or using NPUs. Inputs are the spec, current generated
GEMM and SDK headers. Return precise supported signatures and limitations;
do not infer hardware correctness from headers. Main agent serializes all NPU
tests, source integration and Git writes. Later independent final review uses
the delivered diff and fresh verification evidence.

## Constraints

No changes to default builds, core communicator semantics or unrelated modules.
No source-text implementation guards, hidden fallback, or environment-specific
scripts in commits. Keep compiler pins and reference-only boundaries intact.
Do not reset IPC flags; validate capacity and generation lifetime. Performance
results must use the final binaries and unchanged comparison conditions.

## Current evidence

Historical prerequisite: the standalone `mixed_handoff.py` probe below was
retired after production two-card E2E covered the same handoff/ACK sequence.
Use `tests/relay/hardware.py` for current qualification, not the historical command.

Spec persisted and initial same-device prerequisite implemented as
`tests/tilelang/mixed_handoff.py`. It uses the pinned DSL's built-in copy and
cross-core intrinsics, not unverified generic extern calls. Two AIVs each
publish half a tile; AIC waits before GEMM and acknowledges each wave, bounding
signal counts. More than 15 waves tests repeated consumption, not signal
counter accumulation. Python syntax is not compilation or NPU proof.

On 2026-10-14, blue SSH failed before executing commands (connection closed
and banner timeout). No local CANN compiler is available. Stage 1 target
compile/hardware execution is pending; do not implement or qualify the
distributed path based on this unexecuted prerequisite.

After restoring the same host/toolchain environment and confirming an idle
device, run with the pinned compiler environment and `TASK_QUEUE_ENABLE=0`:

```bash
timeout --signal=TERM --kill-after=15s 90s python tests/tilelang/mixed_handoff.py \
  --device 0 --blocks 1 --waves 20 --rounds 8 --output /unique/probe-output
```

Repeat with four blocks in a new output directory. Establish generated-source
and binary provenance, exact staging data and GEMM correctness before stage 2.

## Resumed development

SSH recovered on 2026-10-14. Stage 1 passed both one/four blocks, 20 waves and
eight varying-input generations on 910B. Installed SDK headers confirm the
cross-core signatures. The first two-rank integrated tiny case passed all four
modes and its framework trace showed one main mixed launch per Relay round.
Native contract mocks and the installed consumer passed 3/3 CTests.

## Final qualification

The final contiguous-copy kernel with fixed 1 MiB publication/ACK planes
passed large, medium, delayed-rank and separately instrumented groups: 464
rank-rounds with exact gathered inputs and CPU-reference GEMM checks. Framework
traces show one Relay main launch instead of 16/8 tiled baseline launches;
diagnostic same-group producer/local-compute intervals intersect on both ranks.
Alternating two shapes for 12 generations, with delayed ranks and no per-round
Host barrier or output check, passed another 24 rank-generations.

Fresh C++14 native mocks/installed consumer passed 3/3 CTests; all 18 Python
measurement/checker tests passed. Kernel-component installation and binary
identity checks passed. Independent final review found no concrete blockers.
See the maintained [validation record](../TILELANG_ALLGATHER_GEMM_VALIDATION.md)
for final provenance and measurements: Relay improves the medium shape but
is slower for the large shape. No universal speedup or new environment,
UDMA, multi-node or 950 qualification is claimed.
