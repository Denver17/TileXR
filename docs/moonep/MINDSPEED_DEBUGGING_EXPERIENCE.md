# TileXR MoonEP 接入 MindSpeed 调试经验

本文记录 2026-08-10 至 2026-08-12 在单机 8 卡、4K/8P/EP8 模型中调试
TileXR MoonEP 接入 MindSpeed 的问题、证据链和可复用方法。后续调试应把本文作为
排查入口，但不能把历史根因直接套到新故障上；必须先用当前源码、二进制和运行日志
重新确认第一个失败边界。

## 最终结论

这次适配不是一个单点故障，而是模型规模和完整前反向流程依次暴露了五类契约问题：

1. 通信 runtime 和 RA 的所有权不唯一。
2. MindSpeed 依赖的 Buffer、Tensor view 和 zero-copy 契约超出了 MoonEP 公共 API。
3. 逻辑 worker 数、物理 QP 数和 AIV block 数被错误地绑定到同一固定上限。
4. CQE、SQ tail、ring index 和 cycle bit 的语义没有明确区分。
5. 同一个 plan 跨 Planner、Dispatch、Prefetch、Combine、反向 Dispatch 和 ReduceGrad
   复用，但 status 的输入、输出和清理责任没有形成完整状态机。

最后阻塞 grouped-URMA Dispatch + Combine V2 完整模型的直接原因，是前向 Combine
成功后在复用 plan 中留下 `status=3000`。反向 URMA Dispatch 要求输入状态为 `0`，
并通过 `CAS(0, error)` 发布首错误。旧状态同时导致正常发送路径被跳过，并阻止真实
错误码写入，最终表现为反向 Dispatch 等待 flag 超时。

Python 侧的 `plan.status.zero_()` 不是可靠修复：NPU task queue 和 Kernel launch 可能
位于不同 stream，无法保证 reset 先于 consumer Kernel。最终方案是通过
`TILEXR_MOONEP_FLAG_RESET_STATUS`，由 Host 在 Dispatch 的同一 stream 上排队
`aclrtMemsetAsync`，然后紧邻 Kernel launch。

## 已确认问题

| 现象 | 根因 | 修复原则 | 发现方式 |
|---|---|---|---|
| `RaInit=328002`，UDMA 注册继发 `-7` | SHMEM/HCCL 和 TileXR 重复拥有 RA；rank 间 owned/attached 状态可能不一致 | 通信 runtime 只能有一个 owner；允许显式 attach，并跨 rank 校验所有权 | TileXR-only、SHMEM -> TileXR、TileXR -> SHMEM 三组初始化顺序 A/B |
| 替换 `moonep.Buffer` 后模型仍失败 | MindSpeed 依赖 `_ctx`、token/route buffers、packed Prefetch/Reduce、caller buffer 等私有契约 | 用适配层显式桥接，不能只替换公共类名 | 从 traceback 沿 caller/adapter/native 三层逐项核对对象契约 |
| 权重被 `storage_offset != 0` 拒绝 | 权重是连续 arena 的合法 subview，随后还会复制到独立注册 backing | 区分 source view 和 registered backing；只在立即复制边界允许 offset | 打印 storage、data pointer、offset 和注册对象身份 |
| backward 要求 SHMEM view | zero-copy alias 缺少 owner、generation 和 plan 元数据 | 地址别名必须携带所有权和生命周期标签 | 验证 alias storage 相同，并校验 owner/plan/generation |
| Combine V1 `DecodeRoute=3002` | 4 字节 `DataCopyPad` 标量读取返回陈旧 UB 数据 | GM 小标量使用当前 CANN 版本实机验证过的读取模式 | 逐分支状态码加设备侧原值捕获；立即在 Combine 后同步状态 |
| PrefetchWeight 在 32 QP 返回 `-3` | 把物理 `qpNum` 当作 worker 数，只接受 1/2/4/8 | `workerCount=min(qpNum, maxWorkers)`，Kernel 仍接收完整 `qpNum` | 查询真实 QP 数并对照 Host 参数校验分支 |
| ReduceGrad workspace query 返回 `-1` | Host/Kernel QP 上限固定为 8，实机为 32 | Host、Kernel 和测试统一支持目标硬件 QP 上限 | 1/2/4/8/32 QP 参数矩阵 |
| ReduceGrad 第二轮 CQ 失败，`entryIdx=0x4000` | `entryIdx` 携带 SQ cycle，原实现错误地要求它小于 ring depth | 先按 depth 归一化，再依据绝对 SQ tail 计算完成 BB 数 | 两轮同 QP 精确复现；dump raw CQE、SQ head/tail、outstanding |
| Combine 后复用 plan 的反向 Dispatch 超时 | 旧 `status=3000` 违反 URMA 输入协议，异步 Python reset 又存在跨 stream 竞态 | consumer Host 在同一 stream reset；先检查旧首错误，不能掩盖真实失败 | 生产规模 oracle 对比不清理、异步清理、同步清理和 Host same-stream reset |
| 4K grouped Dispatch 超时或 SQ 满 | 全量 route 无法一次装入 UB，WQE 也不能一次塞入 SQ | route tiling、WQE 分批发布、每批 CQ 回收，以 `head-tail` 计算 outstanding | case 15 生产规模单算子和 H=7168 grouped oracle |
| 双机 ReduceGrad prepare 返回 `-4`，HCCP 为 `528101` | packed adapter 的占位 Up source 只有 2 KiB，逻辑 view 同时被当作 MR 注册范围 | 保持 source shape/bytes 不变，为 view 提供实机验证过的 2 MiB backing，并在 FFI 中分别描述逻辑 source 与 registration storage | 双机日志定位失败 region/bytes/返回码；两机 NPU probe 证明 source 2 KiB、MR 2 MiB；2 机 16 卡完整模型 8/8 迭代通过 |

“重复 MR 注册泄漏”“完全没有 poll CQ”“peer 调度不对称”都曾是合理假设，但被后续
A/B、原始队列状态和成功对照推翻，不应继续作为既定根因传播。

## 关键测试如何定位根因

### 1. 初始化顺序 A/B

分别运行 TileXR-only、SHMEM -> TileXR 和 TileXR -> SHMEM。结果显示失败随初始化顺序
变化，把第一个失败边界从模型逻辑缩小到 RA 所有权，而不是 UDMA 数据面。

