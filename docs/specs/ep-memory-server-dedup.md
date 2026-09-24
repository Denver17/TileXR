# EP Memory Server Dedup Dispatch：代码与数据流

本文帮助读者理解当前 `TileXRMoeEpDispatchMemoryServerDedup` 实现，实现位于
`codex/SilentEmotion` 分支。只描述已实现行为；Server Dedup Combine 的
消费契约见[联合设计](ep-memory-server-dedup-dispatch-combine-design.md)。本文的分类、转发及五表细节针对开启模式；关闭模式以[关闭去重特性](ep-memory-server-dedup-disabled-mode.md)为准。

## 从一个 Token 理解去重

输入 `x[BS,H]` 中的每个 Token 有 K 条 expert 路由和 K 个 FP32 权重。
Dispatch 将 hidden 送到各 expert 所在卡，形成按本卡 expert/source 分段的输出行，
同时生成计数、前缀和供 Combine 使用的五张表。

去重发生在跨 Server 的数据发送上，最终每条 expert 路由仍产生一行输出：

- 目标位于源 Server，或某远端 Server 只命中一条 route：直接发送，接收类型为 DIRECT（1）。
- 同一 Token 在某远端 Server 命中多条 route：源端只向其中最小原始 k 对应的卡发送一次，
  该卡称为 relay，第一跳接收类型为 2。
- relay 保存自己的 expert 输入，再在该 Server 内转发其余成员，成员接收类型为 3；
  成员也可以是 relay 本卡的另一个 expert。

例如，一个 Token 有三条 route：一条在源 Server，两条在同一远端 Server。
源端发送一个 DIRECT 包和一个第一跳包，relay 再转发一个成员包；最终仍有三条 expert 行。
Count 始终统计三条原始 route，跨 Server 的 hidden 传输则为一次。

阅读顺序建议：先看接口与输出，再沿主 kernel 的 Count → sender/receiver 调用链阅读；
需要核对地址和同步时，再查包格式、UB lane 与通信域布局。

| 源码 | 阅读重点 |
| --- | --- |
| [公共入口](../../src/include/tilexr_ep.h)、[Host wrapper](../../src/ep/host/tilexr_ep_memory_server_dedup_dispatch.cpp)、[Host 检查](../../src/ep/host/ep_memory_server_dedup_host.cpp) | 输入输出、支持范围与提交前检查 |
| [launch](../../src/ep/host/ep_memory_server_dedup_launch.cpp)、[kernel 入口](../../src/ep/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.cpp) | 32B GM tiling、120B 参数包、类型分支 |
| [主 kernel](../../src/ep/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.h) | Count、收发分工和每核完成边界 |
| [counts](../../src/ep/kernels/tilexr_ep_server_dedup_counts.h) | 原始路由计数、发送、前缀及控制状态清理 |
| [sender](../../src/ep/kernels/tilexr_ep_server_dedup_send.h) | 第一跳、DIRECT、量化打包和源端两表 |
| [receiver](../../src/ep/kernels/tilexr_ep_server_dedup_recv.h)、[任务表](../../src/ep/kernels/tilexr_ep_server_dedup_receive_tasks.h)、[包处理](../../src/ep/kernels/tilexr_ep_server_dedup_receive_packet.h) | Probe、输出行、转发和接收侧三表 |
| [Count 布局](../../src/ep/common/ep_memory_server_dedup_count_layout.h)、[数据布局](../../src/ep/common/ep_memory_server_dedup_data_layout.h)、[Host 容量](../../src/ep/host/ep_memory_server_dedup_layout.cpp) | 窗口、地址、容量和 UB 预算 |

## 支持范围

记 R 为 Rank 数，E 为总专家数，L=E/R，BS 为本卡输入 Token 数，H 为 hidden 长度，
K 为 top-k，S 为发送核数，Crecv=64−S。alignN(x) 表示向上对齐到 N 的整数倍。
以下容量上界 Crows=R*BS*min(K,L)，不能与接收核数 Crecv 混用。

| 项目 | 当前契约 |
| --- | --- |
| 平台 | CANN 9.1 / A5；Host 公共接口保持 C++14，AICore 目标使用 gnu++17、dav-c310-vec / CATLASS_ARCH=3510 |
| Rank / Server | 每 Server 固定 8 Rank，连续编号，serverId=rank/8；R 为 16..128 内的 8 的倍数 |
| 专家 | 0<E≤1024，E 能被 R 整除；expert/L 定位 Rank，expert%L 定位本卡专家 |
| 输入 | BS>0、H>0，所有 Rank 的 BS 相同，globalBs=R*BS；1≤K≤min(32,E) |
| 路由 | 每 Token 恰有 K 个合法且互不重复的 expert；输入保证该契约，kernel 不做完整路由合法性扫描 |
| 功能 | 无共享专家、token mask；tpWorldSize 只接受 0/1，tpRankId=0、expertShardType=0 |
| 占位参数 | scales、tpRecvCountsOut、expandScalesOut 为 nullptr |
| 通信 | Memory peer window 支持同节点及跨节点；本入口无 UDMA 自动切换或普通 Memory 回退 |
| 容量 | Host 检查 Count UB、控制区、单窗容量及整数乘积；调用方保证输出分配、收发 UB 能容纳实际任务及至少一个完整 lane |

