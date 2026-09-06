# EP Memory Server Deduplication Design

## Status and Scope

This is a design in progress for the EP memory Dispatch/Combine backend. The
forwarding metadata below records the agreed design discussion; it is not an
implemented API or a hardware-validated protocol. Other parts of server
deduplication remain open for further discussion.

The baseline is [the EP memory specification](ep-memory.md). Existing hidden,
expert counts, and original assist semantics remain the starting point. The
metadata additions describe a new mode/interface to be specified before
implementation; they do not silently change an existing public ABI.

The production topology under discussion has eight ranks per server: 128 ranks
span 16 servers, and the future 256-rank scaling target spans 32 servers. The
four-rank example below deliberately uses two ranks per server for illustration;
it does not define the production topology or establish runtime support for the
larger scaling target.

## Forwarding Logic

### Identity and Routing

Deduplicate a hidden row by `(original source rank, local token ID, destination
server)` within one Dispatch generation. Token values do not define identity.
The communicator must provide rank-to-server membership and mapped peer-window
addresses; a rank does not need other ranks' complete expert routing tables.

For the example and the initial forwarding discussion:

- Each rank owns one expert.
- For a remote server with multiple target ranks, choose the first target in
  top-k order as the relay. The relay is also an expert destination.
- Send hidden once to that relay, which retains its own contribution and
  forwards to the other target ranks in the server.
- Within the source server, the source rank distributes directly. It is already
  the final Combine owner, so no intermediate group is required there.
- A remote server with one target rank follows the ordinary direct path.

The first-target relay rule makes the example deterministic. Production relay
load balancing remains an open design choice.

### Receive Slots and Transient Descriptors

Use a logical source-partitioned receive address:

```text
Recv[destinationRank][originalSourceRank][q]

address = destination receive-window base
        + region offset for originalSourceRank
        + q * token slot stride
```

Here `q` is the zero-based ordinal among the original source's routes to that
destination. The original source computes it from its own routing table. It is
not a global receive index and is not necessarily the source local token ID.
This reuses the current memory backend's source-partitioned window principle;
the physical partition permutation, alignment, and byte layout are not fixed
by this logical notation.

A first-hop packet contains one hidden row and a small descriptor:

```text
originalSourceRank, sourceLocalTokenId, localTopKPosition
forwardEntries[] = {destinationRank, destinationReceiveOrdinal, topKPosition}
```

The source precomputes every forward entry, including its destination ordinal.
A destination rank mask alone is insufficient: the relay does not have the
source's other routes and cannot reconstruct those ordinals from that mask.

The receive slot can also serve as the forwarding payload source because the
relay is itself a target. No second full hidden copy is logically required.
The slot remains live until both local expansion and forwarding have completed.
Exact workspace layout and scheduling are deferred.

Keep count notifications based on the original expert routes. A receiver learns
which source partitions and how many slots to expect from these counts. Counts
do not indicate arrival: a slot becomes ready only after both hidden and its
descriptor are visible for the current generation. The receiver reads the
descriptor after readiness to discover whether forwarding is required.

A terminal forwarded packet preserves the original assist identity, carries its
parent relay/group reference, and has no further forwarding entries. The relay
is the delegated sole writer of that original-source destination slot; the
original source does not also write the same payload there.

### First Implementation: Receive Discovery

The first implementation will not add a separate `ForwardIndex` queue, its
notification window, or producer/consumer cursor protocol. Retain packet
descriptors and the persistent Combine metadata defined below; omitting an
auxiliary discovery index does not remove forwarding instructions or reverse
routing information.

Use the receive-window readiness protocol for discovery, with bounded,
round-robin polling over regions owned by the receive service cores:

1. Before an individual source/expert count is available, poll only within that
   region's configured capacity for the current generation. Do not wait for the
   complete count matrix or final cumsum before allowing forwarding discovery.
2. As individual count records become visible, retain them locally and narrow
   the relevant region to its valid ordinal range; skip known-empty regions.
   Raw count availability and completed output-prefix availability are distinct
   conditions. Service-core access must respect count publication and lifetime;
   destructive count-state clearing must not race with another reader.
