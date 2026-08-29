# MoonEP Dispatch Specification

## Scope

MoonEP Dispatch scatters token hidden rows and optional FP32 route weights into
the expert-local layout described by the saved plan. V1 and V2 public entry
points remain versioned; the native V2 path is the production grouped path.

Dispatch does not own Planner output, input tensors, communicator, stream, or
registered workspace. It owns validation, layout construction, one registered
kernel launch, and stage-specific diagnostics.

## Tensor and Plan Contract

The primary input is FP16/BF16 hidden `[S,H]`; the primary output is expert-slot
hidden `[NvS,H]`. Optional route weights are FP32 `[S,K]` and produce expert-slot
weights `[NvS]`. Routes and padding are consumed from the same plan generation.

The public C, Torch, MindSpeed, asynchronous-event, and zero-copy contracts stay
aligned with `tools/moonep/test_npu_e2e.py`. An implementation change must not
alter that test's calls to accommodate a new internal layout.

## Fused Epoch

One Dispatch V2 call launches one registered AICore kernel and uses one
communication epoch for hidden data and the optional weight payload. Hidden-only
calls are also one launch. Hidden and weight regions, receive scratch, profiles,
and diagnostics are disjoint inside the registered workspace while sharing the
stage generation and completion protocol.

Standalone route-weight Dispatch is not a separate public operation. Optional
weights are accepted only as the paired payload of the hidden dispatch.

## Grouped Modes

`group_credit` is the default Dispatch mode. `group` is used only when an
explicit task or experiment requires it. `legacy` is not a valid model, replay,
cascade, single-operator, diagnostic, qualification, or fallback path.

Grouped schedules assign route publication, receive progress, credits, and
completion counters to unique owners. A credit is published only after the
receiver has made the protected storage reusable. Control tokens and completion
counters are ordered after the data/control state they advertise.

## Transport and WQE Invariants

The memory backend uses mapped peer windows. The UDMA backend uses an ordinary
registered workspace and explicit QP/frontier state. UDMA WQEs are fully built
in UB, copied to SQ with MTE3, and followed by an MTE3 completion boundary before
an `st_dev` doorbell. Scalar or direct-GM SQ construction and doorbells are
forbidden.

Transport selection requires capability and registration readiness. Memory is
valid across nodes and is preferred for small payloads; UDMA is considered for
large registered transfers. Topology alone does not choose the backend.

## Failure and Diagnostics

Host validation rejects inconsistent paired weights, plan status, workspace,
dtype, shape, or rank configuration. Kernel diagnostics record the first failed
stage, owner, peer, queue/frontier, and generation where available. A later
timeout must not hide an earlier plan or completion error.

Compile-time profiling/DFX is separate from framework profiling. Diagnostic
dumps are disabled for a performance baseline unless they are the subject of an
explicit A/B test.

## Verification Boundary

Host/source tests cover layouts, one-launch registration, route mapping, grouped
ownership, and mode guards. Hardware validation must cover the selected grouped
mode, shapes, ranks, backend, correctness, and repeated generations. A cascade
or model is runnable only after all required ranks/stages and correctness checks
complete in an allowed grouped mode.
