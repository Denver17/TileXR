# TileXR ElasticMemory Design

[中文](elastic-memory.md) · [Validation record](validation/elastic-memory.md) · [Torch adapter guide](../integrations/elastic_memory_torch/README.md)

ElasticMemory provides Device-side allocation for operators whose output size becomes known only during execution. Independent HBM segments form a single-owner segmented ring pool: the Device computes the actual size and obtains a contiguous range. If capacity is insufficient, the original kernel waits synchronously for the Host to allocate real HBM, then resumes at the allocation point. The final output is exported as an ordinary NPU Tensor. References and stream completion jointly determine when its memory can be reused or returned.

This document defines the architecture, ownership, and failure contracts. Section 11 distinguishes the current implementation scope from its validation boundaries. Environment details, performance measurements, source-to-binary provenance, and test commands belong in the validation record. This feature does not change the interface contracts of existing EP V1/V2, MoonEP, or upstream DeepEP, and does not require a separate Planner.

## 1. Use cases and constraints

[![Figure 1: ElasticMemory user scenarios](diagrams/elastic-memory/use-cases.drawio.png)](diagrams/elastic-memory/use-cases.drawio.png)

Figure 1 connects Device allocation, Host growth, Torch consumption, and whole-segment return. Materialization waits for stable metadata and enqueues producer completion on the consumer stream before returning. CANN 9.2 functional validation passed; measured pipeline behavior and limits are in the validation record.

- Dynamic allocation serves operator-level outputs. Multiple outputs with the same lifetime may share one bundle. An operator does not repeatedly allocate and free memory internally; this allocator is not intended for large numbers of tiny objects or arbitrary fragmentation management. “Usually large” does not impose a minimum allocation size: zero-size, alignment, and boundary requests must still be handled correctly.
- Each pool has only one active Device allocator owner. Only that core performs Device metadata operations associated with allocation and release. The physical core need not remain the same across kernels; concurrent multi-core malloc is unsupported.
- Calls to the same pool are admitted serially in FIFO order. All ranks must submit communication calls in the same order. Concurrent pipelines using different communicators use separate pools; collective order must not depend on the arrival order of Host threads.
- The Device requests actual bytes after counting and layout calculation inside Dispatch. `actual_rows` includes route replication, expert padding, and required alignment. Incomplete counts must not be treated as the final shape.
- Each Tensor has a contiguous valid storage range. A bundle cannot span segments or be split across the end of a ring. Live addresses remain stable: Tensors are not relocated, and noncontiguous segments are not presented as contiguous storage.
- Data HBM starts at zero; only bounded control metadata is created. A normal hit does not allocate Host-managed HBM or wait for the Host to wrap a Tensor before advancing the data path.
- When capacity is insufficient, the current kernel remains resident and waits for one Host growth response. Failure terminates the entire call and the associated communication domain, without retry, restart, replay, or rollback of data already written.

## 2. Components and ownership

| Component | Responsibility | Ownership and access |
|---|---|---|
| HBM segment | Holds contiguous output ranges | Created and freed by the Host Runtime; accessed by the Device |
| PoolState snapshot | Request header, current segment, and bounded segment directory for this call | Prepared by the Host between calls; read-only to the running kernel |
| RingState commit | Allocation and reclaim cursors for the selected segment | Committed by the Device owner; validated and mirrored by the Host after metadata publication |
| Host reservation queue | Allocation order, padding, lease/event state, and safe retirement prefix | Owned by the Host; not scanned in the Device hot path |
| Host-visible mailbox | Growth request, response, commit, immutable output descriptor, and final status | One writer per slot; paired by the current sequence |
| Native service | FIFO admission, growth, completion checks, and retirement | Uses the device context captured by the pool; independent of Python callbacks |
| Allocation lease | Retains producer, communication, and Storage usage rights | Shared by the native Runtime and Torch adapter |
| Kernel execution state | Completed counting, layout, and existing memory side effects | Preserved inside the original kernel while waiting for growth |

The Host validates a handle against the pool/device, segment ID, offset, bytes, and generation, then reconstructs the device address from a known segment. The Host does not dereference Device pointers. Bulk data stays in HBM; mapped Host memory carries only small control messages. Host pointers and Device mapping addresses are stored separately and need not have the same numeric value.

## 3. Single-owner segmented ring allocation

