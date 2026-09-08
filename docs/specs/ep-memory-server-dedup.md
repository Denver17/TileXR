# EP Memory 服务器级去重设计

## 设计状态

本文合入第一轮 D01-D11 及相关 Combine 讨论已经确认的设计，作为第二轮梳理的基线。
尚未决定的机制集中在[第二轮待确认项 R01-R12](ep-memory-server-dedup-open-topics.md)，
旧 C 编号见 [Combine 议题索引](ep-memory-server-dedup-combine-open-topics.md)。

本文区分已确认的逻辑契约与仍待确认的物理实现：字段示意、容量估算和流水约束不表示公共 ABI、
kernel 或性能已经实现并验证。后续确认更新本文对应章节，并关闭相应 R 项，不再维护多份重复的设计结论。
普通 Memory 行为以 [EP Memory 规格](ep-memory.md)为准；本设计使用独立去重入口，不隐式修改原入口。

## 支持范围

| 维度 | 阶段一约束 |
| --- | --- |
| Server 与 Rank | 每 Server 固定 8P；Server 数 N 为 2 至 16，总 Rank 数 R=8*N，即 16P 至 128P |
| Server 映射 | 连续 Rank 编号，`serverId=rankId/8`，使用整数除法 |
| 普通 expert | 连续 ID 均匀分配，每卡 expert 数不为 1、最多 32 个，典型为 3 或 6 个 |
| Expert 映射 | `rankId=expertId/L`，`localExpertId=expertId%L`，`moeExpertNum=R*L` |
| Top-k | `1<=K<=32`，典型为 6、8、16 |
| BS | 所有 Rank 的原始输入 BS 相同，`globalBs=BS*R`，`maxBS=BS` |
| 输入保证 | 每个 Token 恰有 K 条有效 route；expert ID 在 `[0,moeExpertNum)` 内且互不重复，无空 route 占位 |
| 输入 mask | 阶段一不支持 expert mask 和 token mask，内部不读取或执行筛选 |
| 共享专家 | 阶段一不支持；`sharedExpertNum` 与 `sharedExpertRankNum` 必须均为 0；后续低优先级扩展 |
| 实现与接口 | 原 Memory 与去重入口分开；去重实现独立放在 `.h` 中，不满足支持条件直接报错，不自动回退 |
| 平台基线 | 保留 C++14、CANN 9.1.0 及现有 EP A5 边界；使用 Memory peer window 承载同节点与跨节点通信 |

典型配置为 128P/384 experts（每卡 3 个）和 64P/384 experts（每卡 6 个）。
256P、K>32、任意 Rank/Server 映射及非均匀 expert 分布不属于阶段一。

输入保证直接作为算法前提，不增加重复 top-k 或 expert ID 范围扫描，不继承 invalid expert 跳过分支。
不支持的功能保留接口参数，但不因保留参数而执行旧功能。具体空值/默认值约束在 R10 闭合。
`sourceMask` 和 `memberMask` 是路由元数据，与已排除的输入 mask 功能不同。

## 路由与计数

同一轮 Dispatch 按 `(srcRank,srcTokenId,destinationServer)` 识别可合并的第一跳 hidden，
不比较 Token 数值。阶段一只做 Server 级去重，Server 内保留每条原始 expert route 的独立 payload 和接收槽位。

对于采用 relay 的远端 Server：

1. 原始 source 选择一条实际 expert route 作为第一跳；relay 必须也是该 Token 的 expert 目标。
2. 第一跳只发送一份 hidden，绑定 relay 上该 expert/source/q 的正常接收槽位，并携带其余 route 列表。
3. Relay 保留第一跳本地 expert 贡献，逐 entry 写入目标 expert 的原始接收位置。
   同一目标卡出现三次，就执行三笔 expert payload 写入。
4. Relay 自身额外的 expert route 也写入其各自接收槽位，供对应监听 owner 消费。
   共用发送 UB 不等于卡级去重，不省略任何 expert 行。

