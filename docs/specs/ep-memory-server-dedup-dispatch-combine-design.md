# EP Memory Server Dedup Dispatch / Combine 联合设计

## 1. 状态与文档职责

2026-09-18 建立初版；2026-09-19 确认五表与 Combine 公共接口，内部模块仍待详细设计。代码路径为 `/users/hzy/workspace/TileXR`，
分支为 `codex/SilentEmotion`，本次现状核对基线为 `e2d1704`。
Dispatch 已实现，Server Dedup Combine 尚未实现；本文不新增设备验证结论。

本文集中维护两个算子之间的共同契约：

1. Dispatch 产生哪些持久信息，Combine 如何据此发送、主动读取、转发和聚合。
2. 两者共用同一个 EP 通信域时，窗口内部如何划分、寻址和核算容量。

[Dispatch 主规格](ep-memory-server-dedup.md)继续描述当前实现；
[Dispatch 输出适配设计](ep-memory-server-dedup-dispatch-output-adaptation-design.md)维护五表生产、
新包尾布局及逐项确认状态；
[Combine 独立设计](ep-memory-server-dedup-combine-design.md)集中维护核数、init、阶段、同步、流水和模块实现。
本文 §4/§5 仅描述信息交接和已确认的五表/接口契约，§3 是实际源码现状，§7 跟踪共同契约状态。
接口已确认不代表已写入公开头文件；历史 R/C 文档中的 push-member 地址及旧表不覆盖新 pull 方案。

## 2. 设计边界

### 2.1 已确认的前提

- Dispatch / Combine 共用同一个 EP peer-memory 通信域，沿用现有 magic 选窗方式。
- 选窗的主要目的为隔离 Dispatch 与 Combine；不要求两者各自另做跨轮双缓冲。
- 用户已确认模型在 Dispatch 与 Combine 之间、Combine 与下一次 Dispatch 之间安排其他算子，
  其实际执行顺序保证 buffer 不踩踏。本文直接采用该调度保证，不增加轮次管理、plan/epoch/windowId
  管理或 Server 内读完确认握手，不再把这些作为待讨论项。
- 沿用当前每 Server 8 卡的分组；Combine 内部如何分核、准备、通知和聚合见独立设计。

### 2.2 本轮需要设计的内容

- 行角色、贡献身份、group 成员位置、source mask/weight 的生产和消费契约。
- 持久 metadata 的字段、容量、有效范围及 expertOut 行序约束。
- Dispatch 收包、Combine 本卡中转、source 返回结果及各自控制区的布局。
- 跨算子的存储归属、有效数据范围和对外完成/生命周期契约。
- 格式和布局变更涉及的 Host、kernel、调用方分配及独立参考验证。

### 2.3 不在本轮展开的内容

不重新讨论模型跨轮调度，不扩展并发 plan、多 stream、重复 Combine 或失败重试协议。
不在本文维护 Dispatch/Combine 内部流水、Combine 核数、阶段、UB lane、H tile 或 Event。
不新增 UDMA 路径。具体 API 实现与性能结论留到方案明确后的开发和设备验证。

## 3. 当前已有的信息与空间

### 3.1 符号

R 为 rank 数，E 为 expert 总数，L=E/R，BS 为每个 source 的 Token 数，K 为 top-k。
当前每卡 Dispatch 输出容量 `Crows=R*BS*min(K,L)`，有效行数为 `tokenNum`。
`row` 是本卡输出行；第一跳的 `t=row`；`k` 是原始 top-k 位置。
`source` 指原始 Token 所在卡，`member` 指 expert 贡献所在卡，`relay` 指 group 聚合卡。
同一卡可同时承担这些身份。TERMINAL 指经 Dispatch 转发到达的 expert 行，不表示 Combine 最终输出。

### 3.2 当前 Dispatch 持久输出

这些 buffer 由调用方单独分配，不属于当前 EP 通信窗口；调用方保持 metadata 至对应 Combine 完成。

| 输出 | 当前格式与有效范围 | Combine 用途 |
| --- | --- | --- |
| sendCountsOut | int32[E] inclusive prefix，末项为 tokenNum | 遍历有效 expert 行 |
| assistEx | 32B/行，前 tokenNum 行有效 | 判断角色、原始 source/token/k、真实 weight 和 relay 引用 |
| group | 每行 align32(4+8*(K−1))B，容量 Crows 行 | 本地第一跳行及前 n 个有效 member |
| sourceMask | uint32[BS] | 指定 source 需要接收的 k 槽 |
| expertScalesOut | FP32[BS,K]，仅 mask 置位处消费 | DIRECT 真实 weight；group 的 returnK 权重为 1 |
| expandXOut / dynamicScalesOut | hidden 与可选 MXFP8 scale，前 tokenNum 行有效 | expert 输入；不直接决定 Combine dtype |
| expertTokenNumsOut | 本卡 expert 分段数量或累计数量 | expert 计算的分段信息 |

assistEx 八个 4B 字段依次为：srcRank、srcTokenId、k、FP32 weight、forwardCount、
forwardEntryCount、relayRank、forwardTokenIdx。forwardCount=0 为 DIRECT，>0 为第一跳，-1 为 TERMINAL。
第一跳与 TERMINAL 共享 relayRank/t；不同 relay 的 t 可以相同。

group 当前保存 `firstHopRow=t`，随后为 `(recvSlot=t*K+originalK, FP32 weight)`。
仅第一跳引用的 group 行有效，仅前 n 个 member 有效；第一跳不在 member 列表中。
第一跳是 group 最小 k，亦为 returnK；成员按其余原始 k 升序保存。
当前 group **没有 member rank、expertId 或 q**，不能直接提供 pull 所需的 peer 地址。
新五表替换方案见 §5；本节旧字段均指未修改的实际代码。

Dispatch 包尾已有 32B header 和每项 16B `(expertId, q, originalK, weight)`。
包尾随 Dispatch 数据传输，持久 group 是其处理后的输出，两者不能混用。
expertId/q 目前未保留在 group；若采用它们定位，应在 Dispatch 时持久化，不能依赖 Combine
重新扫描已经消费、清 Flag 或可能被覆盖的 Dispatch 通信包。

### 3.3 当前 EP 通信布局

每卡按 8 GiB peer memory、两个 4 GiB 窗口布局：
`windowOffset=(uint64(magic)%2)*4GiB`。每窗尾部保留 4 MiB；runtime Flag 实际位于整个域末尾。