### 2. 两轮 ReduceGrad 精确复现

第一轮全部通过，第二轮仅部分 rank 失败。raw CQE 显示 `entryIdx=16384`、ring-local
tail 为 0、outstanding 为 1。16384 恰好是一个 SQ cycle，证明错误来自 cycle 归一化，
而不是超时长度、WQE 地址或 MR 注册。加入 modulo-depth 修复后，两轮 16/16 rank-round
通过；扩大 WQE 数量后仍通过。

### 3. 生产规模 grouped-URMA oracle

oracle 使用以下真实模型特征，而不是缩小到会绕开问题的 toy shape：

- 单机 8 卡、8 rank；
- `S=4096`、`K=8`、`H=7168`、EP8、`NvS=32768`；
- grouped-URMA，group width 16；
- model-skew 路由和 route weights；
- PrefetchWeight、Combine V2；
- 连续创建 5 个额外 plan 后复用最后一个 plan；
- 所有 rank 对 Dispatch 和 Combine 输出做 exact comparison。

第一次 Dispatch 和 Combine 正确，而 Combine 后复用 plan 的 Dispatch 失败。A/B 显示：

- 保留 `status=3000`：失败；
- Python `zero_()` 不同步：不稳定；
- Python `zero_()` 后显式同步：通过；
- Host 在 Dispatch stream 上 reset：稳定通过。

该测试把根因从宽泛的“反向 flag 超时”缩小为 plan status 的跨阶段和跨 stream 生命周期
错误。它同时验证了 4K route tiling、WQE 分批和 CQ 回收，因此是完整模型前最重要的
root-cause oracle。

### 4. 完整模型与性能复测

修复后按以下顺序扩大验证范围：

1. case 15：4K/EP8 grouped-URMA Dispatch 单算子复测。
2. grouped oracle：H=7168、model-skew、Prefetch、Combine V2、5 个额外 plan 和 plan reuse，
   8 rank exact comparison。
3. 完整正确性模型 `tilexr_urma_correctness_4k_8p_ep8_0812_015220`：8/8 迭代通过。
4. 当时关闭 DFX、trace、dump 和 profiler 后运行
   `tilexr_urma_perf_v2_4k_8p_ep8_0812_095543`：8/8 迭代、退出码 0、无 skip/NaN。

该条是历史实验身份，不是当前性能基线。当前 MoonEP 模型/replay 对比必须保留框架 NPU
profiler 和 Dispatch/Combine stage barrier，并保证两侧状态一致。

完整模型通过只能证明该组合路径闭环，不能替代单算子对边界语义的证明；单算子通过也
不能证明 plan reuse、前反向和多轮资源复用。

## 推荐调试流程

### 0. 复用一次性 HCCL 环境基线

官方 HCCL Test 是环境准入证据，不是每个任务或每次修改的回归测试。同一环境最多执行
一次；开始前必须先查找已有记录，存在记录时禁止再次运行。环境身份由 host/container、
device subset、topology、accelerator mode、CANN、driver、firmware 和 HCCL Test build
共同确定；重编译 TileXR、重启进程或开始新的调试任务都不构成新环境。

首次运行应选择覆盖当前失败数据路径的 mode，并保存完整命令、环境身份、逐 rank 正确性
输出和有限日志。后续任务直接复用该结果；如果已有运行没有覆盖当前 device subset、
topology 或 accelerator mode，应把该路径标记为基线未验证，而不是在同一环境补跑第二次。
一次性运行失败后，停止算子侧排查，等待环境发生可证明的实质修复或更换新环境。

### 1. 固定运行身份

每次运行先保存以下信息：

- commit、工作树 diff 和源码哈希；
- 实际加载的 `.so`、AICore binary 路径和哈希；
- CANN、驱动、固件、SoC 和 conda 环境；
- launcher、环境变量、rank/device 映射；
- 运行前后的 NPU PID、stdout、stderr、plog 和输出目录。

构建成功不等于运行时加载了新库。没有二进制 provenance 的通过或失败结果都不能作为
最终证据。

### 2. 找第一个失败边界

按 Planner -> Dispatch -> Prefetch -> Combine -> reused Dispatch -> ReduceGrad 顺序，在
每个异步 stage 后只增加一个必要同步点。记录该点的 Host 返回码、`plan.status`、DFX
首错误和相关队列状态。后续超时往往只是更早的异步错误在同步点被暴露。

### 3. 一次验证一个可证伪假设

写清楚：

> X 导致症状 Y；如果成立，只改变 Z 后结果应从 A 变成 B。

优先改变初始化顺序、transport、是否 Prefetch、是否 Combine、是否复用 plan、是否同步
reset 等单一变量。一次同时修改 Kernel、Host、timeout 和路由，会使任何成功都无法归因。

### 4. 从协议状态入手，而不是先加 timeout

超时时至少采集：

- plan status 的阶段来源和预期值；
- flag 矩阵在 producer 前后、consumer 前后和超时后的差异；
- SQ/CQ head、tail、depth、owner、`entryIdx`、outstanding；
- 第一个失败 peer、QP、phase 和原始错误码。

延长 timeout 只有在状态持续前进但速度不足时才有意义。状态完全不变、已进入错误分支或
队列字段非法时，延长 timeout 只会降低调试效率。

### 5. 修复协议拥有者

- 跨 stream reset：由 consumer Host 在目标 stream 上排队。
- 跨 rank runtime：明确 owned/attached，并做一致性校验。
- 错误发布：使用 first-error/CAS，禁止后续 core 覆盖首错误。
- ring 字段：在类型、命名和测试中区分 absolute counter、ring index 和 cycle bit。
- Tensor：区分 source view、copy destination 和 registered backing。

### 6. 逐级扩大验证

推荐门禁顺序：

1. Host/unit/mock：参数、状态转换、边界算术。
2. 单 rank 或最小多 rank：验证 ABI 和第一条真实数据路径。
3. 两轮以上真实 UDMA：验证 QP、MR、SQ/CQ 和 magic 复用。
4. case 15：生产 route 数和 grouped-URMA 流控。
5. grouped oracle：model-skew、Prefetch、Combine、额外 plan、plan reuse、exact comparison。
6. 完整 4K/8P/EP8 前反向模型。
7. 关闭所有 DFX 后的独占性能运行。

## 必须长期保留的回归维度

