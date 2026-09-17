# EP Memory 服务器级去重设计

## 2026-09-16：Dispatch 使用 Token 级 prefix 编号（当前契约）

本节替代本文及历史 R04/R05/R07 中的 AtomicAdd 取号、密排 t 和 group 表 8*BS 行约定。
当前只实现 Dispatch；Combine 的数据区扩容、收发、监听和清理留到 Combine 阶段。

对于第一跳包（forwardCount>0），使用接收端已有的零基 Token 级 prefix 行号：

```text
t = 当前 localExpert/source 段之前的累计条数 + q
  = tasks.Begin() + j = row
forwardTokenIdx = t
C = R * BS * min(K, L)  # 现有 capacityRows，L=moeExpertNum/R
0 <= t < 本卡 tokenNum <= C
groupBytes = C * align32(4 + 8*(K-1))
```

不能使用整段的 inclusive prefix、段内 q、物理 tokenPos 或会变化的 pending 下标取号。
所有接收 row 在本卡本轮唯一，第一跳是其子集，因此无需 AtomicAdd 或额外 prefix 计算。
第一跳 owner 写入 assistEx[row]、group[t] 及公共转发包尾；全部终端副本（包括本卡其他
expert）继承该 owner 的 (relayRank,t)，不以终端自己的 row 重算。DIRECT 保持 -1 引用。
不同卡允许相同 t，由 relayRank 区分；相同路由重复调用的 t 稳定，但不是跨调用持久 ID。

Group 行可以有空洞，第一跳的 group[t].firstHopRow 等于 t，字段及 32B assistEx ABI 保留。
只消费有效 assistEx 中 forwardCount>0 引用的 group，且只读取其前 forwardEntryCount 个成员；
空洞无需初始化或清零。members[i].recvSlot 继续为 t*K+originalK，成员的顺序及真实 weight 不变。
按 C 预留 groupOut；不能依据实际 group 数或旧 8*BS 上限分配、校验地址。删除原子取号及
轮末 counter 清零；当前也移除核末 SyncAll，保留本核完成、双窗口和 Dispatch Flag 清理边界。
原 counter 的 1KiB 布局区和 profiling 事件编号仅作历史保留，不再产生 Atomic/CounterEnd 事件。

当前 Dispatch 不访问 Combine relay 返回区，保留旧通信窗口预留和容量检查以隔离性能变量；
旧 relayBytes=8*BS*K*returnStride 不满足新编号契约，不代表 Combine 已可用。后续 Combine
至少按 C*K*returnStride 预算其离散结果区，重新核算整个窗口、框架分配和生命周期。
不能把新的 recvSlot 直接用于旧 relay 预留区。内存压缩优化留到后续。

本轮验收：编号与独立原始路由参考的第一跳输出行一致；覆盖 t>=8*BS、稀疏空洞、不同接收核、
乱序 ready、DIRECT/第一跳/终端、本卡和跨卡副本、连续 magic；分配和结果校验同步采用新容量。
Host 替身证明编号/字节/边界契约，CANN 9.1 A5 编译证明编译；性能差异需要相同 case 的 A5 实测，
不能从去掉原子指令推断收益。当前调用方必须重新按新 groupBytes 分配，函数签名未变不代表旧分配兼容。

维护要点：从密排计数器改用已有稀疏身份时，容量由最大可寻址索引决定，而不是有效 group 数；
必须同步调整 caller 分配、元数据检查及独立 oracle，否则正常高 row 会越界或被测试误拒。

## 2026-09-17：移除 Dispatch 核末 SyncAll（当前契约）

Token 级 prefix 已取消共享 counter 清零，核末本地收尾之后只剩本核 profiling 发布，
没有消费其他核结果的工作，因此删除末尾 `SyncAll()` 及其 `SyncEnd` 打点。
保留 `counts_.Finish()` 的 `PipeBarrier<PIPE_ALL>()` 和控制寄存器恢复，以及发送完成等待、
接收 lane 回收和 Dispatch Flag 清理；不能因删除核间屏障而一并删掉这些本核依赖。
同一 communicator 的调用仍须在同一 stream 串行执行，并保持所有 Rank 的调用顺序一致；
kernel 完成要求全部核退出，某个核提前返回不允许下一轮提前复用窗口。
双窗口和全 Rank Count 发布（含零计数）继续保护跨轮复用；本次不引入跨 Rank 完成握手。

旧 trace 的 `SyncEnd` 事件 ID 和 `finish_sync` 解析保留兼容；新 trace 的 `finish_return`
从 `LocalEnd` 开始，不生成虚假的 `finish_sync`。各核 `kernel_total` 终点现在允许不同，
其均值/中位数降低不能直接视为整个 kernel 加速：原同步耗时包含快核等待慢核的时间。
收益以相同 case 的 msprof 整个 kernel 时长评估；CANN 编译和 trace 回归不能证明设备并发正确性，
A5 验证须覆盖连续多轮双窗口复用、空任务核及收发负载不均衡，并检查结果和 Flag 清理。

## 2026-09-17：运行时发送/接收核分配（当前契约）

总核数固定为 64；`send_core_num` 支持 16/24/32/40/48，默认 32，
发送核为 `[0, send_core_num)`，接收核为 `[send_core_num, 64)`。
同一套 kernel 产物支持全部取值，`run.sh ... send_core_num=16` 即选择 16 发/48 收；
不传参数时每次恢复 32 发/32 收，不继承旧环境或本地配置中的分核值。
该约定覆盖本文旧版固定 48 发/16 收的说明。

校验只在 Host 执行：脚本、工作流和公开 Dispatch API 拒绝范围外或非 8 倍数取值，
并检查 Count 和发送 UB 容量；kernel 直接使用经过校验的参数，不增加取值检查分支。
公开 API 在 stream 后增加默认值为 32 的 `int64_t sendCoreNum`；内部 launch ABI
在 profilingLaunchSlot 后增加同名 int64 字段，大小从 192B 改为 200B。
调用方、Host 库和独立 kernel 须一起重新编译部署，旧安装不能混用。

Count 的接收分段、两张 cumsum 矩阵、每个接收核的完整 prefix 副本，以及 Token 的
floor 分区均使用本次 `recv_core_num=64-send_core_num`。源端 Token 划分使用发送核数。
`TILEXR_EP_SERVER_DEDUP_ALL_CORE_COUNT_BS_THRESHOLD` 默认 512；`BS<512` 时所有
64 核参与 Count，`BS>=512` 时仅接收核参与 Count。此阈值独立于发送核数。
所有 Rank 在同一 communicator 生命周期内使用相同且固定的分核值，调用顺序一致，
并在同一 stream 串行。每次 run.sh 新建 communicator，因此不同运行可切换分核；
不支持同一 communicator 跨轮改变分核。不同接收核数会移动 Count 控制区，旧 prefix
保留内容可能覆盖另一种分核的 ready Flag，不能只靠两轮窗口交替防止误判。
分核不改变 Token 级 prefix 编号、双窗口、Flag 清理或数据同步契约。

核数泛化不能只修改角色分界：接收矩阵地址、prefix 副本数和任务区间必须使用同一个
运行时值。发送核减少会增大每核 weights/masks/directTasks 的 UB 用量，Host 须在
launch 前确认完整 scratch 和至少一个带 Flag 包的空间，不能让核内 unsigned 差值下溢。
profiling 角色由实际记录中的 send/recv 事件识别，历史 48/16 trace 仍按其自身记录解析。
Host 行为测试覆盖五种分核的 Count、收发及空分区；真实 CANN 编译仅证明 API/编译边界，
实际吞吐和异步多卡正确性需要按每种分核进行 A5 功能及连续多轮测试。

## 设计状态

本文合入第一轮 D01-D11、相关 Combine 讨论及第二轮已经确认的设计，作为后续梳理的基线。
已同步 R03 的 16B entry、固定 K-1 容量与无效区域不清零，R04-07/R04-08 的 expertScales 复用，
R05-01 至 R05-03 的固定分核和 UB 双表；计数器方案已由本文开头的 Token 级 prefix 契约替代，以及 R05-04 已确认的本卡数据发送流水。
Cnt 发送、接收及 cumsum 整体复用 Memory；通信空间按 Server Dedup 独占的三块区域排布。
源端按两遍扫描执行，R02-01 已确认 serverRouteMask + directMask；R05-04 的源端 UB/Event、R07-01 的单份布局和 R10-01 的权重 ABI 已给出具体实现结论。
尚未决定的机制集中在[第二轮待确认项 R01-R12](ep-memory-server-dedup-open-topics.md)，
旧 C 编号见 [Combine 议题索引](ep-memory-server-dedup-combine-open-topics.md)。

本文区分已确认的逻辑契约与仍待确认的物理实现：字段示意、容量估算和流水约束不表示公共 ABI、
kernel 或性能已经实现并验证。后续确认更新本文对应章节，并关闭相应 R 项，不再维护多份重复的设计结论。
普通 Memory 行为以 [EP Memory 规格](ep-memory.md)为准；本设计使用独立去重入口，不隐式修改原入口。

当前代码已实现完整 Dispatch 的 cnt/cumsum、源端发送、接收/转发、输出及清理。
Combine 尚未实现；本次 Token 级 prefix 改动的验证边界见验证指南，不能沿用旧编号的功能/性能结论。
2026-09-15 更新：每 rank 总通信域为 8 GiB（8188 MiB 数据 + 4 MiB runtime Flag，`peerMemBytes=8589934592`）。两个固定 4 GiB 窗口按 `magic % 2` 选择；每窗前 2 MiB 为控制信息，Token 区上限为窗内 4092 MiB。两窗尾部均保留 4 MiB，实际 runtime Flag 只占整个域的最后 4 MiB。当前仅实现 Dispatch，Combine 后续共用此域。
开发阶段 1～4 的当前主流程测试固定为 16P、每卡 6 专家、BS=96、TopK=6、balance_shift；优先验证流程和性能，边界条件在最终验收统一覆盖。这是测试基线，不缩窄算子的设计支持范围。

## 算子函数与模块划分

初版采用一个 `init` 函数和一个 `process` 函数作为算子内部的两层入口，不按数据发送和数据转发拆成两个独立算子。

| 层级 | 函数/模块 | 主要职责 |
| --- | --- | --- |
| 顶层入口 | `init` | 读取输入参数、建立 GM/通信窗口指针、计算去重 Token 的 T、初始化 workspace，完成 64 核角色划分；路由加载及 q/cnt 计算沿用 Memory 对应处理路径的时机 |
| 顶层入口 | `process` | 根据当前核角色分派到 `sendData` 或 `recvData`；不在这里增加阶段级同步 |
| 源端路径 | `sendData`（48 核） | 按 BS 条件执行本核的 `sendCnt`，调用 `all2allDispatch`，完成源端 Token 发送及 sourceMask/expertScalesOut 写出 |
| 接收/转发路径 | `recvData`（16 核） | 执行本组 `sendCnt`，接收 cnt、计算 cumsum，建立监听表，监听接收区并处理直发 Token、待转发 Token 和已转发 Token |

### `sendData` 内部模块

当 `BS<512` 时，`sendData` 先条件调用 `sendCnt`，下发本核被分配的 cnt；`BS>=512` 时不调用该路径。
随后调用 `all2allDispatch`，由该函数对本核负责的 Token 做第二次任务划分和实际发送：

1. 第一遍扫描本核全部 Token 任务，识别远端 Server 的多 route group，构造第一跳 Payload，并发送需要数据转发的 Token。
2. 在同一 Token 处理过程中记录 sourceMask、真实权重和其他源端中间信息到 UB；发送副本使用去重 Token 的完整 T。
3. 第二遍扫描同一 Token 区间，处理无需转发的 DIRECT route 并发送，仍与第一阶段共用源端发送 UB/Event 池，不插入阶段同步。
4. 本核任务完成后，将 sourceMask 和 expertScalesOut 的 source 归约权重写出到各自 GM 输出。

assistEx/group 由后续 recvData 的 Token 接收 owner 生成，不属于源端 sendData 的输出任务。
两遍扫描已按 R02-01 确认采用 serverRouteMask + directMask，具体 UB 和分类步骤见下文「两遍扫描的分类表」。