源 Server 内由原始 source 直接逐 route 分发，Combine 直接返回原始 source。
远端 Server 仅一条 route 时没有后续转发任务，可使用 DIRECT。
远端仅一张目标卡但命中多个 expert 时，所有 expert 行都必须保留；
是否形成仅含 relay 本地贡献的 group，以及如何编号，在 R01/R04 确定，不能将其当成单条 route。
示例沿用 top-k 首目标作 relay；生产选择策略仍为 R01 待确认项。

所有原始 expert cnt 由原始 source 统计并直接发送，包含经 relay 到达的 route。
Relay 不转发 cnt，不为代理写入另加 cnt；数据位置的 source 身份始终保持原始 srcRank。

| 数量 | 计数单位 |
| --- | --- |
| BS | 每个原始 source 的输入 Token 数 |
| M | 本卡 cumsum 得到的 expert 接收行总数，同一 Token 命中本卡多个 expert 时计多行 |
| X | 本 relay 合计需要监听其他七张卡返回的不同原始 Token 数，按 `(srcRank,srcTokenId)` 去重 |
| Y | 上述 X 个 Token 对应的远端 expert 结果条数 |
| 全部本地 group 数 | 还可能含仅有本地贡献的 group，不能直接与 X 等同 |

相同输入 BS 不代表各卡的 M 或 X 相同。一个需要远端返回的 Token 有多份 expert 结果时，
X 只加一，Y 按结果条数增加；按 relay 至少保留一条本地 route 的约束，`X<=Y<=X*(K-1)`。
零 cnt 或整卡 M=0 是合法路由结果，不表示输入存在空 route，也不自动允许该卡跳过其他职责。

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

第一跳与终端包使用相同的 T。新增尾部只增加通信记录长度，不改变 hidden 的 H、cnt 或最终 expert 行数。
本轮每条 route 独占一个接收槽位，不采用逐 Token credit、槽位申请或应答来循环复用该位置。

接收端按本地 expert/source 顺序生成包含当前项的 cumsum，某段之前的累加值加 q 得到其最终输出行。
例如 cnt=`[2,0,3]`，cumsum=`[2,2,5]`，各段起始行为 `[0,2,2]`。
第三段 q=1 对应输出行 3，但通信窗口中的物理位置仍按上式确定。

原始 cnt 表示“应收多少”，cumsum 表示“最终放在哪”，到达 Flag 表示“数据能否读取”。
即使 cumsum 完成，也必须确认 hidden 和全部有效尾部到达后才能消费 Token。
零 cnt 仍沿用 Memory 的计数通知，后续不生成该空区间的 Token 监听任务。

## Token 格式与转发

### 逻辑字段

保留原始身份、头部 k/weight、三态 forwardCount 和逐 route 列表；
新增公共返回引用 `(relayRank,forwardTokenIdx)`。
终端包必须保留原列表，不能用返回引用覆盖 `forwardEntries[0]`。

| 字段 | 已确定的语义 |
| --- | --- |
| srcRank / srcTokenId | 原始 Token 身份，跨转发保持不变 |
| 头部 k / weight | DIRECT 的当前 route，或第一跳绑定的 relay 本地 route；终端公共包中保留第一跳值 |
| forwardCount | 0：DIRECT；正数 n：第一跳有 n 条转发任务；-1：终端包，不再执行 Dispatch 转发 |
| forwardEntries | 每条记录目标 expert、目标 q、原始 k 和对应 weight，转发时原样保留 |
| relayRank / forwardTokenIdx | 中转卡及其分配的本 Token 返回编号，所有转发副本共用 |
| 有效 entry 数 | 必须在 forwardCount 改为 -1 后仍可恢复；独立 forwardEntryCount 是当前候选编码 |

当前字段排列候选为：

```text
srcRank, srcTokenId, k, weight, forwardCount,
forwardEntryCount, relayRank, forwardTokenIdx, forwardEntries

entry = {destinationExpertId, destinationQ, originalK, weight}
```

这是逻辑示意，尚未确认 ABI、字段位宽、offset、padding 或通信 stride，详见 R03。
第一跳绑定的 route 不再列入 entries，因此 `n<=K-1<=31`；
DIRECT 没有有效 entry，第一跳返回编号尚未由 relay 分配。
K 是整个 Token 的路由数，不能用 K-1 代替某个目标 Server 的实际 n。

