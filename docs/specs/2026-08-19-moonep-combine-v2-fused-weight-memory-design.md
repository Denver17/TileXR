# MoonEP Combine V2 Hidden/Weight 单 Launch 双通信域设计

状态：fused-weight 基线已实现并完成阶段正确性验证；第 11、13 节定义的批量 weight
数据面已于 2026-08-21 完成代码实现、Host/source 门禁和 CANN 9.1 Ascend950 目标编译，
实机正确性与性能验证仍受环境基线阻塞。

## 1. 状态与最终目标

本文档定义 MoonEP Combine V2 在一次 AICore Kernel launch、一套 step loop 内同时完成
hidden 和 route weight 传输的方案。

最终数据面如下：

| 数据 | 通信域 | 传输语义 | 完成依据 |
| --- | --- | --- | --- |
| hidden payload | Shared-QP UDMA communicator，domain 0 | UDMA WQE/SQ/CQ | 现有 ordered Done 和 CQ |
| route weight | 独立 Memory-only communicator，建议 domain 1 | `peerMems[]` 上的 MTE copy | 新增 `weightDone` token |

核心决策：

- 保留 hidden 的现有 UDMA schedule、WQE、SQ/CQ 和 Reduce 逻辑；
- route weight 不构造 UDMA WQE，不占用 hidden 的 SQ/CQ；
- hidden 和 weight 共用同一份 `dstLocal` 路由和同一个 step schedule；
- remote/fullmesh step 在最后一批 hidden payload WQE 和 ordered Done WQE 已写入 SQ，完成
  MTE3，并敲完对应 DB 后立即发送 weight，不等待该 Done 的 CQ；
- self step 没有 WQE、Done WQE、DB 或 CQ，以最后一笔 SelfCopy MTE3 完成且两个
  `selfCopyQueue_` buffer 均归还作为 weight 启动边界；
- 接收端必须同时观察到 hidden Done 和 weight Done，之后才能消费本轮输入；
- `routeWeightsNvs == nullptr && routeWeightsSk == nullptr` 时保持纯 hidden 路径；
- 对外 Python `Buffer`、`MoonEPCommPlan`、方法参数、关键字参数和返回 tuple 不变；
- 保留现有 C ABI，新增融合入口，不直接改变已有 `TileXRMoonEpCombineStageV2` 的参数列表。

本文档同时记录已实现基线和下一阶段性能优化。实现过程中如需改变通信域所有权、完成协议、
公共接口、record ABI 或输出语义，必须先更新设计。

## 2. 当前行为与问题

历史实现通过第二次 `h = 1` Combine V2 launch 传输 route weight。当前 fused-weight 基线
已经把 weight 融合到 hidden Kernel 内，并通过独立 Memory communicator 完成：

```text
Send hidden -> ring hidden DB
SendWeightMemoryStep()
Wait hidden CQ
Wait inbound hiddenDone + weightDone
CopyReceivedWeights()
```

通信域和完成协议已经符合本文档，但 Kernel 的 weight payload 和输出处理仍是逐元素路径。
当前 `SendWeightMemoryStep()` 对每个选中元素执行：

```text
4B GM -> UB
MTE2 -> Scalar hard sync
Scalar SetValue 构造 16B record
Scalar -> MTE3 hard sync
16B UB -> remote GM
MTE3 -> Scalar hard sync
```

当前 `CopyReceivedWeights()` 也对每个输出元素执行一次 16B GM -> UB、Scalar magic 检查和
4B UB -> GM，并在每个元素上等待 MTE2/MTE3。该实现虽然正确，但把固定开销放大到
`O(validWeightCount + weightOutputElements)` 次硬同步。

2026-08-21 Terbo 64P、`BS=8192`、`K=16`、`H=3584`、Ring、关闭 reduce 的同二进制 A/B
中，fused-weight 关闭中位数为 3.523897 ms，开启为 27.602538 ms，即 7.832958 倍。该结果
只能证明 fused weight 总路径存在显著开销；逐阶段占比仍需 profile 验证。但代码结构已经足以
确认逐元素 MTE 和硬同步必须移出热循环。

本轮优化保持既有 Memory window、双 epoch、16B record 和 weightDone ABI，只重写 Kernel
内的 weight 搬运、压缩、record 构造、MTE3 提交和输出解包逻辑。

## 3. 目标与非目标

### 3.1 目标

- route weight 存在时，Combine V2 只启动一次 AICore Kernel；
- hidden 和 weight 在同一 step loop 内推进；
- hidden 继续使用当前 UDMA fast path；
- weight 使用独立 Memory domain 的 `CommArgs::peerMems[]`；
- weight 传输与 hidden CQ latency 重叠；
- weight 不改变 hidden 的 WQE 数、CQ target 和 QP 分配；
- 保持 route weight 当前输入、输出 shape、dtype 和下标语义；
- 支持连续多轮调用，以 generation tag 隔离旧记录，不读取上一轮残留数据；
- 对 Memory、UDMA 和 Fullmesh 分别给出可观测的失败状态和 profile 数据。

### 3.2 非目标

- 不改变 `dstLocal` 的编码格式；
- 不改变 hidden 的 Reduce 算法或精度；
- 不将 weight 合并进 hidden row，也不为 weight 注册 UDMA memory；
- 不用 scalar/direct-GM store 代替 Memory 数据面的 MTE copy；
- 不在本设计中重构 Combine V2 的整体 schedule、Grant/Credit 或 FullSync 协议；
- 不以 Host/模拟器测试宣称跨机 Memory 数据面已验证；
- 不在缺少 Memory mapping 时静默退回第二次 weight launch。

## 4. 总体数据流

每个 active worker 在一个 step 中执行：

```text
Select current hidden peer
        |
        v
Build/submit hidden UDMA WQEs
final batch appends hidden Done
ring UDMA doorbell
        |
        +-----------------------+
        | UDMA progresses       |
        | asynchronously        |
        +-----------------------+
        |
        v
SendWeightMemoryStep(peer, step)
dstLocal/routeWeightsNvs chunk -> UB dual GatherMask
-> packedWeight + packedDstSlot
-> batched UB records -> remote weightRecord[target]
        |
        v
MTE3 completion/barrier
publish remote weightDone(magic, step)
        |
        v
Wait hidden CQ and existing admission protocol
```

所有 step 完成后，接收侧执行：

```text
WaitInboundHiddenDone()
            AND
WaitInboundWeightDone()
             |
             v
BatchCopyReceivedWeights(routeWeightsSk)
ReduceHidden()
```

