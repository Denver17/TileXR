# EP Memory Server Dedup Dispatch / Combine

Server Dedup 在远端服务器内复用同一 Token 的传输：Dispatch 对同一 Token、同一远端
Server 的多条专家路由只跨机发送一次，再在 Server 内转发；Combine 在该 Server 内加权
聚合，再向原始 source 返回一个 partial。本地 Server 和远端单条路由直接传输。

本文描述当前完整实现的接口、数据与同步契约。构建验证见
[构建说明](../BUILD_VERIFICATION.md)。普通 Memory 使用独立的公开入口和实现，见
[EP Memory](ep-memory.md)；不能用它的元数据解释 Server Dedup 输出。

## 支持范围

记 R 为 rank 数，E 为总专家数，L=E/R，BS 为每 rank 原始输入 Token 数，H 为 hidden
长度，K 为 top-k。所有 rank 的 shape、构建配置和调用顺序必须一致。

| 项目 | 当前契约 |
| --- | --- |
| 平台 | A5 / Ascend950，固定 64 AIV；项目目标 CANN 9.1.0，Host C++14 |
| 拓扑 | 16..256 ranks，8 的倍数；每 Server 连续 8 ranks，serverId=rank/8 |
| 专家 | 0<E≤1024，E 能被 R 整除；expert/L 为目标 rank，expert%L 为本卡 expert |
| 输入 | BS、H 为正；1≤K≤min(32,E)；每 Token 的 K 个 expert ID 合法且不重复 |
| Dispatch | FP16/BF16 输入；quantMode=0，或 quantMode=4 输出 MXFP8 E4M3/E5M2 |
| Combine | FP16/BF16 expertOut 与 yOut，quantMode=0；不直接接收 FP8 expertOut |
| 不支持 | active mask、shared expert、TP 分片、多 stream 并发、在途多 plan 和失败重试协议 |
| 占位参数 | activeMaskType=NONE、xActiveMask=nullptr、共享专家数为 0；tpWorldSize=0/1、tpRankId=0、expertShardType=0 |
| 通信 | Memory peer window，同节点与跨节点；不自动切换到 UDMA 或普通 Memory |

以上是 shape 上界，不能代替容量准入。Host 还检查整数乘积、Count UB、通信窗口和
Combine lane 预算；Combine 的实际接收行数必须在 `[0,min(C,10000)]`，其中
`C=R*BS*min(K,L)`。设备读取 sendCounts 末项并拒绝非法行数，Host 不回读输入张量。
裸指针接口不能验证全部分配容量、所属设备和张量内容；调用方负责这些条件。

### 去重策略与分核

CMake `TILEXR_EP_SERVER_DEDUP_ENABLE=auto|0|1` 选择构建策略，默认 auto。

| 策略 | 资源布局 | 算法行为 |
| --- | --- | --- |
| auto | 始终使用去重容量、通信包和 UB 布局 | 原始 BS≤32 全部直发；BS>32 开启 Server 去重 |
| 1 | 去重布局 | 强制去重 |
| 0 | 紧凑非去重布局 | 全部原始 route 直发 |

资源布局由 `kEpServerDedupEnable` 决定，实际行为由 `EpServerDedupEnabled(bs)` 决定。
不能用接收 tokenNum 选模式，也不能把 manifest 的布局布尔值当作 auto 小 BS 的执行模式。
Dispatch/Combine 必须使用相同策略和原始 BS；auto 小 BS 仍须提供完整容量的五表指针。
强制 0 时表 1/3/4 可为空；非去重执行不消费这些表、不保证写入其内容。

Dispatch 与 Combine 默认各使用 32 个发送核，其余为接收核；两者可独立配置。
`TILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM` 支持 16..48、步长 4；`TILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM` 支持 16..56、步长 4。
Dispatch 的 Count 在 BS<512 时由全部 64 核参与，否则由接收核参与，与数据核分工独立。
改变策略或分核必须重建 Host、kernel、consumer 并向全部 rank 同步；同一 communicator
生命周期内保持配置一致。rank 上限从 128 扩至 256 改变了 `CommArgs` 数组尺寸、后续字段
偏移和相关 credit 布局；通信库、全部消费这些结构的 Host/kernel 及下游必须配套重建，
所有 rank 部署同一版本，不能混用旧二进制。Host 容量检查不证明 256P 实机已通过。

