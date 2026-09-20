# EP Memory Server Dedup Combine 设计

## 设计推进方式

**M1–M5 的用户细化补充已完成，当前进入整体通读、待确定项分级与收口阶段。**

保留已确认的模块行为，补全跨模块衔接；各模块末尾维护待确定项、推荐设计及依据，文末统一汇总。
M0 接线/布局及 M6 调度/收尾也纳入核对，避免模块各自成立而整体链路不闭合。
细化补充完成不等于所有实现参数已确定，也不代表 Combine 已实现或通过设备验证。
本轮只维护 Combine 设计确认；Dispatch 控制小表固定化及实现修改留待独立专题。
继续补充时沿用文末 CB 编号；已确认条目保留锚点，部分确认条目只讨论其剩余内容。

推荐设计均为本轮建议，未自动转为用户已确认项。优先核对高风险或无参考项；有参考、风险
较低的条目可倾向推荐方案。此前 F01–F10、C01–C24 仅保留历史跳转，当前待确定项使用 CB 编号。

本文维护 Combine 内部流程、同步和资源；五表 ABI、公共接口及共同通信域契约由
[联合设计](ep-memory-server-dedup-dispatch-combine-design.md)维护。内部待确定项不迁回联合文档。

## 已确定的整体结构

总核数固定为 **64 核**。所有核先依次执行两个独立函数：`Init`、`CopyLocalPendingTokens`。
后续分为 Send 核和 Read 核；本文 Read 核与讨论中的 Recv 核是同一组核。
`recvCoreNum=8*N（N=1..6）`，即 8/16/24/32/40/48 核；`sendCoreNum=64-recvCoreNum`。
初版按前 sendCoreNum 核为 Send 核、后 recvCoreNum 核为 Read 核组织，最后 8 核（56..63）负责 M2 通知。

Send 核直发类型 1；Read 核处理类型 2，读取其他成员、加上本地贡献，加权求和后发送 partial。
类型 3 在前置拷贝函数中放入本卡共享中转区，供相应 relay 读取。
最终本卡 BS 个 Token 的接收、归约和输出是独立模块，首版由 Read 核负责，设计上保留 64 核参与能力。

公共接口沿用 Memory 原 24 参数，只追加 tokenType、relayReadIndex、sourceMask。
表 2 复用 assistInfoForCombine，表 5 复用 expertScales；有效 expert 行数继续取 sendCounts 末项。
expertOut 与 Dispatch 输出行序一致；最终 source 任务按本卡 BS 组织。