| 单窗区域 | 当前容量 / 用途 |
| --- | --- |
| 控制区 | 前 2 MiB；Dispatch count、sum、prefix、ready，以及历史留白 |
| source 返回预留 | BS*K*U，Dispatch 不访问 |
| Dispatch 收包区 | E*BS*T，存放 DataAsFlag 包 |
| relay 历史预留 | 8*BS*K*U，Dispatch 不访问 |
| 尾部保留 | 4 MiB，不供 EP payload 使用 |

T 为实际 Dispatch wire stride；当前 U=ceil(2*H/480)*512，仅为历史返回预算。
它不代表新 Combine 已确定为 BF16/FP16 或必须沿用 DataAsFlag。
当前 Host 会将历史预留也计入窗口上限；重做布局需同步 Host，不能只修改 kernel 地址。
当前 group 的稀疏 t 不受 8*BS 上限约束，因此历史 relay 预留不能直接作为新 stage 区。

源码依据：[数据布局](../../src/ep/common/ep_memory_server_dedup_data_layout.h)、
[控制与选窗](../../src/ep/common/ep_memory_server_dedup_count_layout.h)、
[metadata 写出](../../src/ep/kernels/tilexr_ep_server_dedup_receive_packet.h)、
[Host 容量核算](../../src/ep/host/ep_memory_server_dedup_layout.cpp)。

## 4. 跨算子信息交接

本节说明 Dispatch 产生的信息如何满足 Combine 的逻辑收发与聚合需求，不安排执行阶段或核分工。
expertOut 与 Dispatch 输出行序一致；五表的语义、寻址及加权关系见下文。
具体执行顺序与同步见[Combine 独立设计](ep-memory-server-dedup-combine-design.md)。

### 4.1 按本卡操作定义的六类 Dispatch Token

沿用用户命名；这里的 Token 是一次发送/接收操作或一条 expert 贡献，不是对原始输入 Token
做六个互斥集合划分。同一个原始 Token 可以同时包含 DIRECT 和多个 Server group。

```text
原始 source S                    relay A                         member B
s_direct_token  ───────────────→ 某目标卡的 r_direct_token
s_wait_relay_token ────────────→ r_wait_relay_token
                                  s_relay_token ───────────────→ r_relay_token
```

DIRECT 目标、relay 和 member 可在不同卡；同 Server 内 relay 与 member 也可能是同一卡。
`r_wait_relay_token` 同时对应 relay 本地的一条 expert 贡献和一个待展开 group，不能仅视为中转包。

Combine 使用这四类操作描述：

- 发送 r_direct_token：把 Dispatch 收到的 DIRECT 所对应 expertOut 发回 source。
- 发送 r_wait_relay_token：把第一跳本地贡献与成员贡献聚合后，发送一个 partial。
- 主动读取 r_relay_token：**读取者为 relay，数据位于 member 卡**。从 relay 自身的 Dispatch
  视角，读取清单对应它发出的 s_relay_token；从数据所在 member 卡看，是该卡的 r_relay_token。
- 最终接收 direct_token + wait_relay_token：从原始 source 本卡视角，分别对应此前的
  s_direct_token 与 s_wait_relay_token；最终按 sourceMask 归并到原始输入 Token。

这四类不包含“生产卡准备 stage”的本地搬运，后者必须单独保留，才能闭合主动读取路径。

### 4.2 统一的地址符号

以下 Dispatch 地址是当前实现；Combine 已确定沿用相同逻辑槽号，通过 expert/q 定位，
Sstage/Sresult 及窗口内部 offset 尚待详细设计。
所有乘积用 uint64；WD/WC 为两算子按现有规则取得的窗口偏移，不新增选窗机制。

```text
s = 原始 source rank，p = 原始 TokenId，k = 原始 top-k 位置
r = relay rank，m = member/destination rank，e = destination expertId
q = 原始 source 上，在该 route 之前发往同一 expert 的贡献条数
L = E/R，localExpert = e%L

Dslot(s,m,e,q) = (((s+m)%R)*L + e%L)*BS + q
Daddr(s,m,e,q) = peer[m] + WD + dispatchOffset + Dslot(s,m,e,q)*T

Cresult(s,p,k) = peer[s] + WC + resultOffset + (p*K+k)*Sresult

Cstage(m,s,e,q) = peer[m] + WC + stageOffset + Dslot(s,m,e,q)*Sstage
```

Dispatch 的 q 按原始 expertIds 的 token/k 顺序计算，不随 DIRECT 延后发送或 relay 转发重排。
转发的 Daddr 中 s 仍为原始 source，不能换成当前 relay。

接收侧从 count/cumsum 已知每个 `(localExpert,source)` 段的数量：
`row=prefixBefore(localExpert,source)+q`，据此枚举本卡 Daddr；它无需先读包尾才能知道探测位置。
三类接收共用此过程，到达后才通过包尾 forwardCount 区分角色。
输出 hidden 地址按 row 和输出行字节数计算，assistEx 地址为 `assistExBase+row*32`；
物理 Dslot、输出 row、原始 p 不是同一个编号。

### 4.3 当前 Dispatch 发送三类：地址来源与记录

| 本卡操作 | payload 来源与发送地址依据 | 随包信息 | 本卡持久记录 |
| --- | --- | --- | --- |
| s_direct_token | 从 x[p] 取 hidden；expertIds[p,k] 得到 e/m；原始 route 前缀得到 q；写 Daddr(s,m,e,q) | header 保存 s/p/k/真实 weight，forwardCount=0、entry 数=0，relay/t=-1 | sourceMask[p] 置 k；expertScalesOut[p,k] 保存真实 weight |
| s_wait_relay_token | 同一远端 Server 的多条 route 合为 group，最小 k 为 returnK；该 route 的 e/r/q 确定首跳 Daddr(s,r,e,q)；hidden 来自 x[p] | header 保存 s/p/returnK/第一跳真实 weight、n；entries 保存其余每条 e/q/k/真实 weight；发送时 relay/t 尚为 -1 | sourceMask[p] 仅置 returnK；对应 expertScalesOut=1，不等待 group 其他 k |
| s_relay_token | relay 使用刚收到的第一跳 wire buffer；每个 entry 的 e/q 得到 m 和 Daddr(s,m,e,q) | 同一完整包的 role 改为 -1、relayRank=r、t=本地第一跳 row；保留原始 s/p、第一跳 header k/weight 和全部 entries | 由相应 r_wait_relay_token 的接收处理写 assistEx/group；当前不另建发送列表，不修改本卡 sourceMask |