分核和Count阈值在 `ep_memory_server_dedup_count_layout.h` 配置；去重值由 `build_all.sh dedup=0|1` 设置（公共定义在 `ep_memory_server_dedup_mode.h`）。修改后须配套重建 Host、kernel 和消费者，
所有 Rank 使用一致产物；不是 run.sh 参数或运行时环境变量：

| 宏 | 当前默认值 | 行为 |
| --- | --- | --- |
| TILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM | 32 | 支持 16/24/32/40/48；发送核 [0,S)，接收核 [S,64) |
| TILEXR_EP_SERVER_DEDUP_ALL_CORE_COUNT_BS_THRESHOLD | 512 | BS<阈值时 64 核发送 Count，否则仅 Crecv 个接收核发送 Count |
| TILEXR_EP_SERVER_DEDUP_ENABLE | 1 | 1 为远端 Server 分组去重；0 为全部原始 route DIRECT |

Count 阈值与数据核比例独立；无源 Token 的核仍可能负责 Count。控制表起点固定，表内有效范围
及矩阵行距依赖 Crecv；同一 communicator 生命周期内保持编译配置一致。编译开启 auto-sync，
`-cce-aicore-record-overflow=false`。

## Dispatch/Combine 公共接口与 metadata shape

主要输入为 `x[BS,H]`、`expertIds[BS,K]`、`expertScales[BS,K]` 和 communicator。
expertScales 为必需的 FP32 路由权重。公开调用返回成功表示异步提交成功；
输出须在对应 stream 的 kernel 完成之后消费。

`TileXRMoeEpDispatchMemoryServerDedup` 的五个 metadata 指针依次为 tokenTypeOut、
destinationIndexOut、relayReadIndexOut、sourceMaskOut、expertScalesOut。
五表基地址至少 4B 对齐，精确行距如下；`Crows=R*BS*min(K,L)`。

| 输出 | 分配容量 / 有效范围 | 使用契约 |
| --- | --- | --- |
| expandXOut | Crows*H*(quantized?1:2)B；前 tokenNum 行 | expert 计算后恢复原 Dispatch 行序 |
| dynamicScalesOut | 量化时 Crows*scaleBytes；前 tokenNum 行 | hidden 解量化 scale |
| sendCountsOut | int32[E] inclusive prefix | 最后一项为 tokenNum |
| expertTokenNumsOut | int64[L] | 累计或逐 expert 计数 |
| tokenTypeOut | uint32[Crows]；前 tokenNum 行 | 1=DIRECT、2=第一跳、3=转发终端 |
| destinationIndexOut | uint32[Crows][3]，12B/行 | 类型 1/2：[source,token,k]；类型 3：[slot,UINT32_MAX,UINT32_MAX] |
| relayReadIndexOut | Crows*K*8B，仅类型 2 行 | 第 0 块 N/selfWeight，第 1..N 块 expert/q/weight |
| sourceMaskOut | uint32[BS] | 最终等待的原始 k 位集合 |
| expertScalesOut | FP32[BS,K] | mask 有效处 DIRECT 真实权重，group returnK 为 1.0 |

真实 expertScales 输入只读，Host 拒绝与权重输出重叠；其他容量、归属及输出之间不重叠由
持有分配记录的调用方校验。裸指针入口无法推断实际分配大小。
类型 2 行自身对应第一跳 expert，其 selfWeight 位于 relayReadIndexOut 第 0 块。
类型 3 的 slot 为本卡接收逻辑槽号，计算见“Dispatch 接收布局”；输出保存 uint32 槽号，
不包含字节 stride。五表交给 Combine 时的参数映射由联合设计维护。

源端第一跳下发后置 returnK 位并写权重 1.0，DIRECT 置原始 k 位并写真实权重。
核末按本核连续输入 Token 区间写出；K=32 使用 uint32 位集合。关闭编译期去重时全为 DIRECT。
调用方保持五表到对应 Combine 完成；容量尾部及索引无效行/块不消费。

## 算子函数与模块划分

```text
Host：检查 → 注册 A5 二进制 → 取得 magic → 缓存/取得 GM tiling → 提交 64 核
每核 Init：读取配置，初始化 profile / counts / sender 或 receiver
每核 Process：
  SendCnt（未分配 Count 的核只准备后续 scratch）
  ├─ core < S：sender.Process(sharedScratch, reservedCountBytes)
  │              第一跳循环 → DIRECT 循环 → lane drain → sourceMask/weight 写出
  └─ core ≥ S：RecvCnt → CalcCumsum → CompleteCumsum
                 → PIPE_ALL → receiver.Process（Reset、prefix/tasks、Probe/消费、清 Flag）
  counts.Finish：本核 PIPE_ALL，恢复 CTRL[60]
  kernel_total end → profile.Publish → 返回
```