[![Figure 2: Single-owner segmented ring allocation and reclamation](diagrams/elastic-memory/segmented-ring-allocation.drawio.png)](diagrams/elastic-memory/segmented-ring-allocation.drawio.png)

Figure 2 separates normal hits, capacity slow paths, and retirement between calls. Timings are measured baselines from before this asynchronous change and do not establish its benefits; Section 10 defines their limits.

### 3.1 Contiguous ranges and wraparound

Each segment has a logical allocation tail cursor and a reclaim head cursor. Their difference describes the reserved window that cannot yet be crossed. The current implementation stores the physical tail position in `tailOffset` to avoid variable modulo in the hot path.

The owner applies the published reclaim watermark and aligns the candidate range using the actual base address. If the segment suffix cannot hold the entire bundle, the owner charges the suffix to this reservation as padding and attempts to wrap. It commits the cursor only if the payload, alignment padding, and wrap padding together fit the available capacity. A failed attempt leaves committed state unchanged. Address and size arithmetic must check for overflow.

A normal hit accesses only the compact current-segment state, with no free-slot search or arbitrary-hole compaction. If the current segment cannot satisfy the request, the owner checks the bounded segment directory. It requests growth only when no other segment has a suitable contiguous range. Sufficient logically free bytes do not imply that a contiguous range is immediately allocatable.

An empty ring may normalize its cursors at a safe boundary. This must not change live addresses or reuse an allocation generation that may still be observed. A zero-size output consumes no reservation and does not force data-segment allocation.

### 3.2 Contiguous retirement prefix

The Host uses Storage references and the completion of producers, communication, and registered consumer streams to identify each segment's contiguous safely retired prefix. It publishes the reclaim watermark before the next call. The Device owner validates and applies that watermark. The Host does not concurrently overwrite the running owner's cursors.

For `A → B → C`, if A still has a view or an in-flight consumer, releasing B/C cannot advance the reclaim cursor past A. Their space counts as logically free bytes blocked by the head of the ring. Later requests may use another segment or grow the pool, subject to the budget. Each reservation stores its padding and end cursor so that retiring the contiguous prefix also reclaims the padding.

The single-core constraint applies only to Device allocator writers. The Host remains responsible for lifetime checks and actual `aclrtFree` calls. Completion of Device production does not mean a Torch output can already be freed.

### 3.3 ABI and data movement

The current ABI v3 uses a 64B `RingState`: `Segment{id,address,bytes}` plus `head/tail/reclaim/tailOffset/alignment`. A nonzero request sequence is the generation of a nonempty allocation; `alignment` lets the Host validate the result after metadata publication.

The Host combines its mirror of the previous Device commit with the safe retirement watermark into a read-only `PoolState` for the current call, then copies it to HBM before launch. The current segment is first. The Device hot path uses one MTE2 transfer to read the 64B header and 64B current RingState. Other segments are read only when capacity is insufficient. The Host does not decide this call's output bytes or allocation offset in advance; after receiving the Device commit, it recomputes and validates the result.

For a successful nonempty allocation, the Device writes the selected segment's 64B state through MTE3 to a separate `committed` slot, waits for writeback completion, then publishes the 64B Output in order. Output publication first writes the complete payload with sequence zero, then publishes the complete descriptor with the current sequence. Allocation failure clears the valid handle/address and publishes an error. Invalid entry parameters return an error directly for the caller to handle. Once published, Output is immutable. Later peer or data-validation errors use a separate 64B `finalStatus` slot at mailbox offset 320, without overwriting an exported address or shape. After full producer completion, the Host checks final status: sequence zero means no separate status was reported; a nonzero sequence must match this request, and a matching nonzero error fails the entire call.

Current structural limits are 32 segments, 128 outstanding reservations, and separate limits of 128 registered streams and 128 events per lease. The reservation limit includes retired records still blocked by a live prefix. Exhausting control capacity must fail explicitly; counting only live leases must not permit unbounded growth. Producer events and wait-boundary events also have bounded control capacity; exhaustion rejects new submissions or waits. ABI v3 requires rebuilding the native libraries, external kernels, and Torch extension together.

## 4. Dispatch, growth, and FIFO

### 4.1 Actual size and the hot path