第三类发生在 Dispatch 接收核中。终端 packet 的 header k/weight 仍属于第一跳；终端自己的
k/weight 必须按目标 expert 匹配 entry，不能直接照抄 header 的这两个字段。
本地 route 当前全为 DIRECT；远端 Server 仅有一个 route 时也为 DIRECT。上述按去重开启路径描述。

### 4.4 当前 Dispatch 接收三类及目标表映射

| 本卡操作 | 输入地址与识别 | 接收后的动作 | 持久记录及新 pull 缺口 |
| --- | --- | --- | --- |
| r_direct_token | 从 count/prefix 枚举本卡 Daddr；到达后 header role=0 | hidden/scale 写本卡 row | assistEx[row] 保存 s/p/k/真实 weight、DIRECT 角色；足以在 Combine 计算 Cresult，无 group |
| r_wait_relay_token | 同样枚举 Daddr；header role=n>0 | hidden/scale 写本卡 row；令 t=row，展开产生 n 个 s_relay_token | assistEx 保存原始返回身份、第一跳 weight、n、r/t；group 保存 firstHopRow 与 n 个 recvSlot/weight。新 pull 还需持久化 member 定位信息 |
| r_relay_token | 同样枚举 Daddr；header role=-1；本卡 slot 可得目标 expert | 按目标 expert 查找 entry，取该成员 k/weight；hidden/scale 写本卡 row | 当前 assistEx 保存 s/p/该成员 k/weight、继承的 r/t；目标表 1 写 3，表 2 写当前 slotIndex，不再保存 relay/t |

r_wait_relay_token 接收时，entries 中的 e/q/k/weight 全部可用，应在此处形成后续 pull 的成员表。
当前 BuildServerDedupGroup 只保存 `(t*K+k,weight)`，因此丢失了 member rank 和 e/q。
目标类型 1/2 分别写表 1 的 1/2，表 2 均保存 header 的 s/p/k；类型 2 另按 §5.0 写表 3。
原始 source 的表 4/5 在 source 发送阶段产生，不由这里的三类接收重新生成。

### 4.5 Combine 消费信息的对应关系

**类型 3 的目的地址含义：** 对 row<TokenNum 且表 1 为类型 3 的行，
把未加权 expertOut[row] 写入表 2 offset 指定的本卡 stage 槽；包括 relay 同卡的 member。
类型 2 的本地第一跳直接从 expertOut[row] 取值，不再重复放入 stage。

| Combine 操作 | 信息来源 | 逻辑目的 |
| --- | --- | --- |
| 发送 r_direct_token | 类型 1、表 2、expertOut[row] | Cresult(srcRank,srcTokenId,srcTopk) |
| 聚合后发送 r_wait_relay_token | 类型 2、表 3 header、expertOut[row] 与 N 个 member | 表 2 指定的 Cresult(source,p,returnK) |
| 主动读取 r_relay_token | 表 2 的 source、表 3 的 expert/q | Cstage(member,source,expert,q) 指定的成员贡献 |
| 最终接收 direct_token + wait_relay_token | 表 4 指定的结果槽与表 5 权重 | 加权后的 yOut[p] |

表 4 表达接收义务，运行时到达判断由 Combine 内部协议负责；source 不等该 group 其余 member 的 k 槽。

### 4.6 信息产生位置与示例

1. 原始 source 的 Dispatch 发送产生表 4/5，确定最终收哪些槽、如何加权。
2. expert 卡的 Dispatch 接收产生表 1/2；relay 第一跳接收额外产生表 3 的 header 与有效成员。
3. Combine 准备只写 stage 数据及完成状态；不再发送路由表或 weight 表。

例如 source S 的 Token p：k=0 直发 D；k=2 为 A 上的第一跳，k=5 为同 Server 的 B 上成员：

```text
S：表 4 相关位 {0,2}，表 5 相关权重 {weight0,1}
D：表 1[rowD]=1，表 2[rowD]=[S,p,0]
A：表 1[rowA]=2，表 2[rowA]=[S,p,2]
   表 3[rowA][0]={N=1,selfWeight=weight2}
   表 3[rowA][1]={srcExp=expertB,q=qB,weight=weight5}
B：表 1[rowB]=3，表 2[rowB]=[Dslot(S,B,expertB,qB),-1,-1]
Combine：B 暂存 expertOut[rowB]；A 在 ready 后读取该槽
         A 计算 weight2*expertOut[rowA]+weight5*expertOut[rowB] → S 的 (p,2)
         D 发送未加权 expertOut[rowD] → S 的 (p,0)
         S 读取两个结果，分别乘 weight0 和 1，再求和
```

A 的读取清单来自 A 的类型 2 行；不是根据 A 的类型 3 行去寻找其他卡的数据。
A=B 时同样使用类型 3 的 stage 槽；第一跳自身只累计一次。

## 5. 已确认的五表与公共接口

### 5.0 五表契约（2026-09-19）

用户将 Dispatch/Combine 的信息交接整理为五张表，五表的用途与逻辑 shape 均已定义。
新五表契约优先于此前沿用 assistEx/group 的组织建议；当前代码仍使用旧 ABI。

| 表 | 逻辑 shape | 索引身份 | Combine 消费方式 |
| --- | --- | --- | --- |
| 1 TokenType | MaxTokenNum * uint32 | 本卡 Dispatch 输出 row | 判断直返、读取聚合或暂存 |
| 2 DestinationIndex | MaxTokenNum * 3 项 | 同一 row | 类型 1/2 返回 source；类型 3 写本卡 stage |
| 3 RelayReadIndex | MaxTokenNum * K * 8B | 同一 row，仅类型 2 | 定位并加权读取 group 成员 |
| 4 SourceMask | BS * uint32 | 本卡原始输入 TokenId | 枚举最终应接收的 k 槽 |
| 5 SourceWeight | BS * K * FP32 | 本卡原始输入 TokenId、k | 对最终有效返回槽加权归约 |

表 1–3 属于 expert 接收侧行空间；表 4–5 属于原始 source 输入 Token 空间。
同卡两种行数和编号可不同，不能用 row 直接索引表 4–5。

#### 表 1：TokenType

MaxTokenNum 为本卡 Dispatch 最大接收行数，对应当前 Crows；TokenNum 为实际接收行数，
对应当前 sendCountsOut[E−1]。表按 Dispatch 输出 row 索引，与 expandXOut、恢复行序后的
expertOut 对齐，不按原始 source TokenId、通信 slot 或到达先后编号。