## 公共接口与生命周期

公开声明位于 [tilexr_ep.h](../../src/include/tilexr_ep.h)：

- `TileXRMoeEpDispatchMemoryServerDedup`：输入 x、expertIds、FP32 expertScales，输出
  expert 输入、计数和五张持久表。
- `TileXRMoeEpCombineMemoryServerDedup`：输入 expertOut、Dispatch 计数和五表，输出
  按原始 Token 顺序组织的 yOut。

成功返回仅表示异步提交成功。对应 stream 完成后才能读取输出；五表必须存活到对应
Combine 完成，expertOut 必须保持 Dispatch 的行序。expert 计算若重排行，须先恢复。
同一 communicator 在所有 rank 上按相同顺序、同一 stream 串行调用；销毁前等待全部工作完成。
`epWorldSize/epRankId/globalBs` 的 0 值沿用从 communicator/shape 推导的约定，非零值必须匹配。

Host 检查 A5、64 核、参数及布局，注册独立 AICore 二进制后提交。内核文件安装于
`lib/tilexr/kernels`，由 owning Host 库相对加载，不嵌入 Host 共享库。
Dispatch 的 32B tiling 由 communicator 的 `KernelTilingCache` 以完整配置缓存为不可变
GM 快照，保持到 communicator 销毁；参数准备失败不得提交 kernel。固定 ABI 以公共头和
launch 实现为准，改变 tiling 或 CommArgs 时必须同时更新 Host、kernel 和消费者。

## 输出与五表契约

`tokenNum=sendCountsOut[E-1]`，只消费前 tokenNum 行。容量尾部无定义，不要求清零。

| 输出 | 容量与内容 |
| --- | --- |
| expandXOut | C×H 元素，量化时 1B，否则 2B；前 tokenNum 行有效 |
| dynamicScalesOut | 量化时每行 `align2(ceil(H/32))` 字节；非量化为 nullptr |
| sendCountsOut | int32[E]，按 localExpert/source 排列的 inclusive prefix |
| expertTokenNumsOut | int64[L]；expertTokenNumsType=0 为累计数，=1 为逐 expert 数 |
| 表 1 tokenTypeOut | uint32[C]；1=DIRECT、2=第一跳、3=转发终端 |
| 表 2 destinationIndexOut | uint32[C][3]，每行精确 12B |
| 表 3 relayReadIndexOut | 每行 K 个 8B 块，行距精确 K×8B；仅类型 2 行有效 |
| 表 4 sourceMaskOut | uint32[BS]，原始 Token 的有效返回 k 位集合 |
| 表 5 expertScalesOut | FP32[BS,K]，最终结果槽的权重 |

表基地址至少 4B 对齐，不能把 12B 或 K×8B 的行距额外对齐到 32B。表 1–3 索引的是
本卡接收 row，表 4–5 索引的是本卡原始 Token；两者不是同一索引空间。
原始 expertScales 只读且不能与 expertScalesOut 重叠，其他分配也须保证容量和无冲突写入。

### 表 2：目的地址

| TokenType | 三项内容 | Combine 用途 |
| --- | --- | --- |
| 1 | source rank、source TokenId、原始 k | 原始 source 的结果槽，返回未加权 expertOut |
| 2 | source rank、source TokenId、returnK | 同一结果槽格式，返回已加权 group partial |
| 3 | 本卡 stage 逻辑 slot、UINT32_MAX、UINT32_MAX | 将未加权 expertOut 暂存在本卡供 relay 读取 |

类型 3 的第一项是 uint32 槽号，不是字节偏移，0 是合法槽。Combine 的
`assistInfoForCombine` 为 int32 指针，须按该无符号位型解释槽号。

### 表 3：成员读取清单