3. Inspect readiness state before fetching hidden or decoding a descriptor.
   Skip unready candidates and distribute polling work so one slow source cannot
   block all receive/forward progress. Each ready packet has one processing owner.
4. A ready packet can be forwarded using its source-provided destination
   ordinals. Local expert copy-out and final row-index recording wait for the
   necessary output prefixes. Retain separate forwarding/local-copy completion
   state so revisiting a packet cannot duplicate forwarding or release it early.
5. TokenCnt still counts original expert rows, not physical packets. When one
   packet expands to multiple local expert rows, account for those rows through
   its descriptor; do not wait for an independent hidden arrival at every
   logical duplicate slot. Completion includes original row counts and all
   forwarding/local-copy obligations, including empty-source handling.

These are implementation requirements, not claims about the current kernel.
The existing blocking count/cumsum waits and all-core receive partitioning must
be reconciled with the chosen receive service roles. Bounded polling does not
itself create an available forwarding core or prove acceptable scan latency.
Before counts arrive, scanning reserved capacity remains a deliberate first
implementation cost.

A per-source/per-send-lane notification queue reduces Token-slot discovery but
still requires polling many queue states at 128/256 ranks. Active-source bitmaps
or hierarchical notifications could reduce that work, but add publication,
multi-writer ownership, clearing, or aggregation costs. There is no measured
basis yet for choosing those protocols over receive-window polling, so their
design is deferred until scanning is demonstrated to be a bottleneck.

Validation should distinguish scan cost from payload forwarding and local
expansion: record scan cycles, readiness attempts/hits, readiness bytes read,
and local detection-to-forward-submission latency where instrumentation permits.
The latter starts at receiver detection; it does not measure unobserved
arrival-to-detection delay. Compare sparse/skewed and dense routing at relevant
BS values, including 96/128/256, and at available rank scales. Preserve the
project's performance provenance and per-rank reporting requirements, and keep
instrumented diagnostics separate from the performance baseline. Add discovery
indexing only when a controlled comparison shows an end-to-end benefit that
includes its producer-side work, extra traffic, and synchronization overhead.

### Persistent Outputs for Combine

There are three logical arrays, packed into two metadata tensors:

| Metadata tensor | Logical content | Index domain |
| --- | --- | --- |
| `assistEx` | Original `assist` plus `backRef` | Received expert output row |
| `forwardPlan` | Separate `sourceMask` and `groups` sections | Local input token and local relay group, respectively |

These are metadata outputs in addition to the existing hidden/count outputs.
`sourceMask` is not the name of an extended assist row. If its section and
`groups` were separate tensors, there would instead be three metadata tensors.

The logical packing above is agreed. The final `forwardPlan` header, version and
generation encoding, section offsets, capacities, padding, and public ABI are
not yet specified. Its sections must be locatable without scanning payload or
rebuilding routing. The saved plan must outlive the corresponding Combine and
must not retain references to recycled Dispatch scratch descriptors.

#### assistEx and backRef

```text
assistEx: int32[Nrecv, 5]

columns = [sourceRank, sourceLocalTokenId, k, relayRank, groupId]
```

`Nrecv` counts valid expert output rows on this rank, not deduplicated network
packets. The first three columns retain the original assist meaning. If storage
is capacity-sized, consumers use valid receive counts rather than padding rows.

`backRef = (relayRank, groupId)` is interpreted as follows:

- `(-1, -1)`, called `DIRECT`, returns through the original assist destination.
- Otherwise the row contributes to member slot `k` of the named relay group.
- Group IDs are local to a relay rank: `R0.g0` and `R2.g0` are different groups.
- The relay's own expert row also has a group reference. Its data can be read
  directly through the group's local row index without a self-transfer.

#### groups

For the current one-expert-per-rank case, use the logical record layout:

```text
groups: int32[G, 5]

columns = [sourceRank, sourceLocalTokenId, returnK, memberMask, localRow]
groupId = record index within this rank's groups section
```