`all2allDispatch` 的第二次分核只负责把 Token 任务分配到 48 个源端数据发送核；它不改变 q/cnt 的原始路由顺序，
也不创建第三组核或新的通信阶段。源端发送 UB/Event 的申请和环形复用见[本卡数据发送流水](#本卡数据发送流水)。

### `recvData` 内部模块

`recvData` 在固定 16 个核上执行以下顺序：

1. 执行本组负责的 `sendCnt`；小 BS 时与 48 个源端数据发送核共同覆盖全 64 核 cnt 发送，大 BS 时由本组覆盖 cnt 发送。
2. 执行 `recvCnt`，接收原始 source 直接发送的 cnt；随后执行 `calcCumsum`，生成当前 Rank 的接收前缀和及有效 Token 数。
   本部分包含完整前缀的就绪等待、读取及就绪 Flag 清理；由接收组内核 0（物理核 48）沿用 Memory 计算并写出 expertTokenNumsOut。
3. 根据 cumsum 结果均分监听范围，在 UB 构造 `tokenPos`、`pendingIdx` 和监听所需的其他临时状态；监听地址由相对槽位和 T 计算。
4. 持续监听 receive buffer。根据 Token 尾部识别直发 Token、待转发的第一跳 Token 和已经转发过的终端 Token，分别执行本地整理、展开后续转发或匹配有效 entry 后的终端处理；本阶段的数据转发 UB/Event、公共副本和进度回收另行设计。

`recvData` 的 cumsum 完成是监听表生成的前置条件，但不表示各 Token 已经到达；监听仍按完整 Payload 的 DataAsFlag 逐 Token 判定。
`sendData` 与 `recvData` 是核角色路径，不是两个可以相互等待的逻辑阶段；两组核在 `process` 中并行推进，核末各自完成写出和清理后返回，不再执行全核 `SyncAll()`。

### Cnt 与 cumsum 的 Memory 复用边界

已确认 sendCnt、recvCnt、calcCumsum 整体沿用 Memory 产品化路径的统计、记录格式、等待、前缀计算、写回及状态清理代码。
这里的函数名是职责划分，不要求拆开 Memory 已有函数或改造其流水；例如 WaitDispatch 内部已包含接收等待、状态清理和分段计数汇总。
仅适配已确认的固定 48/16 核组、取消双 Group 和 Server Dedup 区域基址/尺寸，不新增统计算法、q 表或同步协议。

| 职责 | 复用的 Memory 路径与结果 |
| --- | --- |
| sendCnt | CalAndSendCntByRankAndExpert 按 expert 区间分核，保留其范围筛选计数/逐 expert 计数选择及 SendExpertCntRange 写出；输入是本 Rank 的原始 expertIds |
| recvCnt | WaitDispatch 及其 WaitDispatchClearStatus、GatherSumRecvCnt，按既有 cnt Flag 等待、清理及汇总 |
| calcCumsum | GetCumSum、CalRecvAndSetFlag 及既有前缀就绪协议，按 localExpert/source 顺序写 sendCountsOut，保持 int32[L*R] 的含当前项前缀语义 |
| expert 计数输出 | 复用 SetValidExpertInfo 中从完整前缀生成 expertTokenNumsOut 的代码及 int64[L] shape；expertTokenNumsType=0 为累计前缀，1 为逐 expert count |

总专家数上限为 1024，因此每 Rank 的 cnt/cumsum 表至多有 `L*R=moeExpertNum=1024` 项，
固定 16 个接收核均分后每核至多 64 项。该上界约束的是 cnt 记录数，不是接收 Token 数或 tokenPos/pendingIdx 表长；后者仍按实际 tokenNum 预算。

Memory 将 expertTokenNumsOut 的生成放在 LocalWindowCopy 准备路径中；去重版保留该计算，接到完整前缀就绪后的接收侧准备位置。
适配其执行核时不能保留旧核 0 的接收职责假设，也不能把旧 LocalWindowCopy 的监听分区带入 recvData 后续设计。
各接收核取得完整前缀的条件仍由 Memory 的就绪协议保证，完成本核 sendCnt 或本核前缀段不等于完整前缀已可读。
完整前缀仅由 16 个接收核消费；前缀副本和就绪 Flag 的发布、读取、清理统一使用接收组内索引 0..15。
保留 Memory 的记录格式、发布顺序和等待机制，适配消费者数量，不再向 48 个发送核发布前缀副本。
零 cnt、整卡 tokenNum=0 沿用 Memory 行为：完成 cnt/cumsum 的通知、等待、输出和清理职责，后续无有效 Token 时不生成监听任务。
上述输出语义及零结果处理已确认，不作为待选方案；具体函数安置、核索引及区域地址是复用代码的实现适配。

### Cnt 增量实现与验证边界

实现位于 `src/ep/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.h`，Host 固定启动 64 个 AIV，
设备不足 64 个 AIV 时拒绝；阶段一空入口的任意 blockDim 覆盖及空 kernel 打点已删除。
`SendCnt` 使用原始 expertIds，保留 Memory 的范围筛选统计/逐 expert reduce 选择，随后异步下发计数，独立保留发送记录并复用筛选 scratch；
16 个接收核执行 `RecvCnt -> CalcCumsum -> CompleteCumsum`，核末等待本核 GM 写入完成。
本增量不访问 hidden/权重，不构建 Token 监听表，也不取号或清零尚未使用的 relay counter。

第一块通信区域的计数子布局由 Host/Kernel 共用 `ep_memory_server_dedup_count_layout.h`，相对 `peerMems[rank]` 定位。
每份计数状态依次包含 `E*32B` cnt 记录、`16*16*32B` 分段汇总、`16*16*32B` 完整前缀就绪 Flag、
`16*E*4B` 前缀副本，整体按 1KiB 对齐。沿用 Memory 的 `magic % PING_PONG_SIZE` 选择计数状态副本，
两份副本之后只预留一份 1KiB relay counter；这不沿用旧 Memory 的绝对 offset，也不决定后续数据区的轮次布局。
E=1024 时每份计数状态为 112KiB，两份加 counter 共 225KiB。Host 检查本增量所需状态容量及 32B 基址对齐。

计数发送峰值 UB 为 `4*align256(BS*K*4)+align8(ceil(E/countCores))*36`，`countCores` 按 BS 取 64 或 16。
Host 在整数窄化前核算该预算，超过沿用的 190KiB UB 预算则拒绝；这是计数资源约束，不是独立的单卡专家数限制，
也不表示完整数据流水的 shape/容量已验证。接收/前缀 UB 与筛选 scratch 共享地址；cnt 记录在发送完成前保持不变，
局部前缀先于跨核结果等待计算。详见 [cnt 发送掩盖优化方案](ep-memory-server-dedup-count-send-overlap.md)，进入 recvdata 前仍保留完成屏障。

复用时容易遗漏的边界是：消费者从全部核变为接收组后，必须同时修改前缀副本数量、Flag 矩阵、读写下标和清理范围，
不能只修改 cnt 分核；Scalar 写好的前缀须建立 S→MTE3 依赖后再复制，副本写完后才发布 ready。
`GM_ADDR` 在 CANN 下是指针宏，多个 GM 成员须分别声明，不能用一条逗号声明把后续成员变成字节变量。

`test_tilexr_ep_server_dedup_counts` 直接运行生产计数方法，以独立 route histogram 检查 cnt、前缀、两类 expert 输出、
16 份副本、清理和轮次隔离，覆盖 BS=47/48、L=1/64、24P 分段尾部、1024 experts、零接收和四轮复用。
其 CPU API 替身只模拟同步搬运与算术，不证明 CANN 编译、异步 MTE 时序或 NPU 通信。
`test_tilexr_ep_server_dedup_chain` 上机入口已增加两类计数输出的参考检查，连续轮次交替 expertTokenNumsType；
旧空 kernel breakdown 模式已移除。构建该入口仍使用 `BUILD_TILEXR_EP_SERVER_DEDUP_CHAIN_TEST=ON`，
在 CANN 9.1/A5 环境运行后须逐 Rank 确认输出匹配；Host stub 对该入口的测试只证明测试程序能识别错误输出。
当前轮次检查保持 communicator、E 和 BS/K 不变；跨 shape、并发调用及完整 Dispatch/Combine 生命周期继续由 R11 收口。

## 支持范围

| 维度 | 阶段一约束 |
| --- | --- |
| Server 与 Rank | 每 Server 固定 8P；Server 数 N 为 2 至 16，总 Rank 数 R=8*N，即 16P 至 128P |
| Server 映射 | 连续 Rank 编号，`serverId=rankId/8`，使用整数除法 |
| 普通 expert | 连续 ID 均匀分配，总专家数 `0<moeExpertNum<=1024` 且能被 R 整除；不单独限制每卡专家数，允许 L=1，典型为 3 或 6 个 |
| Expert 映射 | `rankId=expertId/L`，`localExpertId=expertId%L`，`moeExpertNum=R*L` |
| Top-k | `1<=K<=min(32,moeExpertNum)`，典型为 6、8、16；K 不超过总专家数由有效且互不重复的 route 契约决定 |
| BS | 所有 Rank 的原始输入 BS 相同，`globalBs=BS*R`，`maxBS=BS` |
| 输入保证 | 每个 Token 恰有 K 条有效 route；expert ID 在 `[0,moeExpertNum)` 内且互不重复，无空 route 占位 |
| Dispatch group 容量 | 按 C=R*BS*min(K,L) 行预留稀疏 group；不发布 X 或额外 group 计数。Combine 容量另行实现 |
| 数据发送并发 | 每个源端数据发送核按剩余 UB 配置 1 至 8 路；安全输入保证至少容纳一个完整去重 Token，不增加 0 并发校验 |
| 输入 dtype | 支持 BF16、FP16；对应现有枚举 TILEXR_DATA_TYPE_BFP16 / TILEXR_DATA_TYPE_FP16 |
| Dispatch 量化 | 支持非量化 quantMode=0 及 MXFP8 quantMode=4；MXFP8 沿用 Memory 的 E4M3/E5M2 选择，其他量化模式不支持 |
| 输入 mask | 阶段一不支持 expert mask 和 token mask，内部不读取或执行筛选 |
| 路由权重 | 首版要求非空 FP32 `expertScales[BS,K]` 输入；source 归约权重写入独立 expertScalesOut，由 Combine 的 expertScales 参数消费；nullptr 直接报错 |
| 去重范围 | 仅处理原始 source 与目标位于不同 Server 的 route；源 Server 内逐 route 直发 |
| 共享专家 | 阶段一不支持；`sharedExpertNum` 与 `sharedExpertRankNum` 必须均为 0；后续低优先级扩展 |
| 实现与接口 | 原 Memory 与去重入口分开；去重实现独立放在 `.h` 中，不支持的配置直接报错，不自动回退；中转容量安全输入契约不属于初版校验项 |
| 平台基线 | 保留 C++14、CANN 9.1.0 及现有 EP A5 边界；使用 Memory peer window 承载同节点与跨节点通信 |

典型配置为 128P/384 experts（每卡 3 个）和 64P/384 experts（每卡 6 个）。
每卡 expert 数由 `L=moeExpertNum/R` 推导，上限为 `floor(1024/R)`，不再使用固定的每卡 32 个上限：

| Rank 数 R | 推导的每卡 expert 上限 | 对应总专家数 |
| --- | --- | --- |
| 16 | 64 | 1024 |
| 24 | 42 | 1008；保持总数能被 R 整除 |
| 32 | 32 | 1024 |
| 64 | 16 | 1024 |
| 128 | 8 | 1024 |

256P、总专家数超过 1024、K>32、任意 Rank/Server 映射及非均匀 expert 分布不属于阶段一。

输入保证直接作为算法前提，不增加重复 top-k 或 expert ID 范围扫描，不继承 invalid expert 跳过分支。
不支持的功能保留接口参数，但不因保留参数而执行旧功能。具体空值/默认值约束在 R10 闭合。
`sourceMask` 是持久路由元数据，与已排除的输入 mask 功能不同；group 的贡献集合通过第一跳及有序 member 列表表达，不额外保存 memberMask。

## Dispatch dtype 与 MXFP8（R08-03）

已确认支持 BF16/FP16 输入以及非量化、MXFP8 两类 Dispatch 场景。
沿用 Memory 已有的六种类型组合，不另设量化算法或 scale 格式：

| 输入 dtype | quantMode | expandXOutDtype | dynamicScalesOut |
| --- | --- | --- | --- |
| BF16 / FP16 | 0 | 与输入相同；RESERVED 沿用解析为输入 dtype | nullptr |
| BF16 / FP16 | 4（MXFP8） | 显式 FP8E4M3，对应 fp8_e4m3fn_t | 必须提供，保存每行 MXFP8 scale |
| BF16 / FP16 | 4（MXFP8） | 显式 FP8E5M2，对应 fp8_e5m2_t | 必须提供，保存每行 MXFP8 scale |

量化模式下不把 RESERVED 自动解释成某个 FP8 格式；直接沿用 Memory 对 quantMode/输出 dtype/dynamicScalesOut 的组合判断。
scales、expandScalesOut 继续是本路径未使用的占位。expertScales 则按已确认的去重契约保持必填、FP32 路由权重输入，不能照搬普通 Memory 拒绝非空 expertScales 的条件。

### 量化计算与数据布局复用

量化直接复用 [QuantMxfp8](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L636) 及其调用的
[TileXRMxfp8Quant](../../src/ep/kernels/tilexr_ep_mxfp8_quant.h)：ComputeMaxExp → ComputeScale → ComputeFp8Data。
BF16/FP16 各自的计算分支、FP8 格式、舍入、饱和及特殊值处理均沿用现有实现，不改写成新的逐 Token 或逐 expert 量化算法。
量化仅处理 hidden；路由权重、固定头部和 entries 在量化完成后追加，整数/FP32 编码保持。

记 Q=align256(H)、S=align2(ceil(H/32))，单位除 H/Q 外均为 B：

| 项目 | 非量化 | MXFP8 |
| --- | --- | --- |
| 原始输入每行 | H*2 | H*2 |
| 量化计算长度 | 不使用 | Q 个输入元素；输入计算 UB 为 Q*2，计算 scale 数 Q/32，工作区按 Memory 计算 |
| 有效 hidden 输出每行 | H*2 | H，FP8 每元素 1B |
| UB/通信 Payload 内 scale 起点 | 无 | Q，保留 hidden 格式内的 256B 对齐间隔 |
| scale 每行占用 | 0 | S；每 32 个 hidden 元素一个 1B E8M0 scale，按 2 个 scale 补齐 |
| 尾部之前的数据跨度 D | H*2 | Q+S |
| 去重尾部起点 | align32(D) | align32(D) |
| 去重 Payload / 发送跨度 | Payload=align32(D)+align32(32+16*(K-1))，T=ceil(Payload/480)*512 | 同左，使用 MXFP8 的 D |

表中的公共输出沿用 Memory [CopyInAndOut](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L1506) 的拆出位置和紧密行布局：
expandXOut 为 `[tokenNum,H]`，非量化每行 H*2B，MXFP8 每行 HB；dynamicScalesOut 为字节视图 `[tokenNum,S]`，每行 SB。
量化 hidden 和 scale 按相同 dstPosition 写出，均与 assistEx 行关联；通信中的 256B/32B 对齐填充不写成 public 行间距。
按 capacityRows 分配时，expandXBytes=capacityRows*H*(quantMode==4 ? 1 : 2)，dynamicScalesBytes=(quantMode==4 ? capacityRows*S : 0)。
expertScales/expertScalesOut 仍各占 BS*K*4B，不能把它们当作 MXFP8 的 dynamicScalesOut。

输入计算补齐、量化工作区和量化结果区的初始化继续按 Memory 处理，避免未初始化值进入量化计算；R03 的未用 entry 不清零结论不删除算法所需的初始化，也不为未用 entries 新增专门的清零步骤。
非量化 xInTensor、量化结果 quantTensor_ 和发送 outBuf 按去重 Payload 重算容量；原始量化输入和工作区仍按 Memory 的 Q 和计算 scale 数申请。
随后完整 Payload 仍经 DataAsFlag 的 Vector 打包和既有 Event 配对发送，R05-04 的单包顺序不变。
Relay 转发保持收到的 FP8 hidden 及其 scale，不重新量化；接收/转发流水实现继续归 R05-05/R05-06。

H=7168、K=6 时：非量化 D=14336、Payload=14464、T=15872；MXFP8 的 Q=7168、S=224、D=7392、Payload=7520、T=8192。
MXFP8 每个 public 输出行含 7168B hidden 和 224B 独立 scale；包尾部仍预留 128B。T 必须按 480B 分块重算，尾部增长不保证每种配置都增加一个通信块。

### 去重入口需要接入的现有量化参数

公共去重 Dispatch 已有 quantMode、expandXOutDtype、dynamicScalesOut，沿用这些参数即可；动态 scale 不新增另一份公共输出。
当前空入口尚只接受非量化，其内部 launch/Device 签名缺少 quantMode 和 dynamicScalesOut，layout 的 expandXBytes 也固定按 2B 元素计算。
实现时按上表接入 Host 类型分支、typed kernel 选择、scale 指针传递和输出字节计算；R10-01 已同步更新具体 launch ABI。
量化中的 hidden/scale 拆出规则可以复用，但不因此复用旧 LocalWindowCopy 的监听调度或旧三字段 assist 输出。

源码依据还包括 [Memory Host 支持矩阵](../../src/ep/host/ep_dispatch_memory_host.cpp#L74)、[typed kernel 选择](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L1930) 和
[Memory Host 布局计算](../../src/ep/host/ep_memory_layout.cpp#L166)。Dispatch 量化结果可沿用 [Memory MXFP8 golden](../../tests/ep/demo/mxfp8_golden.h) 及既有校验方式验证，不重新定义数值算法。
本轮收口的是 Dispatch 输入、量化计算复用及 public 输出格式；Combine 的 expertOut dtype、各段返回是否量化和归约内部细节仍按 R08-03 剩余公共项及 C15/C16 讨论。
Dispatch 的 quantMode 不自动决定 Combine 的通信模式，R07 的返回跨度 U_s/U_m 也不能直接用 Dispatch 的 T 代替。
当前去重量化代码尚未接入或完成 NPU 验证，不能把 Memory 的既有实现当成去重版已经支持的证明。

## 路由与计数

去重的目的在于降低 Server 间通信量，适用条件是 `destinationServer != srcRank/8`，其中 srcRank 为原始 Token 的源 Rank。
同一轮 Dispatch 对符合条件的 route 按 `(srcRank,srcTokenId,destinationServer)` 识别可合并的第一跳 hidden，
不比较 Token 数值。阶段一只做跨 Server 的 Server 级去重，目标 Server 内保留每条原始 expert route 的独立 payload 和接收槽位。

对每个远端目标 Server，分别按该 Token 的原始 top-k 顺序处理属于该 Server 的 route。
阶段一固定其中首条 route 的目标 Rank 为 relay，即选择该 Server 子集中原始 k 最小的一条，
不是让整个 top-k 的第一张目标卡代理其他 Server。负载均衡选点留待进阶版本讨论。

记该 Server 的 route 数为 m：

1. m=1 时，source 发送 DIRECT，`forwardCount=0`，没有后续转发任务。
2. m>1 时，source 只发送一份第一跳 hidden，绑定首条 route 的 expert/source/q 正常接收槽位，
   设置 `forwardCount=m-1`，forwardEntries 包含其余全部 m-1 条 route。
3. 第一跳 owner 保留该 expert 的本地贡献，逐 entry 将终端包写入对应 expert 的原始接收位置。
   同一目标卡出现三条后续 route，就执行三笔 expert payload 写入。
4. 指向 relay 本卡其他 expert 的 entry 同样计为转发，写入各自接收槽位并由对应监听 owner 消费。
   后续包统一使用 `forwardCount=-1`；即使全部后续 route 都在本卡，也执行相同转发流程。
   共用转发 UB 不等于卡级去重，不省略任何 expert 行，也不为单卡多 expert 改用旁路填充。

源 Server 内由原始 source 直接逐 route 分发，每条均为 DIRECT，`forwardCount=0`，forwardEntries 无有效元素。
即使同一 Token 命中源 Server 内多个 Rank 或本卡多个 expert，也不选 relay、不创建 group、不分配 forwardTokenIdx，
不创建中转 group 或转发返回记录。Combine 直接返回原始 source 的各 `[srcTokenId][k]` 槽位，sourceMask 保留这些原始 k。
同一个 Token 可以同时有源 Server 内的 DIRECT route 和面向远端 Server 的去重 group。
适用条件始终比较原始 source 与最终目标，远端 relay 收包后在其 Server 内的本卡/跨卡展开仍执行上述转发流程。

Group 表示同一原始 Token 在一个采用去重的目标 Server 上需要汇聚的 expert 贡献集合。
当 m>1 且所有 route 都指向 relay 本卡时，仍形成 group 并执行 m-1 次本卡转发。
本卡转发与跨卡转发共用编号和处理流程：第一跳 owner 在首次转发前为该 Token 原子取号一次，
所有转发副本携带同一个 `(relayRank,forwardTokenIdx)`。
Combine 将本卡转发所得 expert 行同样作为转发 Token 处理，执行结果返回及对应的监听/归约。
因此仅有本卡转发的 group 也使用同一 `forwardTokenIdx`，不设置单独的本地 group 编号或直接读取额外本地行的旁路。
持久表布局、行关联和写入分工已由 R04 确认；R04-05/R07-02/R07-03 已确认本卡与跨卡返回使用统一中转区，按 `(forwardTokenIdx,originalK)` 定位。

所有原始 expert cnt 由原始 source 统计并直接发送，包含经 relay 到达的 route。
Relay 不转发 cnt，不为代理写入另加 cnt；数据位置的 source 身份始终保持原始 srcRank。
Cnt 统计、发送记录、接收和 cumsum 计算沿用当前 Memory，统计输入仍是完整原始路由；核组采用本次运行时 send_core_num/recv_core_num 分工（默认 32/32）。
本版本不使用双 Group。
原始 source 的 cnt 按既有分核覆盖全部目标 expert，每项发送一次，随后进入原有接收/cumsum 流程。
源端数据的“先第一跳、后 DIRECT”仅为各核内部的任务顺序。

| 数量 | 计数单位 |
| --- | --- |
| BS | 每个原始 source 的输入 Token 数 |
| `tokenNum` | 本 Rank Combine 实际输入 Token 数，即本卡 cumsum 得到的 expert 接收行总数；同一 Token 命中本卡多个 expert 时计多行 |
| 实际 group 数 | 本 relay 需要监听返回的不同原始 Token 数，包含本卡转发；仅作为安全容量约束和内部任务数量，不生成独立计数接口 |
| Y | 转发 expert 结果条数，仅用于通信量分析；包含本卡返回，不生成独立接口 |

相同输入 BS 不代表各卡的 `tokenNum` 或实际 group 数相同。一个转发 Token 有多份 expert 结果时，
实际 group 数只增加一个，Y 按后续 route 数增加；每个 group 有一条第一跳及 1 至 K-1 条转发 route。
零 cnt 或整卡 `tokenNum=0` 是合法路由结果，不表示输入存在空 route，也不自动允许该卡跳过其他职责。

## 源端两阶段发送

源端数据发送按原始 Token 分核，一个 owner 处理该 Token 的全部 K 条 route，两阶段保持同一 Token 的处理归属。
两阶段仅表示每个发送核处理自己任务时的先后顺序，各核独立推进。
R02-01 已确认对本核 Token 区间扫描两遍：第一遍按 serverRouteMask 分组并发送全部第一跳，同时保存 directMask；第二遍按 directMask 发送全部 DIRECT。
先按 R01 区分源 Server 内 DIRECT、远端单 route DIRECT 和远端多 route group，再安排发送：

| 阶段 | 原始 source 发送的包 |
| --- | --- |
| 第一阶段 | 筛选远端多 route group，向每个 group 的首目标 relay 发送一个第一跳包，forwardCount>0 |
| 第二阶段 | 发送全部 DIRECT route，包含源 Server 内 route 和远端单 route，forwardCount=0 |

一个原始 Token 可以同时在两个阶段有发送任务；第一阶段发送其 group 第一跳，第二阶段发送其剩余 DIRECT route。
Group 内后续 route 已放入 forwardEntries，仍由目标 relay 展开，source 第二阶段不重复发送这些成员。
原始 expertIds 保持输入顺序和完整内容，筛选仅决定发送任务，不覆盖 q/cnt 使用的原始路由序列。
Source 为每个 group 按原始 k 升序生成有效 forwardEntries；relay 直接沿用该顺序生成密排成员记录，不另做排序。
每核将自己负责的全部第一跳发送任务下发后，随即处理自己的 DIRECT 任务。
两类任务之间不插入 PIPE_ALL、SyncAll 或其他阶段屏障，也不单独等待第一阶段的 MTE3 完成。
不同核无需对齐进度：一个核可以已在发送 DIRECT，另一个核仍在发送第一跳；没有第一跳任务的核可直接处理 DIRECT。
在途 MTE3 可以跨越这个逻辑边界，发送 UB/Event 的正常复用依赖仍按 R05 处理，不增加阶段级等待。
目标 relay 的监听/转发属于接收核组职责，source 进入 DIRECT 不等待目标展开完成。

Source owner 需要确定三类逻辑信息：

| 名称 | 含义与使用方 |
| --- | --- |
| sourceMask | 原始 source 在 Combine 最终需要消费的 k 槽位集合；DIRECT 保留原始 k，group 仅保留 returnK；每 Token 一个 32bit 值，由数据发送核在 UB 生成，核末一次写出到 sourceMask 输出 GM |
| 按 Server 分组 | 从本 Token 的 K 条 route 中识别每个远端 Server 的成员及首目标，用于生成第一跳和 forwardEntries |
| 第一跳发送信息 | 当前发送所需的目标 relay/expert/q、头部 k/weight、转发次数及 entries，可直接用于构造发送 UB |

旧议题中的“第一跳 descriptor”仅指第三项发送信息，不要求新增 descriptor tensor、持久表或控制包，
也不是 Combine 的 forwardPlan。其保存为临时变量、复用 UB 或重访原始路由的实现安排，不能预先当作新增 ABI。
例如 R=64、L=3、K=4，R0/T1 的 top-k 为 `[25,28,4,50]`：
第一阶段向 R8/expert25 发送第一跳并携带 R9/expert28 的 entry；
第二阶段向 R1/expert4 和 R16/expert50 分别发送 DIRECT；sourceMask 为 `0b1101`，保留 k0/k2/k3。

### 两遍扫描的分类表（R02-01，已确认）

| 临时值 | UB 字节数 | 使用方法 |
| --- | --- | --- |
| serverRouteMask[16] | 64B，每核一份 | 当前 Token 的 Server 分组表，bit k 对应原始 route k；处理下一 Token 前清零复用 |
| directMask[count_i] | align32(4*count_i) | 第一遍记录第二遍需要直发的 k，只在本核 UB 保存 |
| sourceMask[count_i] | align32(4*count_i) | 第一遍置 group 的首 k，第二遍置 DIRECT 的 k，核末写出有效 count_i*4B |

第一遍读取当前 Token 的 K 个原始 expertIds，按 `serverId=(expertId/L)/8` 将 `uint32(1)<<k` 并入对应位图。
源 Server 的所有 route、远端 Server 的单 route 并入 directMask；远端多 route 取最小原始 k 为第一跳，其余 k 升序构造 entries。
每个远端多 route Server 独立生成第一跳，一个 Token 可以生成多个第一跳；这些 group 的全部位都不进入 directMask。
头部和 entries 的 q 各自按原始 `srcTokenId*K+originalK` 调用 Memory 的 CalTokenSendExpertCnt；原始 expertIds 不变。
第二遍仅枚举 directMask 中的 k，不能用含有第一跳位的 sourceMask 代替 directMask。
分组表在有效 header/entries 已复制到受发送流水保护的 UB 后即可复用；两遍继续使用同一包序号和发送池。

例如 L=3，R0/T0 的路由为 `[E4,E25,E28,E50,E26,E5]`：第一遍向 R8/E25 发第一跳，entries 为 R9/E28、R8/E26；
directMask=`0b101001`，此时 sourceMask=`0b000010`。第二遍发 E4/E50/E5 后 sourceMask=`0b101011`。
BS=256 时每核 5 或 6 个 Token，directMask 对齐后 32B，连同 serverRouteMask 额外占用 96B/核；不新增 GM 表或 public 参数。

## 本卡数据发送流水

“数据发送”仅指原始 source 将本卡输入 Token 的第一跳或 DIRECT 包写到目标通信槽位，
由固定的 48 个数据发送核执行，目标可以是本卡或其他 Rank。收到第一跳后再展开 entries 的操作称为“数据转发”，
由另 16 个核负责；监听、LocalWindowCopy 和数据转发流水后续单独设计，不套用本节资源数。
复用 Memory 时按执行角色核对 UB/Event 生命周期，不能将其接收侧 buffer 或监听任务计入源端数据发送核。

### 单包发送顺序与 Memory 对应

R05-04 已确认单包数据发送直接沿用 Memory 的以下流水：

```text
MTE2 CopyIn -> 可选量化 -> 在数据 Tensor 尾部填写补充信息
           -> Vector 按 DataAsFlag 格式打包 -> MTE3 发送
```

非量化对应 Memory 的 TokenToExpert：CopyIn 到 xInTensor 后，直接在同一个 Tensor 的数据尾部填写信息。
量化对应 TokenToExpertInQuant：CopyIn 后量化到 quantTensor_，再在量化结果及随包 scale 的有效跨度对齐到 32B 后填写尾部；补充信息不参与量化。
两条路径随后都经原有 Vector Copy 将完整 Payload 打包到环形 outBuf，再按原有 Event 配对执行 MTE3 发送。
这里的“拼接 Flag”沿用 Memory 实现：发送池初始化时准备好 Flag 值，Vector 按每 480B Payload / 32B Flag 的间隔写入数据并保留 Flag 位置。

这条单包流水的变化集中在尾部：Memory 的 FillTriple 填写三个 int32、预留 32B；去重版填写已确认的 32B 头部及 forwardEntries，尾部预留为 align32(32+16*(K-1))。
据此重算 Payload、分块数、相关 Tensor 容量及通信跨度 T，CopyIn/量化/打包/MTE3 的先后关系和发送 Event 配对继续沿用。
不额外引入一个专用于拼尾部的 Tensor 或一次整包中间拷贝。路由分类、两遍任务顺序及核末 sourceMask/expertScalesOut 写出是外层职责，不插入这条单包流水。

### UB 申请与并发数

本卡数据发送沿用 Memory 的输入队列与环形发送 UB：输入队列配置两份输入 buffer，
通过 AllocTensor/EnQue/DeQue/FreeTensor 管理读取及 Vector 消费；量化工作区、路由/q 临时区、
sourceMask 和真实权重取用等源端所需 UB 先计入预算，再用剩余空间申请连续的 outBuf。
非量化的 xInTensor 或量化结果 quantTensor_ 与最终供 MTE3 读取的 outBuf 分开，沿用 Memory 已有的 Tensor 组织；处理完一份输入不表示相应 outBuf 已可覆盖。
临时区复用按 Memory 的 buffer pool 和最后使用者依赖处理，量化计算补齐仍按算法要求准备。
非量化的 inputTensorBytes_、量化结果的 quantTensorBytes_ 均按新的 packedPayloadBytes=blockCount*480 申请；
量化路径的原始输入 xInTensor 仍按原量化计算所需的 hAlignSize_ 申请，outBuf 每路按新的 T=blockCount*512 申请。
不能只增大 MTE3 发送长度而保留容纳旧尾部的 Tensor 容量。

每个数据发送核分别计算以下字节尺寸，`T` 使用去重版[尾部容量与通信长度](#尾部容量与通信长度)公式：

```text
dedupTokenStride = T
                = ceil((align32(D) + align32(32 + 16*(K-1))) / 480) * 512
remainUbSize = 本核 UB 预算 - 已申请的源端输入与临时区字节数
sendTokenBufNum = min(floor(remainUbSize / dedupTokenStride), 8)
outBufBytes = sendTokenBufNum * dedupTokenStride
outBuf = uint8[sendTokenBufNum, dedupTokenStride]
```

并发数上限固定为 8，随 UB 大小缩减，最小为 1；第一版输入保证至少可容纳一份完整去重 Token，
不新增 0 并发校验、报错或回退分支。此处的并发数是单核同时保留的发送 buffer 数，不代表独立物理 MTE3 引擎数。
第一跳和 DIRECT 即使有效 entry 数不同，也使用同一配置的 T。
BS=256、H=7168、K=6、FP16/BF16 非量化时 T=15872B；一份发送 UB 为 15.5KiB，八份共 124KiB，
这些数仅是 outBuf 占用，不含输入队列和其他源端临时区。

### 环形复用与 SetFlag/WaitFlag

每路发送 buffer 绑定固定的 UB 槽位及其 EventID。令 s 为本核已下发的源端通信包数，
下一份包使用 `lane=s%sendTokenBufNum`，UB 起点为 `outBuf+lane*T`。
同一原始 Token 可产生多份源端包，每实际下发一份包才递增 s；两阶段沿用同一个 s 和同一池 buffer。

Memory 源端代码以 lane 下标作为 `MTE3_V`、`V_MTE3` 两类事件的显式 ID，复用顺序如下：

| 步骤 | 流水/操作 | 完成与复用含义 |
| --- | --- | --- |
| 初始化 | 对每路执行 `SetFlag<MTE3_V>(lane)` | 为首次使用准备可消费的初始事件；DataAsFlag 信号区也须具有正确值 |
| 输入与拼包 | MTE2 读取输入；按需量化；Scalar 写固定头部和有效 entries | 非量化在 xInTensor 上使用 MTE2→S，量化在 quantTensor_ 上使用 V→S；Scalar 写后使用 S→V |
| 取得发送槽位 | `WaitFlag<MTE3_V>(lane)` | 在 Vector 覆盖 outBuf 前，消费该槽位上一次 MTE3 完成事件 |
| 构造传输记录 | Vector 将去重 Payload 按 480B 数据/32B Flag 间隔拷入 outBuf | 完整跨度为 T；不改写其他在途发送槽位 |
| 发布可发送数据 | `SetFlag<V_MTE3>(lane)`，`WaitFlag<V_MTE3>(lane)` | MTE3 读取该槽位前等待本次 Vector 写入完成 |
| 数据发送 | MTE3 将该槽位的完整 T 字节写至目标通信窗口 | 写出期间槽位保持只读 |
| 记录完成依赖 | 下发 `SetFlag<MTE3_V>(lane)`，递增 s 并继续后续任务 | 事件在此前 MTE3 完成后才置位；下发 SetFlag 不等于槽位已经空闲 |
| 核末回收 | 消费各已初始化槽位的末次 `MTE3_V` 事件 | 使用过的槽位等待最后一笔写出，未使用槽位消费初始事件，再结束该池生命周期 |

`MTE3_V` 保护下一次 Vector 覆盖；它不直接授权 Scalar 或 MTE2 改写同一槽位。
本基线中 Scalar 在 xInTensor 或 quantTensor_ 尾部改字段，再由 Vector 写入最终发送槽位，沿用 Memory 的写入流水和依赖。
Set/Wait 按相同 HardEvent 与 ID 配对，前一次事件未消费前不能重复置位；临时 FetchEventID 不等于占用一个在途 ID。
R05-04 按 Memory 收口：两类发送事件均使用 lane 作为显式 ID（0 至 sendTokenBufNum-1），不另申请 metadata 发送池。
尾部字段直接在 xInTensor 或 quantTensor_ 内由 Scalar 构造，再经 S_V 和原有 Vector 拷贝进入 outBuf；加长 Payload 不改变这组 Event 配对。

源端发送可在当前 MTE3 在途时准备后续包，环回到忙槽位时按上述 WaitFlag 正常等待；
不要求非阻塞完成查询接口，也不在每包发送后或第一跳/DIRECT 切换时增加 PIPE_ALL、SyncAll 或专门的 MTE3 等待。
核末正常回收发送资源后参与全核完成同步；见[Token 级 prefix 编号](#token-级-prefix-编号)。
上述流程参考 Memory 的 CalcSendTokenBufNum、TokenToExpert/TokenToExpertInQuant 与 SendToMoeExpert；
数据转发的多次写出、公共副本和监听前进性保留为 R05-05/R05-06 后续设计，不因复用源端流水而确认。

### 源端输出 UB 与核末写出（R05-04）

| UB | 申请大小 | 生产/消费与复用边界 |
| --- | --- | --- |
| routeWeightIn | align32(count_i*K*4) | 沿用 Memory Combine ExpertScaleCopy 的连续区间拷入方式，从 expertScales[start_i*K] 读取；MTE2_S 配对后 Scalar 取值，两遍只读 |
| sourceWeightOut | align32(count_i*K*4) | Scalar 按原始 Token/k 写 DIRECT 真实权重、group 首 k 的 1.0；mask 空洞无需初始化，保持到核末 GM 写出完成 |
| sourceMask/directMask | 各 align32(count_i*4) | 分类和持久输出职责见 R02-01；仅 sourceMask 写 GM |
| serverRouteMask | 64B | 当前 Token 的临时分组，不参与 MTE3 读取 |

这些 UB 在 CalcSendTokenBufNum 计算剩余空间前申请；其他输入、q、量化临时区和 outBuf 仍按 Memory 分配/复用。
只拷入本核 Token 区间的权重，不因新增权重再复制一份全量 `[BS,K]`。
sourceMask 与 sourceWeightOut 均由 Scalar 填写，核末按以下顺序写出：

1. 两遍发送完成，按 Memory 消费各 lane 末次 MTE3_V 事件，完成发送池的正常核末回收。
2. 沿用 `SyncFunc<S_MTE3>()` 的即时 Set/Wait 配对，使 Scalar 填写的两个输出 UB 对 MTE3 可见。
3. 用两次 DataCopyPad 分别写 `sourceMask[start_i]` 的 count_i*4B、`expertScalesOut[start_i*K]` 的 count_i*K*4B；不写 UB 对齐填充。
4. 用一次即时 `SyncFunc<MTE3_S>()` 完成这批核末写出，再结束相关 UB 生命周期并进入已确认的算子末尾同步。

S_MTE3/MTE3_S 与环形发送的 V_MTE3/MTE3_V 是不同 HardEvent；即时 Fetch、Set、Wait 在返回前消费事件，不保留额外在途 ID。
因此无需增加发送 lane、每 Token 输出事件或新的发布协议。无 Token 的发送核跳过上述输出拷贝，仍完成其 cnt 和核末职责。
这里的等待位于本核全部发送任务之后，不插入第一跳/DIRECT 两遍之间。依据为 Memory 的 TokenToExpert、SetValidExpertInfo 的 Scalar 输出写回及 SyncFunc。
源码依据见 [TokenToExpert](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L693)、[SetValidExpertInfo](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L1433) 和 [ExpertScaleCopy](../../src/ep/kernels/tilexr_ep_combine_memory_kernel.h#L849)。
R05-04 无剩余设计选择；代码接入与设备编译验证属于实现工作，接收/数据转发的 UB/Event 仍归 R05-05/R05-06。

## Dispatch 接收布局

Cnt 和数据分区沿用无共享专家的 Memory 布局。记 s 为原始 source、d 为目标 Rank、
e 为目标本地 expert 下标、L 为每卡 expert 数，T 为加长后的 Dispatch 通信记录字节跨度：

```text
cntRecordIndex = e * R + s
dataRegionIndex = ((s + d) % R) * L + e
q = number of earlier Tokens from source s routed to this expert

receiveAddress = dispatchDataBase[d]
               + (uint64(dataRegionIndex) * BS + q) * T
```

q 是原始 source 发往该 expert 的从零开始的序号，由 source 预先计算；
它不是 srcTokenId，不同 expert 的 q 可以相同。目标 expert/source 分区的 cnt=c，
就对应 q 在 `[0,c)` 的 c 个接收位置。第一跳、DIRECT 和 relay 代理写入都使用这一布局，
每个位置只有一个指定写入者。
q 的计算沿用 Memory `CalTokenSendExpertCnt`，传入原始 `index=srcTokenId*K+originalK`，
统计完整原始 expertIds 在 `[0,index)` 中等于目标 expert 的条数。Top-k 不重复保证同一 Token 的前序 k 不贡献重复计数。
按 Token 分核和两阶段乱序发送只改变执行顺序，不改变这个前缀或 q；forwardEntries 的每条 q 同样在 source 计算。
不得使用筛选后的任务下标、已发送条数或每阶段重置的计数器代替原始 index。

第一跳与终端包使用相同的 T。新增尾部只增加通信记录长度，不改变 hidden 的 H、cnt 或最终 expert 行数。
本轮每条 route 独占一个接收槽位，不采用逐 Token credit、槽位申请或应答来循环复用该位置。

接收端按本地 expert/source 顺序生成包含当前项的 cumsum，某段之前的累加值加 q 得到其最终输出行。
例如 cnt=`[2,0,3]`，cumsum=`[2,2,5]`，各段起始行为 `[0,2,2]`。
第三段 q=1 对应输出行 3，但通信窗口中的物理位置仍按上式确定。

原始 cnt 表示“应收多少”，cumsum 表示“最终放在哪”，到达 Flag 表示“数据能否读取”。
即使 cumsum 完成，也必须确认 hidden 和全部有效尾部到达后才能消费 Token。
零 cnt 仍沿用 Memory 的计数通知，后续不生成该空区间的 Token 监听任务。

## Dispatch/Combine 公共接口与 metadata shape

去重入口沿用 Memory 的基础参数、输入输出和调用生命周期；在此基础上只新增三类跨 Dispatch/Combine 生命周期保存的 metadata 输出/输入：
`assistEx`、`sourceMask` 和 `group`。不新增 X 参数，也不新增 group 注册、descriptor、metadata-ready 或逐 Token 控制包。
旧接口中保留但首版不使用的参数仍按不支持场景的接口占位处理，不因此增加内部计算或数据结构。
权重采用独立的输入、输出参数及 GM buffer；复用 Memory 的 FP32 `[BS,K]` 索引及 Combine 权重参数语义。
Dispatch 的 expertScales 为只读真实权重输入，expertScalesOut 为 source 归约权重输出；二者不重叠。
调用方将 expertScalesOut 传入 Combine 已有的 expertScales 参数；不另增 Combine 权重参数、权重快照或 partialMask。
三个 metadata 输出仍为 assistEx/sourceMask/group；独立权重输出 expertScalesOut 另列，不能再把“三项 metadata”解释成 Dispatch 只新增三个输出参数。
首版权重输入必须非空，Host 对 nullptr 直接报错。当前空入口已落实非空输入；独立输出 ABI 的具体契约见下节 R10-01，代码尚待接入。

| 权重参数 | 方向 / shape | 含义 |
| --- | --- | --- |
| Dispatch expertScales | 输入，FP32 `[BS,K]`，每元素 4B | 真实 route weight，只读，第一跳/DIRECT/entries 均从此取值 |
| Dispatch expertScalesOut | 独立输出，FP32 `[BS,K]`，共 `BS*K*4B` | DIRECT 为真实值，group 的 returnK 为 1.0，sourceMask 为 0 的位置不消费 |
| Combine expertScales | 沿用已有输入，FP32 `[BS,K]` | 指向 Dispatch 的 expertScalesOut，按 sourceMask 统一加权 |

### 权重输出的 public / launch ABI（R10-01）

Memory 提供的复用依据是普通指针输出、Host 参数结构、按值 GM_ADDR launch 参数及 Combine 的 float* 权重输入；
当前 Memory Dispatch 没有可直接当作 source 归约权重输出的参数，不能将其 expertScales 输入或 expandScalesOut 占位改作该输出。
本轮给出以下确定接口，后续按此接入代码，不再作为格式选择题：

| 层级 | 具体约定 |
| --- | --- |
| 公共 Dispatch | 在现有 groupOut 后、dtype 前增加 `float *expertScalesOut`；expertScales 输入位置和只读语义保持 |
| Host 参数 | EpMemoryServerDedupParams 增加 `float *expertScalesOut`，包装函数直接赋值；quantMode/dynamicScalesOut/expandXOutDtype 继续沿用已存在的 dispatch 字段 |
| shape / 分配 | 输入和输出分别为连续 FP32 `[BS,K]`，各需 BS*K*4B；现有 layout.weightBytes 可用于两份独立分配，无需新增 shape 参数 |
| 基础参数检查 | 输出非空、4B 对齐，与输入及其他在用 buffer 不重叠；沿用 caller allocation 检查，裸指针接口不推断实际分配长度，不新增权重和校验 |
| launch 参数结构 | groupOut 后、bs 前增加 `GM_ADDR expertScalesOut`；同时按 Memory 在 expandXOut 后加入 `GM_ADDR dynamicScalesOut`，expertTokenNumsType 后、dtype 前加入 `int64_t quantMode` |
| Device 入口 | 与 launch 结构保持相同顺序，Init 将 expertScalesOut 绑定为 GlobalTensor<float>，dynamicScalesOut 绑定为 GlobalTensor<uint8_t>；使用 quantMode/expandXOutDtype 选择非量化或 MXFP8 模板 |
| 布局断言 | 按当前 64 位地址 ABI，共 13 个 GM_ADDR + 10 个 int64，大小 184B；dynamicScalesOut offset=40，expertScalesOut offset=96，bs offset=104，quantMode offset=152，magic offset=176 |
| Combine 调用 | 把该输出传给已有 `float *expertScales` 输入，不增加第二份 Combine 权重参数 |
| 提交和生命周期 | 沿用现有注册二进制、按值 launch、stream 及 magic；权重输出由调用方持有到 Combine 消费完成，不加 Host 同步或布局 handle |

源码依据见 [现有去重 launch 结构](../../src/ep/host/ep_memory_server_dedup_launch.h#L12) 和 [Memory launch 结构](../../src/ep/host/ep_kernel_launch.cpp#L283)。
ABI 接入需同步公共声明、包装、Host 校验、launch 结构/初始化、Device 入口和调用测试；不得只改其中一层。
新增的内部量化地址/模式直接消费已有 public 参数，不新增第二套量化 scale 输出或模式参数。
本节是已收口的设计，当前空 kernel 尚不生成 expertScalesOut，也尚未接入上述权重输出及量化 launch 参数。

### 有效行数与 metadata

这里的 `tokenNum` 是当前 Rank Combine 的有效输入 Token 数，由现有 `sendCounts` 的有效计数确定，
不是独立新增的 public scalar；它与 `expertOut` 的有效行数和 `assistEx` 的第一维相同。
首版不含共享专家，沿用 Memory 的 `tokenNum=sendCounts[L*R-1]`，按已有容量分配、按有效行数消费。
R04-02 已确认沿用 Memory 输出行序：`expertOut[row]` 与 `assistEx[row]` 一一对应，
expert 内部若重排，须在交给 Combine 前还原 Dispatch 行序；不新增 public row map，不消费容量中的 padding 行。

| metadata | public shape / stride | 作用 |
| --- | --- | --- |
| `assistEx` | `int32/FP32` 位视图 `[tokenNum,8]`，每行 32B，stride 32B，总字节数 `tokenNum*32B` | 描述每个 Combine 输入 Token 行的 source、originalK、角色、weight 和返回引用 |
| `sourceMask` | `uint32[BS]`，stride 4B，总字节数 `BS*4B` | 描述每个原始 source Token 最终需要消费的 k 槽位 |
| `group` | `uint32[C,rowStride/4]`，C=R*BS*min(K,L)，`rowStride=align32(4*(1+2*(K-1)))` | 保存每个 relay group 的 firstHopRow 和前 n 个 `(recvSlot,weight)` member |

`group` 的第一维是安全容量上限，不代表实际 group 数；实际有效行通过有效第一跳 `assistEx` 关联，
在 `[0,tokenNum)` 中以 `forwardCount>0` 识别第一跳，取 `t=assistEx[row].forwardTokenIdx` 访问 `group[t]`，
且 `group[t].firstHopRow=row=dstPosition`。此关联已由 R04-03 确认，不提供 X，也不扫描未写入的 C 行。
R04-05/R07-02/R07-03 已确认 `recvSlot=uint64(t)*K+originalK`，本卡及跨卡返回共用 `[8*BS,K]` 个结果槽位，来源卡不参与编号。
`C` 是本 relay 的 group 可寻址行容量；`assistEx[tokenNum,8]` 和 `sourceMask[BS]` 的第一维保持各自既有语义。

R04-04 沿用 Memory 的算子完成语义：Dispatch 所有核完成全部任务及 GM 写入后，Dispatch 算子才结束；
后续 Combine 据此读取完整的三个 metadata 输出及 Dispatch 输出的 expertScalesOut，不另设元数据发布协议、ready Flag 或额外同步。
这里的完成指设备侧算子完成；Host 异步提交返回仍沿用 Memory 的既有调用语义。

Token 尾部的固定 header、`forwardEntries` 和对齐填充属于 Dispatch 内部通信 Payload：
它们随 Token 一起经过 DataAsFlag 发送，接收端剥离后用于生成上述持久 metadata，
不作为额外 public output/input，也不在 Dispatch/Combine 之间长期保存整份 Payload。

## Token 格式与转发

### 随 Token 存放

阶段一已确认：补充信息直接接在本 Token 的数据后，作为同一条加长 Token 记录一起发送，
在该 Token 的原有接收槽位内存放，不额外划分转发信息区域，也不单独发送 metadata 包。
数据拷入 UB 后，如需量化则先完成量化，再以待发送数据的有效字节长度向上对齐到 32B；
补充信息整体也向上对齐到 32B，然后追加到已对齐的数据尾部。插入到达 Flag 前的 UB 布局为：

```text
[拷贝/量化后的 Token 数据][数据段 32B 对齐填充][补充信息固定头部][forwardEntries][信息段 32B 对齐填充]
```

补充信息固定头部包含下表中的 srcRank 至 forwardTokenIdx，随后是 entries 的有效内容及配置预留容量。
DIRECT、第一跳、本卡转发和跨卡转发均遵循这一存放方式，并使用同一配置的 Token stride T。
数据段和补充信息段分别对齐后相加，不只对二者原始长度之和做一次对齐。
这是补充信息整体的 32B 对齐；固定头部单字段为 4B，entry 已确认每条 16B，字段间不额外补齐到 32B。
对齐后的 Token 数据段和补充信息段统一视作一个 Payload，整体沿用 Memory Dispatch 的 DataAsFlag 拼接、检测和剥离方式。
具体长度公式见[尾部容量与通信长度](#尾部容量与通信长度)，收发流程见 [Payload 与 DataAsFlag](#payload-与-dataasflag)。
接收端从 Token 尾部提取 Combine 所需信息并写入持久表；assistEx/group 的布局、行关联、写入分工及 recvSlot 编码已确认，完整区域预算继续在 R07-01 讨论。

### 逻辑字段

保留原始身份、头部 k/weight、三态 forwardCount 和逐 route 列表；
新增公共返回引用 `(relayRank,forwardTokenIdx)`。
终端包必须保留原列表，不能用返回引用覆盖 `forwardEntries[0]`。

固定头部字段排列已与 assistEx 的 32B 布局共同确认；forwardEntries 条目布局也已确认：

```text
srcRank, srcTokenId, k, weight, forwardCount,
forwardEntryCount, relayRank, forwardTokenIdx, forwardEntries

entry = {destinationExpertId, destinationQ, originalK, weight}
```

下面给出逐字段字节表。固定头部的八字段、int32/FP32 类型、字段顺序、offset 及未使用返回字段填 -1 已确认。
逻辑身份、逐 route 的 k/weight、forwardCount 三态、原列表保留和公共返回引用的语义已确定；
forwardEntries 的 16B 布局、固定 K-1 容量和无效区域不清零已由 [R03](ep-memory-server-dedup-open-topics.md#r03) 确认并关闭。

记 K 为配置中的总 top-k 数，k 为一条 route 在原始 top-k 中从零开始的位置。
头部携带的是小写 k，不另加一个表示总数的大写 K 字段；接收端通过本次调用配置取得 K。
第一跳绑定的 route 不再列入 entries；某个远端 Server 有 m>1 条 route 时，有效 entry 数 `n=m-1`，
满足 `1<=n<=K-1<=31`。DIRECT 的 n=0，第一跳的返回编号尚未由 relay 分配。
K-1 仅表示最大容量，不能代替某个目标 Server 的实际 n。

### 尾部字段字节表

固定头部已确认使用 4B 字段：整数为 `int32_t`，weight 为 FP32，各字段按 4B 对齐，字段间不插入 padding。
尾部起点为待发送数据有效长度向上对齐到 32B 后的位置，尾部整体长度也向上对齐到 32B。
下表 offset 从逻辑尾部首字节开始，单位均为 B；对应未插入 Flag 的 UB 偏移为 `tailOffset+offset`。
表内 offset 不包含数据段及其对齐填充，也不包含到达 Flag。
DataAsFlag 在 Payload 分块之间插入 Flag，因此这些 offset 不能直接当作通信窗口中的物理偏移；剥离 Flag 后可恢复原 UB 偏移。

| 字段 | 类型 | 大小 | 尾部 offset | 含义、单位与有效范围 |
| --- | --- | --- | --- | --- |
| `srcRank` | `int32_t` | 4B | 0 | 原始输入所属 Rank，范围 `[0,R)`；经 relay 转发后仍是原始 source，不改为 relayRank |
| `srcTokenId` | `int32_t` | 4B | 4 | 原始 source 上的输入 Token 行号，范围 `[0,BS)`；用于 Combine 恢复原始 Token |
| `k` | `int32_t` | 4B | 8 | DIRECT 当前 route 或第一跳 route 的原始 top-k 位置，范围 `[0,K)`；终端包保留第一跳值 |
| `weight` | `float`（FP32） | 4B | 12 | 与头部 k 对应的一个 expert route 权重，单位为标量权重；不是整个 Token 共用的权重，终端包仍保留第一跳值 |
| `forwardCount` | `int32_t` | 4B | 16 | 0 表示 DIRECT；正数 n 表示第一跳有 n 次后续 expert 发送；-1 表示终端包，不再转发。计数包含本卡转发，必须能编码负值 |
| `forwardEntryCount` | `int32_t` | 4B | 20 | 原始列表的有效条目数 n，范围 `[0,K-1]`；forwardCount 改为 -1 后仍保留 n，使终端只解析有效 entries |
| `relayRank` | `int32_t` | 4B | 24 | Combine 应返回的中转 Rank；终端包有效范围 `[0,R)`，由第一跳 owner 填本 Rank。本卡转发同样填写；未使用时填 -1 |
| `forwardTokenIdx` | `int32_t` | 4B | 28 | 第一跳 owner 的零基 Token prefix 输出行 t=row；所有转发副本共用，用于 Combine 定位 group/返回槽位。未分配时填 -1 |
| `forwardEntries` | entry 数组，固定 K-1 条容量 | 每条 16B；有效部分 `16*n` B | 32 | 每条对应一次后续 expert 发送；有效范围 `[0,n)`，排除第一跳 route。转发时原列表完整保留，预留容量见下表 |

固定头部合计 `8*4=32B`。有效 `forwardTokenIdx` 落在 relay 本轮 `[0,tokenNum)`，由 C 容量覆盖；不生成或传递 X。
`forwardTokenIdx` 等于第一跳输出 row，不能用 srcTokenId、段内 q、原始 k 或终端自己的 row 替代。
以上范围描述字段语义，不增加已由输入契约排除的逐 Token 安全扫描。
头部与 assistEx 的 weight 使用 FP32、group 由 relay 执行加权求和均已确认；公共 dtype 契约归 R08，内部计算精度及数值验收归 Combine 文档。

### 各类包的字段值

DIRECT 同时覆盖源 Server 内的逐 route 发送和远端 Server 仅有一条 route 的发送。
第一跳专指远端 Server 的 m>1 分组；终端包包含 relay 向本卡或同 Server 其他卡发送的全部后续副本。
表中未使用返回字段填 -1、DIRECT 有效长度填 0，均为已确认的头部初始化约定；不把 -1 当成有效 Rank 或编号读取。

| 字段 | DIRECT | source 发往 relay 的第一跳 | relay 发出的终端公共副本 |
| --- | --- | --- | --- |
| `srcRank` | 原始 source | 原始 source | 原样保留 |
| `srcTokenId` | 原始 Token 行号 | 原始 Token 行号 | 原样保留 |
| `k` | 当前 route 的原始 k | 该目标 Server 首条 route 的原始 k | 保留第一跳 k；当前接收 expert 的 k 从匹配 entry 取得 |
| `weight` | 当前 route 的 weight | 第一跳 route 的 weight | 保留第一跳 weight；当前接收 expert 的 weight 从匹配 entry 取得 |
| `forwardCount` | 0 | `n=m-1`，n>0 | -1；所有副本相同 |
| `forwardEntryCount` | 0 | n | 保留 n，不改为 -1，也不随已下发任务减少 |
| `relayRank` | 未使用，填 -1 | 不读取，填 -1；第一跳接收端知道自身 Rank | 当前 relay 的 Rank，所有副本相同 |
| `forwardTokenIdx` | 未使用，填 -1 | 尚未分配，填 -1 | 首次转发前一次取号得到 t，所有副本相同 |
| `forwardEntries[0:n]` | 无有效元素 | n 条后续 expert route | 完整保留同一份 n 条记录，不按目标裁剪、不删除已下发条目 |
| 容量内未使用的 entries / 纯传输 padding | 不清零，接收端不解析 | 不清零，接收端不解析 | 随完整 Payload 打包发送，不初始化、不参与匹配或计算 |

头部的 forwardCount 描述包的角色和第一跳初始任务数；发送进度由 owner 的临时状态保存。
不能通过修改公共包中的 forwardEntryCount 或逐项移除 entries 来记录进度，否则会破坏后续接收端的匹配依据。

### ForwardEntries 条目字节表

每个 entry 的字段布局已确认为下表，i 表示从零开始的 entry 下标，`0<=i<n`。
“条目内 offset”相对本条首字节，“尾部 offset”相对整个逻辑尾部首字节，单位均为 B。

| 字段 | 类型 | 大小 | 条目内 offset | 尾部 offset | 含义与使用方式 |
| --- | --- | --- | --- | --- | --- |
| `destinationExpertId` | `int32_t` | 4B | 0 | `32+16*i` | 此次后续发送的全局 expert ID，范围 `[0,moeExpertNum)`；目标 Rank=`id/L`，localExpert=`id%L`，无须再存独立目的 Rank 字段 |
| `destinationQ` | `int32_t` | 4B | 4 | `36+16*i` | 原始 source 发往该 expert 的从零开始的槽位序号 q，由 source 按 Memory 原始路由前缀计算；范围 `[0,cnt)`，有效 q 小于 BS。relay 用它计算接收地址 |
| `originalK` | `int32_t` | 4B | 8 | `40+16*i` | 此 expert route 在原始 top-k 中的位置，范围 `[0,K)`；用于恢复该 expert 行的 k 以及 Combine 返回信息，不等于 entry 下标 i |
| `weight` | `float`（FP32） | 4B | 12 | `44+16*i` | 与本条 originalK 对应的一个 expert route 权重；终端 expert 匹配本条后提取，不使用公共头部的 weight |

单条 entry 合计 16B，包含转发寻址及终端恢复自身 route 所需的信息；公共返回引用仅在头部存一份。
第一跳和 DIRECT 当前包的目的 expert/q 由接收槽位表达，不额外在头部重复携带。
终端同样从监听槽位取得自身 expert，在 `[0,forwardEntryCount)` 中唯一匹配；无需读取未使用容量。

### 尾部容量与通信长度

记 C 为一份通信记录预留的 entry 容量，已确认 `C=K-1`；n 为当前包的有效条数。
记 D 为数据拷入 UB、并完成可选量化后，待发送数据从起点到最后一个有效字节结束位置的跨度。
非量化时 `D=H*sizeof(XType)`；量化时按实际输出布局计入量化值、随包 scale 及其格式内间隔，
不使用量化前输入长度、量化计算 scratch 大小或整个 UB buffer 的申请容量代替 D。
补充信息不再参与 hidden 量化，按其自身字段编码追加。定义 `align32(x)=ceil(x/32)*32`，单位均为 B：

| 项目 | 字节数 | 说明 |
| --- | --- | --- |
| 固定头部 | 32B | 八个 4B 字段，三种包使用相同头部位置 |
| 一条 entry | 16B | 三个 4B 整数加一个 FP32 weight |
| 数据段及其对齐填充 | `align32(D)` B | 已确认先按拷贝/量化后的有效数据跨度对齐；填充长度为 `align32(D)-D` |
| 尾部起始 offset | `tailOffset=align32(D)` | 相对未插入 Flag 的 UB Token 起点；三类包从同一配置规定的尾部起点解析 |
| 当前包有效尾部 | `32+16*n` B | 不含预留 entry 和对齐填充；DIRECT 为 32B，第一跳和其所有终端副本均为同一个 n |
| 配置预留尾部原始长度 | `32+16*C` B | 按 `C=K-1` 固定预留；n<C 时剩余位置不含有效 entry |
| 对齐后尾部占用 | `align32(32+16*C)` B | 补充信息整体 32B 对齐，段末填充长度为 `align32(32+16*C)-(32+16*C)` |
| Payload 长度 | `tokenPayloadBytes=align32(D)+align32(32+16*C)` B | 对齐后的 Token 数据加补充信息；已包含两段 32B 对齐填充，尚未补齐 480B 分块或插入 Flag |
| DataAsFlag 数据块数 | `blockCount=ceil(tokenPayloadBytes/480)` | 以整个 Payload 计算，覆盖转发信息及其预留容量 |
| 补齐分块后的数据长度 | `packedPayloadBytes=blockCount*480` B | 最后一块补齐到 480B；剥离 Flag 后每条记录的 UB 跨度沿用 Memory 的这一长度 |
| Flag 总长 | `blockCount*32` B | 每块的 480B 数据后紧跟 32B Flag |
| Dispatch 通信 stride T | `blockCount*512` B | DataAsFlag 打包后的整条记录；同一配置的 DIRECT、第一跳和终端包使用同一 T，不能随 n 改变接收槽位跨度 |

按已确认的 `C=K-1` 预留，各配置的尾部容量如下。原始长度反映字段容量，对齐后占用反映拼接进 UB 的段长：

| K | C（最多条目数） | 固定头部 | entries 预留 | 原始长度 | 32B 对齐填充 | 对齐后占用 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 0 | 32B | 0B | 32B | 0B | 32B；只有 DIRECT |
| 6 | 5 | 32B | 80B | 112B | 16B | 128B |
| 8 | 7 | 32B | 112B | 144B | 16B | 160B |
| 16 | 15 | 32B | 240B | 272B | 16B | 288B |
| 32 | 31 | 32B | 496B | 528B | 16B | 544B |

例如 K=6、某个远端 Server 只有三条 route，n=2，第一跳及其终端包的有效尾部均为 64B；
按 C=5 预留，尾部原始长度为 112B，32B 对齐后占用 128B，包含 64B 有效字段、48B 未使用 entry 和 16B 对齐填充。
同配置 DIRECT 的有效尾部为 32B，但仍保留 128B 对齐后容量和相同 T；有效长度不等于实际传输字节数。

两段 32B 对齐后的内容统一构成 Payload，已确认沿用 Memory Dispatch 每 480B 数据加 32B Flag 的 DataAsFlag 方式：

```text
dataAlignedBytes = align32(D)
tailRawBytes = 32 + 16 * C
tailAlignedBytes = align32(tailRawBytes)
tailOffset = dataAlignedBytes
tokenPayloadBytes = dataAlignedBytes + tailAlignedBytes
blockCount = ceil(tokenPayloadBytes / 480)
packedPayloadBytes = blockCount * 480
packingPadBytes = packedPayloadBytes - tokenPayloadBytes
flagBytes = blockCount * 32
T = blockCount * 512
```

BS=256、H=7168、K=6、hidden 为 FP16/BF16 且 C=5 时，D=14336B，数据段已满足 32B 对齐；
尾部从 UB offset 14336 开始，原始 112B 对齐为 128B，Payload 长度为 14464B。
DataAsFlag 共 31 块，补齐分块后的数据长度为 14880B，其中分块填充为 416B；Flag 合计 992B，T 为 15872B。
与未计尾部对齐时相比，本例分块数不变，不能据此在其他配置中省略对齐。
BS 不贡献单个 Token 的尾部长度。未使用 entry、数据段/信息段的纯传输填充和末块分块填充均不清零，
只按 n 解析有效 entries，按有效 hidden/scale 长度提取数据；无效字节不参与匹配、寻址或数值计算。
固定头部和前 n 条 entry 仍须正确写入，每个 DataAsFlag 信号区仍按完整 blockCount 准备和检测。
量化算法会读取的计算补齐区按算法要求初始化，不属于可忽略的传输填充。
不清零不减少传输长度，发送 UB、通信槽位和剥离 UB 仍分别按 T 或 packedPayloadBytes 分配。

**Memory 参考依据：** [Dispatch Memory](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h) 的 `SetTilingDataAndCal`
先将数据字节跨度按 `UB_ALIGN=32` 对齐，以 tokenQuantAlign_ 表示 int32 单位的尾部起点，再追加旧 32B 元数据段。
量化路径的数据跨度为 `ceil(hOutSize_/256)*256+scaleOutBytes_`，覆盖量化值的格式内对齐及其后的 scale；
`TokenToExpertInQuant` 先执行 `QuantMxfp8`，再用 `FillTriple` 追加元数据。非量化路径在拷入 UB 后追加。
去重方案复用“按处理后数据跨度定位尾部”的原则，将旧元数据段替换为独立 32B 对齐的新补充信息段。
R08-03 已确认 BF16/FP16 的非量化和 MXFP8 支持范围及上述格式复用；去重版量化路径的代码接入和验证仍需完成。

### Payload 与 DataAsFlag

已确认 Token 数据和转发信息共同组成一个 Payload；数据段、信息段及其对齐/预留内容按连续字节顺序统一分块。
分块边界不因 hidden、scale、固定头部或 entry 的语义边界而改变。通信窗口中的布局为：

```text
[Payload 块 0: 480B][Flag 0: 32B][Payload 块 1: 480B][Flag 1: 32B]...
```

最后一个数据块不足 480B 时补齐，Flag 仍占完整 32B；整条 Token 的物理跨度为 `blockCount*512`。
DIRECT、第一跳和所有终端副本均使用这一方式，不为转发信息另建 Flag 区域或发送单独通知。

| 步骤 | 沿用 Memory Dispatch 的方式 | 去重版的长度与解析范围 |
| --- | --- | --- |
| 拼接并发送 | 在发送 UB 中按每块 480B 数据加 32B Flag 构造完整记录，沿用既有 Flag 值、发送前依赖及 MTE3 发送方式 | 使用整个 Payload 计算 blockCount 和 T，转发信息与数据一并进入数据块 |
| 检测 | 按块读取对应 Flag，沿用现有归约判定方式，确认待处理 Token 的所有块已到达 | 覆盖该 Token 的全部 blockCount 个 Flag，范围包含转发信息所在块；不能仅按 hidden 长度检测 |
| 剥离 | 每块取出 480B 数据并跳过其后的 32B Flag，依次恢复为连续 UB 数据 | 每 Token 的剥离后跨度为 packedPayloadBytes；前 tokenPayloadBytes 为完整 Payload，其余为分块填充 |
| 解析 | 在剥离后的 Payload 中分别读取数据与尾部信息 | 数据/scale 按原输出格式消费；从 tailOffset 解析固定头部和有效 entries，保存 Combine 所需信息或准备公共转发副本 |

对应源码为 `TokenToExpert` / `TokenToExpertInQuant` 的分块拷贝发送、`CheckDataArriveWithFlag` 的 Flag 检测，
以及 `CopyInAndOut` 按 480B 数据、32B 源间隔剥离 Flag 的拷贝。复用的是这些打包、检测和剥离规则，
监听任务分配、MTE2/MTE3 并行及 UB/Event 生命周期仍按本设计 R02/R05 执行，不恢复旧 LocalWindowCopy 的调度。
后续按 R12 验证新 Payload 长度贯穿发送、检测和剥离路径；实现验证不再作为选择另一套 Flag 协议的待确认项。

### 公共转发副本

第一跳 owner 保存原始 n、第一跳本地行信息及处理进度。
只要 n>0，relay 就在第一次转发前取得本 Token 的 forwardTokenIdx，包含所有转发都在本卡的情况。
公共转发 UB 只准备一次：将 forwardCount 设为 -1，填入公共返回引用 relayRank/forwardTokenIdx，
保留头部 k/weight 和全部有效 entries。此后各笔 MTE3 使用同一份只读数据，仅改变目标地址。

接收端从自己监听的槽位知道 localExpertId，从而恢复：

```text
myExpertId = myRank * L + receivedLocalExpertId
myEntry = matching entry in the valid forwardEntries range
myK = myEntry.originalK
myWeight = myEntry.weight
```

Top-k 不重复保证该 expert 唯一匹配。这是正常 route 解析，不是新增输入安全检查。
终端接收端使用匹配 entry 的 k/weight，不把公共头部的第一跳 k/weight 当作自身 route。
它只处理当前 expert 行，不因收到完整列表而再展开或转发其他条目。

原始 q、原始 TokenId、entry 下标与原始 k 的含义不同，不能互相替代。
例如一个 Server 只包含原始 k2/k5，列表下标不直接等于原始 k。

### 发布与存活期

一条逻辑发送包含统一 Payload 及其 DataAsFlag 分块 Flag；不另发 group 注册、plan、metadata-ready、
逐 Token 完成通知、应答或 credit。原始 cnt、核内 Event、cumsum 发布/消费和必要的调用同步仍保留。
源端第一跳与 DIRECT 两类发送之间不增加同步。

Token 的全部 Payload 块共用 Memory Dispatch 的 DataAsFlag 到达协议，覆盖数据、固定头部、entries 和公共返回引用。
按全部块的 Flag 判定到达后，剥离 Flag 并解析 Payload，再处理当前 expert/转发任务。
拼接、检测和剥离方式已确认；一次逻辑发送不表示整包原子可见，仍按既有检测和流水依赖消费完整记录。

公共转发 UB 从首笔转发使用起保持只读，直到全部相关任务下发且最后一笔引用它的 MTE3 完成。
某个目标完成、待处理表剔除或 PIPE_MTE3 排序，都不单独证明 UB 已可由 Scalar/Vector 覆盖。
尚未下发完的 entry 和已分配编号必须持续有效，恢复处理时不重复发送或取号。

## 分核与监听

总核数为 64，默认 32 核负责本卡源端数据发送，另外 32 核负责 cnt 接收/cumsum、LocalWindowCopy 和数据转发。
发送核数 S 支持 16/24/32/40/48，接收核数为 64−S；不单独分出专职转发核，cnt/q 的数值算法沿用 Memory。

| 核组 | 核数 | BS<512 | BS>=512（包含 BS=512） |
| --- | --- | --- | --- |
| 本卡源端数据发送 | S（默认 32） | 先下发本核 cnt，再处理本核第一跳/DIRECT 任务 | 不发送 cnt，直接处理本核数据发送任务 |
| cnt 接收/cumsum/LocalWindowCopy/数据转发 | 64−S（默认 32） | 参与全 64 核的 cnt 发送分工，再接收 cnt、计算 cumsum、监听及 copy/转发 | 全部 cnt 发送在接收核组分配，再接收 cnt、计算 cumsum、监听及 copy/转发 |

Cnt 按目标 expert 范围分配，与源端 Token 任务独立；小 BS 下无源端 Token 的核仍完成分配到的 cnt。
“先 cnt 后数据”是每核自身顺序，不等待所有核 cnt 完成，不增加核间阶段屏障。
全核发送 cnt 不意味着全核算 cumsum 或监听数据；状态/Flag 分区和容量须适配本次接收核数，具体布局归 R07-01。

源端 BS 在 S 个发送核采用连续区间、余数优先分配。发送核组内序号 i 的分配为：

```text
base = floor(BS / S)
remainder = BS % S
count_i = base + (i < remainder ? 1 : 0)
start_i = i * base + min(i, remainder)
sourceTokenRange_i = [start_i, start_i + count_i)
```

同一 Token 的全部 K 条 route、sourceMask、权重取用及两个发送阶段始终归同一 owner。
BS=256 时，前 16 个组内核各负责 6 个 Token，其余 32 核各负责 5 个。

执行顺序为：完成本卡 cumsum，确认结果对监听核组可见，再按实际 Combine 输入 Token 数 `tokenNum` 划分任务。
不再在 cumsum 完成前扫描预留 Token 区。各 Rank 不必同时进入监听阶段，
源端数据发送按 Token 分核、先第一跳后 DIRECT，流水见[本卡数据发送流水](#本卡数据发送流水)。

对于固定 C=16 个监听核，用连续逻辑区间表达均分：

```text
core i:
    begin = floor(i * tokenNum / C)
    end = floor((i + 1) * tokenNum / C)
```

每条 expert 接收行只有一个 owner；不能整除时相差至多一行，`tokenNum<C` 时允许部分核无监听任务。
按行数均分不保证转发工作量均分。同一 source/expert 分区可以跨核切分，q 范围不可重叠。

每核保存稳定位置表 tokenPos 与可变待处理表 pendingIdx。
例如 `tokenNum=2048、C=16`，每核分配 128 行：

| 数据 | 大小与作用 |
| --- | --- |
| tokenPos | 128 个 uint32 位置记录；由稳定下标 j 访问，记录相对本轮 Dispatch 接收区首 Token 槽位的偏移 |
| pendingIdx | 128 个 uint32，初始为 0..127，保存尚未完成所有任务下发的稳定下标 |
| pendingCount | 初始 128，仅此前缀有效；变为 0 表示本核全部任务已下发 |

两张表均仅存 UB，共 1024B，不输出 GM，不采用 GM 存表、溢出转存或分批加载。
各核按 cumsum 区间枚举 source/expert/q 通信槽位，生成 tokenPos；其编码及地址为：

```text
tokenPos[j] = dataRegionIndex * BS + q
slotAddress(j) = dispatchDataBase[thisRank] + uint64(tokenPos[j]) * T
```

基准是本卡本轮数据槽位区的第 0 个 Token 位置，不是最先到达的 Token；表项不是字节偏移、绝对 GM 地址或输出行号。
先扩为 uint64 再乘完整去重 stride T。pendingIdx 删除不修改 tokenPos，稳定位置表保留到本核 Flag 清理完成。
一般配置中每核 m 至多为 `ceil(tokenNum/16)`，每张表有效字节为 `4*m`，UB 各自按 32B 对齐申请。
均衡时每核约 BS*K/16 项；当前预计每核约 1K 项以内，1024 项时单表 4KiB、双表 8KiB。
这个估算不增加固定 1024 项上限；不能将本例 128 或旧 8*BS 容量当作监听表长上限。

监听检测使用 MTE2/Vector/Scalar，数据转发及本地 copy 写出使用 MTE3，包含向本卡接收区的转发。
已确认目标是监听不依赖无关的 MTE3 完成，数据准备和同一 UB 的复用仍须有必要依赖。
这些属于数据转发流水，后续在 R05-05/R05-06 重新设计具体 UB/Event、进度和资源回收方式；
不沿用 Memory LocalWindowCopy 调度，也不直接套用源端数据发送的 1 至 8 路数量，不以未查到的非阻塞查询作为必选接口。

一条 Token 的本地 copy、所有转发及相关持久写入均下发后，按如下方式剔除，不等待发送完成：

```text
j = pendingIdx[p]
if allRequiredWritesIssued(j):
    pendingCount -= 1
    pendingIdx[p] = pendingIdx[pendingCount]
    # Recheck p while pendingCount > 0.
else:
    p += 1
```

循环只访问有效范围，换入项继续检查，轮询到末尾后从头继续。
tokenPos、输出行及 forwardTokenIdx 关联稳定 j，不绑定 pendingIdx 的当前位置。
核末以 `PipeBarrier<PIPE_ALL>` 等待本核已下发的写入和清理完成；该操作不是跨核或跨 Rank 屏障。
各核（包括无 Token 任务核）完成本核收尾后独立返回；不再参与核末 SyncAll，也不清零 counter。

## 中转编号与持久表

### Token 级 prefix 编号

第一跳 owner 直接使用 `t=row=tasks.Begin()+j`，即本卡零基输出行号；编号在本卡第一跳
之间唯一，所有副本继承 `(relayRank,t)`。具体范围、稀疏 group 容量和 Combine 遗留见本文开头。
不再执行 AtomicAdd，也不读取或清零 counter。原 1KiB 区域保留为未使用布局空间。
全部核在各自写出/清理完成后独立返回；下一轮依赖同一 stream 的完整 kernel 完成边界。不新增同步或完成协议。
编号有效不代表数据已就绪：仍须等待完整包到达，并按原有 MTE/调用边界发布 metadata 和转发数据。
原子 API 核对及旧决策记录保留在历史 R05 中，不再描述当前生产路径。

### 元数据与所有权

Dispatch/Combine 只保留三类跨阶段 metadata：`assistEx`、`sourceMask` 和 `group`。
旧讨论中的 `forwardPlan` 不是额外 tensor；它的必要内容拆入 `assistEx` 和 `group`，其余 route/entry 只在发送过程中作为临时信息使用。

| 逻辑数据 | 生成者与消费者 | 必须保留的含义 |
| --- | --- | --- |
| 1. `assistEx` | 当前 expert 行的 Dispatch 接收 owner 生成，Combine expert 发送方消费 | 原始 srcRank/srcTokenId、匹配的 originalK、DIRECT 或 relay/forwardTokenIdx 引用，以及该 route 的 weight |
| 2. `sourceMask` | 原始 source 的数据发送核在 UB 生成，核末一次写出到 sourceMask 输出 GM；source Combine 消费 | 每个原始 Token 独立的 32bit source slot 集合，一个 Token 仅由一个发送核写入 |
| 3. relay group / 中转监听记录 | 第一跳 owner 生成固定容量、行内密排记录，relay Combine 消费 | 每组保存 firstHopRow 和最多 K-1 条 `(recvSlot,weight)`，实际前 n 条有效；通过本卡槽位监听返回，不重复保存 senderRank/originalK |

第一跳 owner 使用 Token 级 prefix 输出 row 设置 `forwardTokenIdx`，随转发副本携带并写入既有字段，关联同一 group 的第一跳、成员和返回槽位。
这是内部取号，不是第 4 项输出，不发布 X 或新增计数 metadata。

`assistEx` 按本卡实际接收的 expert 行保存，远端 expert 的行和 weight 位于其所在 Rank 的 `assistEx`。
它保存逐行身份、角色和权重，不能在 relay 本卡恢复完整的转发返回集合，也不能让 relay 自动取得其他卡的 weight。
中转监听信息通过下文已确认的 firstHopRow 与密排成员表保留；具体函数参数声明归 R10，GM 写入完成沿用 Dispatch 算子完成边界。
已确认 relay 负责本 Server 内该 group 的加权求和，因此 Dispatch 还须在 relay 本卡持久保留所需的成员权重。
第一跳权重已在本卡 assistEx；其他成员权重可由第一跳 owner 从原始 forwardEntries 提取保存，不能依赖临时 Token 尾部存活到 Combine。

第一跳本地 expert 行同样属于 group，需保存其输出行关联。
所有后续 expert 行均按转发 Token 保存原始 k 和相同的 relay/forwardTokenIdx 引用，包含 relay 本卡额外 expert。
Combine 据此走统一返回流程；assistEx 与 relay group 的布局、行关联和生产规则见下文，成员槽位统一为 `recvSlot=uint64(t)*K+originalK`。
ExpertOut 必须保持 Dispatch 的有效行序；中间 expert 计算若重排，由调用方在交给 Combine 前还原，不额外提供行映射。

临时 pendingIdx、发送 UB、Token 尾部不是可跨 Dispatch/Combine 生命周期的 plan。
沿用 Memory 的算子完成语义，所有核完成任务及 GM 写入后 Dispatch 才结束，原始输出和全部持久元数据随算子完成可供后续 Combine 使用。
不单独发布元数据、不增加逐表/逐 group ready 或额外同步，也不在结束时通过扫描 payload 重建 group。
Plan 及其引用的路由数据必须存活到对应 Combine 使用结束；不向终端卡传输整份 relay plan。

### AssistEx 布局与写入前转换

已确认 assistEx 与 Token 尾部固定头部采用相同八字段、相同 offset：

| 字段 | 类型 / 大小 | 行内 offset | 持久行中的含义 |
| --- | --- | --- | --- |
| `srcRank` | int32 / 4B | 0 | 原始输入所属 Rank |
| `srcTokenId` | int32 / 4B | 4 | 原始输入 Token 行号 |
| `k` | int32 / 4B | 8 | 当前 expert 行的 originalK；第一跳行中也表示 returnK |
| `weight` | FP32 / 4B | 12 | 当前 expert route 的权重 |
| `forwardCount` | int32 / 4B | 16 | DIRECT 为 0；第一跳保留原始正数 n；终端为 -1，不记录发送进度 |
| `forwardEntryCount` | int32 / 4B | 20 | 原始列表有效长度：DIRECT 为 0，第一跳/终端为 n |
| `relayRank` | int32 / 4B | 24 | DIRECT 为 -1；第一跳/终端为所属 relay 的 Rank |
| `forwardTokenIdx` | int32 / 4B | 28 | DIRECT 为 -1；第一跳/终端为同一 relay 分配的 t |

存储视图为 `uint32[tokenNum,8]`，基址与每行按 32B 对齐，row stride 为 32B，总容量为 `tokenNum*32B`。
有效行数沿用 `tokenNum=sendCounts[L*R-1]`，每行与同下标 `expertOut` 对应。
整数 word 按 int32 语义解释，weight word 按 FP32 原始位模式解释，不把浮点权重数值转换为整数。
此前 `[tokenNum,6]` 只表达必要信息集合，已由此八字段物理行布局取代；每行不附带 forwardEntries。

当前 expert 行的 Dispatch 接收 owner 负责完整生成该行，写入前按以下步骤转换：

1. 完成 Payload 的 Flag 剥离，并确认头部及需解析的 entries 在 UB 可读；从 `tailOffset` 取固定 32B 头部，
   复制到独立的、32B 对齐的元数据 `metaUB`。它不与后续仍要修改或转发的 Token UB 共用可写存储。
2. 在 metaUB 内按下表完成字段转换；所有字段完整后才提交持久写出，不先写一个未分配编号的第一跳行再回填 GM。
3. 统一将 metaUB 的完整 32B 写到 `assistExBase + uint64(dispatchRow)*32`。
   `dispatchRow=dstPosition=prefixBefore(localExpert*R+srcRank)+q`，沿用 Memory 的输出行计算；首个分区的 prefixBefore 为 0，其余取前一项前缀。
   它是该 owner 当前处理的 Dispatch expert 输出行，不是 srcTokenId 或 q。仅第一跳的 row 等于 forwardTokenIdx；终端继承第一跳编号。
   字段转换对 MTE3 可见后才下发写出，metaUB 保持只读到对应写出完成；按正常 UB/Event 依赖复用，不增加逐 Token PIPE_ALL。

| 接收角色 | metaUB 写出前的转换 | 与公共发送副本的关系 |
| --- | --- | --- |
| DIRECT | 八字段原样保留；计数为 0，返回引用为 `(-1,-1)` | 无须重排或查找 entry |
| 第一跳 | 保留头部 k/weight、正数 forwardCount=n 和 forwardEntryCount=n；首次转发前取号一次，补入本 Rank/t（offset 24/28） | 同一个 t 也填入公共转发 UB；仅转发 UB 的 forwardCount 改为 -1，metaUB 保留正数 n |
| 转发终端 | 按当前 expert 匹配有效 entry，将 originalK/weight 覆盖到 offset 8/12；其他六字段原样保留 | 不修改 relay 的公共发送副本；本卡转发与跨卡转发采用相同转换 |

第一跳的元数据准备和完整 Token 转发不要求先等待元数据写出完成；两者使用独立 UB，并各自满足数据就绪和复用依赖。
发送资源暂满时保留已分配 t，重试不重复取号。Dispatch 对外完成须包含这些持久写出，不能仅以已经下发表示完成。
专家计算后的行引用已由 R04-02 确认；具体函数参数声明和核内 Event 配置仍归 R10/R05，本节未指定或验证目标拷贝 API。

### SourceMask 生成与写出

已确认 sourceMask 输出 GM 按原始 srcTokenId 存放，shape 为 `[BS]`，每项一个 32bit 位集合，stride 为 4B，总容量为 `BS*4B`。
每个原始 Token 的全部 K 条 route 由同一数据发送核处理，因此它的 mask 也只有该核生成、持有和写出，
无需其他核回填、原子 OR 或跨核归约。

每核在 UB 中为自己负责的每个 Token 保留一个独立 uint32 值，初始为 0：

- 识别并发送远端 group 的第一跳时，只置该 group 的 returnK 位，后续 entries 对应位保持 0。
- 处理 DIRECT 时，置当前原始 k 位，包含源 Server 内 route 和远端单 route。
- 两阶段更新同一份 UB 值，切换阶段时不重新初始化；每次置位使用 `uint32(1)<<k`，其中 `0<=k<K<=32`。

各核完成本核两阶段发送任务的处理后，将自己记录的全部 sourceMask 一次批量写出到 sourceMask 输出 GM 的对应 Token 位置。
每个元素仅写入一次，不逐 route 写 GM，也不等待其他发送核到齐后统一写出。
这不增加两阶段之间的 PIPE_ALL/SyncAll，也不要求先等待此前全部 payload MTE3 完成才下发 mask 写出。
源端按已确认的连续分配得到 start_i/count_i；UB 有效空间为 `count_i*4B`，起点按 32B 对齐。
核末使用 DataCopyPad 将有效 `count_i*4B` 写至 `sourceMask[start_i]`，最小为一个 uint32（4B）；
GM 目标起点及有效长度无需 32B 对齐，不使用 DataCopy 向上取整写出，也不引入邻核合并或 32B read-modify-write。
例如 count_i=5 时仅写 20B，count_i=1 时仅写 4B，UB 填充不覆盖邻核元素。
无源端 Token 任务的核不写 mask；有任务的核保持 mask UB 到写出完成，Dispatch 对外完成覆盖其 GM 写入。
该 mask 仅说明 source 最终读取哪些 k 槽位，不表示 payload 已到达，也不单独编码 DIRECT/已加权 partial 类型。

### ExpertScales 输入输出分离与 source 权重生成

沿用 Memory 的 FP32 `[BS,K]` 形状和 `srcTokenId*K+originalK` 索引，Dispatch 权重输入和输出使用两个独立 buffer。
原始 Token 的唯一发送核从只读 expertScales 取真实 route weight，在 UB 生成归约权重并写到 expertScalesOut：

| 槽位 | expertScales 输入与随包权重 | expertScalesOut 输出 | sourceMask |
| --- | --- | --- | --- |
| DIRECT 的原始 k | 真实 route weight | 保持真实值 | 1 |
| group 的 returnK（第一跳 k） | 第一跳真实 route weight | 1.0 | 1 |
| group 其余 k | 对应 entry 的真实 route weight | 不消费，无需赋值或清零 | 0 |

源端发送核先读取真实权重，为第一跳 header、有效 forwardEntries 和 DIRECT header 填入各自真实值。
第一遍和第二遍均从原始输入取真实值；生成输出中的 1.0 不会覆盖输入，也无需等待全部真实权重读取结束才能构造输出。
按已确认的核末写出安排，输出权重暂存在本核 UB；发送 UB 和权重输出 UB 各自保持到对应 MTE3 完成。
expertScalesOut 写出只覆盖本核拥有的元素，
按 R05-04 每核写出 count_i*K*4B，使用 S_MTE3 发布和核末 MTE3_S 完成等待，不增加两阶段之间的 PIPE_ALL/SyncAll 或全部 MTE3 等待。

Dispatch 算子完成包含 expertScalesOut 写出。调用方将该输出传入 Combine 的 expertScales，保持其存储和值到消费完成。
输入 expertScales 按 Dispatch 输入生命周期持有；不会被修改，可在允许的后续调用中复用真实权重，无需因本轮转换而重填输入。
包、assistEx 与 group 中始终保留真实权重，不把 source 的 1.0 回填到 relay 元数据。
首版不支持 `expertScales=nullptr`；如果未来支持顺序重复 Combine，应消费同一份保持不变的已转换权重，具体调用范围归 R11。

例如 R0/T0 的 k0 在源 Server 内 DIRECT，k1/k2 在 R8/R9 形成一个远端 group，k3 在 R17 DIRECT，
输入 expertScales 始终为 `[0.4,0.3,0.2,0.1]`。Dispatch 后 sourceMask=`0b1011`，expertScalesOut 为 `[0.4,1.0,x,0.1]`，
其中 x 不消费；R8 的第一跳和成员仍保存 0.3/0.2。R8 返回 partial 后，R0 对三个有效位置统一乘权重累加。
每个 source Token 最终需要接收的贡献数为 `popcount(sourceMask[token])`，有效位置由位图给出；
不增加 recvCount 或 DIRECT/partial 类型标记，不以 weight 是否为 1.0 判断类型。

### Relay Group 成员表

已确认每个 group 固定预留 K-1 个 member，实际前 n 项密排，`n=assistEx[firstHopRow[t]].forwardEntryCount`。
第一跳通过 firstHopRow 关联本卡 expert 输出和 assistEx，不占 member 槽位；本卡额外转发与跨卡转发均各占一项。
Group 按 `forwardTokenIdx=t` 索引，预留 C=R*BS*min(K,L) 行。Combine 只消费由有效第一跳 `assistEx` 关联到的 group 行，
在 `[0,tokenNum)` 中识别 `assistEx[row].forwardCount>0`，取该行的 t 访问 `group[t]`，且 `group[t].firstHopRow=row`。
每个实际 group 恰对应一条第一跳行；DIRECT 和终端行不产生 group。不读取未写入行，不需要 X、groupCount、validMask、哨兵或整表初始化。
各行使用固定 stride，不跨 group 压缩空间，也不增加原子计数或前缀和。

| 字段 | 类型 / 大小 | 行内 offset | 含义 |
| --- | --- | --- | --- |
| `firstHopRow` | int32 / 4B | 0 | 第一跳的 Dispatch 输出行 `dstPosition`，从同下标 assistEx 取得 srcRank/srcTokenId、returnK、第一跳 weight 及 n |
| `members[i].recvSlot` | uint32 / 4B | `4+8*i` | 本 relay 中转结果区的相对槽位编号 `t*K+entry.originalK`；本卡与跨卡返回相同，不含发送卡维度 |
| `members[i].weight` | FP32 / 4B | `8+8*i` | 对应未加权 expert 输出的 route 权重，保留原始浮点位模式 |

其中 `0<=i<n<=K-1`。逻辑成员 shape 为 `[8*BS,K-1,2]` 个 32 位 word；行引用和成员合并存储，每行按 32B 对齐：

```text
logicalWordsPerRow = 1 + 2 * (K-1)
rowStrideBytes = align32(4 * logicalWordsPerRow)
physicalShape = uint32[8*BS, rowStrideBytes/4]
groupRowAddress = relayGroupBase + uint64(t) * rowStrideBytes
```

BS=256、K=6 时每行有效 44B、对齐后 64B，预留 2048 行，物理 shape 为 `uint32[2048,16]`，共 128KiB。
K=8/16/32 时每行对齐后分别为 64/128/256B。该表是元数据，不含 H 维度，也不额外乘发送卡数。
`members[n:K-1]` 和行尾 padding 不参与解析，不按原始 k 留洞，不要求逐项填写 `{-1,-1}` 哨兵。

第一跳 owner 设置 `group[t].firstHopRow=dstPosition`，使用已分配的 t 遍历原始 `forwardEntries[0:n]`：
每条 entry 用 `recvSlot=uint64(t)*K+entry.originalK` 计算相对槽位，将 `(recvSlot,weight)` 写到同下标 members[i]。
Entries 已按原始 k 升序排列，因此不需要新的密排编号或排序；不为返回寻址计算 senderRank，也不在成员表中重复保存 originalK。
`destinationExpertId` 仍用于 Dispatch 转发寻址及终端匹配，移除来源卡维度不改变该字段的用途。
在独立元数据 UB 完成该行后写入对应持久 GM，元数据 UB 保持到写出完成；原 Token UB 仍按其 copy/转发依赖回收。

Combine 从 firstHopRow 读取第一跳贡献并加权，再沿前 n 项的顺序监听、读取和加权归约转发结果。
每个有效项通过 `relayResultBase + uint64(recvSlot)*memberSlotStride` 定位本轮数据与对应 Flag。
Expert 返回端从 assistEx 读取 relayRank、t 和本行 originalK，使用同一公式定位目标 relay 的槽位；本卡返回同样写入并被监听。
该表不保存跨调用 GM 绝对地址，group 行 stride 和行引用已确认，GM 写入随 Dispatch 算子完成。
具体函数参数声明归 R10，返回 slot stride 的字节格式及区域 base/offset 归 R07-01/R08-03。

## Combine 数据流

Combine 保留每条 expert 输出的独立贡献，先在目标 Server 的 relay 对同一原始 Token 的 group 执行加权求和，
再将 server partial 返回原始 source。Expert 输出通常数值不同，不按 payload 相同来合并。

| 路径 | 目标与消费方式 |
| --- | --- |
| DIRECT | 未加权 expert 输出返回原始 source 的 `[srcTokenId][k]` 结果位置；本地 DIRECT 可直接读取，均由 source 乘对应 weight 后累加 |
| 转发 expert -> relay | 本卡及跨卡转发所得未加权 expert 输出均按 forwardTokenIdx、原始 k 返回统一中转区的对应槽位，由 relay 乘该 route 的 weight 后累加 |
| 第一跳 expert | relay 从关联的本地输出行取得未加权贡献，乘第一跳 weight 后纳入同一 group；每组仅此一条，其他本卡 expert 均走转发返回 |
| Relay partial -> source | 将 group 已加权求和的结果返回原始 source 的 `[srcTokenId][returnK]` 位置 |
| Source 最终归约 | 只消费 sourceMask 指定的位置，统一乘本卡 expertScales 后累加；partial 槽权重为 1.0，DIRECT 槽为真实权重，被 partial 代表的其他 k 不再等待 |

本卡及同 Server 其他卡的转发结果共用一块中转结果区，返回地址统一为：

```text
recvSlot = uint64(forwardTokenIdx) * K + originalK
combineAddress = mappedPeerWindowBase[relayRank]
               + relayResultRegionOffset + recvSlot * memberSlotStride
```

发送方通过自己的 peer 映射定位 relay；本卡返回取本卡对应基址，使用相同公式。
现有 assistEx 中的 relayRank、forwardTokenIdx 和 k 已足够计算地址，不额外传输 recvSlot，不查询远端 group 表，也不再次密排取号。
不同 group 的 t 全卡唯一；同一 group 内各 expert route 的原始 k 唯一，因此 `(t,k)` 唯一确定贡献，不需要发送卡维度。
每组固定预留 K 个槽位，即使 n=1 也保留 K 个；第一跳仍通过 firstHopRow 读取，不写入对应返回槽。
只监听 members[0:n] 列出的位置，第一跳及不属于该 group 的 k 槽位均不使用、不等待；K=1 时不产生 relay group。
Relay 只等待持久表中实际存在的转发贡献，包含本卡返回，不等待全体预留 slot。
本卡转发与跨卡转发使用同样的归约成员记录和完成条件，不因发送卡等于 relay 而绕过返回流程。

沿用已有顺序约定：group 先处理第一跳，再按密排 member 顺序归约，等价于按原始 k 升序消费全部成员；source 按 sourceMask 的 slot 升序归约。
`returnK` 是该 group 的最小原始 k。Source 从 K 个有效位开始，将一个 group 的成员位替换为 returnK 位。
这不会丢弃 expert 贡献，而是将其合入一个 partial；本阶段存在的每个输入 Token 都有有效 route，
不沿用 inactive token 或空 route 的零 mask 规则。

K=32 时 mask 必须按 32 位位集合解释，bit31 不是负长度或无效标志；
构造完整 K 位集合不能使用未定义的 32 位左移 32 操作。

每条 route 的 weight 必须在合并前应用且只应用一次。已确认随 Dispatch 包提供对应信息：
头部保存第一跳 weight，entries 保存各自 weight，终端按 expert 匹配提取。
已确认每条 expert route 的 weight 保存到所在 Rank 的 `assistEx`。
已确认 relay 对本 Server 内该 group 的全部成员执行乘 weight 和求和，包含第一跳、本卡转发和跨卡转发贡献；
转发终端返回未加权 expert 输出。relay 完成加权归约后返回一份 partial，source 使用 Dispatch 输出的 expertScalesOut 权重 1.0；
DIRECT 使用真实权重，两类有效结果统一乘权重累加，不再次应用第一跳真实 weight。源 Server 内逐 route DIRECT 的范围保持 R01 约定。
对同一个原始 Token，逻辑计算为：

```text
partial[g] = sum(k in group[g], weight[k] * expertOut[k])
output = sum(g, partial[g]) + sum(k in DIRECT, weight[k] * expertOut[k])
       = sum(k where sourceMask[k]=1, result[k] * expertScales[token,k])
```

该式描述贡献关系，执行仍遵循前述 k/slot 顺序。
第一跳权重从其本卡 assistEx 取得；其他转发成员的 weight 在 Dispatch 到达 relay 时已随 forwardEntries 提供，
须持久保留为 relay Combine 的本地输入，不直接读取其他卡的 assistEx，也不在 Combine 另发一轮 weight。
已确认每条 member 保存 `(recvSlot,weight)`，Dispatch 按 `t*K+originalK` 完成位置换算后密排保存，GM 写入随算子完成；返回数据格式及完整区域预算继续在 R08-03/R07-01 收口。
头部、assistEx、group 的 weight 均按 FP32 保存，真实权重与 source 归约权重的职责及持久来源已由 R04-07/R04-08 确认。
Source 仅需 sourceMask 和转换后的 expertScales，无需区分结果类型；权重存储保持到 Combine 消费完成。
首版要求 `expertScales` 非空且为 FP32 `[BS,K]`；无 weight 指针直接报错。各计算/通信阶段 dtype 尚待 R08/Combine 对应议题确认。
两级归约改变浮点求和括号，不承诺与普通 Memory 位级一致。

Combine 的 expert 发送、group 监听/归约、partial 发送及 source 归约如何分核、按 H 分块和并行推进，
已迁至 [Combine 内部待确认项 C15-C24](ep-memory-server-dedup-combine-open-topics.md#本轮迁入边界)，
不能直接将 Dispatch 的已确认核组设计当作 Combine 调度方案。

## 容量与生命周期

### 三块通信区域

Memory 与 Server Dedup 算法不混用。执行 Server Dedup 时，通信空间全部供本版本使用，按本版本需要重新划分；
不保留旧 Memory 算法的区域或绝对 offset，也不把兼容旧布局作为前置条件。已确认的排布顺序为：

| 顺序 | 通信区域 | 内容 |
| --- | --- | --- |
| 1 | 状态区 | cnt 状态、cumsum 工作区及相关就绪 Flag，另含一份 1KB counter 专用区；counter 仅使用首 4B |
| 2 | 数据接收区 | 内部先预留 Combine source 返回槽位，再排布 Dispatch 原始 source/expert/q 槽位；沿用 Memory 的两个独立子区，Dispatch 使用完整去重 T |
| 3 | 中转 Token 区 | Combine 接收转发成员结果的统一区域，预留 8*BS*K 个槽位，本卡及跨卡返回共用 |

第二块之后追加第三块；不为 Dispatch 的 Token 尾部另划通信区域，尾部已经包含在第二块各槽位的 Payload 中。
R07-01 的具体 offset、对齐、所需字节数及子区位置按下表收口；不将第二块的两个子区重叠复用。
通信域通过现有可指定大小的 MemoryDomain 接口申请为 8 GiB；Host 校验两份窗口及单窗容量，不修改框架的 runtime Flag 所有权。
这属于 Server Dedup 自身的布局实现，不涉及两个算法共存；cnt/cumsum 的记录格式和流程继续复用 Memory。
assistEx/sourceMask/group 和 expertScalesOut 是调用方持有的普通 GM 输出；输入 expertScales 只读，均不计为通信大区。

### R07-01 单份通信数据域布局

以下是单个窗口内的布局，B_rank = peerMems[rank] + uint64(magic % 2) * 4 GiB；所有 offset、乘积及地址运算使用 uint64。两个窗口使用相同布局，控制信息及 counter 也必须翻转。
沿用 Memory 的记录内容和访问顺序，重算偏移；不调用带旧固定窗口大小的地址常量来计算本版区域。
记 N=R*L、C=16、A32(x)=align32(x)、A512(x)=align512(x)。T 是 Dispatch 去重包跨度。
当前 FP16/BF16 非量化返回沿用 Memory Combine DataAsFlag：`U=ceil(H*2/480)*512`，不附 Dispatch 转发尾部。
为描述后续公共 dtype 扩展，source 返回跨度记 U_s、member 返回跨度记 U_m；当前二者均取 U。
若 R08 后续增加其他通信 dtype/scale，只按其有效载荷重算 U_s/U_m，不重开槽位数量或重新引入来源卡分区。

| 子区 | 相对 B_rank 的 offset | 预留字节数 / 记录形状 |
| --- | --- | --- |
| 已有 profiling 记录 | profileOffset=0 | profileBytes=A512(200*64)=12800；保留当前 profile 的最大记录容量及 64B stride，不改变功能核数 64 |
| cnt 接收记录 | cntOffset=profileOffset+profileBytes | N*32；`[L,R,32B]`，沿用 Memory 的 ready/count 记录 |
| cumsum 核间交换 | sumOffset=A32(cntOffset+N*32) | C*C*32；`[producer,consumer,32B]`，沿用 GetCumSum 的按列读取与清理 |
| 完整 cumsum 副本 | prefixOffset=A32(sumOffset+C*C*32) | C*prefixStride，prefixStride=A32(N*4)；每个接收核一份 N 个 int32 |
| cumsum 就绪 Flag | readyOffset=A32(prefixOffset+C*prefixStride) | C*C*32；`[consumer,producer,32B]`，每个 producer 发布一列，每个 consumer 检测并清理自己一行 |
| 唯一 counter | counterOffset=align1024(readyOffset+C*C*32) | 1024B，仅首 uint32 使用；其余 1020B 不访问、不清零 |
| 状态区结束 | stateBytes=A512(counterOffset+1024) | 对齐空洞不承载记录；profiling 禁用也不改变布局 |
| source 返回子区 | sourceOffset=2 MiB | sourceBytes=BS*K*U_s；`[BS,K,U_s]`，DIRECT 和 partial 共用按原始 k 编码的槽位 |
| Dispatch 接收子区 | dispatchOffset=A512(sourceOffset+sourceBytes) | dispatchBytes=N*BS*T；`[R,L,BS,T]`，保留 Memory 的 source 轮转位置 |
| relay 中转结果区 | relayOffset=A512(dispatchOffset+dispatchBytes) | relayBytes=8*BS*K*U_m；`[8*BS,K,U_m]` |
| 单份数据域需求 | requiredBytes=relayOffset+relayBytes | 本版三块区域的完整所需字节数，不含调用方的普通 GM 输入输出 |

profiling 是现有独立去重空入口已有的记录，不是新增控制协议；其最大 200 行是诊断容量，cnt/cumsum 和数据处理仍固定 48/16 核。
代码接入时让 cnt 基址指向 cntOffset，避免沿用空入口 profileOffset=0 时覆盖状态记录。source 返回预留与 Dispatch 子区先后关系
对应 Memory 的 [SetDataStatus/winDataSizeOffset_](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L556)；完整前缀副本对应 [CalRecvAndSetFlag](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L1336) / SetValidExpertInfo，只将消费者改为 16 核。
当前非量化返回跨度和 Token/k 地址参考 [Memory Combine 返回路径](../../src/ep/kernels/tilexr_ep_combine_memory_kernel.h#L550)，profile 容量取自[已有 profile 定义](../../src/ep/common/ep_memory_server_dedup_profile.h#L9)。

设目标 Rank 为 d，原始 source 为 s，本地 expert 为 e，接收组内索引为 i，producer 为 j：

```text
cntAddr(d,s,e)       = B_d + cntOffset + (uint64(e)*R+s)*32
sumAddr(j,i)         = B_local + sumOffset + (uint64(j)*C+i)*32
prefixAddr(i,index)  = B_local + prefixOffset + uint64(i)*prefixStride + index*4
readyAddr(i,j)       = B_local + readyOffset + (uint64(i)*C+j)*32
dispatchAddr(d,s,e,q)= B_d + dispatchOffset + (((uint64(s)+d)%R*L+e)*BS+q)*T
sourceAddr(s,tid,k)  = B_s + sourceOffset + (uint64(tid)*K+k)*U_s
relayAddr(r,t,k)     = B_r + relayOffset + (uint64(t)*K+k)*U_m
counterAddr          = B_local + counterOffset
```

forwardEntries 的 q 和原始 source 决定 Dispatch 最终槽位；经由 relay 的发送仍用原始 source s，不能替换成 relayRank。
group 保存的 recvSlot=t*K+k 直接用于第三块寻址，第一跳自身通过 firstHopRow 读取 expertOut，不占用一个必须回传的 member 槽位。

| 内容 | 正常单次调用的存活和清理 |
| --- | --- |
| cnt / sum / ready | 沿用 Memory 的读取后清理；只有接收组内 0..15 消费完整前缀，不发布给源端发送核 |
| prefix 副本 | 当前 Dispatch 接收核读取；依赖本轮 ready，下一 Dispatch 可重写，不要求清零前缀内容 |
| counter 首 4B | 本轮 Dispatch 转发取号，算子末 SyncAll 后核 63 清零；Combine 不读取 counter |
| Dispatch 接收槽位 | 保持到接收/本地拷贝/转发最后使用完成，Flag 清理按 R06；与 source 返回子区不重叠 |
| source / relay 结果槽位 | Combine 当前调用的通信数据，按 sourceMask / group 只消费有效位置；与 Dispatch 槽位及彼此均不重叠 |
| assistEx / sourceMask / group / expertScalesOut | 普通 GM 输出，持有到 Combine 消费完成；不保存 Dispatch 通信槽位指针 |

每个 4 GiB 窗口各保存一份上述布局和 counter，状态不得超出前 2 MiB，requiredBytes 不得超过 4 GiB-4 MiB。
框架决定通信数据域的实际分配/映射，R11 继续限定多调用、多 stream 及跨 Rank 的窗口复用；这些不改变本表，也不阻塞本轮 sendData 和 cnt 路径实现。
本轮收口的是字节布局和正常使用边界，Combine 内部调度、数值计算和 Flag 清理细节仍在对应 Combine 议题讨论。

R=128、L=3、BS=256、H=7168、K=6、FP16/BF16 非量化时：

| 子区 | offset（B） | 大小（B） |
| --- | ---: | ---: |
| profiling | 0 | 12800 |
| cnt | 12800 | 12288 |
| cumsum 交换 | 25088 | 8192 |
| 16 份完整前缀 | 33280 | 24576 |
| cumsum 就绪 Flag | 57856 | 8192 |
| counter | 66560 | 1024 |
| source 返回 | 2097152 | 23592960（22.5MiB） |
| Dispatch 接收 | 25690112 | 1560281088（1488MiB） |
| relay 返回 | 1585971200 | 188743680（180MiB） |

状态区为 67584B（66KiB），其中 ready 结束至 counter 起点有 512B 对齐空洞；状态区仍置于前 2 MiB 内；每窗实际布局总量为 1774714880B（1692.5MiB）。
上述预算不包含 metadata/权重/hidden 的普通 GM 输出，也不表示通信分配或 NPU 行为已经验证。

### 中转结果区与本卡返回（历史预算，Combine 实现时替换）

已确认使用一块统一中转结果区，预留 `8*BS` 个 group，每组 K 个槽位；本卡及其他卡的返回共享全部容量。
早期七分区方案及追加本卡成为八分区的候选已撤回，来源卡不再作为结果区的 shape 维度。
全卡统一取号已经区分 group，原始 k 已经区分组内贡献；继续按来源卡分区会重复区分同一贡献并限制容量共享。
初版用例保证本卡实际 group 数不超过 8*BS，Dispatch/Combine 不做超限校验、超限状态记录、报错或降级处理。

该安全输入约束针对本 relay 合计不同监听 Token 数，不是每张发送卡独立的数量。
实际 group 数达到 8*BS 且 Y>8*BS 仍在安全范围内；本卡与跨卡转发的 Token 合计为实际 group 数，仅有本卡转发的 group 也计入。
`8*BS` 是选定的容量上限，不是八卡拓扑天然保证的最大接收量；初版安全用例约束相应采用该上限。
每组实际返回 n 份结果，`1<=n<=K-1`，即使只返回一份也不压缩 K 个预留槽位；group 表只记录实际前 n 个 member。

```text
relayLogicalShape = [8*BS, K, H]
relayResultSlots = 8 * BS * K
relayResultBytes = 8 * BS * K * memberSlotStride
```

当前非量化返回沿用 Memory 结果通信格式：

```text
payloadBytes = H * dtypeBytes
blockCount = ceil(payloadBytes / 480)
memberSlotStride = blockCount * 512
relayPhysicalShape = uint8[8*BS, K, blockCount, 512]
```

每块含 480B 数据、32B Flag，最后一块可有填充。
BS=256、H=7168、K=6 且通信 dtype 为 FP16/BF16 时：

```text
payloadBytes = 14336
blockCount = 30
memberSlotStride = 15360 bytes
relayGroupCapacity = 2048
relayResultSlots = 12288
relayResultBytes = 188743680 bytes = 180 MiB
groupMetadataBytes = 2048 * 64 = 131072 bytes = 128 KiB
```

以上为 R07-01 的一份中转结果区；只有后续明确支持 P 份并发独立数据域时才乘 P，当前布局不额外申请第二份。
不额外乘发送卡数、总 Rank 数、expert 数或核数。group 表的 `8*BS` 行容量随之扩大，每行布局不变；
assistEx 仍按 tokenNum 行、sourceMask 仍按 BS 个原始输入 Token 保存。
上述 180MiB 仅为给定 dtype/stride 的中转结果区估算，不含 source 最终结果区、Dispatch 接收区或其他 metadata/scratch。
后续 dtype/scale 扩展时按 R08 更新 stride；R07-01 已给出完整单份数据域预算，该示例不表示已完成硬件分配或验证。

### 其他区域与超限

Dispatch 数据区仍按 `R*L*BS` 个槽位、各 T 字节预留；中转 group 的安全上限不能缩小其原始 source/expert 分区。
这一乘积不含 cnt、状态、同步区或额外 workspace。
R=128、L=3、BS=256、H=7168、K=6、FP16/BF16 非量化时 T=15872B，Dispatch 数据槽位单独占用 1488MiB；
R07-01 据此计算本方案区域 offset 和所需字节数；4GB 通信域扩容及分配/映射由框架负责，不在本方案处理容量上限。
当总专家数取新上限 1024，同一 BS/H/K/dtype 下 Dispatch 数据槽位为 `1024*256*15872=4160749568B=3968MiB`；
完整总量继续按 R07-01 加入状态、source 和 relay 返回区，不以专家数上限替代字节计算；通信容量上限及扩容由框架负责，不列为本方案待确认问题。
实际 Combine 输入 Token 数上界为 `tokenNum<=R*BS*min(K,L)`，assistEx 分配容量须覆盖该上界，有效行数及行序沿用已确认的 Memory 契约。

Counter 已固定保留一份 1KB、仅使用首 4B，具体 offset 见 R07-01；独立 expertScalesOut 在普通 GM 按 BS*K*4B 预算，不另划通信区域。
源端结果区、assistEx、group、源端数据发送 UB 与数据转发使用的 Flag scratch/UB 分别预算。
当前非量化公共 stride、区域偏移和正常存活关系已在 R07-01 收口，后续数值扩展及并发调用分别归 R08/Combine、R11；
按上述三块顺序核算 Server Dedup 自身的布局与生命周期；通信分配/映射及 4GB 上限由框架负责。

X 不是 Combine 输入，也不需要正常发布；初版仅执行保证实际 group 数不超过 8*BS 的安全用例，
不为超限增加分支、状态或校验。
后续可讨论“尽力转发”：中转容量满额后，将后续 Token 记录为直接返回源端、不再中转。
这只是备忘方向，何时决策、影响哪一段转发及如何同步 sourceMask/返回元数据，见 [R07](ep-memory-server-dedup-open-topics.md#r07)；不构成初版降级路径。

### 清理与跨轮复用

已确认的顺序语义：`PipeBarrier<PIPE_MTE3>` 约束本核前序 MTE3 完成后执行后续 MTE3；
它不替代 Scalar/Vector 对发送 UB 可复用的判定，也不是全局同步。

Flag 清理有两种尚未选择的时机（R06）：

| 方案 | 顺序 |
| --- | --- |
| 核末统一清理 | 本核所有 Token 任务下发后，按稳定 tokenPos 表安排清理，退出前等待完成 |
| Token/批次发送后清理 | 所属发送全部下发，插入 PIPE_MTE3，再通过 MTE3 清理已消费接收记录 |

两者都只清理本核已消费的接收 Flag；向本卡转发时不能清掉目标尚待消费的 Flag，
也不能因 source/expert 分区跨核共享而整段清零。
清理及发送最后通过本核 PIPE_ALL 等待完成，但窗口复用还取决于所有相关生产者和消费者的完成边界。
Dispatch 算子末尾全部 64 核 SyncAll 后由核 63 清零唯一 counter 的首 4B，写零在退出前完成；
counter 随当前窗口翻转；group 等普通 GM 输出由调用方保证生命周期，不能因通信区翻转而提前覆盖。

区分保存的 Dispatch plan 身份与每次 Combine 通信轮次。
沿用运行时 `TileXRCommNextMagic` 的新轮次机制，局部 Flag 清理不替代代际管理。
顺序重复 Combine、多 stream、多个未完成 plan、失败重试与 ping-pong 复用的支持范围在 R11 明确。

## 完整示例

BS=256、H=7168、K=6、R=128、总 expert 数 384，每卡 L=3。
R0 的 T8 选择 `[25,26,27,28,29,30]`，按阶段一首目标规则选 R8 为 relay。

| k | Expert | 目标 | Dispatch | Combine |
| --- | --- | --- | --- | --- |
| 0 | 25 | R8/local1 | 第一跳 R0 -> R8 | 第一跳本地 expert 行 |
| 1 | 26 | R8/local2 | R8 -> R8，独立接收槽位 | 按转发 Token 返回 R8 的统一中转区 `[t,1]` |
| 2 | 27 | R9/local0 | R8 -> R9 | 返回 R8 的统一中转区 `[t,2]` |
| 3 | 28 | R9/local1 | R8 -> R9 | 返回 R8 的统一中转区 `[t,3]` |
| 4 | 29 | R9/local2 | R8 -> R9 | 返回 R8 的统一中转区 `[t,4]` |
| 5 | 30 | R10/local0 | R8 -> R10 | 返回 R8 的统一中转区 `[t,5]` |

R0 直接为六条 source/expert route 发送 cnt。T8 前已有 T0..T7，
所以 q25..q30 分别由这些更早 Token 的路由决定，不能假定为 0、8 或相互相等。

第一跳含 hidden 和下列表格中的尾部。固定头部采用前述[已确认字段表](#尾部字段字节表)，entry 按已确认的 16B 布局计算，
所有 offset 相对逻辑尾部首字节；R8 一次取号得 t=7（示例值，与 T8 的 8 无必然关系）：

| 头部字段 | offset / 大小 | R0 发给 R8 的第一跳 | R8 发给各 expert 的终端公共副本 |
| --- | --- | --- | --- |
| `srcRank` | 0 / 4B | 0 | 0 |
| `srcTokenId` | 4 / 4B | 8 | 8 |
| `k` | 8 / 4B | 0 | 0；仍表示 expert25 的 k |
| `weight` | 12 / 4B | w0 | w0；仍表示 expert25 的权重 |
| `forwardCount` | 16 / 4B | 5 | -1 |
| `forwardEntryCount` | 20 / 4B | 5 | 5 |
| `relayRank` | 24 / 4B | -1（未使用） | 8 |
| `forwardTokenIdx` | 28 / 4B | -1（尚未分配） | 7 |

第一跳与五份终端副本携带的 entries 完全相同，每条 16B：

| entry 下标 i | 起始 offset | destinationExpertId | destinationQ | originalK | weight | 对应发送 |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | 32 | 26 | q26 | 1 | w1 | R8 -> R8/local2 |
| 1 | 48 | 27 | q27 | 2 | w2 | R8 -> R9/local0 |
| 2 | 64 | 28 | q28 | 3 | w3 | R8 -> R9/local1 |
| 3 | 80 | 29 | q29 | 4 | w4 | R8 -> R9/local2 |
| 4 | 96 | 30 | q30 | 5 | w5 | R8 -> R10/local0 |

例如 R9/expert28 消费 entry2，其字段在尾部 offset 64、68、72、76 处，
依次为 expert28、q28、k3、w3；公共头部的 k0/w0 不作为该行的 route 信息。
公共发送副本仅统一设置 `forwardCount=-1, relayRank=8, forwardTokenIdx=7`，
保留头部 k0/w0、有效长度 5 和原始五条 entries。
五次发送共用这份 UB，写入本例的：

```text
receiveAddress(expert) = dispatchDataBase[expert / 3]
                      + (uint64(expert) * 256 + qExpert) * T
```

这是 srcRank=0、L=3 时一般接收公式的特例。
R8/expert26 从 entry0 提取 k1/w1，R9 三条接收行提取 k2/w2、k3/w3、k4/w4，
R10 提取 k5/w5；第一跳 expert25 由头部取得 k0/w0。
各行保存自己的原始 k 和同一个 `(relay=8,idx=7)` 返回引用。

本卡和其他卡返回统一按 `recvSlot=7*6+k` 寻址，五条转发结果的位置为：

| 结果 | 逻辑位置 | 相对槽位下标 |
| --- | --- | --- |
| expert26（本卡） | `[7][1]` | 43 |
| expert27 | `[7][2]` | 44 |
| expert28 | `[7][3]` | 45 |
| expert29 | `[7][4]` | 46 |
| expert30 | `[7][5]` | 47 |

R8/expert26 同样执行返回写入和监听，不因本卡贡献绕过统一流程。
group[7] 的 members[0:5] 为 `(43,w1),(44,w2),(45,w3),(46,w4),(47,w5)`；槽位 42 对应第一跳 k0，不使用、不等待。
R8/expert25 保留第一跳本地贡献。T8 形成一个 group、产生五条转发结果，其中一次本卡返回、四次其他卡返回；
group 成员为 k0..k5，returnK=0，sourceMask 从 `0b111111` 变为 `0b000001`。
R8 读取 expert25 的未加权本地输出，并接收 expert26..30 的未加权返回；由 R8 计算 `partial=sum(k=0..5,wk*expertOut[k])`。
Dispatch 已将 R0/T8 的 expertScalesOut[k0] 设为 1.0，其余 k 不消费；输入 expertScales 保持真实 w0..w5，R8 的 assistEx/group 同样保存真实权重。
该 partial 返回 R0/T8/k0；R0 统一乘 expertScales[k0]=1.0 后累加，不再乘 w0，也不等待本组 k1..k5 的独立结果。

按已确认的 32B 头部，加上五条 16B entry，
有效逻辑尾部为 112B，按已确认规则向上对齐到 32B 后占用 128B，其中最后 16B 是对齐填充。
非量化 FP16/BF16 数据段占 14336B，追加后 UB Token 数据总长为 14464B；该长度尚未计入通信分块和 Flag，不作为最终 T。
示例在非量化 hidden 相同的前提下省去跨 Server 的五份 hidden 第一跳，
仍保留五笔 Server 内逐 expert 写入；它不证明端到端性能收益。

## 验证边界

本文完成文档归并、逻辑示例及容量核对，没有实现或测试去重 kernel。
确认协议必须区分三种证据：路由/地址的参考计算、目标 CANN API 的编译核对、实际设备的可见性/前进性验证。
计数完成不代表数据到达，原子取号不代表 GM 写入完成，任务下发不代表 UB 或通信窗口可以复用；全部输出写入由 Dispatch 算子完成语义保证。

后续 Dispatch 验证要求见 R12，Combine 内部验证见其待确认文档：检查有效数据/字段、完整 Flag、
sourceMask、只读 expertScales 的真实权重与独立 expertScalesOut 的归约权重、有效行及返回地址，不比较未用 entries 或纯传输填充字节。
测试端按固定种子和 Rank 生成随机正权重，逐 Token 归一化为 FP32 `[BS,K]`，Top-K 行和为 1（FP32 检查容差 1e-6），K=1 时为 1.0。
该归一化只构造测试输入，不在算子内增加权重求和校验或重新归一化；source 归约权重输出不要求行和为 1。
覆盖 BS=47/48 的 cnt 分工、空职责核参与核末同步、源端 1 至 8 路 UB 的环形复用和跨逻辑阶段在途任务、
tokenPos 相对槽位、零接收、同卡多 expert、原子唯一取号与连续轮次清零、数据转发恢复及安全范围内的 group 数量。
不增加已由安全输入契约排除的重复/invalid top-k 或中转容量超限测试；初版不验证超限处理或超限校验。

性能报告必须包含各阶段 backend、native status、kernel version、算法或通信数据量、可读 latency、
适用的 bandwidth 和跨 Rank median/min/max，并计入完整尾部、Flag、Server 内转发、原子及归约成本。
复用匹配环境已有的 HCCL Test 资格记录；未来接入 MoonEP 模型/replay 时保留框架 profiler、
Dispatch/Combine stage barrier 和允许的 grouped mode，默认 group_credit。

参考源码：[Dispatch](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h)、
[Combine](../../src/ep/kernels/tilexr_ep_combine_memory_kernel.h)、
[Host Layout](../../src/ep/host/ep_memory_layout.cpp)、
[窗口](../../src/ep/common/ep_memory_window.h)、
[Data-as-flag](../../src/include/tilexr_data_as_flag.h)、
[Kernel Launch](../../src/ep/host/ep_kernel_launch.cpp)。
第二轮讨论从[待确认项索引](ep-memory-server-dedup-open-topics.md#第二轮索引)继续。