优化版在 hidden 最终 doorbell 后完整发送当前 step 的 weight，再进入该 step 的最终 CQ
polling。UDMA 已独立推进，weight MTE3 与 hidden CQ latency 可以重叠。Fullmesh 为避免 SQ
耗尽而在 hidden 发送内部执行的中间 CQ 回收必须保留；“不等待 Done CQ”只指最终 ordered
Done 对应的 CQ。只有 profile 证明 CQ polling 饥饿或单个 weight step 过长时，才进一步改为：

```text
poll one CQ -> send one weight batch -> poll one CQ -> ...
```

## 5. Host 和公共接口

### 5.1 C ABI

保留以下已有符号及参数列表：

```cpp
TileXRMoonEpCombineV2(...);
TileXRMoonEpCombineStageV2(...);
```

新增融合入口，名称可在实现时按现有版本规范最终确定，建议：

```cpp
int TileXRMoonEpCombineStageV2Fused(
    void *registeredWorkspace,
    uint64_t registeredWorkspaceBytes,
    const int32_t *dstLocal,
    TileXRCommPtr udmaComm,
    TileXRCommPtr weightMemoryComm,
    int64_t bs,
    int64_t h,
    int64_t topK,
    int64_t nvS,
    uint32_t aivCoreNum,
    const void *hiddenNvsh,
    void *hiddenSh,
    const float *routeWeightsNvs,
    float *routeWeightsSk,
    uint32_t dtype,
    aclrtStream stream);
```

约束：

- 两个 weight 指针必须同时为空或同时非空；
- weight 非空时 `weightMemoryComm` 必须非空；
- weight 为空时允许 `weightMemoryComm == nullptr`，Kernel 完全跳过 weight 路径；
- `udmaComm` 和 `weightMemoryComm` 必须具有相同的 global/local rank 映射；
- `weightMemoryComm` 必须与 `udmaComm` 是不同实例和不同 domain；
- Host 继续校验 `bs * topK <= nvS`、BF16 hidden、core 数和 registered workspace；
- Host 不再把 weight 拷进 registered workspace，也不再启动 FP32 `h = 1` 的第二次
  Combine V2 Kernel。

旧入口可以继续保持当前两次 launch 行为，作为 ABI 兼容路径。MoonEP Torch Runtime 在
完成双 communicator 初始化后切换到融合入口。

### 5.2 Python Runtime

Runtime 增加内部成员：

```text
_comm                 Shared-QP UDMA communicator, domain 0
_weight_memory_comm   Memory-only communicator, domain 1
```

初始化和销毁必须遵循：

- 所有 rank 以相同顺序创建两个 communicator；
- domain ID 在所有 rank 上一致；
- 任一 communicator 初始化失败时，销毁已经成功创建的实例；
- Runtime shutdown 时分别销毁两个 communicator；
- Python 用户可见的 `comm_ptr`、plan、方法签名和返回值保持兼容；
- 是否启用融合路径由 Runtime 内部能力检查决定，不要求调用方新增参数。

## 6. Memory-only Communicator

当前 `TileXRCommInitRankWithDomain()` 创建普通非 shared-QP communicator，但仍受进程级
`TILEXR_ENABLE_UDMA` 控制并执行 `InitUDMA()`。直接用它创建第二个 domain 可能重复初始化
UDMA/RA，增加资源占用并产生 ownership 冲突。

因此新增显式 Memory-only 初始化能力，例如：

```cpp
int TileXRCommInitRankMemoryDomain(
    int commDomain, int rankSize, int rank, TileXRCommPtr *comm);
```

该 communicator：

- 初始化 bootstrap、socket exchange、本地 peer memory 和远端 mapping；
- 填充 `CommArgs::peerMems[]`、rank 和 topology 字段；
- 不初始化 UDMA context、UDMA QP、UDMA registry 或 Fullmesh view；
- 默认不初始化本设计不需要的 SDMA 数据面；
- 继续使用独立 domain 隔离 peer memory 名称、socket/bootstrap 和生命周期；
- 失败时不能影响已经工作的主 UDMA communicator。

实现应使用 communicator 实例级 capability/options，而不是临时修改进程级环境变量。

## 7. Weight Memory Window 布局

Combine V2 weight 区域位于每个 Memory communicator peer window 的数据区：

```text
peerBase(peer) = memoryArgs->peerMems[peer] + TileXR::IPC_DATA_OFFSET
```

固化布局如下，所有 offset 相对 `peerBase`：

```text
weightRecordEpochBytes = AlignUp(nvS * 16, 64)
weightRecordBytes = 2 * weightRecordEpochBytes
weightRecordOffset = 0
weightDoneOffset = weightRecordBytes
weightDoneEpochBytes = rankSize * 64
weightDoneBytes = 2 * weightDoneEpochBytes

totalWeightBytes = AlignUp(weightDoneOffset + weightDoneBytes, 64)
```

逻辑结构：

```text
weightRecord[2][nvS]           16-byte generation-tagged record
weightDone[2][rankSize]        64-byte token slot
```

每个 weight record 的 ABI 为：

```cpp
struct alignas(16) MoonEpCombineV2WeightRecord {
    float value;
    uint32_t reserved;
    uint64_t magic;
};
```

Host 必须以 checked arithmetic 计算布局，并验证：

- `totalWeightBytes <= IPC_BUFF_MAX_SIZE`；
- 所有 offset 和乘法无溢出；
- record 区与 done 区不重叠；
- weight 区不进入 `IPC_DATA_OFFSET` 之前的 flag 保留区；
- `peerMems[rank]` 以及本轮需要的所有 `peerMems[peer]` 均非空。

本设计不把 weight 区加入 registered UDMA workspace，因此现有
`TileXRMoonEpCombineGetWorkspaceSizeV2()` 的 hidden workspace 大小不因 weight 增长。

### 7.1 Generation 隔离

Credit 迁移后，step 0 前不存在可用于“接收端清零完成”的跨 rank 初始化屏障，因此不清理
weight receive 区。仅使用一份 generation-tagged record 仍不安全：快 rank 完成 generation N
后可先进入 N+1，并覆盖慢 rank 尚未复制的 N 数据；完整 magic 只能发现覆盖，不能防止合法
数据丢失。

record 和 done 区因此都按 `epoch = magic & 1` 分为两份。发送端把 `value`、`reserved=0` 和
当前完整 `magic` 作为一个 16 Byte record 通过同一次 MTE3 写入；接收端只在
`record.magic == current magic` 时输出 `record.value`，否则输出零。两份 epoch 足够，因为
rank 若要从 N+1 继续进入 N+2，必须先收到所有 source 的 N+1 Done；仍在复制 N 的慢 rank
尚未进入 N+1，不可能发布该 Done。因此任一 rank 最多领先一代，不会在慢 rank 消费 N 时
复用 N 的物理 epoch。该方案保证：

