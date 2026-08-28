# EP Dispatch Memory 性能耗时拆解方案

## 状态

- 日期：2026-08-23
- 状态：已实施并完成静态审核，尚未编译或运行测试
- 参考实现：`docs/specs/2026-08-23-ep-combine-memory-profiling-design.md`
- 目标 Kernel：`src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h`
- 主分析场景：64P / 384 experts / BS 96 / H 7168 / K 6 / BF16 / 无量化 / balance

## 目标

为 EP Dispatch Memory 增加与 Combine Memory 同模型的编译期开关 profiling：

1. 每个 AIV 核记录固定下标的绝对 system cycle 时间点；
2. 对循环内重复操作累计细粒度 metric；
3. 区分 sender core 和 count/cumsum core；
4. 每个 AIV 最终只向 GM 发布一次 512B 记录；
5. Host 只在最后一个 timed iteration 完成后做一次 D2H；
6. 64P 自动选择最快、P50、最慢 rank，并生成可直接导入 `chrome://tracing` 的三个 JSON；
7. Dispatch 和 Combine 的 profiling 记录使用不同的通信双 buffer，避免互相覆盖。

profiling 仅用于耗时归因。生产性能数据必须使用 profiling 关闭的二进制。

## 范围和非目标

本次实施范围包括 Dispatch Memory Kernel 打点、公共 profile record、固定 DFX 地址、demo 采集、离线解析、64P 脚本和静态/解析测试。

本次不改变：

- Dispatch/Combine 公共 API；
- Dispatch Kernel 的 22 槽参数 ABI；
- 商发基线的发送、count/cumsum、接收和输出控制流；
- 现有 GM payload、status、workspace 和 cumsum 布局；
- 正常构建默认关闭 profiling 的行为；
- 为方便计时而增加新的 `PipeBarrier`、event wait、DCCI 或 GM polling；
- 每轮采集、多轮 profile 历史或 profiling 区域的 magic 乒乓。

## Dispatch/Combine Profiling Buffer 分离

这里的 buffer 0/1 指通信域已有的两个 round buffer，不是在 100 KiB DFX 区域内再切分空间。两个算子的记录地址固定，不随本次运行的 `magic` 变化：

```text
dispatchProfileBase = TileXREpMemoryRoundDataBase(peerMems[localRank], 0)
                    + kEpMemoryDispatchDfxOffset

combineProfileBase  = TileXREpMemoryRoundDataBase(peerMems[localRank], 1)
                    + kEpMemoryDispatchDfxOffset

record(core)        = profileBase + core * kEpMemoryDispatchDfxStride
```

约束：

- Dispatch profiling 固定使用 buffer 0；
- Combine profiling 从当前 buffer 0 调整为固定使用 buffer 1；
- 每个 buffer 内仍是 200 x 512B，即 `[768 KiB, 868 KiB)`；
- `generation` 只用于校验记录是否属于最终 launch，不参与地址选择；
- Host 不比较两个 buffer 的 magic，也不选取最大 magic；
- `ep_memory_window.h` 增加明确的 profile buffer 常量和静态断言，要求两者不同且均小于 `PING_PONG_SIZE`；
- Combine Kernel、Combine demo collector 和 Combine profiling 文档必须同步切换到 buffer 1，解析日志和 JSON schema 不变。

固定 DFX 区域位于 state window 内：Dispatch receive-count workspace 在其下方结束，cumsum 区在其上方开始，payload 从 1 MiB 开始。因此调整 profile buffer 不改变任何单轮 payload 地址。

## 编译期开关

新增独立选项：

```text
TILEXR_EP_DISPATCH_MEMORY_ENABLE_PROFILING=OFF
```

仅在该选项开启时定义同名 Kernel 宏。关闭时以下内容必须从 AICore binary 中消失：

- `GetSystemCycle()`；
- profile 时间点、metric 和诊断成员；
- 计数器更新；
- profile GM 写入。

Dispatch 与 Combine 的开关相互独立。普通 `all`、correctness 和 production perf 构建明确同时使用 `OFF`；Dispatch `--profile-only` 只打开 Dispatch profiling。

## Record ABI

新增 `src/ep/common/ep_dispatch_memory_profile.h`。每核记录固定 512B，与 `kEpMemoryDispatchDfxStride` 完全一致：

