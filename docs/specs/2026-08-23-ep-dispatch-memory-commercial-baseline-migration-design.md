# EP Dispatch Memory 商发基线迁移方案

## 状态

- 日期：2026-08-23
- 状态：已批准，可进入实现
- 目标文件：`src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h`
- 商发基线：`D:/3_codex/ops-transformer-master/mc2/moe_distribute_dispatch_v2/op_kernel/arch35/moe_distribute_dispatch_v2_a5_full_mesh.h`
- 基线 SHA-256：`6e2ea149e0285e41dba197463a3d93a09c292bb50dbdbbb432340ac1d1ae2801`
- 目标环境：CANN 9.1，A5 / `__NPU_ARCH__ == 3510`

## 目标

以商发 `MoeDistributeDispatchV2A5FullMesh` 为实现主体，迁移发送、接收、状态检测、计数、前缀和、UB 队列和同步控制流。当前 TileXR Dispatch 仅作为公共接口、MXFP8 helper 和通信窗口适配的参考，不作为热路径实现基线。

迁移遵循以下原则：

1. 除 TileXR 通信域、现有 Kernel ABI 和明确阉割的功能外，商发控制流保持一致。
2. `assistInfoForCombineOut` 改为商发连续三元组，不再生成 TileXR 四元组。
3. 不在商发热路径中混入当前 Dispatch 的逐 token scalar 重排、逐 token flag 清理、额外 GM DFX 写或全 UB 清零。
4. 商发迁移实现直接落在现有无版本后缀的 Kernel header；验证依靠源码 provenance
   和 Git 对照，不在活动源码树中并行保留旧实现。

## 范围

迁移实现保留：

- 现有 `TileXRMoeEpDispatchMemory` 和 `TileXRMoeEpDispatchMemoryV2` 公共函数签名；
- 现有嵌入式 AICore Kernel 的 22 槽参数 ABI；
- FP16、BF16 输入；
- 非量化输出；
- 当前 Memory Dispatch 已支持的 MXFP8 E4M3/E5M2 输出和 `dynamicScalesOut`；
- token active mask；
- shared expert；
- `expertTokenNumsType` 的 prefix-sum/count 两种输出；
- 商发小 BS 全核计数策略、sender 多 buffer、receiver UB ping/pong 和批量 flag 检测/清理。

迁移实现明确删除：

- expert active mask；
- zero-compute expert；
- elastic scaling 和 rank remap；
- smooth scales、expert scales 和 `expandScalesOut`；
- performance DFX、通信耗时记录和 run-position GM 打点；
- TP、reduce-scatter 和 expert shard；
- FP4/HIF8 等当前 TileXR ABI 未支持的数据类型；
- 商发内部 `dataState_`、半窗口 ping-pong 和 HCCL Context/window lookup。

`scales`、`expertScales`、`tpRecvCountsOut`、`expandScalesOut` 等公共参数不从 API 中删除，但 Memory Host 继续拒绝非空或非默认配置。`TILEXR_EP_ACTIVE_MASK_EXPERT` 枚举也保留给其他后端，Memory 对该值返回 `TILEXR_ERROR_NOT_SUPPORT`。

## 非目标

- 不改变非 Memory Dispatch/Combine 的四元组协议和 `EpAssistTuple`。
- 不把商发源码目录加入 TileXR include、link 或运行时依赖。
- 不在迁移阶段重构商发算法或引入新的 polling timeout、DCCI、诊断状态写。
- 不在商发迁移头文件首次落地时直接替换生产入口。
- 不以本地 source test 代替 A5 Kernel 编译和多机硬件验证。

## 接口契约

Kernel 入口参数顺序和宽度保持不变：

