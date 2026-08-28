# MoonEP Combine V2 可插拔 Step 映射框架设计

状态：映射框架已实施；当前策略为128P使用Group映射，其余规模使用Single Ring

日期：2026-08-20

## 1. 背景

当前 `MoonEpCombineV2` 已经支持 Single Ring、Bidirectional Ring 和
Server-Pair 等多种发送序，但 Kernel 在不同位置分别计算发送目标、接收来源、Credit
等待来源和 Credit 发布目标：

- `Process()` 使用 `MoonEpCombineV2Peer()` 计算当前 step 的发送目标；
- `WaitInboundDone()` 使用 `MoonEpCombineV2ReceiveSource()` 计算接收来源；
- `WaitStepCredit()` 再次计算发送目标和远端 receiver core；
- `PublishNextCredit()` 计算下一 step 的来源 rank 和对应 sender core。

这些计算在语义上属于同一个 step 路由，但分散在 Kernel 状态机中。修改发送序时必须同时
理解和修改多处正向、反向映射，容易出现数据目标正确但 Done 或 Credit 所有者错误的问题。

本设计为统一的 `MoonEpCombineV2` 建立可插拔映射框架。128P只迁移既有Group发送/接收
公式，由统一Kernel继续执行普通数据面、Done、CQ和Credit协议；独立Group Kernel已删除。

## 2. 目标

1. Kernel 只根据 `schedule mode/rank/core/step/rankSize` 一次计算当前 step 的完整路由。
2. 每种通信算法只需要定义两个核心映射：`SendDst()` 和 `RecvSrc()`。
3. Done 和 Credit 所需的反向 core 映射由通用框架从上述两个映射唯一反解。
4. 保留 Single Ring、Bidirectional Ring 和当前 Server-Pair 模式，允许快速切换或增加发送序。
5. 第一阶段只重构映射计算，不改变任何已启用路径的数据发送、Done、CQ 或 Credit 行为。
6. 保持公共 API、Kernel launch ABI、workspace、profile 和 QP 配置不变。

## 3. 非目标

本设计第一阶段明确不包含：

- 不启用、合并或重构 `MoonEpCombineV2Group` Kernel；
- 不迁移 Group 的并行 Done 轮询、提前发布 Credit 或其他 step 状态机；
- 不引入公共 `RunStep()`，也不让两个 Kernel 共享数据面执行代码；
- 不改变当前 `kCombineV2ScheduleMode` 的取值或选择方式；
- 不改变 payload 的 QP 分配、WQE 顺序、batch 边界或 completion flag；
- 不改变 Self、Fullmesh、CLOS 的选择条件和实现；
- 不改变 Done 数量、Done 所在 QP、Done WQE flag 或 Done 等待规则；
- 不改变 outbound CQ、inbound Done 和 Credit 的现有先后顺序；
- 不改变 Credit 的 `creditMems + MTE3/MTE2` 无 CQ 传输方式；
- 不进行 FullSync、phase barrier 或其他已删除协议的恢复；
- 不在本阶段加入 Group 128P 映射模式。

当前实际运行的普通 `MoonEpCombineV2` 跨 Server 已经在两个数据 QP 上分别追加一个
`ORDERED_COMPLETION` Done，接收端等待两个 Done。本设计保持该行为不变。

## 4. 核心抽象

### 4.1 算法必须提供的两个映射

每种 schedule mode 只对以下两个纯函数负责：

```cpp
uint32_t MoonEpCombineV2MapSendDst(
    MoonEpCombineV2ScheduleMode mode,
    uint32_t sourceRank, uint32_t senderCore,
    uint32_t step, uint32_t rankSize);

uint32_t MoonEpCombineV2MapRecvSrc(
    MoonEpCombineV2ScheduleMode mode,
    uint32_t destinationRank, uint32_t receiverCore,
    uint32_t step, uint32_t rankSize);
```

语义如下：