**首版仅实现非量化 Combine。** 保留公开 quantMode 参数及既定 tiling 字段，首版只接受
`quantMode=0`，Host 在提交前拒绝其他值。通信量化、解量化和再量化不进入首版 kernel、
UB 预算或验收范围，相关问题集中到[附录 A 遗留项](#deferred-quantization)，后续决定支持时再讨论。
表 3/5 的 FP32 路由权重仍为必需；expertScales 是表 5 的复用接口名，不是量化 scale。

沿用已确认的模型调度契约；通信布局按 CB00-02 更新为两个控制区加共用数据区的整体双 buffer。
中转区与结果接收区属于同一通信 buffer，中转区紧接结果接收区之后；共同 offset/容量由联合设计维护。

## 已确认项通读补全

| 范围 | 已确认契约及跨模块约束 |
| --- | --- |
| 接口与有效范围 | Combine 参数内容保留 Memory 24 参数并追加三表，Host/GM TilingData/launch 形式参照 Dispatch Server Dedup（CB00-01）；TokenNum 取 sendCounts[E−1]，五表有效到 Combine 完成 |
| 索引身份 | 表 1–3 按 expertOut 的 Dispatch 输出 row，表 4/5 按本卡原始 TokenId；密排序号、row、中转 slot、source TokenId 分开使用 |
| 均衡 | M1b/type3 按 64 核，M3/type1 按 Send 核，M4/type2 按 Read 核，M5/BS 按参与核均分；不可整除时相差至多 1，不按成员数重新划分 M4 |
| 局部表缓存 | 表 2 行距 12B、表 3 行距 K*8B；区间搬运可带入无效行，但不解析或消费无效条目；表 3 仅消费 type2 第 0..N 块 |
| 首版加权范围 | 仅支持有权重输入；表 3 有效 self/memberWeight 和表 5 为必需，不支持以空指针关闭加权；无权重模式延后设计（CB00-03 已确认） |
| 未实现特性 | 只接受关闭配置，Host 拒绝开启未实现的 active mask/TP/共享专家等特性，不静默忽略；Combine 首版只接受 quantMode=0，其他模式归附录遗留项 |
| 结果 Flag 清理 | M5 本核所有 Token 聚合任务完成后，尾部每 Token 一次 DataCopyPad 清全部 K*N 个 Flag 区，不筛选 mask；共用零值 UB，Token 间无同步，M6 确认写回完成 |
| 控制区清理 | Host 在首次使用前清零两份完整 Combine 控制区；kernel 由消费核用后立即清零，M6 确保清理完成；覆盖核对见 CB1A-02 |
| 加权职责 | M1b 暂存未加权结果，M3 发送未加权 DIRECT；M4 使用表 3 的 self/memberWeight，M5 使用表 5；partial 的最终权重为 1 |
| 成员寻址 | M4 第一跳自身取 expertOut[row]；其他成员含同卡成员均取 stage，用表 2 source 和表 3 expert/q 定位；自身只计一次 |
| 空职责核 | 全 64 核均发布 M1b 完成，最后 8 核均履行 M2 通知；TokenNum=0 不代表该卡无 BS 接收任务，不能跳过控制协议 |
| M5 最新方案 | 按 mask 置位 k 搬运，UB 按 32B Flag 块密排，统一等待后一次首 float 求和；完整性采用用户确认的总线保证，阈值 N*popcount(mask) |
| 控制小表 | Combine 控制区固定 52KiB；prepareDone/serverReady/扩展预留的 offset 固定为 0/2048/26624，预留跨度为 2/24/26KiB，均 512B 对齐；分核变化只改变有效范围 |
| 通信布局 | Dispatch 控制区 → Combine 控制区 → 208KiB 留空 → 共用数据区；数据起点固定为每份 buffer 的 +1MiB（1048576B），均 512B 对齐，前后半区各一份，步长为 peerMemBytes/2；数据区大小取 Dispatch/Combine 需求最大值，Combine 中转区接在结果接收区之后（CB00-02） |
| 复用边界 | 模型顺序不变，不重开跨轮重叠、新公共轮次参数或额外读完握手；M2 每个整体 buffer 内仅一份完成 Flag，随本次 Combine 选窗使用（CB02-02 已确认） |

实现中将 M4 的 N 命名为 memberCount，M5 的 N 命名为 flagBlockCount；M2 分核的
recvCoreNum=8*N 中 N 仅是核数倍数，三者不同。expertOut 输入仍为 FP16/BF16，
首版 quantMode 固定为 0。stage 已确认使用原始 BF16/FP16、512B 对齐行，不带 DataAsFlag；
结果槽已确认采用 DataAsFlag，DIRECT/partial 同次调用须统一可解码，不包含量化 scale。
非量化返回 dtype、累加及舍入细节仍由 CB03-02/CB04-03 收口。
stage 与结果区同时存活，二者的 stride 不可混用。

## 模块边界：M0–M6

M1 拆为两个独立函数 M1a/M1b。各模块的功能预期如下；模块编号表示设计职责，
具体运行时调度由 M6 串联。

| 模块 | 作用范围 | 功能结果 |
| --- | --- | --- |
| M0 接口接线与公共布局 | Host / 公共配置 | 为所有模块提供一致的参数和内存视图 |
| M1a Init | 全部 64 核 | 初始化每核执行上下文和资源 |
| M1b 本地待聚合 Token 拷贝 | 全部 64 核 | 类型 3 行密排后均分，数据写入本卡共享中转区 |
| M2 本卡汇总与 Server ready | 核 56..63 通知，每个 Read 核独立观察 | 两级求和确认本 Server 中转数据就绪，每个整体 buffer 内仅一份卡间 Flag |
| M3 DIRECT 返回 | Send 核 | 类型 1 行密排后按 Send 核均分，数据直接返回原始 source |
| M4 读取、局部聚合与 partial 返回 | Read 核 | 类型 2 行密排后按 Read 核均分，本地贡献和成员贡献合为一份返回结果 |
| M5 最终本卡 Token 聚合 | 首版 Read 核，可扩展为 64 核 | 本卡 BS 个原始 Token 的 yOut |
| M6 调度、资源衔接与整体结束 | 全部 64 核 | 串联模块、推进任务并完成所有核的责任 |

### M0 接口接线与公共布局

**功能预期：** 将已确认的公开参数传递到内部配置、tiling 和 kernel，为各模块提供统一的
shape、五表、expertOut/yOut、通信地址与布局信息。公共签名在联合设计维护，此模块负责内部接线。

**已确认项：** 接口、五表映射不变；stage 为现有通信 buffer 的共享可读区，source 结果槽按原始 TokenId/K 组织；整体双 buffer 的区域顺序及容量按 CB00-02。

<a id="cb00-01"></a>
**CB00-01 已确认：Combine 参数语义，Dispatch Server Dedup 接口组织形式。**

公开接口内容沿用已确认的 Combine Memory 原 24 参数加三表，保持 Combine 的输入、输出、
五表和计数语义。Host 封装、标量配置传递和设备入口的形式以当前 Dispatch Server Dedup
为直接参考：公开入口仍接收业务参数，Host 将设备所需标量组织为独立 GM TilingData。
调用方不新增 tilingData 参数，不要求按 Dispatch 的字段名称或结构大小组织 Combine 参数。

| 层次 | Combine Server Dedup 已确认的组织方式 | 直接参考 |
| --- | --- | --- |
| 公开接口 | 维持联合设计的 27 参数；expertOut/yOut、sendCounts、五表均按 Combine 语义 | Combine Memory 参数内容、联合五表映射 |
| Host 参数封装 | 独立 dedup 参数结构承载 EpCombineParams 与 tokenType/relayReadIndex/sourceMask，保留原参数及支持范围校验 | Dispatch 的 EpMemoryServerDedupParams 内嵌 EpDispatchParams 并追加表指针 |
| 校验与上下文 | 基础参数校验→获取 Host CommArgs→dedup 配置/布局校验→获取设备 CommArgs→构造 tiling 并提交 | DispatchServerDedup 的独立 Host 链路 |
| TilingData | 独立 Combine tiling 结构承载 BS/H/TopK/专家数、dtype/quantMode 等设备所需标量；配置项在其决策收口后加入 | EpServerDedupTilingData、Dispatch launch 构造过程 |
| GM tiling 获取 | 使用通信域现有 TileXRCommGetKernelTiling 获取设备侧 tiling 地址，按现有资源管理方式保证其有效期 | TileXREpLaunchDispatchMemoryServerDedupKernel |
| kernel launch ABI | 以 CommArgs 地址、输入/输出及五表地址、tilingData 地址和独立 magic 为主，不沿用普通 Combine Memory 的逐项标量 launch 参数布局 | EpServerDedupKernelArgs |
| 设备入口 | 每核在入口/Init 读取一次所需 tiling 字段并保存执行上下文，再进入 M1–M6；固定提交 64 核 | 当前 Dispatch dedup kernel 入口 |
| 编译与加载 | 独立注册 Combine dedup kernel，使用既有纯 AICore binary 安装/注册/launch 方式 | Dispatch dedup Register/Submit 链路 |

**标量传递边界：** BS、H、TopK、moeExpertNum、dtype、quantMode 纳入 Combine tiling（首版 quantMode 固定为 0）；
Host 仅用于校验的配置无需机械传入 kernel。rank/rankSize/peer 映射以 CommArgs 为依据，
TokenNum 仍在设备端取 sendCounts[E−1]，不能在 Host 构造 tiling 时额外同步读回该计数。
stream/comm 为 Host 提交与资源管理信息；magic 参照 Dispatch 保留为独立 launch 标量。
若分核选择采用运行时配置，recvCoreNum 也通过 tiling 传递；其选择规则归 CB06-03。
M2 使用本次 Combine 的整体 bufferIndex，不另设 bank 状态或 tiling 字段；步长从 CommArgs.peerMemBytes/2 取得。

**内部 ABI 落实：** Combine tiling 使用 Host/device 共用定义，字段先校验再收窄，保留字节
初始化为零；以 sizeof/offsetof/标准布局断言和 launch 测试约束参数布局。Dispatch 当前
32B tiling、120B launch args 仅是参考实例，Combine 不照搬其专属输出字段、保留字段或总大小。
CB00-02 的公共布局已确认；最终字段表、位宽随 CB03-02/CB04-03/CB06-03 等实现配置收口后落实；本项的接口组织方向已确认，
无需再次作为待定架构方案，但实现仍须验证公开签名、GM tiling 读取与注册入口参数一致。

具体源码见 [Dispatch dedup Host](../../src/ep/host/tilexr_ep_memory_server_dedup_dispatch.cpp)、
[参数封装](../../src/ep/host/ep_memory_server_dedup_host.h)、
[tiling 定义](../../src/ep/common/ep_memory_server_dedup_tiling.h)、
[launch 构造](../../src/ep/host/ep_memory_server_dedup_launch.cpp)、
[launch ABI](../../src/ep/host/ep_memory_server_dedup_launch.h)和
[设备入口](../../src/ep/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.cpp)。

<a id="cb00-03"></a>
**CB00-03 已确认：首版必须有权重，未实现特性只接受关闭配置。**

表 3 有效 self/memberWeight 和表 5 必须提供，不支持 nullptr 关闭加权；无权重模式延后。
公开接口保留的 active mask、TP、共享专家等特性，在尚未实现时只能传入对应的关闭配置，
Host 在提交 kernel 前拒绝启用，不静默忽略或降级。具体关闭值沿用各参数语义，在 Host 校验中显式列出。
首版量化范围亦已确认：仅接受 quantMode=0，量化功能不实现，不静默回退。
量化相关配置和实现问题统一移至附录 A，不再作为首版 CB03-02/CB04-03 的待定内容。

<a id="cb00-02"></a>
#### CB00-02 展开：通信内存归属、槽格式与容量

**已确认。** 共用通信域的权威布局及完整公式见[联合设计 §6](ep-memory-server-dedup-dispatch-combine-design.md#6-共用通信域的布局cb00-02-已确认)。
本节维护对 Combine 模块的约束；C1 已实现 Host/tensor 固定布局与显式 Sresult 地址助手，
C2 已接入空 kernel/注册（16B GM tiling、88B launch、64 核），真实数据区使用和完整容量校验待第二阶段；Dispatch 的布局改造由独立专题维护。

| 用户确认项 | Combine 内部落实 |
| --- | --- |
| 中转区接在结果接收区之后 | 两区复用同一 peer-memory allocation 和 peer 映射，以 offset 划分；M1b/M4 使用 stageOffset，M3/M5 使用 resultOffset |
| 中转数据保持原始 BF16/FP16 | 不量化、不加权、不附加 DataAsFlag；每 Token 行 stride=alignUp(H*2,512)，行尾只用于补齐 |
| 中转行数与 Dispatch 接收槽一致 | stageRows=localMoeExpertNum*rankSize*batchSize=E*BS；表 2 offset 与表 3 解出的 slot 使用相同物理行号，不使用密排 expertOut row |
| 结果槽 | BS*K 个槽，每槽大小为返回 Token 完成 DataAsFlag 拼接后的总字节数；非量化 payload/Flag/补齐均计入，不含量化 scale；返回 dtype、数值与尾块细节仍归 CB03-02/CB04-03 |
| 控制区及共用数据区 | 两个控制区由公共固定宏划分为 764KiB/52KiB（含扩展预留），随后留空 208KiB，数据起点为 1MiB，均 512B 对齐；数据区容量取 max(Dispatch 接收区大小, Combine 接收区+中转区大小)，内部控制细节互不依赖 |
| 整体双 buffer | 前后半区各保留上述三个区域，第二份基址为 peer[rank]+peerMemBytes/2；每份 Combine 控制区只放一份 M2 完成 Flag（CB02-02 已确认） |
| 容量检查 | 两份布局分别放入对应半区；统一检查 singleBufferBytes<=peerMemBytes/2-runtimeReservedBytes，不能仅比较两份需求总和；不足时报错，等于时允许，uint64 运算检查溢出 |

控制小表 offset 已固定：prepareDone=0、serverReady=2048、扩展预留=26624，均相对 Combine
控制区起点且 512B 对齐，每张表由独立宏定义并按最大规模预留；详见[联合设计 §6.5](ep-memory-server-dedup-dispatch-combine-design.md#65-控制小表的固定-offset)。
初始化/清理依赖由 M1a/M1b/M2 继续设计；Combine 独立校验内部使用量
不超过 52KiB 公共宏，无需感知 Dispatch 控制布局。按当前设计，单份最大 26KiB、预留一倍
余量后的宏为 52KiB；完整核算见[联合设计 §6.4](ep-memory-server-dedup-dispatch-combine-design.md#64-固定控制区宏及上限核算)。
固定布局与有效记录分开记账，以下均为单份整体 buffer 内、相对 Combine 控制区起点的区间：

| 区域 | 固定 offset | 固定预留 | 本次调用有效范围 |
| --- | --- | --- | --- |
| prepareDone | 0 | 2KiB | 固定 8*64 个 float |
| serverReady | 2048 | 24KiB | 前 recvCoreNum 个 512B 块，4..24KiB |
| 扩展预留 | 26624 | 26KiB | 当前无活动表，不是第二份 Flag |

每份 Combine 控制区固定占用 52KiB，两份整体 buffer 共 104KiB；两个 buffer 内 Flag 表的
固定预留合计 52KiB，有效记录范围合计 12..52KiB。有效范围缩小不回收固定区间，也不移动后续表。
prepareDone 整表起点 512B 对齐，表内每个通知核的接收行仍为 256B；serverReady 每核块为 512B。
新控制状态在固定 52KiB 预算内安排独立宏并校验，不能改变当前表边界。

M0/Init 统一建立以下地址，Host 和设备端引用同一组公共宏；这些是地址契约，不是新接口：

```text
bufferStride = uint64(CommArgs.peerMemBytes) / 2
bufferIndex = uint64(magic) % 2
bufferBase(rank) = peer[rank] + bufferIndex*bufferStride
combineControlBase(rank) = bufferBase(rank) + COMBINE_CONTROL_OFFSET
prepareDoneBase(rank) = combineControlBase(rank) + COMBINE_PREPARE_DONE_OFFSET
serverReadyBase(rank) = combineControlBase(rank) + COMBINE_SERVER_READY_OFFSET
SHARED_DATA_OFFSET = TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET  // 固定 1048576B
resultBase(rank) = bufferBase(rank) + SHARED_DATA_OFFSET
stageBase(rank) = resultBase(rank) + alignUp(uint64(BS)*K*Sresult,512)
Sstage = alignUp(uint64(H)*2,512)
```

因此结果区在 buffer 0 的地址为 `peer[rank]+1MiB`，在 buffer 1 为
`peer[rank]+peerMemBytes/2+1MiB`；中转区只在该结果区后加上对齐后的接收区大小，
不再额外加一次控制区或 1MiB。1MiB 为 1048576B（0x100000），不是 1000000B。
控制区合计 816KiB，后面 208KiB 保持留空；不纳入 Combine 的 52KiB 控制预算或 stage。

上述大写名称简写自联合设计中 TILEXR_EP_SERVER_DEDUP_ 前缀的对应宏；不把 offset 或
单份控制区大小重新按 BS/分核计算。Sresult 的含义已确认是完整结果槽字节数，不是一个
独立任选的大小；CB03-02/CB04-03 只收口首版非量化返回 dtype 和包块组织，
不会因已移至附录的量化格式问题阻塞首版设计。
同一调用各 rank 使用一致的布局配置，peer 基址与 bufferStride 均须满足 512B 对齐。
Combine 可独立检查自身数据视图能放入半区，无需解析 Dispatch 控制区；共同通信域的
max(Ddata,Cdata) 容量契约仍保留。stage 与结果槽同时存活，M3 返回不能覆盖 M4 尚在读取的 stage。

**Sresult 的推导与示例假设：** 按当前 Memory DataAsFlag 参考布局，每个包块为
480B 有效承载区加 32B Flag，槽间无额外空隙。令 P 为插入 Flag 前需要承载的字节数，
首版仅计非量化通信 payload 和协议要求的内部补齐，不含量化 scale、Flag 及最后一个
480B 块的补齐，则：

```text
N = ceilDiv(P, 480)             // 即 flagBlockCount
Sresult = N * 512               // 最后不足 480B 的承载块也占一个完整包块
```

下例进一步假设 DIRECT 和 partial 均以 H 个 BF16/FP16 元素返回，且不附加 scale、
包头或其他 metadata，即 `P=2H`，才可化为 `Sresult=ceilDiv(2H,480)*512`。
该公式与当前 Memory Combine 非量化路径一致，见其 Init 中的 commDataBytes、
blockCntPerToken_ 和 hAlignWinSize_；它不是由 expertOut 的输入 dtype 单独决定的。
若 partial 改用其他非量化返回精度，P 必须重新计算，且 DIRECT/partial 同一次调用仍需
统一结果协议；这些非量化选择继续归 CB03-02/CB04-03，不由示例自动确认。
后续量化格式如何计算 P 仅记录在附录 A，不属于当前待确定项。

在上述非量化 BF16/FP16 返回假设下，取 R=128、E=384、BS=256、K=16、H=7168：
P=14336B，N=30，Sresult=15360B（15KiB）。

| Combine 区域 | 计算 | 每份 buffer 空间 |
| --- | --- | --- |
| 结果接收区（上述假设） | 4096 槽 * 15360B | 60MiB |
| 中转区 | 98304 行 * alignUp(14336,512)B | 1344MiB |
| Combine 数据需求 | 接收区 + 中转区 | 1404MiB |

上述数值用于该 BF16/FP16 返回假设下的首版容量示例；最终预算随非量化返回格式确认。
完整需求仍须取 max(Ddata,1404MiB)，再加共用数据区之前的固定 1MiB（含控制区及留空），
得到 singleBufferBytes，再分别核对两个半区容量及 runtime 保留边界。不能将 1404MiB 直接视为单份共用数据区的最终值。
在本例假设下，相对所选 buffer 的结果区为 [1MiB,61MiB)，中转区为 [61MiB,1405MiB)。
1405MiB 仅为 Combine 视图含前缀的需求；若 Ddata 更大，完整共用布局仍取较大值。
旧 relayBytes=8*BS*K*returnStride 是历史预留，不能代替新的中转区。

验收需验证 Host/Dispatch/Combine 的区域布局一致且步长均为 peerMemBytes/2、表 2 写槽与表 3 读槽一致、
两个 buffer 以及各内部区域不重叠、DataAsFlag 与 stage 行 stride 区分正确、最高地址不侵入保留区。
当前 run.sh chain 每卡显式分配 8GiB 逻辑通信空间，分配源码及通用 API 默认值见联合设计 §6.3。

### M1a Init

**功能预期：** 全部 64 核执行 Init，建立本核参数、地址、状态和资源；从 sendCounts 末项取得
TokenNum，为后续本地拷贝及 Send/Read 处理提供执行上下文。

**已确认项：** 全 64 核执行 Init，在设备侧读取计数末项，建立角色、固定宏地址、本次整体 bufferIndex 及资源上下文。
控制区采用 Host 首次清零、kernel 消费方用后立即清零；Init 不让 64 核重复清理共享 GM。
CB1A-01 的跨模块 UB 复用暂缓，待所有模块 UB 用量设计明确后统一讨论。

#### 控制 Flag 清零覆盖核对（CB1A-02）

Host 清理必须在任何 rank 发布本轮 Flag 前完成，覆盖两个整体 buffer 内各自完整的
52KiB Combine 控制区，包含 serverReady 的全部 48 个预留块，而非只清本次有效块。
现有 [TileXRComm::InitMem](../../src/comm/tilexr_comm.cpp) 在分配后通过 aclrtMemset
清零整个 allocation，随后 InitCommMem 才交换并打开 peer 映射；新建通信域可复用这次初始化。
正常复用不要求 Host 每轮清零；已用通信域若需要重新初始化，必须先确保旧读写全部结束，
不能与 kernel 的发布并发。异常退出不能继承“上一轮已清理”的前提。

| 区域 | 发布方 | 消费及当场清零责任 | 覆盖结论与完成条件 |
| --- | --- | --- | --- |
| prepareDone[8][64]，2KiB | 64 核各写 8 行的自身列 | 核 56+j 检测第 j 行 sum=64，保存完成状态后，立即清自身 256B 行 | 8 核覆盖整表；不能提前清部分到达项，也不能由发布核写后自清；零 type3 任务核仍发布，通知核仍汇总并清理 |
| serverReady，固定 24KiB | Server 内 8 卡分别写每个有效 Read 块的来源槽 | Read 核检测自身块 sum=8，保存 ready 状态后，立即清该 512B 块；M4 使用本核保存的状态 | 每个有效块唯一 owner；无 type2 或 BS 任务的 Read 核也必须等待并清理；不再依赖被清掉的 GM 值 |
| 扩展预留，26KiB | 当前无写入方 | Host 初始化为零，kernel 无活动记录可消费 | 不需要每轮清理；未来新增控制表必须同时定义发布、消费、清理 owner 和完成依赖 |
| M5 结果槽 DataAsFlag（位于数据区） | M3/M4 的远端返回 | 本核所有 Token 聚合任务完成、相关 payload 读取结束后，单独清理本核 Token 的全部 K 槽、全部 Flag 区 | 尾部全槽清理已确认，不依赖 mask；首次初始化和布局变化仍归 CB1A-02，完成依赖归 CB05-03/CB06-02 |

**控制区覆盖结论：** 在正常完成及既定模型调度下，上述两类活动控制 Flag 可完整覆盖。
recvCoreNum 变化时，新启用的块要么从未使用、保持 Host 初始零值，要么以前已由其 owner 清零；
本次不活动的块没有写入方，无需额外清理。切换整体 buffer 采用相同原则，不新增内部双 bank。
“当场清零”指消费后立即发起清理；UB 清零源在 MTE3 写完前不能复用，且 M6 必须保证这些
清理实际完成后才结束。具体 Event/流水依赖仍由 CB02-03、CB06-02 细化。

**结果 Flag 初始化/跨布局复用列为重点遗留，当前不展开。** 用户确认每次测试前新建通信
buffer 并清零，首次结果 Flag 的零值由整块初始化提供；第一阶段空 Combine 不发布或消费
结果 Flag，本项不阻塞当前功能接入和验收。后续未重新清零的通信域复用、BS/K/H 或数据
角色变化等边界保留在[附录 B](#deferred-result-flags)，待处理遗留问题时单独展开。
这不改变已确认的 kernel 用后清理职责，不增加每轮 Host 清零或新握手。

**待确定项：** 分级见[规则](#待确定项分级规则)，推荐均待确认。

| 编号 | 风险 / 参考 | 待确定内容 | 推荐设计 | 依据与核对重点 |
| --- | --- | --- | --- | --- |
| <a id="cb1a-01"></a>CB1A-01 | 高 / 部分 | 各角色 UB/Event 总预算及生命周期 | **暂缓讨论：待所有模块 UB 用量设计明确后，再统一确认跨模块复用。** 届时列出 M1b/M3/M4/M5 的固定区、索引、表缓存、Flag/work、累加区和 lane 的同时存活图；lane 由剩余 UB 及 Event 计算，保持合法输入至少一 lane 的前提。不得在在途 DMA 未完成时复用空间。 | [R3](#参考依据目录)/[R4](#参考依据目录)；有动态 lane 参考，新模块组合的最坏 H/K/行跨度需重新核算。 |
| <a id="cb1a-02"></a>CB1A-02 | 高 / 部分 | Host 初始化覆盖与结果 Flag 复用边界 | **已确认方向：Host 首次清零、kernel 消费后当场清零。** 当前测试每次新建并清零通信 buffer，落实该初始化接线；结果 Flag 跨布局/已用域复用为附录 B 重点遗留，不阻塞第一阶段。两类控制 Flag 的 owner 和范围见上表，M2/M6 清理完成依赖分别归 CB02-03/CB06-02。 | [R3](#参考依据目录)及 InitMem 整块 aclrtMemset 为参考；控制区覆盖分析不等同于新 Combine 清理时序已设备验证。 |
| <a id="cb1a-03"></a>CB1A-03 | 中 / 直接 | Reset、寄存器与 Event 初始状态 | 推荐 Reset 前排空本核操作，之后重建事件及非量化路径所需寄存器状态；空闲 lane 初始 Event 设置一次，M6 统一回收。 | [R3](#参考依据目录)/[R4](#参考依据目录)/[R9](#参考依据目录)；核对非量化路径的每阶段资源恢复点；量化专用状态移至附录 A。 |

### M1b 本地待聚合 Token 拷贝

**功能预期：** 全部 64 核共同执行 CopyLocalPendingTokens，先通过 Compare 的等于比较筛选
类型 3，再通过 GatherMask 压缩原始行号，得到无空泡的密排行偏移表。每核按密排后类型 3 的
总数分得连续的一段，将该段对应的未加权 expertOut 拷贝到本卡共享中转槽。
加权求和由 M4 执行，本核拷贝完成状态交给 M2 汇总。

**筛选与密排：** 每核对本卡同一有效范围 `[0,TokenNum)` 完成筛选，得到相同的逻辑密排结果；
实际 Token 数据只搬运本核负责的部分。筛选阶段不先按原始行号切成 64 份，任务均分以筛选后的
类型 3 数量为依据。

以下为语义示意，非具体 API 调用原型：

```text
rowIndex = [0, 1, ..., TokenNum-1]
mask3 = Compare(TokenType[0:TokenNum], 3, EQ)
packedRows, type3Count = GatherMask(rowIndex, mask3)
```

GatherMask 使用比较 mask 筛选 rowIndex，输出的是原始行号；直接筛选 TokenType 只会得到
一串 3，不能作为拷贝索引。packedRows 的有效范围为 `[0,type3Count)`，按原始 row 顺序密排，
没有无效占位。若分批执行筛选，各批结果按扫描顺序逻辑拼接，核间分配按总 type3Count 计算。

**按 64 核均分：** 令 `C=type3Count`，核号 `c∈[0,64)`，使用商和余数分配连续区间：

```text
base = C / 64                  // 整数除法
extra = C % 64
count[c] = base + (c < extra ? 1 : 0)
begin[c] = c*base + min(c, extra)
end[c] = begin[c] + count[c]    // 只处理 packedRows[begin[c]:end[c]]
```

全部类型 3 Token 恰好分配一次，各核搬运数量最多相差 1。例如 C=100 时，前 36 核各处理
2 个，其余 28 核各处理 1 个。C<64 时部分核无拷贝任务，仍履行本核准备完成责任。

**拷贝索引：** 对本核区间内的密排下标 i，取 `row=packedRows[i]`，从 expertOut[row] 读取，
再以表 2 `DestinationIndex[row][0]` 中的 offset 定位本卡中转槽。packedRows 保存的行偏移
以 Token 行为单位，不是字节偏移；密排下标 i、原始行号 row、中转槽 offset 三者分别使用。
无空泡描述的是任务索引表，不改变已约定的中转槽编号。该表属于 M1b 临时调度数据，不新增公共输入。
原始 expertOut 每行有效 H*2B，目标 stage 槽 stride 为 alignUp(H*2,512)。拷贝只消费 H 个
原始元素，行尾补齐不参与 M4 加权；不能因为 stage 有补齐就从 expertOut 的下一行多读。
目标地址为 stageBase(self)+uint64(offset)*Sstage，其中 stageBase 由 M0 公共布局取得。


当前 Memory Dispatch 的 [TokenActiveMaskCal](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h)
已有比较 mask、CreateVecIndex 和 GatherMask 压缩行号的组合，可作为实现参考。
[官方 GatherMask 文档](https://asc.gitcode.com/api/SIMD-API/basic_api/memory_vector_compute/compare_and_select/GatherMask.html)
说明该操作按 pattern 筛选源元素，rsvdCnt 给出保留数量；本设计以行号序列作为源元素。
官方检索脚本未返回 9.1.0 的对应页面，上述链接用于语义参考，不代表已验证目标重载；
具体数据类型、分批参数及资源排布在本模块继续细化。

**本核完成发布：** 本核全部类型 3 payload 拷贝提交后，执行 `PipeBarrier<PIPE_MTE3>()`，
在 MTE3 流水上建立 payload 写入先于完成 Flag 写入的顺序，随后用一次多块 DataCopyPad
发布本核完成状态。Flag 暂定为 FP32 `1.0f`；无拷贝任务的核也发布完成。

本卡 GM 控制区中的 prepareDone 表固定起点为 Combine 控制区偏移 0（独立 offset 宏），
固定预留 2KiB，表内划出 8 个接收区，每区 64 个槽。逻辑布局为 `prepareDone[8][64]`，
接收区 j 对应 M2 通知核 `coreId=56+j`，槽 c 对应全局 coreId=c。每核写入 8 个区的同一列：

```text
core c → prepareDone[0][c], prepareDone[1][c], ..., prepareDone[7][c]
```

这 8 份完成记录都位于本卡 GM，供本卡 8 个通知核各自汇总；向 Server 内各卡发送整卡 ready
由 M2 后续完成。M1b 本轮按每槽 4B 的紧凑布局描述，每区 256B，总容量 2048B：

```text
flagAddress(j,c) = prepareDoneBase + (j*64+c)*sizeof(float)
```

**一次 DataCopyPad 写 8 槽：** 采用 UB→GM 的 Normal 搬运布局，UB 源准备 8 个 32B 块，
每块首个 float 为 1.0f。源端预留 256B，并在使用前建立其初始化到 MTE3 读取的依赖。
对本核 c 的参数和调用示意如下，prepareDoneGm 为 float 类型 GlobalTensor：

```cpp
// flagUb 已就绪，各 32B 块首个 float 均为 1.0f。
// blockCount=8，blockLen=4B，UB 块间额外间隔=0，GM 块间额外间隔=252B。
AscendC::PipeBarrier<PIPE_MTE3>();
const AscendC::DataCopyExtParams flagCopy{8U, 4U, 0U, 252U, 0U};
AscendC::DataCopyPad(prepareDoneGm[coreId], flagUb, flagCopy);
```

GM 相邻写块的起点距离为 `4+252=256B`，因此每块恰好落入下一个接收区的本核槽。
Normal 模式下，UB 相邻块起点相距 32B；srcStride=0 表示额外空隙为零，源端不是紧排 8 个 float。
Flag 源缓冲保持有效到这次 MTE3 搬运完成，M1a/M6 在资源初始化和复用中承接该生命周期。

[官方 UB→GM DataCopyPad 文档](https://asc.gitcode.com/api/SIMD-API/basic_api/memory_vector_compute/data_move/DataCopyPad_UBToGM.html)
提供上述非对齐块长和步长单位的语义依据。该页面为开发分支文档；官方检索脚本未返回 9.1.0
对应页面，示意参数尚未经过目标 CANN 编译及设备验证。

**已确认项：** 全范围筛选 type3，密排后按 64 核均分，按表 2 offset 暂存；MTE3 顺序发布八份本核完成记录。

**待确定项：** 分级见[规则](#待确定项分级规则)，推荐均待确认。

| 编号 | 风险 / 参考 | 待确定内容 | 推荐设计 | 依据与核对重点 |
| --- | --- | --- | --- | --- |
| <a id="cb1b-01"></a>CB1B-01 | 中 / 部分 | 大 TokenNum 的分批筛选与密排缓存 | 推荐分批 Compare/GatherMask；先统计全局类型总数确定本核密排区间，再扫描并仅保留该区间对应行，必要时继续分批处理。M3/M4 共用选择组件，保持全局均分和原始行顺序。 | [R5](#参考依据目录)；Crows=98304 的全行号列表占 384KiB，不能假定整表常驻每核 UB；两遍方案需验证批边界。 |
| <a id="cb1b-02"></a>CB1B-02 | 中 / 直接 | 本地拷贝 lane 与表 2 获取 | 推荐表 2 按本核 type3 行号范围分批缓存；MTE2/MTE3 环形 lane 按 UB 配置，复用槽前等待 MTE3 释放。沿用已确认的 stage dtype/stride，尾部搬运不越过 expertOut 有效行，见 CB00-02。 | [R3](#参考依据目录)/[R4](#参考依据目录)；沿用缓冲所有权模式，核对尾块及发布前排空。 |
| <a id="cb1b-03"></a>CB1B-03 | 高 / 部分 | 64 核紧凑 4B 完成记录的精确发布 | 保留 prepareDone[8][64] 和一次 DataCopyPad 写八槽；确定源 UB 初始化依赖、4B blockLen/252B dstStride 的目标重载与 MTE3 屏障。验证各核精确写自身 4B、不覆盖邻槽，payload 先于通知可见。 | [R7](#参考依据目录)；文档支持参数语义，完整 64 核新协议尚无设备证据。M5 首 float 保证不替代此处地址/顺序核对。 |

### M2 本卡汇总与 Server ready

**功能预期：** 最后 8 个核（全局 coreId 56..63）各自汇总本卡 64 核的类型 3 拷贝完成记录，
sum=64.0f 后分别通知 Server 内的 8 张卡（包括本卡）。每个 Read 核独立监听本卡属于自己的
512B 卡间同步块，提取 8 个来源卡的完成 Flag，sum=8.0f 后进入 M4，并独立清理自己的 Flag 区。
每个整体通信 buffer 内仅一份卡间同步记录，随本次 Combine 所选整体 buffer 使用，见下文 CB02-02。

**核映射与本卡汇总：** `recvCoreNum=8*N（N=1..6）`，剩余核作为 Send 核。
初版的 Read 核范围为 `[64-recvCoreNum,64)`，故核 56..63 始终位于 Read 组末尾。
通知序号 `j=coreId-56`，范围 0..7；它在 Read 组内的编号为 `recvCoreNum-8+j`。

```text
通知核 56+j → 本卡 prepareDone[j][0:64] → sum=64.0f → 通知本 Server 的卡 j
```

每个通知核只读取 M1b 为自己准备的那份 64 槽记录，对本次读到的 64 个 FP32 值求和。
全部核各写入 1.0f 后，和为 64.0f，表示本卡待聚合 Token 已写入本地中转区。
通知核消费并清理自己的 prepareDone 接收区；其他通知核使用各自独立的副本。
本卡完成通知的发布不等待其他卡 ready，前述 8 个通知核完成通知责任后再进入卡间监听。

**向 Server 内各卡通知：** 这里的来源编号 s=0..7 是 Server 内卡号（serverLocalRank），
不是跨 Server 的编号。设本卡 s，通知核 56+j 负责目标卡 j，则它将 FP32 1.0f 发布到目标卡
当前整体 buffer 中每个 Read 核对应块的第 s 个槽。每个目标 Read 核都得到一份来自 8 张卡的独立记录。
本卡作为目标也执行同样的通知规则；这一步承接本卡 sum=64.0f 的完成含义。

**单份卡间同步布局：** 目标卡为每个 Read 核提供一个 512B 对齐、512B 步长的独立块。
块内的前 256B 划为 8 个 32B 槽，槽 s 的首个 4B 保存来源卡 s 的 FP32 Flag：

| 来源卡 s | 槽在块内的字节偏移 | 有效内容 |
| --- | --- | --- |
| 0 | 0 | 首 4B 为 FP32 Flag |
| 1 | 32 | 首 4B 为 FP32 Flag |
| 2..7 | 64, 96, 128, 160, 192, 224 | 各槽首 4B 为 FP32 Flag |

每槽其余 28B 和块后半段 256B 不参与完成求和。块按 Read 组内编号 r 密排，
`r=coreId-sendCoreNum`，范围 `[0,recvCoreNum)`；索引及地址关系为：

```text
bufferStride = peerMemBytes / 2
readyReservedBytes = TILEXR_EP_SERVER_DEDUP_COMBINE_SERVER_READY_BYTES  // 固定 24KiB
readyValidBytes = recvCoreNum * 512  // 仅前 recvCoreNum 个块有效
serverReadyOffset = TILEXR_EP_SERVER_DEDUP_COMBINE_SERVER_READY_OFFSET  // 固定 2048B
syncBase(target,bufferIndex) = peer[target] + uint64(bufferIndex)*bufferStride
                             + combineControlOffset + serverReadyOffset
blockAddress(target,bufferIndex,r) = syncBase(target,bufferIndex) + r*512
flagAddress(target,bufferIndex,r,s) = blockAddress(target,bufferIndex,r) + s*32
```

serverReadyOffset 相对 Combine 控制区起点，固定为 2048B；recvCoreNum 改变仅影响有效块数，
不改变 offset 或 24KiB 预留跨度。bufferIndex 是本次 Combine 所选整体 buffer。
通知核 56+j 对目标 j 的所有 r 写同一个来源槽 s，
每个来源槽由对应来源卡唯一写入；接收方按自己的 r 监听。

**每核监听与清理：** 每个 Read 核只监听本卡当前整体 buffer 内的自身 512B 块，
每次取 8 个 32B 槽的首个 float 求和。sum=8.0f 表示本 Server 全部 8 卡的类型 3 数据已准备完成，
本核可以读取中转数据。各核自行消费、清零自己监听的 Flag 区，M6 承接清理写入完成与资源回收；
核 56..63 也按其 Read 组编号监听自己的块。512B 独占块及清理职责不会因 M5 后续全核参与而扩大到 Send 核。

<a id="cb02-02"></a>
**CB02-02 已确认：M2 完成 Flag 随整体双 buffer 复制，不再独立双缓冲。**

每个整体通信 buffer 的 Combine 控制区只保留一份 serverReady，固定预留 48*512B=24KiB，
其中 recvCoreNum*512B 有效。全域两份固定预留 48KiB，合计有效范围为 8..48KiB。
prepareDone 每个整体 buffer 各一份 2KiB，两份共 4KiB；两级 Flag 固定预留共 52KiB，
本次调用的两份有效记录范围合计为 12..52KiB。
单份 Combine 控制区宏为 52KiB（上限 26KiB 的两倍），其他必要控制状态在该预算内安排并校验，
预留余量不作为第二套 Flag，不在单个 buffer 内再次展开 M2 双 bank。

M2 的发布、监听、清理都使用本次 Combine 的整体 bufferIndex，与数据区选择一致。
沿用整体 magic 选窗，不新增 Combine 专用翻转状态；此前“相邻 Combine 必须独立交替 bank”
的描述由本条整体布局取代。连续 Combine 若选到同一半区，仍按既定模型顺序和 owner 清理
完成条件复用，不为满足独立轮换而访问另一份控制区。

两个整体 buffer 内的控制记录均须在首次使用前初始化为 0；Read 核消费后清理本次使用的
512B 块，并在该块再次复用前完成清理。初始化入口和清理依赖分别归 CB1A-02/CB02-03，
物理布局确认不等于这些并发依赖已验证。跨 rank 的 writer/listener 使用一致整体 bufferIndex。

**已确认项：** 56..63 各汇总本卡 64 槽并通知一张卡；Read 核独占 512B，sum=8 后可读 stage 并自行清理；M2 完成 Flag 每个整体 buffer 内一份，随整体选窗使用。

**待确定项：** 分级见[规则](#待确定项分级规则)，推荐均待确认。

| 编号 | 风险 / 参考 | 待确定内容 | 推荐设计 | 依据与核对重点 |
| --- | --- | --- | --- | --- |
| <a id="cb02-01"></a>CB02-01 | 中 / 部分 | 两级求和与通知 fanout 搬运 | 推荐本卡 64 个紧排 float 一次求和，卡间八槽按首 float 求和；通知核多块 DMA 写目标所有 Read 块的同一来源槽，起点相距 512B。源缓冲到写完才复用，4B/32B 写法以目标重载定稿。 | [R3](#参考依据目录)/[R7](#参考依据目录)；有求和及步长搬运参考，新 fanout 需地址覆盖测试。 |
| <a id="cb02-03"></a>CB02-03 | 高 / 部分 | 两级 Flag 消费清理的完成依赖 | 用后当场清零已确认：通知核清自己的 256B prepareDone，Read 核清自身当前整体 buffer 的 512B 块；清理、通知及后续 DMA 分别建依赖，该 buffer 再用前排空。未取得完整 sum 前不清局部已到达槽。 | [R3](#参考依据目录)/[R4](#参考依据目录)；单 owner 清理有参考，新组合需验证快慢核、空任务和整体双 buffer 复用时序。 |

### M3 DIRECT 返回

**功能预期：** Send 核通过 Compare 的等于比较筛选类型 1，再通过 GatherMask 密排原始行号，
按 Send 核数均分 DIRECT 发送任务。各核读取本核任务对应的 expertOut[row]，按表 2 的
source/token/k 发送到原始 source 的结果槽，并发布到达状态。真实路由权重由最终 source 聚合时应用。

**筛选与密排：** 沿用 M1b 的筛选方式，每个 Send 核扫描本卡同一有效范围 `[0,TokenNum)`，
得到相同的逻辑密排结果。先筛选全部类型 1，再划分发送任务，避免原始行区间中的类型分布
不均导致 Send 核任务失衡。Read 核不参与 M3 的任务均分。

以下为语义示意，非具体 API 调用原型；API 语义参考与验证边界沿用 M1b：

```text
rowIndex = [0, 1, ..., TokenNum-1]
mask1 = Compare(TokenType[0:TokenNum], 1, EQ)
directRows, directCount = GatherMask(rowIndex, mask1)
```

directRows 保存类型 1 Token 的原始行号，按原始 row 顺序密排，无空泡；GatherMask 筛选的是
rowIndex，不能将筛选出的类型值 1 作为发送索引。分批筛选时按扫描顺序逻辑拼接，任务划分
使用总 directCount。该索引表为 M3 内部临时调度数据，不新增公共输入。

**按 Send 核均分：** 令 `D=directCount`、`S=sendCoreNum=64-recvCoreNum`，Send 核逻辑编号
`s∈[0,S)`；当前前 S 个核为 Send 核，因此 s=coreId。按商和余数划分密排表的连续区间：

```text
base = D / S                  // 整数除法
extra = D % S
count[s] = base + (s < extra ? 1 : 0)
begin[s] = s*base + min(s, extra)
end[s] = begin[s] + count[s]   // 只发送 directRows[begin[s]:end[s]]
```

D 能被 S 整除时，每个 Send 核的 DIRECT 任务数完全相同；不能整除时，前 extra 个 Send 核
各多处理 1 个，任意两核任务数最多相差 1，全部任务恰好分配一次。例如 S=32、D=100 时，
前 4 核各发送 4 个，其余 28 核各发送 3 个。D<S 时前 D 核各发送 1 个，其余核无 DIRECT
任务；D=0 时全部 Send 核跳过发送，按 M6 完成本核阶段责任。均分约束针对 Token 数量，
不保证不同目的卡的通信等待耗时相同。

**发送索引：** 对本核密排区间内的下标 i，取 `row=directRows[i]`，使用 expertOut[row]
和表 2 的同一行 `DestinationIndex[row]` 取得 payload 与目的 source/token/k。
密排下标 i 仅用于任务分配，不替代原始行号，也不改变目的槽编号。

**表 2 按本核行号范围搬入：** 每核遍历自己负责的 directRows 段，得到需要处理的本卡输入
Token index。由于列表按原始行号递增，非空任务段的首尾元素就是本核的最小、最大行号：

```text
minRow = directRows[begin[s]]
maxRow = directRows[end[s]-1]
```

只将覆盖这些行号的连续表 2 区间搬入 UB，不要求将全表搬入。记实际读取首行为 table2BaseRow，
末行为 maxRow（含），其中 `0 <= table2BaseRow <= minRow`；最小覆盖范围从 minRow 开始，
也可向前扩展读取起点。表 2 每行仍为 3 个 32 位字段，共 12B，不因局部缓存改变布局。
搬入范围内可能包含非 type1 行，这些行只随连续读取进入缓存，不成为发送任务。

例如核 2 的列表段有 16 个 DIRECT Token，最小 index=23、最大 index=66，按本例读取
表 2 的闭区间 `[22,66]`，即 `table2BaseRow=22`，共 45 行、540B 有效表数据。
这里 22 表示额外向前读取一行；最小覆盖区间本身是 `[23,66]`，不约定每次都必须减 1。
后续仍只遍历这 16 个列表元素，以相对于实际缓存首行的 offset 查询：

```text
row = directRows[i]                         // i 属于本核列表段
offset = row - table2BaseRow                // 表 2 缓存内的行偏移
entry = table2UbBase + offset*12            // 字节地址示意
srcRank, srcTokenId, srcTopk = entry 的三个 32 位字段
payload = expertOut[row]
```

本例 row=23 查询 UB 第 1 行（字节偏移 12），row=66 查询第 44 行（字节偏移 528）。
表 2 缓存索引使用原始 row 与读取首行之差，不能使用密排列表中的位置代替。
无 DIRECT 任务的核不读取列表首尾或表 2。跨度过大时按 UB 容量细化分批缓存，
每批保持同一套“连续行范围搬入、列表遍历、相对首行索引”的规则。

**已确认项：** type1 密排后按 Send 核均分，只缓存本核行号范围的表 2，未加权 expertOut 返回 source。

**待确定项：** 分级见[规则](#待确定项分级规则)，推荐均待确认。

| 编号 | 风险 / 参考 | 待确定内容 | 推荐设计 | 依据与核对重点 |
| --- | --- | --- | --- | --- |
| <a id="cb03-01"></a>CB03-01 | 低 / 直接 | 表 2 局部缓存分批与索引 | 推荐按原始行号跨度和 UB 预算切批，缓存首行显式保存，按 (row-baseRow)*12 索引。22..66 仅为示例，不固定减 1；筛选复用 CB1B-01。 | [R1](#参考依据目录)/[R3](#参考依据目录)；三字段表和分块搬运有参考，验证稀疏跨度、末批、12B 行距。 |
| <a id="cb03-02"></a>CB03-02 | 高 / 部分 | 非量化 DIRECT/partial 统一 wire 及发送流水 | 首版只支持 quantMode=0，不带量化 scale。已确认 Sresult 是完整 DataAsFlag 槽字节数；待定非量化返回 dtype、payload/尾块组织和发送依赖。推荐沿用 Memory 的 480B payload+32B Flag，Sresult=ceilDiv(P,480)*512；返回 BF16/FP16 且无附加字段时 P=2H。M3/M4 同次调用统一 Sresult、解码 dtype、flagBlockCount，复用发送 lane，明确 payload 与 Flag 发布边界；partial cast 联动 CB04-03。 | [R1](#参考依据目录)/[R3](#参考依据目录)/[R4](#参考依据目录)；非量化直发有参考，新 partial 返回的尾块和依赖需核对。量化格式问题移至附录 A。 |

### M4 读取、局部聚合与 partial 返回

**功能预期：** Read 核通过 Compare＋GatherMask 筛选并密排类型 2 行，按 Read 核数均分
汇聚转发任务。各核只缓存本核任务行号范围对应的表 2、表 3 区间。中转数据 ready 后，
按表 3 的 N 和有效成员索引读取各成员，加上 expertOut[row] 中的本地第一跳贡献，分别使用表 3 的 weight 加权求和，
再按表 2 返回一份 partial。最终 source 对这份 partial 使用表 5 的 1.0 权重。

**筛选与任务均分：** 沿用 M3 的逻辑，各 Read 核扫描本卡同一有效范围 `[0,TokenNum)`，
先筛选全部类型 2 并密排原始行号，再按密排后的数量分配；不先切分原始行范围。
以下为语义示意，API 语义参考与验证边界沿用 M1b：

```text
rowIndex = [0, 1, ..., TokenNum-1]
mask2 = Compare(TokenType[0:TokenNum], 2, EQ)
type2Rows, type2Count = GatherMask(rowIndex, mask2)
```

type2Rows 按原始 row 递增，无空泡。令 `T=type2Count`、`P=recvCoreNum`，Read 核逻辑编号
`r=coreId-sendCoreNum`，取值 `[0,P)`；分批筛选时按扫描顺序逻辑拼接，以总 T 均分：

```text
base = T / P                  // 整数除法
extra = T % P
count[r] = base + (r < extra ? 1 : 0)
begin[r] = r*base + min(r, extra)
end[r] = begin[r] + count[r]   // 只处理 type2Rows[begin[r]:end[r]]
```

T 能被 P 整除时，各 Read 核负责相同数量的类型 2 汇聚转发任务；否则各核最多相差 1 个，
每个类型 2 Token 恰好归属一个 Read 核。此处按类型 2 Token 数均分，不按各 Token 的
成员数 N 加权分配，因此成员读取数量与实际耗时仍可能不同。无类型 2 任务的核跳过本模块
的表读取及汇聚转发，仍履行 M2/M5/M6 的职责；核 56..63 的通知责任不受任务数影响。

**表 2、表 3 按本核行号范围搬入：** 非空任务段的最小、最大原始行号分别为
`type2Rows[begin[r]]` 和 `type2Rows[end[r]-1]`。两张表均截取覆盖本核这些行号的连续区间
搬入 UB，不要求缓存完整表。最小覆盖范围为 `[minRow,maxRow]`；如 M3 一样可向前扩展
读取起点，并分别记录两张表实际缓存的首行 table2BaseRow、table3BaseRow。

表 2 行距固定为 12B，表 3 行距固定为 `K*8B`。每个任务先从列表取得原始 row，再访问
两张局部缓存中的对应行，以下地址按字节计算：

```text
row = type2Rows[i]                          // i 属于本核任务段
dstEntry = table2UbBase + (row-table2BaseRow)*12
relayRow = table3UbBase + (row-table3BaseRow)*(K*8)
```

连续区间内的非 type2 行只随搬运进入缓存，不生成任务，也不解析其表 3 无效内容。
例如本核有 16 个类型 2 Token，行号在 23..66，若两表均读取 `[22,66]`，则表 2 的有效
搬运量为 `45*12B`，表 3 为 `45*K*8B`；实际只处理列表中的 16 行。
容量按原始行号跨度计算，不能按密排任务数估算两表缓存大小。跨度超出 UB 预算时分批缓存，
每批延续同一规则，保持两表覆盖当前批任务；密排列表下标不能替代原始 row 进行表寻址。

**单个任务的两表消费：** 表 3 首个 8B 块给出 N 和 selfWeight，第 1..N 块给出
srcExp/q/weight；仅消费这些有效块，使用 selfWeight 加权本行 expertOut[row]，并读取、
加权累加 N 个成员贡献。成员读取地址按联合契约由表 3 的 srcExp/q、表 2 的原始 srcRank
及公共布局共同计算；读取中转 payload 前须满足本核 M2 ready。聚合完成后，以表 2 的
srcRank/srcTokenId/srcTopk 定位返回槽并发送 partial。成员地址采用 stageBase(member)+slot*Sstage，
只读取 H 个原始 BF16/FP16 元素，stage 行尾补齐不参与累加；返回 partial 才按 Sresult 的
DataAsFlag 协议打包。两表局部缓存不改变五表 ABI 或权重语义。

**已确认项：** type2 密排后按 Read 核均分，表 2/3 局部缓存；M2 ready 后读取成员，self+成员加权返回 partial。

**待确定项：** 分级见[规则](#待确定项分级规则)，推荐均待确认。

| 编号 | 风险 / 参考 | 待确定内容 | 推荐设计 | 依据与核对重点 |
| --- | --- | --- | --- | --- |
| <a id="cb04-01"></a>CB04-01 | 低 / 直接 | 两表缓存和成员地址实现 | 推荐两表共用批次边界，各自保留 baseRow 与 12B/K*8B stride；header.N 按 uint32，成员按 uint16 expert/q 与 FP32 weight。沿用联合 source/expert/q→slot 公式，同卡成员也读 stage。 | [R1](#参考依据目录)/[R2](#参考依据目录)；协议和生产规则已存在；验证 type3 offset 与读取 slot 闭合、0 权重和高 slot。 |
| <a id="cb04-02"></a>CB04-02 | 中 / 部分 | 成员预取、累加及 partial 输出流水 | 推荐单个 type2 保持同一 owner，按表内顺序累加；多个 MTE2 输入 lane 预取，与 Vector 累加并行，partial 输出 lane 独立交 MTE3。H 分块不引入跨核归约。 | [R3](#参考依据目录)/[R4](#参考依据目录)；有 lane/加权参考，新主动读取与 partial 输出组合需排依赖。 |
| <a id="cb04-03"></a>CB04-03 | 高 / 部分 | 非量化累加精度、partial cast 和误差标准 | stage 原始 dtype/512B 行距已确认；剩余为 M4/M5 累加精度和非量化 partial 数值转换。推荐 FP32 累加，partial 使用统一返回格式；确定 partial/yOut 的 cast 时机和舍入，以两级归约独立参考明确误差标准，不宣称与一次全 K 求和逐位相同。 | [R3](#参考依据目录)；FP32 加权组件有参考，非量化两级归约仍会改变舍入位置。量化 scale、再量化及量化误差问题移至附录 A。 |

### M5 最终本卡 Token 聚合

**功能预期：** 独立函数 AggregateSourceTokens 负责本卡 BS 个原始 Token：按表 4 mask
接收有效结果槽，按表 5 加权归约并写 yOut。首版由 Read 核执行；模块通过参与核的逻辑编号和
数量划分任务，与 Send/Read 核类别解耦，保留后续改为全部 64 核参与的能力。

**按 BS 分核：** 以本卡原始 TokenId 为任务单位，在参与核间均分连续区间。设参与核数为 P、
逻辑编号为 r，首版 `P=recvCoreNum`、`r=coreId-sendCoreNum`；后续 64 核方案可令
`P=64`、`r=coreId`，不改变任务粒度：

```text
base = BS / P
extra = BS % P
count[r] = base + (r < extra ? 1 : 0)
begin[r] = r*base + min(r, extra)
end[r] = begin[r] + count[r]
```

同一 Token 的最多 K 个结果副本，其监听、接收、加权聚合和 yOut 写出全部归属同一个核，
不将不同 k 分给不同核。核间任务数最多相差 1；无任务核直接完成本模块责任。

M5 使用 M0 的 resultBase 和 Sresult 定位副本及其 DataAsFlag，Flag 属于结果数据槽，
不放入 Combine 的 52KiB 控制区，也不使用 stage 的原始行 stride。

**表 4/5 局部缓存：** 开始监听前，将表 4 的 `[begin[r],end[r])` 和表 5 的
`[begin[r],end[r])×[0,K)` 搬入 UB，有效数据量分别为 `count[r]*4B`、`count[r]*K*4B`。
遍历 TokenId=p 时用 `p-begin[r]` 索引两张缓存，不加载其他核负责的表段。

**逐 Token 非阻塞轮询：** 在本核未完成 Token 之间循环扫描。每次检测一个 Token 的所有
有效 k 槽；未到齐就保留该任务并检查下一个，不在当前 Token 内持续等待。扫描末尾后回到
仍未完成的任务，已经完成接收聚合的 Token 不再参与监听。到齐后接收 mask 置位的副本，
按表 5 的权重聚合并写出 yOut[p]；mask=0 的 Token 无接收义务，推荐按空和输出零（CB05-01），
不调用全零 mask 的 ReduceSum 来推断 ready。

**按 mask 选择副本并密排 Flag：** 采用用户明确的新方案：在搬运阶段遍历表 4 的置位 k，
逐个副本拷贝其全部 N 个 32B Flag 区，在 UB 中连续追加，跳过未置位副本。
N 表示单个结果副本的 Flag 区数量，实现中命名为 flagBlockCount，与 M4 的成员数 N 无关。
令 `m=popcount(SourceMask[p])`、`B=m*N`，本次 probe 的 UB 有效区为 B 个连续 32B 块。
例如 K=6、mask=0b001001，UB 依次存放 k=0 的 N 块、k=3 的 N 块，共 2*N 块。
不再采用全 K 槽拷贝、按 n/k 重排及动态展开 sourceMask 的归约方案。

这里密排的是 **32B Flag 块**，块内首 float 仍相隔 8 个 FP32 元素；无需压缩成连续的 4B
标量数组。读取首 float、步进一个块即可求和，后续原始 k 与权重的对应仍通过表 4/5 获取。

**已确认的首 float 完成契约：** 用户确认总线协议层保证数据写入完整性，每个 32B Flag 区
只检查首个 float，其余 7 个 float 不参与检测。各区首值为本轮 FP32 的 0/1，所有有效副本
具有同一 N，检测目标固定为：

```text
flagSum = sum(packedFlags[b][0] for b in [0,B))
expected = B = N * popcount(SourceMask[p])
ready = (flagSum == expected)
```

mask=0 时走前述无接收义务路径，不提交零次数的搬运或归约。

**每个有效副本一次多块拷贝：** “一个一个拷贝”以置位的 k 副本为单位，不对其中每个
32B Flag 单独发出一条指令。同一副本的 N 块按结果区的固定 Flag 间距，通过一次带步长
DataCopy 搬入连续 UB；下一副本追加到 `packedFlags+j*N*8` 个 float 的位置，j 为有效
副本的密排编号。所有 m 次搬运先提交，统一建立一次 MTE2→Vector 依赖后再求和，
不在各 k 的拷贝之间逐次等待。

当前 Dispatch 的 [ReadServerDedupFlags](../../src/ep/kernels/tilexr_ep_server_dedup_receive_packet.h)
已有这种多块读取：`DataCopyParams{N,1,15,0}`，从每个 512B 包块的 480B 偏移处读取
32B Flag，源块间跳过 15 个 32B 块，目的块连续。Combine 若沿用该包块布局可以参考此
调用；实际首地址、源步长按 M3/M4 确定的结果区布局计算，并满足 DataCopy 参数范围。
规则内每次 probe 为 m 次 DataCopy 调用，不是 m*N 次；设备仍需访问 m*N 个分散 Flag 区。

**一次首 float 求和：** 对已密排的 B 个块，使用 ReduceSum 连续 mask 重载，每个 repeat
只计算首个 FP32，下一 repeat 前进一个 32B 块。无需表 4 位图参与归约：

```cpp
// 示意：m 次 Flag 搬入已完成，B > 0，缓冲区和临时空间已准备。
AscendC::ReduceSum<float>(sumOut, packedFlags, work, 1, B, 1);
// mask=1：每个 repeat 只取第一个 float。
// repeatTime=B，srcRepStride=1：每次前进一个 32B 块。
// sumOut[0] == float(B) 表示此 Token 的有效副本全部到齐。
```

该调用形态已经用于当前 [ServerDedupCounts::WaitDispatch](../../src/ep/kernels/tilexr_ep_server_dedup_counts.h)
的连续状态块首 float 求和；Memory Combine 的 CheckPackedFlagRangeArrive 也有相同
调用，LocalWindowCopy 已有逐 Token 未就绪则 continue 的轮询结构（见 R3）。官方
[ReduceSum](https://asc.gitcode.com/api/SIMD-API/basic_api/memory_vector_compute/reduction_compute/ReduceSum.html)
说明其支持连续 mask、以 DataBlock 为单位的 repeat 步长，并归约为一个总和。
一次 API 调用内部仍会执行多个 repeat 和中间归约，不等于一条硬件指令，也不意味着归约
成本为常数。UB 源缓冲的尾部访问余量、对齐及 work 大小须按目标实现准备，不能只按 B 个
有效首 float 分配源缓冲。此路径不再受之前每个 repeat 最多覆盖 8 个完整 k 槽的布局限制。

**Flag 拷贝代价评估：** 单次 probe 的有效拷贝字节数为 `32*m*N`，仅参与求和的值为
m*N 个；跳过无效副本后，拷贝量为全 K 槽方案的 m/K。以 N=16 为例，K=6、m=2 时为
2 次多块拷贝、32 个 Flag 区、1024B 有效搬运量；m=6 时为 6 次、96 个区、3072B。
有效搬运字节数不等于底层总线实际流量，后者还受事务粒度、跨距及访问竞争影响。

从指令数量和搬运量看，该方案可作为简单的首版实现：复用现有多块 Flag 读取方式，
不引入 k/n 重排。不能仅据字节数断言耗时很低；潜在代价主要为 Scalar 的 m 次地址生成和
指令下发、MTE2 分散小块读取、每次 probe 的 MTE2→Vector→Scalar 依赖，以及未到齐时
反复扫描。若一个 Token 被检测 P 次，拷贝调用累计为 P*m 次、有效搬运量为 P*32*m*N；
同时轮询的核数越多，访问竞争也可能增大。避免逐块或逐副本同步是本方案的直接实现约束。
后续验证应拆分指令下发、等待搬入、归约并读取结果的代价，记录每 Token 的 probe 次数，
再判断是否成为瓶颈；当前无真实设备耗时数据，不给出微秒级预测。

目标为 CANN 9.1/A5。此前 API 调研时，版本过滤的官方检索脚本未返回 9.1.0 ReduceSum 页面（count=0，
无页面获取错误），当时所查常用安装路径及仓库 env/cann 未找到目标头文件；这是历史调研记录，
本次文档通读未重新执行环境或 API 验证。上述官方链接为
asc-devkit master 文档，列明 Ascend 950 支持。尚未用目标版本头文件、编译或设备验证，
不能将接口语义推导视为目标环境验证通过。后续核对头文件为
`basic_api/kernel_operator_vec_reduce_intf.h`；具体实现需满足 UB 对齐、临时空间和搬运依赖。

#### M5 尾部统一清理 DataAsFlag（已确认）

清理不再跟随逐 Token 接收：本核负责的全部 Token 完成接收、加权聚合及输出处理后，
单独进入清理阶段。此时相关 payload 的 MTE2 读取必须已完成；不要求等待其他核的聚合任务。
M6 仍负责确认 yOut、清理及其他在途写出完成后才能结束本核。

清理范围为本核连续 Token 段 `[begin,end)` 的**全部 K 个副本槽、每槽全部 N 个 32B Flag 区**。
不查询 sourceMask，不判断哪些 k 本轮实际被写入，也不只清首个 float；mask=0 的 Token
同样纳入清理。无 Token 任务的核不下发清理命令。聚合阶段仍只读取有效副本，且已完成 Token
不再 probe，因此保留到尾部的旧 ready 值不会让本轮重复聚合。
此边界要求已收齐所有应到达写入，无效槽按协议没有本轮 writer；不引入额外全核清理屏障。

**已确认：每 Token 一次 DataCopyPad，Token 间无同步。** 若最终 wire 沿用当前
480B payload+32B Flag，且 `Sresult=N*512B`、槽之间没有额外空隙，单 Token 的 K*N 个
Flag 可展开为一串相隔 512B 的地址。本核在尾部用一个 for 循环逐 Token 提交，
每次只修改目的 GM 起点，拷贝参数和零值 UB 源地址保持不变：

```text
F = K * N                     // 单 Token 全部副本的 Flag 区数量
firstFlag(p) = resultBase + p*K*Sresult + 480
flagAddress(p,i) = firstFlag(p) + i*512,  i in [0,F)
// i = k*N + n；下一 Token 的 GM 起点增加 K*Sresult。
```

对应普通 UB→GM 重载的调用示意为：

```cpp
// 本核全部聚合任务及相关 payload 读取已完成；无 Token 任务时跳过此阶段。
// zeroUb 为 32B 对齐、至少 K*N*32B 的连续零值区。
// 复用该 UB 前确认旧使用者结束；置零后在循环前建立一次 V→MTE3 依赖。
const uint32_t flagsPerToken = K * N;
AscendC::DataCopyExtParams params{
    static_cast<uint16_t>(flagsPerToken), 32U, 0U, 480U, 0U};
for (uint32_t p = begin; p < end; ++p) {
    AscendC::GlobalTensor<float> tokenFlagGm;
    tokenFlagGm.SetGlobalBuffer(reinterpret_cast<__gm__ float *>(
        resultBase + uint64_t(p) * K * Sresult + 480U));
    AscendC::DataCopyPad(tokenFlagGm, zeroUb, params);
    // 不在 Token 间插入 SetFlag/WaitFlag 或 PipeBarrier。
}
// 全部提交后，由 M6 统一确认清理 MTE3 完成，再释放/改写 zeroUb 或结束本核。
```

同一零值 UB 在循环中只被 MTE3 读取，各 Token 的 GM 写入范围互不重叠，因此不需要
在相邻调用间同步，也不逐次重新置零。最后一个 Token 的清理写入结束地址恰为
`resultBase + end*K*Sresult`，不会进入相邻核 Token 或 stage。每核提交 `end-begin` 次
DataCopyPad，与 mask 中置位数量无关；不再采用将本核所有 Token 合并成一条命令的方案。
若 CB03-02 最终增加槽间空隙，跨 k 的 Flag 间距将改变，需重新核对上述单 Token 展开公式。

**UB 复用：** 普通重载的 `srcStride=0` 表示连续读取 UB 块，零值源需求为 `K*N*32B`，
不随本核 Token 数增加。采用尾部复用已完成任务的 UB；清理期间保持该源区不变，全部
清理写回完成后才允许覆盖或释放。按 M0 的非量化 BF16/FP16 返回示例假设，H=7168、N=30、K=16 时，需要 15360B
（15KiB）零值区，每条命令处理 480 个 Flag；即使本核负责 32 个 Token，也只是连续提交
32 次调用，共用同一块 15KiB 零值区。该方案按单 Token 预算落实，具体复用区间和依赖
仍纳入 CB1A-01 的跨模块 UB 总表；不再为本核全部 Token 预留成倍零值空间或保留跨 Token 分批方案。

**API 依据与验证边界：** 现有 Memory Combine 的 ClearPackedTokenFlags 使用
`DataCopyExtParams{N,32,0,480,0}`，但按单 Token 的每个 k 分别调用；新设计在尾部将单 Token 的 K 个槽合为一次，Token 间循环提交。
官方 [9.1 beta2 DataCopyPad](https://www.hiascend.com/doc_center/source/zh/CANNCommunityEdition/910beta2/API/ascendcopapi/atlasascendc_api_07_0265.html)
给出 UB→GM 原型和上述步长单位，通用 blockCount 上限写为 4095；
[asc-devkit master 的 UB→GM 页面](https://asc.gitcode.com/api/SIMD-API/basic_api/memory_vector_compute/data_move/DataCopyPad_UBToGM.html)
则区分 950 的 65535 与 A2/A3 的 4095，不能把这些不同版本上限混用。
本次目标仍为 CANN 9.1.0/A5；本地未找到目标安装头文件，官方检索脚本按 9.1.0 精确筛选返回 count=0、fetch_error_count=0，
未取得最终版匹配页。单次块数上限须以目标 `kernel_struct_data_copy.h`、对应实现和编译验证定稿，
不把 uint16_t 字段宽度当作硬件上限；实现时核对的是单 Token 的 K*N，
而非本核所有 Token 的总 Flag 数。目前确认逐 Token 调用设计，未运行目标编译或设备验证。

**已确认项：** BS 整 Token 分核，预载本核表 4/5；未到齐就检查下一个；有效 k 的 Flag 密排后一次首 float 求和。本核所有任务完成后，尾部按每 Token 一次 DataCopyPad 清全部 K 槽的 Flag，Token 间无同步，共用零值 UB。

**待确定项：** 分级见[规则](#待确定项分级规则)，推荐均待确认。

| 编号 | 风险 / 参考 | 待确定内容 | 推荐设计 | 依据与核对重点 |
| --- | --- | --- | --- | --- |
| <a id="cb05-01"></a>CB05-01 | 低 / 直接 | 任务状态、表段缓存与空 mask | 推荐沿用 Memory tokenStatus+循环扫描，完成后不再 probe；两表按 p-begin 索引。mask=0 建议按空和写零，标为本轮推荐边界行为；TokenNum=0 仍按 BS/sourceMask 执行。 | [R1](#参考依据目录)/[R3](#参考依据目录) LocalWindowCopy/ExpertScaleCopy；验证快慢交错、bit31、空核与零 mask。 |
| <a id="cb05-02"></a>CB05-02 | 中 / 直接 | Flag/work 预算及 probe 指令组织 | 推荐每个有效 k 一次 N 块 DMA，统一 MTE2_V；ReduceSum(mask=1,repeat=m*N,stride=1) 后一次 V_S。按 m=K 预算 Flag/work 和目标尾部访问余量，N/计数/步长由公共布局计算并校验 API 范围。 | [R3](#参考依据目录) CheckPackedFlagRangeArrive、[R4](#参考依据目录) ReadServerDedupFlags、[R7](#参考依据目录)；算法有参考，真实延迟与 H/K 资源上界待验证。 |
| <a id="cb05-03"></a>CB05-03 | 高 / 部分 | 结果消费、加权输出及 Flag 清理 | 推荐复用 Memory 非量化解包、FP32 加权及 cast 输出，仅消费置位槽，partial 权重为 1。**已确认：本核全部聚合任务完成后，在尾部单独清本核 Token 的全部 K 槽和全部 Flag 区，不依赖 mask。** **每 Token 一次 DataCopyPad，循环中只更新 GM 地址、不在调用间同步，共用 K*N*32B 零值 UB。** 具体 UB 复用区间、目标参数范围和首尾依赖仍待落实，M6 排空输出及清理；首次及共用数据区复用的 Flag 初始状态归 CB1A-02，不能只用控制区清零代替。权重仍按原始 k 索引。 | [R3](#参考依据目录)；算术与单槽清理有参考，尾部全槽清理方向已确认，仍需核对目标 API 限制、UB 复用和在途依赖。 |

### M6 调度、资源衔接与整体结束

**功能预期：** 串联全核 Init/本地拷贝、Send/Read 分工、ready、DIRECT/group 处理和最终
source 聚合，保证任务持续推进，并在本核所有发送、输出和在途搬运责任完成后结束。
M5 后续采用 64 核时，也由本模块安排 Send 核完成发送后进入最终聚合的路径。

**已确认项：** 串联 M1–M5，M5 在本核全部聚合任务完成后单独清理所有负责槽的 Flag，在本核发送、输出、清理及在途操作完成后结束；首版 M5 仅 Read 核，全核版保留映射能力。

**待确定项：** 分级见[规则](#待确定项分级规则)，推荐均待确认。

| 编号 | 风险 / 参考 | 待确定内容 | 推荐设计 | 依据与核对重点 |
| --- | --- | --- | --- | --- |
| <a id="cb06-01"></a>CB06-01 | 高 / 部分 | 运行时阶段顺序与进度依赖 | 推荐 Send 在自身 M1b 发布后进入 M3；Read 完成自身 M2 责任后处理全部本核 M4，排空再进入 M5，不等其他 Read 切阶段。验证发送不依赖对方先跑 M5、通知核不被其他监听阻塞；首版暂不交替 M4/M5。 | [R3](#参考依据目录) 有先发后收参考；两级 ready+分核的新组合需检查等待依赖图无环。 |
| <a id="cb06-02"></a>CB06-02 | 高 / 部分 | 结束判据与在途资源排空 | 推荐分别跟踪 payload、Flag、yOut、清理责任，全部相关引擎完成才返回；Send 无 M5 任务不代表 MTE3 可提前释放。沿用模型调度、不加 Server 读完握手，额外核内阶段屏障按依赖图定稿。 | [R3](#参考依据目录)/[R4](#参考依据目录)；核末排空有参考，新控制区与输出/清理需合并核对，不能仅看 token 计数。 |
| <a id="cb06-03"></a>CB06-03 | 中 / 无 | recvCoreNum 初值及选择策略 | 推荐首轮固定 recvCoreNum=32/sendCoreNum=32 作为未调优起点，再在已确认 8..48 范围内按负载及实测选择。Host/tiling/kernel 配置一致，不新增公共分核参数。 | 新 Combine 尚无分核性能参考；32/32 只是工程建议，需用户核对，不是最优结论。 |
| <a id="cb06-04"></a>CB06-04 | 中 / 直接 | 验证梯度与诊断/性能口径 | 推荐先 Host 地址/容量及独立数值 oracle，再 A5 编译、BF16/FP16 单轮、多轮和倾斜任务，并验证 Host 拒绝 quantMode 非零输入。记录阶段耗时/probe 次数、后端/版本/数据量及跨 rank 统计；细打点只做诊断，performance 与 breakdown 分开。 | [R8](#参考依据目录)/[R9](#参考依据目录)；覆盖同卡成员、多成员、空任务、整体 buffer 选择与复用、尾块及不同 expertOut，不沿用普通 Memory/CPU 设备结论。 |

### 第一阶段 breakdown 记录隔离（已确认）

扩充标准 profiling 记录空间，为 Dispatch 和 Combine 分别提供独立 base 和不重叠的容量。
记录区独立于 peer-memory 通信窗口，不占用 Combine 52KiB 控制区或共用数据区。
除 timestamp/event 记录外，各算子的有效记录数、丢弃计数和 latest-launch 预留/重置状态
也必须隔离；不能仅移动 payload base，却共用计数或让后一个算子清掉前一个算子的记录。
可各自保留最近一次 launch，但两者不能共用同一个 latest 记录槽或互相约束记录宽度。

| 测试模式 | 解析及 trace 输出 |
| --- | --- |
| Dispatch 单测 | 只解析 Dispatch 区、输出 Dispatch trace；Combine 区不解析、不要求有效记录 |
| Combine 单测 | 只解析 Combine 区、输出 Combine trace；Dispatch 区不解析、不要求有效记录 |
| D+C 联测 | 分别解析两区，分别输出 Dispatch trace 和 Combine trace，不合并成一个 trace 文件 |

每份 trace 绑定相同测试 run_id 及自身 operator、rank、kernel 版本和 launch 身份；沿用现有
跨 rank 导出组织，两套 trace 分别保存。只检查本次启用的算子，未启用区的空值或旧记录
不参与解析和成败判断。C6 验证记录互不覆盖、缺失/溢出能够报错及对应模式的输出文件集合。
这是已确认的扩展要求，尚未修改 profiling runtime、writer、consumer 或 parser。

## 初版完成后的整体通读

2026-09-20：依据用户确认，M1–M5 的模块细化补充阶段结束，本轮已执行整体通读。
已确认项保留正文；待确定项在各模块原位维护。M0/M6 的跨模块衔接一并纳入，
不等待全部实现细节完成后才暴露容量、同步、数值和结束条件方面的缺口。

最近一次通读已将整体半区步长、每窗单份 M2 Flag、固定控制区宏和小表 offset 同步到 M0–M5；
区分固定预留与有效记录，明确 stage 原始行和 result DataAsFlag 槽的不同地址规则。
已确认接口、布局、加权范围不再重复作为方案选择；待确认项继续保留原编号，未把推荐自动视为批准。

### 待确定项分级规则

风险与参考强度分别标记，**有参考不意味着低风险**；推荐也不等于用户已批准。

| 标记 | 含义 | 建议处理 |
| --- | --- | --- |
| 高 | 可能改变数据正确性、地址安全、跨核/跨卡进度、数值契约或资源复用 | 用户逐项核对推荐及边界，再收口 |
| 中 | 方向已定，剩资源、性能或支持范围取舍，错误实现仍可能影响正确性 | 有参考时倾向推荐方案，保留所列验证 |
| 低 | 已确认 ABI/算法下的局部实现选择，有明确参考和验证边界 | 建议采用推荐设计并做针对性验证 |
| 直接参考 | 仓库已有相同操作或局部控制模式 | 不代表新模块组合已设备通过 |
| 部分参考 | 仅有组件或文档，需重新设计组合关系 | 高风险仍需重点核对 |
| 无参考 | 未找到满足新职责的直接代码或已验证方案 | 即使风险为中，也单独交用户核对 |

### 待确定汇总表

原 24 项中，[CB00-01](#cb00-01) 接口组织和 [CB00-02](#cb00-02) 共同布局、[CB02-02](#cb02-02) M2 随整体 buffer 单份配置均已确认。
[CB00-03](#cb00-03) 首版有权重及未实现特性只接受关闭配置均已确认；以下 **20 项仍有待确认内容**。
量化子问题已移至附录 A，不计入首版待确定项；CB03-02/CB04-03 仅保留非量化通信与数值边界。
原条目都仍有非量化工作，故待定总数保持 20，编号及风险等级不变。CB1A-01 等待所有模块 UB 用量明确后再讨论；
CB1A-02 的 Host 初始化、kernel 用后清零方向已确认；当前测试新建并清零 buffer，结果 Flag
跨布局/已用域复用列为附录 B 重点遗留，不阻塞第一阶段。其余覆盖与依赖见该节。控制区大小、
小表 offset、M2 副本数及 stage 原始 dtype/行对齐已经确认。初始化/发布/清理依赖、
返回槽具体通信格式、精度、UB/lane 和调度仍分别由原编号承接，不新增重复议题。
优先看“重点核对”项；其余为可倾向推荐方案的候选。
具体建议、证据和验证边界见编号链接。共享问题只设一个主编号，其他模块通过编号关联。

| 编号 | 模块 | 待确定项 | 风险 | 参考 | 建议处理 |
| --- | --- | --- | --- | --- | --- |
| [CB02-03](#cb02-03) | M2 | 两级 Flag 消费清理的完成依赖 | 高 | 部分 | 重点核对 |
| [CB03-02](#cb03-02) | M3 | 非量化 DIRECT/partial 统一 wire 及发送流水 | 高 | 部分 | 重点核对 |
| [CB04-03](#cb04-03) | M4 | 非量化累加精度、partial cast 和误差标准 | 高 | 部分 | 重点核对 |
| [CB05-03](#cb05-03) | M5 | 结果消费、加权输出及 Flag 清理 | 高 | 部分 | 尾部逐 Token 调用已确认，资源与首尾依赖待核对 |
| [CB06-01](#cb06-01) | M6 | 运行时阶段顺序与进度依赖 | 高 | 部分 | 重点核对 |
| [CB06-02](#cb06-02) | M6 | 结束判据与在途资源排空 | 高 | 部分 | 重点核对 |
| [CB1A-01](#cb1a-01) | M1a | 各角色 UB/Event 总预算及生命周期 | 高 | 部分 | 暂缓；各模块 UB 用量明确后重点核对 |
| [CB1A-02](#cb1a-02) | M1a | Host 初始化覆盖与结果 Flag 复用边界 | 高 | 部分 | 当前测试新建并清零；跨布局复用列为附录 B 重点遗留 |
| [CB1B-03](#cb1b-03) | M1b | 64 核紧凑 4B 完成记录的精确发布 | 高 | 部分 | 重点核对 |
| [CB06-03](#cb06-03) | M6 | recvCoreNum 初值及选择策略 | 中 | 无 | 重点核对 |
| [CB02-01](#cb02-01) | M2 | 两级求和与通知 fanout 搬运 | 中 | 部分 | 倾向采用推荐设计 |
| [CB04-02](#cb04-02) | M4 | 成员预取、累加及 partial 输出流水 | 中 | 部分 | 倾向采用推荐设计 |
| [CB05-02](#cb05-02) | M5 | Flag/work 预算及 probe 指令组织 | 中 | 直接 | 倾向采用推荐设计 |
| [CB06-04](#cb06-04) | M6 | 验证梯度与诊断/性能口径 | 中 | 直接 | 倾向采用推荐设计 |
| [CB1A-03](#cb1a-03) | M1a | Reset、寄存器与 Event 初始状态 | 中 | 直接 | 倾向采用推荐设计 |
| [CB1B-01](#cb1b-01) | M1b | 大 TokenNum 的分批筛选与密排缓存 | 中 | 部分 | 倾向采用推荐设计 |
| [CB1B-02](#cb1b-02) | M1b | 本地拷贝 lane 与表 2 获取 | 中 | 直接 | 倾向采用推荐设计 |
| [CB03-01](#cb03-01) | M3 | 表 2 局部缓存分批与索引 | 低 | 直接 | 倾向采用推荐设计 |
| [CB04-01](#cb04-01) | M4 | 两表缓存和成员地址实现 | 低 | 直接 | 倾向采用推荐设计 |
| [CB05-01](#cb05-01) | M5 | 任务状态、表段缓存与空 mask | 低 | 直接 | 倾向采用推荐设计 |

当前待确认 20 项（含暂缓的 CB1A-01）：高风险 9 项、中风险 8 项、低风险 3 项；
无参考 1 项（与风险等级交叉），合计 10 项建议重点核对。

建议收口顺序：在 CB00-02 已确认布局下，先 CB03-02/CB04-03 确定非量化通信与数值格式，再 CB1A-02/CB1B-03/
CB02-03 明确初始化、发布和控制区复用，之后 CB05-03/CB06-01/CB06-02
核对消费和结束依赖；各模块 UB 用量明确后再讨论 CB1A-01 跨模块复用。CB06-03 的分核起点单独确认；后续调优不改变既定任务归属。

### 参考依据目录

| 编号 | 当前参考及可借鉴范围 | 不能据此推断的结论 |
| --- | --- | --- |
| R1 | [联合设计](ep-memory-server-dedup-dispatch-combine-design.md)：五表、27 参数、逻辑寻址、共同窗口预算 | 内部同步或物理布局已实现 |
| R2 | [公开接口](../../src/include/tilexr_ep.h)、[协议结构](../../src/ep/common/ep_memory_server_dedup_protocol.h)、[Combine Memory Host](../../src/ep/host/tilexr_ep_memory_combine.cpp)提供语义；CB00-01 所列 Dispatch dedup Host/tiling/launch/设备入口提供形式参考 | 新 Combine dedup 入口已接通，或应照搬普通 Memory 的逐项标量 launch ABI |
| R3 | [Memory Combine kernel](../../src/ep/kernels/tilexr_ep_combine_memory_kernel.h)：ReadSendCount、SplitCoreCal、BuffInit、CheckPackedFlagRangeArrive、ExpertScaleCopy、ProcessMoeExpert、LocalWindowCopy、ClearPackedTokenFlags | 普通 Memory 全槽接收、全核屏障可原样套用，或新 partial 误差已验证 |
| R4 | [Dedup Send](../../src/ep/kernels/tilexr_ep_server_dedup_send.h)、[Recv](../../src/ep/kernels/tilexr_ep_server_dedup_recv.h)、[Flag 读取](../../src/ep/kernels/tilexr_ep_server_dedup_receive_packet.h)、[接收任务](../../src/ep/kernels/tilexr_ep_server_dedup_receive_tasks.h)：动态 lane、Event 所有权、多块搬运、核末排空 | 新两级协议或 M4 拉取流水已验证 |
| R5 | [Memory Dispatch](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h) 的 TokenActiveMaskCal：比较/GatherMask 密排行号 | type1/2/3 分批两遍筛选已实现 |
| R6 | [当前布局计算](../../src/ep/common/ep_memory_server_dedup_data_layout.h)：Host/device 共用 uint64 计算；relayBytes 注释明确是历史预留 | 旧区域可直接容纳 E*BS 个 stage 槽 |
| R7 | M1b/M5 引用的官方 DataCopyPad、GatherMask、ReduceSum 文档及当前 Count/Memory 调用 | master 页面等同目标 CANN 9.1 头文件或并发设备证据 |
| R8 | [联合验证边界](ep-memory-server-dedup-dispatch-combine-design.md#8-验证边界与维护要求)、[验证指南](../EP_VALIDATION_GUIDE.md)、[EP 测试](../../tests/ep) | Dispatch/CPU 通过覆盖新 Combine |
| R9 | [进度与 Reset 经验](../EP_MEMORY_SERVER_DEDUP_PROGRESS.md)、[当前 Dispatch 规格](ep-memory-server-dedup.md)：打点和性能口径；量化历史经验仅供附录后续参考 | 同类历史问题就是新实现的已知故障 |

## 参考与验证边界

基于[当前 Memory Combine kernel](../../src/ep/kernels/tilexr_ep_combine_memory_kernel.h)及
[Host 入口](../../src/ep/host/tilexr_ep_memory_combine.cpp)进行实现设计；
接口与五表以[联合设计](ep-memory-server-dedup-dispatch-combine-design.md)为准。

本轮完成设计通读、已确认项补全和待确定项分级，尚未实现 Server Dedup Combine，未运行 NPU 验证。
后续模块初版、整体通读和实现验证分别记录状态，不将设计完成视为实现通过。


<a id="deferred-quantization"></a>
## 附录 A：遗留项——量化功能（首版不实现）

本附录仅保留后续设计线索，不属于首版待确定汇总、UB 预算、实现计划或验收门槛。
仅在后续明确启动 Combine 量化支持时重新讨论，不沿用 Memory/Dispatch 已有能力作为新 Combine 已支持的依据。
公开 quantMode 参数保留但首版仅接受 0；表 3/5 的 FP32 路由权重及 expertScales 接口仍然必需，
不能把“暂不量化”解释为取消加权。非量化 FP32 累加、BF16/FP16 cast 和舍入仍在正文讨论。

| 遗留编号 | 原关联项 | 后续需讨论内容 |
| --- | --- | --- |
| Q01 | CB00-03、CB00-01 | 量化模式与 FP8 格式支持矩阵、合法参数组合、Host 校验和 tiling 配置；外部量化 expertOut 输入若需支持，还需另行定义 scale 契约 |
| Q02 | CB03-02、CB00-02 | 量化 payload、scale 个数/类型/存放顺序及内部补齐，DIRECT/partial 的统一解码格式；重新推导 P、N、Sresult、结果区及 stage 起点、容量和 Flag 清理范围 |
| Q03 | CB04-03、CB05-03 | M3 量化、M4 partial 再量化与 M5 解量化的职责，scale 计算顺序、cast/舍入/溢出策略，以及两级归约叠加量化后的独立误差标准 |
| Q04 | CB1A-01、CB04-02、CB05-02 | 量化/解量化临时区、scale 缓冲、work 和独立 lane 的 UB/Event 预算，以及与 MTE2/MTE3/Vector 的依赖和性能 |
| Q05 | CB1A-03 | Reset 后重建量化专用寄存器状态；参考历史 FP8 溢出模式丢失问题，按新调用链核对恢复点，不能推断首版非量化路径存在同样故障 |
| Q06 | CB06-04 | 各量化模式和格式的编译、单轮/多轮正确性、极值/舍入/溢出、scale/尾块、精度误差及性能验证，单独记录产物和设备证据 |

来源：量化接口和算法参考现有 [Memory Combine](../../src/ep/kernels/tilexr_ep_combine_memory_kernel.h)，
Reset/FP8 历史经验见[进度记录](../EP_MEMORY_SERVER_DEDUP_PROGRESS.md)。本附录不新增已确认的量化方案。

<a id="deferred-result-flags"></a>
## 附录 B：重点遗留——结果 Flag 初始化与通信域复用

关联 CB1A-02，用户确认后续处理遗留问题时单独展开，当前不作为第一阶段开发或验收阻塞项。
当前测试基线为每次测试新建通信 buffer，执行前完成整块清零，覆盖两个整体 buffer 的控制区
和数据区。测试内多轮沿用既定配置和已确认的窗口角色；不把测试前清零改成每轮 Host 清零。
第一阶段空 Combine 不执行结果 Flag 发布/监听；后续真实 kernel 的用后清理仍按 M2/M5/M6 实现。

保留的后续问题是：在未重新清零的已用通信域上改变 BS/K/H、数据角色或 wire 布局时，
如何保证本次所有有效结果 Flag 地址的初始值。M5 尾部清理只覆盖本次 BS*K 槽的全部 Flag，
不覆盖未来布局新增的地址；例如结果区扩张进入旧 stage 区时，旧 BF16/FP16 payload 不因
控制区清零而消失。后续需单独确定初始化责任、覆盖范围和完成时机，并给出对应复用验证。
当前新建清零测试不作为上述扩展复用场景已验证的证据，也不据此新增模型轮次管理或握手。
