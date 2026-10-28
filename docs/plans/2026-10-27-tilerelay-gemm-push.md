# TileRelay GEMM/push execution plan

Durable plan for the user-approved
[design](../specs/2026-10-27-tilerelay-pipelined-gemm-push-design.md).
Baseline: `3994dd34dc822a59d42c9834d24143ff7073ec60`.
Follow AGENTS.md and the retained validation guide. No native ABI/runtime,
compiler fork, precision reduction, extra kernel or shape-selected fallback.

## Execution status (2026-10-27)

Tasks 1–3 are implemented as candidates. Task 4 numerical qualification passed
24 specializations on both cards through execution and one-shot/bindings paths.
Formal two-seed comparisons show large AG shared-GEMM gains of 25–27%, but
push fails its incremental gate; retaining local UB and batching transfers do
not recover the loss. PMU and separately fenced phase measurements investigate
the rejected hypothesis. See the maintained
[acceptance record](../TILELANG_ALLGATHER_GEMM_VALIDATION.md#2026-10-27-gemm-pipeline-and-fp32-push-acceptance-experiment).

The user approved narrowing delivery to shared GEMM with the original pull and
layout. Tasks 1 and 3 remain rejected experiments, not shipped interfaces.
Their workspace, layout marker and tests have been removed. Final production
scope is `device/gemm.py` plus the two operator generators, with native code,
Plan, peer primitives and build CLI unchanged. Final full-matrix GEMM-only
qualification passed both ranks: 24 specializations, 48 rounds each in execution
and one-shot/bindings modes, 96/48 launches per rank. Local final sources match
the already measured GEMM-only build source. Python contracts (29), profiler
analysis tests (12), C++14 Host mock (1/1), simplification and independent review
are complete. Delivery retains the experiment record and comparative tables;
tasks 1/3 below describe the historical experiment, not active production work.

## 1. Operator resource contract

- Owner: bounded Python-contract workstream; files `ops/plans.py`,
  `scripts/build_tilexr_relay.py`, existing `tests/relay/test_python_contracts.py`.
- Prerequisite: approved slots/layout and compatibility boundaries in the spec.
- Add immutable AR push layout derived from artifact, a fixed payload-layout
  marker, private FP32 partial workspace and incoming/outgoing payload bindings.
  Planner and generator share shapes/slots/capacities. Preserve AG and complete
  historical Plan+ELF loading; reject replanning old pull artifacts as push.
- Verify `python3 -m pytest -q tests/relay/test_python_contracts.py`; add only
  contract boundaries not already established by device E2E.
- Output: compiler-free layout with shape, peer layout and canonical tensor
  specs, consumed by task 3. No commits/pushes by a delegated shared-tree writer.

## 2. Shared GEMM and isolated compute qualification

- Owner: main agent; `device/gemm.py`, `ops/allgather_gemm.py`, compute section
  of `ops/gemm_allreduce.py`. Independent of task 1 until AR push integration.
- Implement explicit L1/L0 credits, K64 MMA, FIX ownership and matched drains;
  retain gcd(K,256), FP32, local-first AG and existing two-slot schedule.
- Build separate GEMM-only artifacts before AR layout migration. Compare to
  preserved baseline artifacts, never overwrite prior qualified installations.
- Compile all supported boundary shapes; use existing two-card E2E for numeric,
  precision, tail/idle-worker and epoch correctness. Inspect resource/flag
  lowering without introducing source-token enforcement tests.
- Output: common inlined GEMM macros and independently measured compute change.

## 3. Push integration and isolation

- Owner: main agent; `ops/gemm_allreduce.py`, minimal reusable `device/peer.py`
  changes only if needed. Depends on task 1's layout; combined result on task 2.
- AIC writes private partial; AIV pushes FP32 before publishing epoch, waits
  peer publication, consumes local incoming, finishes output before ACK, then
  returns two-slot credit after peer ACK. Preserve full row-major staging.
- Compile task-local push-only and combined artifacts. Confirm paired ranks,
  private/peer allocation separation and FP32 partial probe. No production
  algorithm toggles for these temporary ablations.
- Output: one candidate production push path, complete resource manifest.

## 4. Regression and performance acceptance

- Owner: main agent, serialized NPU access after idle check. Reuse existing
  HCCL qualification evidence; never rerun an already qualified environment.
- Run existing 22-specialization coverage plus any demonstrably uncovered K,
  tail, idle-worker, delayed-rank, mixed-shape/operator and lifecycle boundary.
  `tests/relay/hardware.py` remains the arithmetic/lifecycle E2E; no redundant
  unit framework. Run one-shot/prepared bindings and prepared execution paths.
- Measure baseline/GEMM-only/push-only/combined and official fused operators,
  small through large shapes, 10 warmups + 30 measured iterations, seeds 19/37,
  same profiler/barrier state. Preserve correctness checks and per-rank samples.
  Match official AR full device span, not only AICore duration; record precision
  and workspace differences. Keep diagnostic PMU runs separate from timing.
- Apply the spec's reproducible-gain and regression gates. A gain is unproven
  until hardware comparison passes; if push lacks incremental value, investigate
  or report unmet acceptance rather than ship an unhelpful alternate path.
- Output: maintained validation evidence, artifact identities and residual gaps.

## 5. Simplification, review and delivery

- Main agent simplifies changed code, runs Python tests and relevant Host CI,
  and requests independent review of final layout and synchronization changes.
  Refresh invalidated verification after fixes; no broad unrelated cleanup.
- Update integration/spec/validation with actual outcome and reusable lessons.
  Commit approved spec and plan with implementation only after acceptance, then
  normal push to the established task-owned PR branch under delivery rules.
  No force-push, merge, historical artifact removal or environment-specific
  orchestration commits.
