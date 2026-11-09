# ElasticMemory Torch NPU adapter

`tilexr_elastic` returns ordinary NPU tensors backed by the native ElasticMemory
pool. Compact Dispatch determines receive rows on Device, allocates the output
inside the running kernel, and resumes after Host growth when necessary. Tensor
Storage owns the resulting allocation; views keep that ownership alive.

This first adapter supports **inference on A5**, FP16/BF16 contiguous ND hidden
input, int32 routes, ordinary Memory EP, and FP32 Combine weights. It rejects
`requires_grad` inputs. There is no autograd implementation, graph-capture
support, UDMA direct output, shared-expert, quantized, or TP path here.

## Build

Build and install TileXR with EP and ElasticMemory enabled first, then use a
Python environment with a compatible installed `torch` and `torch_npu`:

```bash
source /path/to/cann/set_env.sh
cd integrations/elastic_memory_torch
TILEXR_INSTALL_PREFIX=/absolute/path/to/tilexr/install python setup.py build_ext --inplace
export PYTHONPATH="$PWD${PYTHONPATH:+:$PYTHONPATH}"
```

The extension uses the installed Torch C++ ABI and C++17. The native pool remains
C++14. Mailbox ABI v3 adds a separate final-status slot while keeping published
Output metadata immutable. Rebuild the native libraries, external kernels and
this extension together. It links installed Host libraries and uses their external AICore binaries;
it does not embed kernel objects. Runtime library paths must resolve the actual
NPU driver rather than CANN driver stubs. A tested Torch/CANN version must be
recorded separately; this source alone is not compatibility evidence.

## Use

Every rank creates an independent output pool and uses the same communicator
domain, `TILEXR_COMM_ID=IPv4:PORT`, rank order, and call sequence. Device selection is the application's
responsibility.

```python
import torch
from tilexr_elastic import ElasticMemory, DeepEPBuffer

torch.npu.set_device(device_index)
pool = ElasticMemory(device=f"npu:{device_index}", max_committed_bytes=1 << 30)
buffer = DeepEPBuffer(output_pool=pool, rank=rank, world_size=world_size,
                      comm_domain=domain)
pending = buffer.dispatch_compact_async(
    hidden, topk_experts=routes, num_experts=num_experts)
result = pending.materialize()  # Metadata ready; current stream waits for producer data.
expert_output = result.hidden * 2  # May be submitted immediately, without a Host wait.
combined = buffer.combine_compact(result.handle, expert_output,
                                 topk_weights=weights)
```

`materialize(stream=None)` waits for immutable address/shape metadata, then
enqueues an already-recorded producer completion event on the current or specified
NPU stream. It may return before Dispatch finishes. Native progression needs no
Python callbacks; waiting releases the GIL. Repeated materialization preserves the
same result and tensor Storage and establishes the dependency on each new stream.

The event guarantees execution order, not successful execution. Use
`pending.synchronize()` or `result.synchronize()` to wait for final producer status
and report late peer/runtime errors. A late error invalidates the output and any
results derived from failed data. `combine_compact()` and `result.discard()` also
check producer status before proceeding; successful Combine/discard releases the
communicator turn, and Combine waits for its own completion before returning.
A buffer permits one active routing handle. Discarding it preserves retained
output Tensors. Failure poisons the communication domain; it cannot accept the
next Dispatch.

For native callers, `Pending::Wait()` retains full-completion semantics;
`Pending::WaitMetadata()` returns the early allocation. Pair the latter with
`Allocation::WaitOnStream()` for each consumer and `Allocation::Synchronize()`
for final errors. The service still waits for full completion before admitting
the next same-pool FIFO request. It checks the independent ABI v3 `finalStatus`
after completion: zero sequence means no separate status, a nonzero sequence
must match the request, and a matching nonzero error fails the call. Published
Output is never overwritten to report a late error.

Zero-byte outputs retain the producer event too: their nonempty count Tensors
still need the stream dependency. The Runtime retains events until references
and queued waits can safely retire. Dropping Pending early does not cancel the
producer or release its input owners before completion.

The materialization stream is registered automatically. Use a scope to establish
the producer dependency and register every additional consumer stream:

```python
consumer = torch.npu.Stream(device=result.hidden.device)
with result.use_on_stream(consumer):
    value = result.hidden.square()
```