- forward 和 backward 都执行；
- 同一个 plan 在 Combine 后被 Dispatch 复用；
- 至少两轮 MR、QP、SQ/CQ 和 magic 复用；
- 独立 Memory 通信域与 UDMA 域融合时，至少覆盖三个连续 magic，并包含本轮未覆盖的
  route target，确认旧 record 不会泄漏到输出；
- route 数覆盖 UB tile 和 SQ depth 边界；
- QP 数覆盖 1/2/4/8/32/48；
- SQ 位置覆盖 `depth-1`、`depth`、`depth+1`、`2*depth`；
- 同时覆盖 1-BB 和多 BB WQE；
- balanced、model-skew、sparse 和 unique routing；
- `S=4096`、`K=8`、`H=7168`、EP8 的生产规模；
- 正确性运行可按需开启失败时 DFX。性能基线关闭 trace、dump、Kernel 编译期 DFX/profile、
  调试同步和 framework prewarm；框架 NPU profiler 与 Dispatch/Combine stage barrier 必须
  保留，并在模型和 replay 两侧保持一致。报告中同时记录两者状态。

性能复测不能只检查运行时环境变量。Dispatch DFX 和部分 profiling 是编译期 CMake
选项；即使 provenance 显示 trace/dump/profile 都关闭，已嵌入 `.so` 的 AICore binary
仍可能包含 DFX 路径。性能运行前必须同时保存并核对 CMake cache、实际加载库哈希和
运行时环境，普通 Release 构建应默认关闭 DFX，需要诊断时再显式开启并重新构建。
框架 NPU profiler 与 Kernel 编译期 DFX/profiling 必须分开记录：前者可用于算子计时，
后者会改变被测 Kernel 路径，不能在默认性能构建中开启。

## 避免重复踩坑

- 不要把 error code 的字面含义直接当根因；查调用边界和相邻日志。
- 不要在第一次成功后停止，队列 cycle 和资源复用问题通常第二轮才出现。
- 不要用 Python 异步 tensor 操作实现 Host/Kernel 协议同步。
- 不要把 worker、QP、AIV block 和 rank 数视为同一种并行度。
- 不要用 toy shape 证明生产规模的 UB/SQ 容量安全。
- 多 rank 功能门禁要同时记录全局 shape 和每 peer 负载。2026-08-20 的 8P A/B 中，
  `bs=8192, K=16` 的 hidden-only 与 fused 路径都在每 peer 16384 行时触发相同的 Fullmesh
  中间 CQ 非法状态，而已验证的 `bs=128` hidden-only 和 fused 路径均通过。融合功能门禁
  应使用已验证 shape；大 per-peer Fullmesh 回收边界必须作为独立问题验证，不能归因于
  weight Memory 路径，也不能用小 shape 的通过覆盖。
- 不要依赖已消费 SQE 的内容推导 CQ completion。
- 不要在缺少跨 rank 初始化屏障时用“接收区清零”隔离轮次；远端 step 0 写入会与本地清零
  竞争。完整 generation 与 payload 必须写入同一条 record，并在完成 token 后按 generation
  过滤输出；若相邻 launch 可重叠，还必须为 record 和完成 token 使用双 epoch 物理区，避免
  N+1 覆盖慢 rank 尚未消费的 N 数据。
- 不要在调试运行和性能运行之间复用未审计的环境变量。
- 多机 HCCL rank table 与 TileXR UDMA RootInfo 是两种不同配置：前者描述全局
  rank/topology，后者描述每台主机的本地 UDMA EID/port。不能用一个文件替代另一个；
  应分别保存路径、哈希和 parser/init 证据。
- 多机 launcher 全部退出 0 只证明进程完成，不证明模型数值正确。至少同时门禁完整迭代数、
  最终有限 loss、全程有限 gradient norm、skip/NaN 计数、profiler artifact 数和退出后 idle。
- 每次实机 A/B 必须同时保存源码 commit/diff 与实际加载 `.so` 路径和哈希。源码同步成功
  不代表远端安装树已重编译；如果重新构建后行为变化而代码修复尚未被单变量证明，应明确
  记录为 binary-provenance 问题，不能把诊断插桩本身误报成根因修复。
- 不要把小 Tensor view 的逻辑字节数等同于 HCCP MR 范围；扩大 backing storage 时保持逻辑 shape 不变，否则会污染算子工作量和性能数据。
- 不要因某次补丁通过完整模型就跳过最小 reproducer；最小 reproducer 才能证明因果。
- 不要删除被推翻的假设记录。保留否定证据可以防止后续重复猜测。
- 多阶段共享普通 UDMA 注册时，不能在热路径中按 stage 反复切换单一 active MR；不同
  rank 的 stage 到达顺序可能不同，使无标签注册 collective 串轮。可预测的工作区应合并
  到一个持久注册 arena，算子继续使用各自逻辑子区间；子区间激活必须命中已有 MR，不能
  再发起注册 collective。扩大 arena 后仍要分别验证初始化内存峰值和至少两轮模型数值。
- Combine V2 的 shared-QP/fullmesh 路径还依赖 credit IPC。`extraFlag` 中同时存在 UDMA、
  `UDMA_SHARED_QP` 和 `UDMA_FULLMESH`，以及 UDMA 初始化成功日志，都不能证明
  `CommArgs::creditMems[]` 已初始化。若 `TileXRMoonEpCombineStageV2Fused` 在 launch 前
  同步返回 `-6`，先用 Host DFX 或最小 A/B 检查 `creditMems[]`；单独
  `TILEXR_ENABLE_CREDIT_IPC=1` 使同一 case 从失败变为通过时，应把缺省环境修在 launcher/
  model runner，而不是修改 Combine Kernel。
- shared-QP 只共享物理 QP 编号规划，不会把不同 peer 合并到同一套 SQ/CQ context。
  `UDMAGetWQCtx` 和 `UDMAGetSCQCtx` 均按 `(peer, physicalQp)` 索引。独立 credit QP
  必须为每个 peer 分别维护和回收 head、tail、cqTail；可以延迟 CQ 回收以覆盖 payload
  准备，但必须在复用 credit source 或算子返回前逐 peer drain。