Within the same kernel, Dispatch completes the final receive counts, computes the actual layout of hidden and auxiliary outputs, and has the designated owner call `AllocateBundle`. Outputs with the same lifetime share a bundle and lease, with alignment appropriate to their dtype and communication requirements. The Combine handle retains routing and inverse mappings; users do not run a Planner in advance. Payload received before counting completes must remain in bounded staging governed by credits.

In the current Memory implementation, the final count core performs allocation. Only actual output cores receive and clear count flags. Prefix, expert-map, and count-output preparation overlaps owner allocation. Before copying, an output core waits for the immutable local address descriptor, observing the published magic before reading the complete body. Cross-core publication must use a visibility protocol validated for the target platform. It must not assume arbitrary blocks are simultaneously resident and synchronize them through an ordinary global spin barrier.

The owner's aggregation of peer errors may overlap local copies on other cores. The Host may export a Tensor once metadata is stable, but the producer event guarantees ordering rather than final success. A late error invalidates that Tensor and downstream results derived from failed data. Callers check final status through synchronize, Combine, or discard.

### 4.2 The original kernel waits for real HBM growth

1. If neither the current segment nor other candidates can satisfy the request, the Device publishes one growth request containing sequence, bytes, and alignment.
2. The original kernel waits at the allocation point for a matching response, preserving its preceding execution state and side effects. It does not write to an address that has not been allocated.
3. The native service checks the budget and allocates an independent HBM segment sufficient for the request. If a transport requires mapping or registration, those operations must succeed before the address is published.
4. The Host replies with a segment ID, address, and capacity, or an error. On success, the Device owner attempts allocation in the new segment exactly once, commits, and resumes from the original allocation point. A successful response that cannot accommodate the request is a protocol failure; it does not trigger another growth attempt.
5. Host allocation, budget, required registration, and post-allocation bookkeeping failures must all deliver a failure response to the Device. HBM allocated but not published is freed according to safe cleanup rules; the kernel must not be left waiting forever.

Host allocation and response must not depend on completion of the current kernel or be queued behind its subsequent stream operations. A growth request cannot wait until kernel completion to be read through a D2H copy. A platform that cannot perform these operations while the kernel remains resident does not support this path; it must not silently substitute exit and replay.

### 4.3 FIFO and communication order

Queued and in-flight allocation requests are bounded by `maxPending`; reaching the limit rejects new submissions. Requests not yet admitted wait in the Host queue without occupying AICore resources. An admitted kernel that needs growth holds its execution resources and waits while resident. Later requests cannot overtake the head request's sequence of “attempt allocation → wait for growth → obtain a range”; new space serves the head first.

The next request must wait until the allocator's write phase safely ends. Observing a published address alone is not permission to launch another writer. The current implementation prepares the next snapshot after full completion of the current call. Existing outputs may be consumed on other registered streams; this does not allow concurrent allocations in the same pool.

FIFO determines only allocation order within the pool. It does not replace address-readiness, credit, or remote-completion conditions in the communication protocol. The current Torch buffer permits only one active routing handle. The next Dispatch can be submitted after Combine completes or the handle is explicitly discarded.

## 5. Mailbox and visibility

Requests, responses, commits, immutable Output, and finalStatus use separate single-writer slots. ABI version, request sequence, and allocation generation reject duplicate or stale messages. Before wrapping output, the Host checks pool/device ownership, segment bounds, and the bytes required by the shape.

Write the complete descriptor before publishing its sequence in the platform-approved order. After observing publication, the reader obtains a consistent snapshot. C++ `volatile` or CPU atomics alone do not establish cross-device release/acquire semantics. The current Device growth wait combines volatile GM reloads with DCCI and checks the current sequence. Requests/results use ordered UB-to-GM writeback. Field widths, cache handling, slot reuse, and bidirectional visibility during execution must be validated with evidence for the applicable CANN/hardware combination.

The Host service does not depend on a caller holding the Python GIL, nor on a user calling `materialize` to make progress. The active poll loop requests `sleep_for(5µs)` each iteration; this is neither an operating-system wakeup guarantee nor an end-to-end latency bound. Measure Host polling, queueing, and CPU utilization separately.

## 6. Interfaces and asynchronous semantics

The sequence distinguishes metadata readiness from data completion. Early materialization is implemented; hardware validation and performance conclusions must be recorded separately.