### 公共转发副本

Relay 在第一次转发前取得本 Token 的 forwardTokenIdx，保存原始 n、第一跳本地行信息及处理进度。
发送 UB 只准备一次：将 forwardCount 设为 -1，填入 relayRank/forwardTokenIdx，
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

一条逻辑发送包含 hidden、统一尾部和到达 Flag；不另发 group 注册、plan、metadata-ready、
逐 Token 完成通知、应答或 credit。原始 cnt、核内 Event 和必要的调用/阶段同步仍保留。

到达判定必须覆盖 hidden、全部有效 entries 和公共返回引用。
现有 Memory 的 480B 数据加 32B Flag 分块格式可作为实现参考，但新记录的实际打包和发布顺序须在 R03 核对。
一次逻辑发送不表示整包原子可见。

公共发送 UB 从首笔发送使用起保持只读，直到全部相关任务下发且最后一笔引用它的 MTE3 完成。
某个目标完成、待处理表剔除或 PIPE_MTE3 排序，都不单独证明 UB 已可由 Scalar/Vector 覆盖。
尚未下发完的 entry 和已分配编号必须持续有效，恢复处理时不重复发送或取号。

## 分核与监听

Cnt 接收/cumsum、LocalWindowCopy 和转发由同一组核负责，不单独分出专职转发核。
非全核发送 cnt 时，该组核还发送本 Rank 作为原始 source 的 cnt；全核发送 cnt 时由全核参与该阶段。

执行顺序为：完成本卡 cumsum，确认结果对监听核组可见，再按实际 expert 接收行数 M 划分任务。
不再在 cumsum 完成前扫描预留 Token 区。各 Rank 不必同时进入监听阶段，
但具体核间同步、核数和源端发送任务分工尚待 R02/R05 确认。

对于 C 个监听核，可以用连续逻辑区间表达均分：

```text
core i:
    begin = floor(i * M / C)
    end = floor((i + 1) * M / C)
```

每条 expert 接收行只有一个 owner；不能整除时相差至多一行，M<C 时允许部分核无监听任务。
按行数均分不保证转发工作量均分。同一 source/expert 分区可以跨核切分，q 范围不可重叠。

每核保存稳定位置表 tokenPos 与可变待处理表 pendingIdx。
例如 M=2048、C=16，每核分配 128 行：

| 数据 | 大小与作用 |
| --- | --- |
| tokenPos | 128 个 uint32 位置记录，由稳定下标 j 访问；编码单位待 R05 确定，不存截断的 GM 指针 |
| pendingIdx | 128 项，初始为 0..127，保存尚未完成所有任务下发的稳定下标 |
| pendingCount | 初始 128，仅此前缀有效；变为 0 表示本核全部任务已下发 |

若 pendingIdx 也使用 uint32，两张表共 1024B，不包含 Flag scratch、发送 UB 和在途状态。
一般配置中每核表长由实际 M 决定，不能将本例 128 或 Combine 的 BS 上限当作所有场景的表长上限。

监听检测使用 MTE2/Vector/Scalar，发送使用 MTE3，包含向本卡接收区的写入。
监听不依赖无关的 MTE3 完成；数据准备到相应发送、UB/Event 复用仍有必要依赖。
允许多笔 MTE3 在途，具体资源数与非阻塞完成检测方法待 R05 核对，不假定多套物理 MTE3 引擎。

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
退出前以 `PipeBarrier<PIPE_ALL>` 等待本核已下发的写入和清理完成；该操作不是跨核或跨 Rank 屏障。

## 中转编号与持久表

### 原子取号

每张 relay 为当前轮次维护跨监听核共享、初值为 0 的计数器。
对需要其他卡返回结果的原始 Token，由唯一第一跳 owner 在首次转发前分配一次编号：

```text
forwardTokenIdx = atomic_fetch_add(relayListenTokenCount, 1)
```

这里描述“原子加一并获得本次旧值”的语义，不是已核对的目标 CANN API。
成功轮次编号为 `0..X-1`，各目标卡和 expert 共用；它是分配顺序，不是网络到达顺序。
同卡额外 route 的监听核、部分转发恢复均不再次取号。

