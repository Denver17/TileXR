# TileRelay: bind channel declarations to the compiled peer layout

## Status and scope

The user approved fixing the disconnected Channel contract on 2026-10-21.
This written design, including the manifest compatibility boundary, was approved
on 2026-10-26. It narrows the original Core design's
unimplemented general channel-allocation promise to the fixed layout actually
supported by the two current operators.

Today the operator planner declares Channel(0, tiles), generators independently
index publication/ACK flags, and native validation checks only declarations.
Replacing a Plan's channel with Channel(100, 1) passes validation without changing
its artifact or launch slots. Disjoint declarations do not establish disjoint
device accesses. This is not evidence that existing matched builds compute
incorrect results.

## One immutable layout

- Introduce a small compiler-free, immutable fixed peer-memory layout value in
  Python contracts. It describes the tile-slot count and the canonical owner/peer
  flag argument slots. Publication starts at zero, ACK at the fixed second
  1 MiB plane, slots are 64 bytes, there are two participants and one credit.
  Plane offsets are protocol constants, not user-selectable layouts.
- The Python operator layer derives this value from validated specialization:
  AllGather-GEMM needs rows/128 slots; GEMM-AllReduce needs
  (rows/128)*(n/256). Operator flag argument positions remain unchanged.
- Record the layout in KernelArtifact, include it in serialization and identity,
  and preserve the same value from precompile description through final artifact.
  Do not maintain independently writable copies inside the artifact.
- Both planner and generator consume that layout. The planner derives Channel
  and flag bindings from it. The generator uses its count/offsets for bounds,
  flag buffer shapes and publication/ACK indexing; it must not reconstruct an
  independent communication layout. Operator math and scheduling stay unchanged.
- Device DMA primitives may continue accepting resolved addresses and explicit
  scratch buffers. No Python object, interpreter or dynamic channel descriptor
  is passed to the device.

## Validation and supported boundary

- Operator planners/generator entrypoints verify that the artifact layout matches
  their specialization and argument-slot convention. Shape mathematics remains
  in the Python operator layer.
- Generic RelayPlan validation compares its channels and owner/peer flag bindings
  against the artifact's recorded layout, without dispatching on operator ID or
  importing operator modules. Reject changed counts, starts, extra channels,
  missing declarations, and shifted/swapped flag bindings before native Create.
  This applies to direct construction, dataclass replacement and JSON loading.
- Artifacts without flag communication (such as ABI echo) may have no layout;
  such plans must not declare channels or owner/peer flag bindings.
- This Python contract supports no caller-selected nonzero first slot or multiple
  channels yet. Future multi-channel composition needs a separately designed
  allocator/lowering contract, not relaxed validation of unused declarations.
- C++ descriptors, validation, registration, launch, public ABI-v1 and ELF naming
  remain unchanged. Low-level native callers still own the correspondence of
  their descriptors to their compiled kernel; this is not ELF verification.
- Build/install metadata remains trusted. Consistently rewriting an artifact and
  its Plan is not authenticated or checked against machine code. No runtime ELF
  rehash, native operator enum, fallback, compatibility shim or new dependency.

## Manifest compatibility

New Python artifacts serialize the fixed communication layout explicitly.
Existing communication manifests lacking it are rejected with regeneration
guidance, not inferred silently from operator names or unverified Plan fields.
Regenerate them using the updated builder; the device ABI remains version 1.
No-communication artifacts remain valid without a layout. Update examples and
hardware probes for the new manifest metadata while preserving their behavior.
Use a separate artifact/install directory for validation; do not overwrite or
delete historical artifacts, baseline branches or user installations.

## Verification and delivery

Extend existing Python contract tests for the reported Channel(100, 1) case,
count/multi-channel and flag-binding mismatches, missing layout, and JSON
round trips. Keep successful numerical behavior in existing hardware E2Es.
Do not add source-text enforcement tests or a second arithmetic test framework.

Run Host CI; compile both operators with the pinned CANN 9.1/910B toolchain and
verify artifact/Plan round trips. On idle blue devices 0/1, use the existing ABI
echo and mixed-operator tests for one-shot/prepared bindings and prepared
execution, including precision, epoch reuse and teardown. Compare old/new
generated source or ELF where useful, without treating textual equality as
device correctness. Retain profiling/correctness checks and reuse the existing
HCCL qualification record. Record evidence and limits in the maintained guides.
No scheduling optimization or new performance claim is part of this fix.