收发组并行推进，各核按自己的依赖前进；核末完成本核操作后独立返回。
`CompleteCumsum` 与 receiver 之间的 PIPE_ALL 确保计数写出完成，再允许 Reset 和 UB 重用。
Kernel 完成要求所有核完成，Host 异步 launch 返回不代表结果已可读。

## Host 提交与 GM tiling

Host 在基本参数、communicator、容量和 A5/核数检查通过后，注册独立 AICore 二进制，
取得本轮 magic 和 GM tiling 地址，调用 `rtKernelLaunchWithFlagV2` 提交 64 核。
二进制加载与提交封装见 [ep_kernel_launch.cpp](../../src/ep/host/ep_kernel_launch.cpp)。

内部 launch 参数按以下顺序组织，每个地址和 magic 均为 8B，总大小 120B：

```text
commArgs, x, expertIds, expertScales,
expandXOut, dynamicScalesOut, expertTokenNumsOut, sendCountsOut,
tokenTypeOut, destinationIndexOut, relayReadIndexOut, sourceMaskOut,
expertScalesOut, tilingData, magic
```

[EpServerDedupTilingData](../../src/ep/common/ep_memory_server_dedup_tiling.h) 固定 32B，
Host 校验数值范围后填入，reserved 字节为零：

| 字节 offset | 字段 | 类型 |
| --- | --- | --- |
| 0、4、8 | bs、h、profilingLaunchSlot | uint32_t |
| 12 | moeExpertNum | uint16_t |
| 14 | topK | uint8_t |
| 15 | reservedSendCoreNum | uint8_t |
| 16～19 | expertTokenNumsType、quantMode、dtype、expandXOutDtype | uint8_t |
| 20 | reservedEnableServerDedup | uint8_t |
| 21～31 | reserved | 11B |

核数和去重开关来自编译期宏；profilingLaunchSlot 的无效值为 UINT32_MAX。
非量化 RESERVED 输出 dtype 由 Host 解析为输入 dtype。globalBs 在 Host 推导/校验，
magic 单独传入；kernel 从 tilingData GM 地址读取配置后，传给 counts/sender/receiver 的 Init。

communicator 的 [KernelTilingCache](../../src/comm/tilexr_kernel_tiling_cache.h) 以完整 32B
配置为 key。首次使用时分配 GM 并同步 H2D 初始化，成功后发布地址；相同配置直接复用。
各配置是不可变快照，持续有效到 communicator 销毁，防止后续提交改写尚在使用的参数。
分配或复制失败不提交 kernel；销毁前调用方须等待该 communicator 的全部工作完成。

Runtime 的 argsSize=120、hasTiling=0；参数包只包含 tiling 地址，32B tiling 内容独立存放在 GM。
安装产物须配套包含通信库、EP Host 库、kernel 和 consumer。

## 路由与计数

原始 source 向所有目标 expert 发送原始 route 的 cnt，包含之后由 relay 代理发送的数据。
Relay 不转发 cnt，不改变包的 source 身份。计数次序为 localExpert/source：

```text
cntIndex = localExpert * R + srcRank
q = 原始 expertIds[0 : srcTokenId*K+originalK] 中等于目标 expert 的数量
row = prefixBefore(cntIndex) + q
prefixBefore(0)=0，其余取前一项 inclusive sendCountsOut
```

q 不等于 srcTokenId、已发包数、DIRECT task 下标或 pending 下标。发送重排不改变 q。
`sendCountsOut` 是 int32[E] 的含当前项前缀；`tokenNum=sendCountsOut[E-1]`。
`expertTokenNumsOut` 为 int64[L]：type=0 输出各 expert 末尾累计值，type=1 输出逐 expert 数量。
由接收组索引 0（物理核 S）写出。零计数区间仍通知、求和并清理，tokenNum=0 不允许跳过协议。

### Count 异步发送与 scratch 交接

`SendCnt` 按目标 expert 连续区间分核，先筛选范围，再逐 expert 归约计数。
待发送记录与筛选 scratch 分离：

```text
countCores = BS < threshold ? 64 : Crecv
alignedRecords = align8(ceil(E/countCores))
保留发送记录 = alignedRecords * 32B（未参与或空 Count 核为 0）
Count 峰值 UB 上界 = 4*align256(BS*K*4) + alignedRecords*(32+4)
总 UB 预算 = 190 KiB
```

记录区只读保持到对应 MTE3 完成；独立 16 项 stagePool 与 5 项 countPool 视图使后续 sender
或 recv-count 重用筛选 scratch。`SendCnt` 下发后不执行 PIPE_ALL、TPipe::Reset 或 pool Reset；
这些操作会破坏预期重叠。sender 的 lane 预算扣除保留记录区。

接收端先等待自己负责的 incoming cnt，Gather/Sum 并生成本分段相对前缀，随后发布局部和、
清理本分段 cnt 状态；`CalcCumsum` 等待跨核局部和，取得全局偏移并修正本段前缀，再向各接收核
发布完整 prefix 的分段数据及 ready。sumCoreBuf 复用为 ready 源前，等待局部和的 MTE3 读操作完成。
`CompleteCumsum` 消费本核完整 prefix，清理 ready，并写 expert 计数。

