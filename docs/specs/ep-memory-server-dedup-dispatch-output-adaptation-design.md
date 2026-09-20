# Server Dedup Dispatch 输出适配 Combine：改动方案

## 1. 状态与讨论方式

日期：2026-09-19。源码基线：`e2d1704`，分支：`codex/SilentEmotion`。
本文是 Dispatch 修改输出以适配 Server Dedup Combine 的方案设计，尚未实施。
共同语义依据[Dispatch / Combine 联合设计 §5.0](ep-memory-server-dedup-dispatch-combine-design.md#50-五表契约2026-09-19)。

用户已确认：

- 直接使用五表输出接口，废弃旧接口，不保留旧输出的兼容路径。
- 内部改动按发生位置拆分，逐项说明原因和建议方案，由用户逐项确认。
- 接口确认后，测试改为校验新输出；不再使用的旧接口校验和残留逻辑可以删除。

2026-09-19 讨论更新：D01 已确认；表 5 沿用 expertScalesOut 名称。D03 改为源端直接构造
可原样保存的 RelayReadIndex 段。包头保留 32B 空间及 srcRank/srcTokenId/topK/type 四个有效值，
包头与索引段分别 32B 对齐，索引段不跨传输块的 payload 边界，Init 预计算两个 wire offset；
这些布局决定已确认。PacketHeader.type 与最终 TokenType 输出使用同一 uint32 枚举：
1=DIRECT、2=第一跳、3=转发终端；无中间类型编码或输出转换。
第一跳原样保存源端生成的 8*K 索引段已确认，其余连带项按清单确认。
D02 沿用源端生成规则、D04 沿用接收任务定位、D05 按类型生成 DestinationIndex 已确认。
D07 已确认：本卡正确保留 type_2，最终转发前仅将包头 type 改为 type_3，其他字段不修改。
T01 已确认采用独立参考与逐输出校验。T02 按最新讨论改为全局路由参考加各 Rank 本地匹配，
不新增 Rank 间校验通信或逐成员记录汇总；2026-09-20 已确认采用该方案。
D08 写出与完成依赖、D09 Host/caller 迁移、T03 清理与回归推荐方案已确认。
生产修改 D01–D09 及测试 T01–T03 已全部确认，2026-09-20 开始按第六节实施。
本文按清单记录已确认决策，确认不代表实现或验证已完成。
确认某项后更新该项状态和结论，不将其他待确认项自动视为通过。
用户已授权先提交并推送本文档，再修改生产代码与测试代码。
本轮讨论改动原因、数据来源和新行为，不展开性能、容量或资源影响。

## 2. 已确认的目标输出契约

### 2.1 五张表

`row` 是本卡 Dispatch 输出行，与 expandXOut 及恢复行序后的 expertOut 对齐。
`p` 是本卡原始输入 TokenId。两者不能混用。
`C` 表示现有 MaxTokenNum，即 `R*BS*min(K,E/R)`；`TokenNum=sendCountsOut[E-1]` 为有效输出行数。

| 表 / 建议参数名 | 格式 | 有效范围及语义 |
| --- | --- | --- |
| TokenType / `tokenTypeOut` | uint32[C] | 前 TokenNum 行；1=DIRECT，2=第一跳，3=转发终端 |
| DestinationIndex / `destinationIndexOut` | uint32[C][3] | 类型 1/2 为 `[srcRank, srcTokenId, originalK]`；类型 3 为 `[slot, UINT32_MAX, UINT32_MAX]` |
| RelayReadIndex / `relayReadIndexOut` | 每行 K 个 8B 块 | 仅类型 2 行有效；第 0 块为 header，第 1..N 块为成员 |
| SourceMask / `sourceMaskOut` | uint32[BS] | 原始输入 p 的最终应接收 k 位集合 |
| SourceWeight / `expertScalesOut` | FP32[BS][K] | 仅 mask 置位处有效；DIRECT 为真实 weight，第一跳返回槽为 1.0 |

DestinationIndex 采用上一轮建议的三个 uint32，行距为 12B。
slot 是本卡 Combine stage 的逻辑 Token 槽号，不是字节地址；0 是合法槽。
类型 2 的 originalK 是 group 的 returnK，即第一跳的原始 k。

**与 Combine 接口衔接：** Dispatch 保留已确认的 `uint32_t *destinationIndexOut`，
Combine 保留联合设计的 `int32_t *assistInfoForCombine`。调用方将同一 GM 分配的地址
按目标指针类型传入（例如 `reinterpret_cast<int32_t *>(destinationIndexOut)`），不复制或转换表内容。
两端共用三个 32 位 word、12B 行距的存储契约；类型 3 的首项始终按 uint32 位型解释，
后两项的 `UINT32_MAX` 与 `-1` 是同一 0xffffffff 位型。不能将首项的符号作为合法性判据。
这一衔接保留两边已确认的入口，不新增兼容旧 assistEx 的路径。

TokenType 是 wire 包头与持久输出表共同使用的类型定义：

| 值 | 统一语义 | 包头与输出的关系 |
| --- | --- | --- |
| 1 | DIRECT / r_direct_token | 源端填写 1，接收端原值写入 TokenType[row] |
| 2 | 第一跳 / r_wait_relay_token | 源端填写 2，第一跳本地输出保存 2 |
| 3 | 转发终端 / r_relay_token | 第一跳向成员发送前将包头改为 3，终端原值写入输出 |

Host/Dispatch/Combine 共用一份 uint32 类型枚举定义，PacketHeader.type 和表 1 不分别维护数值映射。
N 只表示索引段成员数量，不用 N 推导或替换 type；有效行没有类型 0。
第一跳本地表 1 的类型在改写 wire 前保存，发出类型 3 不改变本地行的类型 2。

RelayReadIndex 的字段按联合设计固定为：

| 块 | 字节 0..3 | 字节 4..7 |
| --- | --- | --- |
| 第 0 块 | uint32 N：第一跳以外的成员数 | FP32 selfWeight |
| 第 i 块，1≤i≤N | 字节 0..1 为 uint16 expertId，字节 2..3 为 uint16 q | FP32 memberWeight |

类型 2 满足 `1≤N≤K-1`，本地第一跳贡献直接取 expertOut[row]。
成员保持原包尾 entry 顺序，不保存 originalK；第 N+1..K-1 块无效。
类型 1/3 的整行 RelayReadIndex 无效。K=1 不产生类型 2。
weight=0 是合法数据，不作为有效性标记；权重保存 FP32 原始位型。
本次按当前必填权重路径适配，可选无权重模式另行设计。

### 2.2 旧接口的处理

废弃的是 Server Dedup Dispatch 的旧 metadata 输出契约：

| 旧输出 | 新处理 |
| --- | --- |
| `assistExOut` | 拆为 TokenType 和 DestinationIndex；第一跳 selfWeight/N 移入 RelayReadIndex header |
| `groupOut` | 替换为 RelayReadIndex，不再输出 firstHopRow、t*K+k |
| `expertScalesOut` | 名称、类型及源端有效权重语义均沿用；SourceWeight 仅为表 5 的逻辑名称 |
| `sourceMaskOut` | 沿用 |
| Server Dedup 入口中的 `assistInfoForCombineOut` | 删除旧参数名称及其未使用语义；新接口显式传 DestinationIndex |

不同时写旧 assistEx/group，不提供旧格式转换、旧重载或兼容开关。
保留 expandXOut、dynamicScalesOut、expertTokenNumsOut、sendCountsOut 的现有数据语义。
普通 Memory Dispatch/Combine 的三字段 assistInfoForCombine 属于另一接口，不在本次废弃范围内。
历史 Dispatch 包尾字段按已确认的 D03/D07 替换，旧协议仅留在历史实现记录中。

## 3. 按发生位置拆分的改动项

已确认项及部分确认项见第五节。第二节五表契约和 D03 布局不重复请求确认；
各项记录改动原因、方案和确认状态；D01–D09 的生产方案均已确认。

### D01：公共入口、Host 参数与 kernel launch 传参

**位置：** [tilexr_ep.h](../../src/include/tilexr_ep.h)、
[公共 Host wrapper](../../src/ep/host/tilexr_ep_memory_server_dedup_dispatch.cpp)、
[Host 参数](../../src/ep/host/ep_memory_server_dedup_host.h)、
[launch 参数](../../src/ep/host/ep_memory_server_dedup_launch.h)、
[launch 构造](../../src/ep/host/ep_memory_server_dedup_launch.cpp)、
[kernel 入口](../../src/ep/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.cpp)。

**原因：** 当前参数只有旧 assistEx/group 等输出，且 Server Dedup kernel 忽略 assistInfoForCombineOut。
新五表必须从公共 API 一直传到对应生产核，不能只改 kernel 局部变量名。

**建议方案：**

- 保留函数名 `TileXRMoeEpDispatchMemoryServerDedup`，替换其 metadata 参数段为下列五个显式地址。
- 在公共签名中删除旧 assistInfoForCombineOut 槽位，五表按表号排列；其他参数顺序保持原相对顺序。
- Host 专用参数结构、注册 launch 参数与 kernel 签名统一使用新名称；去掉被忽略的旧字段。
- Sender 接收表 4/5；Receiver 接收表 1/2/3。标量仍由现有 GM tiling 地址解析。
- Host 将五表作为新接口必需输出校验，调用方同步改为新声明并重新编译，不将 nullptr 当成旧接口兼容标识。

```cpp
// 公共函数中的新 metadata 参数段；不是一个新增的独立函数。
uint32_t *tokenTypeOut,
uint32_t *destinationIndexOut,
uint32_t *relayReadIndexOut,
uint32_t *sourceMaskOut,
float *expertScalesOut
```

RelayReadIndex 指针按 uint32 words 传递，每块两 words；解析 helper 明确区分 header 和 member。
member 第一 word 的低 16 位存 expertId、高 16 位存 q，第二 word 保存 FP32 weight 位型。
Host/设备/调用方共享格式定义并用编译断言验证字段位置。调用方分配和边界检查统一引用新布局定义。

内部注册 launch 的地址顺序固定为 commArgs、x、expertIds、expertScales、expandXOut、
dynamicScalesOut、expertTokenNumsOut、sendCountsOut、tokenTypeOut、destinationIndexOut、
relayReadIndexOut、sourceMaskOut、expertScalesOut、tilingData，最后为 int64 magic。
仍为 14 个地址加 magic（64 位地址目标下 120B）；tilingData/magic 位于 104/112B。
Host 打包、kernel 声明、入口替身、安装 consumer 与 sizeof/offsetof 断言同步更新。
tiling 仍仅传 GM 地址；不把两个计算所得 offset 或 tiling 内容追加到 launch 参数。

**确认状态：** D01 已确认。表 5 采用用户本轮纠正的原接口名 expertScalesOut，其余参数组织与传递方式按本节。

### D02：源端 SendData 的分类与表 4/5 生成

**位置：** [ServerDedupSender::Init / Process](../../src/ep/kernels/tilexr_ep_server_dedup_send.h)。

**原因：** 当前 sender 已经生成 SourceMask/SourceWeight 所需结果，需要对接新接口并明确索引归属。

**建议方案：**

- 沿用 expertScalesOut 地址、名称和现有写出逻辑，输入 expertScales 继续只读。
  上一版改名建议仅为对应 SourceWeight 术语，并无功能必要，现撤销。
- 第一跳发送后，仅置 returnK 位，对应 sourceWeight 写 1.0。
- DIRECT 发送后，置原始 k 位，写该 route 的真实 weight；其余成员位不置位。
- 表 4/5 按本卡输入 p 写出，不使用接收 row，也不由转发接收核补写。
- 关闭编译期去重时，所有 route 为 DIRECT，K 位均有效，全部权重为原始输入权重。
- 保留当前先第一跳、后 DIRECT 的任务组织和 server +1 顺序。

**确认状态：** 已确认沿用以上源端生成规则与现有任务组织；表 5 沿用 expertScalesOut。

### D03：源端 SendData 的 token 包尾构造

**位置：** [ServerDedupSender::FillTail / SendPacket](../../src/ep/kernels/tilexr_ep_server_dedup_send.h)、
[内部 wire 字段定义](../../src/ep/common/ep_memory_server_dedup_protocol.h)。

**原因：** 用户希望第一跳接收侧将尾部的 8B*K 段直接保存为 RelayReadIndex，
无需逐成员重建、压缩或改写字段。为此，源端发送时就应形成最终表行。

**建议方案：** 将逻辑包尾分为独立 PacketHeader 和不可变的 RelayReadIndex 段。

| PacketHeader 相对字节偏移 | 字段 | 类型/语义 |
| --- | --- | --- |
| 0 | srcRank | uint32，原始 source |
| 4 | srcTokenId | uint32，原始输入 token 行号 |
| 8 | topK | uint32，DIRECT 原始 k 或第一跳原始 k，即 returnK；不是总路由数 K |
| 12 | type | uint32，1=DIRECT、2=第一跳、3=转发终端 |
| 16..31 | padding | 占位，无有效业务字段，接收端不解析 |

包头仍占用完整 32B，不采用删除 srcRank 或缩为 12B 的方案。四个有效值各占 4B，
连续排列；32B 起始地址对齐针对 PacketHeader 和 RelayReadIndex 两个独立段，
不是要求包头内的每个 uint32 或索引段内的每个 8B 条目分别占用 32B。
包头 type 直接使用第二节的统一 TokenType 枚举；topK 的业务语义为 returnK。

RelayReadIndex 段与表 3 完全相同：第 0 个 8B 块为 `(uint32 N, FP32 selfWeight)`，
第 1..N 块为 `(uint16 expertId, uint16 q, FP32 weight)`，整个段固定为 8*K 字节。
成员顺序仍为原始 k 升序，但包内也不再存储成员 originalK。
DIRECT 的 N=0、selfWeight 填本 route 真实权重，接收端不输出其 RelayReadIndex 行。
无效成员块不要求初始化；整段复制后仍只允许消费第 0..N 块。

```text
hidden / 可选量化 scale
    → 对齐 → PacketHeader（32B）
    → 必要填充 → RelayReadIndex 原样段（8*K B）
    → 传输块尾填充
```

包尾角色独立于 RelayReadIndex.header.N。源端填 type=1/2；relay 转发时只把
PacketHeader.type 改为 3，整个 RelayReadIndex 段保持不变。
旧 forwardCount/forwardEntryCount 的职责由 tokenType/N 分别承接；删除 wire 中独立的
relayRank、forwardTokenIdx 和成员 originalK，权重仅在该段相应位置保存。

**已确认的对齐与 Init 偏移计算：** 当前每个 512B DataAsFlag block 由 480B payload
和尾部 32B Flag 构成。PacketHeader 和 RelayReadIndex 的 wire 起点均须 32B 对齐。
索引段若放不下当前 block 的 payload 区，则整段移到下一个 block 的 payload 起点；
即使尚未越过 512B，只要占用了尾部 Flag 区也必须移到下一块。
Init 中先用未插 Flag 的逻辑 payload 坐标计算布局，再缓存两个相对完整 wire 包起点的偏移：

```text
dataEnd = quantized ? quantCount + scaleBytes : H*2
headerPayloadOffset = align32(dataEnd)
readIndexBytes = 8*K
candidate = headerPayloadOffset + 32
readIndexPayloadOffset = candidate
if candidate % 480 + readIndexBytes > 480:
    readIndexPayloadOffset = align480(candidate)

headerWireOffset = (headerPayloadOffset / 480)*512 + headerPayloadOffset % 480
relayReadIndexWireOffset = (readIndexPayloadOffset / 480)*512 + readIndexPayloadOffset % 480

payloadBytes = align32(readIndexPayloadOffset + readIndexBytes)
blockCount = ceil_div(payloadBytes, 480)
packedPayloadBytes = blockCount * 480
tokenStride = blockCount * 512
```

quantCount/scaleBytes 沿用现有量化布局定义；布局计算用 uint64 中间值。
logical payload 和 wire 是两个不同坐标：Sender 现有 FillTail 在插入 Flag 前的 payload 中
使用 headerPayloadOffset/readIndexPayloadOffset；接收解析与转发改写使用两个 wire offset。
共用布局 helper 一次给出这些值，禁止把 wire offset 用在未打包的 payload 上。
Sender 保留现有打包步骤；新增间隙及无效成员不参与结果语义，所有 block 的 Flag 仍按原规则填写。

K≤32 时 readIndexBytes≤256，整个段可以放在一个 480B payload 块内；
两个段的逻辑起点和对应 wire 起点均保持 32B 对齐。32B 包头本身也不会跨入 Flag 区。
Sender/Receiver 在各自 Init 中通过共用布局计算获得并保存 headerWireOffset 和
relayReadIndexWireOffset，逐 token 的包头读写和索引段访问直接使用这两个偏移，
不重复计算位置、不逐条跳过 Flag；它们由已有 shape/layout 推导，不新增外部 launch 参数。
Host 侧包长、tokenStride 与设备 Init 使用同一布局规则，包长包含移到下一块所需的填充。
Flag 仍按完整包原协议插入和检查，
不能将其夹入 RelayReadIndex 表。第一跳仍按现有过程接收整个包，之后直接从 wire 中该段
向 RelayReadIndex[row] 写出恰好 8*K 字节，无额外字段转换或逐成员拼表。
若“读取下来”指另起一次仅尾部的 GM 读取，本候选不新增该动作，而是使用已接收的包。

源端 FillTail 改为填写新 PacketHeader 和最终表行；Host/kernel 共用新 offset 计算，
DataAsFlag 打包、探测与槽位寻址使用更新后的统一布局。量化与非量化采用同一尾部规则。

**确认状态：** 32B 包头的四个有效值、两个段的 32B 对齐、索引段整体移块及 Init
预计算两个 wire offset 已确认。源端生成可原样保存的 8*K 索引段按用户要求设计；
包头 type 与输出 TokenType 共用枚举、第一跳原样保存索引段也已确认。
具体任务定位、转发循环和写出同步已按后续项确认；生产代码与测试均未实施。

### D04：接收任务定位与 TokenType 生成

**位置：** [ServerDedupReceiveTasks](../../src/ep/kernels/tilexr_ep_server_dedup_receive_tasks.h)、
[ServerDedupReceiver::ConsumeLoaded](../../src/ep/kernels/tilexr_ep_server_dedup_recv.h)。

**原因：** 新表按稳定输出 row 索引，并且类型 3 需要接收 slot；当前两个值都已存在。

**建议方案：**

```text
row  = tasks.Begin() + j
slot = tasks.Position(j)
TokenType[row] = packet.header.type  // 同一 uint32 枚举，直接保存 1/2/3
```

继续使用稳定 j，不用 pending 数组中的当前位置或到达顺序作为表索引。
Count、prefix、任务分区与 Probe 负责现有接收定位；新表不需要另一次编号或原子取号。
有效行之外不新增类型值，也不由类型 0 推断结束。

**确认状态：** 已确认沿用现有稳定 row/slot 和任务归属，不改变 Count/prefix/Probe 的定位逻辑。
包头与表 1 的类型定义一致，接收端直接保存类型，不做编码转换。

### D05：解析 token 包尾并构造 DestinationIndex

**位置：** [BuildServerDedupAssist](../../src/ep/kernels/tilexr_ep_server_dedup_receive_packet.h)、
[ConsumeLoaded](../../src/ep/kernels/tilexr_ep_server_dedup_recv.h)。

**原因：** 新 Combine 对类型 1/2 需要 source 返回身份，对类型 3 只需要本卡 stage 槽号。
旧终端 assistEx 的自身 k/weight、继承 relay/t 不再是持久输出所需信息。

**建议方案：**

- 类型 1/2：从新 PacketHeader 取 srcRank、srcTokenId、topK（returnK），写 DestinationIndex[row]。
- 类型 3：写 `[tasks.Position(j), UINT32_MAX, UINT32_MAX]`。
- 删除终端为构造 assistEx 而遍历 entries、匹配自身 expert、提取自身 k/weight 的逻辑。
- 删除对应终端 expert 推导和旧 assistEx 八字段写入；不将终端包保留的第一跳 returnK/selfWeight 当作本行返回信息。
- slot 保存逻辑编号；Dispatch 不把 Combine stageOffset、Sstage 或绝对地址编码进表。

**确认状态：** 已确认采用本节方案。类型 1/2 保存原始返回身份；类型 3 直接保存 slot，
取消旧终端 entry 匹配及 relay/t 持久记录。

### D06：第一跳原样保存 RelayReadIndex

**位置：** [BuildServerDedupGroup](../../src/ep/kernels/tilexr_ep_server_dedup_receive_packet.h)。

**原因：** 旧 group 不满足 pull 寻址；D03 已确定由源端生成最终表行，
第一跳不再需要 BuildServerDedupGroup 的字段转换。

**建议方案：**

- 仅类型 2 保存表 3，将已接收 wire 的 RelayReadIndex 段原样写到输出 row。
- 删除旧 group 构造，第一跳不重填 N/selfWeight，不逐成员重新写 expert/q/weight。
- 接收端为执行 Dispatch 转发可以读取该段 N 和各成员 expert/q，但始终不修改该段。
- 原 firstHopRow、t*K+originalK 不再生成；类型 2 的返回身份由表 2 保存。
- 源端负责字段窄化和按位保存 weight；Host 保证范围，独立参考负责核验结果。

**确认状态：** 第一跳直接保存源端生成的 8*K 段，不修改字段、不重新拼表，已确认。
转发读取该段的循环见 D07，搬运与完成依赖见 D08，不重复讨论本项保存方式。

### D07：接收侧展开转发与公共 wire 改写

**位置：** [PrepareServerDedupForward](../../src/ep/kernels/tilexr_ep_server_dedup_receive_packet.h)、
[ConsumeLoaded 的转发循环](../../src/ep/kernels/tilexr_ep_server_dedup_recv.h)。

**原因：** 第一跳既要保留类型 2 的持久输出，又要让后续副本被识别为类型 3；
角色切换不能改动要原样保存的 RelayReadIndex 段。

**建议方案：**

1. 从原始 PacketHeader 记录本地 TokenType=2（type_2）和 DestinationIndex，读取 RelayReadIndex.header.N。
2. 将索引段原样保存到本地 RelayReadIndex[row]；本地类型记录不再依赖后续可改写的 wire 字段。
3. 最终转发前，仅将 wire 的 PacketHeader.type 从 2 改为 3（type_3）；
   srcRank、srcTokenId、topK、包头占位以及整个 RelayReadIndex 段均保持不变。
4. 随后从 wire 索引段读取 N 个成员 expertId/q，结合原始 source 计算 Dispatch 目标槽，
   将这一 type_3 公共副本发送给各成员，不再逐目标改写包内字段。

本地表 1 的值必须来自改写前的角色。表 3 所对应字节始终只读，因此其写出可使用相同 wire。
本地 TokenType[row] 最终必须为 2，不能因异步写出仍引用可变包头而误写成 3。
以上规定数据保存与改写的先后关系；MTE 写出完成和 lane 复用依赖按已确认的 D08 执行。
不修改 N，不移除已发送成员，不把 source 改为 relay；转发任务直接读 wire，不回读 GM 输出表。
不再填写 wire relayRank/t。终端依 D04/D05 识别类型 3，并把自身接收 slot 写入表 2。

**确认状态：** 已确认最终转发时仅修改包头 type，其余字段不变；本卡正确保留 type_2。
按以上顺序展开转发，不回读输出表，不再填充旧 relayRank/t。具体写出同步归入 D08。

### D08：接收结果写出与完成边界

**位置：** [ServerDedupReceiver::Init / ConsumeLoaded / Process](../../src/ep/kernels/tilexr_ep_server_dedup_recv.h)。

**原因：** 原来写出 assistEx/group 的位置需要改为三张独立输出表，并确保输出与同 row 的数据一致。

**建议方案：**

- 每个有效 row 写 TokenType 和 DestinationIndex；类型 2 从原 wire 直接写出整个 8*K 字节 RelayReadIndex 段。
- GM 使用已定义行距：表 1 为 4B、表 2 为 12B、表 3 为 K*8B；内部 staging 的对齐不改变 GM 行距。
- 表 3 整段写出会携带无效块字节，但仍只消费第 0..N 块；不要求清零无效行/块及各表容量尾部。
- 表 1/2 在本 lane 内构造并通过 MTE3 写出；表 3 直接引用本 lane 的 wire 段。
  分别维持 metadata Scalar 写入、wire MTE2 搬入到 MTE3 读取的发布依赖。
- metadata 写出和该 lane 的其他写出一起纳入完成事件；完成前不复用其 staging。
- expandXOut/量化 scale 继续按原 row 写出，核末仍完成本核写入和接收 Flag 清理。
- 删除旧 assistEx/group 的 GM Tensor、写出调用及只服务于旧输出的变量/helper。

**实施细化：** 每个接收 lane 在完整 wire 后保留两个独立 32B staging，分别暂存表 1 的
4B 和表 2 的 12B，laneStride=tokenStride+64B；表 3 直接引用 wire，不再分配旧 group staging。
使用支持按字节长度写出的搬运路径，GM 分别写恰好 4B、12B、8*K B，不能按 staging 大小写 32B。
表 3 的 GM 行起点不保证 32B 对齐（例如 K=6 时行距 48B）；不能因此改变表的紧密行距。

依赖顺序为：MTE2 搬入完成后 Scalar 解析 → 独立保存本地 type/目的信息 →
最终转发前改写 wire.type → 发布 Scalar→MTE3 依赖 → 写出 metadata/payload 并转发 →
该 lane 全部 MTE3 完成后才允许下一次加载和 staging 改写。
保留 wire 搬入到 MTE3 读取的依赖，以及核末 drain、ClearFlags 和本核完成边界；不新增全核 SyncAll。
索引段保存是逻辑上发起原样写出，不要求它先写完 GM 才能开始转发；两者只读同一 wire 段。
lane 数继续按剩余 UB 与新 laneStride 计算；发送 scratch 也按新 packedPayloadBytes/tokenStride 计算，
不得保留旧 groupStride 或旧包长作为分配依据。

**确认状态：** D08 推荐方案已确认。上述细化用于落实已确认的行距、角色快照和完成依赖。
搬运 API 的目标版本兼容性与实际事件顺序必须在 CANN 9.1/A5 编译及验证中核验。

### D09：Host 布局定义、调用方与旧定义清理

**位置：** [布局定义](../../src/ep/host/ep_memory_server_dedup_layout.h)、
[布局计算](../../src/ep/host/ep_memory_server_dedup_layout.cpp)、
[Host 校验](../../src/ep/host/ep_memory_server_dedup_host.cpp)、各 Server Dedup 调用方。

**原因：** 调用方必须分配并持有新表，旧字段名和旧布局不能继续作为新输出的描述。

**建议方案：**

- 以 TokenType、DestinationIndex、RelayReadIndex 对应布局字段替换 assistEx/group 字段。
- 更新指针、有效字段范围及分配记录校验；将字段定义供调用方与测试共同引用。
- 调用方按五表顺序传参，并持有这些输出直到对应 Combine 完成。
- 保留当前 sendCountsOut/expertTokenNumsOut 的生产方式；TokenNum 仍取现有计数，不额外输出 group 总数。
- 删除废弃输出结构和参数的活跃代码引用；D03 的新内部包头与索引段定义单独维护，删除已被替代的旧 wire 字段/entry 定义。
- 主规格在代码迁移时更新为新实现；历史验证记录保留其当时版本身份，不改写成新接口已通过。

Host 布局使用 tokenTypeBytes=C*4、destinationIndexBytes=C*12、relayReadIndexRowStrideBytes=K*8、
relayReadIndexBytes=C*K*8；sourceMaskBytes=BS*4、weightBytes=BS*K*4 沿用。
乘法沿用现有溢出检查，五表基地址至少 4B 对齐，表内行距不额外向 32B 取整。
裸指针入口只能校验可见的指针/shape/布局条件，不能声称能推断实际分配大小；
容量、设备归属、分配之间重叠由持有分配记录的 caller/test 校验。
新输出的 shape/大小和 wire 布局同时迁移，安装头文件、Host 库、kernel 与 consumer 必须配套重建。
Combine stage/result 的物理布局及历史 relay 预留区重设计不在本次 Dispatch 输出迁移中展开；
已有通信窗口的容量计算仅按本次新 Dispatch tokenStride 传播更新，不能混入未实现的 Combine 格式。

**确认状态：** D09 推荐方案已确认。公共表布局由第二节定义，Dispatch→Combine 指针衔接也见第二节。

## 4. 测试改造方案（T01–T03 已确认）

接口已确认采用五表；以下测试规则在对应内部方案确认后随实现更新。

### T01：独立参考与逐输出校验

**位置：** [输出参考](../../tests/ep/integration/server_dedup_dispatch_reference.h)、
[设备结果校验](../../tests/ep/integration/server_dedup_dispatch_consumer.h)、
[测试入口](../../tests/ep/integration/test_tilexr_ep_server_dedup_chain.cpp)。

**原因：** 当前测试读取旧 assistEx/group。应从原始路由直接建立新五表预期，不通过旧表转换，也不拿实测字段充当预期。

| 输出 | 建议校验 |
| --- | --- |
| TokenType | 按原始路由分组逐行判断 DIRECT/第一跳/终端，与 1/2/3 比较 |
| DestinationIndex | 类型 1/2 比较原始 source/token/k；类型 3 由参考 source/expert/q 独立算 slot，并检查两个 UINT32_MAX |
| RelayReadIndex | 类型 2 比较 N、selfWeight、每个 expertId/q/weight 及顺序；仅比较有效块 |
| SourceMask | 按原始路由构造 DIRECT k 与各 group returnK 的并集，逐 token 比较 |
| SourceWeight | mask 有效位中，DIRECT 比较真实权重，第一跳比较 1.0；FP32 按位比较 |

保留完整 hidden、量化 hidden/scale、sendCountsOut、expertTokenNumsOut、输入权重只读和已消费槽 Flag 清零检查。
继续在批次完成后检查最后一轮输出，不因接口调整改变测试的轮次口径。

**确认状态：** T01 已确认采用本节推荐方案。

### T02：基于全局路由参考的成员地址匹配

**位置：** [输出参考](../../tests/ep/integration/server_dedup_dispatch_reference.h)、
[本 Rank 结果校验](../../tests/ep/integration/server_dedup_dispatch_consumer.h)、
[结果汇总](../../tools/validation/workflow.py)及其测试。

**原因：** 新表不再持久保存终端 relay/t，旧 relay_links 关联方式不再适用。
当前 DispatchRows 已遍历所有 source，并通过 Ids(shape, source) 在本地重建全局路由，
因此可以独立构造各 Rank 的预期输出和成员对应关系，无须新增 Rank 间数据交换。

**修订建议：**

1. Host 参考从测试输入的全局专家路由表出发，独立计算 group、原始 source/token/k、
   expert 的 q、各 Rank 预期行及 `(memberRank, slot)`；不用实测表或 kernel 寻址 helper 构造预期。
   当前测试可按一致的 shape、route mode、seed 重建输入；已有全局输入时直接使用该输入。
2. 参考中先验证每个类型 2 成员与预期类型 3 行一一对应，保留完整身份与出现次数，
   不用去重集合掩盖重复引用；检查遗漏、重复槽及无 owner 的终端。
3. NPU 运行批次完成后，各 Rank 仅回读自己的输出。类型 2 的实测 source/expert/q/weight
   及派生读槽逐项匹配本地预期；类型 3 的实测写槽逐行匹配本地预期。
   同时核对行数、类型、成员数和顺序，不能只比较槽集合或总数。
4. 沿用现有测试结果收集，确认同一次 case 的全部预期 Rank 均完整校验通过；
   缺 Rank、重复 Rank、case/输入不一致或任一 Rank 校验失败均不得判为通过。
5. 错误时保留 source/token、owner row、member 序号及预期/实测槽等诊断上下文。
   不增加 NPU/Host 的 Rank 间校验 collective、远端输出读取或逐成员关系记录的跨 Rank 汇总。

参考成员关系闭合且所有 Rank 的实测输出分别完整匹配该参考，即可确认实测 metadata 的读写槽对应。
仅在参考内部完成匹配不代表实测结果已通过，必须完成步骤 3/4。
原 relay_links 字段和依赖其明细的汇总判据直接删除，汇总改为检查各 Rank 新校验结果，
不在 Host 虚构 relay/t。此项与 T01 共用参考和本地遍历，无须再交换一遍实测表。
本文前版的“跨 Rank 实测关系记录汇总”由本方案替代；那一版也不要求增加算子通信。
实际 Combine 搬运和数值归约由后续 Combine 测试验证。

**确认状态：** 2026-09-20 已确认采用上述无新增 Rank 间通信、全局参考加本地实测匹配的方案。

### T03：废弃校验清理与回归覆盖

**位置：** Host ABI/layout/validation 测试、sender/receiver 替身测试、安装包 consumer、workflow fixture。

**删除或替换：**

- 旧 assistEx 八字段比较、输出 t==row 和 relay/t 继承检查。
- 旧 group.firstHopRow、recvSlot=t*K+originalK 和旧 group 布局断言。
- 旧 DispatchAssist 参考生成、旧输出分配、旧参数签名及测试替身。
- 基于旧 relay_links 的解析/故障注入，以及仅代表旧契约的 group_validated/prefix_indices_validated 标记。
- 用新五表与读写槽闭合的校验标记替换旧标记，并同步所有生产 reader、fixture 与断言。

上述删除覆盖已废弃的持久输出和 D03/D07 替换的旧 wire 字段；新 wire 按新协议独立测试，
普通 Memory 的 assistInfo 测试和有效 Dispatch 行序/Count prefix 校验继续保留。

**新增或迁移的场景：**

- 三种类型、K=1/K=32、无 group、关闭去重、本卡无接收行但有源端 mask/weight。
- 第一跳多个成员、同卡其他 expert 与跨卡成员、不同 source 的相同 tokenId、接收乱序。
- slot=0、高编号 slot、零权重、稀疏类型 2 行及无效块不消费。
- 错误 type、source/token/k、slot、N、selfWeight、expert/q、memberWeight 和成员顺序的故障注入。
- 在本 Rank 输出中注入错误端点、重复/遗漏成员和写读槽不一致，验证全局参考能发现错误；
  汇总测试覆盖缺 Rank、重复 Rank 和 case 不一致，不为故障注入新增 Rank 间通信。
- 校验新 PacketHeader、索引段及其 offset；第一跳表 3 有效字节同时对照源端段和独立 oracle。
- 校验包头 type 与表 1 使用同一枚举，DIRECT 输出 1、第一跳本地输出 2、转发终端输出 3。
- 校验头/索引段起点 32B 对齐，Init 缓存的两个 wire offset 与实际字节位置一致。
- 覆盖索引段原起点会侵入 Flag、恰好结束于 480B payload 边界及不跨 Flag 三种情形，
  K=1/K=32、量化与非量化均覆盖；侵入 Flag 时检查整段移至下一 512B block 起点。
- 检查转发只改变角色字段，RelayReadIndex 段逐字节保持；无效成员块不要求等于某个固定值。
- 保留已有非量化/量化结果比较及多轮复用测试。

实现阶段按项目要求进行 Host/替身验证、真实 CANN 9.1 A5 编译及用户侧设备验证；本文不登记通过结果。

**确认状态：** T03 推荐方案已确认，T02 无新增通信匹配方式也已确认。

## 5. 逐项确认清单

| 编号 | 确认内容 | 当前状态 |
| --- | --- | --- |
| A01 | 五表替换旧持久输出，旧接口废弃，不提供兼容路径 | 已确认 |
| A02 | 测试迁移到新输出，删除不再使用的旧接口校验 | 已确认 |
| D01 | 公共参数组织与 Host→kernel 接线；表 5 沿用 expertScalesOut | 已确认 |
| D02 | SendData 沿用 mask/weight 生成规则及现有组织；表 5 沿用 expertScalesOut | 已确认 |
| D03 | 32B 四字段头 + 8*K 索引段；段起点 32B 对齐，索引段整体移块，Init 缓存两个 wire offset | 已确认；连带处理见后续项 |
| D04 | 包头 type 与表 1 共用枚举，原值保存；沿用 row/slot 任务定位 | 已确认 |
| D05 | DestinationIndex 按类型生成，终端取消 entry 匹配 | 已确认 |
| D06 | 第一跳原样保存源端生成的 8*K RelayReadIndex 段，删除接收侧拼表 | 已确认；写出依赖见 D08 |
| D07 | 本地保留 type_2，最终转发前仅改包头为 type_3，其他字段不变 | 已确认；写出同步见 D08 |
| D08 | 按既定行距/范围写出三表，并确保 lane 完成边界 | 已确认 |
| D09 | Host/caller 迁移与旧定义清理 | 已确认 |
| T01 | 新五表独立参考与逐输出比较 | 已确认 |
| T02 | 全局路由参考构造成员关系，各 Rank 本地匹配，不新增校验通信 | 已确认 |
| T03 | 旧校验清理范围与故障注入覆盖 | 已确认 |

生产修改 D01–D09 与测试 T01–T03 均已确认，可按第六节完整实施。
五表格式、32B 包头、统一类型枚举、索引段原样保存及对齐/Init 偏移不再列为待确认问题。
维护原则：可直接保存的 wire 索引段应与输出表共用格式，角色字段应与该段分离；
包头和持久表表示同一角色时使用同一枚举，第一跳必须先保存本地角色再改写转发包头；
废弃旧字段时同步替换其参考、reader 和关联判据，
避免只删旧断言而丢失原有正确性覆盖。

## 6. 实现就绪检查与执行顺序

本节是本方案的实施顺序，不扩大已确认范围。目标为 Server Dedup Dispatch 产生可直接交给
Combine 的五张表；不实现 Combine，不引入 ephandle，不改编译期核数/去重开关、调度算法或普通 Memory API。
保持 C++14 Host 和 CANN 9.1/A5 目标，kernel 继续采用独立二进制注册启动。

| 步骤/依赖 | 修改范围与目标 | 交付及验收 |
| --- | --- | --- |
| I01，无前置代码依赖 | 公共协议定义、common data_layout、Host layout：固定枚举、五表格式、四字段头、payload/wire 偏移和包长 | 下游共用布局；protocol/layout 测试覆盖字段偏移、K1/6/32、跨 Flag、容量乘法和 Host/device 一致性 |
| I02，依赖 I01 | tilexr_ep.h、Host params/wrapper/launch、kernel 入口与 Init、全部 Server Dedup 调用方：五表接线 | public ABI/launch 测试逐指针检查身份及顺序；安装 consumer 使用新头文件编译；不保留旧签名兼容入口 |
| I03，依赖 I01/I02 | sender 与 wire helper：源端填写最终索引段、使用新布局打包、沿用 mask/weight 规则 | source_send 测试核验 header、索引有效字段、Flag、原始 q、发送顺序与重复 lane 使用；产物供 I04 消费 |
| I04，依赖 I01–I03 | receive_packet、receiver：按稳定 row 写三表、type_2 快照、type_3 转发、删除旧拼表及终端 entry 搜索 | receive 测试覆盖转发仅改 type、表 3 逐字节保存、4/12/8K 精确写出、相邻行保护、在途 lane 不覆盖、空接收和多轮复用 |
| I05，依赖 I02–I04 | reference/consumer、Host/替身测试、安装和验证 reader：执行 T01/T03，删除旧输出判据 | 五表独立预期及故障注入通过；量化/非量化 payload、计数及清 Flag 校验保留；按 T02 替换跨 Rank 汇总判据 |
| I06，依赖 I01–I05 | 真实 A5 构建及交付文档：验证新头文件/Host/kernel 配套，更新当前规格和验收边界 | 记录源码版本、CANN/编译器、构建配置、日志和退出码；用户设备验证另记，不将 Host 通过写成 NPU 通过 |

按依赖顺序执行，相关测试随修改迁移，不以大批删除测试使构建变绿。涉及源端、收端、
ABI 的共有定义必须先统一；旧结构是否可删以调用方迁移完成为前提，不能删除普通 Memory 仍使用的定义。

**验证入口（本次仅列计划，未执行代码验证）：**

```bash
cmake -S tests/ep -B build/server-dedup-host
cmake --build build/server-dedup-host -j8
ctest --test-dir build/server-dedup-host --output-on-failure -R 'test_tilexr_ep_server_dedup_|test_tilexr_ep_kernel_launch'
python3 tests/ep/integration/test_server_dedup_chain.py
python3 -m unittest discover -s tools/validation -p 'test_chain.py'
python3 -m unittest discover -s tools/validation -p 'test_workflow.py'
```

Host 测试构建包含现有 profiling OFF/ON 替身；这次入口和 wire 共同变更，两种入口均应检查。
按 [EP 验证指南](../EP_VALIDATION_GUIDE.md)在已配置 blue 环境尝试真实 CANN 9.1/A5 编译，
构建目标包括 tilexr_ep_dispatch_memory_server_dedup_kernel 和 tilexr-ep；如改动触及 profiling 接线，
按指南使用分离构建目录检查 OFF/ON。blue 不可达或工具链不全时保留实际失败，不用替身结论替代。

文档完备性结论：生产侧输入/输出、字段来源、地址公式、角色变更、写出归属与完成边界均有明确规则，
可以开始代码修改。API 具体调用与异步行为仍须通过上述编译/测试和后续真实设备验证，
文档审查不构成实现正确性证明；所有确认项的代码和验证结果需在实施后登记。