`G` is the number of intermediate groups owned by this rank; zero is valid.
`returnK` is the minimum original top-k position in this remote server group.
`memberMask` identifies all original top-k contributions in the group, including
the relay's own contribution. `localRow` indexes this rank's expert output;
its original `k` is available in `assistEx[localRow]`.

Other ranks need only `(relayRank, groupId, k)` to address their logical Combine
member slot. The source rank needs the representative `returnK`, not the relay's
local group ID. No group-ID reply to the source is required.

#### sourceMask

```text
sourceMask: int32[BS]
```

`BS` is this rank's input token count. For a fixed `BS`, routing changes values,
not this shape. Within the single-word representation, only the lowest `K` bits
are meaningful, interpreted as a bit mask with bit 0 on the right.

A set bit means that the corresponding source result slot participates in final
Combine reduction. That slot can contain an individual expert result or a
server partial representing several expert contributions. A bit does not mean
that an original expert route was discarded, nor does it imply a network
transfer when the contribution is local.

Start with the token's valid route mask. For each intermediate remote group,
clear its `memberMask` bits and set only its `returnK` bit. Direct routes,
including routes inside the source server, keep their original bits. Inactive
tokens or tokens without valid routes use zero and must not wait for payload.

For `BS=96, K=6`, this section contains 96 `int32` values, or 384 bytes. Each
element uses its lowest six bits. `0b010101` means source result slots k0, k2,
and k4 are valid; the remaining valid expert contributions have been folded
into those representative slots. An active token with at least one valid route
has between one and six set bits. This shape example is independent of the
four-rank routing example below. `K>32` requires a later representation extension.

### Generation and Recording During Dispatch

1. The original source processes a token's top-k routes together, determines
   server membership and relay selection, and computes per-destination ordinals
   using its existing route-position accounting. It writes `sourceMask` and
   sends hidden with the relevant forwarding descriptor. Original expert counts
   still describe all expert rows, including rows reached through a relay.
2. On a ready multi-destination packet, the relay assigns a group ID once,
   records source identity, `returnK`, and `memberMask`, and forwards the
   descriptor entries with that relay/group reference. Group allocation must
   have a unique owner, but its parallel allocation mechanism is still open.
3. Each expert receiver uses the original source identity, ordinal, and current
   count/cumsum layout to place the expert row. At that same point it records
   the original assist and `backRef` in `assistEx`.
4. When its own expert row is placed, the relay completes `groups[g].localRow`.
   Dispatch completion covers both the original outputs and all persistent
   metadata writes. No final scan of all expert rows is needed to discover
   token groups or reconstruct reverse routes.

The transient descriptor describes Dispatch work. `assistEx` and `forwardPlan`
retain the resulting reverse routing after that work and its scratch have ended.
They are distinct from mutable Combine payload, ready, credit, or completion
buffers.

### Use During Combine

1. A `DIRECT` expert row returns to the logical source slot
   `Result[sourceRank][sourceLocalTokenId][k]`, using its original assist.
2. A nonlocal grouped row writes to `GroupResult[relayRank][groupId][k]`.
   The relay's own member is read from `groups[groupId].localRow` instead of
   first copying it to a self-receive slot.
3. One owner reduces the members named by `memberMask`, in ascending original
   `k` order. Each remote member slot has one writer. The owner can consume
   ready members in that order while other transfers progress; it returns the
   complete group result to the source's `returnK` slot.
4. The original source consumes only the result slots named by `sourceMask`,
   again in ascending slot order, including directly available local results.
   It does not wait for original slots suppressed by a remote group's mask.

This is an execution plan for direct addressing and fixed reduction order;
Combine does not sort routes, rediscover server membership, or build a tree.
Every valid expert contribution must be weighted correctly and included once.
Weight distribution, partial precision/quantization, and the numerical effect
of regrouping sums still require separate decisions. In particular, the current
memory Dispatch rejects non-null route weights; the table alone does not make
remote weighted reduction possible or preserve bitwise accumulation order.