计数归约将整数位型作为 FP32 subnormal 参与运算，`counts.Init` 保存 CTRL[60] 并置 0；
发送侧继承该配置执行 q 计算和可选量化。`counts.Finish` 完成本核流水后恢复入口值。

## 源端两阶段发送

每个 Token 的 K 条 route 归同一发送核：

```text
base=BS/S，remainder=BS%S
count_i=base+(i<remainder)
start_i=i*base+min(i,remainder)
```

默认 S=32、BS=256 时每核 8 Token。每核独立先发全部第一跳，再发 DIRECT，两段之间不等待
其他核或第一跳 MTE3 完成；目标 relay 的展开不阻止 source 继续 DIRECT。

开启去重时，仅对目标位于远端 Server 且同 Token 在该 Server 命中多条 route 的集合分组。
最小原始 k 是第一跳/returnK，目标 Rank 为 relay，其余 k 按升序放入 entries。
源 Server 内的每条 route、远端 Server 的单条 route 均为 DIRECT；同一个 Token 可有多个 group。
即使某 group 全部 expert 位于 relay 本卡，后续成员仍生成类型 3 的接收行。

### 分类表与 DIRECT 调度

第一遍逐 Token 调用 `Classify`，使用 Scalar 的
seen/grouped/directMask、64B serverState 和 firstHopMasks：第一次遇到远端 Server 时暂列 DIRECT，
第二次遇到时生成 group 并移出 DIRECT，随后扩充该 group 位图。
每个 Token 的第一跳立即发送；DIRECT 记录 `(token,k,expert,q)` 写入 16B 的 directTasks。
第二遍复用 `ServerDedupHalfOrder`，先本半区、再另一半区轮转扫描该任务表，同 Server 内保持 Token/k 顺序。
令 N=serverNum、H=floor(N/2)，两半区为 [0,H) 和 [H,N)，r 为 myServer 在自身半区的相对位置。
依次对本半区和另一半区，目标为 `base+(r+j)%length`，j=1...length，使用各自半区长度取模。
例如 N=8、myServer=2：3→0→1→2→7→4→5→6；N=7、myServer=6：3→4→5→6→1→2→0。
本 Server 在本半区最后，奇数 N 的两个半区长度不同；空半区不参与取模。

仅 DIRECT 使用该顺序，第一跳仍逐 Token 分类后立即发送，本核所有第一跳先于 DIRECT。
每核连续 Token 区间、缓存的原始 q、目标地址、sourceMask/weight 和量化打包规则不变。
累计 packets/lane 跨第一跳、Server 和半区连续，边界不增加事件、barrier 或 MTE3 drain。
关闭去重时所有 route 走同一 DIRECT 半区顺序。

| 源端 UB | 大小/职责 |
| --- | --- |
| serverState | 64B，开启模式当前 Token 的首 k 或 group 下标 |
| firstHopMasks | align32(4*min(serverNum−1,floor(K/2)))，当前 Token 的 group 位图 |
| directTasks | align32(count_i*K*16)，最大 DIRECT 任务数，保存原始 q |
| sourceMask | align32(count_i*4)，group 保留 returnK，DIRECT 保留原始 k |
| weightIn / weightOut | 各 align32(count_i*K*4)，只拷入本核权重区间 |

关闭模式使用独立的按Server路由筛选调度，不运行本节的Token分类、不生成DIRECT缓存表或sourceMask；每条路由直发，表5原样复制。包尾、UB及M6接收细节见[关闭去重特性](ep-memory-server-dedup-disabled-mode.md)。

## 本卡数据发送流水

`SendPacket` 每个物理包执行一次 MTE2 输入读取及可选量化，不缓存一个 Token 的量化结果供所有
目标复用。输入为两个 queue buffer；量化结果区与最终 MTE3 outBuf 分开：

```text
MTE2 输入 → 可选 Vector MXFP8 → Scalar FillTail → Vector DataAsFlag 打包 → MTE3
```

非量化输入 buffer 为 packedPayloadBytes；量化输入 buffer 为 2*align256(H) 字节，
共享 cleanPool 只用于初始化量化计算补齐。routeIds、q/quant scratch、quant payload、权重、mask、
DIRECT 表和 Count 保留区均先计入预算，再分配发送环：

```text
Nsend = min(8, floor((190KiB - usedBytes)/T))
outBufBytes = Nsend*T
lane = packetsIssued % Nsend
```

输入保证 Nsend≥1。lane 是缓存槽数，不是引擎数。第一跳与 DIRECT 共用累计包序号及同一环。

| 动作 | 依赖与所有权 |
| --- | --- |
| 输入/量化到 Scalar | 非量化 MTE2_S，量化 V_S，随后 FillTail |
| Scalar 到打包 | S_V；尾部位于 input/quant payload 中，不直接改写在途 outBuf |
| 复用输出 lane | Wait MTE3_V(lane)，保证此前 MTE3 读完 |
| Vector 打包到发送 | V_MTE3(lane)，保留每块原有 Flag，只拷 Payload |
| MTE3 写完整 T 字节 | 下发 MTE3_V(lane)，下一次轮回再消费事件 |
| 本核收尾 | 消费全部 lane 事件，S_MTE3 后写 mask/weight，MTE3_S 等待这批输出完成 |

