# EP Memory 服务器级去重设计

## 设计状态与源码基线

2026-09-20 按 `codex/SilentEmotion` 的五表输出实现更新；设计基线为 `3b0ba92`。
Dispatch 已将旧 assistEx/group 接口替换为 TokenType、DestinationIndex、RelayReadIndex、
SourceMask 和 expertScalesOut。设计依据及逐项确认见
[Dispatch 输出适配方案](ep-memory-server-dedup-dispatch-output-adaptation-design.md)。
跨算子的交接契约见[联合设计](ep-memory-server-dedup-dispatch-combine-design.md)，
Combine kernel 尚未实现。编译/Host 证据不代表新版本已在生产 A5 上运行，验证边界见进度文档。

本文描述当前 Dispatch 代码，后面的 Combine 数据流明确标为设计契约。
[第二轮讨论](ep-memory-server-dedup-open-topics.md)与[接收优化记录](ep-memory-server-dedup-receive-optimization.md)
用于历史追溯，不覆盖本文。双 Probe、剩余 ≤5 时预探测、ready 后立即认领删除、Vector ready bitmap
以及 metadata 地址缓存均未落地，暂不继续推进，不能作为 Combine 已有能力使用。

| 源码 | 职责 |
| --- | --- |
| [公共入口](../../src/include/tilexr_ep.h)、[Host 检查](../../src/ep/host/ep_memory_server_dedup_host.cpp) | `TileXRMoeEpDispatchMemoryServerDedup`，配置与指针契约 |
| [launch](../../src/ep/host/ep_memory_server_dedup_launch.cpp)、[入口](../../src/ep/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.cpp) | 独立 GM tiling、120B launch、六种类型分支 |
| [主 kernel](../../src/ep/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.h) | Count、收发角色和本核完成边界 |
| [counts](../../src/ep/kernels/tilexr_ep_server_dedup_counts.h) | 原始路由计数、异步发送、前缀及状态清理 |
| [sender](../../src/ep/kernels/tilexr_ep_server_dedup_send.h) | 第一跳、DIRECT、量化打包、源端 mask/weight 输出 |
| [receiver](../../src/ep/kernels/tilexr_ep_server_dedup_recv.h)、[任务表](../../src/ep/kernels/tilexr_ep_server_dedup_receive_tasks.h)、[包处理](../../src/ep/kernels/tilexr_ep_server_dedup_receive_packet.h) | Probe、动态 lane、输出行和 relay |
| [Count 布局](../../src/ep/common/ep_memory_server_dedup_count_layout.h)、[数据布局](../../src/ep/common/ep_memory_server_dedup_data_layout.h)、[Host 容量](../../src/ep/host/ep_memory_server_dedup_layout.cpp) | 两窗、UB 预算、metadata 容量 |

## 支持范围

记 R 为 Rank 数，E 为总专家数，L=E/R，K 为 top-k，S 为发送核数，Crecv=64−S。
以下容量上界 Crows=R*BS*min(K,L)，不能与接收核数 Crecv 混用。

| 项目 | 当前契约 |
| --- | --- |
| 平台 | CANN 9.1 / A5；Host 公共接口保持 C++14，AICore 目标使用 gnu++17、dav-c310-vec / CATLASS_ARCH=3510 |
| Rank / Server | 每 Server 固定 8 Rank，连续编号，serverId=rank/8；R 为 16..128 内的 8 的倍数 |
| 专家 | 0<E≤1024，E 能被 R 整除；expert/L 定位 Rank，expert%L 定位本卡专家 |
| 输入 | BS>0、H>0，所有 Rank 的 BS 相同，globalBs=R*BS；1≤K≤min(32,E) |
| 路由 | 每 Token 恰有 K 个合法且互不重复的 expert；输入保证该契约，kernel 不做完整路由合法性扫描 |
| 功能 | 无共享专家、token mask；tpWorldSize 只接受 0/1，tpRankId=0、expertShardType=0 |
| 占位参数 | scales、tpRecvCountsOut、expandScalesOut 为 nullptr；assistInfoForCombineOut 保留但 kernel 不使用 |
| 通信 | Memory peer window 支持同节点及跨节点；本入口无 UDMA 自动切换或普通 Memory 回退 |
| 容量 | Host 检查 Count UB、控制区、单窗容量及整数乘积；调用方保证输出分配、收发 UB 能容纳实际任务及至少一个完整 lane |