[![Figure 3: Dispatch asynchronous-interface sequence](diagrams/elastic-memory/dispatch-sequence.drawio.png)](diagrams/elastic-memory/dispatch-sequence.drawio.png)

The C++14 native API consists of `Pool`, `Pending`, and `Allocation`. `Pool::Submit` records the input-stream dependency and executes the launch callback on the native service; the callback retains input owners until the producer fully finishes. `Pending::Wait()` preserves its original full-completion semantics, returning an allocation or throwing the final error. `Pending::WaitMetadata()` may return once immutable address/shape validation succeeds and the producer completion event has been recorded. Consumers must call `Allocation::WaitOnStream(stream)` to establish a dependency and use `Allocation::Synchronize()` to check the final result. The Device entry point is:

```cpp
// Called by a single owner; the public header is authoritative for the declaration.
template<uint32_t ScratchBytes = 128>
uint64_t AllocateBundle(DeviceView view, uint64_t bytes, uint64_t alignment,
                        uint64_t rows, uint64_t* address,
                        Profile::Recorder* profiler = nullptr);
```

Torch's `pending.materialize(stream=None)` uses WaitMetadata, enqueues the producer dependency on the current or specified stream, and returns ordinary Tensors, potentially before data completion. Repeated materialization preserves the same result and Storage and establishes dependencies for additional consumer streams. Waiting releases the Python GIL; the native service needs no Python callback. See the [adapter README](../integrations/elastic_memory_torch/README.md) for examples.

| State | Meaning |
|---|---|
| METADATA_READY | Final address/shape is stable, allocation succeeded, the lease can be delivered, and the producer completion event is recorded; final execution success is not promised |
| DATA_READY | Producer writes and required communication accesses are complete; final status must still be checked |
| FAILED | This result and downstream results derived from failed data are invalid; clean up after associated accesses stop, without resuming execution |

`pending.synchronize()` or `result.synchronize()` waits for full producer completion and reports late errors. `combine_compact()` and `result.discard()` also check final producer status first. Combine returns after its own completion; successful Combine/discard releases the communicator turn. Failure poisons that domain, so it cannot be reused by another submission.

The producer event guarantees stream ordering only. It does not replace error checking or guarantee that the Host learns of every peer error before downstream work starts. The Runtime retains the event through reference and queued-wait safety boundaries. Zero-byte outputs retain this dependency too, because count Tensors may still need it. Early metadata return does not change same-pool FIFO: the service admits the next request only after the current call fully finishes.

Dropping a pending object does not cancel the device task. The Runtime retains input owners and any early lease until safe completion, then retires it according to Storage and consumer conditions. Forced cancellation during execution is not provided.

## 7. Tensors, streams, and capacity return

### 7.1 Storage lease

The adapter creates NPU Tensors through the external-memory factory matching the installed `torch_npu` version, setting device, dtype, shape, stride, and ND format. It currently uses `at_npu::native::from_blob`, without copying output or replacing the global Torch allocator.

A custom Storage deleter captures the native lease. When the last Storage reference disappears, it submits a retirement request rather than freeing HBM directly. Views, detach, and repeated materialization share storage ownership. A Python finalizer cannot replace Storage retention. Training also requires separate autograd and backward-stream integration; `requires_grad` is currently rejected.

The materialization stream is registered automatically and receives a producer-event dependency. Repeating materialization on a new stream preserves Storage and establishes that dependency there. Additional consumer streams can also be registered with their dependency through `result.use_on_stream(stream)`. Torch's `record_stream` does not automatically establish safety for a custom segment; consumption on unregistered streams is outside the supported contract.

Tasks in Torch's CPU submission queue may outlive Python references. The native retirement hook flushes registered queues on the service thread, outside the pool mutex, then records final stream-tail events. A Storage deleter that may run on a Torch queue worker must not synchronously flush that queue. The device context and registered streams must remain valid until lease retirement.

### 7.2 Logical retirement and physical return

A range can retire only when no Storage/export or pending-materialization handle retains it, and its producer, communication, and all registered consumers have completed. The ring pool then advances only the contiguous safe prefix. Logically free, available for allocation, and eligible for whole-segment HBM return are three distinct states.

At a safe boundary, an empty segment is excluded from future snapshots and checked for in-flight accesses. If a transport holds registration resources, the corresponding deregistration must complete before release. A segment-table version still used by a kernel must not be freed, and live addresses must not be moved to create space.

