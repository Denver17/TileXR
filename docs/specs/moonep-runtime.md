# MoonEP Runtime and Planner Specification

## Scope

The optional MoonEP component provides a five-stage runtime:

1. Planning
2. Dispatch
3. PrefetchWeight
4. Combine
5. ReduceGrad

The native C ABI is declared in `src/include/tilexr_moonep.h`, Planner V2/V3 in
`tilexr_moonep_planner.h` and `tilexr_ep_plan.h`, and the supported Python
facade in `integrations/moonep_torch/tilexr_moonep/`. The contract consumed by
`tools/moonep/test_npu_e2e.py` is compatibility-critical.

## Ownership and Lifetime

The caller owns tensors, ACL streams, communicators, and workspace allocations.
Native Host entries validate arguments and launch embedded pure AICore binaries
through `rtDevBinaryRegister`, `rtFunctionRegister`, and
`rtKernelLaunchWithFlagV2`. Host `kernel<<<...>>>` wrappers are not supported.

A saved plan and its backing storage remain alive through Dispatch,
PrefetchWeight, Combine, and both backward branches. Asynchronous events guard
the same lifetime. Zero-copy results retain the producer storage until the
consumer and recorded event complete.

## Public ABI

`TileXRMoonEpGetAbiVersion` and capability queries describe the installed
runtime. V1 reports Planning, Dispatch, PrefetchWeight, and Combine as native and
ReduceGrad as a compatibility stub. V2 reports all five stages as native; its
ReduceGrad entry uses the query/prepare/execute/destroy lifecycle. Stage-specific
V1/V2 structures preserve tensor dtype, shape, pointer, workspace, plan, and
stream semantics. New versions do not reinterpret existing structures in place.

The Python API preserves the `Buffer` and `MoonEPCommPlan` names, method
signatures and keyword arguments, return tuple arity/order, exposed plan fields,
async-event behavior, and zero-copy contracts used by the NPU E2E tool.

## Planner Contracts

The installed planner library exposes three separate supported contracts:

- Native Planner V2 exports `TileXRMoonEpPlanner*V2` and produces `dst`,
  `cuSeqlens`, `expertsToCopy`, `remoteStats`, and planner status.
- Optimized Planner V2 exports `TileXRMoeEpPlanV2*` and produces either
  `TileXRMoonEPPlanDesc` or the count-bearing `TileXRMoonEPPlanMetadataV2`
  declared in `tilexr_ep_plan.h`.
- Planner V3 is used by the native five-stage path and additionally publishes
  the reverse `dstLocal` mapping required by Combine V2.

The forward route maps each `(token, topk)` to an expert rank and receive slot.
Counts, cumulative offsets, remote-expert metadata, target masks, global token
identity, and padding sentinels share one plan generation. Combine's reverse map
is built only after every rank has published the corresponding forward route.

For optimized Planner V2, `rawDst = dstRank * nvS + recvSlot`. A negative
`encodedDst` is a duplicate route and decodes as `rawDst = ~encodedDst`; it still
owns a receive slot and route-weight/remap entry even though it suppresses a
duplicate hidden send. Dispatch, not Planner, writes `tokenRemap[nvS]` from the
global-token helpers, and Combine consumes that mapping. Metadata counts are
element counts, and partial planner status remains distinguishable from both
success and hard configuration errors.

Planner metadata communication uses the peer-memory communicator window. It is
not ordinary UDMA registration and does not register `peerMems[]`.

## Peer Protocol

All ranks agree on dimensions, dtype, top-k, expert partition, plan version, and
workspace layout before a stage consumes peer data. Reusable shared state is
tagged with a new communicator magic/generation rather than reset to zero.

Memory mappings support same-node and cross-node traffic. Operators may use UDMA
only for explicitly registered ordinary device workspaces and only after the
capability and registration contract is ready.

## Build and Installation

`TILEXR_BUILD_MOONEP_PLANNER` builds the planner libraries without the complete
five-stage runtime. `TILEXR_BUILD_MOONEP` builds the planner plus the MoonEP ABI
and native stage libraries. `TILEXR_BUILD_EP` controls the separate standalone
EP component under `src/ep`. Each MoonEP stage owns its Host target, kernel
binary, registration signature, and installation rules. Installed libraries
must not carry toolkit `devlib` in RPATH/RUNPATH; runtime HAL resolves from the
driver.

## Failure Handling

- Shape, dtype, rank, plan-version, layout, and workspace errors fail before
  launch.
- Planner status is propagated to downstream stages; invalid plans are not
  partially consumed.
- Timeout is a bounded diagnostic outcome, not a substitute for identifying the
  first failed owner/state boundary.
- A stale installed binary or mismatched source identity invalidates runtime
  conclusions until provenance is corrected.

## Validation Boundary

API, Host, Python, and source tests cover structures, layouts, error propagation,
facade compatibility, and reference planning. NPU validation proceeds from
single-stage tests through five-stage correctness, model replay, and model flow.
Profiler and stage barriers remain enabled for model/replay performance
comparison. Build and hardware commands live in `docs/BUILD_VERIFICATION.md`;
the evidence-driven debugging workflow lives in
`docs/moonep/MINDSPEED_DEBUGGING_EXPERIENCE.md`.