1. `commArgsGM`
2. `xGM`
3. `expertIdsGM`
4. `xActiveMaskGM`
5. `expandXOutGM`
6. `dynamicScalesOutGM`
7. `expertTokenNumsOutGM`
8. `sendCountsOutGM`
9. `assistInfoForCombineOutGM`
10. `bs`
11. `h`
12. `topK`
13. `moeExpertNum`
14. `sharedExpertNum`
15. `sharedExpertRankNum`
16. `globalBs`
17. `expertTokenNumsType`
18. `activeMaskType`
19. `quantMode`
20. `dtype`
21. `expandXOutDtype`
22. `magic`

`DispatchMemoryKernelArgs` 仍满足 `22 * sizeof(uint64_t)`，Host launch 和公共 C API 不增加参数。

### 三元组输出

Memory 的每个接收 token 输出连续三个 `int32_t`：

```text
[sourceEpRank, sourceTokenIndex, routeSlot]
```

- MoE route 的 `routeSlot` 为 `[0, topK)`；
- shared expert route 的 `routeSlot` 为 `topK + sharedExpertIndex`；
- 三元组与 `expandXOut` 的接收顺序一一对应；
- 第 `i` 条记录从 `assistInfoForCombineOutGM + i * 3 * sizeof(int32_t)` 开始。

公共指针类型不变，但 Memory Dispatch/Combine 的内部宽度固定为 3。`src/ep/common/ep_window.h` 中非 Memory 后端使用的 `kEpAssistTupleInts == 4` 和 `EpAssistTuple` 不修改；在 `ep_memory_window.h` 中新增独立的 `kEpMemoryAssistInfoWidth = 3`。Memory Combine 必须同步使用该常量，不能继续以 4 为 stride。

## 模板和运行时分派

迁移实现保留商发 `if constexpr` 热路径，核心类型为：

```text
MoeDispatchMemory<XType, IsMxfp8, IsMxfp8E4M3>
```

Kernel entry 负责把运行时 `dtype`、`quantMode` 和 `expandXOutDtype` 分派到编译期特化。token 循环中不得保留 dtype 或量化模式的运行时分支。

当前量化接口契约保持不变：Dispatch 的 `quantMode == 0` 为非量化，`quantMode == 4` 为 MXFP8，E4M3/E5M2 由 `expandXOutDtype` 选择。MXFP8 数值计算复用 `tilexr_ep_mxfp8_quant.h`，但输入搬运、padding、三元组填充、480 B 打包和 MTE event 顺序必须放在商发对应位置。

## 通信域适配

### Context adapter

新增 `TileXRDispatchMemoryContext`，只负责：

- 从 `CommArgs` 读取 `rank`、`rankSize` 和 `peerMems[]`；
- 保存 `magic`；
- 返回当前 round 的 peer state、combine window 和 dispatch window 基址。

不得在 adapter 中复刻 HCCL context、elastic rank map 或商发 `InitWinState`。

### Round 和窗口地址

外层双 buffer 只由 `magic` 选择：

```text
roundBase(rank) = TileXREpMemoryRoundDataBase(peerMems[rank], magic)
stateBase(rank) = roundBase(rank)
combineBase(rank) = stateBase(rank) + kEpMemoryStateWindowBytes
dispatchBase(rank) = combineBase(rank)
                   + axisMaxBS * (topK + sharedExpertNum) * combinePackedRowBytes
```

其中 `combinePackedRowBytes`、dispatch 的 `hCommuSize_`、`expertPerSizeOnWin_` 和 512 B/480 B 分块规则继续按商发公式计算。Host layout validation 必须证明 dispatch payload 在当前 round 的 `IPC_BUFF_MAX_SIZE` 内。

商发以下逻辑直接删除：

- `dataState_ * (totalWinSize_ / 2)`；
- `dataState_ * WIN_STATE_OFFSET`；
- `winDataSizeOffset_` 中的内部 half-window 分量；
- `InitWinState` 对窗口代际的选择；
- `selfDataStatusGMTensor_[1]` DFX 写。

### Round 生命周期

Dispatch Memory 不执行 Kernel 内部的 round 复用等待或 retirement 发布。GM round
的复用生命周期由外部通信域双 buffer 管理，Kernel 只按
`magic % PING_PONG_SIZE` 选择本轮地址。