分配编号不表示持久表项已就绪，也不表示发送完成。计数器初始化、可见性、复用及准确返回语义在 R05/R11 核对。
仅有 relay 本地贡献的 group 不计入 X，其编号与本地关联方式属于 R01/R04，不能直接套用这一个计数器。

### 元数据与所有权

沿用 assistEx 与 forwardPlan 两个元数据 tensor 的逻辑约定。
中转监听表已确定由 Dispatch 生成并作为 Combine 输入，如何在既有元数据中组织由 R04/R10 确定。

| 逻辑数据 | 生成者与消费者 | 必须保留的含义 |
| --- | --- | --- |
| assistEx | 当前 expert 行的 Dispatch 接收 owner 生成，Combine expert 发送方消费 | 原始 srcRank/srcTokenId、匹配的 k、DIRECT 或 relay/forwardTokenIdx 引用 |
| sourceMask | 原始 source 在分组时生成，source Combine 消费 | 本地输入 Token 最终需要读取的 source slot 集合 |
| relay group / 监听记录 | 第一跳 owner 建立，相关本地行 owner 补齐，relay Combine 消费 | 原始身份、成员 k、远端发送卡及槽位、本地 expert 行关联、returnK，及数值方案需要的权重 |
| X 与超限状态 | Dispatch 生成，Combine 在通信前检查 | 合计不同监听 Token 数及可信超限状态，不能由表容量反推实际数量 |

原始三列 assist 的身份语义保留；扩展行在逻辑上包含五项：
`(srcRank,srcTokenId,k,relayRank,forwardTokenIdx)`。
逻辑行 shape 为 `[M,5]`，DIRECT 返回引用沿用 `(-1,-1)`；具体字节打包在 R04 确认。
sourceMask 的逻辑 shape 为 `[BS]`，每个 Token 一个 32 位位集合，仅最低 K 位参与消费。

第一跳本地 expert 行同样属于 group；额外本地 expert 行通过同一随包引用补齐关联。
单一 localRow 不能覆盖每卡多 expert，具体按 k 保存行引用或采用其他可直接定位的布局尚未决定。
ExpertOut 是否必须保留 Dispatch 行序在 R10 确定，不能假设中间 expert 计算不会重排。

临时 pendingIdx、发送 UB、Token 尾部不是可跨 Dispatch/Combine 生命周期的 plan。
Dispatch 完成须覆盖原始输出和全部持久元数据写入，不能在结束时通过扫描 payload 重建 group。
Plan 及其引用的路由数据必须存活到对应 Combine 使用结束；不向终端卡传输整份 relay plan。

## Combine 数据流

Combine 保留每条 expert 输出的独立贡献，先在目标 Server 的 relay 汇聚 group，
再将 server partial 返回原始 source。Expert 输出通常数值不同，不按 payload 相同来合并。

| 路径 | 目标与消费方式 |
| --- | --- |
| DIRECT | 返回原始 source 的 `[srcTokenId][k]` 结果位置；本地 DIRECT 可直接读取 |
| 其他 expert 卡 -> relay | 返回 relay 的发送卡分区、forwardTokenIdx、原始 k 对应槽位 |
| Relay 本地 expert | 从关联的本地输出行取得贡献，不占七个远端发送卡分区 |
| Relay partial -> source | 返回原始 source 的 `[srcTokenId][returnK]` 位置 |
| Source 最终归约 | 只消费 sourceMask 指定的结果位置，被 partial 代表的其他 k 不再单独等待 |

对于同 Server 且 senderRank 不等于 relayRank 的返回，使用：

```text
senderLane = senderRank % 8
relayLane = relayRank % 8
peerRegion = senderLane < relayLane ? senderLane : senderLane - 1

slotIndex = (uint64(peerRegion) * BS + forwardTokenIdx) * K + originalK
combineAddress = mappedPeerWindowBase[relayRank]
               + relayResultRegionOffset + slotIndex * memberSlotStride
```

该映射将其他七张卡编号为 0..6。发送方通过自己的 peer 映射计算地址，
不传递 relay 的本地指针，也不查询远端 group 表。
同一原始 Token 跨分区共用 forwardTokenIdx；不同 expert 按原始 k 区分。
Relay 只等待持久表中实际存在的远端贡献，不等待全体预留 slot。