```text
SendDst(mode, R, C, S) = rank R/core C 在 step S 的数据发送目标
RecvSrc(mode, R, C, S) = rank R/core C 在 step S 管理的入站数据来源
```

输入只包含 schedule、当前 rank/core/step 和 rankSize。映射函数不得访问 Kernel 队列状态、
workspace、QP、CQ、payload 或运行时完成状态。

`SendDst()` 必须返回规范化后的真实目标 rank。旧 schedule 中的 invalid peer 继续按当前
语义映射为 self，但该转换收敛在映射层，执行层不再重复解释 invalid peer。

### 4.2 通用反向映射与现有模式快速路径

Credit 需要知道接收方 owner core 和下一发送方 sender core。框架提供从两个基础映射
反解的统一入口：

```cpp
uint32_t MoonEpCombineV2FindRecvCore(
    mode, destinationRank, sourceRank, step, rankSize);

uint32_t MoonEpCombineV2FindSendCore(
    mode, sourceRank, destinationRank, step, rankSize);
```

定义为：

```text
FindRecvCore(D, S, step) 找唯一 C，使 RecvSrc(D, C, step) == S
FindSendCore(S, D, step) 找唯一 C，使 SendDst(S, C, step) == D
```

找不到或找到多个 core 都视为无效映射。正确性不能依赖 Kernel 中默认关闭的 safety check，
必须由 Host 穷举测试证明唯一性。

新 mode 默认可以通过至多 16 个 active core 的有界枚举完成通用反解。现有四种 mode 的
`MapRecvSrc()` 为保持行为等价，仍适配旧 `ReceiveSource()`，而该 helper 本身会遍历
`rankSize`。如果 `FindRecvCore()` 再逐 core 调用它，会把旧路径的 `O(rankSize)` 成本放大为
`O(16 * rankSize)`。因此现有 mode 在统一入口内部复用经过穷举证明等价的
`ReceiveCore()/TransferCore()` 快速反解；后续新 mode 仍可只实现 `MapSendDst/MapRecvSrc`
并使用通用回退，只有硬件性能证明有必要时才增加等价快速路径。

### 4.3 StepRoute

Kernel 每个 step 只构造一次以下路由：

```cpp
struct MoonEpCombineV2StepRoute {
    uint32_t sendDstRank;
    uint32_t recvSrcRank;
    uint32_t sendSuccessorRank;

    uint32_t creditWaitSrcRank;
    uint32_t creditWaitSrcCore;

    uint32_t creditPublishDstRank;
    uint32_t creditPublishDstCore;

    bool waitCredit;
    bool publishCredit;
    bool valid;
};

MoonEpCombineV2StepRoute MoonEpCombineV2BuildStepRoute(
    MoonEpCombineV2ScheduleMode mode,
    uint32_t rank, uint32_t core,
    uint32_t step, uint32_t rankSize);
```

字段关系为：

```text
sendDstRank = SendDst(rank, core, step)
recvSrcRank = RecvSrc(rank, core, step)
sendSuccessorRank = 唯一 R，使
  SendDst(R, core, NextStep(step)) == sendDstRank

step > 0:
  creditWaitSrcRank = sendDstRank
  creditWaitSrcCore = FindRecvCore(sendDstRank, rank, step)

step + 1 < stepCount:
  creditPublishDstRank = RecvSrc(rank, core, step + 1)
  creditPublishDstCore =
      FindSendCore(creditPublishDstRank, rank, step + 1)
```

`creditWaitSrcRank/core` 表示谁在上一 step 完成入站接收后给当前 sender 放行。
`creditPublishDstRank/core` 表示当前 receiver 完成本 step 入站接收后，需要放行的下一 sender。
`sendSuccessorRank` 只用于 Fullmesh profile；现有 mode 复用旧 successor 公式保持 trace 不变，
新 mode 由 `SendDst()` 通用反解，Kernel 不再直接理解 mode。

## 5. 多种发送序的接入方式