- `dstLocal == -1` 或本轮未覆盖的 target 不会暴露旧 weight；
- 旧 record 即使物理保留，也会因 magic 不匹配而失效；
- 相邻 generation 写入不同物理 epoch，不会覆盖仍在消费的数据；
- step 0 不依赖额外 receiver-ready token 或初始化屏障；
- `weightDone` 仍用于证明本 source 本 step 的所有 record 写入已经完成。

## 8. Route 映射和唯一所有者

weight 与 hidden 共享 `dstLocal[source]`：

```text
encoded = dstLocal[source]

encoded == -1:
    skip

peer   = encoded / nvS
target = encoded - peer * nvS
```

对当前 step 的 `peer`，复用现有 8192-entry selection chunk、上下界 compare 和 packed
mask。weight 路径不再逐项读取 `selectedIndexBuf`，而是对同一 mask 分别压缩 destination
和 weight：

```text
LoadSelectionChunk()
Compare dstSlot in [peerBase, peerEnd)
GatherMask<int32_t> -> packedDstSlot
Load routeWeightsNvs[chunkStart : chunkEnd] -> rawWeight
GatherMask<float> -> packedWeight
```

两个 dense tensor 的第 `i` 项必须来自同一个 source：

```text
packedWeight[i]  = routeWeightsNvs[source]
packedDstSlot[i] = dstLocal[source] - peer * nvS
```

它们是“每个 selection chunk 内的密排结果”，不是覆盖整个 peer 的常驻 tensor。当前 chunk
完成 record 构造和 MTE3 提交后即可被下一 chunk 覆盖。不能只保留 FP32 weight 和
`packedDstSlot` 就直接声称满足远端 ABI；发送前仍必须构造
`{value, reserved=0, magic}` 16B record，详见第 11 节。

必须保持以下唯一所有者约束：

- 一个有效 source 在一次 Combine 调用中只由一个 sender core 发送；
- 同一 `(destination rank, target)` 本轮只有一个有效 writer；
- 发布 `weightDone[destination][source rank]` 的 core 是该 source/destination pair 的唯一
  schedule owner；
- self、Fullmesh 和 CLOS 只改变 hidden path，不改变 weight 的 owner 推导。

Host schedule 单测需要穷举所有支持的 rank size，证明每个有效 pair 的 sender core、step 和
receiver 期望完全一致。

`dstLocal` 对同一 destination 的 target 唯一性是 Planner/route contract 的前置条件。批量
MTE3 不新增去重或冲突消解；若输入包含重复 target，hidden 与 weight 都存在多 writer，结果
未定义。Host/Planner 测试必须覆盖该约束，Kernel 热路径不为此增加逐项检查。

## 9. Kernel ABI 与状态

`CombineV2Params` 和 launch context 增加：

```text
weightMemoryComm
weightMemoryHostArgs
weightMemoryDevArgs
routeWeightsNvs
routeWeightsSk
weight layout offsets/bytes
hasRouteWeight
```

Kernel 参数在现有参数之后追加，保持原有参数的顺序和含义：

```text
memoryCommArgs
routeWeightsNvs
routeWeightsSk
weightRecordOffset
weightDoneOffset
weightWindowBytes
weightOutputElements
hasRouteWeight
```

Host `CombineV2KernelArgs`、静态大小断言、注册 Kernel signature 和 AICore 入口必须同步
修改。现有 hidden-only入口也按新 Kernel ABI 填充尾部字段，weight 关闭时传空指针和
`hasRouteWeight = 0`。

Kernel 新增主要状态：

```text
weightMemoryArgs_
localWeightWindow_
weightRecordOffset_
weightDoneOffset_
weightWindowBytes_
routeWeightsNvsAddr_
routeWeightsSkAddr_
hasRouteWeight_
```

所有rank size统一使用`MoonEpCombineV2` Kernel和同一Host ABI、token/layout定义；128P只在
统一StepRoute中切换Group映射，不能分叉weight数据面。

## 10. Step 内执行协议

### 10.1 CLOS remote step

```text
SendRemoteStep(peer, step)
  -> hidden payload WQEs
  -> ordered hidden Done WQE
  -> CopyIssueToSq + MTE3 completion
  -> update SQ head/count
  -> st_dev both lane doorbells

SendWeightMemoryStep(peer, step)
  -> acquire both reusable 64 KiB Self relay buffers
  -> process selection/weight chunks and batched record scatter
  -> wait all weight MTE3 complete
  -> PublishWeightDone(peer, step)

WaitStepCqs(step)
Wait existing Grant/Credit/admission state
```

`SendRemoteStep()` 成功返回是 weight 的启动条件。它必须代表最后一批 WQE 的 SQ MTE3 已
完成并且两个 lane 的 DB 均已敲完，但不代表 ordered Done CQ 已返回。不能把
`WaitStepCqs()` 前移到 weight 之前。remote 当前每 lane 只为最终 batch 请求 completion，
weight 阶段不改变其 `cqTarget/submittedHead`。

### 10.2 Fullmesh step

Fullmesh hidden 已有独立 QP/CQ。顺序调整为：

```text
SendFullmeshStep(peer, step)
SendWeightMemoryStep(peer, step)
PublishWeightDone(peer, step)
WaitFullmeshCq(step, peer)
existing control handling
```

weight 始终通过 Memory communicator，不因为 hidden peer 在同 server 而切换到 UDMA
Fullmesh。`SendFullmeshStep()` 成功返回同样要求最终 Done WQE 的 SQ 发布和 DB 已完成。
Fullmesh 在大量 payload 下可能为控制 SQ outstanding 而等待中间 completion；这些内部 CQ
回收不能删除，只有最终 Done CQ 延后到 weight 之后。

### 10.3 Self step

self hidden 保留本地 scratch copy。self 没有 hidden WQE、Done WQE、DB 或 CQ，因此不能把
remote 的“DB 后启动”条件机械套用到 self。启动边界固定为：

```text
SendSelfStep(rank, step)
-> final SelfCopy MTE3 completed
-> self relay ping/pong both idle
-> SendWeightMemoryStep(rank, step)
```

self weight 仍使用：

```text
memoryArgs->peerMems[rank] + IPC_DATA_OFFSET
```

执行本地 Memory-window MTE copy并发布本地 `weightDone`，从而保持与 remote 完全一致的
generation、输出和完成协议。self 不构造 UDMA WQE，也不等待 hidden CQ。若 SelfCopy 提前
返回但 queue 中仍有 pending tensor 或 MTE3 尚未完成，weight 复用 UB 会产生覆盖，必须视为
实现错误。

### 10.4 空 payload

即使某个 peer 在当前 step 没有被选择的 hidden/weight row：