三个宏在 `ep_memory_server_dedup_count_layout.h` 配置，修改后须配套重建 Host 和 kernel，
所有 Rank 使用一致产物；不是 run.sh 参数或运行时环境变量：

| 宏 | 当前默认值 | 行为 |
| --- | --- | --- |
| TILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM | 32 | 支持 16/24/32/40/48；发送核 [0,S)，接收核 [S,64) |
| TILEXR_EP_SERVER_DEDUP_ALL_CORE_COUNT_BS_THRESHOLD | 512 | BS<阈值时 64 核发送 Count，否则仅 Crecv 个接收核发送 Count |
| TILEXR_EP_SERVER_DEDUP_ENABLE | 1 | 1 为远端 Server 分组去重；0 为全部原始 route DIRECT |

Count 阈值与数据核比例独立；无源 Token 的核仍可能负责 Count。Count/prefix 的物理布局依赖
Crecv，同一 communicator 生命周期内保持该布局不变。当前编译开启 auto-sync、关闭
`-cce-aicore-record-overflow`；编译器自动同步和设备时序不能由源码标点数量推断。

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

收发组并行推进。不存在独立的 `all2allDispatch` 调度入口、全核 Count 阶段屏障或核末 SyncAll。
`CompleteCumsum` 到 receiver 的 PIPE_ALL 仍保留，防止后续 Reset/UB 重用覆盖未完成的计数输出。
Kernel 完成要求所有核完成，Host 异步 launch 返回不代表结果已可读。

## 2026-09-18：独立 GM tiling 参数（当前 launch ABI）

本节保留独立 GM tiling，不引入 epHandle。2026-09-20 五表替换已删除未使用的
assistInfoForCombineOut 槽，metadata 顺序为 type/destination/relayIndex/mask/weight。
内部 Kernel 参数仍为 13 个输入输出/通信地址、
GM_ADDR tilingData、int64_t magic，共 120B。globalBs 继续在 Host 校验/推导，
不再传到 Kernel；magic 仍由 Host 每次取得并直接传入，不存入 tiling。

共享 EpServerDedupTilingData 固定 32B，字段如下，均在 Host 校验后窄化：

| Offset | 字段 | 类型 |
|---|---|---|
| 0、4、8 | bs、h、profilingLaunchSlot | uint32_t |
| 12 | moeExpertNum | uint16_t，当前最多 1024 |
| 14、15 | topK、reserved | uint8_t，发送/接收核数由编译期宏配置 |
| 16～20 | expertTokenNumsType、quantMode、dtype、expandXOutDtype、reserved | uint8_t |
| 21～31 | reserved | 11B，Host 全部置零 |

profilingLaunchSlot 的 UINT32_MAX 无效值保留。非量化的 RESERVED 输出 dtype 在
Host 转成输入 dtype 后打包。核内不新增范围检查，也不缩小地址计算使用的 64 位中间值。

Host 通过 communicator 管理的 KernelTilingCache 初始化独立的 32B Device GM，
在 aclrtMemcpy(HOST_TO_DEVICE) 同步返回成功之后取得真实 tilingData 地址。
Kernel 直接从该地址读取字段；入口读到局部值后，轮询和 Token 循环不重复读取 tiling。
rtKernelLaunchWithFlagV2 的 argsSize 恰为 **120B**，hasTiling=0、两个 tiling offset=0，
不附带 tiling 内容，不使用 Runtime 的占位地址回填。32B 数据单独初始化，不计入 launch 包。

缓存以完整且零填充的 32B 配置为 key；相同配置直接复用 GM 地址，不再次分配或复制。
不同配置（包括 profilingLaunchSlot 的变化）拥有不同的不可变 GM，旧配置一直保留，
因此后续提交不会覆盖尚未执行完成的 Kernel 参数。magic 不在缓存 key 内，逐轮递增
不会创建新条目。每种不同配置占一份 32B 逻辑 GM（实际分配按 Runtime 粒度取整），
生命周期内缓存随不同配置数增长；communicator 销毁时统一释放，不按 magic 淘汰。

