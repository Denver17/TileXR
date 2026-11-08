# RDMA explicit completion implementation plan

Approved scope: `docs/specs/2026-11-05-rdma-explicit-completion-design.md`.
Repository constraints: root `AGENTS.md`; C++14, CANN 9.1, Ascend950 AIV.
This durable plan supports blue implementation and exact-revision green validation.

## 1. Production pipeline and adapters (main agent, critical path)

Own `src/include/tilexr_rdma_{pipeline,device,device_types,write}.h`.
Persist two pending batches in existing reserved state words, remove final drain,
and add `RDMAWait(view, scratch, maxPolls)` in the device header so both low-level
and public WRITE callers can use it. Keep single-owner QP state, queue ordering,
non-wrapping admission, error codes and legacy/Host completion semantics.
Admission must include pending CQEs; backpressure must count WRs, not WQEBBs.
Output: submit/wait APIs and unchanged exported POD layout.

## 2. Behavioral regressions (main agent, follows 1)

Own `tests/rdma/test_rdma_pipeline.cpp` and `test_rdma_write_options.cpp`.
Extend existing production-algorithm/adapter fixtures for submit-only return,
cross-call pending, explicit/empty/repeated wait, changed batch/SGE sizes, shared
QP views, bounded windows, non-wrap CQ capacity and sticky failures. Migrate
existing synchronous expectations by adding explicit waits, not dropping checks.
Retain ABI and legacy regressions. No source-text enforcement or new framework.
Compile with `-std=c++14 -Wall -Wextra -Werror -UNDEBUG`; run focused tests and
standalone `cmake -S tests/rdma -B <temporary-build>` / build / CTest.

## 3. Application and guide migration (independent write scope)

Own `tools/rdma_compare/{bench_kernel.cpp,multiqp_kernel.cpp,README.md}` and
`docs/{RDMA_API.md,RDMA_INTEGRATION.md}`. Based on the approved API contract,
put explicit successful Wait inside both benchmark timed regions and propagate
errors. Update every affected example, lifecycle and completion statement;
add required bilingual headers to modified code. Do not change benchmark ABI,
workloads, measurement definitions, Host APIs or unrelated guides.
Output: callers retain completed-transfer timing and document resource ownership.
Verify all pipeline call sites are migrated; device compile remains separate
from CPU tests. This task may run alongside 1/2 with disjoint ownership.

## 4. Simplification, verification and independent review

After 1–3, inspect the task diff for extra capabilities and remove only task-owned
non-core probes. Run whitespace, focused and broader RDMA tests; request review
against the approved spec, final diff and actual results. Evaluate findings,
refresh affected checks, and document unavailable CANN/device evidence honestly.
No bandwidth or hardware-correctness claims from CPU fixtures.

## 5. Git delivery and green handoff

Use the task worktree branch `codex/rdma-explicit-wait`; preserve the original
main checkout. Follow the delivery skill for scoped commit and normal push;
no main update, force-push or automatic PR. Green must fetch the same branch
and verify the exact delivered commit SHA before device compilation/debugging.
Resolve outstanding cross-zone recovery with the required authorization first.
Do not transfer credentials, full logs or deployment configuration into Git.
If green validation is blocked, report the exact gap and leave handoff runnable.