- hidden 保持当前空 payload 仍发送 ordered Done 的规则；
- weight 不执行 payload MTE3，但仍发布 `weightDone(magic, step)`；
- 接收端因此可以区分“本轮没有 weight”与“发送端未完成”。

## 11. Weight MTE 数据搬运

### 11.1 UB 所有权与固定布局

当前 AIV UB 总量是 216 KiB（221,184 B），`InitBuffers()` 固定分配 207,360 B，未分配
13,824 B。其中 Self relay 占两个 64 KiB buffer，共 131,072 B。remote/fullmesh step 中它们
完全闲置，self step 返回时也已经释放，因此 weight 必须完整复用这 128 KiB，不增加
`kSendBufferBytes`，也不依赖剩余 13,824 B 扩大常驻 tensor。

当前 `selfCopyQueue_` 的两个 64 KiB buffer 是两个 queue slot，不能假设为一个可随意切片的
连续 128 KiB tensor，也不能在 step 热路径调用 `TPipe::Reset()` 后重新建 buffer；Reset 会
同时破坏 selection 和 WQE buffer。实现时应在 `InitBuffers()` 将该物理预算显式建模为两个
固定 64 KiB `TBuf`（例如 `selfRelayPingBuf_`、`selfRelayPongBuf_`），SelfCopy 用事件维护
ping/pong，weight 直接取得两个固定 tensor。该重构只改变 UB 所有权表达，不改变 SelfCopy
的数据流或总 UB 大小。

每个 8192-entry chunk 的布局固定如下，所有 offset 保持 32B 对齐：

| 64 KiB buffer | 区域 | 大小 | 用途 |
| --- | --- | ---: | --- |
| relay ping | `rawWeight` | 32,768 B | 8192 个原始 FP32 weight |
| relay ping | `packedWeight` | 32,768 B | 最多 8192 个密排 FP32 weight |
| relay pong | `packedDstSlot` | 32,768 B | 最多 8192 个 peer-local `uint32_t` target |
| relay pong | `recordStage[0]` | 4,096 B | 128 条、UB stride 32B 的 record |
| relay pong | `recordStage[1]` | 4,096 B | 第二个 record staging，用于 ping/pong |
| relay pong | `doneScratch` | 64 B | payload 完成后构造 weightDone |
| relay pong | 保留 | 24,512 B | 不纳入首版功能，可用于后续调优 |

远端 record ABI 仍是 16B；record staging 使用 32B local stride，是因为 CANN 9.1
`DataCopyPad` Normal 模式会把每个非 32B block 的 UB 源步长向上对齐。MTE3 只发送每条
record 的前 16B，后 16B 是本地 padding，绝不能写到远端相邻 slot。

### 11.2 每 chunk 双 GatherMask

weight selection 按以下顺序执行，避免在空 peer 上无条件搬入完整 weight chunk：

1. 将 `dstLocal[chunkStart : chunkStart + chunkElements]` 整段搬入现有 `dstSlotBuf_`；
2. 复用现有 `GE(peerBase)`、`LT(peerEnd)` 和 mask `And` 得到本 peer 位图；
3. 先以该位图执行 `GatherMask<int32_t>`，得到密排 encoded destination 和
   `selectedCount`；
4. `selectedCount == 0` 时直接进入下一 chunk；
5. 将 `routeWeightsNvs[chunkStart : chunkStart + chunkElements]` 以一次连续 MTE2 搬入
   `rawWeight`；
6. 对同一位图执行 `GatherMask<float>`，得到 `packedWeight`；
7. 将密排 encoded destination 减去 `peerBase`，得到 peer-local `packedDstSlot`；
8. 以 chunk 内 `selectedCount` 为界进入 record batch loop。

CANN 9.1/Ascend 950 的 `GatherMask` 自定义 Tensor 模式支持 FP32 和 INT32，但 mask tensor
类型必须与 32-bit 数据匹配为 `uint32_t`。现有 hidden 索引 gather 是 INT16，使用
`lowerMask.ReinterpretCast<uint16_t>()`；weight 和 destination gather 必须改用同一 1024B
位图的 `ReinterpretCast<uint32_t>()`，不能直接复制现有 INT16 调用。推荐参数为：

```cpp
GatherMask(packedDstEncoded, dstSlot,
    lowerMask.ReinterpretCast<uint32_t>(), true, chunkElements,
    {1U, 1U, 0U, 0U}, dstCount);

GatherMask(packedWeight, rawWeight,
    lowerMask.ReinterpretCast<uint32_t>(), true, chunkElements,
    {1U, 1U, 0U, 0U}, weightCount);
```

两次调用必须各自从 0 初始化 count，并在一次 Vector-to-Scalar 边界后验证
`dstCount == weightCount == selectedCount`。自定义 Tensor 模式不占用固定 pattern 模式所需
的额外 8 KiB reserved UB。tail chunk 只把实际 `chunkElements` 作为 Counter mask，位图中
tail 后的旧 bit 不得产生输出。

### 11.3 批量 record 构造

仅有 `packedWeight` 和 `packedDstSlot` 还不能发送，因为远端 ABI 是 16B
`MoonEpCombineV2WeightRecord`。每批最多 128 条，与现有 hidden payload batch 对齐。使用
现有 128-thread SIMT 模式并行构造 staging record，而不是 AICore Scalar 逐项
`GetValue/SetValue`：

```text
recordStage[batchPingPong][thread * 32 + 0]  = packedWeight[batch + thread]
recordStage[batchPingPong][thread * 32 + 4]  = 0            // reserved
recordStage[batchPingPong][thread * 32 + 8]  = magic        // uint64
recordStage[batchPingPong][thread * 32 + 16] = local padding, not sent
```

最后一个不足 128 条的 batch 只由 `thread < batchCount` 的线程写入。SIMT 完成后建立
Vector/SIMT-to-MTE3 依赖，MTE3 才能读取该 stage。`packedDstSlot` 不写入 record，它只用于
计算远端 GM 地址。

### 11.4 批量 MTE3 scatter 的准确含义

`packedDstSlot` 的顺序由 source 顺序决定，通常不是连续目标。普通 `DataCopyPad` 只有固定
stride，不能用一次调用表达任意 scatter。因此本设计中的“批量 MTE3”定义为：

- 一批并行构造最多 128 条 record；
- 对相邻且 `dstSlot[i + 1] == dstSlot[i] + 1` 的最大连续 run，使用一次
  `DataCopyPad(blockCount=runLength, blockLen=16, srcStride=0, dstStride=0)`；Normal 模式
  按 32B local stride 读取 staging，并向远端连续写 16B record；