第 0 块为 `uint32 N` 和 `FP32 selfWeight`。N 是除本地第一跳外的成员数，
`1≤N≤K-1`；K=1 不产生类型 2。后续第 1..N 块为：

| 字节偏移 | 字段 |
| --- | --- |
| 0 | uint16 全局 expertId |
| 2 | uint16 q：原始 source 发往该 expert 的段内序号 |
| 4 | FP32 memberWeight |

只消费 N+1 个块，其余块无定义。成员按原始 k 的相对顺序排列；0 权重也是合法贡献，
有效性由类型和 N 决定。当前 E 与 Count UB 准入使 expertId/q 可无损压入 uint16，
扩大 shape 范围时必须重新检查，禁止截断后寻址。

### 表 4/5 与权重

每个 DIRECT route 在 SourceMask 中置原始 k 位，SourceWeight 保存真实权重。
同一远端 Server 的 group 只置最小 k（returnK），该位置权重为 1.0；原始各权重保存在
表 3。未置位位置不消费。K=32 须按完整 uint32 位集合处理，不能使用有溢出的移位表达式。
非去重模式隐式等待全部 K 槽，表 5 原样复制全部路由权重。

交给 Combine 时：表 1→tokenType，表 2→assistInfoForCombine，表 3→relayReadIndex，
表 4→sourceMask，表 5→expertScales；sendCounts 仍是完整 int32[E] 前缀。

## 路由、行序和寻址

原始 source 为 s，目标 expert 为 e，其所属 rank 为 m=e/L，localExpert=e%L。
q 是原始 expertIds 的 token/k 扫描顺序中、该 route 之前发往 e 的贡献条数。
Count 统计全部原始 route，包括之后由 relay 代理转发的贡献；relay 不转发 Count，
也不将 wire 中的 source 改成自身。

```text
cntIndex = localExpert*R + s
row = prefixBefore(cntIndex) + q
slot(s,m,e,q) = (((s+m)%R)*L + e%L)*BS + q
window(magic) = (uint64(magic)%2) * (CommArgs.peerMemBytes/2)
Dispatch address = peer[m] + window(Dmagic) + dispatchOffset + slot*T
Combine stage address = peer[m] + window(Cmagic) + stageOffset + slot*Sstage
Combine result address = peer[s] + window(Cmagic) + resultOffset + (token*K+k)*Sresult
```

prefixBefore(0)=0，其余取前一项前缀。q 不随包发送次序改变；row、通信 slot、原始 TokenId
不能混用。类型 3 输出的 stage slot 与 relay 从表 3 计算的 slot 必须相同。

同一 Token 可能同时有本地 DIRECT、远端单条 DIRECT 和多个远端 group。
每个 group 以最小原始 k 为第一跳；relay 自身的 expert 贡献参与聚合，不能只读其他成员。
成员可能与 relay 同卡，此时仍经过该成员的 stage 槽；第一跳自身则直接读取 expertOut。

## 共享通信域与包格式

每个 peer 逻辑通信域均分两半，`peerMemBytes` 须是 1024B 的倍数，peer 基址 512B 对齐。
每半区有同样布局；Host 按每半区检查需求并扣除 `TILEXR_FLAG_BUFF_BYTES`，不能只在
两半合计后扣一次 runtime 保留区。实际大小来自 CommArgs，不能硬编码窗口步长。

| 每半区偏移 | 内容 |
| --- | --- |
| 0..1244 KiB | Dispatch 控制区 |
| 1244..1340 KiB | Combine 控制区 |
| 1340..1536 KiB | 对齐预留区，两个算子均不得使用 |
| 1536 KiB 起 | 共享数据区，Dispatch 与 Combine 分别解释 |

控制表精确偏移以 [buffer layout](../../src/ep/common/ep_memory_server_dedup_buffer_layout.h)
为唯一来源。Dispatch Count 的 GM 槽为 512B，实际传输前 32B；固定表起点、表内 stride 和
有效复制长度分别计算。固定 counter 区是预留布局，不作为当前接收分配器。