### Four-Rank Example: BS=2, K=2

`S0={R0,R1}`, `S1={R2,R3}`. Each rank has one expert and two input tokens.
`T0` through `T7` are display labels; stored identity always uses source rank
and source-local token ID. Thus T7 is `(R3, localToken1)`.

| Token | Source | Original target ranks, in k order | Dispatch path |
| --- | --- | --- | --- |
| T0 | R0 | R0/R1 | Retain on R0; R0 -> R1 |
| T1 | R0 | R2/R3 | R0 -> R2; retain on R2 and forward R2 -> R3 |
| T2 | R1 | R1/R0 | Retain on R1; R1 -> R0 |
| T3 | R1 | R2/R3 | R1 -> R2; retain on R2 and forward R2 -> R3 |
| T4 | R2 | R0/R2 | Retain on R2; R2 -> R0 |
| T5 | R2 | R3/R2 | Retain on R2; R2 -> R3 |
| T6 | R3 | R3/R0 | Retain on R3; R3 -> R0 |
| T7 | R3 | R0/R1 | R3 -> R0; retain on R0 and forward R0 -> R1 |

Only T1, T3, and T7 create intermediate groups. Local-source tokens T0, T2, and
T5 keep direct reverse routes, since their source is already the final owner.
The logical hidden transfer counts change from eight to five inter-server
transfers and from three to six intra-server transfers, excluding self-use.
All 16 expert rows remain. These are example message counts, not measured
latency or bandwidth results.

#### R0 Sending T1

R0 computes `q=0` for T1's route to R2 and `q=0` for its route to R3. It writes
hidden once to `Recv[R2][R0][0]` with local k0 and a forward entry
`{destinationRank=R3, destinationReceiveOrdinal=0, topKPosition=1}`.

R2 assigns its local group `g0`, retains the expert row, and forwards to
`Recv[R3][R0][0]` with `backRef=(R2,g0)`. R0 records `sourceMask[T1]=0b01`;
it does not need R2's group number to receive the eventual partial in k0.

#### R0 Receiving and Forwarding T7

R3 computes the following ordinals using only T6 and T7:

```text
Destination R0: T6 -> q0, T7 -> q1
Destination R1: T7 -> q0
```

R3 notifies R0 of two original-source receive slots and R1 of one. It writes
T7 hidden to `Recv[R0][R3][1]` with local k0 and a forward entry
`{destinationRank=R1, destinationReceiveOrdinal=0, topKPosition=1}`.

R0 observes that slot's current-generation ready state, reads the descriptor,
and assigns its local group `g0`. It forwards T7 to `Recv[R1][R3][0]`, carrying
original assist `(3,1,1)`, `backRef=(R0,g0)`, and no further forward entries.
The destination remains in R3's source partition, even though R0 performs the
write. R0 did not need T7's routing table before this descriptor arrived.

#### Complete Outputs by Rank

The tables below use expert rows sorted by display Token ID solely to give
concrete row indices. The implementation must store the actual Dispatch output
row in `localRow`; this illustration does not change the existing output order.
Group IDs are one possible local allocation for this example.

`assist=(sourceRank, sourceLocalTokenId, k)`; `backRef=(relayRank, groupId)`;
`DIRECT=(-1,-1)`. Every displayed mask is binary.

##### Rank 0

`sourceMask = [0b11, 0b01]`, corresponding to `[T0, T1]`.

| Output row | Token | Original assist | backRef |
| --- | --- | --- | --- |
| 0 | T0 | (0,0,0) | DIRECT |
| 1 | T2 | (1,0,1) | DIRECT |
| 2 | T4 | (2,0,0) | DIRECT |
| 3 | T6 | (3,0,1) | DIRECT |
| 4 | T7 | (3,1,0) | (0,g0) |

| groupId | sourceRank | sourceLocalTokenId | returnK | memberMask | localRow |
| --- | --- | --- | --- | --- | --- |
| g0 | 3 | 1 | 0 | 0b11 | 4 |