沿用已有顺序约定：group 按原始 k 升序归约 memberMask 中的贡献，source 按 sourceMask 的 slot 升序归约。
`returnK` 是该 group 的最小原始 k。Source 从 K 个有效位开始，将一个 group 的成员位替换为 returnK 位。
这不会丢弃 expert 贡献，而是将其合入一个 partial；本阶段存在的每个输入 Token 都有有效 route，
不沿用 inactive token 或空 route 的零 mask 规则。

K=32 时 mask 必须按 32 位位集合解释，bit31 不是负长度或无效标志；
构造完整 K 位集合不能使用未定义的 32 位左移 32 操作。

每条 route 的 weight 必须在合并前应用且只应用一次。已确认随 Dispatch 包提供对应信息：
头部保存第一跳 weight，entries 保存各自 weight，终端按 expert 匹配提取。
Weight 的实际应用端、持久布局、Dispatch 后能否修改、无 weight 语义及各阶段 dtype 尚待 R08。

如果采用 relay 统一加权候选，expert 返回未加权结果，relay 逐 k 加权后汇总，
source 对该 partial 不再乘 returnK 的 weight；DIRECT 仍需自己的加权责任。
sourceMask 本身不能区分 DIRECT 和已加权 partial，必要的类型信息在 R04/R08 决定。
两级归约改变浮点求和括号，不承诺与普通 Memory 位级一致。

Combine 的 expert 发送、group 监听/归约、partial 发送及 source 归约如何分核、按 H 分块和并行推进，
仍属 R09，不能直接将 Dispatch 的已确认核组设计当作 Combine 调度方案。

## 容量与生命周期

### 固定中转结果区

每张 relay、每份在用中转 buffer 固定预留 `7*BS*K` 个结果槽位，七块各供一张同 Server 的其他卡发送。
准入条件为本卡合计 X<=BS；X>BS 时 Combine 直接报错，不自动扩容、分批或回退。

这个限制只针对不同监听 Token 数。X=BS 且 Y>BS 可接受；
每张发送卡各自涉及的 Token 数不超过 BS、但合计不同 Token 数超限时仍须拒绝。
仅含本地贡献的 group 不计入 X，也不能据此把它们的元数据容量截断为 BS。

```text
logicalShape = [7, BS, K, H]
relayResultBytes = 7 * BS * K * memberSlotStride
```

按当前非量化 Memory 结果通信格式估算：

```text
payloadBytes = H * dtypeBytes
blockCount = ceil(payloadBytes / 480)
memberSlotStride = blockCount * 512
physicalShape = uint8[7, BS, K, blockCount, 512]
```

每块含 480B 数据、32B Flag，最后一块可有填充。
BS=256、H=7168、K=6 且通信 dtype 为 FP16/BF16 时：

```text
payloadBytes = 14336
blockCount = 30
memberSlotStride = 15360 bytes
relaySlots = 10752
relayResultBytes = 165150720 bytes = 157.5 MiB
```

每个发送卡分区为 22.5MiB。若同时保留 P 份独立中转 buffer，上式乘 P；两份为 315MiB。
不额外乘总 Rank 数、expert 数或核数。最终 dtype/scale/返回字段改变时须重新计算 stride，
该数值不是最终公共 workspace 大小或硬件分配结果。

### 其他区域与超限

Dispatch 数据区仍按 `R*L*BS` 个槽位、各 T 字节预留；X 的上限不能缩小其原始 source/expert 分区。
这一乘积不含 cnt、状态、同步区或额外 workspace。
实际 expert 接收行数上界为 `M<=R*BS*min(K,L)`，监听表容量和位置编码须据此核对。

源端结果区、assistEx、forwardPlan、监听表、原子计数器、Flag scratch、发送 UB 和累加暂存另行预算。
不同区域的 dtype/stride 与同时存活关系在 R07/R09 确定；
只有确认生命周期不重叠后才能复用，不能因沿用 Memory 逻辑就认定已有 window 足够。