```text
uint32 marker
uint16 version
uint16 recordBytes
uint32 rank
uint32 core
uint32 blockDim
uint32 coreRole
uint32 timePointCount
uint32 metricCount
uint64 generation
uint64 timePoint[40]
uint64 metric[16]
```

记录使用独立 marker，例如 `EPDP`，初始 version 为 1。`coreRole` 固定为：

- `0`: sender core，即 `aivId_ < aivUsedAllToAll_`；
- `1`: count/cumsum core，即 `aivId_ >= aivUsedAllToAll_`。

未执行的分支时间点在 `WriteProfile()` 中以前一个有效时间戳回填，保证所有记录使用相同 `timePointCount` 且单调。分支差异通过 `coreRole` 和诊断字段解释，而不是改变 schema 或 point 顺序。

## Time Point Schema

所有时间点只放在现有控制流边界，不增加同步。建议固定 19 个点：

| Index | 名称 | 精确边界 |
| ---: | --- | --- |
| 0 | `INIT_BEGIN` | `Init()` 的第一条 cycle 采集，早于 overflow mode 和 TPipe 初始化 |
| 1 | `STATIC_INIT_END` | Context、属性、GM Tensor、workspace/window 地址全部初始化完成 |
| 2 | `ROUTE_PREPARE_END` | `ExpIdsCopyAndMaskCal()` 返回 |
| 3 | `COUNT_PUBLISH_END` | 本核 `CalAndSendCnt()` 完成；未执行的 sender core 回填 |
| 4 | `SEND_BUFFER_INIT_END` | `CalcSendTokenBufNum()` 完成，动态 sender buffer 数已确定 |
| 5 | `SEND_ISSUE_END` | `SendToMoeExpert/SendToSharedExpert` 返回，分配给本核的 payload 写已发起 |
| 6 | `COUNT_STATUS_WAIT_END` | count core 的 `WaitDispatch()` 返回，status 等待、清理和 recv count 汇总完成 |
| 7 | `CUMSUM_PUBLISH_END` | `CalRecvAndSetFlag()` 返回，sendCounts/workspace/cumsum flag 已发布 |
| 8 | `PRE_LOCAL_BARRIER_END` | `Run()` 中 sender/count 分支后的既有 `PIPE_ALL` 完成 |
| 9 | `LOCAL_COPY_INIT_END` | `LocalWindowCopy()` reset、固定 UB 分配和 receiver 分核完成，尚未等待 cumsum gate |
| 10 | `CUMSUM_GATE_END` | `WaitCumSumFlag()` 返回，sendCounts 可用 |
| 11 | `RECEIVE_METADATA_END` | `SetValidExpertInfo()` 返回，valid expert 和 token 范围确定 |
| 12 | `FIRST_PAYLOAD_READY` | 第一个 batch 通过 `CheckDataArriveWithFlag()` |
| 13 | `ALL_PAYLOADS_READY` | 最后一个 valid expert 的最后一个 batch 通过 flag 检测 |
| 14 | `OUTPUT_DRAIN_END` | `WaitAndFormatOutput()` 中已使用 MTE3 ping/pong buffer 的既有 wait 全部完成 |
| 15 | `FLAG_CLEAR_ISSUE_END` | `ClearLocalWindowDataFlags()` 返回，清 flag 写已发起 |
| 16 | `LOCAL_COPY_END` | `LocalWindowCopy()` 返回；无接收任务的核也记录此点 |
| 17 | `FINAL_BARRIER_END` | `Run()` 尾部既有 `PIPE_ALL` 完成 |
| 18 | `FINAL_END` | overflow mode 恢复后、发布 profile record 前的最后时间点 |

分支顺序保证：

- sender core 记录 2/可选3/4/5，6/7 回填；
- count core 记录 2/3/6/7，4/5 回填；
- `sendNum_ == 0` 或 `validNum == 0` 时，11-15 中未执行的点自动回填；
- `WriteProfile()` 在 18 之后执行，因此 GM 发布耗时不计入 Kernel 拆解。

## Accumulated Metric Schema

循环内操作使用 Combine Memory 同样的 `BeginProfileMetric()` / `EndProfileMetric()` 累计 cycle。异步写结尾统一命名为 `issue`，不得为获得完成时间新增 barrier。