- 对离散 target 提交单条 `DataCopyPad(blockCount=1, blockLen=16)`；可以连续提交多条，
  但不在每条后执行 `MTE3_S`；
- SIMT build 完成后以 `V_MTE3` 建立 record-to-MTE3 依赖；每个 staging buffer
  的整批命令提交后设置一个独立的 `MTE3_S` 完成事件，只有再次由 Scalar/SIMT 覆写该
  staging buffer 前才等待该事件；
- 最后一个 payload batch 后等待两个 staging buffer 的 MTE3 全部完成，然后才能复用
  `doneScratch` 并发布 weightDone。

连续 run 合并是正确性无关的 fast path；完全离散时最坏仍有 `selectedCount` 次 MTE3 指令
提交，但硬同步从“每 record 一次”降为“每 128 records 至多一次 stage 回收”。不能把离散
scatter 描述为一次连续 MTE3，也不能为了合并 run 对 target 排序，因为排序必须同步重排
weight，且额外排序成本和 UB 需求尚无证据支持。

双 staging 的流水状态如下：

```text
build stage 0 -> V_MTE3 -> submit batch 0 -> arm MTE3_S event 0
build stage 1 -> V_MTE3 -> submit batch 1 -> arm MTE3_S event 1
wait event 0 -> overwrite stage 0 -> submit batch 2 -> ...
wait all armed events
build/send weightDone
```

首版不在每个 weight batch 间轮询 hidden CQ。remote 最终只有既有 completion target；
Fullmesh 为 SQ 容量执行的中间 CQ 回收已发生在 `SendFullmeshStep()` 内。若 profile 显示
单个 weight step 足以造成 CQ progress 饥饿，再在 batch 边界做公平 polling，但不能等待
最终 Done CQ 后才继续 weight。

### 11.5 完整发送伪代码

```text
SendWeightMemoryStep(peer, step):
    acquire fixed relay ping/pong tensors
    for each selection chunk:
        load dstLocal chunk
        build peer mask
        gather packed destination; read selectedCount once
        if selectedCount == 0: continue

        load complete FP32 weight chunk
        gather packed weight with the same mask
        convert packed destination to peer-local dstSlot

        for batchStart in [0, selectedCount), step 128:
            wait only if the selected record stage is still owned by MTE3
            SIMT build padded 32B-stride records
            submit contiguous runs or discrete 16B MTE3 copies
            arm stage completion event

    wait all armed payload MTE3 events
    publish 64B weightDone(magic, step)
    wait done MTE3 completion
    release relay ping/pong ownership
```

必须保持以下边界：

- 不使用 scalar/direct-GM store 写 peer memory；
- 不把 16B record 扩大成远端 32B 写，避免覆盖相邻 slot；
- `selectedCount == 0` 的所有 chunk 不发送 payload，但 step 末仍发送 weightDone；
- `chunkStart + chunkElements`、source offset、target offset 和 record offset 使用 checked
  64-bit arithmetic；
- record 的 `magic` 使用本轮完整 `magic_`，不是 epoch bit 或 step token；
- 任何 Gather count 不一致、target 越界或 event ownership 异常都必须在 safety build 中失败，
  不能发布 weightDone；
- 统一Kernel的所有schedule mode必须使用同一batch size、UB layout、record ABI和完成顺序。

## 12. Weight Done 完成协议

现有 hidden final batch 已经追加 ordered Done WQE。它只能证明 UDMA hidden 路径完成，不能
证明独立 Memory 路径完成，因为两个通信域之间不存在天然顺序。

weight token slot：

```text
index  = sourceRank
offset = weightDoneOffset + epoch * weightDoneEpochBytes + index * 64
value  = MoonEpCombineV2Token(magic, step)
```

发送端：

```text
all weight payload MTE3 submitted
-> MTE3 completion/barrier
-> UB token -> MTE3 -> remote weightDone slot
-> wait token MTE3 complete before reusing UB
```

接收端根据现有 schedule 计算每个 source 的 expected receive step，并同时等待：

```text
hiddenDone[epoch][source][required lane]
weightDone[source]
```

只有两类条件全部满足，才能进入 Reduce/output 阶段。推荐将其实现为统一的
`WaitInboundReady()` 公平轮询，分别保留 hidden Done 与 weight Done 的 pending mask、观察
值、timeout detail 和 profile metric。

weight 关闭时，weight pending mask 初始为空，行为退化为现有 hidden Done 等待。

## 13. 输出处理

hidden 输出保持现有流程：

```text
WaitInboundReady()
-> ReduceHidden()
-> registered workspace output
-> Host D2D copy to hiddenSh
```

weight 不执行 Reduce。接收端在 `weightDone` 全部满足后，读取本地 record 区的：

```text
weightRecordOffset + epoch * weightRecordEpochBytes + [0, bs * topK) * 16
```

若 record magic 等于当前 magic，则复制 value 到 `routeWeightsSk`；否则写零。该长度与当前
Stage V2 从 active scratch 拷出的长度一致。

发送端批量化后不能保留当前逐 record 的 `CopyReceivedWeights()`，否则 16B MTE2、Scalar
magic 检查、4B MTE3 和逐元素硬同步仍会主导 fused-weight 开销。输出路径也复用两个 Self
relay buffer，并由 active cores 按连续元素区间分片：

1. 每次连续 MTE2 搬入最多 2048 条 16B record，共 32,768 B；
2. 使用现有 1024B mask buffer 依次构造“每 4 个 uint32 取第 0/2/3 项”的 pattern，即
   `0x11111111`、`0x44444444`、`0x88888888` 重复位图；
3. 对 record words 执行三次自定义 Tensor `GatherMask<uint32_t>`，分别得到密排 value、
   `magicLo` 和 `magicHi`；value tensor 可 reinterpret 为 FP32；
4. 分别比较 `magicLo/magicHi` 与当前完整 `magic_` 的低/高 32 bit，AND 两份 compare mask；
5. 使用最终 mask 原位选择有效 value，对无效 record 写 0；
6. 以一次连续 MTE3 写出该 tile 的 FP32 `routeWeightsSk`，只在 tile 边界同步和复用 UB。

选择 2048 records 是因为其 8192 个 uint32 source word 对应 1024B 自定义位图，正好复用
现有 selection mask buffer；它不是新的常驻 UB 分配。实现也可使用等价的 SIMT 批量解包，
但不得回退到 AICore Scalar 每 record `GetValue/SetValue`。固定 pattern 形式的 `GatherMask`
在 Ascend 950 上额外占用 8 KiB reserved UB，因此首选显式构造并复用现有 mask 的自定义
Tensor 模式。