商发 receiver 的 UB ping/pong 仍然保留；它用于 receiver 本地流水，不属于
已删除的 GM round generation 同步。

### State 和 workspace 布局

每个 sender 到本 rank/local expert 的 count status 仍是 32 B record，字段布局严格使用商发格式：

```text
word 0: float 1.0 bit pattern, 表示 count 到达
word 1: int32 token count
word 2..7: padding
```

商发 `workspaceGM` 不在现有 Kernel ABI 中。迁移实现将 replicated receive-count workspace 放在本 round 的 state window 内，并从 `kEpMemoryDispatchDfxOffset` 向低地址分配：

```text
recvWorkspaceBytes = AlignUp(aivNum * rscvStatusNum * sizeof(int32_t), 32)
recvWorkspaceBase  = selfStateBase + kEpMemoryDispatchDfxOffset - recvWorkspaceBytes
```

Host 必须验证：

```text
rscvStatusNum * 32 <= kEpMemoryDispatchDfxOffset - recvWorkspaceBytes
```

这样 workspace 不与 count status、cumsum、round sync 或 payload 重叠，也不再使用当前实现的 `IPC_BUFF_MAX_SIZE - workspaceBytes` 尾部地址。

cumsum 数据和完成 flag 分别使用：

- `kEpMemoryDispatchCumsumOffset`；
- `kEpMemoryDispatchCumsumFlagOffset`。

两段地址可以不同于商发 workspace 中的相对偏移，但其数据布局、写入批次、轮询和清理控制流必须保持商发一致。

## 商发流程映射

| 商发函数/流程 | TileXR Dispatch Memory |
| --- | --- |
| `SetTilingData` | 从现有 22 槽 ABI 和 `CommArgs` 设置同名有效属性 |
| `SetTilingDataAndCal` | 保留 shape、分块、专家/rank、count core 和 send core 计算 |
| `SetDataStatus` | 改为 Context adapter 地址绑定，不调用 `InitWinState` |
| `ExpIdsCopyAndMaskCal` | 保留无 mask/token mask 路径，删除 expert mask 和 zero expert 路径 |
| `TokenActiveMaskCal` | 原样保留商发 token 有效数计算和同步 |
| `CalcSendTokenBufNum` | 按 UB 物理地址差计算剩余空间，并限制到 8 个 event buffer |
| `TokenToExpert` | 保留输入 queue、三元组填充、480 B 打包和 MTE2/V/MTE3 顺序 |
| `TokenToExpertInQuant` | 保留商发流水位置，仅替换为 TileXR MXFP8 helper |
| `SendToMoeExpert` | 保留 token 分核、目的 slot 计算和 sender buffer 循环 |
| `SendToSharedExpert` | 保留 shared group/rank 和 routeSlot 计算，删除 elastic remap |
| `AllToAllDispatch` | 保留 UB pool、固定 buffer 后动态 sender buffer 的分配顺序 |
| `CalAndSendCnt` | 直接进入非 elastic 的 `CalAndSendCntByRankAndExpert` |
| `SendExpertCntRange` | 保留同 rank 连续 expert 的 strided MTE3 count 发送 |
| `BufferInit` | 保留 count/cumsum UB，删除 performance/elastic UB |
| `WaitDispatch` | 保留批量 status DMA、flag reduce、status 清理和 count 求和 |
| `GetCumSum` | 保留 cumSum core 间软同步、前缀和和 flag 清理 |
| `CalRecvAndSetFlag` | 保留 `sendCountsOut` 输出和每 AIV workspace 副本 |
| `CalCumSum` | 保留小 BS/大 BS 两种 count 分核策略 |
| `WaitCumSumFlag` | 保留批量轮询和清理，不增加逐 flag fallback |
| `SetValidExpertInfo` | 保留 `expertTokenNumsOut` 和有效 source/expert 筛选 |
| `CheckDataArriveWithFlag` | 保留一次 strided GM-to-UB flag 搬运和 ReduceSum 检测 |
| `CopyInAndOut` | 保留 payload 和连续三元组的 strided MTE3 输出，删除四元组 scalar 重排 |
| `GetLocalWindowSrcDataBlockIdx` | 保留 full-mesh rotation，删除 elastic 分支 |
| `ClearLocalWindowDataFlags` | 保留输出完成后的批量 strided flag 清理 |
| `WaitAndFormatOutput` | 保留 receiver UB ping/pong、批量轮询和轮转 source 检测 |
| `LocalWindowCopy` | 保留 reset、cumsum gate、连续化和输出顺序，删除 run-position DFX |
| `Process` | 保留 send/count 并行和 local-copy stage barrier，不增加 round sync |