| Index | 名称 | 统计范围 |
| ---: | --- | --- |
| 0 | `route_prepare` | `ExpIdsCopyAndMaskCal` 的 expertIds/mask 搬运和计算 |
| 1 | `count_compute` | 每段 expert count 的 Duplicate、比较、ReduceSum 和 scalar 组装 |
| 2 | `count_store_issue` | `SendExpertCntRange` 的 count status GM 写发起 |
| 3 | `dispatch_input_load` | 每个发送 route 的输入 GM-to-UB 及已有 MTE2 完成边界 |
| 4 | `dispatch_quant` | MXFP8 quant 计算到已有 V/S 同步；无量化为 0 |
| 5 | `dispatch_pack` | 三元组填充、480B packing 和 sender buffer 复用等待 |
| 6 | `dispatch_store_issue` | packed payload GM 写发起，不增加完成 wait |
| 7 | `status_flag_load` | `WaitDispatch` 每轮 status flag GM-to-UB |
| 8 | `status_flag_reduce` | `WaitDispatch` 每轮 ReduceSum 和 scalar 判断 |
| 9 | `cumsum_peer_wait` | `GetCumSum` 的跨 count-core 完成轮询总耗时 |
| 10 | `cumsum_gate_wait` | `WaitCumSumFlag` 的 local-copy gate 轮询总耗时 |
| 11 | `payload_flag_load` | `CheckDataArriveWithFlag` 的 strided flag GM-to-UB |
| 12 | `payload_flag_reduce` | payload flag ReduceSum 和 scalar 判断 |
| 13 | `payload_load` | `CopyInAndOut` 的接收 buffer 复用等待、packed payload GM-to-UB 及既有 MTE2/MTE3 event |
| 14 | `output_store_issue` | payload、dynamic scale、assist 三元组输出写发起 |
| 15 | `payload_flag_clear_issue` | `ClearLocalWindowDataFlags` 的分批 strided 清零写发起 |

Metric 可以重入累计，但不得互相嵌套统计同一大阶段；time point 表示阶段墙钟时间，metric 表示该核在重复子操作上的累计 cycle，两者不要求求和等于 Kernel 总耗时。

## Diagnostics

`timePoint[19..39]` 的空余槽存放固定下标诊断值，建议首版记录：

| 名称 | 含义 |
| --- | --- |
| `assigned_routes` | sender core 分配到的 route 数 |
| `issued_routes` | 实际发起 payload 写的 route 数 |
| `count_records` | 本核计算/发布的 count status 数 |
| `alltoall_core_count` | `aivUsedAllToAll_`，用于校验 sender/count 角色边界 |
| `send_buffer_num` | `sendTokenBufNum_` |
| `status_poll_attempts` | `WaitDispatch` status 检测次数 |
| `status_poll_misses` | 未满足目标值的次数 |
| `cumsum_poll_attempts` | `GetCumSum` 轮询次数 |
| `cumsum_gate_attempts` | `WaitCumSumFlag` 轮询次数 |
| `valid_experts` | 本核 `SetValidExpertInfo` 得到的 validNum |
| `receive_batches` | 成功接收并输出的 batch 数 |
| `receive_tokens` | 成功接收并输出的 token 数 |
| `payload_poll_attempts` | `CheckDataArriveWithFlag` 调用次数 |
| `payload_poll_misses` | payload flag 未全部到达次数 |
| `max_copy_token_count` | receiver UB 反推得到的单 batch 最大 token 数 |
| `blocks_per_token` | 每 token 的 512B block 数 |

计数器更新仅在 profiling 编译期开关内存在。不得为了诊断新增 GM 读取或改变 polling 次序。

## Kernel 修改映射

### 公共 helper

在 `MoeDispatchMemory` 中增加与 `MoeCombineMemory` 同形的：

- `InitProfile(startCycles, magic)`；
- `RecordProfilePoint(index)`；
- `BeginProfileMetric()`；
- `EndProfileMetric(index, startCycles)`；
- `WriteProfile()`。

`TileXRDispatchMemoryContext` 增加 `ProfileRecordBase(core)`，固定返回 buffer 0 的本 rank 地址。记录发布前回填缺失点、写入 diagnostics，并校验数组下标均小于 capacity。

### Sender 路径

