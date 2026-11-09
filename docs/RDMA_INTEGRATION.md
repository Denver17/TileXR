# RDMA 集成、实现与验证 / RDMA Integration and Validation

This document is for maintainers and integrators of the optional RDMA backend.
Start with the bilingual [WRITE user guide](RDMA_API.md) for application setup,
the four AIV WRITE adapters, complete examples and cleanup rules. Use this
reference for build dependencies, source ownership, low-level contracts and
retained validation evidence. Clock diagnostics retain their original MTE path.

本文供集成与维护使用；应用接入、四种 WRITE 接口和完整示例见中英文使用指南。
下文区分当前实现契约与历史硬件记录，文档改写不表示重新运行了硬件验证。

## Supported paths

| Path | Producer and payload | Readiness and scope |
| --- | --- | --- |
| Host, `TILEXR_RDMA_MODE=host` | Host posts synchronous READ/WRITE through standard verbs; payload is registered Host RAM or supported original NPU memory. | `connected && hostDriven && !failed`; use `TileXRRDMAPut/Get`. |
| AIV, `TILEXR_RDMA_MODE=aiv` | An Ascend950 AIV writes UBNIC WQEs to exported device queues; Host still owns setup and registration. | `connected && deviceDirect && !failed`, plus every kernel result; use `RDMAWrite*` or the documented low-level entries. |

AIV resource/submission failure never selects Host posting. Host memory placement
and the producer of WQEs are separate choices. This RoCEv2 RC transport is
independent of UB UDMA: GID/QPN/PSN/rkey metadata cannot be replaced with UB
EID/Jetty/token metadata. Existing collective, EP and MoonEP kernels do not
automatically select RDMA; `CommArgs` and UDMA APIs keep their existing ABI.
The historical `ExtraFlag::RDMA` does not advertise this backend.

## Build dependencies

The project targets C++14/CANN 9.1. Follow [build verification](BUILD_VERIFICATION.md)
for submodule initialization, environment setup, build and installation.
`TILEXR_BUILD_RDMA=ON` is the default. CMake detects system verbs headers and
checks the extension header ABI. Extension support requires major version 4 and
minor version at least 2, with runtime version checks against the build headers.

| Dependency | Used for | Behavior when unavailable |
| --- | --- | --- |
| `infiniband/verbs.h`, `libibverbs.so.1` | NIC/PD/QP/MR management and Host posting | Compiles the unavailable provider without headers; dynamically missing libraries leave RDMA unavailable. |
| Matching `ibv_extend.h`, `libibv_extend` | NDA queue allocation/export for AIV | AIV setup is unsupported; no Host fallback for an AIV request. |
| ACL and the real `libascend_hal.so` | Device allocation, UB segment registration and doorbell mapping | Required operations fail setup instead of claiming device capability. |
| `kernel_operator.h`, AIV architecture `3510` | Device cache, MTE and `st_dev` adapter | Other targets return unsupported; compilation alone does not qualify transfers. |

`TILEXR_BUILD_RDMA=OFF` keeps public Host APIs available with an unavailable
backend. `dlopen` uses normal runtime lookup; never put toolkit `devlib` stubs in
RPATH/RUNPATH. Production targets neither include nor link `reference/` sources.
Use registerable original allocations, not remote IPC aliases. No SHMEM or Hcomm
is required by this backend.

## Configuration

RDMA is opt-in: `TILEXR_ENABLE_RDMA` defaults to `0`, accepts exactly `0` or `1`,
and must agree on all ranks. Configure before communicator initialization and
select the actual NIC associated with each rank's NPU. Other setup failures
normally disable the optional capability collectively; failed cleanup/control
exchange can still return an error. Check readiness after initialization.

| Variable | Default / accepted values |
| --- | --- |
| `TILEXR_RDMA_MODE` | `host` (default), or `aiv` for kernel-driven WRITE |
| `TILEXR_RDMA_DB_MODE` | Required for `aiv`: `soft` or `hard`, matching the installed firmware; never guessed |
| `TILEXR_RDMA_DEVICE` | Required explicit local verbs device |
| `TILEXR_RDMA_PORT` | 1; range 1–255, checked against NIC |
| `TILEXR_RDMA_GID_INDEX` | First nonzero RoCEv2 GID if unset; explicit index 0–255 |
| `TILEXR_RDMA_MTU` | 4096; 256/512/1024/2048/4096, limited by active local port and peer |
| `TILEXR_RDMA_TRAFFIC_CLASS` | 0; range 0–255 |
| `TILEXR_RDMA_HOP_LIMIT` | 64; range 1–255 |
| `TILEXR_RDMA_TIMEOUT_MS` | 5000; range 1–600000; Host completion deadline |
| `TILEXR_RDMA_DMA_MODE` | `indep_ub` for `hrn5_*`, otherwise `default`; explicit override accepted |

The bootstrap address (`TILEXR_COMM_ID='<rank-0-bootstrap-ip>:10067'`) is not the
RDMA GID. TCP reachability proves neither RoCE data reachability nor routed L3
forwarding. `InitThread`, allocation-only `Init/InitAll`, memory-only domains and
single-rank communicators do not initialize RDMA.

For an RDMA-only communicator, explicitly disable IPC/credit IPC/UDMA before
initialization, as shown in the user guide. These switches do not make existing
peer-memory kernels runnable. Where other transports are available their normal
behavior remains; topology alone must not choose memory versus UDMA.

RoCEv2 selection uses the extended GID query, with sysfs GID-type metadata as a
fallback. Ports must be active Ethernet. Peers exchange GID/QPN/PSN/MTU and
transition RC QPs through INIT, RTR and RTS. Fixed-width network-order metadata
also validates version, source/destination rank, QP index and MR generation.

For Host RAM, use `TILEXR_RDMA_DMA_MODE=default`, including on `hrn5_*`, to avoid
NPU segment registration. Supported original HBM on UBNIC uses `indep_ub`.
AIV mode requires `indep_ub` and an explicit firmware-matched doorbell mode.
Never infer the associated NIC from a logical NPU index or guess a GID/DB mode.

## Source ownership and dependencies

