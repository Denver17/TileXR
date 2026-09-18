# EP Memory 服务器级去重设计

## 设计状态与源码基线

2026-09-18 按 `/users/hzy/workspace/TileXR`、`codex/SilentEmotion` 的当前源码校准。
核对基线为 `4858ce6`（包含编译期分核、GM tiling、Count 重叠及编译期去重开关）；
该源码身份不等同于生产环境已部署版本。本次只修订文档，不改变 Dispatch 行为。
Dispatch 优化设计阶段收口，后续工作转入 [Combine 待确认项](ep-memory-server-dedup-combine-open-topics.md)。
“设计收口”表示以现有实现为基线，不表示全部规模、性能目标或偶发环境问题已经验收。

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

本节替代历史按值标量 launch 布局；只迁移 tiling，不引入 epHandle。
公共 Host API、13 个输入输出/通信地址及其输出语义保持不变；未使用的
assistInfoForCombineOut 地址仍保留。内部 Kernel 参数顺序为这 13 个地址、
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
K 位均有效，输出权重全为真实值。不能只将原第一跳的 forwardCount 置零，否则会漏 route。

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
tailOffset = align32(quantized ? quantCount + scaleBytes : H*2)
payloadBytes = tailOffset + align32(32 + 16*(K-1))
blockCount = ceil(payloadBytes/480)
packedPayloadBytes = blockCount*480
T = tokenStride = blockCount*512
```

MXFP8 scale 位于逻辑 payload 的 quantCount 起点；每 32 个 hidden 元素一个 1B scale，按两项对齐。
输出只写 H 个 FP8 元素和 scaleBytes 字节，通信补齐不变成 public 行间距。
H=7168、K=6：非量化 payloadBytes=14464、T=15872；MXFP8 payloadBytes=7520、T=8192，
输出每行含 7168B hidden 和 224B scale。

### Payload 与 DataAsFlag

每个 512B wire 块包含 480B Payload 和 32B Flag（8 个 FP32 1）。所有块的 Flag 都参与 ready；
最后一块也在内，未使用的 entry/padding 不解析。没有另一个 metadata-ready 消息。
逻辑字节位置 `x` 到 wire 位置的换算为 `x + floor(x/480)*32`。
接收侧保留带 Flag 的整包，不先完整 unpack 到第二份 payload buffer；头部和每个 entry 分别换算
wire 位置，输出时 MTE3 按区间跳过 Flag。

## Token 格式与转发

### 随 Token 存放

尾部属于 Dispatch Payload，与 hidden/scale 一起按完整 T 发送，不另发控制包。

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
| `forwardTokenIdx` | 未使用，填 -1 | 尚未分配，填 -1 | 首次转发前使用第一跳输出 row 作为 t，所有副本相同 |
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

每个 lane 由同一 wire 包及独立 assist/group 区组成，没有两套整包输入/输出池：

```text
B = blockCount，M = 16，m = 本核实际任务数
prefixBytes = align32(E*4)
tablesBytes = 2*align32(m*4)
flagsBytes = M*B*32 + 256
workBytes = M*align32(ceil(B/8))*4 + 256
fixedBytes = prefixBytes + tablesBytes + flagsBytes + workBytes + M*36
G = align32(4 + 8*(K-1))
laneStride = T + 32 + G
Nrecv = min(8, floor((190KiB-fixedBytes)/laneStride))
```

M*36 包含每候选 32B sum 存储和 4B 快照下标；Sum 使用紧凑结果下标，其额外分配和保护区用于
匹配 CANN 9.1/A5 的实际访问范围。输入保证 Nrecv≥1；Nrecv 不固定为 2，也不与 Nsend 共用。

调度使用累计 issued/consumed：先将 ready 批最多 Nrecv 个包排入 MTE2，再每消费一个补充预取，
序号跨批连续。`IssueLoad` 等待 MTE3_MTE2(lane)，读取完整 wire，再设置 MTE2_MTE3 与 MTE2_S。
`ConsumeLoaded` 等待 MTE2_S 后做 Scalar metadata，S_MTE3 后等待 MTE2_MTE3，依次下发 hidden、
可选 scale、assist、可选 group 及全部 relay；最后设置 MTE3_MTE2。下一次 MTE2 覆盖同一 lane
前必须消费该事件。因此多 lane 允许 MTE2 与前序 MTE3 重叠，标点不证明硬件已实际重叠。

### 公共转发副本与输出

DIRECT 的 assist 直接保留八字段。第一跳先用 t=row 写独立 assist/group，再将当前 wire 的
forwardCount 改为 -1、relayRank/t 改成本 owner，保留全部 entries 和原始头部 k/weight。
所有 relay 写复用该只读 wire，仅变更目的地址。终端根据自身物理槽的 expert 匹配有效 entry，
把对应 originalK/weight 写入 assist，不用公共头部的第一跳权重。

MTE3 输出从 wire 跳过 Flag，按 row 密排写 hidden/scale；assist 和 group 也由同一 lane 的
MTE3 完成事件保护。当前不增加 metadata 地址缓存、Vector 整包中间拷贝或每 token 完成屏障。

## Dispatch/Combine 公共接口与 metadata shape

当前公共函数为 `TileXRMoeEpDispatchMemoryServerDedup`，三个持久 metadata 输出为 assistEx、
sourceMask、group，另有独立 `float *expertScalesOut`（groupOut 后、dtype 前）。
真实 expertScales 输入只读，Host 明确拒绝与权重输出重叠；裸指针 API 不推断所有分配的实际长度，
调用方负责容量和其他 buffer 不重叠，分配夹具可用专门的 allocation validator 检查。

| 输出 | 分配容量 / 有效范围 | Combine 的使用契约 |
| --- | --- | --- |
| expandXOut | Crows*H*(quantized?1:2)B；前 tokenNum 行有效 | expert 计算后按原 Dispatch 行序交回 |
| dynamicScalesOut | 量化时 Crows*scaleBytes；前 tokenNum 行有效 | Dispatch hidden 解量化所需；不等于路由权重 |
| sendCountsOut | int32[E] inclusive prefix | 最后一项给出 tokenNum |
| expertTokenNumsOut | int64[L]，累计或逐 expert 数 | 按 expert 定位输出分段 |
| assistEx | Crows*32B，前 tokenNum 行有效 | 保存行角色、原始 source/k/weight、relay 引用 |
| sourceMask | uint32[BS] | 只等待/消费置位的原始 k 槽 |
| group | Crows*G，G=align32(4+8*(K−1)) | 仅第一跳 assist 引用的稀疏 group[t] 有效 |
| expertScalesOut | FP32[BS,K] | DIRECT 真实 weight；group returnK 为 1.0；mask 空洞不读 |

### AssistEx 布局与写入前转换

assistEx 的八个 4B 字段顺序是 srcRank、srcTokenId、k、FP32 weight、forwardCount、
forwardEntryCount、relayRank、forwardTokenIdx。第一跳保留正数 n 和真实第一跳 weight，
终端保留 -1 角色并从 entry 取得自身 k/weight；DIRECT 引用为 (-1,-1)。

### Relay Group 成员表

Group 的 offset 0 为 uint32 firstHopRow=t；offset 4 起最多 K−1 个 8B 成员，
每项为 uint32 recvSlot=t*K+originalK 和 FP32 weight。仅前 n 项有效，成员按原始 k 升序；
第一跳不进入 member 列表，firstHopRow 指向它的本地 expert 行。group 行可有空洞，不发布 group 数 X。

### SourceMask 生成与写出

源端在第一跳下发后将 returnK 置位、对应权重写 1.0；DIRECT 下发后将原始 k 置位并写真实权重。
核末按本核连续 Token 区间写出，不消费未置位的 weight；K=32 按 uint32 位集合处理。

调用方保持 assistEx/sourceMask/group/expertScalesOut 到对应 Combine 消费完成，且 expert 内部
若重排行，须先还原 Dispatch 行序。不跨调用重用 t 作为持久 ID，也不在 Combine 重新扫描通信
payload 构造 plan。上述是 Combine 将消费的输入契约，不表示该去重 Combine API/实现已存在。

## 中转编号与持久表

### 2026-09-16：Dispatch 使用 Token 级 prefix 编号（当前契约）

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

## Combine 数据流

本节为后续 Combine 的设计契约，尚未实现，不作为 Dispatch 设备能力或 Combine 验收结论。

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

### R07-01 单份通信数据域布局

每 Rank 至少 8 GiB peer memory；`windowOffset=(uint64(magic)%2)*4GiB`。
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

当前 groupOut 已按 Crows 分配，但 relay 结果预留仍为旧 8*BS*K*U；两者不能混用。
沿用 t*K+k 地址契约时，Combine 至少需要 Crows*K*memberStride 的可寻址空间；实际 group 数小
也不能限制最大 t。应先重算完整窗口和 Host/caller 分配，再实现返回，不得把高 t 直接写入旧区。
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
多个在途 Dispatch plan、重复 Combine 或失败后复用的支持证明，后续归 Combine 生命周期议题。

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

Combine 下一轮先完善以下问题：

1. expertOut、member 返回、partial、累加器及最终输出的 dtype、精度与权重只应用一次的规则。
2. 稀疏 t 的返回区容量与窗口预算，Dispatch 输出到 Combine 的生命周期和 magic 选择。
3. expert 返回、relay 归约/partial、source 归约的核职责、H 分块与不会循环等待的推进方式。
4. 各类 Flag/UB/GM 的唯一 owner、完成与清理，以及 Combine-only 独立 oracle 和联合验收。

保留 [Combine C15–C24](ep-memory-server-dedup-combine-open-topics.md#本轮迁入边界) 的讨论入口，
不把 Dispatch 的动态 lane、核数或已收口参数直接当作 Combine 的最终调度选择。