- `Init()` 最早捕获 `INIT_BEGIN`，尾部记录 `STATIC_INIT_END`；
- `ExpIdsCopyAndMaskCal()` 外层累计 `route_prepare`，返回后记录 `ROUTE_PREPARE_END`；
- count 计算和 `SendExpertCntRange()` 分别累计 `count_compute`、`count_store_issue`；
- `CalcSendTokenBufNum()` 后记录 `SEND_BUFFER_INIT_END`；
- `TokenToExpert()` 与 `TokenToExpertInQuant()` 分别累计 input load、quant、pack、store issue；
- `AllToAllDispatchA5()` 返回后记录 `SEND_ISSUE_END`；
- active token 数为 0 的 early return 保持不变，缺失点交由发布阶段回填。

### Count/Cumsum 路径

- `CalAndSendCnt()` 后记录 `COUNT_PUBLISH_END`；
- `WaitDispatch()` 的每次 load/reduce 累计 metric，并更新 attempts/misses；
- `WaitDispatch()` 完整返回后记录 `COUNT_STATUS_WAIT_END`；
- `GetCumSum()` 和 `WaitCumSumFlag()` 只在现有 while 外取总 metric，在循环内只更新 attempt counter；
- `CalRecvAndSetFlag()` 返回后记录 `CUMSUM_PUBLISH_END`。

### Local Copy 路径

- 在 `LocalWindowCopy()` 固定 UB 初始化后记录 `LOCAL_COPY_INIT_END`；
- `WaitCumSumFlag()` 后记录 `CUMSUM_GATE_END`；
- `SetValidExpertInfo()` 后记录 `RECEIVE_METADATA_END`；
- `CheckDataArriveWithFlag()` 分开累计 flag load 和 reduce，并更新 attempts/misses；
- 第一个成功 batch 记录 `FIRST_PAYLOAD_READY`；最后一个 batch 成功时记录 `ALL_PAYLOADS_READY`；
- `CopyInAndOut()` 累计 payload load 和 output store issue；
- ping/pong MTE3 wait 完成后记录 `OUTPUT_DRAIN_END`；
- `ClearLocalWindowDataFlags()` 累计 clear issue，返回后记录 `FLAG_CLEAR_ISSUE_END`；
- `Run()` 在 `LocalWindowCopy()` 返回后记录 `LOCAL_COPY_END`，既有最终 barrier 后记录 `FINAL_BARRIER_END`。

## Host 采集契约

在 `tests/ep/demo/tilexr_ep_dispatch_demo.cpp` 增加 `CaptureDispatchMemoryProfile()`，沿用 Combine collector 的对齐安全读取方式：

1. 仅当 `TILEXR_EP_DEMO_DISPATCH_MEMORY_PROFILE=1` 时启用；
2. 强制 `PERF=1`、memory backend、standalone dispatch mode 和合法 run ID；
3. run ID 使用 `TILEXR_EP_DISPATCH_MEMORY_PROFILE_RUN_ID`；
4. warmup/timed loop 不增加 D2H；
5. 最终 stop event 完成后，从固定 buffer 0 拷贝一次 200 x 512B；
6. 用 core 0 的 `blockDim` 确定活跃记录数，并逐核校验 marker、version、rank、core、role、generation、point/metric count 和时间戳单调性；
7. 任何本 rank 采集错误都必须进入已有跨 rank barrier，再统一失败，避免其他 rank 卡住；
8. 输出一条 `EP_DISPATCH_MEMORY_SAMPLE` 和每核一条 `EP_DISPATCH_MEMORY_PROFILE`。

standalone dispatch 的期望 generation 与 Combine 当前规则一致：新 communicator 下为 `warmupCount + loopCount`。只有最终 iteration 被保留。

日志字段包含现有 shape/mode provenance、`core_role`、全部 `tN`、`mN` 和 diagnostics。不同 rank 的 `blockDim` 可以不同，但每个 rank 内必须完整覆盖 `[0, blockDim)`。

## 离线解析与 Chrome Trace

新增 `tools/ep/dispatch_memory_trace.py`，以 `combine_memory_trace.py` 为结构基线，但使用独立 prefix、schema、time point、metric 和 diagnostic 表。

解析器必须拒绝：

- rank/core 集合不完整；
- marker/version/count 不匹配；
- generation 混杂或陈旧；
- 时间戳为 0 或非单调；
- `core_role` 与 `core < aivUsedAllToAll` 的日志元数据冲突；
- run ID、shape、dtype、quant、active mask provenance 与命令行不一致；
- blockDim 超过 200。