第一阶段保留现有 `MoonEpCombineV2ScheduleMode`：

```text
MOONEP_COMBINE_V2_SINGLE_RING
MOONEP_COMBINE_V2_BIDIRECTIONAL_RING
MOONEP_COMBINE_V2_SERVER_PAIR_SAME_CROSS
MOONEP_COMBINE_V2_SERVER_PAIR_PARITY
```

现有算法先通过适配层生成与旧 helper 完全相同的 `SendDst/RecvSrc`。这一步不重写公式，目标是
建立行为等价基线。

增加新算法时，只需要：

1. 增加一个 schedule mode；
2. 实现该 mode 的 `MapSendDst()`；
3. 实现该 mode 的 `MapRecvSrc()`；
4. 通过通用双射和 Credit 唯一写者测试。

Kernel 的数据发送、Done、CQ、Credit 和 Reduce 代码不需要理解新算法。

为了保持 ABI 和性能边界清晰，第一阶段继续使用 Kernel 内部编译期
`kCombineV2ScheduleMode`。是否增加 Host/runtime schedule 选择属于后续独立设计。

## 6. Kernel 接入方式

只修改普通 `MoonEpCombineV2`。`Process()` 的 step 循环在开头创建 route，并将预计算字段
传给现有 helper：

```text
route = BuildStepRoute(current mode, rank, core, step, rankSize)

WaitStepCredit(step, route)
-> 现有 Self/Fullmesh/SendRemoteStep(route.sendDstRank, step)
-> 现有 outbound CQ 等待
-> WaitInboundDone(step, route.recvSrcRank)
-> PublishNextCredit(step, route)
```

这里不是引入新的 `RunStep()`，也不是重写状态机。`Process()` 保持现有分支和调用顺序，
仅停止在多个 helper 中重复计算映射。

函数签名预期调整为：

```cpp
WaitStepCredit(uint32_t step, const StepRoute &route);
WaitInboundDone(uint32_t step, uint32_t sourceRank);
PublishNextCredit(uint32_t step, const StepRoute &route);
```

这些函数的 signal 格式、offset、MTE、cache、timeout 和 profiling 行为保持不变。

以下数据面函数不在第一阶段修改范围内：

```text
BuildPayloadWqes / SubmitPair / SendRemoteStep
BuildFullmeshPayloadWqes / SubmitFullmeshBatch / SendFullmeshStep
SendSelfStep
WaitStepCqs / WaitFullmeshCq
AppendControlWqe / CopyIssueToSq / CQ poll
```

## 7. 行为冻结合同

第一阶段重构前后必须逐项等价：

| 行为 | 等价要求 |
| --- | --- |
| 发送目标 | 每个 mode/rank/core/step 与旧 `EffectivePeer(Peer())` 相同 |
| 接收来源 | 与旧 `ReceiveSource()` 相同 |
| Credit 等待来源 | rank/core 与旧 `WaitStepCredit()` 计算结果相同 |
| Credit 发布目标 | rank/core 与旧 `PublishNextCredit()` 计算结果相同 |
| payload | 目标、QP、lane、顺序、数量、flag 和 batch 不变 |
| Done | 目标 slot、lane 数、WQE 顺序和 flag 不变 |
| CQ | completion count、target、轮询和回收顺序不变 |
| Credit | signal、slot、MTE3/MTE2 和等待/发布时间不变 |
| step 时序 | Credit wait -> send -> outbound CQ -> inbound Done -> Credit publish 不变 |

任何不能满足此表的修改都不属于映射框架重构，必须单独说明行为差异、收益、风险和验证方式，
经人为决策后再实施。

## 8. Group 参考边界

128P Group只保留以下两个纯映射公式：

```text
GroupSendDst(rank, core, step)
GroupRecvSrc(rank, core, step)
```