## 删除项清单

active header 中不得出现以下成员或功能实现：

- `isExpertMaskFlag_`；
- `zeroComputeExpertNum_`；
- `hasElasticInfoFlag_`、`isScalingDownFlag_`、`elasticInfoTensor_`；
- `hasExpertScalesFlag_`、`expertScalesTensor_`、`expandScalesOutGM_`；
- `isPerformanceFlag_`、`performanceInfoTensor_`、`performanceFlagTensor_`；
- `ExpertActiveMaskInit`、`ExpertActiveMaskCal`、`CalValidBSCnt`、`CalValidExpIdx`；
- `GenerateGatherMaskTensor`、`MaskZeroComputeExpert`、`ZeroComputeExpertMaskCal`；
- `InitElasticInfo`、`CalAndSendCntByExp`、`RecordRankCommDuration`、`RunPosRecord`；
- `dataState_`、`totalWinSize_` 和商发内部 half-window offset；
- current Dispatch 的四元组 `logicalExpert` 生成循环。

删除功能后，对应 GlobalTensor、LocalTensor、TBuf 和运行时 bool 也必须一并删除，不能保留“暂时不用”的 UB 占位。

## UB 策略

### Sender

1. `calBeginBuf_` 仅作为 UB 起点标记。
2. 先分配 expertIds、token mask、量化临时区和输入 queue 等固定 buffer。
3. 分配 `calEndBuf_`，使用商发物理地址差计算剩余 UB。
4. `sendTokenBufNum_ = remaining / hCommuSize_`，最大 8；Host/shape validation 必须确保至少为 1。
5. 非量化路径不得为每个 token 清零整个 input UB；只有 MXFP8 padding 所需范围允许按商发方式初始化。

### Receiver

1. `LocalWindowCopy` 前按商发执行 `TPipe::Reset`。
2. 固定分配 scalar、cumsum、expert map/finish/left buffer。
3. 剩余 `tBufRealSize_` 统一用于两个 packed-data buffer、flag reduce workspace 和 flag receive buffer。
4. `maxCopyTokenCnt` 使用商发反推算法，确保 ping/pong + flag workspace 全部落入 `kEpMemoryFullUbBytes`。
5. 输出 buffer 复用前只等待对应 `MTE3_V` event，不增加 `PIPE_ALL`。

## GM 访问和同步不变量

- expertIds 每个参与 sender/count 的 core 只批量搬入一次。
- count status 每个目的 expert/rank 只发布一个 32 B record。
- sender 每个 route 只有输入 GM 读取和一个 packed payload GM 写。
- receiver 每次 polling 使用一次 strided flag DMA，不逐 slot scalar 读 GM。
- payload 到达后批量搬入 UB，再用 MTE3 分别输出 payload、dynamic scale 和三元组。
- 三元组使用 `blockCount = arriveCount`、`blockLen = 3 * sizeof(int32_t)` 的 strided MTE3，不做逐 token `GetValue/SetValue` 重排。
- payload flag 在相应 payload/metadata 输出全部完成后批量清理。
- 不写 performance、run-position、超时或调试 GM 状态。
- token 热循环中不允许 `PIPE_ALL`；保留商发 stage barrier 和必要 event。
- TileXR 新增 GM 访问仅限现有 ABI 缺少 workspace 所需的 state-window receive-count workspace。