- `npu-smi info` 在 950 上可能显示每卡无进程，但 HCCL Test 或其他加速器工作负载仍在
  使用设备。多机 idle gate 必须同时检查设备节点 owner 和已知加速器进程；发现外部作业
  只判 busy，不终止。连续模型启动后的 `EI0007/halSqCqAllocate` 表示驱动 stream/SQ-CQ
  资源尚未回收，应等待并用最小 stream 探针确认恢复，不能改算子代码规避。
- idle gate 识别已知加速器进程时不能用 `pgrep -f` 扫描任意命令文本；远端诊断 shell、
  监控 agent 或用户提示中包含 `pretrain_gpt.py` 也会造成所有节点长期假 busy。应按真实
  `comm`、Python entry/module 和 `/proc/<pid>/exe` 分类，并与 `npu-smi` 和设备节点 owner
  交叉验证。通过 PowerShell 发远端命令时也不能把 `$(...)` 放在双引号参数中，否则会先在
  Windows 本地展开，污染所记录的远端身份。
- 反过来，脚本级 `probe_idle` 返回 `8/8` 也不能单独证明机器可用于大模型。2026-08-20
  的 2 机 16 rank model-replay 调试中，`.195` 的 idle gate 连续通过，但 `npu-smi info`
  显示每卡仍有外部 `sglangschedul` 进程占用约 102 GiB HBM；TileXR 模型在首次 Planner
  附近表现为跨节点 peer 超时，开启 stage barrier 后进一步暴露为每卡追加 11.6 GiB 分配
  失败。多机模型定位前应同时保存 idle gate、`npu-smi` HBM/进程表和已知加速器进程检查；
  发现外部作业时只记录 busy 并更换/等待机器，不要修改 MoonEP kernel 或清理非本任务进程。
- `probe_idle=8/8` 只证明没有检测到 owner，也不能覆盖设备运行时健康。2026-08-21 的
  32-rank 启动中，多台机器各有一张卡显示 `Critical`、零功耗和零 HBM，且无进程或设备节点
  owner；对应 rank 和独立 B131 探针都在 `torch.npu.set_device` 返回 `507033/TsdOpen failed`。
  多节点启动前应对全部入选设备同时检查 health，并至少完成一次逐设备 `set_device` 探针；
  `Alarm` 按项目规则仍可用，但 `Critical` 且探针失败必须判为环境边界，停止算子侧定位并等待
  修复或更换机器，不能以重试 Kernel、放宽 idle gate 或重置共享设备规避。

## Combine V2 共享 scratch 的 completion 约束

ring 调度可以把发送工作分给不同 AIV core，但不能据此把接收完成条件也按 core
分片。每个 core 的 reduction 都会读取包含所有 source 写入的共享 scratch，因此每个
consumer core 必须在 reduction 前观察全部 source 和 lane 的 Done token；只等待本 core
负责发送的 source 会在远端写仍在进行时提前读取，表现为偶发少聚合，而不是稳定超时。

本地 Self copy 也必须进入同一 completion 协议。只有在 Self 的最后一笔 MTE3 写完成后
才能发布本地 Done，然后才能发布 step grant。不能把 `source == rank` 直接视为 ready，
否则其他 core 仍可能早于本地 copy 完成开始 reduction。

不要用 launch-wide barrier 修复这个问题。Host 的 launch block 数可能大于运行时 active
block 数，inactive block 会提前返回，`SyncAll` 的参与者集合因而无法收敛。应使用现有
magic/epoch/step 编码的 GM Done token 表达真实 producer-consumer 依赖。验证至少覆盖：

- 非 2 次幂 rank 的轮询游标，不能用位与代替取模；
- route weights、Prefetch、额外 plan、registration 切换和 plan reuse；
- 所有 rank 的 Dispatch/Combine hidden 与 weight 逐元素比较；
- 完整前反向模型的有限 loss/gradient，而不能只看进程退出码。

2026-08-13 的根因 oracle 使用单机 8 rank、S=4096、K=8、H=7168、model-skew
路由。修复后 5 轮严格 oracle 全 rank exact，随后单机 8 卡和双机 16 卡完整模型均达到
8/8 且 loss/gradient 有限。该结果只证明已测试的 Ascend950PR、B131 CANN 和对应拓扑；
其他硬件、rank 规模和 topology 仍需按相同测试阶梯验证。

## Dispatch V2 fused epoch 约束与验证边界

一次 paired `TileXRMoonEpDispatchV2` 应只有一个 magic、一次 AICore launch 和
一个 UDMA epoch。Hidden 与 RouteWeight 必须使用互不重叠的 source 和双 scratch；
同一 route 在同一逻辑 QP 上按 Hidden、Weight 顺序发 WQE，ordered completion 必须
排在该 QP 的全部 payload WQE 之后。completion、group credit、CQ reclaim 和 final
quiet 每个 fused epoch 只执行一轮。发生上游或设备错误后可以停止 payload work，但
不能跳过 signal-only completion、incoming wait/credit 和最终 status/quiet 收敛，否则
健康 rank 会退化为超时发现错误。

诊断仍保留独立 Hidden/Weight Profile 与 DFX，但共享阶段只能有一个 owner。paired
以 Weight record 承载 route scan、flag wait、credit、CQ 和 quiet；Hidden record 的共享
耗时为零，并由 kernel status 的 fused feature bit 明确标识。Kernel 编译期 profiling OFF
与 ON 的运行必须分开，且两者都保留框架 NPU profiler；不能把两个 payload record 伪装成
两轮独立通信。

实机验证时还要注意以下边界：

- Host 环境值是 `group_credit`，不是 `group-credit`；错误拼写会在 launch 前返回 `-3`。
- grouped/group-credit 只适用于 vector route selection。小于 64 routes、非 2 的幂
  `NvS` 或 UB/vector 不满足条件的 shape 应使用 legacy scalar-tiled 路径；同步返回
  `-6` 不是 UDMA 数据面失败。
- padded zero-fill 要验证完整输出 tensor，不能只比较有效 slot。若 shape 不满足 grouped
  条件，用 legacy 路径证明 Hidden 和 Weight 的空 slot 都为零。
- shared-QP 不能仅凭配置名推断。应同时确认 runtime 调用 shared-QP-domain 初始化、日志
  显示 domain 启用，且 communicator `extraFlag` 含 UDMA 与 `UDMA_SHARED_QP`；本次
  Ascend950PR 单机 8 rank 证据对应固定 48 QP shared domain。
