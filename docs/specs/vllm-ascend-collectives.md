# vllm-ascend Collective Integration Specification

Architecture, call stacks and runnable usage examples are in the
[vLLM integration design and guide](../VLLM_ASCEND_INTEGRATION.md).

## Scope and components

`integrations/vllm_ascend/` adapts ordinary eager-mode `NPUCommunicator`
collectives without changing the native ABI. Fusion operators (including
`npu_mm_all_reduce_base`) and graph/compile execution are outside this scope.
Use `enforce_eager=True` for the qualified model path.

- `runtime.py` loads `libtile-comm.so` and `libtilexr-collectives.so`, declares
  their ctypes ABI, and propagates native errors.
- `group_binding.py` binds one independently owned native runtime to each
  framework CPU ProcessGroup during communicator construction.
- `torch_collectives.py` handles dtype, layout, raw pointers and stream lifetime.
  Its rank/size/device cache is for standalone smoke tools only; framework
  communicators always pass their owned runtime explicitly.
- `vllm_adapter.py` preserves framework mutation, dimension and local-root
  contracts and orders calls that share native workspace.
- `vllm_patch.py` wraps construction, ordinary collective methods and destruction.
  `sitecustomize.py` activates the patch only when
  `VLLM_ASCEND_TILEXR_COLLECTIVES=1`; explicit patching is also supported.

## Communication group ownership

Use the registered CPU ProcessGroup's ordered global members, local rank and
size. The group root allocates a domain from its process-local sequence and
root global rank, reserving a device-sized interval for the native default
endpoint's device offset. It creates a 128-byte unique ID with
`TileXRGetUniqueId`, broadcasts the complete bytes (including embedded zeroes)
inside that CPU group, and all members call `TileXRCommInitRank` with that ID.
Distinct groups, including groups with identical members/rank/size, have
independent domains, native flags, memory and handles. No new native API is
required. Group identity is exposed on the adapter as `domain` and `members`.

This allocator is qualified for one node with one worker per global rank and
uniform device visibility. Separate jobs must use separate bootstrap endpoints
(`TILEXR_COMM_ID`). The domain is an offset to the configured native bootstrap
port, not an unbounded namespace; exhausted port range is a startup error.
This qualification does not establish multi-node or UDMA domain behavior.

Before native bootstrap, ranks use one CPU all-gather to agree that native
libraries and symbols loaded. The CPU group then distributes the root's unique
ID or creation error. There is no per-operation CPU agreement or distributed
recovery protocol. The framework continues to own collective participation,
ordering, worker failure handling and startup timeouts.

## Tensor and stream contract

AllReduce returns and mutates the same input object, matching the framework.
The native reduction writes a separate output and a current-stream copy updates
the input; this avoids assuming native in-place alias support. Broadcast also
mutates and returns its input, with `src` interpreted as a group-local root.
AllGather concatenates along the requested dimension. ReduceScatter matches
`DeviceCommunicatorBase`'s `movedim(0, dim)` before scatter and after scatter,
including the observable 3-D behavior in vLLM 0.18.0.

Non-contiguous supported inputs are materialized on the NPU. Native input,
output and intermediate allocations are recorded on the current NPU stream;
ctypes launches do not automatically register their allocator use. A group lock
orders host enqueue, and stream events order successive group operations across
execution streams because the native communicator shares workspace. Ordinary
operations do not synchronize the device. Destruction synchronizes the owning
device before native memory is freed, is idempotent after success, and preserves
the handle if native destruction fails. Close groups consistently on all ranks.

Supported dtypes are int8, int32, int64, fp16, fp32 and bf16 subject to the native
operation/hardware contract. Inputs must satisfy the operation's shape and
rank-participation requirements. Unsupported dtype, non-NPU or empty input
uses the original framework operation before any TileXR launch. AllToAll stays
with the original framework path: native AllToAll requires `TOPO_910_93`, which
is not qualified by the 910B TP8 model environment. Fusion paths are untouched.

## Error and route contract

Explicit bootstrap and group initialization errors propagate. Python normally
ignores ordinary `sitecustomize` exceptions, so explicit opt-in bootstrap failure
uses `SystemExit` to prevent silent backend substitution. Environment inspection
and standalone shim tools run with the framework opt-in disabled. A submitted native operation error propagates
and marks its adapter failed; it must never replay through HCCL. Switching
collective implementations after one rank has submitted work can mismatch rank
participation and hang. Later calls on a failed or closed adapter also fail.
The patch is idempotent and records per-operation `tilexr`, `fallback` and
`error` counters, along with the last exception for diagnostics. These counts
are rank-local; summing eight ranks does not represent global invocation count.

Library and Python module selection must identify the actual deployed artifacts.
The standalone Torch wrapper is distinct from the framework adapter; its smoke
coverage alone cannot qualify group ownership or framework mutation semantics.

## Verification boundary and reusable lessons

Focused unit tests cover unique-ID ABI, startup error propagation/readiness,
domain allocation, independent ownership, routing and destruction failure.
`tests/collectives/integration/vllm_group_npu.py` verifies overlapping TP8 groups,
nonzero-global-root TP4 subgroups, creation/destruction reuse, fp16/bf16 values,
non-contiguous/3-D tensors and alternating execution streams on a qualified node.
Reuse the retained official HCCL environment qualification; do not rerun it for
an integration code change.

The retained deployment evidence additionally compares a real Qwen3.5-4B TP8
model with the original backend across multiple prompts and generation calls.
Model evidence must show all required ranks and route deltas after engine-ready,
not only startup counters. Diagnostic route file writes are not a performance
baseline. Matching token output validates the exercised inference requests,
not every model, dtype, topology, fusion path, graph mode or deployment workload.