The scope flushes Torch's CPU submission queue before recording the consumer's
completion event. Storage destruction only schedules a native retirement hook;
that hook flushes registered Torch queues on the pool service thread before
recording final stream-tail events. It runs outside the pool lock so queued
Torch tasks can destroy other leases without a lock cycle. Keep NPU contexts and consumer streams alive until their
leases retire. `close()` rejects new submissions and drains admitted work while
preserving live Tensor storage. `trim(wait=True)` waits retired consumers and
returns completely empty segments; it does not wait for users to delete live
Tensors. `collect()` is a nonblocking request plus a current snapshot, so its
returned counters can precede the requested collection.

## Memory accounting and tests

While a producer is running, `live_bytes` includes its reservation: before its
size is published the pool conservatively reserves all unassigned committed
bytes; after output publication and validation it counts the Device-committed
reservation, including alignment and ring-wrap padding. That reservation becomes
an exported lease without double counting. `reclaimable_segment_bytes`
is zero while an allocator kernel is in flight. Completed-call snapshots report
exact reservation windows, and control `metadata_bytes` drops to zero after cleanup.

Allocation is single-owner, output-oriented and sequential within each segment.
Released allocations behind a live prefix cannot be reused until that prefix
retires: `blocked_free_bytes` reports those bytes as a subset of `free_bytes`,
not immediately allocatable capacity. `padding_bytes` reports alignment/wrap
padding included in live or pending reservations. No arbitrary-hole compaction
or concurrent Device allocation is supported.

Pool statistics cover ElasticMemory segments and control metadata, separately
from Torch allocations and EP communication staging. This Torch adapter configures
2 GiB data staging per rank, plus a 4 MiB flag area and a 1 MiB
memory-domain offset guard. `staging_bytes` reports configured data capacity;
`peer_window_bytes` includes the flag area. This adapter does not remove that
pre-existing fixed communication allocation.

Run the real hardware contract on two already qualified, available devices.
Set the same `TILEXR_COMM_ID=IPv4:PORT` in both processes. Start these commands
concurrently with the same unused communicator domain:

```bash
python tests/test_npu.py --rank 0 --device DEVICE0 --comm-domain DOMAIN
python tests/test_npu.py --rank 1 --device DEVICE1 --comm-domain DOMAIN
```

The test checks Dispatch/Combine against an independent routing oracle, ordinary
Torch operations submitted immediately after materialization, repeated
materialization on another stream, zero-output count dependencies, hot reuse,
growth, view ownership, cross-stream retirement, and close with live Storage.
Run a separate paired invocation with `--failure` to check late rank failure
through `pending.synchronize()`. The native held-producer test separately checks
that metadata can return before producer completion. These are hardware tests;
Python syntax checks or a successful build do not establish these contracts.

CANN 9.2 functional validation of this asynchronous change passed. The
[validation record](../../docs/validation/elastic-memory.md) separates the new
matched pipeline profiles from earlier allocation/kernel baselines. Neither
establishes a universal asynchronous speedup. The active Host poll loop
now requests a 5µs sleep; operating-system scheduling may delay wakeup, so this
value does not guarantee polling latency or end-to-end performance.

## Pipeline profiling

After a fresh combined idle/mapping gate, run two ranks of
`tests/benchmark_pipeline.py` with the same `TILEXR_COMM_ID` and a separate Gloo
`--rendezvous tcp://HOST:PORT`. For example, each rank supplies:

```bash
python tests/benchmark_pipeline.py --rank RANK --device DEVICE \
  --comm-domain DOMAIN --rendezvous tcp://HOST:PORT --mode metadata \
  --bs 16 --h 128 --warmup 20 --samples 100 --output /absolute/evidence/path
```

Repeat with `--mode full_wait` in the same evidence root; use a fresh root for
`--bs 256 --h 4096`. Both modes keep the framework profiler and the same Host
stage barrier enabled. The script checks ordinary Torch mul correctness and no
pool growth in measured samples, retains raw profiler records and loaded-library
hashes, and reports per-rank and cross-rank device Dispatch duration and
Dispatch-to-mul gap. Missing or ambiguous hardware tasks are reported as
unavailable. Host times are separate auxiliary observations. This compares two
waiting APIs in the same build, not old versus new binaries or isolated 50us
versus 5us polling; lower gaps alone do not prove earlier metadata observation.
