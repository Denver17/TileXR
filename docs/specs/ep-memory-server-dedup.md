# EP Memory 服务器级去重设计

## 设计状态

本文合入第一轮 D01-D11、相关 Combine 讨论及第二轮已经确认的设计，作为后续梳理的基线。
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
| 初版中转容量 | 安全用例保证每个 relay 的实际 group 数不超过 `8*BS`；初版不生成 X 状态，不做超限校验、报错或超限测试 |
| 输入 mask | 阶段一不支持 expert mask 和 token mask，内部不读取或执行筛选 |
| 去重范围 | 仅处理原始 source 与目标位于不同 Server 的 route；源 Server 内逐 route 直发 |
| 共享专家 | 阶段一不支持；`sharedExpertNum` 与 `sharedExpertRankNum` 必须均为 0；后续低优先级扩展 |
| 实现与接口 | 原 Memory 与去重入口分开；去重实现独立放在 `.h` 中，不支持的配置直接报错，不自动回退；中转容量安全输入契约不属于初版校验项 |
| 平台基线 | 保留 C++14、CANN 9.1.0 及现有 EP A5 边界；使用 Memory peer window 承载同节点与跨节点通信 |

典型配置为 128P/384 experts（每卡 3 个）和 64P/384 experts（每卡 6 个）。
256P、K>32、任意 Rank/Server 映射及非均匀 expert 分布不属于阶段一。

输入保证直接作为算法前提，不增加重复 top-k 或 expert ID 范围扫描，不继承 invalid expert 跳过分支。
不支持的功能保留接口参数，但不因保留参数而执行旧功能。具体空值/默认值约束在 R10 闭合。
`sourceMask` 是持久路由元数据，与已排除的输入 mask 功能不同；group 的贡献集合通过第一跳及有序 member 列表表达，不额外保存 memberMask。

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
   共用发送 UB 不等于卡级去重，不省略任何 expert 行，也不为单卡多 expert 改用旁路填充。

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
Cnt 统计、发送记录、接收和 cumsum 计算沿用当前 Memory，统计输入仍是完整原始路由。
去重版不采用 Memory 的双 Group 逻辑：不按 32 Rank 条带筛选发送目标，不执行其两轮发送或轮次间全核同步。
原始 source 的 cnt 按既有分核覆盖全部目标 expert，每项发送一次，随后进入原有接收/cumsum 流程。
源端数据的“先第一跳、后 DIRECT”是本设计的发送阶段，与已取消的双 Group 轮次无关。

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

这里的 `tokenNum` 是当前 Rank Combine 的有效输入 Token 数，由现有 `sendCounts` 的有效计数确定，
不是独立新增的 public scalar；它与 `expertOut` 的有效行数和 `assistEx` 的第一维相同。
首版不含共享专家，沿用 Memory 的 `tokenNum=sendCounts[L*R-1]`，按已有容量分配、按有效行数消费。
R04-02 已确认沿用 Memory 输出行序：`expertOut[row]` 与 `assistEx[row]` 一一对应，
expert 内部若重排，须在交给 Combine 前还原 Dispatch 行序；不新增 public row map，不消费容量中的 padding 行。

| metadata | public shape / stride | 作用 |
| --- | --- | --- |
| `assistEx` | `int32/FP32` 位视图 `[tokenNum,8]`，每行 32B，stride 32B，总字节数 `tokenNum*32B` | 描述每个 Combine 输入 Token 行的 source、originalK、角色、weight 和返回引用 |
| `sourceMask` | `uint32[BS]`，stride 4B，总字节数 `BS*4B` | 描述每个原始 source Token 最终需要消费的 k 槽位 |
| `group` | `uint32[8*BS,rowStride/4]`，`rowStride=align32(4*(1+2*(K-1)))` | 保存每个 relay group 的 firstHopRow 和前 n 个 `(recvSlot,weight)` member |

