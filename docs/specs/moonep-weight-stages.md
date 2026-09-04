# MoonEP Weight Stages Specification

## Scope

PrefetchWeight and ReduceGrad are the weight-side stages of the native MoonEP
flow. They share the saved plan and public tensor ownership contract but have
independent kernels, workspaces, transport state, and completion boundaries.

PrefetchWeight V1 and ReduceGrad V2 use external pure AICore binaries loaded
from the owning Host library's `tilexr/kernels` directory and launched through
the runtime registration APIs. ReduceGrad V1 remains the capability-reported
compatibility stub. The stages preserve the C and Python contracts declared in
`src/include/tilexr_moonep.h`.

## PrefetchWeight

PrefetchWeight gathers the gate, up, and down projection rows required by the
local plan. The public operation is one fused launch across the three projection
sets and fills rows `[E,E+B)` in the same projection allocations. The projection
backing remains caller-owned, registered, and alive until the recorded
stream/event completes.

The current native path is A5 UDMA-only: the three projections occupy one
ordinary registered backing allocation, and Host validation requires a live
UDMA registry and QP set before launch. It may distribute workers across the
communicator's shared-QP profile. Logical workers map deterministically to
physical QPs; every queue retains its own submission/completion frontier. There
is no peer-memory PrefetchWeight fallback.

An explicit multi-QP environment route does not override a communicator profile
whose QP contract was fixed at initialization. Unsupported profile or
registration state fails before launch instead of silently changing transport.

## ReduceGrad

ReduceGrad combines FP32 gate, up, and down gradients for experts owned by the
local rank and updates those local gradients in place. Remote source slices and
ownership are derived from the saved plan. Only the declared local expert rows
are modified.

V2 separates workspace query, preparation, prepared-state destruction, and
execution. Prepared state captures stable registered regions and queue/profile
bindings; it does not capture per-call gradient contents or stream ownership.
Destroying prepared state requires all launches that use it to be complete.

The owner-pull implementation uses persistent registered profiles and bounded
large-chunk GETs across deterministic QP lanes. Each owner pulls the remote
source slices needed for its local experts, waits for exact completion frontiers,
accumulates in the declared order, and releases reusable storage only after the
consumer completes.

## Shared Invariants

- `peerMems[]` are not registered as ordinary UDMA targets.
- Registered ranges cover every submitted source and destination byte.
- WQEs are assembled in UB, published with MTE3, and followed by an MTE3
  completion before an `st_dev` doorbell.
- Queue frontiers are not shared concurrently by multiple owners.
- Missing UDMA capability or registration makes PrefetchWeight and ReduceGrad V2
  unavailable without changing other runtime memory paths. A selected UDMA
  protocol never switches backend mid-round.
- Saved plan and tensor lifetimes extend through both backward branches.

## Failure and Diagnostics

Host validation rejects unsupported dimensions, expert ownership, dtype,
profile, registration, or workspace. Device status reports the first failed
owner/peer/QP/frontier where possible. Timeouts remain diagnostic outcomes and
must not replace investigation of the first queue or state boundary.

## Verification Boundary

Reference and Host tests cover ownership, slice construction, workspace/profile
layout, queue mapping, dtype, and exact output rules. Hardware correctness covers
the selected rank/expert/shape/profile and repeated launches. Performance claims
require matching framework/runtime provenance and report backend, native status,
kernel version, data volume, latency, bandwidth, and rank median/min/max. A
different CANN, torch_npu, rank set, or profile is not a strict A/B baseline.