##### Rank 1

`sourceMask = [0b11, 0b01]`, corresponding to `[T2, T3]`.

| Output row | Token | Original assist | backRef |
| --- | --- | --- | --- |
| 0 | T0 | (0,0,1) | DIRECT |
| 1 | T2 | (1,0,0) | DIRECT |
| 2 | T7 | (3,1,1) | (0,g0) |

`groups` is empty.

##### Rank 2

`sourceMask = [0b11, 0b11]`, corresponding to `[T4, T5]`.

| Output row | Token | Original assist | backRef |
| --- | --- | --- | --- |
| 0 | T1 | (0,1,0) | (2,g0) |
| 1 | T3 | (1,1,0) | (2,g1) |
| 2 | T4 | (2,0,1) | DIRECT |
| 3 | T5 | (2,1,1) | DIRECT |

| groupId | sourceRank | sourceLocalTokenId | returnK | memberMask | localRow |
| --- | --- | --- | --- | --- | --- |
| g0 | 0 | 1 | 0 | 0b11 | 0 |
| g1 | 1 | 1 | 0 | 0b11 | 1 |

##### Rank 3

`sourceMask = [0b11, 0b01]`, corresponding to `[T6, T7]`.

| Output row | Token | Original assist | backRef |
| --- | --- | --- | --- |
| 0 | T1 | (0,1,1) | (2,g0) |
| 1 | T3 | (1,1,1) | (2,g1) |
| 2 | T5 | (2,1,0) | DIRECT |
| 3 | T6 | (3,0,0) | DIRECT |

`groups` is empty.

For T7, R1's row contributes k1 to `R0.g0`; R0 reads its own row 4 for k0.
The group returns one partial to `Result[R3][1][0]`. R3's second source mask
is `0b01`, so it does not wait for `Result[R3][1][1]`.

### Cost and Invariants

The extra raw metadata over the original three-column assist is
`8*Nrecv + 4*BS + 20*G` bytes per rank for the layouts above, excluding headers,
alignment, capacity padding, and transient descriptors. The four-rank example
adds 68, 32, 80, and 40 bytes on R0 through R3, respectively: 220 bytes total.
This is a layout calculation, not a measured kernel overhead.

Additional grouping can be performed while processing each token's routes,
using small direct-index maps and touched-entry bookkeeping. The design avoids
a global routing-table exchange, a sort by token after Dispatch, and rebuilding
groups in Combine. Existing route ordinal/count work still exists; the metadata
design does not establish the runtime cost of a particular grouping kernel.

Preserve these invariants when implementing or extending the design:

- A forwarded row retains its original source and destination ordinal. Replacing
  the source with the relay, or confusing `q` with local token ID, can collide
  with another row or break count/cumsum placement. Carry the source-computed
  ordinal and delegate exactly one payload writer per destination slot.
- Readiness covers hidden and descriptor together. A count notice or hidden-only
  flag cannot authorize consuming an unpublished forwarding descriptor.
- A relay receive slot is reusable only after both local use and forwarding
  finish. The persistent plan cannot depend on its scratch lifetime.
- Every valid original route is represented exactly once: either directly, or
  in one group's member mask. A group returns to one set source-mask bit, and
  suppressed source slots are not separately awaited or accumulated.
- Row IDs, group references, masks, and original assist belong to the same
  Dispatch generation. Example row order and group allocation are illustrative.

The example establishes the metadata's routing and indexing intent. It does
not validate MTE visibility, forward progress, buffer reuse on device, numerical
accuracy, or performance; those require implementation-specific validation.

## Remaining Design Topics

Further discussion will determine server mapping integration, relay load
balancing, core/task allocation, inter-server/intra-server overlap, ready/credit
protocols, workspace sizing and reuse, group allocation, and the concrete tensor
ABI. Multiple experts per rank may require a local-row list in place of a single
`localRow`; shared experts, wider top-k masks, route-weight distribution, and
partial precision/quantization also remain open. No implementation or hardware
qualification is implied by this document.