- 参考形状 `S=128,K=16,H=3584,NvS=2048` 的不重叠布局仍为 30 MiB；其他 shape
  必须使用 checked add/multiply 计算真实容量，并在扩大 workspace 绑定后重新检查全部
  active region 与 common tail 边界。

这些 Host/mock/source guard 只能证明 ABI、布局和源码协议不变量。只有相同 CANN、设备、
拓扑上的 HCCL baseline 和 Ascend950 实机逐元素多轮结果，才能证明 UDMA 数据面；性能结论
还必须使用同 shape、同 pair 模式、同 warmup/迭代和独立 Kernel profiling-off 构建做 A/B；
框架 NPU profiler 与 stage barrier 仍按项目规则保留。

## 8K/K16/H3584/EP8 模型 replay 调试证据边界

2026-08-16 至 2026-08-19 在 `S=8192,K=16,H=3584,EP=8,R=8` 单机 8 rank
模型 replay/cache 路径上，问题先后暴露在 Prefetch、Combine V2 和反向 Dispatch
边界。该规模截至记录时没有形成可交付修复；以下内容只作为后续定位经验，不能把临时
诊断补丁当作已验证方案保留。

保留的运行身份和边界证据：

- 目标机器为 `141.61.49.223` 单机 8 卡，CANN `/home/pkg/b131/cann-9.1.0`，
  conda 环境 `ai_moe_test`，TileXR 安装树
  `/home/c30061605/ai/TileXR-model-replay-cache-stage-20260817`。
- `model-replay-cq-owner-8k-ep8-20260819-run1` 中，epoch-1 Prefetch 全 rank
  返回 `4000`；rank0 Combine V2 在 `InitLaneStates()` 报 `invalid_config(1)`，
  `core=5 peer=5 lane=0 qp=5`，`expected=34657 observed=34656`，
  `cq_status=0x80000000`。CQE owner 已 ready，entryIdx 等于旧 tail，说明残留 SQ
  entry 已完成但没有被回收，而不是仍在飞行。
- `model-replay-dispatch-status-8k-ep8-20260819-retry1` 没有进入 MoonEP 边界，
  训练开始前 HCCL allreduce/stream setup 报 `halSqCqAllocate drvRetCode=17`，
  之后最小 NPU stream probe 和 8/8 idle gate 通过；该结果应按环境资源瞬态处理，
  不能归因到算子。
- `model-replay-dispatch-status-8k-ep8-20260819-retry2` 进入 MoonEP 后，epoch1
  全 rank Dispatch 为 `0`、Prefetch/Combine 为 `4000`，但诊断读取的 Combine V2
  failure-record 区域同时出现 `outstanding_limit(3)`、`done_timeout(7)` 记录。这说明
  failure-record 读取必须先证明当前 launch、magic、收敛轮次和成功路径清理语义，不能把
  旧记录或非最终记录直接当作首失败。
- 同一 `retry2` 后续 epoch 出现 `dispatch_forward_complete status=2004`，
  Prefetch `0xd700ffff/0xd708ffff`，Combine V2 记录中多处
  `invalid_config(1)` 且 `cq_status=0x80000000`；部分记录的
  `expected - observed = 16384`，正好是一个 SQ depth/cycle。最终反向 Dispatch 在
  `_check_plan_status` 处显式失败，如 rank4/rank5：
  `actual -687800321, expected 4000`。因此该次模型已从“卡死”收敛为
  “共享 QP/CQ completion 回收或诊断记录生命周期”问题。

已被证据约束或否定的方向：

- 单纯增加 timeout 没有意义；多次日志显示状态已经进入错误分支或队列字段不满足协议。
- route 内容不是充分条件。使用 call0/call1 真实捕获路由拼出的 55-stage case19 replay
  曾在 8 rank 全部通过，说明完整模型调度、跨 stage 资源复用或共享队列生命周期仍需单独
  证明。
- HCCL stage barrier A/B 对 TileXR backend 不可靠：当 barrier 仍走
  `torch.distributed.barrier()` 时，可能在 MoonEP 边界前卡在 HCCL collective，不能用于
  证明 TileXR stage skew。
- failure-record 诊断只能作为定位工具。若成功状态和 failure-record 同时出现，下一步应先
  验证记录清理、magic 选择和 converged-success 语义，再决定是否改 Kernel 协议。

后续若恢复该规模，应从最小可证伪边界继续：先用只读诊断确认 Combine V2
`PublishFailureAndConverge()` 成功路径是否会留下可被下一轮误读的 poison/marker，再确认
共享 QP 的 SQ head/tail、CQ tail、entryIdx、owner/cycle 与 generic helper 的回收口径是否
一致。不要在未证明归属前同时修改 Prefetch、Combine 和 Dispatch。

## 4K/K8/H7168/EP8 model replay 共享队列 frontier 经验

2026-08-20 在 `S=4096,K=8,H=7168,EP=8,R=8` 单机 8 rank model replay 级联中，
故障从 PrefetchWeight CQ invalid 推进到首次 ReduceGrad：`epoch 6` 的
`reduce_grad_status` 返回 `1/3`，而相同 shape 的 ReduceGrad 单算子 correctness 通过。
这说明 ReduceGrad 自身基本路径健康，级联前序 stage 留下的共享 QP/SQ/CQ 状态才是首要
边界。

本次根因是 profile UDMA helper 仍把 `wqeCntAddr` 当作 completion frontier 传给
`UDMAPollCQ()`。在 shared-QP 中，前序 Dispatch/Combine 可能提交不产生 CQE 的 WQE；
`wqeCntAddr` 会累计这些 WQE，但 CQ tail 只按实际 CQE 前进。后续 ReduceGrad READ 只产生
一个 CQE 时，应按该 CQE 的 `entryIdx` 推进 SQ tail，跨过历史无 CQE WQE，而不是等待
`wqeCntAddr - cqTail` 个 CQE。修复后：

- `UDMAProfileCompletionFrontier()` 返回 SQ `headAddr`；
- `UDMAProfileQuietStatusOnQpUntil()` 使用 SQ-tail frontier，通过 CQE `entryIdx` 推进
  SQ tail；
- mock 覆盖“历史无 CQE SQ gap + 一个 READ CQE”场景；
- `.223` 上 ReduceGrad 单算子 correctness 通过，8rank 级联 replay 通过，输出目录为
  `/home/c30061605/ai/TileXR/run/moonep/case17-cascade-sqfrontier-parser-20260820-080256`。