## Host 和消费者适配

公共函数签名保持不变，但实施时必须同步以下内部契约：

1. `ep_dispatch_memory_host.cpp` 对 `ACTIVE_MASK_EXPERT` 返回 `TILEXR_ERROR_NOT_SUPPORT`。
2. Host 增加 state status、replicated receive-count workspace、cumsum 和 payload 边界校验。
3. `tilexr_ep_dispatch_memory_kernel.cpp` 保留参数顺序和防御性检查，并调用迁移后的 compile-time specialization。
4. `tilexr_ep_combine_memory_kernel.h` 的 `ASSIST_INFO_WIDTH` 改为 `kEpMemoryAssistInfoWidth`，批量搬入宽度和索引 stride 同步变为 3。
5. 非 Memory `ep_window.h`、Dispatch/Combine kernel 和 `EpAssistTuple` 保持四元组，不受影响。
6. Demo、buffer-size 文档和 Memory 测试按三元组最低容量计算；已有按四元组超额分配的调用仍然内存安全，但第四项不再产生。

## 实施顺序

### 任务 1：建立商发迁移骨架

- 目标：以固定 SHA-256 的商发文件覆盖 `tilexr_ep_dispatch_memory_kernel.h`
  的实现，迁移类、常量和主流程。
- 范围：现有无版本后缀的 active header。
- 约束：删除无关 include/tiling/HCCL 依赖，保留商发函数边界和顺序。
- 验收：源码可清晰逐函数对照商发基线，阉割项不存在。

### 任务 2：接入 TileXR ABI 和通信 adapter

- 目标：实现 22 槽参数到商发属性的映射和 peer window 地址。
- 范围：active header、必要的 `ep_memory_window.h` 常量。
- 前置：任务 1。
- 验收：无内部 `dataState_`，所有 peer 地址由 `peerMems[] + magic round` 推导，布局静态断言和 Host 可验证公式一致。

### 任务 3：迁移 sender、count 和 receiver 热路径

- 目标：按映射表完成商发控制流，并接入现有 MXFP8 helper。
- 范围：active header。
- 前置：任务 2。
- 验收：sender 动态 1-8 buffer、商发 count 分核、receiver ping/pong、批量 flag 检测和清理全部存在；禁止项和额外 GM 路径不存在。

### 任务 4：统一 Memory 三元组消费者

- 目标：Dispatch Memory 直接输出三元组，Combine Memory 以 stride 3 批量读取。
- 范围：`ep_memory_window.h`、`tilexr_ep_dispatch_memory_kernel.h`、`tilexr_ep_combine_memory_kernel.h` 和对应 source tests。
- 前置：任务 3。
- 验收：Memory 路径无 `* 4` assist stride 和 logical-expert scalar 重排；非 Memory 四元组测试不变。

### 任务 5：Kernel 编译和入口接入

- 目标：以相同 22 槽 ABI 构建现有 `tilexr_ep_dispatch_memory_kernel`
  binary/name，并将 entry 直接接入迁移实现。
- 范围：现有 entry `.cpp`、CMake embed 和注册状态。
- 前置：任务 4。
- 约束：公共 C API 不增加版本选择参数；A/B 使用独立 Git revision/build 进行。
- 验收：现有 binary/name 注册到迁移实现，stub 和 registration 状态保持一致。

### 任务 6：Host 限制、测试和正式切换

- 目标：固化 unsupported 配置和窗口容量检查，完成硬件验证后将默认 Memory Dispatch 指向迁移实现。
- 范围：Memory Host validation、Kernel entry、tests 和维护文档。
- 前置：任务 5 的 correctness/performance gate 通过。
- 验收：公共 API/Kernel ABI 不变，不引入临时 A/B 选择或并行 binary 注册；
  需要回退时整体回退迁移提交。

## 验证方案

### 本地和静态验证

