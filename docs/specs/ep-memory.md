# Expert-Parallel Memory Backend Specification

## Scope

The optional EP library exports Dispatch and Combine interfaces from
`src/include/tilexr_ep.h`. Its memory backend uses communicator peer mappings for
same-node and cross-node traffic. It is distinct from the UDMA backend and must
not be selected solely from topology.

The Host layer validates public arguments, builds a round-specific window
layout, selects the embedded kernel, and launches through the registered AICore
signature. The communicator and stream are borrowed; EP owns only its launch
metadata and kernel registration.

## Dispatch Contract

Dispatch routes input rows to expert owners and returns expert-local payload,
combine-assist metadata, and receive counts according to the selected API
version. V2 additionally preserves active-mask, quantization, scale, and expanded
output contracts declared in `tilexr_ep.h`.

The memory kernel uses a generation-tagged round in the peer window. Sender,
count/cumsum, local-copy, and receiver stages have disjoint state and workspace
regions. Every state cell has a unique writer, and consumers wait for the
generation associated with the current launch.

MXFP8 modes preserve the public E4M3/E5M2 selection, packed-row format, and
dynamic-scale output. Unsupported quantization or dtype combinations fail Host
validation.

## Combine Contract

Combine consumes expert output plus Dispatch-produced assist/count metadata and
restores token order, applying expert weights where required by the selected
entry point. The memory kernel follows the same generation and peer-window
ownership model as Dispatch while keeping Dispatch and Combine buffers separate.

Accumulation order and precision are part of the API behavior. Empty routes and
inactive tokens remain valid inputs and must not read uninitialized peer-window
rows.

## Window and Round Ownership

`src/ep/common/ep_memory_window.h` and the Host layout helpers define the
authoritative window partition. Payload, status, count, synchronization,
profiling, and scratch regions do not overlap. A round is reusable only after
all producers and consumers have crossed its completion boundary; a new
generation distinguishes stale state without bulk-clearing the shared window.

Memory supports cross-node mapped peer traffic. UDMA is selected only when the
specific entry point, communicator capability, payload, and registered workspace
contract require it.

## Profiling

Dispatch and Combine use separate fixed-size profiling records declared in
`src/ep/common/ep_dispatch_memory_profile.h` and
`src/ep/common/ep_combine_memory_profile.h`. Profiling is compile-time opt-in,
does not alter the public ABI, and writes only the owning core's record.

The generic profiling report path consumes versioned stage timestamps and
accumulated metrics. Incomplete ranks or records are labeled explicitly. The
64-rank runner unifies functional, performance, and breakdown modes but does not
make profiler/diagnostic configuration part of normal operator semantics.

## Failure and Compatibility

- Invalid shapes, layouts, datatypes, modes, or communicator capabilities fail
  before launch.
- Kernel status identifies the first protocol failure; Host collection must not
  replace it with a later timeout symptom.
- Public V1/V2 entry points remain stable unless a separate ABI change is
  approved.
- Active code never includes or links comparison sources from `reference/`.

## Verification Boundary

Host tests cover layouts, validation, quantization helpers, registration, and
source-level invariants. Functional multi-rank runs verify the selected memory
path and shapes. Performance reports must state backend, native status, kernel
version, data volume, latency, bandwidth where applicable, and rank median/min/
max. Results on one rank count or topology do not validate another.