- `collect()` nonblockingly requests processing of eligible retired objects and returns the current snapshot. Returned counters may precede the effects of this collection request.
- `trim(wait=True)` waits for device work associated with retired objects and releases eligible whole segments. It does not wait for the user to delete live Tensors or forcibly release them.
- `close()` rejects new submissions and drains or fails in-flight requests. Live Storage retains the native Core and segment; closing admission does not mean all HBM has been released.

The free-cache budget controls how much capacity is retained in empty segments and may be zero. A small view can still retain an entire segment. Growth selects segment size from the request, alignment, Runtime granularity, and bounded spare capacity, rather than doubling indefinitely. `max_committed_bytes` limits actual committed capacity, although Runtime OOM may occur sooner. A preferred segment size must not prevent a smaller request from succeeding within the remaining budget.

[![Figure 4: Storage retirement, pool reuse, and physical HBM return](diagrams/elastic-memory/reclaim-sequence.drawio.png)](diagrams/elastic-memory/reclaim-sequence.drawio.png)

Figure 4 distinguishes loss of Storage references, completion of accesses, reuse of a contiguous prefix, and whole-segment return. A live view may retain an entire segment.

### 7.3 Statistics

| Metric | Meaning |
|---|---|
| live_bytes | Active leases and in-flight producer reservations, including their alignment/wrap overhead |
| pending_retire_bytes | Reservations no longer retained by the user whose accesses have not all finished |
| free_bytes | Total logically free segment capacity, including space temporarily blocked by the ring head |
| blocked_free_bytes | Subset of free_bytes that cannot pass a live prefix; not added again to the total |
| padding_bytes | Alignment and wrap overhead in live/pending reservations; not additional HBM |
| committed_bytes / peak_committed_bytes | Current actual committed data-segment capacity and its peak |
| reclaimable_segment_bytes | Capacity that can safely be returned as whole segments |
| largest_free_range | Largest allocatable range under ring-watermark and contiguity constraints |
| metadata_bytes | Control storage included in accounting; whether Host container and thread overhead is included must be stated |

Accounting satisfies `committed_bytes = live_bytes + pending_retire_bytes + free_bytes`, with no overlapping categories. Before a producer publishes its actual size, unassigned committed space is conservatively reserved and must not be reported as reclaimable. `reclaimable_segment_bytes` is zero while allocator work is in flight. After metadata publication, reservations are validated against the Device commit and registered exactly once. Early lease export neither double-counts them nor removes producer reclamation protection.

The current metadata counter includes the mapped mailbox and Device PoolState, but excludes Host map/deque, lease/event, and service-thread object overhead. The Torch allocator does not automatically account for these external segments. Total HBM usage must not be inferred from `torch.npu.memory_allocated()` alone, and the Torch cache is not automatically emptied.

## 8. Communication integration and failure contract

The current implementation pools final local outputs, retains existing communication staging, and then copies into the output. Output, staging, auxiliary flags/guard, and copy traffic must be reported separately. This is not communication zero-copy, and savings must not be offset by another hidden maximum-output buffer.

Direct peer-memory/UDMA writes into pooled output are a separate extension: complete mapping/registration and exchange peer descriptors before publishing addresses. Registration must also progress while the original kernel remains resident. UDMA registers only permitted owner-local allocations, not remote IPC mappings, and does not register in `InitThread`. WQEs are built in UB and published through MTE3; the `st_dev` doorbell is used only after publication completes. Retirement and deregistration must wait for remote protocol completion. Transport selection depends on capabilities, registration, and size, not topology alone.

| Condition | Required behavior |
|---|---|
| First nonempty call | Obtain space through growth from zero data HBM; pre-allocation side effects occur exactly once |
| Empty receive or all inactive | Return zero-size output correctly while completing the communication protocol |
| Current segment insufficient or head blocked | Try other segments; if none fits, the original kernel waits for Host growth |
| Budget exceeded, OOM, failure of required registration, or bookkeeping failure | Publish an error, end associated waits on participating ranks, and terminate the call/domain and queued requests |
| Stale, duplicate, or out-of-bounds descriptor | Reject delivery and duplicate release; enter the appropriate error handling |
| Kernel or communication failure | Preserve the first error; quarantine resources whose accesses have not been confirmed stopped rather than reclaiming them early |
| Partially written output | Invalidate the result without rollback or restart; clean up resources after accesses stop |