| File or component | Responsibility and neighboring dependencies |
| --- | --- |
| [comm/CMakeLists.txt](../src/comm/CMakeLists.txt) | Builds/installs the Host runtime and headers; probes optional verbs/extension support. |
| [comm_wrap.cpp](../src/comm/comm_wrap.cpp), [tilexr_comm.h](../src/comm/tilexr_comm.h), [tilexr_comm.cpp](../src/comm/tilexr_comm.cpp) | Public communicator entry points and owning lifecycle; call RDMA setup/teardown alongside existing transports. |
| [tilexr_sock_exchange.h](../src/comm/tools/socket/tilexr_sock_exchange.h), [implementation](../src/comm/tools/socket/tilexr_sock_exchange.cpp) | TCP bootstrap/AllGather control channel used for rank agreement and metadata exchange. |
| [tilexr_rdma_api.cpp](../src/comm/rdma/tilexr_rdma_api.cpp) | Bridges public Host functions and `TileXRComm` to the transport; agrees optional enablement/configuration. |
| [tilexr_rdma_config.cpp](../src/comm/rdma/tilexr_rdma_config.cpp) | Parses environment variables into `RdmaConfig` with concrete range/mode checks. |
| [tilexr_rdma_transport.h](../src/comm/rdma/tilexr_rdma_transport.h), [implementation](../src/comm/rdma/tilexr_rdma_transport.cpp) | Own regions, QP-count state and collective phases; delegate NIC resources to `RdmaProvider`. |
| [tilexr_rdma_verbs.cpp](../src/comm/rdma/tilexr_rdma_verbs.cpp) | Implements `RdmaProvider`, dynamically loads verbs/HAL, connects QPs and registers MRs. Delegates AIV ownership to NDA resources. |
| [tilexr_rdma_nda.h](../src/comm/rdma/tilexr_rdma_nda.h), [implementation](../src/comm/rdma/tilexr_rdma_nda.cpp) | Own extension QP/CQ handles, bootstrap MR, callback allocations and mappings. Plain verbs handles are borrowed aliases. |
| [tilexr_rdma_ub_res.h](../src/comm/rdma/tilexr_rdma_ub_res.h) | Private HAL resource mapping ABI consumed by NDA resources. |
| [Public and device headers](RDMA_API.md#7-文件分工与依赖--file-responsibilities-and-dependencies) | Host API/shared ABI, application WRITE adapters, device IO, pipeline scheduler and UBNIC wire encoding. |

The runtime path is Host API -> transport -> provider -> NDA/verbs resources.
The AIV path is WRITE adapter -> device adapter -> wire/pipeline algorithm ->
`QueueIO` cache/MTE/MMIO operations. Wire/pipeline IO templates are also used by
CPU models; those models are not a second device encoder or hardware evidence.
The RDMA files and their communicator/socket integration points carry bilingual
purpose/dependency overviews at the start of each source file.

## Resource lifecycle and failure agreement

1. Communicator setup agrees enablement/configuration and submission mode before
   allocating peer queues. Device-direct readiness requires actual resources and
   validated exports, not an NDA capability bit alone.
2. QP growth is collective, consistent across ranks and allowed only before MR
   registration. A partially failed expansion poisons the transport until teardown.
3. Register original caller allocations in matching rank order. Region generations
   are never reused; remote sizes may differ. Reject overlapping registrations in
   one communicator, and avoid overlapping UB segment ownership across libraries.
4. For UBNIC HBM, register the UB segment before the verbs MR. Deregister MR before
   segment release. AIV NDA MRs add the vendor access bit; Host NDR uses standard
   access flags. Queue setup owns a bootstrap MR before the first extension CQ.
5. Export Host POD views only after successful setup/registration, then copy them
   to GM. One QP has one state allocation across every MR/view; registration does
   not reset counters. Do not reuse expired/failed exports.
6. On the success path, explicitly `RDMAWait` on every owned QP before reusing
   source data or notifying remote consumers. Pending sends span calls and MR
   views; `aclrtSynchronizeStream` after a submit-only kernel is not an RDMA
   completion proof. Quiesce all local kernels/streams and remote consumers before
   collective cleanup.
   Destroy all peer QPs/CQs on every rank and agree success **before** MR release;
   then release segments, PD and context. Partial release retains ownership for retry.

Metadata exchange has an agreed result phase before consumers inspect metadata or
publish a handle. Preserve a local I/O failure even if a later exchange appears
successful: a misframed stream may supply stale zeros. A broken bootstrap poisons
the control channel; local queue destruction cannot prove that every remote QP
stopped. Keep MRs until process exit rather than freeing potentially NIC-referenced
allocations. Failed registration rollback can retain memory without returning a
handle; query Host failure state and require successful collective shutdown.

## Host posting API

Include `tilexr_rdma.h` (also included by `tilexr_api.h`) and select `host` mode.
`TileXRRDMAQuery` must report `connected && hostDriven && !failed` before posting.
Collectively register original allocations, then use synchronous
`TileXRRDMAPut(comm, region, peer, localOffset, remoteOffset, bytes)` or
`TileXRRDMAGet` with the same argument structure. Put writes local to remote;
Get reads remote to local. Offsets are bytes in the matching region generation;
both bounds are checked. Zero lengths, self transfers and sizes over `UINT32_MAX`
are rejected. Host calls serialize locally and use a signaled CQE per request.

Publish NPU-produced source data before Host submission. A successful local CQE
provides no application receiver notification; establish remote cache/stream
consumption dependencies explicitly. Post/poll/WC errors or timeout poison the
transport, and do not cancel outstanding memory references. Normal unregister
requires quiescence; after a posted error use collective shutdown before freeing.
Large Host READ has a retained data-visibility failure in the hardware record
below; successful AIV WRITE does not qualify that path. Host Put/Get reject AIV
transports.

## Low-level AIV queue contracts

Applications normally use `tilexr_rdma_write.h`. The lower-level entries in
`tilexr_rdma_device.h` remain available with different completion/scratch rules:

| Entry | Completion and capacity model | Required UB scratch |
| --- | --- | ---: |
| `RDMAPut` / `RDMAPutStridedBatch` | Synchronous completion, all WRs signaled; batch window bounded by actual WR/SQ/CQ limits. Single-SGE SQ/CQ ring reuse is supported by the low-level algorithm. | 512B |
| `RDMAPutSgeList` | Synchronous completion of one gathered WRITE and one CQE; rejects the WQE/stop crossing the physical SQ tail. | 512B |
| `RDMAPutPipeline` | Submit-only, per-QP SQ/WR-adaptive pending window across calls, each batch signaling its final WR; backpressure may wait; no physical SQ/CQ reuse. | 4608B |
| `RDMAPutSgeListPipeline` | Same submit-only scheduler with 1..16 gathered segments; SQ accounting uses multiple BBs per WR; no physical SQ/CQ reuse. | 20992B |
| `RDMAWait(view, scratch, maxPolls = 1000000U)` | Drains all shared pending pipeline batches on the selected QP, including other MR views; valid healthy empty/repeated waits do not poll. | Reuse WRITE scratch; only the 512B base workspace is used. |

All scratch is 32B-aligned caller-owned UB and remains valid through each call.
The pipeline and public `RDMAWrite*` adapters return success after submitting
the current WRs, without an unconditional tail drain. They can still block on
the oldest batch for WR-window, SQ or pending-record backpressure. `RDMAWait`, declared
in `tilexr_rdma_device.h` and available through `tilexr_rdma_write.h`, provides
the explicit send-completion boundary. It returns a device status that callers
must check; it neither traverses other QPs nor synchronizes remote consumers.
Each multi-QP owner waits on `views + qpIndex` with its own scratch and poll budget.
Host Put/Get and the legacy all-signaled device entries remain synchronous.

WQE publication and its MTE work finish within submission, so UB scratch and
kernel-local SGE descriptors can be reused when WRITE returns. Reuse the WRITE
scratch for Wait and keep it valid during Wait. Do not confuse those temporary
descriptors with the source HBM still read by the NIC: source contents, MRs,
QPs, shared state and relevant views remain owned through completion. Preserve
single-AIV QP ownership and ordered use across calls, MR views, streams and
kernels; no concurrent access capability is added. A submit-only kernel may
exit before the NIC completes, so stream sync alone is insufficient. Before
freeing allocations, also quiesce users and complete collective cleanup.

Both submission backpressure and Wait can report CQE errors or timeouts.
Every posted error leaves sticky QP state and does not cancel memory references;
retain resources for the existing quiesce/destroy protocol. Sparse signaling tags
the QP mode: a fully drained all-signaled QP may enter pipeline, but cannot return to an
all-signaled API. Public `RDMAWriteSge` selects the smaller single-segment
pipeline for one SGE; use `RDMAWriteScratchBytes` for application sizing.

### WQE, completion and publication

The current versioned UBNIC RC WRITE ABI has a 16B control, 32B RDMA task and
16B per local SGE, rounded to 64B WQEBBs. One SGE uses one BB; 2..5/6..9/10..13/
14..16 SGEs use 2/3/4/5 BBs. BDSL counts 8B units (`2*numSge`), task length is the
sum, the final SGE marks NEXT_SGE_INVALID and padding is zero. Every source uses
one local MR/lkey; one remote address/rkey receives the ordered gathered stream.

Assemble WQEs in UB. Publish the invalid stop, then continuation/tail BBs, then
the first control BB through completed MTE3 before ringing `st_dev`. A bulk copy
is not atomic across cache lines; NIC prefetch must never see valid control with
an unpublished tail. Keep fresh cache invalidation/CQE reads and event ordering.
The device adapter loads the view header first and rejects older/smaller views
before reading V3-only fields. Both Host view-output C symbols are versioned V3
as well, so an old caller cannot provide an undersized output allocation.

SQ `producer/completed` count BBs, CQ CI counts completion slots, and pipeline
WR ordinal is independent. CQE identity checks use the correct WR sequence;
partial successful prefixes do not retire a failed window. `maxPolls` bounds
snapshots per expected completion, not elapsed milliseconds. AIV errors require
kernel/state inspection in addition to the Host `Query.failed` value.

The 64B QP state persists FIFO producer/consumer and pending WR counts across
calls/MR views. Each QP additionally owns an HBM array of 8B records, with capacity
`min(maxSendWr, sqDepth-1, cqDepth-1)`. Each record stores the SQ end, final WR
identity and actual WR count. This covers variable batches and single-WR tails,
not just `ceil(window / currentBatch)` records. Backpressure checks pending WRs
against the effective SQ/WR window plus an optional application cap; BB differences
cannot replace WR counts when adjacent calls change SGE count or batch size.
Wait retires records FIFO only after a successful matching CQE. Software record
slots may be reused; hardware SQ/CQ wrap remains forbidden. The record buffer
shares QP lifetime and cleanup, is never NIC-registered, and is shared by all MR views.

### Limits, reuse and ABI

Query actual provider `max_qp_wr`, `max_cqe` and `max_sge` before queue creation.
Request at most 8191 send WRs, 8192 CQEs and 16 SGEs, clamped to advertised limits;
export negotiated `maxSendWr/maxSendSge` and actual SQ BB/CQ depths. Requested WR
admission and exported ring memory are distinct. A device admitting 4095 WRs
cannot meet a 4096-in-flight request just because its ring has more BBs.

The low-level all-signaled batch defaults to a 4096-WR upper window, additionally
bounded by `maxSendWr`, `sqDepth-1` and `cqDepth-1`. Public WRITE defaults instead
to requested batch64 and `min((sqDepth-1)/WqeBlocks(numSge), maxSendWr)` WRs in flight.
A positive application `maxInFlight` only narrows that window; the default
`UINT32_MAX` adds no cap. Actual batch length is `min(postBatch, window)`.
WRITE checks the **entire cumulative call** before posting; see
[capacity formulas](RDMA_API.md#5-参数与容量--options-and-capacity). Warmups, control
WRs and pending batches from earlier calls/MR views count toward admission:
reserve their unconsumed CQEs as well as new batch CQEs before any backpressure
drain or publication. Consuming CQEs does not restore physical no-wrap capacity.
Fresh QPs, not in-place state reset, are needed to keep tests free of wrap.
For example, seven rounds of 1000 data
WRs plus one control WR each need 7007 entries, exceeding a 4096-entry ring.

V3 exports a 168B view, 120B queue, 64B state and 16B SGE. The Host symbols are
`TileXRRDMAGetDeviceViewV3` and `TileXRRDMAGetDeviceViewsV3`; the source spellings
without suffixes map to them. Rebuild library,
Host application, kernels and headers together, and recreate queues/views.
Never reuse stale views after unregister/shutdown or fabricate copies of QP state.
Adaptive pending storage grows the view/queue from V2 and changes state semantics;
matching library/application/kernel/header implementations are required on a QP.
Do not mix old and new pipeline code. Successful Wait does not notify a remote consumer or order
different QPs.

## Clock synchronization over AIV RDMA

The diagnostics Host hooks use the public `TileXRRDMAGetDeviceView` spelling,
which selects the versioned symbol matching the current headers. Rebuild the
Host library and kernel together after an RDMA view ABI upgrade; a successful
textual rebase alone does not validate the Host hooks against the new API.

Build diagnostics with `-DTILEXR_BUILD_DIAGNOSTICS=ON`. The clock API keeps
its default MTE peer-memory path. Set `TILEXR_CLOCK_TRANSPORT=rdma` before
creating the detector to opt into device-driven WRITE; every rank must select
the same clock transport. Also configure the AIV RDMA backend above. With
`TILEXR_ENABLE_IPC=0`, no remote IPC mapping is needed. An unavailable NDA
capability is an error, with no Host or MTE fallback.

By default the kernel retains the five 64-byte messages (READY, REQUEST,
RESPONSE, TIMESTAMP, ACK), fresh magic/sequence checks, and midpoint timestamps. Each
rank owns one registered 2 MiB allocation containing its local mailboxes,
device views and serialized send staging. AIV publishes a message to staging,
constructs/publishes its WQE, issues `st_dev`, and waits for completion before
reusing the source. Receive polling reads only local mailboxes and invalidates
NIC-updated cache lines. Host handles setup and result collection, not message
submission. This is the NDA queue path, independent of SHMEM and UB UDMA.

Two explicit RDMA options reduce serialization; all ranks must agree on them:

| Options | Messages per sample | Source lifetime and CQ policy |
| --- | --- | --- |
| Default | READY / REQUEST / RESPONSE / TIMESTAMP / ACK | One staging slot; each send waits for its CQE |
| `TILEXR_CLOCK_RDMA_DEFER_CQE=1` | Same five messages | Distinct immutable source slots; drain every CQE at the end of the kernel |
| Both `TILEXR_CLOCK_RDMA_DEFER_CQE=1` and `TILEXR_CLOCK_RDMA_COMPACT=1` | REQUEST / RESPONSE / TIMESTAMP | Same immutable sources and complete end-of-kernel CQ drain |

Compact mode requires deferred mode. Registration and the launch control
collective establish initial readiness; receive slots are not cleared when the
kernel starts. A leaf sends its next REQUEST only after consuming the preceding
TIMESTAMP, so the next REQUEST also acknowledges consumption of the preceding
RESPONSE/TIMESTAMP. The root waits for that REQUEST before overwriting either
slot. The final result collective establishes that all receivers have finished
before another round or teardown. Each peer has separate receive slots, and
each send has its own source slot. The root still visits peers in order, but
compact mode can advance to the next peer while previous transfers remain in
flight. Different peer QPs can therefore overlap; four-rank elapsed time need
not equal three times the two-rank elapsed time. Deferred mode never interprets a remote flag
as SQ completion: it still checks every CQE's freshness, QPN, opcode, sequence
and error, publishes CQ CI, and verifies exact queue progress on the Host.
Successful CQ prefixes may retire on error; unresolved sources and the producer
remain owned until transport shutdown. Distinct staging must fit before launch.

RDMA `t0` and `t2` bracket completed WQE publication through doorbell issue,
using the midpoint of that scalar issue window. They do not include a subsequent
CQ wait, and do not claim to timestamp NIC consumption or packets on the wire.
Using the midpoint of the entire synchronous send previously included CQ
waiting and artificially shortened the computed RTT; older RTT/error-bound
values must not be compared directly with the corrected timing boundary. MTE
retains its original write-window midpoint. RTT/2 is only a theoretical bound
under these timestamp and delay assumptions, not a measurement of clock accuracy.

The offset model assumes symmetric **effective one-way delays**, not merely
the same physical route. With local timestamps `t0,t3` and reference timestamps
`t1,t2`, it uses `RTT=(t3-t0)-(t2-t1)` and
`offset=((t1-t0)+(t2-t3))/2`, selecting the minimum valid RTT sample.
The offset is reference minus local; add it to a local timestamp. A forward/reverse
delay difference introduces offset bias `(forward-reverse)/2`. Effective delays
include endpoint software, SQ/NIC queueing, transfer, and receive-poll detection.
The model also assumes stable counter rates and valid timestamp boundaries;
the RTT-derived bound does not cover arbitrary timestamp errors or later drift.

For an RDMA comparison, check that both endpoints use matching RoCE version,
GID address family, MTU, QP type/capacity policies, doorbell/DMA modes and clock
protocol options. Record each actual NIC, GID and QP configuration: names,
addresses and QPN values naturally differ. Matching configuration does not
prove equal effective delays. The clock API does not measure path symmetry.
`errorBoundCycles` is `ceil(minRTT/2)`; `precisionNs` is the maximum converted
per-rank bound. JSON retains the ABI-compatible field names and explicitly marks
the symmetry assumption, `path_symmetry_verified: false` and
`measured_residual_ns: null`. Repeatability or agreement between transports is
not an independent measurement of synchronized-clock residual error.

Clock synchronization and RDMA detector destruction are collective. Quiesce
other users of the communicator's RDMA QPs first. Before unregistering, all
ranks agree that their streams have stopped. After a clock transfer failure,
destruction uses `TileXRRDMAShutdown` to close the transport's QPs and release
MRs before freeing backing memory; this invalidates every RDMA view/handle on
that communicator. Failed cleanup retains the session for collective retry.
A broken bootstrap cannot establish safe MR release and requires process exit.

The current clock path deliberately rejects SQ/CQ wrap. Each round consumes
`3*(warmup+samples)` WRs per root-to-peer QP and
`2*(warmup+samples)` per peer-to-root QP; compact mode uses respectively
`2*(warmup+samples)` and `warmup+samples`. It checks actual queue depth and
progress before launch. Start a fresh communicator before capacity is exhausted.

For diagnosis, build separately with `-DTILEXR_DIAGNOSTICS_CLOCK_PROFILE=ON`
(default OFF). The integration executable exports per-state send phases,
receive-poll counts, and the deferred round CQ drain. Counters stay in UB until
the protocol finishes. Detailed profiling perturbs the loop; use an OFF build
for final latency. CQ read/decode subphases belong inside doorbell-to-CQE time
and must not be added again. Moving CQ waits may move the same underlying delay
into receive polling, so a large CQ phase alone is not evidence that all that
time is removable software overhead.

The integration executable accepts explicit physical device IDs, including
noncontiguous selections; launch one process per assigned device:

```bash
test_tilexr_clock_sync_e2e --rank-size "$RANK_SIZE" --rank "$RANK" \
  --device-id "$DEVICE_ID" --comm-mode socket --rounds 10 \
  --samples 128 --warmup 32 --output-dir "$OUTPUT_DIR"
```

Create the output directory first and use a shared `TILEXR_COMM_ID` for all
ranks. `RDMA_BENCH_CPU_ONLY` forbids running this executable on NPUs. Each rank
writes per-round metrics, raw measured samples and RDMA queue progress CSVs.
`host_ns` measures the synchronous API, including bootstrap collectives and
first-call registration; `kernel_cycles` spans the device protocol including
warmup and initial rank-arrival skew. Convert with `cycles_per_second`. Keep
first-call and steady results separate. The min-RTT/2 `error_bound_ns` is an
algorithmic uncertainty bound, not independent absolute-clock-error evidence.
The benchmark agrees all local validation outcomes before another clock round
or collective teardown, preventing mismatched bootstrap message sizes.

MTE requires peer-memory reachability, which RoCE TCP/bootstrap connectivity
does not establish. If cross-host `rtIpcOpenMemory` fails, retain that failure
and compare MTE versus RDMA on the same supported local pair. Cross-host RDMA
results must be reported separately. A direct same-subnet IPv4 RoCEv2 run does
not prove that the packets traversed a layer-three router.

## Verification

CANN-free Host checks (no NPU launches):

```bash
cmake -S tests/rdma -B /tmp/tilexr-rdma-build
cmake --build /tmp/tilexr-rdma-build -j4
ctest --test-dir /tmp/tilexr-rdma-build --output-on-failure
```

Tests cover multirank wire exchange, registration and read/write bounds, partial
failure agreement, stale handles, poisoned completions, cleanup order/retry,
C-header compatibility, enabled/disabled provider compilation, and an actual
provider call path through fake verbs/HAL DSOs. A real-library missing-device
check exercises optional-unavailable behavior. These tests prove Host contracts,
not NPU memory registration or L3 data-plane transfer.
The shared wire NIC model also covers batched SQ/CQ wraps, 16/24-bit rollover,
stride/counter overflow rejection before publication, partial completion errors,
and bounded window/doorbell counts for 48 and 1000 WRs. The reusable WRITE API
adds CPU contract coverage for options, scratch sizing, explicit QP selection,
SGE gather, progress and errors. These checks do not validate the device path.
Historical batch/multi-QP/multi-SGE measurements qualify only their recorded
source, binaries and configurations. Earlier single-WR or task-specific
benchmark results do not qualify the new public wrappers or establish their
bandwidth; a matching hardware run must verify complete data and guard regions,
per-QP progress, errors, capacity behavior and teardown.

The maintained Host CI script also runs this suite. A full production build still
requires CANN and the NPU driver headers/libraries.

The number of configured tests depends on available headers: verbs enables its
provider cases; compatible extension and ACL headers enable two additional NDA
mock cases. A 14-case Host run without those headers must not be described as a
16-case NDA validation. Preserve the detected build configuration with results.

## Hardware validation boundary

The following results were retained in the supplied patch. They were not rerun
for this documentation revision and qualify only their recorded sources, binaries,
environments and cases.

The submit/explicit-wait change is not qualified by those historical synchronous
runs. It remains unqualified pending matching CANN compilation and device evidence
from the green validation target for data, cross-call completion, errors/timeouts
and teardown. Both benchmark kernels
include successful Wait in their timed intervals; early submission return is not
a bandwidth improvement, and this migration makes no performance claim.

The supplied patch records a 41-case public multi-QP/WRITE API regression on 2026-11-04
on host 79, physical devices 0/2, CANN 9.3.0 and driver 25.9.rc1.8. This includes
all QP1/2/4/8/16 x SGE1/2/4/8/16 combinations at 64KiB per SGE with 65 WRs,
selected separate reverse-direction cases, partial/full batches through 1000 WRs,
and 1MiB QP1/4/16 transfers. Every case checked both ranks' complete
source/destination/guard regions, independent queues, SQ/CQ progress and cleanup.
The production library, pure-AIV tools and four documented kernel examples
compiled; the final Linux RDMA CPU suite passed 16/16, including delayed
multi-QP expansion failures and retryable teardown. Case groups and retained
evidence locations are listed below. This does not establish duplex,
queue-wrap, routed-L3 forwarding, cross-host or CANN 9.1 device behavior.

A supplemental 16-QP, SGE1, 512-WR test passed at 1KiB but failed at 2KiB with
CQ timeouts; 4KiB was not run after that failure. The unchanged pre-refactor
Host/kernel benchmark reproduced the 2KiB timeout on the same devices/NICs;
affected queues stopped at producer=192, completed=64, cqConsumer=1. The provider,
NDA resource and device wire/pipeline sources were unchanged. This is an existing
low-level limitation, not a successful regression point or a diagnosed root cause.
Do not suppress freshness/errors, reset state or extend polling to classify it
as passing. Preserve the failed samples separately from valid bandwidth data.

Keep the 2026-11-04 measurements under
`outputs/tilexr-multiqp-perf-20261104/` separate from this regression. Their
`full-shared-v2` and `sge-full-v1` evidence includes 16-QP short-payload CQ
timeouts; the single-SGE 1/2/4 KiB official points are missing. Successful
independent retries do not replace those original failures, and the API
refactor does not establish that the timeout is fixed. The following older
correctness results retain their original scope.

An Ascend950PR / CANN 9.3 / Ascend driver 25.9.rc1.8 / UBNIC firmware 30.5.2.8
single-host pair passed Host RAM 4 KiB bidirectional WRITE/READ (8 rounds), then
AIV HBM WRITE through the installed library's public communicator, registration
and device-view APIs. In the AIV run, each direction completed 513 writes of
4 KiB across two registrations, with two SQ wraps, complete-region pattern and
guard verification, and successful cleanup. A fresh process/connection repeated
257 writes per direction with one SQ wrap. Submission used independent UB DMA,
firmware-matched hard doorbells, UB resource mappings, MTE3 SQ publication and
`st_dev`; Host posting was disabled. The hardware CQ did not wrap in these runs.
These results establish bounded correctness, not throughput or routed L3 forwarding.

On the older CANN 9.2 / UBNIC environments, bootstrap MR registration succeeded
but extension CQ creation failed with `errno=22` and firmware FE lookup status 15.
Those environments remain unqualified for AIV; the numeric status alone does not
identify its root cause. Host RAM results do not qualify NPU HBM or AIV submission.
Large Host READ on the CANN 9.3 pair also exposed a completion/data-visibility
failure; READ investigation is deferred and is not covered by the AIV WRITE result.

Before device communication runs, reuse the matching retained environment
qualification required by `AGENTS.md`. Hardware validation must establish both
ranks' per-round data correctness, queue reuse and cleanup. Record the selected
NIC/GID, NPU affinity, software versions and binary provenance. L3 validation
additionally needs RDMA payload forwarding evidence, such as switch counters or
capture; a configured gateway or an ICMP TTL change alone is insufficient.

The retained 41-case public-API matrix consists of:

| Case group | Retained coverage |
| --- | --- |
| QP x SGE matrix | QP1/2/4/8/16 x SGE1/2/4/8/16, 64KiB per SGE, 65 WRs: 25 cases. |
| Separate reverse direction | SGE2 with each QP count: 5 cases. These are separate unidirectional runs, not duplex. |
| Partial/full batches | QP2/SGE4, 512B per SGE, 1/48/64/512/1000 WRs: 5 cases. |
| Large contiguous payload | QP1/4/16, SGE1, 1MiB, 512 WRs, both separate directions: 6 cases. |

A separate single-QP/single-SGE smoke passed. Each public-API case used fresh QPs
without queue wrap and checked complete source/destination/guards, independent
QPN/state, SQ/CQ progress and cleanup. Single diagnostic bandwidth samples do
not establish a repeated performance distribution.

Raw commands, device occupancy/resource records, source manifests and binary
hashes were retained outside the repository in
`outputs/tilexr-rdma-api-refactor-20261104/` and its `.deployment` evidence directory.
Earlier `outputs/tilexr-multiqp-perf-20261104/` records are a separate experiment;
keep `full-shared-v2`/`sge-full-v1` failures and independent retries separate. These
external records are evidence references, not files shipped in this repository.

### Earlier gathered-WR validation

A separate retained SGE record on Ascend950/CANN 9.3 exercised both directions of a same-host
NPU pair: 1/2/3/4/5/8/12/16 SGEs with uneven lengths and reverse source order,
plus 2- and 16-SGE 1MiB concatenations. Each call produced one CQE; ordinary and
strided WRs before/after it on the same QP completed with independent BB/WR counts.
Every byte of each 8MiB registered allocation, including source and guard regions,
was verified. CPU tests cover all 1..16 counts, golden bytes, capacity/bounds,
publication order, and sticky errors in both DB modes. Hardware exercised HARD DB;
SOFT multi-SGE, SQ wrap, multiple MRs, performance, and external L3 routing were
not validated by that same-host test.

## Lessons and source provenance

For matching AIV microbenchmarks, use [tools/rdma_compare](../tools/rdma_compare/README.md).
The opt-in `RDMAPutPipeline` separates CQ indices from WR ordinals, publishes up
to 64 descriptors per batch and signals each batch's last WR, including a partial
tail. It requires 4608B UB and rejects physical SQ/CQ reuse before posting. Keep
the invalid stop and tail-before-head publication order: a completed bulk copy
does not establish atomic visibility of all cache lines to a prefetching NIC.
After sparse signaling, the old all-signaled entry points cannot interpret CQ
progress and reject that tagged QP. Compilation/CPU models do not qualify AIV
DMA, and direction averages during duplex traffic do not substitute for a
unidirectional measurement.

`RDMAPutSgeListPipeline` composes that scheduler with ordered local gather.
Its UB requirement is `kPipelineSgeScratchBytes` (20992B for 64 maximum-size
WQEs). SQ producer/completed counters count 64B blocks, while in-flight limits
and sparse-CQE identities count WRs; 1/2/4/8/16 SGEs use 1/2/2/3/5 blocks.
Admission must reserve every block in the call plus its invalid stop before
posting. Advance each source segment by localStride and the concatenated remote
span by remoteStride; the two strides need not match. Keep scattered-source gaps
and reverse source ordering in validation so an accidental contiguous copy cannot
pass. When per-SGE bytes are fixed, bandwidth uses `bytes * numSge * WRcount`,
and plots must distinguish per-SGE bytes from aggregate bytes per WQE.

Kernel timer calibration must retain its event/cycle consistency check. Warm up
the calibration-only kernel before the measured calibration: first registration
or launch can add event time outside the kernel's counter interval. Log measured
ticks, event duration and ratio before rejecting a calibration; failed samples
do not become bandwidth measurements. Calibration must not post WQEs or consume
the fresh QP's queue progress.

When migrating a caller from synchronous WRITE to submission plus explicit
Wait, keep Wait inside any completed-transfer timer and propagate its error.
Stream synchronization proves kernel exit, not completion of pending RDMA.
Only UB scratch and kernel-local SGE descriptors are reusable on submission
return; source HBM and MR/QP/view ownership persist until completion or safe
collective teardown. CPU tests and historical measurements cannot qualify the
new device-completion boundary or establish a performance gain.

从同步 WRITE 迁移到提交加显式 Wait 时，completed-transfer 计时必须包含 Wait，
并传播其错误。stream sync 仅证明 kernel 退出，不证明 pending RDMA 完成。
提交返回后可复用的是 UB scratch 和 kernel-local SGE 描述符，不是 NIC 仍可能读取的
source HBM；MR/QP/view 必须保留至完成或安全集体清理。CPU 测试及历史测量不证明
新完成边界的设备正确性或性能收益。

- An A5 L3 RoCE connection needs GID/QPN/PSN/rkey metadata; UB EID/Jetty/token
  metadata cannot describe it. Keep capability and memory contracts separate.
- A local completion timeout leaves WR ownership uncertain. Preserve MRs and
  caller allocations until collective QP destruction completes; an error return
  alone does not make memory safe to free.
- The UBNIC reference registers UB segments even for its Host-driven NDR path.
  Preserve segment-before-MR and MR-before-segment teardown order, including MR
  registration failure and retryable partial cleanup. A failed registration can
  retain pinned memory without returning a handle when rollback also fails;
  consult `failed` and destroy before freeing that allocation.
- UBNIC extension CQ creation also requires an existing FE from MR registration.
  TileXR initializes queues before caller payload registration, so the independent
  backend owns one 2 MiB NDA bootstrap MR per resource owner/PD. Register its UB
  segment and MR before the first CQ; retain the MR until every QP/CQ is destroyed,
  then deregister the MR before releasing its UB segment and HBM. Failed MR
  deregistration retains the backing for explicit teardown retry. This mirrors
  perftest `ctx_init()`'s `create_mr()` before `create_cqs()` ordering.
- Queue-format and doorbell compatibility belong to a vendor NDA extension.
  A Host RC handshake must not be reported as successful AIV data-plane support.

Source: Ascend/perf-reference-ascend revision
`66a7475f610a12d05827c23ad4750f9181e3a5d3` (2026-10-29), specifically
[perftest_resources.c](https://gitcode.com/Ascend/perf-reference-ascend/blob/66a7475f610a12d05827c23ad4750f9181e3a5d3/rdma-perftest/perftest/src/perftest_resources.c),
[perftest_communication.c](https://gitcode.com/Ascend/perf-reference-ascend/blob/66a7475f610a12d05827c23ad4750f9181e3a5d3/rdma-perftest/perftest/src/perftest_communication.c),
and [acl_memory.c](https://gitcode.com/Ascend/perf-reference-ascend/blob/66a7475f610a12d05827c23ad4750f9181e3a5d3/rdma-perftest/perftest/src/canninterface/acl_memory.c).
The reference demonstrates the control flow; the selected CANN 9.1/UBNIC driver
and hardware remain the authority for actual NPU registration support.

- Host NDR success proves registered-memory networking but cannot prove NDA or
  kernel submission. Capability bits alone do not establish whether the standalone
  extension can create resources. Check its actual results, keep the advertised
  bits unchanged, and never substitute Host posting for AIV submission.
- UBNIC can return active-MTU enum 6 (8192 bytes), beyond upstream verbs' MTU
  names. Recognize it only for this provider and negotiate at most the configured
  standard MTU (4096 here); rejecting that active port value would prevent even
  the later NDA capability check. Reject unknown values rather than guessing.
- SQ PI, CQ CI, ring-local index and cycle/owner bits are separate values. Keep
  absolute counters per QP and cover ring and 16-bit sequence wrap in tests.
- Total WR count, QP count and outstanding depth are separate parameters. A
  synchronous single-WR loop keeps depth one even for thousands of WRs, serializes
  completion latency and is unsuitable for claiming peak bandwidth. State all
  three parameters, signal/CQ policy, timing boundary, direction aggregation and
  GB/s versus Gb/s units when comparing against Host perftest. Do not remove
  MTE publication ordering, freshness checks or teardown ownership to improve a
  microbenchmark; batch the work while preserving those dependencies.
- Doorbell mapping callbacks may repeat for several QPs sharing a page. Reuse a
  containing mapping with reference counts, preserve the exact offset/extent,
  and retain failed unregister/free resources for explicit cleanup retry.
- An exported `db_hw_va.iov_len` of 8 describes the doorbell register width,
  not its full mapped aperture. Hard doorbells use PI-dependent offsets covering
  2048 bytes. Validate that range against the owned mapping ledger, then expose
  that proven range to the kernel; do not reject a valid register-width export
  or assume an unverified aperture from the register width alone.
- Extension callbacks have no user context. Bind the resource owner only during
  synchronous extension calls with thread-local scope; never use a process-wide
  mutable current-device owner.
- `RDMAPutStridedBatch` accepts an optional compile-time observer after
  `maxPolls`; the existing overload uses `RdmaWire::NoBatchTrace`, without clock
  reads or trace storage. An observer implements `Stage`, `WqeStart`,
  `WqeEncoded`, `WqePublished`, `PollStart`, `PollRead`, and `PollDecoded` (the
  latter receives whether the snapshot was pending). Stage boundaries separate
  view/state setup, reservation, stop publication, WQE publication, doorbell
  issue, first completion, CQ drain and retirement. Stage events repeat for
  each window, so collectors must retain or aggregate each window explicitly.
  Observers must not touch queue/state ownership or add synchronization. Keep
  traces outside production performance baselines and compare coarse/detail
  instrumentation with an untraced control. A doorbell issue timestamp does not
  prove NIC receipt, and CQ polling time includes network/device waiting; it
  cannot all be counted as removable software overhead. Use a calibrated timer
  and report raw observer cost instead of subtracting guessed instruction costs.

### Clock messages and GM staging

Clock RDMA always stages each complete 64B protocol slot from UB into GM,
then publishes an ordinary one-SGE RDMA WRITE. QP creation does not request
inline capacity; providers with zero inline bytes remain supported. Deferred
sends use distinct immutable GM slots until the full-round CQ drain completes.
The synchronous path reuses its staging slot only after completion. Each WRITE
consumes one SQ entry and one CQ entry; the reserved queue-mode field stays zero.
Owner, QPN, opcode, sequence, syndrome, credit, no-wrap and teardown checks apply
to both completion policies.

Keep NDA tests focused on the active queue contract: when removing a posting
mode, remove its capability sweep and fake-provider controls together. Ordinary
queue creation, capacity, export and cleanup tests still cover the GM path;
varying an unused inline limit only repeats the same case.

An earlier inline experiment on CANN 9.3/Ascend950PR reduced the median of
per-round half-minimum effective RTTs from about 7.1us to 6.6us. The maintained
clock path uses GM staging to avoid a second queue format and an inline
capability requirement. Send preparation before
publication is outside the clock's effective RTT, and root processing between
receive and response is subtracted from it. Retain publication/receive timestamp
boundaries and raw samples when evaluating changes. Neither RTT/2 nor matching
bidirectional configuration measures synchronized residual clock error or
proves path-delay symmetry. Host queue models validate source lifetime and
completion accounting; they do not validate the NPU/NIC data plane.

The installed extension's `QP_ENABLE_DIRECT_WQE` flag advertises a separate
capability request. Its presence alone does not specify an AIV DWQE aperture,
payload format or publication ordering. The reference routine named
`roce3_post_send_deal_dwqe_and_db` still rings an ordinary SQ doorbell; its name
is not evidence of a working direct-WQE data path. Qualify those contracts
before using DWQE as a further optimization.

Protocol snapshot UB reuse needs both dependencies: `S_MTE2` before the copy
overwrites the preceding Scalar-consumed snapshot, and `MTE2_S` before Scalar
reads the new snapshot. Do not assume the compiler supplies the first edge;
the tested CANN 9.3 binary did not. Optional receive profiling separately
exports successful REQUEST/RESPONSE snapshot window counts and sum/min/max,
captured after the existing t1/t3 endpoints. Counts include warmup, and the root
aggregates its peers. Require nonzero expected window counts before using the
new fields: old profile binaries have zero-filled padding at those offsets.

A later Host-memory control built from the reference perftest actually
negotiated inline64 and reported median half RTTs of 6.14--6.21us on the same
NIC pair (two independent trials in each orientation). This narrows the gap
to AIV clock measurements but still uses different timing, source memory and
CQ policies; it is neither a matched NDA baseline nor a hardware-floor proof.
The inspected CANN SIMT DWQE implementation targets URMA/UB-CTP, whereas its
RoCE implementation uses `st_dev`. Never reuse an aperture offset from one
protocol for another without the matching provider contract.
# Corrected-clock validation

## Frozen offset and rate compensation

Set `TILEXR_CLOCK_DRIFT_WINDOW_US=20000000` before creating a detector to fit
the clock offset over approximately 20 seconds. Repeated `TileXRClockSynchronize`
calls collect the raw four-timestamp exchanges; callers control their sampling
cadence. The first exchange whose local-midpoint span reaches the requested
window closes training. The returned model records the actual span, point
count and training generation. Coefficients then stay frozen until the detector
is destroyed. A window of `20` requests 20 microseconds, not 20 seconds; the
exchange duration may permit too few points to fit a rate. Report such a model
as `INSUFFICIENT`, rather than silently extending the window or applying noise.

`TileXRClockGetDriftModel` returns the separate extension without changing
existing clock/config ABI layouts. For a READY model, `TileXRClockCorrectCycle`
and straggler timestamp alignment use:

```text
reference = local + offset_at_anchor + rate_correction * (local - anchor)
```

`rate_correction` is reference-minus-local cycles per local cycle. Multiply by
1e9 for the nominal ns/s correction; it has the opposite sign to uncorrected
local-minus-reference drift. Fit using centered cycle differences, keeping large
absolute timestamps out of the regression. Root mapping is identity. Rejected
models retain offset-only alignment; inspect model state explicitly. The model
does not change physical clocks, the AIV RDMA exchange or the RTT/2-derived
`precisionNs` field. The original synchronization result remains the per-call
offset candidate; use the model getter for the frozen affine mapping. Later
measurement rounds do not refresh the accepted rate model's Host calibration
age: the existing maximum-calibration-age check still limits its use.
Gathered model arrays are committed only after the complete collective and all
rank/window/version checks succeed; an interrupted or mismatched gather must
not replace the previously accepted mapping with partial receive data.

For validation, keep the fit closed, exclude its entire closing round, and use
new exchanges for at least a minute. Compare offset-only-at-anchor and affine
corrections on exactly the same held-out timestamps, retaining all outliers.
Verify raw sample provenance, queue progress, actual window and coefficient
immutability. Training RMS or a regression standard error is not independent
post-calibration accuracy or a guarantee of long-term extrapolation. Effective
path symmetry remains an assumption; changing asymmetric endpoint/network delay
can look like clock drift and bias either fitted coefficient.

`precisionNs` is half the minimum effective RTT, not a measured difference
between corrected clocks. For held-out two-rank validation, run the clock E2E
test with `--freeze-calibration 1 --round-gap-ms 250 --rounds 20`. Round zero
selects the production minimum-RTT correction; subsequent rounds reuse the raw
sampler but discard its candidate corrections and restore the first calibration.
The frozen correction is exported with every sample. Run
`tests/diagnostics/analyze_clock_residual.py --input RUN --output ANALYSIS`.

For local `t0,t3`, root `t1,t2`, and frozen additive local-to-root correction
`f`, the signed residual estimate is `f-((t1-t0)+(t2-t3))/2`. The two-card range
is its absolute value. This requires equal **effective** one-way delays and
approximately constant offset during each exchange. Matching NIC/GID/QP policy
does not prove those assumptions. Without an independent common event/time
reference, keep the independently measured residual null. Include all validation
outliers, the finite validation window and model assumptions; never substitute
RTT/2, launch/barrier skew, or calibration-sample fit for the requested maximum.

## Maintaining the documentation / 文档维护

When integrating communicator lifecycle changes, preserve every cleanup stage:
internal resources, profiling/kernel tilings, profiling slots, RDMA, then UDMA.
Keep rank agreement and early failure returns so a failed stage retains the
communicator for retry rather than deleting resources still owned by a peer.

合入 communicator 生命周期变更时，保留内部资源、profiling/kernel tilings、profiling slots、
RDMA、UDMA 的完整清理顺序，以及 rank 状态协商和失败返回；失败后应保留 communicator 供重试。

Describe the actual call path: Host resource preparation remains required for
AIV posting, and does not mean that the Host submits its WQEs. Keep examples,
header overviews, option units, scratch sizes and negotiated limits consistent.
Record validation by the tested mode and configuration; distinguish conditional
CPU coverage, retained hardware evidence and untested boundaries. This prevents
an application guide from promising a mode or capacity that its provider did not
admit.

维护时区分 Host 资源准备与 Host 下发 WQE；同步更新示例、文件说明、参数单位及实际协商限制。
验证记录应标明模式和配置，分开说明条件启用的 CPU 用例、历史硬件证据与尚未验证的边界。
