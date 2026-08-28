# EP Combine Memory Commercial-Baseline Migration Design

## Status

- Date: 2026-08-23
- Status: Approved for implementation
- Invocation path: embedded AICore binary launched through the existing direct Kernel ABI
- Target: CANN 9.1, A5 / `__NPU_ARCH__ == 3510`

## Objective

Replace the implementation in `src/ep/kernels/tilexr_ep_combine_memory_kernel.h`
with the active Combine flow migrated from the latest commercial A5 MTE implementation:

`D:/3_codex/ops-transformer-master/mc2/moe_distribute_combine_v2/op_kernel/arch35/moe_distribute_combine_v2_a5_mte.h`

The commercial implementation is the control-flow, UB-queue, data-movement, flag-polling, accumulation, and synchronization baseline. The existing TileXR Combine implementation is used only for the public ABI, TileXR communication-window adaptation, and the currently supported MXFP8 helper.

## Scope

The migrated active header preserves:

- the existing `RunCombine(...)` logical interface;
- the 19-field Kernel argument order and widths;
- FP16 and BF16 inputs and outputs;
- unquantized, MXFP8 E5M2, and MXFP8 E4M3 communication modes;
- token active mask support;
- shared expert slots and optional `sharedExpertX`;
- optional FP32 expert scales.

The following commercial features are intentionally excluded because the current ABI cannot express them or the current Host rejects them:

- expert mask;
- zero, copy, and constant experts;
- elastic scaling;
- performance DFX;
- TP/reduce-scatter;
- AddRmsNorm.

The migration replaces the active header in place and keeps the existing Kernel entry,
binary name, CMake target, and Host launch ABI. Source tests and target-hardware
validation cover the migrated implementation; no parallel legacy header is retained.

## ABI Contract

The eventual entry remains `tilexr_ep_combine_memory_kernel` with the existing arguments:

1. `commArgsGM`
2. `expertOutGM`
3. `assistInfoForCombineGM`
4. `sendCountsGM`
5. `expertScalesGM`
6. `xActiveMaskGM`
7. `sharedExpertXGM`
8. `yOutGM`
9. `bs`
10. `h`
11. `topK`
12. `moeExpertNum`
13. `sharedExpertNum`
14. `sharedExpertRankNum`
15. `globalBs`
16. `activeMaskType`
17. `quantMode`
18. `dtype`
19. `magic`

`quantMode` remains a runtime ABI field. `RunCombine` dispatches it to compile-time Kernel specializations so the sender and receiver hot loops use `if constexpr` in the same manner as the commercial baseline.

## Communication Adaptation Boundary

TileXR owns rank discovery, peer mappings, and round selection. The Kernel derives addresses as follows:

```text
roundDataBase(rank) = TileXREpMemoryRoundDataBase(peerMems[rank], magic)
combineBase(rank)   = roundDataBase(rank) + kEpMemoryStateWindowBytes
slotAddress         = combineBase(rank) + slotIndex * packedRowBytes
```

The `magic % PING_PONG_SIZE` rule in `ep_memory_window.h` is authoritative. The Kernel must not add commercial half-window ping-pong state.

The Kernel does not perform round reuse waiting or retirement. Its production contract does not support standalone back-to-back multi-round combine calls; the caller must ensure that the selected communication window is not still in use.

The following commercial communication-state operations are removed rather than adapted:

- `InitWinState`;
- `dataState_` and `winDataSizeOffsetEp_`;
- `winStatusOffset_`;
- `selfDataStatusGMTensor_[1]` progress writes;
- HCCL context/window lookup.

## Commercial Flow Mapping

| Commercial flow | TileXR implementation |
| --- | --- |
| `InitTilingAttrs` / `InitAttrs` | derive the same active attributes from Kernel arguments and `CommArgs` |
| `InitCommContext` | `TileXRCombineMemoryContext` for rank, peer mapping, and round address selection |
| `CalcDispatchBufferNum` / `BuffInit` | preserve UB-address-based sender buffer calculation, capped at 8 |
| `SplitCoreCal` | preserve contiguous per-core assist-row partition |
| `SetWaitTpStatusAndDisPatch` | preserve stage barrier and dispatch call; no TP state |
| `ExpertAlltoAllDispatchCopyAdd` | batch-copy all assist rows assigned to a core, then use commercial permutation order |
| `ExpertAlltoAllDispatchInnerCopyAdd` | preserve packed `480 B payload + 32 B flag` construction and GM write |
| `AlltoAllCommBuffInit` | preserve active receive buffers and commercial 1/2-buffer selection |
| `TokenMaskCalCnt` | preserve commercial active-token-count flow |
| `CheckPackedFlagRangeArrive` | preserve batch flag DMA, compare, and scalar mask scan |
| `CheckPackedTokenArriveBatch` / `WaitDispatch` | always use the commercial no-expert-mask fast path |
| `ClearPackedTokenFlags` | preserve per-slot strided flag clear |
| `ExpertScaleCopy` | batch-copy the full per-core scale range once |
| `ProcessMoeExpert` | preserve Cast/Muls/Add and barrier ordering |
| `ProcessSharedExpertsLoop` / `AddSharedExpertX` | preserve commercial ordering and scale semantics |
| `LocalWindowCopy` | preserve poll/process/clear/cast/copy ordering; remove commercial progress DFX writes |
| `Process` | preserve send, reset, receive, and local-copy order without extra round retirement |

## UB Planning

The sender allocates fixed metadata and quantization buffers first, then measures the remaining UB between `calBeginBuf_` and `calEndBuf_`. The sender input/output queue count is `remaining / perDispatchBufferBytes`, clamped to `[1, 8]`, matching the commercial algorithm.

The receiver calculates whether the commercial active buffer set fits with two `moeMainSumQueue_` buffers. It uses `TileXREp::kEpMemoryFullUbBytes` as the available Kernel UB budget and falls back to one receive buffer when the two-buffer estimate exceeds that budget.

Unsupported-feature buffers are not allocated. In particular, the active receiver does not allocate constant/copy-expert queues, expert-mask buffers, elastic buffers, performance buffers, or AddRmsNorm buffers.

## Hot-Path Invariants

- Assist metadata is copied once per core, not once per row.
- Sender queue count is dynamic and no greater than 8.
- The unquantized sender does not clear the complete input UB before copying a row.
- There is no per-row rank/token/topK guard that silently drops a payload after dispatch has produced assist metadata.
- A token's complete flag range is checked with one strided GM-to-UB transfer.
- Flag polling does not add timeouts, DCCI, per-slot fallback checks, or diagnostic GM writes.
- Expert scales are copied once per core, not once per token.
- The commercial `Cast`, `Muls`, `Add`, queue, event, and barrier ordering is retained.
- Flags are cleared only after the token payloads have been consumed.
- No internal half-window state or per-token communication-state write is present.

## Validation Boundary

This implementation step performs only source inspection and available local/static checks. It does not claim Kernel compilation or hardware correctness.

Before a later production switch, validation must include target-toolchain compilation and the established 1P, 8P, 32P, and 64P ladder. The primary hardware case is:

```text
64P / BS=96 / H=7168 / K=6 / 384 experts / BF16 / quantMode=0
```

Remote source deployment must use Mutagen `one-way-safe`. Any NPU process means the device is busy; retry every 5 seconds and stop after a bounded 120-second wait.