Dispatch 数据区有 E×BS 个 wire 槽。每个 512B 块由 480B payload 加 32B Flag 组成，
所有块（包括尾块）的 Flag 均到达才允许消费。逻辑 payload 字节 x 的 wire 偏移为
`x+floor(x/480)*32`。包头为 32B，四个有效 uint32 依次为 source rank、TokenId、
原始 k（group 使用 returnK）、type；类型值与表 1 一致。去重布局在包头后保留连续 K×8B 索引段，若会跨 Flag
则整体移到下一块 payload 起点。布局计算见
[data layout](../../src/ep/common/ep_memory_server_dedup_data_layout.h)。

非量化 hidden 占 H×2B。MXFP8 用 `align256(H)` 个量化元素的通信空间，scale 紧随其后；
每 32 个 hidden 元素一个 scale，字节数向 2 对齐。公开输出只写 H 个 FP8 值和有效 scale，
通信 padding 不能变成输出行距。量化只作用于 hidden，路由权重保持 FP32；relay 原样转发
FP8/scale，不二次量化。非量化 dynamicScalesOut 必须为空，量化时输出 dtype 显式选择
E4M3 或 E5M2 并提供 scale 缓冲区。

Combine 结果区从 1536 KiB 开始，含 BS×K 个槽：

```text
Sresult = ceil(H*2/480)*512
resultBytes = BS*K*Sresult
stageOffset = resultOffset + resultBytes
Sstage = align512(H*2)
stageBytes = (布局启用去重 ? E*BS : 0)*Sstage
requiredWindowBytes = stageOffset + stageBytes
```

stage 保存未加权 FP16/BF16，不带逐 Token Flag；可读性由 Server ready 协议保证。
结果槽使用 DataAsFlag。Dispatch 和 Combine 复用数据区，但各有控制区和本次 magic。
联合 D→C 每次 launch 分别取新 magic，因而两个算子常各自固定使用一个不同半区；
不能拿联合循环次数代替各自 magic 计算地址。模型必须保证窗口消费后才复用，当前协议
不提供额外 epoch、多 stream 或并发 plan 保护。首次使用的通信状态须为零；重复调用
依赖本轮 Flag 清理与一致的 shape/layout。动态 shape 切换不能仅凭布局准入就认定安全，
须另行检查切换前后的窗口状态与设备回归。

## Dispatch 执行与所有权

1. Count 从原始路由生成每 expert/source 的数量，包括零计数。接收组计算前缀，发布
   expertTokenNumsOut 和 sendCountsOut，按各自 owner 消费并清理控制 Flag。
2. 发送核先构造远端 group 并发送第一跳，再按 Server 调度 DIRECT；每条任务保存原始
   Token/k/expert/q。无 Token 权重分区的核仍可能拥有非去重 route，不能提前退出。
3. 接收核按 count/prefix 建立稳定任务，Probe 检查整包就绪，使用可用 UB lane 预取。
   输出 hidden/scale 与表 1/2；第一跳额外保存表 3，再按成员清单转发。
4. 转发只将包头 type 改为 3，source/Token/returnK/索引段与数据保持不变。第一跳的本地
   类型和返回身份须先保存，不能被 wire 修改污染。终端通过接收槽生成表 2 的 stage slot。
5. 各 lane 输出与转发完成后才能复用；所有输出排空后，owner 只清自身包的 Flag，保留
   payload。发送核写回 SourceMask/SourceWeight 后完成，kernel 完成才代表全部输出就绪。

Count 异步发送的 UB records 必须保留到 MTE3 读完；sender/receiver scratch 只能复用已经
释放的区域。前缀发布完成后才可 Reset/reuse。每核最终排空本核流水并恢复 CTRL[60]。
预取 lane 的输入可用、Vector 读取完、MTE3 读取完是不同事件，不能因指令已下发就复用。

## Combine 执行与数值语义

实现中的 M0–M6 是当前算法模块名，并不要求调用方理解开发阶段。