每个 rank 的耗时定义为所有活跃 AIV 的最大 `FINAL_END - INIT_BEGIN`。按 `(duration, rank)` 排序后选择：

- fastest：首项；
- P50：`ceil(0.5 * rankCount) - 1`；
- slowest：末项。

每个 JSON 包含一个 rank、每核一个 thread、相邻 time point 的完整事件和 accumulated metrics 即时事件。Trace 不再额外生成覆盖全部阶段的 Kernel 包络事件，避免与阶段事件形成冗余的父子覆盖。thread 名必须包含 `sender` 或 `count_cumsum` 角色。rank 内以最早 `INIT_BEGIN` 归一化，不跨 NPU 比较绝对 cycle。

Chrome Trace JSON 不写 `"displayTimeUnit": "us"`。`ts` 和 `dur` 仍按 cycle/1000 转为微秒，依赖 Trace Event 标准单位，确保可直接被当前 `chrome://tracing` 导入。

## 64P 脚本接入

统一入口及节点脚本：

- `tests/ep/cluster/run_memory_dispatch_msprof_64p.sh`；
- `tests/ep/cluster/run_memory_dispatch_node.sh`。

通过 `--op dispatch --mode breakdown` 执行：

1. 以 Dispatch profiling ON、Combine profiling OFF 构建并执行 EP CTest；
2. 按现有方式部署 artifact；
3. node 脚本接受 `profile` mode，设置 standalone dispatch profiling 环境变量；
4. 每个 rank 日志必须包含对应 run ID 的 `EP_DISPATCH_MEMORY_PROFILE`；
5. 主节点收集所有 rank 日志后调用 Dispatch trace exporter；
6. 输出 fastest/P50/slowest 三个 trace JSON 的路径和各自耗时；
7. 普通 `all` 和 `--perf-only` 强制 profiling OFF，防止误用打点二进制报告生产性能。

远端同步和执行继续遵守仓库规则：本地到主节点使用 Mutagen `one-way-safe`，服务器间使用 rsync/scp，测试逻辑写入脚本后通过 SSH 执行。

## 文件级实施清单

| 文件 | 修改内容 |
| --- | --- |
| `src/ep/common/ep_dispatch_memory_profile.h` | 新增 Dispatch record ABI、time point、metric、diagnostic 和 role 枚举 |
| `src/ep/common/ep_memory_window.h` | 新增 Dispatch=0、Combine=1 固定 profile buffer 常量及静态断言 |
| `src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h` | 增加编译期 helper、成员、各阶段 time point/metric/counter 和 buffer 0 发布 |
| `src/ep/kernels/tilexr_ep_combine_memory_kernel.h` | 仅将 profile record 地址从 buffer 0 切换到 buffer 1 |
| `src/ep/CMakeLists.txt` | 新增 Dispatch profiling option/define 和 profile header 依赖 |
| `tests/ep/build.sh` | 独立传递 Dispatch/Combine 两个 profiling CMake option |
| `tests/ep/demo/tilexr_ep_dispatch_demo.cpp` | 增加 Dispatch buffer 0 collector；Combine collector 改读 buffer 1 |
| `tools/ep/dispatch_memory_trace.py` | 新增日志校验、rank 选择和 Chrome Trace 导出 |
| `tests/ep/unit/test_tilexr_ep_kernel_sources.cpp` | 增加开关关闭零开销、打点位置、record 发布和 buffer 分离源码契约 |
| `tests/ep/python/test_dispatch_memory_trace.py` | 新增 synthetic 64P parser/selection/JSON 测试 |
| `tests/ep/CMakeLists.txt` | 注册 Dispatch trace Python 测试 |
| `tests/ep/cluster/run_memory_dispatch_msprof_64p.sh` | 统一 profiling 构建、breakdown 模式和 exporter 调用 |
| `tests/ep/cluster/run_memory_dispatch_node.sh` | 增加 profile mode、环境变量和日志完整性检查 |
| `docs/specs/2026-08-23-ep-combine-memory-profiling-design.md` | 将 Combine profile 地址更新为固定 buffer 1 |

## 实施顺序

### 任务 1：固化 ABI 和地址分区

- 先新增 Dispatch profile header 和 buffer 0/1 常量；
- 更新静态断言，证明每条 512B、最多 200 核且两算子使用不同 round buffer；
- 将 Combine Kernel/collector 改到 buffer 1，不改变 Combine schema。