第 BS+1 次原子取号得到 forwardTokenIdx>=BS 时，Dispatch 不得先写越界表项。
必须记录可信超限状态，最终由 Combine 拒绝；不能取模、覆盖旧行或截断 X。
超限轮次的 Dispatch 如何继续/退出，以及 Combine 通信前各 Rank 如何协调失败，仍属 R07。
单个 relay 报错而其他 Rank 继续写入或无限等待，不构成完整错误协议。

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

区分保存的 Dispatch plan 身份与每次 Combine 通信轮次。
沿用运行时 `TileXRCommNextMagic` 的新轮次机制，局部 Flag 清理不替代代际管理。
顺序重复 Combine、多 stream、多个未完成 plan、失败重试与 ping-pong 复用的支持范围在 R11 明确。

## 完整示例

BS=256、H=7168、K=6、R=128、总 expert 数 384，每卡 L=3。
R0 的 T8 选择 `[25,26,27,28,29,30]`，本例按首目标规则选 R8 为 relay。

| k | Expert | 目标 | Dispatch | Combine |
| --- | --- | --- | --- | --- |
| 0 | 25 | R8/local1 | 第一跳 R0 -> R8 | 本地 expert 行 |
| 1 | 26 | R8/local2 | R8 -> R8，独立接收槽位 | 本地 expert 行 |
| 2 | 27 | R9/local0 | R8 -> R9 | 返回 R8 的 R9 分区 |
| 3 | 28 | R9/local1 | R8 -> R9 | 返回 R8 的 R9 分区 |
| 4 | 29 | R9/local2 | R8 -> R9 | 返回 R8 的 R9 分区 |
| 5 | 30 | R10/local0 | R8 -> R10 | 返回 R8 的 R10 分区 |

R0 直接为六条 source/expert route 发送 cnt。T8 前已有 T0..T7，
所以 q25..q30 分别由这些更早 Token 的路由决定，不能假定为 0、8 或相互相等。

第一跳含 hidden 和下列内容；有效长度使用 R03 的独立字段候选：

```text
srcRank=0, srcTokenId=8, k=0, weight=w0
forwardCount=5, forwardEntryCount=5
forwardEntries=[
    (26,q26,1,w1),
    (27,q27,2,w2),
    (28,q28,3,w3),
    (29,q29,4,w4),
    (30,q30,5,w5)
]
```

R8 一次取号得 t=7（示例值，与 T8 的 8 无必然关系）。
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

R9 的 peerRegion=0，R10 的 peerRegion=1，因此四个远端 Combine 位置为：

| 结果 | 逻辑位置 | 相对槽位下标 |
| --- | --- | --- |
| expert27 | `[0][7][2]` | 44 |
| expert28 | `[0][7][3]` | 45 |
| expert29 | `[0][7][4]` | 46 |
| expert30 | `[1][7][5]` | 1583 |

R8 的两条本地贡献不占七个分区。T8 对 X 加一，对 Y 加四；
group 成员为 k0..k5，returnK=0，sourceMask 从 `0b111111` 变为 `0b000001`。
完成组内加权归约后，partial 返回 R0/T8/k0；R0 不再等待本组 k1..k5 的独立结果。

若整数为 32 位且 weight 为 FP32，候选头部 8 个 4B 字段加五条 16B entry，
有效逻辑尾部为 112B。不含 Flag、对齐或未使用的预留 entry 容量，不作为最终 T。
示例在非量化 hidden 相同的前提下省去跨 Server 的五份 hidden 第一跳，
仍保留五笔 Server 内逐 expert 写入；它不证明端到端性能收益。

## 验证边界

本文完成文档归并、逻辑示例及容量核对，没有实现或测试去重 kernel。
确认协议必须区分三种证据：路由/地址的参考计算、目标 CANN API 的编译核对、实际设备的可见性/前进性验证。
计数完成不代表数据到达，原子取号不代表 plan 发布，任务下发不代表 UB 或通信窗口可以复用。

后续验证要求见 R12：使用可区分各 expert 的结果，检查贡献守恒、原始 k/weight、返回地址、
零接收、同卡多 expert、原子并发、部分发送恢复、X 边界与跨轮生命周期。
不增加已由安全输入契约排除的重复/invalid top-k 测试。

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