## Dispatch dtype 与 MXFP8

| 输入 | quantMode | expandXOutDtype | 每行公开输出 |
| --- | --- | --- | --- |
| BF16 / FP16 | 0 | 同输入，RESERVED 在 Host 解析为输入 dtype | H*2B；dynamicScalesOut=nullptr |
| BF16 / FP16 | 4 | 显式 FP8E4M3 / FP8E5M2 | H 字节 hidden，另有 scaleBytes 字节 dynamicScalesOut |

`TileXRMxfp8Quant` 依次执行 ComputeMaxExp、ComputeScale 和 ComputeFp8Data。
量化只处理 hidden；真实 route weight 始终 FP32。relay 保持收到的 FP8/scale 位型，不重新量化。

### 尾部容量与通信长度

布局函数先计算 payload 中的逻辑位置，再转换为插入 Flag 后的 wire 位置：

```text
quantCount = align256(H)
scaleBytes = quantized ? align2(ceil(H/32)) : 0
headerPayloadOffset = align32(quantized ? quantCount + scaleBytes : H*2)
readIndexPayloadOffset = headerPayloadOffset + 32
if readIndexPayloadOffset % 480 + K*8 > 480:
    readIndexPayloadOffset = align480(readIndexPayloadOffset)
headerWireOffset = (headerPayloadOffset/480)*512 + headerPayloadOffset%480
relayReadIndexWireOffset = (readIndexPayloadOffset/480)*512 + readIndexPayloadOffset%480
payloadBytes = align32(readIndexPayloadOffset + K*8)
blockCount = ceil(payloadBytes/480)
packedPayloadBytes = blockCount*480
T = tokenStride = blockCount*512
```

MXFP8 scale 位于逻辑 payload 的 quantCount 起点；每 32 个 hidden 元素一个 1B scale，按两项对齐。
输出只写 H 个 FP8 元素和 scaleBytes 字节，通信补齐不变成 public 行间距。
H=7168、K=6：非量化 payloadBytes=14464、T=15872；MXFP8 payloadBytes=7488、T=8192，
输出每行含 7168B hidden 和 224B scale。

### Payload 与 DataAsFlag

每个 512B wire 块包含 480B Payload 和 32B Flag（8 个 FP32 1）。所有块的 Flag 都参与 ready；
包括最后一块。Flag 表示整包各块到达，未使用的成员块和 padding 不解析。
逻辑字节位置 `x` 到 wire 位置的换算为 `x + floor(x/480)*32`。
接收侧保留带 Flag 的整包，不先完整 unpack 到第二份 payload buffer；Init 预计算 headerWireOffset 和 relayReadIndexWireOffset，
索引段物理连续；hidden/scale 输出时 MTE3 按区间跳过 Flag。

## Token 格式与转发

### 随 Token 存放

尾部属于 Dispatch Payload，与 hidden/scale 一起按完整 T 发送，不另发控制包。

### 32B 包头与连续索引段

包头和索引段起点各自 32B 对齐。包头固定 32B，四个有效 uint32 字段为：

| 偏移 | 字段 | 语义 |
| --- | --- | --- |
| 0 | srcRank | 原始 source，转发时保持不变 |
| 4 | srcTokenId | 原始 source 输入行 |
| 8 | topK | DIRECT 的原始 k 或第一跳 returnK，不是总 K |
| 12 | type | 1=DIRECT、2=第一跳、3=转发终端，与持久 TokenType 枚举一致 |
| 16..31 | padding | 无业务语义，不要求清零 |

RelayReadIndex 段固定 K*8B；若当前 block 的 480B payload 放不下整段，则移至下一 block
起点。第 0 块为 uint32 N 与 FP32 selfWeight；第 1..N 块为 uint16 expertId、uint16 q、
FP32 weight，按原始 k 升序保存，低 16 位为 expert、高 16 位为 q。
N 不包含第一跳；DIRECT 的 N=0。无效块不消费、不要求清零。
源端在打包前直接形成该段；第一跳收包后原样写入 relayReadIndexOut，不做字段转换。
成员 expertId/q 各占 16 位；当前 E≤1024，Count UB 预算约束 BS*K<2^14，合法输入可无损编码。

第一跳先将本地类型 2 与原始返回身份保存到独立 staging，最终转发前仅将 wire 包头 type
改为 3；包括索引段、其他头字段、占位及 Flag 在内的其他字节均保持不变。
所有成员收到相同 wire，成员地址由 expert/q 计算。终端直接记录本卡接收 slot，
以 slot 写入 DestinationIndex；relay 的本地记录仍为类型 2。

## Dispatch 接收布局

```text
dataRegionIndex = ((srcRank + dstRank) % R)*L + localExpert
receiveAddress = peerMems[dstRank] + windowOffset + dispatchOffset
               + (uint64(dataRegionIndex)*BS + q)*T
```