最后一个 output tile 只处理实际 `tileElements`，不能让 padding record 产生输出。Kernel
返回前必须等待最后一笔 `routeWeightsSk` MTE3 完成，使同一 stream 的后续任务可以按正常
stream 依赖消费输出。输出完成后再进入现有卡内最终汇合和 hidden Reduce 流程。

## 14. 失败、超时和可观测性

Host 在 launch 前拒绝：

- 两个 communicator rank/world/local 映射不一致；
- weight 启用但 Memory communicator 或 `peerMems[]` 不完整；
- weight layout 超出 peer memory data capacity；
- weight 指针只提供一个；
- `bs * topK > nvS` 或 size arithmetic 溢出；
- Memory-only capability 未启用。

Kernel 增加可区分的失败信息：

```text
WEIGHT_MEMORY_INVALID_CONFIG
WEIGHT_DONE_TIMEOUT
```

沿用 operation timeout 上限，但失败 detail 至少记录：

```text
step, peer/source, core, epoch, expected token, observed token
```

Profile 增加：

```text
weight destination selection/gather cycles
weight full-chunk MTE2 + FP32 gather cycles
weight record SIMT build cycles
weight MTE3 submit cycles
weight payload drain cycles
weightDone publish point
weight inbound wait cycles
weight output record-load/unpack/store cycles
```

同时记录每 step 的 `selected records`、`record batches`、`contiguous runs`、`MTE3 commands`
和 `stage waits`。这些计数用于区分“目标确实离散”与“run 合并失效”，并验证硬同步次数从
逐 record 变为按 chunk/batch。现有 hidden WQE/CQ metric 必须保持可比，以验证 weight
没有进入 UDMA SQ/CQ。

## 15. 兼容与迁移

融合基线的 Host、双 communicator、window、完成协议和 Runtime 迁移已经实施。批量 weight
优化按以下顺序进行：

1. 把 Self relay queue 重构为两个固定 64 KiB buffer，先证明 SelfCopy 行为不变；
2. 实现 destination/weight 双 GatherMask 和 count/tail 检查；
3. 实现双 record staging、SIMT 构造和离散/连续 run MTE3 提交；
4. 保持 payload-drain -> weightDone 的现有完成协议；
5. 将接收输出改为连续 record tile load 和批量 magic/value 解包；
6. 增加分段 profile 后执行同口径正确性和性能 A/B。

不允许在融合路径初始化失败时无提示地退回两次 launch。调用方必须收到明确 capability 或
初始化错误；如确实需要兼容回退，应由显式配置控制并记录日志。

## 16. 修改范围

| 模块 | 主要职责 |
| --- | --- |
| `src/include/tilexr_api.h` | 声明 Memory-only communicator 初始化接口 |
| `src/comm/comm_wrap.cpp` | 暴露新初始化入口和失败清理 |
| `src/comm/tilexr_comm.h/.cpp` | 增加实例级 capability，跳过第二 domain 的 UDMA/SDMA |
| `src/include/tilexr_moonep_combine_v2.h` | 增加融合 Stage C API，保留旧 ABI |
| `src/moonep/combine_v2/host/combine_v2_host.*` | 双 comm 校验、layout 和 launch context |
| `src/moonep/combine_v2/host/combine_v2_launch.*` | 扩展 registered Kernel 参数 ABI |
| `src/moonep/combine_v2/host/combine_v2_layout.*` | 增加 weight Memory window layout builder |
| `src/moonep/combine_v2/host/tilexr_moonep_combine_v2.cpp` | 融合入口及删除默认路径的第二次 weight launch |
| `src/moonep/combine_v2/kernels/*` | step weight send、token、wait、output 和 profile |
| `integrations/moonep_torch/tilexr_moonep/runtime.py` | 第二 communicator 生命周期及融合入口调用 |
| `tests/`、`tools/moonep/` | Host contract、schedule、Kernel 和硬件端到端验证 |

实施时必须保留工作区中与本设计无关的未提交修改。128P只切换统一Kernel的Group映射，
不能重新引入独立数据面分支。

本轮性能优化的必改范围应收敛到统一Kernel、weight/profile公共常量及对应测试；
不改变 Host launch ABI、Memory communicator、window layout、Python Runtime 或公共 API。

## 17. 验证策略

### 17.1 Host 和纯逻辑测试

- weight layout 的正常、边界、overflow 和 capacity case；
- 两个 communicator rank/local-rank 映射不一致时拒绝 launch；
- weight 指针同时为空、同时非空和单边为空；
- 所有支持 rank size 的 pair -> step -> core 唯一性穷举；
- token index、magic 和 expected receive step 一致；
- Kernel argument struct、signature 和参数顺序静态检查；
- Python API 参数和返回 tuple 与 `tools/moonep/test_npu_e2e.py` 保持兼容。

### 17.2 Kernel 编译与局部验证

- CANN 9.1 target-toolchain 编译 Generic 及实际启用的 Group variant；
- 编译确认 FP32/INT32 `GatherMask` 使用 `uint32_t` mask view，count 和 tail 语义正确；
- 编译确认两个 Self relay 固定 buffer 的总 UB 仍为 207,360 B，剩余仍为 13,824 B；
- source guard 禁止 weight send/output 热循环出现逐 record `MTE2_S`、`MTE3_S`，并禁止
  Scalar `GetValue/SetValue` 搬运 weight、构造 record 或解包 output；scatter planner 为形成
  离散 GM 地址读取 `packedDstSlot` 是唯一允许的逐项 Scalar 数据访问；
- 单测覆盖 0、1、127、128、129、8191、8192 个 selected entries 的 batch/tail 切分；
- 单测覆盖全部连续、全部离散、多个连续 run 和 chunk 边界的 MTE3 plan；
- 检查 Normal 模式 32B local staging stride 只向远端写 16B record；
- 验证 weight 关闭时不访问 Memory comm 和 weight 指针；
- 验证空 payload 仍发布 weight Done；
- 连续多轮调用覆盖多个 magic generation，检测残留值。

### 17.3 实机正确性

在匹配设备和拓扑上，先运行对应模式的官方 HCCL Test 作为环境基线，再验证：

- 无 route weight：结果与当前纯 hidden V2 一致；
- 有 route weight：hidden 和 weight 同时与 PyTorch/CPU reference 一致；
- `dstLocal == -1`、非连续 target、空 peer 和 self peer；
- 连续 target、反序 target、跨 8192-entry chunk 和 128-entry batch tail；
- CLOS、Fullmesh 和跨节点 Memory peer mapping；
- 连续压力调用、generation 复用和 timeout/error 注入；
- Runtime trace 证明一次 combine 调用只有一次 Combine V2 Kernel launch；
- UDMA WQE/CQ 计数与纯 hidden 路径一致，weight 没有进入 UDMA 队列。