Dropping tokens, changing routing, or reducing dtype must not be used to disguise failure as success. An error must not merely throw on one Host thread while leaving the Device waiting forever. If the device or communication cannot cooperate to exit, use the existing Runtime/job failure mechanism; ElasticMemory does not provide a fault-recovery system. Resource cleanup does not restore contents or make a failed domain reusable.

[![Figure 5: Host growth failure propagation and cleanup](diagrams/elastic-memory/failure-sequence.drawio.png)](diagrams/elastic-memory/failure-sequence.drawio.png)

Cleanup in Figure 5 requires confirmed termination of accesses. Writes may have occurred before failure; cleanup does not imply rollback or resumed execution.

## 9. Diagram sources and maintenance

The figures are embedded in the relevant sections throughout this document. Each PNG contains editable diagram data and can be opened at full size through its image link. Sources and measurement details:

- [Segmented ring draw.io](diagrams/elastic-memory/segmented-ring-allocation.drawio) · [Flow and measurement notes](diagrams/elastic-memory/segmented-ring-allocation.md)
- [Device profiling draw.io](diagrams/elastic-memory/device-allocation-profile.drawio) · [Stage definitions and evidence](diagrams/elastic-memory/device-allocation-profile.md)
- [Four-page scenario, Dispatch, failure, and reclamation draw.io](diagrams/elastic-memory/elastic-memory.drawio)

Keep diagram sources, PNGs, and both language versions synchronized. Distinguish metadata readiness, producer completion, and validated behavior beside the figures. Event ordering must not be presented as error checking, nor implementation as hardware validation. The shared diagram labels are in Chinese; the accompanying English sections describe the same contracts.

## 10. Performance and validation requirements

Figure 6 and earlier allocation/Dispatch measurements are baselines from before this metadata-only / 5µs poll change. The new functional tests and matched pipeline profiles are recorded separately in the validation record; prior measurements establish neither an asynchronous speedup nor a latency guarantee.

[![Figure 6: Device allocation flow and per-rank profiling times](diagrams/elastic-memory/device-allocation-profile.drawio.png)](diagrams/elastic-memory/device-allocation-profile.drawio.png)

Figure 6 uses SDK-instrumented measurements for internal stages and separate runs with Device diagnostics disabled for the complete Dispatch comparison. Stages are nested and may overlap work on other cores; adding or directly subtracting them does not isolate allocation benefits.

The allocation hot-path target for normal hits and nonempty wraparound without growth is **≤2.5µs**. The interval runs from entry into the Device allocation interface through completed result publication, including initialization, state reads, reclaim-watermark application, allocation arithmetic, commit, publication, and required synchronization. Report segment switches and Host growth separately without moving ordinary per-call costs out of the measured path. Record median/min/max/P95 per rank, sample counts, and samples above the target; do not treat the observed maximum as a bound for every workload.

Core operator performance compares device kernel execution time only. With identical shape, inputs, routes, dtype, ranks, communication path, compilation, and diagnostic configuration, compare maximum-capacity preallocation, a Host actual-size reference, and the Device pool. Pool capacity must be sufficient, and growthCount must remain unchanged throughout measurement. The Host reference is a comparison only and must not become a size Planner in the production path.

Diagnosis uses the repository's `TileXR::Profiling` SDK with capture on the actual service stream. Collect runs after completion and must not block the growth service.

The consumer uses standalone SDK 1.0.0 / DeviceView ABI 2; rebuild Host and kernel together. `tests/elastic_memory/profile_compact.py` declares participants and workload before launch, merges rank-owned manifest, completion and Host-event evidence after completion, and delegates repeat reports to the public capture-set API. The mapping template leaves the timer scale unset: supply an evidenced scale and its basis. Read allocator samples from the owner lane, rather than treating an absent stage on the slowest lane as zero allocation time.

Validate timer conversion, all-lane/key-owner markers, overflow/drop, and instrumentation disturbance. Do not directly substitute HWTS/AIV/sysClock frequencies for the GetSystemCycle frequency or subtract timestamps across lanes without establishing that this is valid.

