# Retryable UDMA Communicator Teardown Design

## Goal

Make communicator destruction a rank-consistent protocol that preserves
retryable ownership, while correctly handling HCCP results that consume a Host
handle despite reporting a server-side failure. A failed local queue destroy,
context destroy, or bound-stream synchronization must return an error without
discarding the handles, loader, or communicator state required for a later
`TileXRCommDestroy()` retry.

The change fixes lifecycle ownership only. It does not change the A5 AllToAll
data plane, routing, Credit protocol, kernel ABI, or launch geometry.

## Public Contract

`TileXRCommDestroy(comm)` remains the only public destroy API. It reaches the
existing final communicator destructor only after all ranks complete the
internal-resource, profiling, and UDMA stages. If a retryable internal or UDMA teardown phase fails,
the function returns an error and does not delete `comm`; all ranks must retry
the same handle collectively.

Destroy is not concurrent with arbitrary public operations. The existing
per-communicator internal-resource mutex serializes A5 launch-state mutation
with destruction, so no new A5 grouped launch can enter after its teardown
starts. General concurrent use of a communicator being destroyed remains
unsupported.

## A5 Runtime Quiescence

The private A5 runtime owns the stream identity used by all of its launches.
Its destroy callback performs these fixed phases on every rank:

1. synchronize the bound stream locally;
2. agree the synchronization result across all ranks;
3. stop immediately on global failure, leaving profiles and workspaces intact;
4. unregister active and retired profiles;
5. agree profile cleanup results;
6. free only workspaces whose profiles are fully unregistered;
7. agree workspace cleanup results;
8. delete the runtime only after global success.

Each retry executes the same rank-level agreements. Local success never clears
the communicator callback while another rank reports failure.

## Communicator State Machine

`TileXRCommDestroy()` advances through retryable internal stages while the
socket exchange remains alive:

1. finalize the opaque collectives resource;
2. finalize profiling;
3. finalize UDMA transport;
4. invoke the existing communicator destructor for SDMA, IPC, and remaining
   communicator-owned memory.

Completed stages are idempotent. A failed stage preserves the communicator and
returns its globally agreed error. The destructor remains a best-effort fallback
for initialization unwind and process termination, but normal public destruction
uses the status-returning path and never relies on destructor-only UDMA cleanup.
Making the pre-existing SDMA and IPC destructor paths status-returning is outside
this UDMA lifecycle change.

## UDMA Teardown Protocol

`TileXRUDMAContext::Shutdown()` and `TileXRUDMATransport::Shutdown()` return a
TileXR status. The context enters `CleanupPending` before destructive work and
publishes disabled UDMA device state before releasing registrations.

Transport teardown uses a fixed sequence of Host socket-exchange agreements.
Every rank participates even when its local shared/fullmesh vectors are empty:

1. teardown-ready agreement;
2. registered profile and memory cleanup;
3. remote QP unimport for fullmesh, shared, explicit, and legacy queue state;
4. remote-unimport agreement;
5. local QP, CQ, and channel destruction only after global remote success;
6. local-queue agreement;
7. token and context release;
8. context agreement;
9. RA/TSD deinitialization and loader unload.

The queue helpers separate remote unimport from local destruction. They clear
an individual handle when its HCCP call succeeds or when b131 returns `228104`.
In the latter case the server rejected the unimport opcode, but the b131 client
still freed the Host-side `rem_qp_handle`; retaining or retrying that pointer is
a use-after-free. Shared aliases are deduplicated by `(context, remote handle)`,
and every alias is cleared from the cached result. Empty rank-local state still
participates in the same agreement phases.

If any other unimport or teardown operation fails, later ownership layers do
not run. Queue failure prohibits token/context destruction, state-vector
clearing, RA deinit, and loader unload. A retry resumes from handles whose
ownership is known to remain local; already consumed handles are skipped while
all ranks still execute the same agreements.

Base device information, registries, fullmesh views, routes, and configuration
flags are cleared only after the complete transport teardown succeeds.

## Error Handling

Raw HCCP/RA return values remain failures except for the precisely established
b131 `RaCtxQpUnimport()` ownership case. Return `228104` means the service does
not support explicit QP unimport, but the client has already consumed the Host
handle. TileXR clears that pointer, emits one bounded warning per cleanup phase,
and continues local teardown so RA context destruction can reclaim server-side
resources. This is not reported as a successful server-side unimport. Other
nonzero values remain failures and their handles are retained.

When multiple local operations fail, the implementation retains the first
TileXR error while continuing only within the current ownership phase where
doing so cannot invalidate retry state. Cross-rank agreement determines whether
all ranks may advance to the next phase.

## Testability

Teardown operations are routed through a small internal operation table or
equivalent injection seam around the HCCP calls. Production uses the dynamically
loaded functions; Host tests inject deterministic outcomes.

Host tests must prove:

- `RaCtxQpUnimport` return `228104` clears the consumed Host handle and never
  calls unimport on the same shared alias twice;
- any other `RaCtxQpUnimport` failure preserves the remote handle, queue state,
  context, and loaded operation table for retry;
- empty and non-empty rank-local queue state execute the same agreement stages;
- local QP destruction never occurs before globally successful remote unimport;
- A5 stream synchronization failure prevents profile unregister and workspace
  free;
- `TileXRCommDestroy()` does not delete the communicator on retryable teardown
  failure.

## Hardware Validation

After Host/build tests, run 16P, 32P, and 64P on supported Ascend950 hardware.
Each scale performs at least 20 consecutive
`init -> run -> destroy -> init` cycles. Acceptance requires:

- every destroy and immediate reinitialization succeeds on every rank;
- zero TileXR `unimport failed`, `cleanup incomplete`, `128103`, double-free, or
  invalid-pointer records; b131 may still log its service-side `ret(-93)`;
- no `259`, `507035`, hang, or correctness failure;
- RA/QP resource counts do not grow across cycles.

Host tests and a single successful destroy do not prove multi-rank teardown.

## Compatibility And Non-Goals

- Preserve C++14 and CANN 9.1 compatibility.
- Add no public API or device-side synchronization.
- Do not add steady-state launch synchronization.
- Do not change generic or A5 kernel behavior.
- Do not treat `228104` as server-side success or generalize its Host-handle
  ownership rule to any other error code or HCCP version.
- Do not claim 16P evidence covers 32P or 64P.