缓存有互斥保护，不占用 collective 的单槽 InternalResource。Host 容器先建立所有权记录，
再分配/复制；分配或复制失败不向 launch 发布地址，返回错误，未完成初始化的分配仍由
缓存持有以供重试或销毁。释放失败保留对应所有权；跨进程清理状态并入已有的 Rank 状态一致性检查，
参与交换的 Rank 一起返回错误后可重试销毁，避免其他 Rank 已删除通信域时单卡重试状态交换。
销毁前须停止提交并等待所有依赖该 communicator 的工作完成，与现有 peer memory 生命周期一致。
CANN 9.1 的 aclrtMemcpy 是同步接口，故初始化使用的 Host 局部结构可在返回后释放；
不另设 pinned 缓冲区、异步 H2D 源生命周期或每轮同步等待。

维护要点：**传入指针并不等于只提交指针**。验证须检查实际 Runtime argsSize=120、
hasTiling=0，同时确认地址指向独立初始化的 GM；不能只检查 Kernel 签名大小。
单槽按配置直接覆写会破坏连续异步调用的旧参数，不可通过删除附带内容后随意复用可变缓冲区。

安装 manifest 记录 launch_bytes=120、tiling_bytes=32、runtime_args_bytes=120，
layout=r11-independent-gm-tiling。虽然 Kernel 签名未变，旧 r10/160B 随 launch 搬运产物
仍须重新构建/同步；本次需匹配 libtile-comm、owning Host 库和 Kernel。
公共 Host 接口及 epHandle 范围不变。

验证要求：Host 检查独立分配、同步复制、真实 GM 地址解析、120B 提交、配置命中不复制、
连续配置切换的旧内容保持、通信域隔离、分配/复制失败及释放重试。保留字段字节、
64 位 magic、五种核数、量化类型、profiling 无效槽及窄化前拒绝检查；编译实际
CANN 9.1/A5 入口、owning Host 库及通信库。Host 替身和编译不能证明 NPU 正确性或性能收益。

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

`SendCnt` 按目标 expert 连续区间分核，范围筛选/逐 expert reduce 保留原 Memory 数值算法。
当前把待发送记录与筛选 scratch 分离：

```text
countCores = BS < threshold ? 64 : Crecv
alignedRecords = align8(ceil(E/countCores))
保留发送记录 = alignedRecords * 32B（未参与或空 Count 核为 0）
Count 峰值 UB = 4*align256(BS*K*4) + alignedRecords*(32+4)
总 UB 预算 = 190 KiB
```

记录区只读保持到对应 MTE3 完成；独立 16 项 stagePool 与 5 项 countPool 视图使后续 sender
或 recv-count 重用筛选 scratch。`SendCnt` 下发后不执行 PIPE_ALL、TPipe::Reset 或 pool Reset；
这些操作会破坏预期重叠。sender 的 lane 预算扣除保留记录区。

接收端先等待自己负责的 incoming cnt，Gather/Sum 并生成本分段相对前缀，随后发布局部和、
清理本分段 cnt 状态；`CalcCumsum` 等待跨核局部和，取得全局偏移并修正本段前缀，再向各接收核
发布完整 prefix 的分段数据及 ready。sumCoreBuf 真正复用为 ready 源前等待旧 MTE3 读完。
`CompleteCumsum` 消费本核完整 prefix，清理 ready，并写 expert 计数。

`recv_cnt` 包含局部前缀计算；`cumsum` 包含跨核等待及偏移修正。send_cnt_issue 只测下发，
不代表 Count DMA 已完成。完整所有权和编译记录见[Count 发送掩盖方案](ep-memory-server-dedup-count-send-overlap.md)。
历史全核 Count 的 FP8 失败与 Reset 重设 CTRL[60] 有关；当前 Init 置 0，Count 不再 Reset，
Finish 恢复入口值。不能把历史的“Reset 后重新置 0”当成当前仍存在的调用序列。

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
即使某 group 全部 expert 位于 relay 本卡，后续成员仍走同一 relay 返回契约。

### 当前分类表与 DIRECT 调度