```text
TokenType: uint32[MaxTokenNum]
有效范围: row ∈ [0, TokenNum)
```

| 值 | Dispatch 接收类型 | 对应 Combine 动作 | 当前代码的识别来源 |
| --- | --- | --- | --- |
| 1 | r_direct_token | 本地 expert 结果直接返回原始 source | forwardCount=0 |
| 2 | r_wait_relay_token | 读取 Server 内成员贡献，与本地第一跳聚合后返回原始 source | forwardCount>0 |
| 3 | r_relay_token | 将本地 expert 结果写入本卡中转区，供 Server 内 relay 读取 | forwardCount=-1 |

Dispatch 接收核在解析包尾角色并确定 row 后写 TokenType[row]；各 row 唯一 writer。
新 Dispatch wire 的 32B PacketHeader.type 与本表共用同一 uint32 枚举，值和语义均为
1=DIRECT/r_direct_token、2=第一跳/r_wait_relay_token、3=转发终端/r_relay_token。
接收端直接保存包头 type，不做另一套类型映射。上表 forwardCount 仅描述旧代码识别来源。
源端填写 1 或 2；第一跳先保留本地表 1 的类型 2，最终转发前仅将包头 type 改为 3，
随后开始转发；终端输出类型 3。包头其余字段及后续 8*K 索引段均保持不变。
本地类型记录不能受 wire 改写影响，最终必须为 type_2；成员数 N 不承担类型编码。
Combine 根据三种类型分别暂存、直返、读取并聚合；执行分工在独立设计维护。
最终 source 输出按原始输入 Token 组织，不通过这张接收行类型表决定等待哪些返回槽。

类型 2 包含第一跳本地 expert 贡献，不能只聚合其他成员；成员还可能位于 relay 同卡的其他
expert，这些同卡成员也按类型 3 暂存，再由类型 2 owner 读取。类型 3 不负责直接向原始 source 返回结果。

容量尾部 [TokenNum,MaxTokenNum) 不消费，无需定义类型或额外清零；本版不新增类型 0。
TokenType 只提供三类枚举，不承载原 forwardCount 中的成员数量，成员数/列表有效性需由其余
表或公共参数契约承接；原始返回地址、成员地址、weight 也不由本表推导。

在 128P、384 experts、BS256、K16 的示例中，MaxTokenNum=98304，表 1 占 393216B=384KiB。
本节为目标契约；新 Combine 使用五表，当前代码中的旧 assistEx/group 尚未迁移。

#### 表 2：目的地址索引表

用户定义共用表 `DestinationIndex[MaxTokenNum][3]`，与表 1 使用相同 Dispatch 输出 row，
只消费前 TokenNum 行。三项的解释由 TokenType[row] 决定：

| TokenType | 三项内容 | Combine 操作 |
| --- | --- | --- |
| 1 | [srcRank, srcTokenId, srcTopk] | 将本行未加权 expert 结果发送到原始 source 的对应结果槽 |
| 2 | [srcRank, srcTokenId, srcTopk] | group 聚合完成后，将 partial 发送到原始 source 的对应结果槽 |
| 3 | [offset, -1, -1] | 将本行 expert 结果写入本卡中转区指定位置，供 relay 主动读取 |

类型 1 的 srcTopk 是该 DIRECT route 的原始 k；类型 2 的 srcTopk 是 group 的 returnK，
当前即第一跳的原始 k。两个类型使用同一返回地址规则，类型 2 不向其余 member k 逐项返回：

```text
resultSlot = uint64(srcTokenId)*K + srcTopk
address = peer[srcRank] + combineWindowOffset + resultOffset + resultSlot*Sresult
```

类型 3 的 offset 为 **uint32 中转 Token 槽号**，从本卡 stage 起点按 Token 槽计数，
不是字节偏移，也不是 relay A 的地址或原始 source 的结果槽：

```text
stageWriteAddress = peer[self] + combineWindowOffset + stageOffset
                  + uint64(offset)*Sstage
```

Combine 准备核从 expertOut[row] 读入，再写上述位置；offset=0 为合法首槽。
后两项 -1 为无效占位，不参与地址运算，类型判断以表 1 为准。若三项统一采用 uint32 存储，
无效占位的位型为 UINT32_MAX。接口已明确复用 int32_t* assistInfoForCombine，每行三个 32 位项；
类型 3 的第一项按 uint32 位型解码，后两项为 -1。
Dispatch 已确认入口使用 `uint32_t *destinationIndexOut`；调用方以 int32 指针视图将同一 GM
分配交给 Combine 的 assistInfoForCombine，不转换内容或另分配一张表。两端按同一 12B 行距和
32 位字段位型读写；有符号接口不改变类型 3 slot 的无符号含义。

Dispatch 写表时，类型 1/2 的三项取包尾 header 的原始 source/token/k；类型 3 的 offset
直接取当前接收逻辑 slot，即 `(((source+self)%R)*L+localExpert)*BS+q`。
A 按表 3 计算出的读取 slot 与 B 在表 2 保存的 offset 相同。
因此 Dispatch 不需要提前知道 Combine 的 payload dtype/字节 stride，Combine 用自身 Sstage 换算；
也不需要在 Combine 仅为暂存而重新从 prefix 恢复本行 e/q。

表 2 是每个接收行一项的目的索引，**不是此前每行最多 K 项的转发成员去向表**。
它解决“本行结果往哪里写”，类型 2 “需要读取哪些成员”由表 3 描述。

#### 表 3：转发 Token 读取索引表

用户定义 `RelayReadIndex[MaxTokenNum][K]`，每块 8B，总容量为 MaxTokenNum 行，
候选行范围为 `[0,TokenNum)`；此范围内依然稀疏，只有 TokenType[row]=2 的行包含有效信息。
其他类型整行 K*8B 均无效，Combine 不读；不要求无效内容为零。

类型 2 的第 0 块是 header，不是成员索引：

| 块内字节偏移 | 字段 | 类型 | 含义 |
| --- | --- | --- | --- |
| 0 | N | uint32 | 本地贡献之外，还需要读取并聚合的成员数，1≤N≤K−1 |
| 4 | selfWeight | FP32 | 当前第一跳本地 expertOut[row] 的真实 weight |

后续第 1..N 块为有效成员条目：