验收：Dispatch 与 Combine 的地址公式在 Kernel、Host 和文档中完全一致，二者不再写同一物理区域。

### 任务 2：实现 Dispatch Kernel 打点

- 先实现 helper 和 publication；
- 再按 sender、count/cumsum、local-copy 顺序加 time points；
- 最后加循环 metric 和 diagnostics；
- 每完成一个函数即与原控制流对比，确认只增加 profiling 宏内语句。

验收：profiling OFF 的预处理路径无 cycle、counter、数组或 GM profile 写；profiling ON 不新增同步和轮询。

### 任务 3：实现 Host 采集和日志契约

- 增加 Dispatch collector 和环境校验；
- 保持 timed loop 不变，仅在最终 stop event 后 D2H；
- 同步切换 Combine collector 到 buffer 1。

验收：错误路径不会导致 rank barrier 踩踏；最终 generation 和所有 core metadata 均被验证。

### 任务 4：实现解析器和自动测试

- 从 Combine exporter 复用结构化字段解析和 rank 选择规则；
- 增加 role-aware thread、metrics 和 diagnostics；
- 用 synthetic 64P 覆盖不同 blockDim、sender/count 分支、缺失 rank/core、旧 generation 和非单调时间戳。

验收：三个 JSON 可由标准 JSON parser 读取，不包含不受支持的 `displayTimeUnit=us`，选择出的 fastest/P50/slowest 与构造数据一致。

### 任务 5：接入 64P breakdown 模式

- 扩展统一 64P/Node 脚本；
- 构建、部署、执行、日志收集和 trace 导出保持单一脚本入口；
- profiling 后另做 profiling OFF A/B，不混用同一 binary 结果。

验收：64 rank 均有完整记录并生成三个 trace；输出明确给出 rank、总耗时和 JSON 路径。

## 验证策略

### 不占 NPU 的验证

- C++ `static_assert`：record=512B、容量和 buffer index；
- source contract：编译期开关、地址、打点边界、无新增 barrier；
- CANN A5 profiling ON/OFF 各编译一次；
- EP CTest；
- synthetic parser 测试和三个 JSON schema 检查；
- profiling OFF binary 的反汇编/字符串检查仅作为可选补充，不作为主要门槛。

### 硬件验证

先 1P/8P 确认记录和角色，再运行主 64P case。需要分别保存：

- profiling OFF 的 Dispatch 平均/P50/P95 性能；
- profiling ON 的最终 iteration 分解；
- fastest/P50/slowest rank JSON；
- 每个选中 rank 的最慢 sender core、最慢 count/cumsum core 和最慢 local-copy core；
- status/payload polling attempts/misses 与对应 metric；
- `send_buffer_num`、`max_copy_token_count`、receive batch/token 数。

profiling ON/OFF 使用相同输入、balance expert 分布、warmup、loop、rank 拓扑和通信域创建方式。

## 风险和控制

1. `GetSystemCycle` 和循环 metric 会扰动小粒度循环：仅在 profiling 构建存在，生产性能用 OFF 结果。
2. sender/count 分支点不同：固定 schema、缺失点回填和 `coreRole` 消除解析歧义。
3. profile GM 写影响 Kernel 尾部：`FINAL_END` 早于 `WriteProfile`，GM 发布不计入拆解。
4. Combine 当前仍在 buffer 0：Dispatch 实施前必须先完成 Combine buffer 1 的 Kernel/collector 同步切换，否则仍会覆盖。
5. 64P blockDim 不一致：按 rank 自校验，不要求跨 rank 相同，最大值仍限制 200。
6. Chrome Trace 单位兼容：不输出 `displayTimeUnit=us`，并用自动测试锁定。
7. 轮询 metric 可能放大 polling 开销：time point 负责可信的阶段墙钟耗时，细分 metric 只用于定位，不替代 profiling OFF 性能。

## 完成判定

完成实现必须同时满足：

- Dispatch 固定写 buffer 0，Combine 固定写 buffer 1；
- profiling OFF 不增加 Kernel 热路径操作；
- sender、count/cumsum、local-copy 三段均可在 per-core trace 中辨识；
- 记录、日志和解析器 schema 一致且 generation 可验证；
- 64P 输出 fastest/P50/slowest 三个可导入 Chrome Trace 的 JSON；
- profiling OFF A/B 性能结果与 profiling ON 拆解结果分开报告。
