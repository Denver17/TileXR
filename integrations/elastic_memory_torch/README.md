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
C++14. Ring ABI v2 changes both the internal state and public statistics layout;
rebuild the native libraries, external kernels and this extension together. It links installed Host libraries and uses their external AICore binaries;
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
result = pending.materialize()
expert_output = result.hidden * 2
combined = buffer.combine_compact(result.handle, expert_output,
                                 topk_weights=weights)
```

`materialize()` currently waits for complete producer execution while releasing
the Python GIL; the native service progresses without Python callbacks. Repeated
materialization preserves the same tensor storage. `combine_compact()` currently
waits for its recorded completion event before returning. A buffer permits one
active routing handle: combine it or call `result.discard()` before submitting
another Dispatch. Discarding the handle does not invalidate retained output
Tensors. Any communication failure poisons that domain; create a fresh domain
only after the failed execution has safely ended.

The materialization stream is registered automatically. Use a scope for every
additional consumer stream:

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

The test checks Dispatch/Combine against an independent routing oracle, normal
Torch consumption without output copies, hot reuse, growth, zero receive rows,
view ownership, cross-stream retirement, and close with live Storage. Run a
separate paired invocation with `--failure` to verify that rank-0 allocation
failure also terminates rank 1. These are hardware tests; Python syntax checks
or successfully building the extension do not establish these contracts.
