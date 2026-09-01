# UDMA Transport Specification

## Scope

UDMA is TileXR's registered remote-memory transport for A5 and Ascend950-class
hardware. The core runtime owns discovery, queue creation, memory registration,
registry exchange, device views, and teardown. Operators own transfer policy and
their registered workspaces.

UDMA is a best-effort communicator capability. Failure to initialize it must
preserve peer-memory and other existing paths.

## Runtime Structure

`TileXRComm` owns an internal UDMA context. The context separates:

- capability and topology discovery;
- per-peer QP/CQ resources and imported memory metadata;
- ordinary registered regions;
- profile registrations that bind multiple regions to selected QPs;
- device-visible `UDMAInfo`, registry, and fullmesh views.

Public Host operations are declared in `src/include/tilexr_api.h`. They register,
query, and unregister memory; report the QP count; query fullmesh resources; and
expose immutable Host or device registry views.

## Registration Contract

`TileXRUDMARegister` accepts ordinary device memory and exchanges the resulting
remote descriptors across ranks. Registration is unsupported during
`TileXRCommInitThread`. By default, `peerMems[]` are not UDMA registration
targets. When every rank opts in with `TILEXR_UDMA_USE_PEER_MEM=1`, `TileXRComm`
registers only its owner rank's original `peerMem_[rank_]` allocation after IPC
setup; remote `peerMems[peer]` IPC mappings are never registered locally. Public
register/unregister calls cannot replace or release this communicator-owned
region. Every submitted remote range must be contained by a live registration.

Registration handles are communicator-owned identities. The caller must quiesce
every stream and device operation that can use a registration before unregister.
Unregister does not synchronize those operations; it first hides the device
registry, then releases transport and Host registry resources. Partial
registration or teardown failure must not publish an apparently complete
registry.

Profile registration describes one or more regions and deterministic QP
bindings. Every rank must agree on the profile contract. Queries return a
read-only view whose lifetime ends at profile unregister or communicator
destruction. Profile users have the same caller-managed quiescence requirement.

## QP and Routing Contract

The default configuration preserves the single-QP path. An explicit
`TILEXR_UDMA_QP_ROUTE_SPEC` requests a strict process-wide route set. Unsupported
routes or counts fail UDMA capability setup rather than silently substituting a
different route.

Queue ownership is per `(peer, qpIdx)`. Operations on one queue are serialized
by the owning kernel state; independent queues may progress concurrently.
Fullmesh and shared-QP domains expose explicit views so operators do not infer
registered-memory availability from topology alone.

## Device Submission Contract

Device APIs in `src/include/tilexr_udma.h` validate capability, rank, QP, and
registered ranges before constructing a transfer. The following invariants are
mandatory:

- assemble the complete WQE in UB;
- publish the WQE to the SQ with MTE3;
- wait for the corresponding MTE3 write before ringing the doorbell;
- ring the doorbell only with `st_dev`;
- never construct, patch, or publish an SQ WQE with scalar/direct-GM stores;
- track the exact submission frontier and reclaim CQ entries in SQ order;
- do not reuse UB WQE storage or registered payload storage before its required
  completion boundary.

Nonblocking put/get/signal APIs return or advance a queue frontier. Flush and
quiet operations wait to a defined frontier, not merely for an arbitrary CQE.
Operators using multiple QPs maintain independent frontiers.

## Peer-Memory Publication Contract

Registering the owner allocation makes UDMA writes address the same physical
memory exposed through local IPC aliases, but completion alone does not publish
a message. A producer must clean its local source cache lines, complete the
payload UDMA write, and then complete a separate cache-line-aligned ready-token
write. A consumer polls that token through `peerMems[ownerRank]` using
`DataCacheCleanAndInvalid<CACHELINE_OUT>` followed by scalar
`GlobalTensor::GetValue`; repeatedly copying the token through MTE2 can keep
returning a stale cached value on Ascend950. After acquiring the ready token,
the consumer may copy payload lines that it has not read before through MTE2.

Control slots must not share cache lines with payload or other flags. Reusing a
slot requires a new magic generation. If a consumer can have cached payload
from an earlier generation, it must apply the documented cache acquire for
those lines before consuming the next generation.

## Failure and Fallback

Unavailable UDMA does not fail communicator initialization. Once an operator
selects UDMA for a registered transfer, validation or runtime failure is
reported; it must not silently reinterpret an unregistered pointer or switch
mid-protocol to peer memory. Memory-versus-UDMA selection considers payload size,
runtime capability, and registration readiness, not topology alone.

## Verification Boundary

Host tests cover configuration, registration, registry layout, QP selection,
frontier accounting, and teardown. Source guards cover UB/MTE3 publication and
doorbell constraints. A5/Ascend950 multi-rank demos are required to prove the
data plane. Simulator, Host, and 910B fallback tests do not prove UDMA transfer.
Current commands and evidence are maintained in `docs/UDMA_TEST_STATUS.md`,
`docs/UDMA_INTEGRATION_SUMMARY.md`, and `docs/BUILD_VERIFICATION.md`.