当前不是持久保存 directMask[count_i] 再重扫原始 Token。第一遍 `Classify` 使用 Scalar 的
seen/grouped/directMask、64B serverState 和 firstHopMasks：第一次遇到远端 Server 时暂列 DIRECT，
第二次遇到时生成 group 并移出 DIRECT，随后扩充该 group 位图。
每个 Token 的第一跳立即发送；DIRECT 记录 `(token,k,expert,q)` 写入 16B 的 directTasks。
第二遍从本 Server 开始按 `(myServer+step)%serverNum` 扫描该任务表，同 Server 内保持 Token/k 顺序。

| 源端 UB | 大小/职责 |
| --- | --- |
| serverState | 64B，当前 Token 的首 k 或 group 下标；关闭去重时复用为 Server route 位图 |
| firstHopMasks | align32(4*min(serverNum−1,floor(K/2)))，当前 Token 的 group 位图 |
| directTasks | align32(count_i*K*16)，最大 DIRECT 任务数，保存原始 q |
| sourceMask | align32(count_i*4)，group 保留 returnK，DIRECT 保留原始 k |
| weightIn / weightOut | 各 align32(count_i*K*4)，只拷入本核权重区间 |

关闭去重仍扫描 Server mask，但所有 route 进入 DIRECT 表，不发第一跳或 relay；sourceMask 的
K 位均有效，输出权重全为真实值。不能只将第一跳包头改为 DIRECT，否则会漏 route。

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

## Dispatch dtype 与 MXFP8（R08-03）

| 输入 | quantMode | expandXOutDtype | 每行公开输出 |
| --- | --- | --- | --- |
| BF16 / FP16 | 0 | 同输入，RESERVED 在 Host 解析为输入 dtype | H*2B；dynamicScalesOut=nullptr |
| BF16 / FP16 | 4 | 显式 FP8E4M3 / FP8E5M2 | H 字节 hidden，另有 scaleBytes 字节 dynamicScalesOut |

沿用 `TileXRMxfp8Quant` 的 ComputeMaxExp/ComputeScale/ComputeFp8Data 和现有 FP8 golden。
量化只处理 hidden；真实 route weight 始终 FP32。relay 保持收到的 FP8/scale 位型，不重新量化。

### 尾部容量与通信长度

以下是布局函数的实际公式，A32/A256 表示字节或元素数按标注向上对齐：

```text
quantCount = align256(H)
scaleBytes = quantized ? align2(ceil(H/32)) : 0
headerPayloadOffset = align32(quantized ? quantCount + scaleBytes : H*2)
readIndexPayloadOffset = headerPayloadOffset + 32
if readIndexPayloadOffset % 480 + K*8 > 480:
    readIndexPayloadOffset = align480(readIndexPayloadOffset)
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
最后一块也在内，未使用的 entry/padding 不解析。没有另一个 metadata-ready 消息。
逻辑字节位置 `x` 到 wire 位置的换算为 `x + floor(x/480)*32`。
接收侧保留带 Flag 的整包，不先完整 unpack 到第二份 payload buffer；Init 预计算 headerWireOffset 和 relayReadIndexWireOffset，
索引段物理连续，条目不再逐个换算；hidden/scale 输出时 MTE3 按区间跳过 Flag。

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
源端在打包前直接形成该段；第一跳收包后原样写入表 3，不做字段转换。

第一跳先将本地类型 2 与原始返回身份保存到独立 staging，最终转发前仅将 wire 包头 type
改为 3；包括索引段、其他头字段、占位及 Flag 在内的其他字节均保持不变。
所有成员收到相同 wire，成员地址由 expert/q 计算。终端直接记录本卡接收 slot，
不搜索索引段，不恢复自己的原始 k/weight，不再传递 relayRank/forwardTokenIdx。

## Dispatch 接收布局

```text
dataRegionIndex = ((srcRank + dstRank) % R)*L + localExpert
receiveAddress = peerMems[dstRank] + windowOffset + dispatchOffset
               + (uint64(dataRegionIndex)*BS + q)*T