DIRECT、第一跳和终端使用相同地址公式、跨度和原始 source 身份；每 route 一个独占槽位，
不使用 credit 循环复用。cumsum 规定最终输出 row，DataAsFlag 判定该槽是否能读，两者不能互相替代。

## 分核与监听

`receiver.Process` 在本核 Count 写出完成后 Reset TPipe，重新加载本接收核的完整 prefix。
Crecv 个核按实际 tokenNum 分配输出行：

```text
begin_i=floor(i*tokenNum/Crecv)
end_i=floor((i+1)*tokenNum/Crecv)
j=row-begin_i
```

`tokenPos[j]` 记录稳定物理槽号 dataRegionIndex*BS+q，`pendingIdx[p]` 保存尚未完成所有任务下发的 j。
构表先二分定位起始前缀段，再枚举本核行范围。两表各 align32(4*本核行数)，只在 UB；
零任务核完成 prefix/tasks/prepare 后返回，不建 Probe/lane，不清其他核槽位。

### Probe 与 pending

每次 Probe 最多 16 个候选：保存稳定 j 快照，MTE2 读取每包所有块的 Flag，
一次 MTE2_V 后用二维 `Sum<float>` 逐行归约，V_S 后 Scalar 比较各 sum 与 blockCount*8，形成 ready 位图。

ready=0 时 scan 向后推进，至 pending 尾部回绕；直到某批非零 ready 才进入搬运。
按快照下标从大到小加载/消费 ready 项，处理完当前整批后再 Probe。
每项的本地输出、全部 relay 与 lane 释放指令提交后，才用末项替换删除 pending；
倒序删除保证当前快照中较小下标仍有效。

pendingCount=0 只代表全部任务已经下发；物理 DMA 完成仍依赖 lane 事件和最后 PIPE_ALL。
稳定 tokenPos 保留到核末清理；row 与可变 pending 位置分别维护。

### 动态 N 槽的输入与输出流水

每个 lane 包含一个 wire 包及 type/destination 两个独立 32B staging：

```text
B = blockCount，M = 16，m = 本核实际任务数
prefixBytes = align32(E*4)
tablesBytes = 2*align32(m*4)
flagsBytes = M*B*32 + 256
workBytes = M*align32(ceil(B/8))*4 + 256
fixedBytes = prefixBytes + tablesBytes + flagsBytes + workBytes + M*36
laneStride = T + 64
Nrecv = min(8, floor((190KiB-fixedBytes)/laneStride))
```

M*36 包含每候选 32B sum 存储和 4B 快照下标；Sum 使用紧凑结果下标，其额外分配和保护区用于
匹配 CANN 9.1/A5 的实际访问范围。输入保证 Nrecv≥1；接收 lane 数按本核预算计算。

调度使用累计 issued/consumed：先将 ready 批最多 Nrecv 个包排入 MTE2，再每消费一个补充预取，
序号跨批连续。`IssueLoad` 等待 MTE3_MTE2(lane)，读取完整 wire，再设置 MTE2_MTE3 与 MTE2_S。
`ConsumeLoaded` 等待 MTE2_S 后做 Scalar metadata，S_MTE3 后等待 MTE2_MTE3，依次下发 hidden、
可选 scale、type、destination、类型 2 的索引段及全部 relay；最后设置 MTE3_MTE2。下一次 MTE2 覆盖同一 lane
前必须消费该事件。因此多 lane 允许 MTE2 与前序 MTE3 重叠，标点不证明硬件已实际重叠。

### 公共转发副本与输出

本地 type/destination 从改写前的包头构造，类型 2 的索引段直接引用 wire 的连续片段。
转发改写 type 后发布 S_MTE3，接收 MTE2→MTE3 依赖也保留；各表精确写 4B/12B/K*8B，
不把 UB staging 的 32B 对齐扩大成 GM 写入长度。GM 的索引行也不增加 32B 行间填充。
全部本 lane 的 metadata、hidden/scale 和 relay 写出完成后才允许复用该 lane。
核末排空所有 lane、清理本核 Flag，并等待本核流水完成。

## 输出行与持久表

输出行固定为 `row=tasks.Begin()+j`。pending 删除只改变扫描队列，不改变该任务的
输出行与物理槽号。三张接收侧表的写出规则为：

| 收到的 type | tokenTypeOut[row] | destinationIndexOut[row] | relayReadIndexOut[row] |
| --- | --- | --- | --- |
| 1 | 1 | [srcRank,srcTokenId,originalK] | 无效，不写出 |
| 2 | 2 | [srcRank,srcTokenId,returnK] | 原样写入源端的完整 K*8B 索引段 |
| 3 | 3 | [tasks.Position(j),UINT32_MAX,UINT32_MAX] | 无效，不写出 |

表 1/2 按 row 密排，表 3 仅类型 2 行有效，因此按最大输出行容量分配并允许空洞。
类型 3 的 slot 与源端索引中 expert/q 推导的成员槽一致。第一跳保存本地 type 后才修改
转发 wire，确保 type/destination 的异步写出不受包头变更影响。

## 容量与生命周期

### 共用通信 buffer 与固定控制布局