`group` 的第一维是安全容量上限，不代表实际 group 数；实际有效行通过有效第一跳 `assistEx` 关联，
在 `[0,tokenNum)` 中以 `forwardCount>0` 识别第一跳，取 `t=assistEx[row].forwardTokenIdx` 访问 `group[t]`，
且 `group[t].firstHopRow=row=dstPosition`。此关联已由 R04-03 确认，不提供 X，也不扫描未写入的 8*BS 行。
R04-05/R07-02/R07-03 已确认 `recvSlot=uint64(t)*K+originalK`，本卡及跨卡返回共用 `[8*BS,K]` 个结果槽位，来源卡不参与编号。
`8*BS` 是本 relay 合计 group 容量；`assistEx[tokenNum,8]` 和 `sourceMask[BS]` 的第一维保持各自既有语义。

R04-04 沿用 Memory 的算子完成语义：Dispatch 所有核完成全部任务及 GM 写入后，Dispatch 算子才结束；
后续 Combine 据此读取完整的三个 metadata 输出，不另设元数据发布协议、ready Flag 或额外同步。
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
这是补充信息整体的 32B 对齐；固定头部单字段为 4B，entry 当前候选为每条 16B，字段间不额外补齐到 32B。
对齐后的 Token 数据段和补充信息段统一视作一个 Payload，整体沿用 Memory Dispatch 的 DataAsFlag 拼接、检测和剥离方式。
具体长度公式见[尾部容量与通信长度](#尾部容量与通信长度)，收发流程见 [Payload 与 DataAsFlag](#payload-与-dataasflag)。
接收端从 Token 尾部提取 Combine 所需信息并写入持久表；assistEx/group 的布局、行关联、写入分工及 recvSlot 编码已确认，完整区域预算继续在 R07-01 讨论。

### 逻辑字段

保留原始身份、头部 k/weight、三态 forwardCount 和逐 route 列表；
新增公共返回引用 `(relayRank,forwardTokenIdx)`。
终端包必须保留原列表，不能用返回引用覆盖 `forwardEntries[0]`。

固定头部字段排列已与 assistEx 的 32B 布局共同确认；forwardEntries 条目布局仍为候选：

```text
srcRank, srcTokenId, k, weight, forwardCount,
forwardEntryCount, relayRank, forwardTokenIdx, forwardEntries

entry = {destinationExpertId, destinationQ, originalK, weight}
```

下面给出逐字段字节表。固定头部的八字段、int32/FP32 类型、字段顺序、offset 及未使用返回字段填 -1 已确认。
逻辑身份、逐 route 的 k/weight、forwardCount 三态、原列表保留和公共返回引用的语义已确定；
forwardEntries 的具体字节布局、预留容量和 padding 初始化仍在 [R03](ep-memory-server-dedup-open-topics.md#r03) 收口。

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
| `forwardTokenIdx` | `int32_t` | 4B | 28 | 第一跳 owner 在本 relay 上通过 AtomicAdd 为本 Token 分配一次的非负编号 t；所有转发副本共用，用于 Combine 定位 group/返回槽位。未分配时填 -1 |
| `forwardEntries` | entry 数组，条目布局待 R03 | 候选每条 16B；有效部分 `16*n` B | 32 | 每条对应一次后续 expert 发送；有效范围 `[0,n)`，排除第一跳 route。转发时原列表完整保留，预留容量见下表 |

固定头部合计 `8*4=32B`。初版安全输入保证每个 relay 实际 group 数不超过 `8*BS`，
因此有效 `forwardTokenIdx` 落在 `[0,8*BS)`；Dispatch/Combine 不生成或传递 X，也不做中转容量超限校验、状态记录或报错。
`forwardTokenIdx` 是独立编号，不要求与 srcTokenId、q 或原始 k 数值相等，也不能用它们替代。
以上范围描述字段语义，不增加已由输入契约排除的逐 Token 安全扫描。
头部与 assistEx 的 weight 使用 FP32、group 由 relay 执行加权求和均已确认；计算精度及数值验收仍在 R08 确认。

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
| 容量内未使用的 entries / padding | 接收端不解析 | 接收端不解析 | 随完整 Payload 打包发送，接收端不解析其语义；初始化值在 R03 确认 |

头部的 forwardCount 描述包的角色和第一跳初始任务数；发送进度由 owner 的临时状态保存。
不能通过修改公共包中的 forwardEntryCount 或逐项移除 entries 来记录进度，否则会破坏后续接收端的匹配依据。

### ForwardEntries 条目字节表

每个 entry 的字段布局固定为下表候选，i 表示从零开始的 entry 下标，`0<=i<n`。
“条目内 offset”相对本条首字节，“尾部 offset”相对整个逻辑尾部首字节，单位均为 B。

| 字段 | 候选类型 | 大小 | 条目内 offset | 尾部 offset | 含义与使用方式 |
| --- | --- | --- | --- | --- | --- |
| `destinationExpertId` | `int32_t` | 4B | 0 | `32+16*i` | 此次后续发送的全局 expert ID，范围 `[0,moeExpertNum)`；目标 Rank=`id/L`，localExpert=`id%L`，无须再存独立目的 Rank 字段 |
| `destinationQ` | `int32_t` | 4B | 4 | `36+16*i` | 原始 source 发往该 expert 的从零开始的槽位序号 q，由 source 按 Memory 原始路由前缀计算；范围 `[0,cnt)`，有效 q 小于 BS。relay 用它计算接收地址 |
| `originalK` | `int32_t` | 4B | 8 | `40+16*i` | 此 expert route 在原始 top-k 中的位置，范围 `[0,K)`；用于恢复该 expert 行的 k 以及 Combine 返回信息，不等于 entry 下标 i |
| `weight` | `float`（FP32） | 4B | 12 | `44+16*i` | 与本条 originalK 对应的一个 expert route 权重；终端 expert 匹配本条后提取，不使用公共头部的 weight |

单条 entry 合计 16B，包含转发寻址及终端恢复自身 route 所需的信息；公共返回引用仅在头部存一份。
第一跳和 DIRECT 当前包的目的 expert/q 由接收槽位表达，不额外在头部重复携带。
终端同样从监听槽位取得自身 expert，在 `[0,forwardEntryCount)` 中唯一匹配；无需读取未使用容量。

### 尾部容量与通信长度

记 C 为一份通信记录预留的 entry 容量，n 为当前包的有效条数。
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
| 配置预留尾部原始长度 | `32+16*C` B | 候选按 `C=K-1` 固定预留；n<C 时剩余位置不含有效 entry |
| 对齐后尾部占用 | `align32(32+16*C)` B | 补充信息整体 32B 对齐，段末填充长度为 `align32(32+16*C)-(32+16*C)` |
| Payload 长度 | `tokenPayloadBytes=align32(D)+align32(32+16*C)` B | 对齐后的 Token 数据加补充信息；已包含两段 32B 对齐填充，尚未补齐 480B 分块或插入 Flag |
| DataAsFlag 数据块数 | `blockCount=ceil(tokenPayloadBytes/480)` | 以整个 Payload 计算，覆盖转发信息及其预留容量 |
| 补齐分块后的数据长度 | `packedPayloadBytes=blockCount*480` B | 最后一块补齐到 480B；剥离 Flag 后每条记录的 UB 跨度沿用 Memory 的这一长度 |
| Flag 总长 | `blockCount*32` B | 每块的 480B 数据后紧跟 32B Flag |
| Dispatch 通信 stride T | `blockCount*512` B | DataAsFlag 打包后的整条记录；同一配置的 DIRECT、第一跳和终端包使用同一 T，不能随 n 改变接收槽位跨度 |

按 `C=K-1` 候选预留时，各配置的尾部容量如下。原始长度反映字段容量，对齐后占用反映拼接进 UB 的段长：

| K | C（最多条目数） | 固定头部 | entries 预留 | 原始长度 | 32B 对齐填充 | 对齐后占用 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 0 | 32B | 0B | 32B | 0B | 32B；只有 DIRECT |
| 6 | 5 | 32B | 80B | 112B | 16B | 128B |
| 8 | 7 | 32B | 112B | 144B | 16B | 160B |
| 16 | 15 | 32B | 240B | 272B | 16B | 288B |
| 32 | 31 | 32B | 496B | 528B | 16B | 544B |

例如 K=6、某个远端 Server 只有三条 route，n=2，第一跳及其终端包的有效尾部均为 64B；
若按 C=5 预留，尾部原始长度为 112B，32B 对齐后占用 128B，包含 64B 有效字段、48B 未使用 entry 和 16B 对齐填充。
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
BS 不贡献单个 Token 的尾部长度。未使用容量/padding 的初始化值继续在 R03 确认，打包、检测、剥离和 T 的计算方式已确定。

**Memory 参考依据：** [Dispatch Memory](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h) 的 `SetTilingDataAndCal`
先将数据字节跨度按 `UB_ALIGN=32` 对齐，以 tokenQuantAlign_ 表示 int32 单位的尾部起点，再追加旧 32B 元数据段。
量化路径的数据跨度为 `ceil(hOutSize_/256)*256+scaleOutBytes_`，覆盖量化值的格式内对齐及其后的 scale；
`TokenToExpertInQuant` 先执行 `QuantMxfp8`，再用 `FillTriple` 追加元数据。非量化路径在拷入 UB 后追加。
去重方案复用“按处理后数据跨度定位尾部”的原则，将旧元数据段替换为独立 32B 对齐的新补充信息段。
这是当前源码的布局与执行顺序核对，不表示去重版量化范围或新打包路径已经实现并验证；数值支持范围仍归 R08。

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
发送 UB 只准备一次：将 forwardCount 设为 -1，填入公共返回引用 relayRank/forwardTokenIdx，
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

公共发送 UB 从首笔发送使用起保持只读，直到全部相关任务下发且最后一笔引用它的 MTE3 完成。
某个目标完成、待处理表剔除或 PIPE_MTE3 排序，都不单独证明 UB 已可由 Scalar/Vector 覆盖。
尚未下发完的 entry 和已分配编号必须持续有效，恢复处理时不重复发送或取号。

## 分核与监听

Cnt 接收/cumsum、LocalWindowCopy 和转发由同一组核负责，不单独分出专职转发核。
非全核发送 cnt 时，该组核还发送本 Rank 作为原始 source 的 cnt；全核发送 cnt 时由全核参与该阶段。
当前 Memory 以 `BS<48` 启用全核 cnt，否则由 cumsum 核组发送 cnt；该选择及 cnt 核组计算沿用 Memory。

执行顺序为：完成本卡 cumsum，确认结果对监听核组可见，再按实际 Combine 输入 Token 数 `tokenNum` 划分任务。
不再在 cumsum 完成前扫描预留 Token 区。各 Rank 不必同时进入监听阶段，
源端数据发送已确认按 Token 分核、先第一跳后 DIRECT，并取消旧双 Group 轮次。
两阶段仅为各发送核独立的任务下发顺序，切换时不增加屏障或 MTE3 等待；UB/Event 资源安排待 R05。

对于 C 个监听核，可以用连续逻辑区间表达均分：

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
| tokenPos | 128 个 uint32 位置记录，由稳定下标 j 访问；编码单位待 R05 确定，不存截断的 GM 指针 |
| pendingIdx | 128 项，初始为 0..127，保存尚未完成所有任务下发的稳定下标 |
| pendingCount | 初始 128，仅此前缀有效；变为 0 表示本核全部任务已下发 |

若 pendingIdx 也使用 uint32，两张表共 1024B，不包含 Flag scratch、发送 UB 和在途状态。
一般配置中每核表长由实际 `tokenNum` 决定，不能将本例 128 或 Combine 的 8*BS group 上限当作所有场景的表长上限。

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
对有后续转发的原始 Token，由唯一第一跳 owner 在首次转发前分配一次编号，本卡与跨卡转发使用同一计数器：

```text
forwardTokenIdx = atomic_fetch_add(relayListenTokenCount, 1)
```

这里描述“原子加一并获得本次旧值”的语义，不是已核对的目标 CANN API。
成功轮次编号从 0 开始，各目标卡和 expert 共用；它是分配顺序，不是网络到达顺序，也不是对外 group 计数。
全卡共用 `[0,8*BS)` 的编号容量，不按原始 source 或 Combine 返回发送卡分别取号；不同 group 的 t 唯一，同组全部副本共用 t。
同卡额外 route 的监听核、部分转发恢复均不再次取号。

分配编号不表示持久表项已就绪，也不表示发送完成。计数器初始化、可见性、复用及准确返回语义在 R05/R11 核对。
仅有本卡转发的 group 同样正常取号并记录信息；不为其另设索引域。

### 元数据与所有权

Dispatch/Combine 只保留三类跨阶段 metadata：`assistEx`、`sourceMask` 和 `group`。
旧讨论中的 `forwardPlan` 不是额外 tensor；它的必要内容拆入 `assistEx` 和 `group`，其余 route/entry 只在发送过程中作为临时信息使用。

| 逻辑数据 | 生成者与消费者 | 必须保留的含义 |
| --- | --- | --- |
| 1. `assistEx` | 当前 expert 行的 Dispatch 接收 owner 生成，Combine expert 发送方消费 | 原始 srcRank/srcTokenId、匹配的 originalK、DIRECT 或 relay/forwardTokenIdx 引用，以及该 route 的 weight |
| 2. `sourceMask` | 原始 source 的数据发送核在 UB 生成，核末一次写出到 sourceMask 输出 GM；source Combine 消费 | 每个原始 Token 独立的 32bit source slot 集合，一个 Token 仅由一个发送核写入 |
| 3. relay group / 中转监听记录 | 第一跳 owner 生成固定容量、行内密排记录，relay Combine 消费 | 每组保存 firstHopRow 和最多 K-1 条 `(recvSlot,weight)`，实际前 n 条有效；通过本卡槽位监听返回，不重复保存 senderRank/originalK |

第一跳 owner 通过一次 AtomicAdd 分配 `forwardTokenIdx`，随转发副本携带并写入既有字段，关联同一 group 的第一跳、成员和返回槽位。
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
   它是该 owner 当前处理的 Dispatch expert 输出行，不是 srcTokenId、q 或 forwardTokenIdx。
   字段转换对 MTE3 可见后才下发写出，metaUB 保持只读到对应写出完成；按正常 UB/Event 依赖复用，不增加逐 Token PIPE_ALL。

| 接收角色 | metaUB 写出前的转换 | 与公共发送副本的关系 |
| --- | --- | --- |
| DIRECT | 八字段原样保留；计数为 0，返回引用为 `(-1,-1)` | 无须重排或查找 entry |
| 第一跳 | 保留头部 k/weight、正数 forwardCount=n 和 forwardEntryCount=n；首次转发前取号一次，补入本 Rank/t（offset 24/28） | 同一个 t 也填入发送 UB；仅发送 UB 的 forwardCount 改为 -1，metaUB 保留正数 n |
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
UB 有效空间为 `本核 Token 数*4B`；源端分核区间与具体批量拷贝/尾部处理在 R05 落实，写出不得因向上取整到 32B 而覆盖其他核的元素。
无源端 Token 任务的核不写 mask；有任务的核保持 mask UB 到写出完成，Dispatch 对外完成覆盖其 GM 写入。
该 mask 仅说明 source 最终读取哪些 k 槽位，不表示 payload 已到达，也不单独编码 DIRECT/已加权 partial 类型。

### Relay Group 成员表

已确认每个 group 固定预留 K-1 个 member，实际前 n 项密排，`n=assistEx[firstHopRow[t]].forwardEntryCount`。
第一跳通过 firstHopRow 关联本卡 expert 输出和 assistEx，不占 member 槽位；本卡额外转发与跨卡转发均各占一项。
Group 按 `forwardTokenIdx=t` 索引，预留 8*BS 行。Combine 只消费由有效第一跳 `assistEx` 关联到的 group 行，
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
| Source 最终归约 | 只消费 sourceMask 指定的结果位置；partial 直接累加，DIRECT 乘对应 weight 后累加，被 partial 代表的其他 k 不再单独等待 |

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
转发终端返回未加权 expert 输出。relay 完成加权归约后返回一份 partial，source 直接累加，不再乘 returnK 的 weight。
DIRECT 输出仍由 source 乘各自 weight 后累加；源 Server 内逐 route DIRECT 的范围保持 R01 约定。
对同一个原始 Token，逻辑计算为：

```text
partial[g] = sum(k in group[g], weight[k] * expertOut[k])
output = sum(g, partial[g]) + sum(k in DIRECT, weight[k] * expertOut[k])
```

该式描述贡献关系，执行仍遵循前述 k/slot 顺序。
第一跳权重从其本卡 assistEx 取得；其他转发成员的 weight 在 Dispatch 到达 relay 时已随 forwardEntries 提供，
须持久保留为 relay Combine 的本地输入，不直接读取其他卡的 assistEx，也不在 Combine 另发一轮 weight。
已确认每条 member 保存 `(recvSlot,weight)`，Dispatch 按 `t*K+originalK` 完成位置换算后密排保存，GM 写入随算子完成；返回数据格式及完整区域预算继续在 R08-03/R07-01 收口。
头部与 assistEx 的 weight 均按 FP32 保存；Dispatch 后能否修改、无 weight 语义及各计算/通信阶段 dtype 尚待 R08。
sourceMask 本身不能区分 DIRECT 和已加权 partial；source 的结果类型编码及 DIRECT weight 的持久来源在 R04/R08/R10 确定。
两级归约改变浮点求和括号，不承诺与普通 Memory 位级一致。

Combine 的 expert 发送、group 监听/归约、partial 发送及 source 归约如何分核、按 H 分块和并行推进，
仍属 R09，不能直接将 Dispatch 的已确认核组设计当作 Combine 调度方案。

## 容量与生命周期

### 中转结果区与本卡返回

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

按当前非量化 Memory 结果通信格式估算：

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

若同时保留 P 份独立中转结果 buffer，relayResultBytes 乘 P；两份为 360MiB，是否同时保留由生命周期决定。
不额外乘发送卡数、总 Rank 数、expert 数或核数。group 表的 `8*BS` 行容量随之扩大，每行布局不变；
assistEx 仍按 tokenNum 行、sourceMask 仍按 BS 个原始输入 Token 保存。
上述 180MiB 仅为给定 dtype/stride 的中转结果区估算，不含 source 最终结果区、Dispatch 接收区或其他 metadata/scratch。
最终 dtype/scale/返回字段改变时须重新计算 stride；R07-01 的完整 workspace 预算仍待确认，该示例不表示已完成硬件分配或验证。

### 其他区域与超限

Dispatch 数据区仍按 `R*L*BS` 个槽位、各 T 字节预留；中转 group 的安全上限不能缩小其原始 source/expert 分区。
这一乘积不含 cnt、状态、同步区或额外 workspace。
实际 Combine 输入 Token 数上界为 `tokenNum<=R*BS*min(K,L)`，assistEx 分配容量须覆盖该上界，有效行数及行序沿用已确认的 Memory 契约。

源端结果区、assistEx、group、原子计数器、Flag scratch、发送 UB 和累加暂存另行预算。
不同区域的 dtype/stride 与同时存活关系在 R07/R09 确定；
只有确认生命周期不重叠后才能复用，不能因沿用 Memory 逻辑就认定已有 window 足够。

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

第一跳含 hidden 和下列表格中的尾部。固定头部采用前述[已确认字段表](#尾部字段字节表)，entry 仍按当前 16B 候选计算，
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
该 partial 返回 R0/T8/k0；R0 直接累加，不再乘 w0，也不再等待本组 k1..k5 的独立结果。

按已确认的 32B 头部，加上当前候选的五条 16B entry，
有效逻辑尾部为 112B，按已确认规则向上对齐到 32B 后占用 128B，其中最后 16B 是对齐填充。
非量化 FP16/BF16 数据段占 14336B，追加后 UB Token 数据总长为 14464B；该长度尚未计入通信分块和 Flag，不作为最终 T。
示例在非量化 hidden 相同的前提下省去跨 Server 的五份 hidden 第一跳，
仍保留五笔 Server 内逐 expert 写入；它不证明端到端性能收益。

## 验证边界

本文完成文档归并、逻辑示例及容量核对，没有实现或测试去重 kernel。
确认协议必须区分三种证据：路由/地址的参考计算、目标 CANN API 的编译核对、实际设备的可见性/前进性验证。
计数完成不代表数据到达，原子取号不代表 GM 写入完成，任务下发不代表 UB 或通信窗口可以复用；全部输出写入由 Dispatch 算子完成语义保证。

后续验证要求见 R12：使用可区分各 expert 的结果，检查贡献守恒、原始 k/weight、返回地址、
零接收、同卡多 expert、原子并发、部分发送恢复、安全范围内的 group 数量与跨轮生命周期。
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
