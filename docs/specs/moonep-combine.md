# MoonEP Combine Specification

## Scope

MoonEP Combine returns expert rows to token order and reduces top-k contributions
with optional FP32 route weights. Combine V2 is the sole supported backend; the
runtime has no backend-version selector or V1 fallback.

The V2 public ABI is declared in `src/include/tilexr_moonep_combine_v2.h`. The
caller owns the saved plan, registered workspace, output, stream, and
communicator. Combine owns layout validation, registered launch, schedule state,
transfer completion, and reduction.

## Route Contract

Planner V3 supplies `dstLocal`, the reverse mapping from an expert receive slot
to source rank, token, and top-k position. Padding and unused slots use the
declared sentinel and are skipped. Route decoding, schedule mapping, data
movement, and reduction all consume the same plan generation.

The pluggable step mapping computes one complete `StepRoute` from schedule mode,
rank, core, step, and rank size. Send destination, receive source, credit wait
owner, and credit publication owner are derived together. Mapping helpers do not
inspect workspace, QP, CQ, payload, or mutable kernel state.

Current production mapping uses Group for 128 ranks and Single Ring for other
supported sizes. Other compiled mappings remain explicit alternatives and are
not automatically selected.

## Data Paths

A step is exactly one of:

- CLOS UDMA to a remote peer;
- fullmesh UDMA to a remote peer;
- self-copy through local GM/MTE;
- an empty payload that still obeys the control protocol.

Fullmesh and CLOS maintain independent resource views and queue frontiers.
Self-copy participates in the same step lifecycle but does not fabricate a CQ
completion. Memory and UDMA selection follows capability, registration, and
payload policy rather than topology alone.

## Credit and Completion Protocol

Credit is the current reuse protocol. Historical grant and full-sync/phase
barrier schemes are not active fallbacks. Each storage or queue frontier has one
publisher and one defined waiter:

1. wait for the credit protecting the next reusable region;
2. build and submit the step payload/control work;
3. wait for the exact completion frontier required by that transport;
4. publish done/control state only after the protected data is visible;
5. publish the next credit only after the receive-side consumer releases the
   corresponding region.

The last step follows the same completion and credit ownership rules as earlier
steps. Cross-round safety comes from generation-tagged state and completed reuse
boundaries, not from clearing all shared state.

## WQE and UB Contract

Route chunks are loaded to UB, selected/compacted, and converted into complete
payload WQEs in UB. Peer/QP-constant fields may be prefetched, but every published
WQE is complete before its MTE3 SQ copy. Doorbells use `st_dev` only after the
MTE3 completion. Queue accounting tracks the precise submission frontier.

UB allocations for route selection, WQE batching, self-copy, weight gathering,
and reduction have explicit non-overlapping lifetimes. A `TPipe::Reset` or buffer
reuse occurs only after all relevant MTE/vector work has completed.

## Fused Hidden and Weight

The fused path moves BF16/FP16 hidden rows through the selected transport and
handles optional FP32 route weights through the defined memory domain in one
Combine stage. Weight storage has a generation-separated peer-window layout and
unique writers. Hidden and weight completion share stage ordering without
aliasing payload, profile, or scratch regions.

The Python return tuple, zero-copy lifetime, optional-weight behavior, and
async-event contract remain unchanged.

## Reduction

Reduction consumes the received rows in route order and accumulates in FP32
before producing the declared output dtype. The pipeline may overlap MTE2 input,
vector accumulation, and MTE3 output only when buffer ownership and events prove
the rows independent. Large hidden dimensions are tiled without changing the
logical accumulation order.

Empty and padded routes do not read payload. Output becomes visible only after
all inbound data, self-copy, route-weight data, and reduction writes complete.

## Failure and Diagnostics

Host validation rejects unsupported rank sizes, plan versions, layouts, dtypes,
workspace ranges, or unavailable registered transports. Kernel failure records
the first stage/peer/core/frontier/generation where possible. Profiling records
are versioned and disjoint from payload and correctness state.

## Verification Boundary

Host tests exhaustively compare route mappings and validate layout, registration,
mode selection, source invariants, and empty/self/remote cases. Hardware
correctness must exercise selected ranks, schedules, backends, repeated rounds,
skew/sparse/empty routes where supported, optional weights, and reduction.
Performance reports include backend, native status, kernel version, algorithm or
communication volume, latency, bandwidth where applicable, and rank median,
minimum, and maximum. Host or simulator evidence cannot prove UDMA ordering or
bandwidth.

When retiring a stage backend, remove its source target, public ABI, runtime
selector, reporting provenance, demos, and dedicated tests in one change. Keeping
any one of those surfaces creates a false compatibility promise; validate the
remaining backend independently after the removal.