| 块内字节偏移 | 字段 | 类型 | 含义 |
| --- | --- | --- | --- |
| 0 | srcExp | uint16 | 读取结果所属的全局 expertId，来自 Dispatch entry.expertId |
| 2 | q | uint16 | 原始 source 发往该 expert 的段内序号 |
| 4 | weight | FP32 | 该成员在 relay 聚合时应用的权重 |

第 N+1..K−1 块无效，不读、不要求清零。总有效块数为 N+1，总贡献数也为 N+1；
N 不包含本地第一跳，不是有效块总数。K=1 时不存在类型 2 行。
每块地址为 `tableBase + (uint64(row)*K+i)*8`；第 0 块的前 4B 不得按两个 uint16 解析。

A 在 Dispatch 处理 r_wait_relay_token、row 已确定时，将源端生成的完整 8*K 索引段
原样保存到本表对应行；不在接收侧重新构造 header 或转换成员字段。
段内第 0 块已包含 N/selfWeight，第 1..N 块已包含成员 expertId/q/weight；
条目保持原始 k 的相对顺序，不持久化 originalK。selfWeight 不再属于独立 32B 包头。
这里 srcExp 指 Combine 数据所在 expert，不是原始 Token source rank。

独立包头保留 32B，四个有效 uint32 依次为 srcRank、srcTokenId、topK（原始 k/returnK）、
type，后 16B 为占位。包头与索引段分别从 32B 对齐位置开始；索引段若侵入某个 512B block
末尾的 32B Flag 区，则整体移至下一块的数据起点。Init 缓存两个相对 wire 包起点的偏移；
具体公式与生产路径见 [Dispatch 输出适配设计 D03](ep-memory-server-dedup-dispatch-output-adaptation-design.md#d03源端-senddata-的-token-包尾构造)。
此处描述已确认目标格式，旧 wire 的 16B entry 与接收侧拼表逻辑仍待代码迁移。

压缩字段要求 expertId≤65535、q≤65535；不重复 top-k 保证每个 source/expert 的 q<BS。
当前 Host 限制 E≤1024，Count UB 的四份 int32 route 缓冲受 190KiB 预算限制，使 BS*K<12160，
因而当前合法输入可无损使用 uint16。后续扩大输入范围时需同步校验，不能先截断再寻址。

Combine 仅对类型 2 行读取 header；本地贡献直接取 expertOut[row]，不为第 0 块计算 stage 地址。
对 i=1..N 联合使用表 2/表 3 计算读取地址；可读时机由 Combine 内部协议保证：

```text
s = DestinationIndex[row][0]       // 原始 source rank
e = RelayReadIndex[row][i].srcExp
q = RelayReadIndex[row][i].q
m = e/L
localExpert = e%L
slot = (((uint64(s)+m)%R)*L + localExpert)*BS + q
readAddress = peer[m] + combineWindowOffset + stageOffset + slot*Sstage

partial = selfWeight*expertOut[row]
for i in 1..N, 按表内顺序:
    partial += memberWeight[i]*readMember(i)
```

上述为逻辑加权关系，计算 dtype/cast 按 J05 收口。聚合完成后按表 2 的
srcRank/srcTokenId/srcTopk 发送 partial；最终 source 按表 5 的 1.0 消费。
成员卡类型 3 的表 2 offset 必须等于上述 slot，因此生产端写入与 relay 读取闭合。

MaxTokenNum=98304、K=16 时，每行 128B，表 3 共 12582912B=12MiB/卡。
第 0 块占用已有 K 块中的一块，不增加表行大小。当前格式含 weight；可选关闭时的计算语义
仍待 J05 明确；本版行 stride 固定为 K*8B，关闭加权也不隐式压缩字段。
零 selfWeight/memberWeight 都是合法权重，不能用于判断块是否有效；有效性只取决于 row 范围、
TokenType 和 header.N。此前“第 0 项也存 expert/q”的建议已被本定义替代。

#### 表 4：最终本卡 Token 接收 mask 表

用户定义 `SourceMask[BS]`，每项 uint32；TokenId=p 对应本卡 Dispatch 原始输入行。
bit k=1 表示 Combine 需要接收并消费本卡结果槽 `[p,k]`，bit k=0 不等待、不消费。
这里 k 是原始 top-k 位置，不是表 3 的密排条目下标。

表由原始 source 的 Dispatch 发送阶段生成：s_direct_token 对应 k 置位；每个
s_wait_relay_token group 只将其 returnK 置位，其他转发成员位不置位。
同一个 Token 的 DIRECT 与各 Server partial 共同构成最终 mask，逻辑 bit 数仍为 K。
K≤32，K=32 时 bit31 按无符号位处理。当前实现的 sourceMaskOut 已提供此语义。

#### 表 5：最终本卡 Token 加权表

用户定义 `SourceWeight[BS][K]`，每项 FP32；有效位置与表 4 的置位完全对应。

| 返回贡献 | mask 对应位 | 表 5 权重 |
| --- | --- | --- |
| DIRECT 的未加权 expert 结果 | 原始 k | 该 route 的真实 weight |
| relay 已完成局部加权求和的 partial | group 的 returnK | float 1.0 |
| 不单独返回的 member 或其他无效槽 | 0 | 无语义，Combine 不读取 |

表由原始 source 的 Dispatch 发送阶段生成；当前实现的 expertScalesOut 已提供此语义。
relay 的 partial 已包含第一跳和有效成员的权重，source 不再次应用第一跳真实 weight。
表 3 的 weight 属于 relay 内部成员加权，表 5 的 weight 属于最终 source 加权，作用位置不同。
合法 weight 为 0 的 DIRECT 仍按 mask 接收；不能用权重是否为 0 判断结果是否需要到达。

Combine 以表 4 枚举 k，按表 5 加权并写本地输出：

```text
output[p] = sum over k where SourceMask[p] bit k == 1:
                result[p,k] * SourceWeight[p,k]
```

对有效 k 的消费沿用 k 升序；原始贡献在 relay 或 source 恰好加权一次。
表 4 是接收义务 mask，不是运行时已到达 bitmap；到达判断仍由结果区发布协议完成。
无效 weight 槽不要求清零。weight 可选模式下是否省略表 5 及其默认语义，仍在 J05/J07 收口。

BS=256、K=16 时，表 4 占 1024B=1KiB，表 5 占 16384B=16KiB。
沿用 MaxTokenNum=98304 的例子，表 2 复用三字段 12B 格式，占 1179648B；
五表逻辑容量共 14173184B；分配起始地址/尾部可对齐，表 2 的行 stride 为 12B、表 3 为 K*8B，
不能为方便搬运而单独将每行取整到 32B。实现需要处理跨行/尾部搬运，不能覆盖相邻 owner 的行。

#### 五表信息完备性核对（2026-09-19）

**设计层面结论：在已约定的行序、专家映射、公共布局和模型调度前提下，五表足以描述
Server Dedup Combine 的全部路由与加权关系，不需要第六张路由表、group 数表或原始 expertIds。**
这不等于算子只接收五个指针：expertOut、TokenNum、输出空间、通信句柄和 shape/dtype 等
基础参数仍然必需。当前未实现新 Combine，以下是信息依赖闭合核对，不是设备执行验证。

| Combine 职责 | 信息来自哪里 | 闭合结果 |
| --- | --- | --- |
| 确定有效行及分工 | TokenNum + 表 1 | 遍历 row<TokenNum，类型 1/2/3 各有明确职责 |
| 类型 3 本卡暂存 | expertOut[row] + 表 2 uint32 offset + 公共 stage stride | 写本卡 stage，对外只需发布准备完成 |
| 类型 1 DIRECT 返回 | expertOut[row] + 表 2 source/token/k | 写目标 source 唯一结果槽，不需要本地路由 weight |
| 类型 2 本地贡献 | expertOut[row] + 表 3 第 0 块 selfWeight | 不再需要旧 firstHopRow 或 assistEx.weight |
| 类型 2 有效成员 | 表 3 第 0 块 N | 仅读取第 1..N 块，不需要额外成员数或有效位表 |
| 类型 2 远端/同卡成员读取 | 表 2 source rank + 表 3 expert/q + 公共 R/L/BS/布局 | 得到 member rank 和 slot，与类型 3 的 offset 相等 |
| 类型 2 成员加权 | 表 3 各成员 weight 与表内顺序 | 第一跳后按成员顺序聚合，无需 originalK 字段 |
| 类型 2 partial 返回 | 表 2 source/token/returnK | 返回唯一槽，无需 relayRank/t 或重新查原始路由 |
| 本卡最终接收 | BS/K + 表 4 | 每个 p 的 mask 给出应等待的 k，无需每 source 接收计数 |
| 本卡最终归约 | 表 5 + 本卡结果槽 payload | DIRECT 真实权重、partial 权重 1，写 yOut[p] |

地址一致性可直接核对：B 的 Dispatch 接收 slot 为
`(((source+B)%R)*L+localExpert)*BS+q`，被写入 B 表 2 offset；
A 表 3 的 expertId 满足 expertId/L=B、expertId%L=localExpert，再结合 A 表 2 的原始 source，
计算得到同一个 slot。该映射区分不同 source、expert 和 q，不依赖 relay 的 t 或原始 TokenId。
同卡 member 也使用此公式，以本卡 peer 基址读取；无需新增来源类型字段。

返回端每个合法 DIRECT route 独占 `[source,p,k]`，每个 group 用其 returnK 独占一个槽，
source mask 不再等待该 group 的其他 k，因此不会重复消费同一贡献。
第一跳自身 weight 已在表 3 header，成员 weight 已在有效 entries，最终权重已在表 5，
不存在需要回读旧 assistEx 或原始权重矩阵才能补齐的加权信息。

Combine 仍需的基础接口信息：

| 信息 | 必要性与建议来源 |
| --- | --- |
| expertOut | 实际 expert 计算结果；与表 1–3 的 row 对齐，不能用 Dispatch hidden 替代 |
| TokenNum | 本卡实际有效行数，由保留的 sendCounts 数组末项在设备侧读取 |
| BS、K、H、R、E/L | 遍历与 expert 映射所需；R/rank 可从 comm 取得，L 可由 E/R 推导，不必重复传所有值 |
| dtype、stride、布局配置 | expertOut/中转/返回/累加/输出格式、区域偏移，由公共配置及 Host tiling 统一确定 |
| comm、stream、现有 magic/选窗 | peer 映射、算子提交及通信窗口，沿用现有机制 |
| yOut、通信区/必要 workspace | 输出与内部状态的存储，不是新增业务路由信息 |

沿用 Memory 的 expertOut 输入契约：FP16/BF16 expert 结果，quantMode 控制通信量化。
本接口不包含外部量化 expertOut 的 scale，不能把路由 weight 当成解码 scale；若未来支持该类
输入，应另行扩展契约，不在当前已确认接口中隐含承诺。

TokenNum 沿用 `sendCounts[E−1]` 读取逻辑，不改为标量或仅指向末项的指针；
完整 prefix 作为原接口输入保留，但新路由计算只需要末项。expertTokenNums、旧 assistEx/group、
原始 expertIds、独立 relayRank/t、firstHopRow 和 group 总数均不需要额外传入。
MaxTokenNum 是分配上界，可由配置推导，不能替代 TokenNum。即使本卡 TokenNum=0，
仍可能需要根据 BS/表 4/表 5 接收本卡原始 Token 结果；该值只界定 expert 行数，不代表整卡无工作。

运行时到达状态及归约进度由 Combine 内部维护，不是调用方额外提供的路由 metadata。
五表的完备性不代表单靠 mask 能判断 payload 已到达；窗口复用采用已确认的模型调度保证。

### 5.1–5.4 历史候选的处理

此前拆分成员去向/weight、保存 originalK、按 source/token/k 或 relay/t 定位的候选已由 §5.0
替代，不再作为实现选项。保留本节编号以便追溯，当前权威布局只有上述五表。

### 5.5 基于现有 Memory Combine 的接口调整（2026-09-19）

用户已明确：沿用 Memory Combine 的现有参数，不删除、不替换，只新增；暂不使用的参数
保留，供后续功能接入。本文撤回此前精简为 16 参数的建议。
确定新增独立 `TileXRMoeEpCombineMemoryServerDedup`，保留现有 Memory 24 个参数的名称、
类型和顺序，再追加新输入；现有 `TileXRMoeEpCombineMemory` 保持兼容。
以下签名作为本阶段确定的设计接口；尚未修改公开声明、Host 或 kernel。

#### 五表与现有参数的对应

| 表 / 基础信息 | 接口处理 | 说明 |
| --- | --- | --- |
| 表 1 TokenType | 新增 tokenType | uint32[MaxTokenNum] |
| 表 2 DestinationIndex | 沿用 assistInfoForCombine | 同一三字段存储，dedup 按表 1 区分 source/token/k 与 uint32 offset/占位 |
| 表 3 RelayReadIndex | 新增 relayReadIndex | MaxTokenNum*K*8B，含 N/selfWeight header |
| 表 4 SourceMask | 新增 sourceMask | uint32[BS]，与旧 bool xActiveMask 不同 |
| 表 5 SourceWeight | 沿用 expertScales | FP32[BS,K]，传入 Dispatch 生成的最终权重 |
| TokenNum | 沿用 sendCounts 数组输入 | 已核实是 int32_t* prefix 数组，无共享专家时末项为 TokenNum；不存在另一个标量 epRecvCounts |
| expertOut/yOut、comm/stream、shape、dtype/quantMode | 全部沿用 | 基础数据、通信与配置输入 |
| epWorldSize/epRankId、tpWorldSize/tpRankId、expertShardType、globalBs | 全部保留 | 保留现有参数，当前范围按支持约定校验；后续拓展仍可使用 |
| xActiveMask/activeMaskType、sharedExpertX/sharedExpertNum/sharedExpertRankNum | 全部保留 | 当前不实现的特性使用当前允许的关闭配置；参数存在不等于功能已支持 |

现有参数保留，只新增 tokenType、relayReadIndex、sourceMask 三项，共 27 参数。
assistInfoForCombine 承载表 2，expertScales 承载表 5，不重复增加对应指针。
assistInfoForCombine 继续按现有 int32_t* 声明，每行三项、12B；类型 3 第一项按 uint32 位型
解释为槽号，其他两项存 -1，不能按负数过滤第一项。类型 1/2 仍为非负 source/token/k。

#### sendCounts / epRecvCounts 命名与 TokenNum

Memory Dispatch 公开输出名为 sendCountsOut，在公共 Host 参数结构里赋给 epRecvCountsOut；
Memory Combine 公开输入名为 sendCounts，在 Host 参数结构里赋给 epRecvCounts，
最后又以 sendCountsGM 传入 Memory kernel。这些是同一 buffer 在公共结构与 Memory 接口中的
不同命名，不是还缺一张计数表：

```text
TileXRMoeEpDispatchMemory(..., sendCountsOut, ...)
    → EpDispatchParams.epRecvCountsOut = sendCountsOut
    → Memory Dispatch kernel 写累计 prefix
    → 原 buffer 传入 TileXRMoeEpCombineMemory(..., sendCounts, ...)
    → EpCombineParams.epRecvCounts = sendCounts
    → Memory Combine kernel 的 sendCountsGM
```

当前 Memory Combine 的 sendCountsGM 仅在 ReadSendCount 中读取最后一个有效项，得到
selfSendCnt_，作为要处理的本卡 expertOut 有效行数；后续不遍历完整 prefix 来决定发送地址。
实际代码对非正 count 取 0；合法 Dispatch 输出为非负前缀。

```text
moeRankNum = rankSize - sharedExpertRankNum
localExperts = moeExpertNum / moeRankNum
countLength = (rank < sharedExpertRankNum) ? rankSize : rankSize*localExperts
TokenNum = sendCounts[countLength - 1]
```

当前 Server Dedup 无共享专家，因而 countLength=moeExpertNum，
`TokenNum=sendCounts[moeExpertNum−1]`。将来接入共享专家时，不能把该特例无条件用于全部 rank。
普通非 Memory Dispatch 的公开参数确实叫 epRecvCountsOut，但不能仅凭同名就混用不同 backend
的数据格式；这里核对的是上述 Memory 路径。当前 Memory 公开签名没有另一个 epSendCounts 参数。

epRecvCountsOut/epRecvCounts 是同一 int32_t* buffer 的内部名称，没有额外的标量输出。
本次确认继续从数组末项读取 TokenNum；是否保留 ReadSendCount 函数名不影响这个输入契约。
普通非 Memory 路径的逐 source count 不能与 Memory 的累计 prefix 混用。

#### 已确认：只追加参数的公开签名

前 24 项沿用原 Memory Combine，sendCounts 保持数组指针；新 3 项追加在末尾：

```cpp
int TileXRMoeEpCombineMemoryServerDedup(
    void *expertOut, int32_t *assistInfoForCombine, int32_t *sendCounts,
    float *expertScales, bool *xActiveMask, int64_t activeMaskType,
    void *sharedExpertX,
    TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK,
    int64_t moeExpertNum, int64_t epWorldSize, int64_t epRankId,
    int64_t tpWorldSize, int64_t tpRankId, int64_t expertShardType,
    int64_t sharedExpertNum, int64_t sharedExpertRankNum,
    int64_t quantMode, int64_t globalBs, void *yOut,
    TileXR::TileXRDataType dtype, aclrtStream stream,
    const uint32_t *tokenType,         // 新增表 1
    const void *relayReadIndex,        // 新增表 3
    const uint32_t *sourceMask);       // 新增表 4；表 5 沿用 expertScales
```

表 2 沿用原三字段 12B 存储，uint32 offset 以相同 32 位存储承载。新旧入口为不同符号，
不改变原 Memory 的 ABI。内部 tiling/launch ABI 与 27 个公开参数不是一一对应。
不新增 TokenNum、MaxTokenNum、peer buffer、windowOffset、stride、magic 或 workspace 参数；
现有 shape/comm/sendCounts 提供相应信息。现有 Memory 公开入口本身也没有 workspace 参数。

weight 可选的精确契约仍待确认：现有 Memory 在 quantMode=0 时允许 expertScales=nullptr 并
直接求和，但 quantMode=3/4 的 Host/kernel 入口会拒绝空权重。新入口如复用 nullptr 表示关闭
加权，还必须统一规定表 3 的 selfWeight/memberWeight 同时是否忽略；不能只跳过表 5 却继续
在 relay 加权。该规则通过现有参数收口，不增加第四个新参数；详细设计完成前不能宣称支持可选加权。

现有 Memory 的 quantMode=3/4 以 FP16/BF16 expertOut 为输入，发送时生成 MXFP8 与 scale，
接收时解量化；没有外部量化 scale 指针。因此沿用这套输入契约无需新增 expertOut scale 参数。
Dispatch 曾输出 FP8 不等于 expert 计算后的 expertOut 也是 FP8。若要直接接收量化 expertOut，
才需要另行扩展输入 dtype/scale；这不是当前 Memory 已提供的功能。

Memory 内部框架的复用与改造见[Combine 模块设计](ep-memory-server-dedup-combine-design.md#模块边界m0m6)。

源码依据：[公开签名](../../src/include/tilexr_ep.h)、
[Memory Host 入口](../../src/ep/host/tilexr_ep_memory_combine.cpp)、
[Memory kernel](../../src/ep/kernels/tilexr_ep_combine_memory_kernel.h)。

## 6. 共用通信域的布局框架

两算子使用同一组 peer 映射及现有选窗；窗口内部允许按算子定义各自视图。
不固定“窗口 0 永远 Dispatch、窗口 1 永远 Combine”，实际基址继续由现有 magic 选窗得到。

```text
每卡同一 EP 通信域
  窗口 0：4 GiB
  窗口 1：4 GiB

Dispatch 选中窗口的视图          Combine 选中窗口的视图
  控制区                         控制区：Combine 内部状态
  Dispatch 收包区                本卡 stage：供 Server 内 peer 读取
  对齐及保留区                   source 结果区：DIRECT 与 partial
                                 对齐及保留区
```

这是此前共用 EP 域的逻辑划分，具体顺序和 offset 待 J03 定稿；不是当前代码已删除历史预留。
用户最新提出“新的共享中转内存”，其域内划区/独立分配归属待补充；目前不据此改写已确认接口。
建议持久 metadata、expertOut、yOut 继续使用独立 GM，不挤入临时通信区。
相同窗口相对地址在两个算子视图中的用途可以不同；不需要为此新增阶段切换握手。
Combine 内 stage 与 source 结果区需同时存活，不能彼此覆盖。

| 区域 | writer | reader | 容量依据 |
| --- | --- | --- | --- |
| Dispatch 控制区 | 既有 count/cumsum owner | 对应 Dispatch 核 | 现有布局 |
| Dispatch 收包区 | source/relay 发送核 | 本卡 Dispatch 接收核 | E*BS*T |
| Combine 控制区 | Combine | Combine | 内部设计给出控制区预算，联合布局预留 |
| Combine stage | 本卡 | 本 Server relay 卡 | E*BS*Sstage |
| Combine source 结果区 | DIRECT / relay 卡 | 原始 source 卡 | BS*K*Sresult，加独立 Flag/scale（若未计入 stride） |
| Runtime 保留区 | runtime | runtime | 保持现有保留边界 |

DIRECT 和 partial 可共用 source 结果槽规则 `slot=tokenId*K+k`；mask 保证有效返回身份互不重叠。
五表没有返回 payload 的类型标签，SourceWeight=1 也不能区分真实权重 1 的 DIRECT 与 partial。
因此同一次调用的 DIRECT/partial 必须使用统一可解码的结果槽协议；dtype、scale 与 stride 在 J05/J06
确定，不能令 source 猜测格式。stage 格式可以与结果槽不同。

布局预算分别为：

```text
Dbytes = DispatchControl + aligned(E*BS*T) + 必要保留
Cbytes = CombineControl + aligned(E*BS*Sstage) + aligned(BS*K*Sresult)
         + 未计入 stride 的 Flag/scale/scratch + 必要保留
```

各算子视图均需满足选中窗口尾部保留边界；目标不是在每窗机械预留 Dbytes+Cbytes。
如果首版为降低迁移成本保留旧 offset/历史空洞，则按实际最高地址核算，而不是只算有效数据之和。
offset 与地址乘积使用 uint64，Host 与 kernel 共用布局计算，调用方分配按同一契约更新。

## 7. 共同契约状态与文档归属

| 编号 | 状态 / 归属 | 结论或下一步 |
| --- | --- | --- |
| J01 | 已确认 | expert/q 与类型 3 uint32 offset 定位同一逻辑槽，容量 E*BS |
| J02 | 已确认 | 五表语义、有效范围、行 stride；同步 Dispatch 生产与调用方分配 |
| J03 | 联合契约待收口 | 新中转区的存储归属、共享映射和窗口预算；Combine 给出自身内部区域需求 |
| J04 | 已迁出 | 分核、init、阶段和任务推进统一在 Combine 独立设计维护 |
| J05 | 接口与内部各自维护 | 本文维护输入 dtype/weight 语义；内部累加、通信格式和数值规则在 Combine 设计维护 |
| J06 | 已迁出 | Flag 发布/观察/清理与在途资源完成统一在 Combine 设计维护 |
| J07 | 接口已确认 | 原 24 参数追加 3 表；TokenNum 取 sendCounts 末项 |
| J08 | 验证分工 | 本文验证五表、地址和信息完备性；Combine 设计负责内部流程与设备验收 |

[Combine 模块设计](ep-memory-server-dedup-combine-design.md#模块边界m0m6)独立维护内部功能与设计方向。
推进顺序为完成全部模块初版，再由用户整体通读，届时统一整理遗漏点和待确认项。

## 8. 验证边界与维护要求

本版只核对源码与整理设计，不修改 kernel，不运行 NPU，不声称新 Combine 可执行。
后续先以独立路由参考验证角色、每条贡献恰好一次、权重、mask、寻址和容量，再做设备验证。
至少覆盖 K=1/K=32、高 row/高物理 slot、不同 source 同 tokenId、同卡/跨卡成员、
无 group、仅 DIRECT；Combine 的空职责核、ready/调度与在途资源验证见独立设计。
数值验证覆盖多成员不同 expertOut，不能用完全相同 payload 掩盖错址。

Flag 可见性与多引擎流水需要设备证据；CPU/reference 和编译不能证明跨卡数据可读。
联合测试遵循用户给定的模型调度边界，不另加跨轮重叠要求。

维护要点：从 push 改为 pull 时，需同时确认“哪张卡存、双方如何定位、何时可读”；
复用边界按已确认的模型契约。位置字段一旦变化，metadata 写出、Host 容量、调用方分配、
Combine 消费及独立 oracle 必须一起更新，不能只反转搬运方向。

维护要点：复用接口应核对公开指针→Host 参数别名→设备读取位置。Memory 的 epRecvCounts
名称不改变 sendCounts 的累计 prefix 契约；只有末项代表 TokenNum，不能把首项或 int32_t*
误当单值计数。接口参数保留与特性已实现也应分别标注，避免调用方据签名推断支持范围。

文档维护边界：联合设计只维护跨算子契约；Combine 核数、init、同步、流水与模块实现
集中在独立 Combine 设计，避免双份流程在后续修改中分叉。