Host、模拟器或 910B fallback 结果不能证明跨节点 Memory 数据面。跨节点可达性和 MTE
顺序必须在目标 A5/Ascend950 环境上单独给出证据。

### 17.4 性能验收

至少比较相同代码、环境、shape 和 mapping 上的：

```text
weight off：纯 hidden 基线
weight on before：当前逐元素 fused-weight 基线
weight on after：批量 Gather/record/MTE3/output 优化
```

记录：

- 端到端 combine latency；
- Kernel launch 数和 Host enqueue 时间；
- 每 step hidden send、weight send、CQ wait、inbound wait；
- UDMA SQ/CQ 数量；
- selected record 数、MTE3 command/run 数、batch/stage wait 数；
- output record tile 数及 load/unpack/store 分段耗时。

沿用 2026-08-21 Terbo 64P A/B 作为首个回归点：相同口径下优化后的 weight-on 中位数必须
低于 27.602538 ms，且差异必须大于三轮运行噪声；同时 weight-off 不得出现统计显著回退。
具体性能目标在取得首轮分段 profile 后再固化，不能用减少源码循环数替代端到端证据。

若仍慢，按 `destination gather -> weight gather -> record build -> MTE3 submit/drain -> inbound
wait -> output unpack` 的首个主导阶段继续优化。只有 CQ wait 指标证明 progress 饥饿时才引入
CQ/weight batch 交替，不改变 DB 启动边界和 token 协议。

## 18. 验收条件

设计实现完成需要同时满足：

- route weight 存在时，Runtime trace 中每次 Combine 只有一次 V2 Kernel launch；
- hidden 和 weight 使用同一 `magic`、step schedule；hidden 控制区继续使用 epoch，weight
  record/done 使用双 epoch 物理区，record 仍校验完整 magic；
- weight 只访问 Memory communicator 的 `peerMems[]`，不新增 UDMA WQE/CQ；
- remote/fullmesh weight 在最终 hidden DB 后、最终 Done CQ 前启动；self 在 SelfCopy 两个
  buffer 均 idle 后启动；
- 常驻 UB 仍为 207,360 B，两个 64 KiB Self relay 同时服务 SelfCopy 和 weight，不在 step
  内 Reset/ReInit `TPipe`；
- weight selection 产生一一对应的 `packedWeight` 和 peer-local `packedDstSlot`，FP32/INT32
  gather 均使用 `uint32_t` mask view；
- 16B record 由 batch SIMT 构造，离散 MTE3 只在 batch/stage 边界同步，远端不发生 32B
  扩大写；
- hidden Done 早到时接收端不会提前读取 weight；
- weight Done 早到时接收端仍等待 hidden Done；
- self、空 payload、连续 generation 复用和 `dstLocal == -1` 输出正确；
- 旧公共 C ABI 和 MoonEP Python API 契约保持兼容；
- Memory-only communicator 不重复初始化 UDMA/RA；
- 目标硬件上的正确性和性能证据已记录，验证声明不超出实际硬件范围。

## 19. 已确认实施决策与边界

1. Credit 协议不提供 step 0 清零屏障；weight record/done 采用双 epoch，record 使用完整
   magic 过滤同 epoch 的旧数据，接收区不清零；
2. CANN 9.1 Kernel 以完整 FP32 chunk MTE2、FP32/INT32 双 GatherMask、16B record MTE3、
   64B done token 和连续 FP32 output tile 为基本单位；跨流水线依赖显式同步，但禁止逐
   record 硬同步；
3. 所有规模走统一Kernel；128P使用Group映射，其余规模使用Single Ring映射；
4. 实机验证必须先通过匹配拓扑的 HCCL `all_reduce_test -a aiv_only`，再运行 fused-weight
   probe；
5. Host/编译验证不能替代跨节点 Memory mapping、MTE 顺序和连续 generation 的实机证据；
6. 两个逻辑 dense tensor 是每 chunk 的 `packedWeight` 和 peer-local `packedDstSlot`；16B
   `recordStage` 是不可省略的传输 staging，不属于第三个路由语义 tensor；
7. 任意离散 target 不能由普通 `DataCopyPad` 一次 scatter；只合并连续递增 run，其余记录
   连续提交 MTE3 并在 stage 边界统一等待；
8. 发送端优化与接收端批量解包必须一起落地，否则现有逐 record output 路径仍会保留
   `O(weightOutputElements)` 次硬同步。

## 20. 2026-08-20 阶段验证证据

目标环境为 2 号柜 Ascend950DT、CANN 9.1.0。验证使用 `bs=128`、`K=16`、`H=3584`、
1 次 warmup 和 3 次计时 launch：

- pure AICore Kernel、Host 库和硬件 probe 通过目标工具链编译；
- 29/29 Host、layout、schedule、public ABI 和 source-guard 测试通过；
- 8/32/64P 分别先通过 HCCL `all_reduce_test -a aiv_only`；
- 双 epoch 修复后的 8/32/64P fused probe 分别有 8/32/64 个 rank 报告
  `correctness=passed` 和 `weight_correctness=passed`；
- probe 使用不同的 generation 0/1 weight 输入，分别校验首轮输出和连续
  `1 warmup + 3 timed` 序列的末轮输出；该证据不表示每个中间 generation 都被单独读回。

远端日志：

```text
/home/h00580772/tilexr_validation/cab2_fused_weight/logs/hccl_aiv_8p_20260820_015904.log
/home/h00580772/tilexr_validation/cab2_fused_weight/logs/hccl_aiv_32p_20260820_015949.log
/home/h00580772/tilexr_validation/cab2_fused_weight/logs/hccl_aiv_64p_20260820_020046.log
/home/h00580772/tilexr_validation/cab2_fused_weight/logs/combine_fused_weight_8p_20260820_015924.log
/home/h00580772/tilexr_validation/cab2_fused_weight/logs/combine_fused_weight_32p_20260820_020011.log
/home/h00580772/tilexr_validation/cab2_fused_weight/logs/combine_fused_weight_64p_20260820_020110.log
```

本轮证明了 Generic Kernel 在单机 Fullmesh、4 节点和 8 节点路径上的 hidden/weight
正确性及跨节点 Memory 可达性。它没有覆盖 Group/128P、`dstLocal == -1`、错误注入、
Runtime trace、旧双 launch 性能基线或 8P 大 per-peer Fullmesh CQ 回收边界，不能对这些
维度作通过声明。

## 21. 2026-08-21 优化设计的漏洞与歧义闭合