- source test 检查 active header、商发核心函数、三元组 stride 和 TileXR context adapter；
- source test 禁止 expert mask、zero expert、elastic、performance DFX、internal half-window 和四元组 scalar loop；
- Host unit test 将“memory expert mask supported”改为 `TILEXR_ERROR_NOT_SUPPORT`；
- 增加 state workspace 边界、payload window 边界和 `sendTokenBufNum_ >= 1` 的配置测试；
- CANN 9.1 A5 工具链编译现有 Kernel binary；
- 保持 `DispatchMemoryKernelArgs` 大小和字段顺序静态断言。

### 硬件正确性阶梯

按 1P、8P、32P、64P 顺序验证：

- FP16/BF16，`quantMode=0`；
- `activeMaskType=NONE/TOKEN`；
- 无 shared expert、单 shared expert、多 rank shared expert group；
- `expertTokenNumsType=prefix-sum/count`；
- BS 小于 128 和不小于 128，覆盖两种 count core 策略；
- MXFP8 E4M3/E5M2；
- 连续轮次大于 `PING_PONG_SIZE`，覆盖外部双 buffer 选择和 magic wrap；
- Dispatch Memory + Combine Memory 端到端三元组消费。

每个 case 对比 expand payload、三元组、sendCounts 和 expertTokenNums。比较必须使用相同输入、rank 拓扑、barrier 和 profiler 设置。

### 性能验证

主性能 case：

```text
64P / BS=96 / H=7168 / K=6 / 384 experts / BF16 / quantMode=0
```

至少记录：

- Dispatch Kernel 总时延和 P50/P95；
- sender、count/cumsum、receiver 尾部阶段占比；
- GM read/write bytes 或 MTE transaction 指标；
- receiver flag polling 和 local-copy stall；
- UB 使用量、sender buffer 数和 receiver batch token 数。

性能验收以商发控制流和旧 TileXR Dispatch A/B 为依据。迁移实现不应再出现当前四元组 scalar 重排、逐 token flag 清理和非量化全 UB 清零造成的额外开销；新增 round generation GM 访问必须是每 core/每 round 的固定成本，不能进入 token 循环。

### 远端执行约束

- 使用 Mutagen `one-way-safe` 单向同步；
- 将构建、进程检测、测试和日志收集写入任务级 shell 脚本，再通过 SSH 执行固定脚本路径；
- 任意 NPU 进程都视为占用，每 5 秒重检，120 秒仍未空闲则停止本轮任务；
- 性能对比保留相同的 framework profiler 和 Dispatch/Combine stage barrier，关闭非目标 DFX。

## 切换门槛和回退

满足以下条件后才切换默认入口：

1. A5 Kernel 编译通过且无 UB 超限；
2. 三元组 Dispatch/Combine pair 在全部正确性阶梯通过；
3. 连续 round 测试无复用死锁、脏 flag 或 payload 覆盖；
4. 64P 主 case 无显著性能回退，并确认 Dispatch Memory 不产生 round-sync GM 访问；
5. Host unsupported 配置和窗口边界均有测试；
6. binary 注册、Kernel name 和实际源码 provenance 可追溯。

源码树不保留旧 `tilexr_ep_dispatch_memory_kernel.h` 或并行 binary。出现 correctness、
hang 或性能门槛失败时，通过 Git 回退迁移提交；公共 API 和调用方参数保持不变，
通信窗口布局随实现提交整体回退。

## 迁移完成判定

完成迁移必须同时满足：

- active header 的核心流程可以逐函数映射到固定商发基线；
- 与商发不同的代码只位于 TileXR ABI、MXFP8 helper 和通信 adapter 边界；
- unsupported feature 的代码、成员和 UB buffer 已实际删除；
- Memory assist 数据为连续三元组，Combine Memory 使用同一宽度；
- 没有额外的逐 token GM 访问、scalar metadata 重排或不合理 UB 清零；
- 通过约定的编译、正确性、重复 round 和性能门槛后完成默认入口切换。