同一轮还暴露了 profiler 解析边界：当前 CombineV2 replay 的实际 kernel 数是 15
（10 次 forward combine + 5 次 backward combine），旧解析器只接受历史 20 次
（backward 双 launch）形态。性能解析应以实际 launch 形态分支处理，不能把 kernel
count mismatch 误判为算子失败。

## 双机 16 rank reverse Dispatch 与 rendezvous 端口经验

2026-08-21 在 C02-A04 两台 Ascend950DT、B131、
`S=4096,K=8,H=7168,EP=16,R=16` model replay 中，group 模式的模型在
ReduceGrad 调用前报告 `epoch 6: actual 2007, expected 0`。该异常由
`check_pending_status()` 暴露，真正 owner 是前序 reverse Dispatch，而不是抛错位置的
ReduceGrad。Dispatch DFX 进一步记录 `flags=0x80` 和 `quiet=0xfffffffc`；后者来自 final
SQ drain 观察到 `pending >= TILEXR_UDMA_SQ_BB_COUNT`。因此 grouped shared-QP 的 SQ
completion frontier 是未闭环边界，不能通过改 ReduceGrad、扩大 buffer 或继续增加
Planner timeout 来规避。

只改变 `TILEXR_MOONEP_DISPATCH_PEER_MODE=legacy` 后，两节点模型均退出 0；使用同一份
真实路由缓存的 16 rank 级联也全部通过并完成全局汇总。短期 launcher fallback 应同时
作用于模型采集和 replay，显式 peer-mode 环境仍优先。该结果没有证明 grouped Kernel
已修复，也没有覆盖 64/128 rank。

同一次验证还暴露了多通信域端口约束。UDMA communicator 使用 `TILEXR_COMM_ID` 时，
memory-only communicator 会使用相邻端口；把 `TILEXR_MOONEP_BARRIER_ADDR` 手工设为
communicator port + 1，会使所有算子和 profiler 都完成，但多数 rank 卡在最终 Host
rendezvous。barrier 必须通过 `tools.moonep.rendezvous.offset_host_port()` 生成；当前标准
偏移为 113。修正后 16/16 rank 生成结果和 node completion 标记，controller 汇总退出 0。
多节点一键 launcher 还必须在所有节点共享 `TILEXR_COMM_ID`、barrier address、launch ID
和 HMAC secret；只设置 launch ID 不足以进入 worker。

在 C02-A04 的 `.150/.83` 上，node0-managed launcher 已用同一缓存完成一次 16-rank 实机
闭环：node0 为 `hit`、远端自动拉起节点为 `follower-hit`，两个 node completion 标记和
16 个 rank 结果均完成，最终全局汇总退出 0。该验证没有手工注入 communicator、barrier、
secret、peer mode、Planner wait 或 follower 命令；只覆盖 B131 和该两节点拓扑，不能外推
到 64/128 rank。

## 多节点 model replay cache 发布与部署闭包

2026-08-21 在 `141.61.49.223/.192`、B131、
`S=4096,K=8,H=7168,EP=16,R=16` 验证中，leader 已完成两节点模型采集并把
generation 同步到 follower，但 follower 一直停在 `wait_for_replay_cache()`，导致 leader
仅启动 ranks 0-7 并等待不存在的 ranks 8-15。只读诊断证明两台机器的
`/usr/local/Ascend/driver/version.info` 哈希不同：leader cache key 为 `81c79c...`，
follower 本地重算为 `846def...`。generation、completion marker 和 artifact 校验本身均
完整，首失败边界是多节点 cache handoff，而不是级联 Kernel 或通信数据面。

多节点 managed launcher 必须把 leader 已发布的 generation 路径显式传给 follower。
follower 应校验路径位于 cache root、目录 key 与 manifest 一致、artifact 完整，并比较
shape、runner、adapter、Kernel 模式和执行控制等共享契约；`driver/firmware/soc` 这类
主机局部 provenance 不应参与 follower 对 leader generation 的重新寻址。不能改成扫描
“最新 generation”或放宽全部 identity 校验，否则可能复用错误 shape 或二进制的输入。
显式设置的 Dispatch peer mode 也必须进入 provenance；实际 `legacy` 不能按 shape
默认值误记成 `group`。

修复后 `.192` 单机 8 rank 完成两轮模型、8/8 profiler 和 55-stage replay；`.223/.192`
双机 16 rank 生成两个 node completion、16 个 rank 结果并通过全局汇总。该结论仅覆盖
上述主机、B131、Ascend950PR 和 `legacy` Dispatch peer mode。

同一轮首次双机启动还暴露了部署闭包问题：模型目录中的 `MindSpeed`、
`MindSpeed-LLM` 和 `shmem` 是指向 `temp/moonep_mindspeed` 的绝对 symlink，只同步链接本身
会在远端得到 `Required path not found`。部署检查必须解析绝对链接并同步其真实目标目录；
Linux 间使用 `rsync -a` 保留链接语义，但不能把“链接存在”当作依赖已闭包，也不要使用
`--delete`。

## model replay meta 的跨机器兼容性边界

可上库的 replay meta 必须把路由可复用性和性能可比性分开判断。同一 `S/K/H/EP/R/E/Hf/P`、
55-call 契约、adapter/runner、model stack 和 Kernel 契约下，TopK 路由可以在另一台同规模
机器上解压并重建 runtime cache，不应因为主机名、IP、driver 文件哈希或拓扑地址变化而重新
跑模型。相反，历史性能只有在 CANN、driver、firmware、SoC、拓扑、rank mapping、profiler
和 stage barrier 等完整 provenance 一致时才是 direct baseline；否则必须标成
`checked-in reference`。不要为了复用路由而放宽性能标签，也不要为了性能 provenance 的
主机差异而阻止路由 replay。

Dispatch 的 `legacy`、`group` 和 `group_credit` 是数据面 peer/WQE 调度策略，不是模型
TopK、expert histogram 或 Planning metadata 的输入契约。同一 shape 和其余 route contract
一致时，meta 路由应允许跨 peer mode 重建；完整的 meta checksum 和 mode provenance 仍须
保留，runtime cache identity 也继续按实际 mode 隔离。性能兼容性必须比较包含 peer mode 的
完整 provenance：mode 不同时标为 `checked-in reference`，并在比较输出中同时显示模型采集
mode 和当前 replay mode，不能把 legacy 性能冒充 grouped 实测。本规则的自动化边界覆盖
合成 meta、上库 bundle 和禁止模型 callback 的 meta-hit 路径；它不替代对应 mode 的实机
数据面验证。