| 问题或歧义 | 最终设计结论 | 实现/验证防线 |
| --- | --- | --- |
| “DB 后启动”是否包含 CQ | remote 两 lane/fullmesh 最终 Done WQE 已完成 SQ MTE3并敲 DB 即可；不等最终 CQ | 保持 `SendHidden -> SendWeight -> WaitFinalCq` 调用顺序 |
| Fullmesh 中间 CQ 是否也删除 | 不删除；它用于回收 SQ outstanding，不是最终 Done wait | 大 per-peer payload 覆盖 SQ 边界测试 |
| self 没有 DB，何时复用 UB | 最后一笔 SelfCopy MTE3 完成且 ping/pong 都 idle 后 | SelfCopy 单测和 event ownership 断言 |
| 两个 Self queue slot 是否是一块 128 KiB tensor | 当前不是；重构成两个显式 64 KiB fixed buffer 后分别使用 | UB compile-time size/offset assert；禁止 step 内 `TPipe::Reset()` |
| 是否能保留 hidden 的全部 selection | `nvS > 8192` 时不能；DB 后按 chunk 重新扫描。单 chunk 复用只能作为后续可证明的 fast path | profile 独立统计 selection rescan |
| mask 能否直接复用 | 位图字节可复用；INT16 gather 用 `uint16_t` view，FP32/INT32 必须用 `uint32_t` view | CANN 9.1统一Kernel target compile gate |
| 两个 dense tensor 的范围 | 都是当前 chunk 内密排；相同 mask 保证第 `i` 项来自同一 source | 两次 count 相等检查和 reference test |
| `packedDstSlot` 保存什么 | 保存 peer-local target `[0, nvS)`，不是 encoded global destination，也不是 source index | 构造后范围检查；地址计算单测 |
| 两个 dense tensor 能否直接发送 | 不能；现有远端 ABI 要求 16B `{value,reserved,magic}` record | 128-thread SIMT batch record builder |
| “批量 MTE3”是否是一条任意 scatter | 不是；连续 run 可单指令，离散 target 是多条命令批量提交、stage 级等待 | 记录 run/command/stage-wait 数 |
| 16B record 在 UB 中是否也密排 16B | Normal `DataCopyPad` staging 按 32B local stride，远端仍只写 16B | 检查生成参数和相邻 slot canary |
| 双 staging 是否允许覆盖 | 只有对应 `MTE3_S` 完成事件返回后才可由 Scalar/SIMT 覆写 | 每 stage 显式 `free/inflight` 状态 |
| 两次 `FetchEventID(MTE3_S)` 是否得到独立事件 | 不会；`FetchEventID` 只查询首个空闲 ID，不占用。双 stage 必须 `AllocEventID`，全部 drain 后 `ReleaseEventID` | CANN 9.1 `TPipe` 实现核对、source guard 和目标编译 |
| SIMT record builder 后能否使用 `PipeBarrier<PIPE_S>` | 不能；Ascend950/CANN 9.1 对该组合有编译期 static assertion。SIMT VF 属于 `PIPE_V`，VF 参数按值传递，返回后直接建立 `V_MTE3` 依赖；禁止用 `PIPE_ALL` 把另一个 stage 的 MTE3 一并串行化 | Ascend950 目标编译；最终依赖正确性仍需实机 canary |
| payload 与 weightDone 的顺序 | 所有 payload stage 全部 drain 后才提交 64B weightDone | 空/非空 batch、跨 chunk、tail 测试 |
| 只优化发送是否足够 | 不足；当前接收输出同样逐 record 硬同步，必须同时批量解包 | output 分段 profile 和 source guard |
| 128 条连续 MTE3 提交是否必然适合硬件 | 尚需目标工具链和 Ascend950 实机确认；若 pipe/指令限制更小，只缩小 sub-batch，不恢复逐 record wait | 1/127/128/129、全离散压力测试 |
| duplicate target 如何处理 | 依赖 Planner 的单 writer contract，不在 Kernel 热路径排序或原子化 | Planner/Host 唯一性测试；非法输入行为不定义 |

以上结论中只有 CANN 9.1 接口签名、数据类型组合、mask 类型、Counter 语义和
`DataCopyPad` stride/长度范围已经由版本匹配文档与头文件确认；SIMT record builder 的生成、
128-command batch 的最优大小、连续 run 的实际收益和 CQ 是否需要 batch 间 polling，仍是
目标 Ascend950 编译与 profile 验证项，不能在实现前写成已证明的性能结论。

## 22. 2026-08-21 批量实现与验证边界

已实现范围：

- Generic 和 Group 源码均将 Self queue 重构为两个固定 64 KiB `TBuf`，总常驻 UB 仍为
  207,360 B，剩余 13,824 B；
- weight 发送按 8192-entry chunk 执行 INT32 destination 与 FP32 weight 双 GatherMask，
  128-thread SIMT 构造 32B local-stride record，并用两个 4 KiB stage 提交连续 run 或离散
  16B MTE3；
- 两个 stage 使用独立分配的 `MTE3_S` event，全部 payload drain 后才复用 done scratch 并
  发布 weightDone；
- SIMT VF 的 `magic/batchStart/batchCount` 按值传递，避免两个异步 stage 争用共享 context；
  VF 返回后使用 `V_MTE3` 建立 record stage 到 MTE3 的流水依赖；
- weight 输出按最多 2048 条 record 连续 MTE2，通过 `0x11111111/0x44444444/0x88888888`
  三种自定义 mask 解包 value 和完整 magic，无效值批量置零后连续 MTE3 输出；
- profile v7 增加 8 个 weight 分段耗时、每 step 的 records/batches/merged-runs/
  MTE3-commands/stage-waits 计数和 output tile 计数。

已通过：

- 远端 `scripts/validate_combine_v2_host.sh`：6 个 Host/source 测试全部通过；
- 纯逻辑测试覆盖 0/1/127/128/129/8191/8192 batch tail、连续/离散/混合 run 和
  destination/weight 同源密排；
- 当时CANN 9.1、`CATLASS_ARCH=3510`、Ascend950的server-pair profiling Kernel/benchmark、
  独立Group profiling Kernel和Generic non-profiling Kernel目标编译通过；独立Group Kernel
  后续因负优化删除，该历史结果不验证当前统一128P Group映射路径；
- 编译产物包含独立 `MoonEpCombineV2BuildWeightRecordsVf_simt_entry`。

未通过/未执行：

- 32P 官方 HCCL `all_reduce_test -a aiv_only` 在 `device_init` 返回 `507033`，日志为
  `hccl_aiv_allreduce_32p_o0_20260821_152107.log`，且没有 per-rank `success`；
- 依据通信环境基线约束，未继续运行 TileXR fused-weight 正确性或性能用例；因此 MTE3
  stage 事件顺序、16B 相邻 slot canary、连续 generation 和端到端性能收益尚不能声明通过。