`MOONEP_COMBINE_V2_GROUP`只接入`MapSendDst/MapRecvSrc`及其反向映射，由普通
`MoonEpCombineV2`执行现有数据面和step协议。旧Group Kernel的数据面、并行Done轮询、
提前Credit和collective/reduce状态机均不迁移。

## 9. 修改范围

第一阶段预期修改：

- `src/moonep/common/moonep_combine_schedule.h`
  - 增加两个标准映射入口、通用 core 反解、`StepRoute` 和 route builder；
  - 保留现有 schedule helper，先作为适配层的权威实现。
- `src/moonep/combine_v2/kernels/tilexr_moonep_combine_v2_kernel.h`
  - `Process()` 每 step 构造一次 route；
  - Credit/Done helper 改为消费 route 字段，不改变传输和等待实现。
- `tests/moonep_combine_v2/unit/test_combine_v2_schedule.cpp`
  - 增加旧/新映射等价、双射、反向 core 和 Credit 唯一写者测试。
- `tests/moonep_combine_v2/unit/test_combine_v2_source_guard.cpp`
  - 固化数据面函数未被替换、step 调用顺序未变化、Group Kernel 未接入的边界。
不修改 Host API、layout、launch、`CommArgs` 和 Kernel 参数。

## 10. 实施顺序

1. 为现有 mode 建立 `MapSendDst/MapRecvSrc` 适配函数。
2. 增加通用 `FindRecvCore/FindSendCore` 和 `BuildStepRoute`。
3. 先完成 Host 穷举等价测试，证明新 route 与当前行为完全一致。
4. 调整普通 Kernel helper 签名，使其消费 route，不修改数据面函数。
5. 更新 source guard，固定 step 调用顺序和禁止项。
6. 完成 Host build、CANN 9.1 target compile 和2号柜 32P/64P验证。
7. 框架验证完成后，再单独设计和迁移 Group 映射 mode。

## 11. 验证策略

### 11.1 Host 穷举

覆盖当前支持的 rankSize 和全部现有 schedule mode：

```text
rankSize = 2,3,4,5,6,7,8,16,32,64,128
```

检查：

- 新 `SendDst` 等于旧发送目标；
- 新 `RecvSrc` 等于旧接收来源；
- 每个 step 的发送边和接收边完全一致；
- `FindRecvCore/FindSendCore` 结果唯一且可逆；
- `StepRoute` 的 Credit publisher/waiter 字段与旧实现相同；
- 每个 `(epoch, transitionStep, targetRank, targetCore)` 恰好一个 writer；
- Self/invalid-as-self 的行为不变；
- 无效 rank/core/step/mode 返回无效 route。

### 11.2 Source guard 与编译

- `Process()` 仍按原顺序调用 Credit、send、CQ、Done 和 Credit publish；
- 普通 V2 仍使用既有 Self/Fullmesh/CLOS helper；
- payload/Done WQE builder 和 CQ helper 无结构性修改；
- Kernel入口只保留统一`MoonEpCombineV2`实现；
- C++14 Host build 和 CANN 9.1 AICore target compile 通过；
- Kernel ABI 和公共 MoonEP API 不变。

### 11.3 硬件边界

当前只在健康的2号柜完成 32P/64P：

- balanced payload 和 fused weight exact correctness；
- epoch0 -> epoch1 -> epoch0 连续多轮；
- 无新增 Done/Credit/CQ timeout；
- Host 穷举证明相同输入下的发送目标和 step route 与重构前一致；
- 性能无超出自然波动的回退。

skew、sparse 和 empty payload 不在当前硬件 probe 的输入能力内，作为后续扩展验证项记录，
不计入本阶段已通过范围。

本阶段不尝试 HCCL，不执行或宣称 128P Group 数据面验证。

## 12. 风险与完成标准

主要风险是正向发送映射正确但反向 receiver/sender core 不唯一，导致 Done 等待或 Credit
写错 slot。该风险通过基础映射双射合同、通用反解和 Host 全量穷举消除，不能用运行时
timeout 代替静态证明。