隐私扫描 IP 时必须要求完整点分数字 token 边界。固件版本 `9.0.0.200.200` 包含四段数字
子串，但不是 IP；把它误拒绝会发生在模型和 profiler 均已完成后的 meta 发布边界。扫描器应
继续拒绝嵌入文本的真实 IPv4，同时用回归测试覆盖五段固件版本。

重复使用同一个显式输出目录会让 Case 17 profiler 同时发现旧、新两份
`kernel_details.csv`，所有 rank 即使已生成完整 `result.json` 也会在汇总阶段失败。实机 A/B
应给每次运行分配唯一输出目录；不要把多 CSV 解析错误归因于级联 Kernel，也不要通过删除
不属于当前运行的 profiler 数据规避。2026-08-21 的 B131 单机 8 rank 验证中，强制
`model` 完成 8/8 模型迭代并发布 meta；随后在模型启动器设为 `/bin/false` 时，强制 `meta`
从空 runtime cache 重建 generation 并通过 55-stage replay，强制 `cache` 再次命中并通过。
该 bundle 包含 80 个 rank-call、40 个唯一 TopK，三个文件合计 1,075,429 bytes。

同日 B131 双机 16 rank、`S=4096,K=8,H=7168,EP=16,R=16` 也完成了相同的
`model -> meta -> cache` 闭环。`meta` 使用独立空 runtime root，并把模型启动器设为
`/bin/false`，仍从上库数据重建 generation、完成 16 rank 55-stage replay；随后 `cache`
命中同一 generation。该 bundle 包含 160 个 rank-call、80 个唯一 TopK，expert ID 范围
为 0..31，解压后路由为 2,621,440 bytes，三个文件合计 2,092,469 bytes。meta 反建结果
除 capture/source provenance 外与模型生成的 runtime route 语义字段完全一致；比较完整 JSON
时应分别校验两类 provenance，不能要求新 generation 伪装成原 capture。

模型、8/8 profiler 和 post-idle 全部成功后仍可能在 meta 发布边界失败；2026-08-23 的
`S=8192,K=16,H=3584,EP=8,R=8` grouped capture 首次暴露
`model performance provenance firmware is unavailable`。根因是 runtime provenance 只接受
手工 `TILEXR_MODEL_REPLAY_FIRMWARE_ID`，没有使用目标机已有的只读 board 信息。缺少显式
覆盖时，应有界调用 `npu-smi info -t board -i 0`，只提取 Firmware Version、Chip Name 和
Chip Version；显式环境覆盖仍优先，命令失败或字段缺失仍保留 `unavailable` 并拒绝发布，
不能猜测版本。单元测试覆盖解析和组合规则，B131 Ascend950PR 实机探针得到
`9.0.0.200.200` 与 `Ascend950PR-V100`；该证据只验证 provenance 探针，不替代模型/replay
数据面验证。

## managed follower 的源码身份与偶发算子状态

2026-08-22 的 B131 双机 16 rank、`S=8192,K=16,H=3584,EP=16,R=16`
model replay 中，leader 已发布完整 generation，但 follower 在启动级联前报告
`follower cache identity mismatch`。首个失败边界不是路由或通信：Mutagen 部署按规则
排除了 `.git`，leader 通过 `TILEXR_MODEL_REPLAY_TILEXR_GIT_SHA` 提供显式源码版本，
managed follower 却没有转发该变量，因而把同一源码识别为 `unavailable`。修复应在 follower
环境白名单中转发已经校验的 Git SHA；不能放宽 shape、runner、adapter、Kernel 或安装产物
身份校验。脚本回归测试应固定覆盖该变量。

同一轮诊断中，一次长轮次 replay 在 ReduceGrad 返回 rank-local `3` 后由跨 rank barrier
传播为 `1`，但保持输入、缓存和二进制不变的后续 16-rank 完整运行通过，无法稳定复现。
因此临时 phase/status 编码只用于证伪，不应作为产品行为提交，也不能据单次状态修改
ReduceGrad 协议。验证边界至少要保存失败 epoch、owner rank、前序 stage DFX、运行身份，
并用相同 generation 重跑；只有 rich status 稳定指向同一 post/flush/quiet 边界时才进入
对应 SQ/CQ 修复。安装产物身份必须包含独立的
`libtilexr-moonep-reduce-grad.so`，否则 Kernel 变更后旧 cache key 仍可能命中，破坏该 A/B。

在变基到当时最新 `main` 后，正式非诊断构建进一步完成了同一 8K/K16/H3584 路径的
验证闭环：单机 EP8/R8 和双机 EP16/R16 均以 5 轮 warmup、20 轮统计，通过全部 rank 和
55 stage。两个规模都分别强制执行 `model`、`meta`、`cache`；`meta` 从空 runtime root
重建 generation，`meta` 和 `cache` 都把模型启动器设为 `/bin/false`，证明 compatible meta
不依赖当前机器已有 runtime cache，且两条命中路径都没有隐藏的模型 fallback。三条路径都
记录 framework profiler 和 stage barrier 为启用，Kernel 编译期 Dispatch DFX/profiling 和
Combine V2 profiling 为关闭。
该结果只覆盖 B131、Ascend950PR、legacy Dispatch peer mode 和已记录的两节点拓扑；一次
不可复现的 ReduceGrad 状态不足以支持修改 Kernel 协议。

## grouped replay 的 completion counter 与控制 token 发布顺序

2026-08-22 在 B131、Ascend950PR、`S=8192,K=16,H=3584` 的 grouped replay 中，
`group` 和 `group_credit` 的首个失败边界分成两层。首先，Dispatch 把
`wqeCntAddr` 当成 SQ head frontier：一个 batch 只生成一个 ordered-completion CQE，旧代码
却按 batch 内 WQE 数增加该字段，并用它作为 terminal drain 目标。这里应把
`wqeCntAddr` 作为 completion counter，每个 completion batch 只加一；SQ drain 则等待
真实的最终 `head`。修复后短轮次通过，但双机 16 rank 的 `group` 长轮次仍稳定在
replay epoch 50 的 Combine Done wait 失败。