| 模块 | 当前职责 |
| --- | --- |
| Init/M0 | 读取 device tokenNum、准备当前窗口和 UB；非法 tokenNum/无 lane 触发设备错误 |
| M1 | 全核把类型 3 的未加权 expertOut 写到本卡 stage，并发布本核完成 |
| M2 | 指定接收核汇总 Server 内卡的 stage 完成并通知；接收核等待 Server ready |
| M3 | 发送核将类型 1 的未加权 expertOut 返回 source；非去重时处理全部有效行 |
| M4 | 接收核对类型 2 读取同 Server 成员，以 FP32 加权累加，返回 partial |
| M5 | 接收核读取 SourceMask 指定的返回槽，用表 5 加权累加并写 yOut |
| M6 | 非去重的最终归约，隐式等待全部 K 槽；不消费类型、relay 或 mask 表 |

去重时 M1 后发送组进入 M3，接收组经 M2→M4→M5；非去重跳过 M1/M2/M4，执行 M3/M6。
ready 只能在 stage 数据对读取者可见后发布；stage 不需要再携带路由或权重表。

M4 以 FP32 累加 `selfWeight*本地结果 + sum(memberWeight*成员结果)`，随后舍入回输入
FP16/BF16 再传输。M5 将收到的各有效槽转换为 FP32，按表 5 加权累加，最终舍入为 yOut。
DIRECT 在 M3 不预先乘权重。零权重槽仍须等待、消费并清理 Flag，不能用权重跳过协议。
这一中间舍入意味着不能用单次无限精度全局 sum 作为逐位相同的参考结果。

M4/M5 共享 UB，但 M4 未完成的输出仍有独立生命周期。M5 复用区域前须按物理槽等待
相关输出退役；M6 没有 M4 的事件可继承，须初始化自己的输出槽并按真实映射排空。
每个最终 Token 的全部结果块就绪后才读入；完成后清理本轮所有结果 Flag，不覆盖 payload。
无效表行、未置位槽及 padding 不作为数值判据。

## Profiling 与维护边界

Server Dedup 使用 EP 专用兼容实现：设备 writer 位于 `src/ep/common/ep_profiling_compat.h`，
Host 配置、分配与导出位于 `src/comm/ep_profiling_compat/`。它使用 `CommArgs` 中的兼容
记录字段及 `TILEXR_PROFILING_*` 环境配置，不改变独立 SDK 的公开入口或捕获生命周期。
`src/include/tilexr_profiling.h` 仍转发到 `<tilexr/profiling.h>`；新接入使用 `TileXR::Profiling`。

EP Dispatch/Combine 使用独立记录域，复用 latest launch 槽；每核记录时间戳和数字事件 ID，
末尾发布有效计数和丢点数。兼容导出与独立 SDK 共用 `records.v1` 文件协议及
`tilexr_profiling` 离线读取包，文件兼容不代表捕获生命周期相同。
事件解释由各自 mapping JSON 维护。EP 的 Dispatch/Combine 输出分别由
`TILEXR_PROFILING_RECORDS_PATH` / `TILEXR_PROFILING_COMBINE_RECORDS_PATH` 配置，
`TILEXR_PROFILING_CAPTURE_ID` 指定共享 UUID，`TILEXR_PROFILING_RECORDS_PER_CORE`
设置每核容量；在 communicator 初始化前设置，未设置输出路径时禁用对应记录域。

记录位置是 Scalar 到达点；issue 结束不等于 DMA 完成，只有原有完成 fence 才提供完成
语义。禁止为了打点新增同步。Probe 等无界重试按一次最终 ready 前的整体等待记录，避免
轮询耗尽记录区。Host、kernel、consumer 必须共用容量规则；latest capture 的有效范围和
overflow 状态每轮重置，不能混入上一轮尾部。

Host 准入、CPU 替身、参考模型和 CANN 编译各自只能证明对应边界；真实异步 DMA、跨机
进度、256P 规模与性能须在匹配环境中验证。本规格不将历史局部测试提升为当前设备验收。
后续改动应同步更新当前契约及必要回归，实验过程和旧设计通过 Git 历史追溯。