第一阶段完成必须同时满足：

1. 所有现有 mode 的新旧 route 穷举等价；
2. 普通 Kernel 每 step 只构造一次 route，后续 helper 不再重复计算 schedule；
3. 数据面、Done、CQ、Credit 和 step 时序没有未经批准的行为差异；
4. 独立Group Kernel数据面未被接入统一Kernel；
5. Host/unit/source-guard 和 CANN target compile 通过；
6. 2号柜 32P/64P 多轮正确性通过，验证结论不外推到 128P。

## 13. 实施与验证结果

2026-08-20 第一阶段已经落地：

- 普通 `MoonEpCombineV2::Process()` 每个 step 构建一次 `StepRoute`；
- `WaitStepCredit()`、`WaitInboundDone()` 和 `PublishNextCredit()` 只消费 route 字段；
- Fullmesh profile successor 也消费 route 字段，普通 Kernel 不再直接调用 mode-specific
  successor helper；
- Self、Fullmesh、CLOS、payload WQE、Done WQE 和 CQ helper 未改动；
- 独立Group Kernel未被普通V2调用，后续已作为重复实现删除；
- Host 穷举覆盖全部现有 mode 及
  `2,3,4,5,6,7,8,16,32,64,128` rankSize，证明发送、接收、反向 core、Credit route 和
  Credit 唯一写者与旧 helper 等价；
- CANN 9.1 AICore 编译通过，Combine V2 schedule/layout/host/launch/public ABI/source guard
  六个测试全部通过。

2号柜最终运行使用 `bs=128`、`topK=16`、`H=3584`、`reduce-hidden`、`fused-weight`、
1 次 warmup 和 10 次连续计时迭代：

| world | experts | avg_ms | max_ms | 结果 |
| --- | ---: | ---: | ---: | --- |
| 32P | 64 | 0.438278 | 0.461473 | payload/weight correctness passed |
| 64P | 128 | 0.668115 | 0.722182 | payload/weight correctness passed |

初版通用反解在 64P 的 3 次迭代样本为 `0.845645 ms`；接入现有 mode 快速反解后，同口径
样本降至 `0.788673 ms`，消除了可定位的嵌套枚举开销。历史旧版本 3 次迭代样本在 32P 为
`0.493259/0.530328 ms`，64P 为 `0.780749/0.753477 ms`。最终结果未观察到超出样本波动的
性能回退，但因最终轮次与历史轮次不同，不据此宣称性能提升。

本轮硬件测试覆盖 balanced 路由、fused weight 和连续 generation；未单独覆盖 skew、sparse
和 empty payload。没有运行 HCCL，也没有运行或外推 128P。完整日志保存在2号柜：

```text
/home/h00580772/tilexr_validation/cab2_mapping_framework/logs/
combine_mapping_32p_20260820_031714.log
combine_mapping_64p_20260820_031749.log
```

## 14. 最终映射选择

本版不引入CMake配置或Host参数，只在统一Kernel初始化时按规模选择一次：

```text
rankSize == 128 -> MOONEP_COMBINE_V2_GROUP
otherwise       -> MOONEP_COMBINE_V2_SINGLE_RING
```

`MOONEP_COMBINE_V2_BIDIRECTIONAL_RING`和Server-Pair枚举继续保留为映射框架能力和回归
对象，但当前生产路径不自动选择它们。Group mode严格限制为128P，非128P调用必须生成无效
route，不能静默回退。

Group反向core使用`GroupInnerIndex(source/destination)`直接求逆；successor保留source
inner身份，由下一step的Group receive映射唯一确定。普通Kernel的payload、QP、Done、CQ、
Credit和Kernel ABI均不变，只有step route端点随mode改变。

独立Group Kernel曾复制完整数据面并产生负优化，删除后不得重新引入第二套发送、完成或Reduce
实现。需要新增映射时，只扩展`MapSendDst/MapRecvSrc`及统一`StepRoute`合同。