第二层证据更具辨识度：失败接收端期望 token `1792`，实际为 `1776`，差值恰好是一次
launch 的 token 步长；另一轮也观察到期望 `2872`、实际 `2856`。发送端 ordered CQ 已完成，
但稀疏 lane 1 的接收端仍收到上一轮 token。根因是 Combine V2 先用 scalar GM store 更新
UDMA 本地 source，再 clean cache 并立即发布 WQE/doorbell；这个序列没有建立目标 CANN
数据面可依赖的 MTE3 完成顺序。修复方式是复用现有 256B WQE context UB，在 UB 中构造
完整 64B token cache line，通过 `DataCopyPad` 写入注册 GM，并在组装引用该 source 的 WQE
前等待 `MTE3_S`。复用的 fullmesh WQE 还必须显式清零 `flag`，避免上一个 completion batch
遗留 ordered-completion 位。

最小 A/B 只改变 token 发布路径后，原稳定失败的双机 16 rank `group`、warmup 5 +
iterations 20 连续两次通过；相同规模 `group_credit` 5+20 和 `legacy` 5+1 通过，单机
8 rank 的 `group`、`group_credit` 5+20 也通过。所有运行都完成全 rank、55-stage 汇总，
保留 framework profiler 和 stage barrier；输入由 compatible meta 重建，并把模型启动器
设为 `/bin/false`，因此这些结果证明 replay 数据面和 meta-only 路径，不代表 grouped
模型采集本身已完成验证。该结论不能外推到 64/128 rank 或其他 CANN、SoC 和拓扑。

## 多节点 model-flow stage barrier 与 grouped fullmesh 队列域

2026-08-27 在 B131、Ascend950PR、双机 16 rank 的 4K/K8/H7168 和
8K/K16/H3584 model replay 中，短轮次可以通过，但 5 轮 warmup + 20 轮统计会在不同
rank 形成稳定长尾或停滞。首先确认 replay 的 55-stage 实际走
`tools/moonep/model_flow.py`；该路径原先没有执行多节点 stage barrier，两个节点可能以约
13 秒错位进入首次 Planning，并在 device sync 返回 `507035`。修复必须在 model-flow 的
Planning、Dispatch、Combine 和 ReduceGrad 边界调用与 benchmark 相同的真实 rendezvous，
不能通过放宽 Planner、修改 Dispatch 输入或增加 timeout 掩盖 stage skew。

补齐 barrier 后，host stage trace 把剩余首失败固定到
`recompute_backward_1` 的 Dispatch backward 设备完成：所有 rank 都完成 Host launch，
只有部分 rank 走出 `buffer.synchronize()`，其余 rank 停在其中。Planning、后续 Host
rendezvous、Combine 和 ReduceGrad 因而都不是该次复现的首个失败边界。保持 compatible
meta、16 rank、5+20、profiler、barrier、二进制和跨节点 registered QP 不变，只禁止
grouped Dispatch 为同节点 peer 选择 fullmesh QP 后，`group` 与 `group_credit` 均通过。
这组单变量 A/B 说明 grouped plan 的跨 stage 复用不能混用生命周期不同的 SQ/CQ 队列域：
fullmesh QP 保留的历史状态不受当前 operator-generation registry 契约约束，会让重复
Dispatch 在历史 CQ/SQ frontier 上失去一致性。

最小修复只把 grouped Dispatch 固定到 registry-owned QP；不删除通用 fullmesh runtime，
也不改变 MoonEP API、route、credit 协议或 stage barrier。交付前应移除 Planner phase-stop、
stage trace 转发等任务期诊断，使用 Dispatch DFX、Dispatch profiling 和 Combine V2
profiling 均关闭的 Release 构建，同时继续保留 framework NPU profiler 和真实 stage
barrier。最终验证覆盖 B131 的单机 8 rank 和双机 16 rank、`group`/`group_credit`、
4K/8K 两组 shape；16 rank 的 model、空-cache meta 和 runtime cache 路径均有实机证据，
但本结论不能外推到 64/128 rank、其他 CANN/SoC/拓扑，也不能证明 fullmesh runtime 在
获得等价 generation/reset 契约后仍不可用于其他算子。

最终交付证据必须晚于最后一次产品代码修改。调试期已经通过的 shape 或 replay source
只能作为回归参考；清理 DFX、修改默认 mode、launcher、Host 或 Kernel 后，必须冻结完整
源码 diff/hash，重新构建和部署，并让全部交付矩阵从该同一冻结版本重新运行。每条结果应
同时保存源码 diff hash、构建选项、实际加载 DSO hash 和运行时间；任一后续产品代码修改
都会使这组闭环证据失效。MoonEP Dispatch 的未设置或空 peer mode 必须解析为
`group_credit`，显式 `group` 仅用于要求覆盖该模式的测试，显式 `legacy` 必须在 Kernel
launch 前拒绝；不能只在外层脚本禁止 legacy 而让 Host 保留隐藏默认或 fallback。

Python 上下文必须在任何 NPU workspace 或 registered mailbox 分配前镜像原生 Planner 的
参数上限。原生 Planner 当前只接受 `expert_count <= 4096`；如果 Python 只校验正数和 rank
整除，非法但可整除的超大值会先放大 mailbox 分配，产生 OOM，而不是稳定返回参数错误。
边界回归应同时覆盖 `4096` 可接受和大于 `4096` 且仍可整除的输入被提前拒绝；该 Host
校验只证明失败顺序和 API 契约，不替代合法 shape 的实机数据面闭环。

## 当前实现状态说明

本文记录的是已验证经验，不代表所有修复都已经进入 `main`。截至 2026-08-12：

- 已进入 `main`：MoonEP 上游 API 兼容、Combine V2 路由与 reduction、阶段性能报告。
- 当前调试分支中：same-stream status reset、32-QP Prefetch/ReduceGrad、grouped Dispatch
  route tiling/WQE/CQ 流控和相关测试。
- 任务补丁或 MindSpeed 工作树中：MindSpeed adapter、external communication owner、RA
  attach/ownership guard、历史 Combine V1 标量修复和通用 CQE cycle 修复。

开始新的调试任务时，应先检查这些修改是否已经进入当前目标分支和实际加载的二进制，
不能根据本文的历史状态假设代码已经包含修复。