Dispatch/Combine 复用同一个 peer-memory allocation。两份完整 buffer 分别位于实际逻辑
通信空间的前后半区：`windowOffset=(uint64(magic)%2)*(CommArgs.peerMemBytes/2)`。
每份依次为 Dispatch 控制区 764KiB、Combine 控制区 52KiB、共用数据区；数据起点固定为
1MiB（1048576B），均按 512B 对齐；两个控制区末尾的 816KiB 到 1MiB 之间
208KiB 留空，不属于任何算子的控制表。标准 profiling、五表及输出仍使用独立 GM。

公共容量及 offset 宏定义于
[buffer 布局头](../../src/ep/common/ep_memory_server_dedup_buffer_layout.h)。
下表 offset 相对 Dispatch 控制区；主控制区 offset 相对本次所选 buffer。
CountLayout 内部 cumsumOffset/prefixOffset/cumsumFlagOffset 相对 cntOffset，构造时由宏相减取得。

| 表 | 固定 offset（B） | 固定预留（B） | 实际有效范围 |
| --- | --- | --- | --- |
| 前置保留 | 0 | 12800 | 不访问 |
| count | 12800 | 32768 | E*32 |
| cumsum | 45568 | 73728 | Crecv*Crecv*32 |
| prefix 总区域 | 119296 | 393216 | Crecv*E*4 |
| ready | 512512 | 73728 | Crecv*Crecv*32 |
| 对齐间隙 | 586240 | 512 | 不访问 |
| counter 保留 | 586752 | 1024 | 不访问 |
| 区尾扩展预留 | 587776 | 194560 | 不访问 |

prefix 区域按 `1024*4*48*2=384KiB` 预留，内部各核副本继续紧凑存放，
`prefixStride=align32(E*4)=E*4`。额外一倍仅为空间预留，不增加复制量。
写入副本、CompleteCumsum 和接收阶段均沿用实际 E 寻址；后续 ready 表不能占用 prefix 预留。
两个矩阵也保持实际 Crecv 行距，固定整表起点不代表改变表内布局。

```text
dispatchOffset = TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET  // 1MiB
dispatchBytes = uint64(E)*BS*T
requiredBytes = align512(dispatchOffset+dispatchBytes)
bufferStride = CommArgs.peerMemBytes/2
```

T 是含包尾和 DataAsFlag 的 Dispatch wire stride。Dispatch 不预留 Combine 返回/中转区；
Combine 数据视图与 Dispatch 共用同一起点，其中中转区紧接结果区之后；调用方按两算子
数据需求最大值预算，详见联合设计 §6。
双方独立检查各自视图，不用 Dispatch T 代替 Combine 的 Sresult。
Host 先检查 shape/UB/整数范围，再要求：

- `peerMemBytes%1024==0`，peer 基址 512B 对齐，地址加法无溢出；
- `bufferStride>=TILEXR_FLAG_BUFF_BYTES`；
- `requiredBytes<=bufferStride-TILEXR_FLAG_BUFF_BYTES`，等于时允许；
- `stateBytes=587776<=764KiB`，各控制表实际范围不超固定预留。

runtime 4MiB Flag 实际只占全域尾部，第一半区对应尾部留空，保证两份布局具有相同容量约束。
物理 allocation 为 rank 地址偏移额外申请的字节不计入逻辑 peerMemBytes。
chain 测试默认仍显式申请 8GiB；生产算子接受符合上述容量条件的其他大小。

维护规则：整表起点、表内 stride 和复制长度是三个独立概念。固定控制表位置时不得意外扩大
prefix 读写量；修改选窗步长时必须同步 Count、发送、接收、consumer 和替身地址解析。
只验证两份总需求加一次 runtime 保留区会漏掉第二半区越界，必须按每半区检查。

### 清理与跨轮复用

接收核在所有 Token 输出和转发指令提交后，消费全部 MTE3_MTE2 lane 事件；
准备全零 Flag 源，V_MTE3 后遍历稳定 tokenPos，仅清理自己拥有的每个包的 32B Flag 块，
最后 PIPE_ALL。清理不覆盖 payload，也不清其他接收核拥有的槽位。

Count 的 cnt/sum/ready 由各自 owner 消费后清理；prefix 读取受本轮 ready 约束，无需清零。
sourceMask 每个输入 Token 都写出，expertScalesOut 仅 mask 有效位有定义；
RelayReadIndex 只定义类型 2 行的前 N+1 块，其余内容不要求清零。
每核 `counts.Finish` 执行本核 PIPE_ALL 并恢复 CTRL[60]，随后记录总耗时终点和 Publish。

同一 communicator 的调用在所有 Rank 保持顺序一致，在同一 stream 串行执行。
Count 发布包括零计数，各 owner 按本轮协议完成清理；同一物理窗口每隔一轮复用。
跨调用安全性依赖该执行顺序；多 stream 并发或多个在途 plan 不属于当前支持契约。

## 打点与性能统计

启用 profiling 的构建在每次 launch 中记录 Scalar 时间戳和事件 ID，复用每核记录槽，
最终保留最后一次 launch。profiling OFF 构建不产生这些记录。
`Publish` 发布记录计数；consumer 在 kernel 完成后读取。