Formal comparisons remove Device diagnostic instrumentation, retain identical Host stage barriers/framework profiler settings, and measure msprof Task Duration for the named Dispatch/Combine kernels. Allocation, synchronization, and cross-rank waits inside the operator remain included; Host wall time or Task Wait Time is not a substitute. Cover at least bs16/h128 and bs256/h4096 with BF16/topK2/2 ranks, retaining warmup, sufficient samples, and per-rank/cross-rank distributions. Improved medians do not establish improved tail latency.

Reports must also include backend, native status, kernel version, data volume, applicable bandwidth, actual output bytes, committed/peak/trim, blocked/padding, and retained staging. Host submission/queueing, materialization, CPU utilization, and growth/registration/trim latency are auxiliary observations, excluded from core kernel performance.

Functional validation covers real growth before kernel completion, preceding side effects exactly once, reads/writes at the new address, failure exit without replay, FIFO head priority over newly grown space, initially zero data HBM, wraparound/segment switch/overflow/zero size, lifetimes and whole-segment return, and real multi-rank Dispatch/Combine. Host models and the M0 feasibility experiment do not replace full integration tests. Async validation must separately hold a producer in flight to prove early metadata return, confirm full Wait semantics, and cover immediate Torch consumption, repeated materialization on another stream, zero-output dependencies, late errors, and early reference drops.

Before every hardware launch, recheck proc-mem and identity mapping for all target physical cards. Reject busy devices, query/parse failures, and unknown mapping. An empty `fuser` result does not permit launch. Do not kill/reset external tasks. Reuse existing HCCL qualification for the same environment; record incomplete evidence as a qualification boundary rather than rerunning it per task.

## 11. Current support boundaries

| Item | Current scope |
|---|---|
| Platform | Measured on CANN 9.2.0 and A5/Ascend950DT; compatibility with other versions or hardware is not promised |
| Compilation and deployment | Native C++14; Torch extension uses C++17/ABI matching installed Torch; pure AICore external binaries are registered and launched, without embedding kernels in Host libraries |
| EP | Memory Dispatch/Combine with pooled local outputs; no separate Planner |
| Torch | Ordinary FP16/BF16 contiguous ND Tensors, int32 routes, and FP32 Combine weights; inference only |
| Asynchrony | Metadata-only materialize, consumer event dependencies, and explicit final-error checks are implemented; native Wait and Combine retain full-completion semantics; CANN 9.2 functional validation passed; see the validation record for timing and limits |
| Lifetimes | Storage/views, registered streams, pending input owners, and live Storage retained across close |
| Extensions not covered | Autograd/backward, graph capture, UDMA direct output writes, multi-host, quantization, shared-expert, and TP paths |

The [validation record](validation/elastic-memory.md) is authoritative for current allocation and complete-kernel measurements, uncovered cases, and incident disclosures. Goals in this design are not claims of validated capabilities. Development owns implementation, review, all tests, and evidence. Design acceptance checks submitted results against the contracts and does not require the design session to compile or repeat hardware validation.

## 12. Code and maintenance entry points

| Content | Path |
|---|---|
| Native API | [tilexr_elastic_memory.h](../src/include/tilexr_elastic_memory.h) |
| Shared ABI | [tilexr_elastic_memory_abi.h](../src/include/tilexr_elastic_memory_abi.h) |
| Device allocation | [include/device/tilexr_elastic_memory_device.h](../src/include/device/tilexr_elastic_memory_device.h) |
| Device profiling | [include/device/tilexr_elastic_memory_profile.h](../src/include/device/tilexr_elastic_memory_profile.h) |
| Ring algorithm and Host service | [allocator.h](../src/elastic_memory/allocator.h), [pool.cpp](../src/elastic_memory/pool.cpp) |
| Dispatch kernel | [tilexr_ep_dispatch_memory_kernel.h](../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h) |
| Torch adapter | [README](../integrations/elastic_memory_torch/README.md) |
| Functional tests | [tests/elastic_memory](../tests/elastic_memory), [Torch tests](../integrations/elastic_memory_torch/tests) |
| Related designs | [EP Memory](specs/ep-memory.md), [Profiling SDK](specs/profiling.md) |

Device headers install under `include/device/`; Host API and shared ABI headers install under `include/`. Moving a public header requires updating includes, CMake dependencies, and installation rules together, then compiling an external consumer against a clean install. The Chinese and English design versions must preserve the same contracts. Keep environment details, raw logs, commits, and binary hashes in the validation record so that the design document does not become an execution history.