```

DIRECT、第一跳和终端使用相同地址公式、跨度和原始 source 身份；每 route 一个独占槽位，
不使用 credit 循环复用。cumsum 规定最终输出 row，DataAsFlag 判定该槽是否能读，两者不能互相替代。

## 分核与监听

`receiver.Process` 在 Count 完成屏障后 Reset TPipe，重新加载本接收核的完整 prefix。
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

当前只有一个 Probe 批，最多 16 个候选：保存稳定 j 快照，MTE2 读取每包所有块的 Flag，
一次 MTE2_V 后用二维 `Sum<float>` 逐行归约，V_S 后 Scalar 比较各 sum 与 blockCount*8，形成 ready 位图。
未使用 Compares/Vector 位图，也没有第二个 Probe buffer。

ready=0 时 scan 向后推进，至 pending 尾部回绕；直到某批非零 ready 才进入搬运。
按快照下标从大到小加载/消费 ready 项，处理完当前整批后再 Probe。
每项的本地输出、全部 relay 与 lane 释放指令提交后，才用末项替换删除 pending；
倒序删除保证当前快照中较小下标仍有效。**不是 ready 一出现就认领删除。**

pendingCount=0 只代表全部任务已经下发；物理 DMA 完成仍依赖 lane 事件和最后 PIPE_ALL。
稳定 tokenPos 保留到核末清理，row 和 t 都不绑定可变 pending 位置。

### 动态 N 槽的输入与输出流水

每个 lane 由同一 wire 包及 type/destination 两个独立 32B staging 组成，没有两套整包输入/输出池：

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
匹配 CANN 9.1/A5 的实际访问范围。输入保证 Nrecv≥1；Nrecv 不固定为 2，也不与 Nsend 共用。

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
核末 drain、Flag 清理和本核完成边界保持；不恢复 SyncAll 或逐 Token 全流水屏障。

## Dispatch/Combine 公共接口与 metadata shape

`TileXRMoeEpDispatchMemoryServerDedup` 的五个 metadata 指针依次为 tokenTypeOut、
destinationIndexOut、relayReadIndexOut、sourceMaskOut、expertScalesOut，旧签名已废弃。
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
类型 2 第一跳自身的贡献取 expertOut[row]，不在后续成员条目重复保存。
类型 3 的 slot 是本卡 Combine stage 逻辑槽号，不是字节地址；stage 的物理 stride 由 Combine 决定。
Dispatch 的 uint32 DestinationIndex 可通过同一 GM 分配的 int32 指针视图交给 Combine 的
assistInfoForCombine，按 32 位位型解释，不复制/转换内容。

源端第一跳下发后置 returnK 位并写权重 1.0，DIRECT 置原始 k 位并写真实权重。
核末按本核连续输入 Token 区间写出；K=32 使用 uint32 位集合。关闭编译期去重时全为 DIRECT。
调用方保持五表到对应 Combine 完成；容量尾部及索引无效行/块不消费。

## 输出行与持久表

所有接收行继续使用稳定 Token 级 prefix 身份：`row=tasks.Begin()+j`，
不能用物理 tokenPos、段内 q 或变化中的 pending 下标替代。无需 AtomicAdd 或重新取号。
表 1/2 按 row 密排；表 3 仅类型 2 行有效，因此按最大输出行容量分配，允许稀疏空洞。
类型 3 的本卡物理逻辑槽直接来自 tasks.Position(j)，与第一跳索引中 expert/q 推导的成员槽一致。
旧 forwardTokenIdx、group.firstHopRow 和 t*K+originalK 的持久格式已移除；Combine 改用 pull。
历史 counter 留白与 profiling 编号保留，不生成原子取号事件，也不写 counter。

维护要点：从通信包直接保存 metadata 时，wire 角色与本地行角色必须分离保存，避免最终转发
改写导致异步输出 type_3 覆盖本地 type_2。4B/12B/8K 紧密表必须按实际字节写出，
不能沿用旧 32B 行搬运；测试须同时检查相邻行和 lane 在途数据，Host 替身不证明真实硬件时序。

## Combine 设计入口

Combine 内部实现集中在[Combine Server Dedup 设计](ep-memory-server-dedup-combine-design.md)，
按 M0、M1a/M1b、M2–M6 整理功能预期和简要设计方向；全部模块初版完成后再统一通读查漏。
本文不重复维护 Combine 内部流程，当前尚未实现该 kernel。

五表与 27 参数接口由[联合设计](ep-memory-server-dedup-dispatch-combine-design.md)维护。
当前 Dispatch 已产生五表；新 Combine 的消费与执行仍待实现。
新中转区的存储归属和物理布局以共同契约的后续收口为准。

## 容量与生命周期

### R07-01 单份通信数据域布局

每 Rank 至少 8 GiB peer memory；`windowOffset=(uint64(magic)%2)*4GiB`。
联合设计沿用此选窗方式，主要隔离 Dispatch/Combine；模型调度保证跨调用安全复用。
每窗前 2 MiB 为控制区，两窗各保留最后 4 MiB 不作数据使用，实际 runtime Flag 仅位于整个域末尾。
以下 offset 相对选中窗口，所有地址乘积使用 uint64：

| 区域 | offset / 字节数 |
| --- | --- |
| 历史 profile 留白 | [0,12800)，当前标准打点不用此区 |
| cnt | cntOffset=12800；E*32 |
| cumsum 交换 | sumOffset=cntOffset+E*32；Crecv*Crecv*32 |
| prefix 副本 | prefixOffset=sumOffset+Crecv*Crecv*32；Crecv*prefixStride，prefixStride=align32(E*4) |
| ready | readyOffset=prefixOffset+Crecv*prefixStride；Crecv*Crecv*32 |
| 历史 counter 留白 | counterOffset=align1024(readyOffset+Crecv*Crecv*32)；1024B，不读、不写、不清零 |
| source 返回预留 | sourceOffset=2MiB；BS*K*U |
| Dispatch 数据 | dispatchOffset=align512(sourceOffset+sourceBytes)；E*BS*T |
| relay 返回历史预留 | relayOffset=align512(dispatchOffset+dispatchBytes)；8*BS*K*U |

当前 U=ceil(H*2/480)*512，只是布局函数的历史非量化 Combine 返回预算；Dispatch 不访问
source/relay 返回区。Host 仍用 requiredBytes=relayOffset+relayBytes 检查单窗 4GiB−4MiB 上限，
stateBytes=counterOffset+1024 检查前 2MiB 上限。标准 profiling 另分配 GM，不复用历史留白。

### Combine 容量缺口

当前 relayReadIndexOut 按 Crows*K*8 分配，但通信域 relay 预留仍为旧 8*BS*K*U；两者不能混用。
旧 push 方案沿用 t*K+k 时至少需要 Crows*K*memberStride，实际 group 数小不能限制最大 t。
新 pull 方案已确定 expert/q 逻辑槽，生产卡 stage 预算为 E*BS*Sstage；source 结果为
BS*K*Sresult，加上控制 Flag、scale/scratch 和保留区。MaxTokenNum 是输出行容量，不是 stage 槽数。
后续确定具体格式、offset 并重算 Host/caller 分配，再实现读写，不得套用旧 relay 区。
若完整预算超出窗口，需在 Combine 设计中明确新的容量/窗口方案，不能假定 8GiB 自动足够。
原“实际 group 数≤8*BS 因而可用旧容量”的约束已失效，不是当前 Dispatch 输入要求。

### 清理与跨轮复用

接收核在所有 token 写指令提交后，消费全部 MTE3_MTE2 lane 事件；准备一个全零 Flag 源，
V_MTE3 后遍历稳定 tokenPos，仅清理自己拥有的每个包的 32B Flag 块，最后 PIPE_ALL。
清理不覆盖 payload、其他核的槽位或本卡转发目标尚待处理的 Flag。
当前采用核末统一清理，逐 token/批次清理不是仍待选择的当前实现。

Count 的 cnt/sum/ready 按各自 owner 消费后清理；prefix 数据无需清零，读取受本轮 ready 约束。
源端 mask/weight、group 空洞不额外清零，只读其有效范围。counter 已退休，核末不执行 SyncAll。
每核 `counts.Finish` 保留本核 PIPE_ALL 与 CTRL[60] 恢复，再记录总耗时终点和 Publish。

同一 communicator 的调用保持所有 Rank 顺序一致，在同一 stream 串行执行；相同窗口隔轮复用，
全 Rank Count 发布（包括零 cnt）及清理共同保护代际边界。此规则不能直接外推为多 stream、
多个在途 Dispatch plan、重复 Combine 或失败后复用的支持证明；这些扩展不列入本轮联合设计。
当前模型的跨调用安全复用由用户确认的执行顺序保证，不增加轮次管理或结束握手。

## 打点与性能统计

当前标准 writer 每条记录先读 GetSystemCycle，再 Scalar 写 GM 时间戳和事件 ID。
启用 breakdown 时每次 launch 都打点，覆盖相同每核槽位，仅保留最后一次 launch；
不是仅最后一轮才启用打点。profiling OFF 的 performance 构建不记录这些事件。
Publish 补齐、发布记录计数，不显式 clean/invalidate cache；消费者在 kernel 完成后读取。
总耗时终点在 Publish 前，tiling 读取及 profile 初始化也在起点之前，不能等同于 msprof 完整 kernel。

| 当前区间 | 含义 |
| --- | --- |
| send_cnt_filter / send_cnt_issue | Count 筛选和下发；下发区间不含完整 DMA 完成保证 |
| send_first_hop | 路由分类、第一跳发送和 DIRECT 任务准备；不等于纯网络时间 |
| send_direct | 按 Server 轮转的 DIRECT 下发循环 |
| send_prepare / send_drain_and_outputs | 第一跳循环前的准备，以及 DIRECT 循环结束后的 drain 和输出 |
| probe | 多次空扫描直到一次非空 ready，只有一对 begin/end |
| recv_token | ConsumeLoaded 的等待、metadata、本地输出、relay 及释放指令；不含提前预取下发 |
| recv_enter/prefix_load/tasks/prepare/loop/drain/clear 等 | 接收阶段边界，保留；与内部 token/probe 存在包含关系 |
| finish_local / finish_return | 本核收尾及返回观察；新记录没有 finish_sync |

不再产生 PacketIssue、load_issue/load_wait/metadata/output_issue/relay_issue、Atomic、CounterEnd
或 SyncEnd；旧 ID/mapping 保留解析历史 trace。Scalar 区间不证明各 DMA 引擎实际完成，父子时长
不能重复相加。某个记录自身的 GM store 开销会落在后续区间，打点改变编译和执行也可能扰动结果。

模式 1 使用 msprof，剔除 warmup 后取最后 min(50,loops) 轮：每 Server 将 8 卡所有选中样本取均值，
再输出各 Server 代表值的最大、最小、平均。模式 2 用最后一轮每 Rank 的最大 core kernel_total，
再统计跨 Rank median/min/max；两种口径不同，不把历史 49us/391us 直接当作当前版本的确定差额。
编译模式、核数、阈值、去重开关、shape、二进制哈希都须一致或明确记录差异。
详细语义与历史诊断见[打点指南](../../tools/profiling/OPERATOR_PROFILING_GUIDE.md)。

## 验证边界

当前 Dispatch 已实现完整 cnt/cumsum、源端发送、接收及 relay、metadata 输出、双窗与清理。
已有用户回传的 64P/E384、BS96、H7168/K6、BF16→E4M3、breakdown 100+50 轮全 Rank PASS，
run_id=b662f5c701e74ec187fdb7f09cb39bc9，kernel 摘要前缀 65f0523c47a6；它只证明该次产物和配置，
不覆盖此后分核、tiling、Count 重叠及编译配置的全部修改。最近 32P/BS256 的偶发执行失败
只有汇总信息，原因未定；不能登记成已确认环境故障，也不据此改协议。

本次文档核对不运行 NPU，不新增设备验收结论。历史 Host 替身与 CANN 编译仅覆盖其各自提交；
正式范围以保存的源码/产物身份和 run summary 为准，见[进度记录](../EP_MEMORY_SERVER_DEDUP_PROGRESS.md)。

Combine 后续按[模块设计](ep-memory-server-dedup-combine-design.md#模块边界m0m6)逐一完善初版，
完成所有模块后由用户整体通读，再整理遗漏点和待确认项。内部设计不在本文重复维护。