| 区间 | 覆盖内容 |
| --- | --- |
| send_cnt_filter / send_cnt_issue | Count 筛选和下发，下发结束不代表 DMA 完成 |
| recv_cnt / cumsum | Count 等待与局部前缀、跨核局部和等待与全局偏移修正 |
| send_first_hop | 路由分类、第一跳发送和 DIRECT 任务准备 |
| send_direct | 按 Server 轮转的 DIRECT 发送 |
| send_prepare / send_drain_and_outputs | 发送准备、lane drain 和源端两表输出 |
| probe | 从开始扫描到得到非空 ready，包含多次无效等待，仅一对 begin/end |
| recv_token | ConsumeLoaded 的等待、metadata、本地输出及 relay 下发，不包含提前预取下发 |
| recv_enter/prefix_load/tasks/prepare/loop/drain/clear | 接收阶段边界 |
| finish_local / finish_return | 本核收尾及返回观察 |

kernel_total 从 profile 初始化后开始，到 Publish 前结束，不能等同于 msprof 的完整 kernel 时间。
嵌套区间不能重复求和，Scalar 下发区间也不能直接视为 DMA 完成时间。
性能比较需核对编译模式、核数、阈值、去重开关、shape 和二进制摘要。
运行模式、采样与报告口径见[EP 验证指南](../EP_VALIDATION_GUIDE.md)，
事件解释见[打点指南](../../tools/profiling/OPERATOR_PROFILING_GUIDE.md)。

## 验证边界

结果校验在 NPU 批次完成后进行。独立参考从全局原始路由重建每卡预期，逐 Rank 核对：

- cnt/prefix、expert 计数和完整路由覆盖。
- 最终 hidden、量化时的 dynamicScalesOut，以及输入权重未被修改。
- TokenType、DestinationIndex、类型 2 的 RelayReadIndex 有效块、SourceMask 及有效 SourceWeight。
- 参考中的成员读取槽与类型 3 写入槽一致，所有消费槽的 Flag 已清理。

现有 workflow 要求同一构建/case 的全部 Rank 结果完整通过，不新增 Rank 间五表交换通信。
代码入口为[参考](../../tests/ep/integration/server_dedup_dispatch_reference.h)、
[consumer](../../tests/ep/integration/server_dedup_dispatch_consumer.h)和
[workflow](../../tools/validation/workflow.py)。

Host/替身测试覆盖协议字节、容量、相邻行保护、lane 生命周期及错误注入；真实 CANN 编译
验证目标工具链兼容性。NPU 数值、异步行为及性能按具体源码和产物单独验收，
已保存的执行证据见[验证指南](../EP_VALIDATION_GUIDE.md)与[进度记录](../EP_MEMORY_SERVER_DEDUP_PROGRESS.md)。

本文与代码保持同步：接口、字段或调度行为更新时，直接更新对应说明、公式和示例。
只在本文保留当前实现；优化过程与已废弃方案从 Git 历史查阅。

## Dispatch DIRECT 双半区优化记录（2026-09-22）

基于 Combine 优化9 `a5d2b55` 的半区映射，仅替换 sender 的 DIRECT Server 遍历。
用户反馈 Combine 优化9效果显著，未提供本次具体时延，因此不量化收益，也不推断
Dispatch 同样获益。Dispatch 第一跳和接收 relay 的调度均沿用原实现。

维护约束：q 是原始路由前缀位置，不能根据新发送顺序重算；核内所有第一跳先于
DIRECT。跨半区继续复用同一发送环，不能按半区重置 packets 或提前排空。
Dispatch CMake kernel 依赖显式包含共享 select 头文件，确保后续映射修改触发增量构建。
独立 Host 参考用列表旋转验证奇偶半区顺序，延迟 MTE3 模型检查每个后续 payload
读取时仍有上一个发送在途，同时继续校验原始 q/包头/字节、源端表和多轮双窗保护。

本次验证：

- blue CANN9.1.0、ccec clang15.0.5，A5 `dav-c310-vec`/`CATLASS_ARCH=3510`：
  Dispatch kernel profiling OFF/ON 独立构建均通过，包含量化/非量化生产模板。
- 5项 Host 回归通过：Count OFF/ON、source-send OFF/ON、receive-packet。
  source-send 覆盖 N=2/3/7/8/16、两侧半区和绕回、空目标 Server、第一跳与 DIRECT
  混合、原始 q/包头/权重、连续 lane、多轮双窗和相邻输出保护。
- 默认 Send32 纳入上述回归；Send16/24/40/48 及 Send32 关闭去重分别构建并运行
  source-send 测试，全部通过。本地 source-send OFF/ON 构建也通过。
- 源码基线 `a5d2b55`、1113个非文档文件SHA256、编译器版本、构建/测试日志和
  退出码0保留于 `build/dispatch-half-verification/`，最终源码与验证快照一致。

Host sender 的逐字节检查使用非量化输入；量化发送的设备数值不由上述 Host 结果证明。
本版本 A5 多卡功能（含量化）、performance 和 breakdown 尚待验证，未登记性能收益。
