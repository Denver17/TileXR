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
初版默认 sendCoreNum=32、recvCoreNum=32；分核按 CB06-03 的 Combine 独立编译期宏配置。
前 sendCoreNum 核为 Send 核、后 recvCoreNum 核为 Read 核，最后 8 核（56..63）负责 M2 通知。
同一 communicator 生命周期内所有 rank 使用相同且固定的 Combine 分核策略。

Send 核直发类型 1；Read 核处理类型 2，读取其他成员、加上本地贡献，加权求和后发送 partial。
类型 3 在前置拷贝函数中放入本卡共享中转区，供相应 relay 读取。
最终本卡 BS 个 Token 的接收、归约和输出是独立模块，首版由 Read 核负责，设计上保留 64 核参与能力。

公共接口沿用 Memory 原 24 参数，只追加 tokenType、relayReadIndex、sourceMask。
表 2 复用 assistInfoForCombine，表 5 复用 expertScales；有效 expert 行数继续取 sendCounts 末项。
expertOut 与 Dispatch 输出行序一致；最终 source 任务按本卡 BS 组织。

**第一阶段实际行数约束：每卡 `0<=TokenNum<=10000`（本文 10K 指 10000 行）。**
该值仍取 sendCounts 末项，由调用方/测试场景保证；不是 BS、每核任务数或类型筛选后的数量。
超过 10K 的场景不在第一阶段支持范围，不通过截断 TokenNum 或丢弃任务适配。
MaxTokenNum/Crows 及中转槽容量继续按共同契约分配，不因实际行数上限而缩小；
超限筛选、密排与资源设计迁入[附录 C](#deferred-large-token-num)，后续作为专题展开。

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
| 第一阶段 TokenNum | 每卡实际有效行数不超过 10000；M1b/M3/M4 仅筛选 [0,TokenNum)，大行数扩展归附录 C，MaxTokenNum 容量公式不变 |
| 首版加权范围 | 仅支持有权重输入；表 3 有效 self/memberWeight 和表 5 为必需，不支持以空指针关闭加权；无权重模式延后设计（CB00-03 已确认） |
| 未实现特性 | 只接受关闭配置，Host 拒绝开启未实现的 active mask/TP/共享专家等特性，不静默忽略；Combine 首版只接受 quantMode=0，其他模式归附录遗留项 |
| 结果 Flag 清理 | M5 本核全部有效副本确认到齐、最后一次 probe 结束后，将 Flag 检测 UB 置零，提前与剩余 payload 读取/聚合重叠；每 Token 一次 DataCopyPad 只清全部 K*N 个 32B Flag 区，保留 480B payload，不筛选 mask；Token 间无同步，M6 确认写回完成 |
| 控制区清理 | Host 在首次使用前清零两份完整 Combine 控制区；kernel 由消费核用后立即清零，M6 确保清理完成；覆盖核对见 CB1A-02 |
| 加权职责 | M1b 暂存未加权结果，M3 发送未加权 DIRECT；M4 使用表 3 的 self/memberWeight，M5 使用表 5；partial 的最终权重为 1 |
| 成员寻址 | M4 第一跳自身取 expertOut[row]；其他成员含同卡成员均取 stage，用表 2 source 和表 3 expert/q 定位；自身只计一次 |
| 空职责核 | 全 64 核均发布 M1b 完成，最后 8 核均履行 M2 通知；TokenNum=0 不代表该卡无 BS 接收任务，不能跳过控制协议 |
| M5 最新方案 | 按 mask 置位 k 搬运，UB 按 32B Flag 块密排，统一等待后一次首 float 求和；完整性采用用户确认的总线保证，阈值 N*popcount(mask) |
| 控制小表 | Combine 控制区固定 96KiB；prepareDone/serverReady/扩展预留的 offset 固定为 0/16384/49152，预留跨度为 16/32/48KiB，均 512B 对齐；分核变化只改变有效范围 |
| 通信布局 | Dispatch 控制区 → Combine 控制区 → 196KiB 留空 → 共用数据区；数据起点固定为每份 buffer 的 +1.5MiB（1572864B），均 512B 对齐，前后半区各一份，步长为 peerMemBytes/2；数据区大小取 Dispatch/Combine 需求最大值，Combine 中转区接在结果接收区之后（CB00-02） |
| 复用边界 | 模型顺序不变，不重开跨轮重叠、新公共轮次参数或额外读完握手；M2 每个整体 buffer 内仅一份完成 Flag，随本次 Combine 选窗使用（CB02-02 已确认） |

实现中将 M4 的 N 命名为 memberCount，M5 的 N 命名为 flagBlockCount；M2 分核的
recvCoreNum=8*N 中 N 仅是核数倍数，三者不同。expertOut 输入仍为 FP16/BF16，
首版 quantMode 固定为 0。stage 已确认使用原始 BF16/FP16、512B 对齐行，不带 DataAsFlag；
结果槽已确认采用 DataAsFlag，DIRECT/partial 同次调用须统一可解码，不包含量化 scale。
CB03-02/CB04-03 已确认：通信返回使用本次 dtype 对应的 BF16/FP16，M4/M5 均先转 FP32 再加权累加，
M4 完成后转回通信 dtype 发送，M5 完成后转为目标 dtype 输出。
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
分核按 CB06-03 采用 Host/device 共用的编译期配置，不新增运行时公开分核参数，
也不为选择核数新增 tiling 字段；更改分核必须同步重编译并部署 Host 与 kernel。
M2 使用本次 Combine 的整体 bufferIndex，不另设 bank 状态或 tiling 字段；步长从 CommArgs.peerMemBytes/2 取得。

**内部 ABI 落实：** Combine tiling 使用 Host/device 共用定义，字段先校验再收窄，保留字节
初始化为零；以 sizeof/offsetof/标准布局断言和 launch 测试约束参数布局。Dispatch 当前
32B tiling、120B launch args 仅是参考实例，Combine 不照搬其专属输出字段、保留字段或总大小。
CB00-02 的公共布局已确认；最终字段表、位宽依据 CB03-02/CB04-03 已定格式落实，CB06-03 分核为编译期配置；本项的接口组织方向已确认，
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
| 中转数据保持原始 BF16/FP16 | 不量化、不加权、不附加 DataAsFlag；每 Token 行 stride=alignUp(H*2,512)，只读写 H*2B 有效数据，行尾对齐空隙既不写入也不读取 |
| 中转行数与 Dispatch 接收槽一致 | stageRows=localMoeExpertNum*rankSize*batchSize=E*BS；表 2 offset 与表 3 解出的 slot 使用相同物理行号，不使用密排 expertOut row |
| 结果槽 | BS*K 个槽，每槽大小为返回 Token 完成 DataAsFlag 拼接后的总字节数；非量化 payload/Flag/补齐均计入，不含量化 scale 或 Dispatch 包尾；CB03-02/CB04-03 已确认 BF16/FP16 返回及 480B+32B 包块，Sresult=ceilDiv(2H,480)*512 |
| 控制区及共用数据区 | 两个控制区由公共固定宏划分为 1244KiB/96KiB（含扩展预留），随后留空 196KiB，数据起点为 1.5MiB，均 512B 对齐；数据区容量取 max(Dispatch 接收区大小, Combine 接收区+中转区大小)，内部控制细节互不依赖 |
| 整体双 buffer | 前后半区各保留上述三个区域，第二份基址为 peer[rank]+peerMemBytes/2；每份 Combine 控制区只放一份 M2 完成 Flag（CB02-02 已确认） |
| 容量检查 | 两份布局分别放入对应半区；统一检查 singleBufferBytes<=peerMemBytes/2-runtimeReservedBytes，不能仅比较两份需求总和；不足时报错，等于时允许，uint64 运算检查溢出 |

控制小表 offset 已固定：prepareDone=0、serverReady=16384、扩展预留=49152，均相对 Combine
控制区起点且 512B 对齐，每张表由独立宏定义并按最大规模预留；详见[联合设计 §6.5](ep-memory-server-dedup-dispatch-combine-design.md#65-控制小表的固定-offset)。
初始化/清理依赖由 M1a/M1b/M2 继续设计；Combine 独立校验内部使用量
不超过 96KiB 公共宏，无需感知 Dispatch 控制布局。按当前设计，单份预留需求 48KiB、预留一倍
余量后的宏为 96KiB；完整核算见[联合设计 §6.4](ep-memory-server-dedup-dispatch-combine-design.md#64-固定控制区宏及上限核算)。
当前公共头文件已采用 96KiB 控制区、16KiB prepareDone 及 serverReady=16384B。
2026-09-24 Dispatch cnt 的 GM 槽位扩为 512B 后，Combine 控制区起点顺延至 1244KiB，
共用数据起点改为 1.5MiB，整体选窗仍取 peerMemBytes/2。Host、kernel、consumer 和布局验证须同步使用新偏移；
历史 C1–C7 设备验收不作为本次升级的设备验证证据。
固定布局与有效记录分开记账，以下均为单份整体 buffer 内、相对 Combine 控制区起点的区间：

| 区域 | 固定 offset | 固定预留 | 本次调用有效范围 |
| --- | --- | --- | --- |
| prepareDone | 0 | 16KiB | 固定 8*64 个 32B 槽，首 float 有效 |
| serverReady | 16384 | 32KiB | 按 64 块预留；当前前 recvCoreNum 块有效，4..24KiB |
| 扩展预留 | 49152 | 48KiB | 当前无活动表，不是第二份 Flag |

每份 Combine 控制区固定占用 96KiB，两份整体 buffer 共 192KiB；两个 buffer 内 Flag 表的
固定预留合计 96KiB，有效记录范围合计 40..80KiB。有效范围缩小不回收固定区间，也不移动后续表。
**强制约束：两个监听核的接收区域互不重叠，区域起点均按 512B 对齐，起点间距至少 512B。**
prepareDone 每监听核独占 2048B，区域步长为 2048B；serverReady 每监听核独占 512B，步长为 512B。
这里的间距指起点距离，不要求在两个独占区域之间再额外留出 512B 空洞。
两级控制 Flag 均每槽占 32B，仅首个 4B FP32 有效，发布完整 32B，求和不包含其他 7 个 float。
新控制状态在固定 96KiB 预算内安排独立宏并校验，不能改变当前表边界。

M0/Init 统一建立以下地址，Host 和设备端引用同一组公共宏；这些是地址契约，不是新接口：

```text
bufferStride = uint64(CommArgs.peerMemBytes) / 2
bufferIndex = uint64(magic) % 2
bufferBase(rank) = peer[rank] + bufferIndex*bufferStride
combineControlBase(rank) = bufferBase(rank) + COMBINE_CONTROL_OFFSET
prepareDoneBase(rank) = combineControlBase(rank) + COMBINE_PREPARE_DONE_OFFSET
serverReadyBase(rank) = combineControlBase(rank) + COMBINE_SERVER_READY_OFFSET
SHARED_DATA_OFFSET = TILEXR_EP_SERVER_DEDUP_SHARED_DATA_OFFSET  // 固定 1572864B
resultBase(rank) = bufferBase(rank) + SHARED_DATA_OFFSET
stageBase(rank) = resultBase(rank) + alignUp(uint64(BS)*K*Sresult,512)
Sstage = alignUp(uint64(H)*2,512)
```

因此结果区在 buffer 0 的地址为 `peer[rank]+1.5MiB`，在 buffer 1 为
`peer[rank]+peerMemBytes/2+1.5MiB`；中转区只在该结果区后加上对齐后的接收区大小，
不再额外加一次控制区或 1.5MiB。1.5MiB 为 1572864B（0x180000），不是 1000000B。
控制区合计 1340KiB，后面 196KiB 保持留空；不纳入 Combine 的 96KiB 控制预算或 stage。

上述大写名称简写自联合设计中 TILEXR_EP_SERVER_DEDUP_ 前缀的对应宏；不把 offset 或
单份控制区大小重新按 BS/分核计算。Sresult 由 CB03-02 已确认的非量化返回格式计算。
同一调用各 rank 使用一致的布局配置，peer 基址与 bufferStride 均须满足 512B 对齐。
Combine 可独立检查自身数据视图能放入半区，无需解析 Dispatch 控制区；共同通信域的
max(Ddata,Cdata) 容量契约仍保留。stage 与结果槽同时存活，M3 返回不能覆盖 M4 尚在读取的 stage。

**Sresult 已确认：** 沿用 Dispatch 非量化 DataAsFlag 包块，每块为 480B payload+32B Flag，
DIRECT/partial 均只承载 H 个 BF16/FP16 元素，不附加 Dispatch 的包头、转发索引或量化 scale。
同次调用通信 dtype 与 expertOut 的 dtype 一致；最终输出遵循现有目标 dtype 契约，不新增混合 dtype 参数。

```text
P = uint64(H) * 2               // 有效 payload 字节数
N = ceilDiv(P, 480)             // flagBlockCount，去掉包尾后重新计算
Sresult = N * 512               // 最后不足 480B 的承载块也占一个完整包块
```

槽内按上述完整包块连续排列，槽间没有额外空隙。末块补齐不参与运算，不附加有效字段。
当前 Memory Combine 非量化路径采用相同长度推导；量化扩展仍仅记录在附录 A。

按已确认的非量化返回格式，取 R=128、E=384、BS=256、K=16、H=7168：
P=14336B，N=30，Sresult=15360B（15KiB）。

| Combine 区域 | 计算 | 每份 buffer 空间 |
| --- | --- | --- |
| 结果接收区 | 4096 槽 * 15360B | 60MiB |
| 中转区 | 98304 行 * alignUp(14336,512)B | 1344MiB |
| Combine 数据需求 | 接收区 + 中转区 | 1404MiB |

上述为已确认格式在该 shape 下的首版容量示例。
完整需求仍须取 max(Ddata,1404MiB)，再加共用数据区之前的固定 1.5MiB（含控制区及留空），
得到 singleBufferBytes，再分别核对两个半区容量及 runtime 保留边界。不能将 1404MiB 直接视为单份共用数据区的最终值。
在本例中，相对所选 buffer 的结果区为 [1.5MiB,61.5MiB)，中转区为 [61.5MiB,1405.5MiB)。
1405.5MiB 仅为 Combine 视图含前缀的需求；若 Ddata 更大，完整共用布局仍取较大值。
旧 relayBytes=8*BS*K*returnStride 是历史预留，不能代替新的中转区。

验收需验证 Host/Dispatch/Combine 的区域布局一致且步长均为 peerMemBytes/2、表 2 写槽与表 3 读槽一致、
两个 buffer 以及各内部区域不重叠、DataAsFlag 与 stage 行 stride 区分正确、最高地址不侵入保留区。
当前 run.sh chain 每卡显式分配 8GiB 逻辑通信空间，分配源码及通用 API 默认值见联合设计 §6.3。

### M1a Init

**功能预期：** 全部 64 核执行 Init，建立本核参数、地址、状态和资源；从 sendCounts 末项取得
TokenNum，为后续本地拷贝及 Send/Read 处理提供执行上下文。

**已确认项：** 全 64 核执行 Init，在设备侧读取计数末项，建立角色、固定宏地址、本次整体 bufferIndex 及资源上下文。
控制区采用 Host 首次清零、kernel 消费方用后立即清零；Init 不让 64 核重复清理共享 GM。
CB1A-01 已确认首版 M4/M5 以 PIPE_ALL 分阶段隔离，并按常驻、公共和阶段专用资源规划 UB；具体容量、lane 配额和 Event 编号仍待核算。
**当前 M1a 初始化方案已收口：CB1A-02、CB1A-03 关闭设计待定，保留实现验证；
CB1A-01 正在讨论，未视为完成。** 各模块 Event/流水依赖及附录遗留仍在各自条目维护。

#### 控制 Flag 清零覆盖核对（CB1A-02）

Host 清理必须在任何 rank 发布本轮 Flag 前完成，覆盖两个整体 buffer 内各自完整的
96KiB Combine 控制区，包含 serverReady 的全部 64 个预留块，而非只清本次有效块。
现有 [TileXRComm::InitMem](../../src/comm/tilexr_comm.cpp) 在分配后通过 aclrtMemset
清零整个 allocation，随后 InitCommMem 才交换并打开 peer 映射；新建通信域可复用这次初始化。
正常复用不要求 Host 每轮清零；已用通信域若需要重新初始化，必须先确保旧读写全部结束，
不能与 kernel 的发布并发。异常退出不能继承“上一轮已清理”的前提。

| 区域 | 发布方 | 消费及当场清零责任 | 覆盖结论与完成条件 |
| --- | --- | --- | --- |
| prepareDone[8][64]，16KiB（每槽 32B） | 64 核各写 8 行的自身列 | 核 56+j 检测第 j 行 sum=64，保存完成状态后，立即清自身 2048B 行 | 8 核覆盖整表；不能提前清部分到达项，也不能由发布核写后自清；零 type3 任务核仍发布，通知核仍汇总并清理 |
| serverReady，固定 32KiB | Server 内 8 卡分别写每个有效 Read 块的来源槽 | Read 核检测自身块 sum=8，保存 ready 状态后，立即清该 512B 块；M4 使用本核保存的状态 | 每个有效块唯一 owner；无 type2 或 BS 任务的 Read 核也必须等待并清理；不再依赖被清掉的 GM 值 |
| 扩展预留，48KiB | 当前无写入方 | Host 初始化为零，kernel 无活动记录可消费 | 不需要每轮清理；未来新增控制表必须同时定义发布、消费、清理 owner 和完成依赖 |
| M5 结果槽 DataAsFlag（位于数据区） | M3/M4 的远端返回 | 本核全部有效副本确认到齐、最后一次 probe 结束后，复用 Flag 检测 UB 置零，只清本核全部 K 槽的 32B Flag，可与剩余 payload 读取/聚合重叠 | 不依赖 mask，不写 480B payload；保存 ready 后不再 probe；首次初始化和布局变化归 CB1A-02，完成依赖见已确认 CB05-03及 CB06-02 |

**控制区覆盖结论：** 在正常完成及既定模型调度下，上述两类活动控制 Flag 可完整覆盖。
recvCoreNum 变化时，新启用的块要么从未使用、保持 Host 初始零值，要么以前已由其 owner 清零；
本次不活动的块没有写入方，无需额外清理。切换整体 buffer 采用相同原则，不新增内部双 bank。
“当场清零”指消费后立即发起清理；UB 清零源在 MTE3 写完前不能复用，且 M6 必须保证这些
清理实际完成后才结束。两级 Flag 的依赖原则已由 CB02-03 确认；具体 Event 随模块实现落实，整体排空按 CB06-02 已确认方案执行。

**结果 Flag 初始化/跨布局复用列为重点遗留，当前不展开。** 用户确认每次测试前新建通信
buffer 并清零，首次结果 Flag 的零值由整块初始化提供；第一阶段空 Combine 不发布或消费
结果 Flag，本项不阻塞当前功能接入和验收。后续未重新清零的通信域复用、BS/K/H 或数据
角色变化等边界保留在[附录 B](#deferred-result-flags)，待处理遗留问题时单独展开。
这不改变已确认的 kernel 用后清理职责，不增加每轮 Host 清零或新握手。

**待确定项：** 分级见[规则](#待确定项分级规则)，推荐均待确认。

| 编号 | 风险 / 参考 | 待确定内容 | 推荐设计 | 依据与核对重点 |
| --- | --- | --- | --- | --- |
| <a id="cb1a-01"></a>CB1A-01 | 高 / 部分 | 各角色 UB/Event 总预算及生命周期 | **阶段隔离与资源分类已确认，具体预算待定。** 首版 M4/M5 保留 PIPE_ALL，分别管理阶段内 UB/Event；按下述常驻区、公共区、阶段专用区规划复用。继续核算各角色峰值、lane 配额及 Event 编号，保持合法输入至少一 lane 的前提。 | [R3](#参考依据目录)/[R4](#参考依据目录)；有动态 lane 参考，新模块组合的最坏 H/K/行跨度需重新核算。 |

#### CB1A-01 已确认：先隔离阶段，再按性能证据考虑融合

这里的两步是资源设计与优化的推进顺序，不改变既有 P2.1–P2.4 模块开发顺序。
首版 M4、M5 仍为独立模块，由同一组 Read 核依次执行；默认 32 核，不写死为 32。
M4 结束后保留一次本核 `AscendC::PipeBarrier<PIPE_ALL>()`，然后才进入 M5 的资源初始化、
表加载和 Flag 检测。两模块的 UB 使用生命周期、EventID 管理和流水状态在时间上独立，
以便分别定位读取、聚合、发送与最终接收问题；首版不要求跨阶段延续在途任务或融合调度。

**UB 按资源职责规划，空间复用不等于继承旧内容或旧流水状态：**

| 资源类别 | 内容与规划 | 生命周期及复用边界 |
| --- | --- | --- |
| 常驻资源 | 跨模块仍需保留的上下文，以及尚有通知/清理在途时使用的控制缓冲；Scalar 参数不重复计入 UB | 不被阶段工作区覆盖；M2 已确认的检测、work、sum、清理、通知独立缓冲策略保持不变，各源缓冲保留至实际消费者完成 |
| 公共资源 | M4/M5 的输入 lane、FP32 转换/乘法/累加区、低精度工作区、独立输出环形 lane | 统一考虑物理空间复用，分别建立模块内视图、lane 状态和事件；M4 排空后 M5 才可覆盖，重新初始化必要数据，不继承 M4 的累加结果或权重 |
| 阶段专用资源 | M4 的 type2 列表及表 2/3 缓存；M5 的 pendingTokenIds、表 4/5、Flag/work/sumOut | 两阶段可使用同一物理区域，容量覆盖各自峰值；M5 内部 probe/清理零值源仍与 payload、FP32 和输出区独立，清理完成前不得覆盖 |

固定分区方案按 `常驻区 + 公共区 + max(M4专用区, M5专用区)` 核算；公共区的每种缓冲
按两模块所需容量较大值规划，包含对齐和访问余量，不能先让 M4 占满 UB 后再额外加入 M5 probe。
这只是 Read 核 M4/M5 的预算表达式，M1b、M2 及 Send 核 M3 的峰值仍须分别纳入全流程预算。
首版不强制两模块使用相同 lane 数或连续轮转下标，实际配额按 UB/Event 预算落实。

公共区复用保留模块语义差异：M4 连续读取 self/stage 的有效 payload，输出拼接 DataAsFlag；
M5 从结果槽跳过 Flag 读取 payload，输出连续 yOut。若输出物理 lane 按较大的 M4 包容量预留，
M5 仍只写出有效 H 个目标 dtype 元素，不发送旧 Flag 或尾部空间。任务列表、权重来源和数值顺序
继续分别遵守 CB04-01/CB04-03/CB05-01，公共缓冲并不要求首版合并模块实现。

**首版切换顺序与 Event 规则：**

```text
M4：提交本核全部任务 → 完成阶段 Event 配对收尾
    → PipeBarrier<PIPE_ALL>()
M5：建立本阶段 UB 视图/必要初始化及 Event 状态 → 加载表 4/5、构造 pending → probe/聚合/输出
```

PIPE_ALL 必须位于 M5 第一次覆盖复用 UB 之前，不能等加载表或初始化已经覆盖 M4 数据后才执行。
屏障只等待本核流水，不代表其他核切换完成、远端已经消费或 M5 输入全部到达；M5 仍按既定协议监听。
PIPE_ALL 不代替 SetFlag/WaitFlag 配对，不自动清除 Event 或重置队列管理状态。M4 的释放事件
（含预置但未使用的 lane 事件）按实际协议收尾后，M5 再建立自己的事件状态；完成收尾后允许复用
相同方向的 EventID，无需将两模块编号永久分开，也不要求保留跨阶段事件状态。
是否调用 Reset 由实际资源管理方式决定，不以 Reset 覆盖仍需保留的常驻资源。
M5 内 probe 与 payload/输出流水可能同时存活，仍须避免它们的 Event 冲突。

屏障仅位于阶段边界，不增加逐 Token 的 PIPE_ALL，也不改变 M5 在 Scalar 提交完成后立即
删除 pending 条目的规则。各模块内部继续使用已确认的环形 lane 和复用前等待机制。
本项确认的是阶段隔离及 UB 分类原则；精确字节数、offset、lane 配额和 Event 分配尚未完成，
CB1A-01 保留待定。后续移除屏障及更深度融合仅记入[附录 D 后续优化方案](#deferred-m4-m5-fusion)，首版不要求实现。

<a id="cb1a-02"></a>
**CB1A-02 已收口（当前设计范围）：** Host 在首次使用前清零两个 buffer 的完整控制区，
kernel 消费方按 M2/M5 已定职责清理。当前测试采用新建并清零的通信 buffer，
已有 InitMem 接线可复用；接线及完成时序保留为实现验证，不再作为 M1a 的方案选择。
结果 Flag 跨布局/已用域复用仍为附录 B 重点遗留，未因本项关闭而认定解决；
M2 按 CB02-03 已定依赖实现，整体清理排空按 CB06-02 已确认方案执行。

<a id="cb1a-03"></a>
**CB1A-03 已收口：** M1a 不统一预置未使用的 lane Event 或寄存器状态。
实际使用阶段中，首次先 Wait 的 lane 预置一次释放事件；先 Set 再 Wait 的路径不另加预置。
实际执行 Reset 前确认旧任务排空，再建立新阶段资源；空 Combine 不初始化未使用的收发流水。
具体 Event/UB 生命周期由 CB1A-01 及 M1b/M3/M4/M5 的流水设计承接，结束排空按 CB06-02 已确认方案执行；
量化专用状态仍归附录 A。关闭本项不删除这些依赖，也不宣称目标实现已经验证。

**CB1A-03 的 Dispatch 对照：** 顶层 Init 主要记录上下文，不能据此判断没有阶段初始化。
当前 Dispatch 将以下资源操作放在实际使用阶段；非量化收发路径也经过 lane Event 预置。

| 当前源码路径 | 实际操作 | 对 Combine 的约束 |
| --- | --- | --- |
| [Sender::Process / SendPacket](../../src/ep/kernels/tilexr_ep_server_dedup_send.h) | 每个输出 lane 预置 MTE3_V；发包时先 Wait，MTE3 使用结束后再 Set 释放 | 采用同样“先等空闲再写入”的环形流水时，需要预置首次可用状态，不能因暂不量化而删除 |
| [Receiver::Process / IssueLoad](../../src/ep/kernels/tilexr_ep_server_dedup_recv.h) | Process 预置各 lane 的 MTE3_MTE2；IssueLoad 首先 Wait，消费完成后 Set 释放 | 同样属于阶段内流水初始化，不要求在顶层 M1a 提前建立 |
| [Dispatch::Process](../../src/ep/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.h) 与 Receiver::Process | Count 阶段后先 PIPE_ALL，进入接收阶段再 Reset、InitBuffer | 仅在实际切换并复用资源时安排；不要求所有阶段无条件 Reset |
| [Counts::Init / Finish](../../src/ep/kernels/tilexr_ep_server_dedup_counts.h) | 保存 CTRL[60]，设为 0，结束时恢复；注释说明服务于将整数位型按 FP32 subnormal 求和的 Count 算法 | Combine 的完成 Flag 为普通 1.0f，不能仅因参考 Dispatch 就照搬此 Count 专用设置 |

保留的是首次 Wait 必须有对应事件、复用前旧读写必须结束的依赖要求，不新增统一寄存器
初始化流程。UB 数据本身的必要初始化（例如 M5 尾部清理所用零值区）由使用模块负责，
与是否预置硬件 Event 是不同职责；Host 清通信区仍归 CB1A-02。

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
没有无效占位。第一阶段按不超过 10000 行的有效范围构造和筛选行号列表，
不为超大列表设计两遍统计/分批保留方案；核间分配仍按完整 type3Count 计算。
具体 API 的指令次数限制与临时区需求按合法输入落实，不将其等同于附录 C 的超限算法。

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
原始元素；M1b 仅写 stage 行内前 H*2B，M4 也仅读取这 H*2B。
行尾 `[H*2,Sstage)` 对齐空隙既不写入也不读取，无需初始化；不能因为 stage 有补齐
就从 expertOut 的下一行多读，也不能在 M4 中搬入整条补齐后的 stage 行。
目标地址为 stageBase(self)+uint64(offset)*Sstage，其中 stageBase 由 M0 公共布局取得。

<a id="cb1b-02"></a>
**CB1B-02 已确认：有界表段缓存与按剩余 UB 配置的共用环形 lane。**

**表 2 获取：** 使用单份、有界的行范围缓存。

本核按递增的 packedRows 遍历，从尚未处理的首个原始行号开始，选取缓存容量能覆盖
的一批任务，读取其最小至最大原始行号对应的完整表 2 行段。每行 3 个 uint32，共 12B；
type3 仅使用第一个字段，但按连续完整行搬运，不逐 Token 发起 4B metadata 读取。
例如本批行号为 `[23,29,41,66]`，读取 `[23,66]` 共 44 行，以
`table2Cache[row-cacheStartRow][0]` 得到 stageSlot。任务数量少不代表行号跨度小，
跨度超过缓存容量时继续分批，不要求将本核全部跨度或全表常驻 UB。

表缓存首版采用单份；新表段搬入后，等待其对 Scalar 可见再查询 offset。
覆盖旧缓存前须已使用完旧段 metadata；已发出的 payload MTE3 只引用 payload lane，
因此换表段不以这些 MTE3 完成为前提。缓存容量和同时存活资源纳入 CB1A-01。
无 type3 任务的核跳过表段读取和 payload 拷贝，但仍按 CB1B-03 发布八份本核完成 Flag。

**payload 流水：** MTE2 和 MTE3 共用一组环形 lane，每个 lane 同时是搬入目的和写出源，
不另设输出副本，不经过 Vector 加权或格式转换。先扣除任务列表、表 2 缓存、完成 Flag
及其他同时存活资源，再按剩余 UB 和 Event 资源上限确定 laneCount；不固定为双缓冲。
合法输入保证至少支持一个 lane，具体 UB 对齐、预算及 Event 分配归 CB1A-01。

本核第 t 个拷贝任务使用 `lane=t%laneCount`，轮转下标跨表缓存批次连续推进。
首次使用按 CB1A-03 建立可用状态；只有在 MTE2 即将再次写入该 lane 前，才等待
上一次 MTE3 读取该 lane 完成。MTE2 搬入 H*2B 有效数据，建立 MTE2→MTE3 就绪依赖后，
由 MTE3 写入对应 stageSlot 的 H*2B 有效范围。写出完成后释放该 lane。
不能在每个 Token 写出后立即等待完成，从而允许不同 lane 上的搬入和写出重叠。
单 lane 时仍遵守相同所有权顺序，不据此承诺可实现搬入/写出重叠。

本核全部任务下发后，按 CB06-02 排空所有实际在途 lane，确认 stage 数据写出完成，
再发布八份 prepareDone。空任务核没有 payload 在途操作，但仍执行完成 Flag 发布。
换表缓存批次不统一排空 payload；若后续阶段要复用资源，另须满足对应最后使用者完成条件。
当前 Dispatch Receiver 的 IssueLoad 可参考 MTE3→MTE2 释放、MTE2→MTE3 就绪依赖，
不照搬其 metadata/转发职责。本项设计待定关闭；lane 回绕、非满批、有效尾块、空任务、
发布顺序及多轮复用进入 CB06-04 验证，UB/Event 总预算仍由 CB1A-01 承接。


当前 Memory Dispatch 的 [TokenActiveMaskCal](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h)
已有比较 mask、CreateVecIndex 和 GatherMask 压缩行号的组合，可作为实现参考。
[官方 GatherMask 文档](https://asc.gitcode.com/api/SIMD-API/basic_api/memory_vector_compute/compare_and_select/GatherMask.html)
说明该操作按 pattern 筛选源元素，rsvdCnt 给出保留数量；本设计以行号序列作为源元素。
官方检索脚本未返回 9.1.0 的对应页面，上述链接用于语义参考，不代表已验证目标重载；
具体数据类型、分批参数及资源排布在本模块继续细化。

<a id="cb1b-03"></a>
**CB1B-03 已确认：32B 独占 Flag 槽，卡内 8×2048B 监听区。**

本核 type3 中转写入完成后，按 CB06-02 建立 payload 先于完成 Flag 的发布依赖，
随后一次 DataCopyPad 发布本核完成状态。无拷贝任务的核也发布。每个 Flag 槽独占 32B，
只有首 4B 的 FP32 `1.0f` 表示完成，其余 28B 不参与判断；UB 可统一置零后设置各槽首值。

本卡 prepareDone 固定起点为 Combine 控制区偏移 0，固定预留 16KiB。物理布局为
`prepareDone[8][64][32B]`：8 个监听区各占 2048B，监听区 j 归通知核 `56+j` 独占。
每个监听区内包含 64 个生产核的 32B 槽，核 c 写入每个监听区的 `c*32B` 位置：

```text
flagAddress(j,c) = prepareDoneBase + j*2048 + c*32
j = 0..7, c = 0..63
```

同一生产核的 8 个目的起点间距为 2048B；同一监听区中不同生产核的槽起点间距为 32B。
不同监听核的区域互不重叠，起点间距 2048B，满足至少 512B 的强制隔离约束。
不使用紧凑 4B GM 槽，也不采用此前未确认的 16 核分组方案。

**一次 DataCopyPad 写 8 槽：** UB 连续准备 8×32B=256B，每块首 float 为 1.0f；
初始化完成后建立到 MTE3 的依赖。本核中转数据写出完成后发布完整 32B 块：

```cpp
// flagUb: 连续 8 个 32B 块，首 float=1.0f；初始化到 MTE3 的依赖已建立。
// 本核 stage payload 写出已完成，发布顺序按 CB06-02 落实。
// 8 块 × 32B，UB 块连续，GM 块间跳过 2016B，目的起点步长 2048B。
const AscendC::DataCopyExtParams flagCopy{8U, 32U, 0U, 2016U, 0U};
AscendC::DataCopyPad(prepareDoneGm[coreId * 8U], flagUb, flagCopy);
// prepareDoneGm 是 float GlobalTensor；coreId*8 个 float 对应 coreId*32B。
```

Flag 源 UB 在该 MTE3 搬运完成前不得改写或释放。每核仅写自己的 32B 槽；同一监听区
中的其他生产核槽不受影响。M2 消费核检测到 64 个槽的首 float 之和为 64.0f 后，
保存完成状态并清理自身完整 2048B 监听区，不能在尚未到齐时提前清除部分槽。

[官方 UB→GM DataCopyPad 文档](https://asc.gitcode.com/api/SIMD-API/basic_api/memory_vector_compute/data_move/DataCopyPad_UBToGM.html)
提供块长及步长单位的语义参考；目标 CANN 9.1/A5 重载、64 核并发和实际性能仍需验证。
本项设计待定关闭，UB/Event 具体预算归 CB1A-01，M2 求和按 CB02-01 已确认流程执行，
目标编译、发布顺序、邻槽保护与多轮清理验证归 CB06-04，不将完整 32B 写出直接视为性能已提升。

<a id="cb1b-01"></a>
**CB1B-01 第一阶段不展开，已迁入专题遗留：** 实际 TokenNum 限定为不超过 10000，
一份 uint32 原始行号列表最多 40000B（约 39.1KiB），筛选后的行号列表不会更大。
这里仅计算单份列表，TokenType、输入/输出列表和工作区的同时存活预算仍归 CB1A-01，
并不代表所有表可一次常驻 UB。第一阶段不要求实现 98304 行全列表或大行数两遍筛选。
超限问题及原方案保留在[附录 C](#deferred-large-token-num)，不计入当前待确定汇总。

**已确认项：** 全范围筛选 type3，密排后按 64 核均分；表 2 单份有界行段缓存，按 offset 暂存，仅搬运 H*2B 有效数据，stage 对齐空隙不读不写；本核 payload 写出完成后发布八份完成记录，无 type3 任务也发布。

**待确定项：** M1b 本模块方案已确认；跨模块 UB/Event 总预算保留在 CB1A-01，实现验证归 CB06-04。

### M2 本卡汇总与 Server ready

**功能预期：** 最后 8 个核（全局 coreId 56..63）各自汇总本卡 64 核的类型 3 拷贝完成记录，
sum=64.0f 后分别通知 Server 内的 8 张卡（包括本卡）。每个 Read 核独立监听本卡属于自己的
512B 卡间同步块，提取 8 个来源卡的完成 Flag，sum=8.0f 后进入 M4，并独立清理自己的 Flag 区。
每个整体通信 buffer 内仅一份卡间同步记录，随本次 Combine 所选整体 buffer 使用，见下文 CB02-02。

**空任务不免除同步：** 每个 Read 核即使没有 M4 汇聚转发或 M5 最终接收任务，
也必须监听自己 512B serverReady 块中的全部 8 个来源 Flag，sum=8 后保存就绪状态，
并清理自己的块。最后 8 个通知核仍须先履行本卡汇总和通知职责。
不能按接收任务数为零提前退出，否则本核 Flag 会残留并污染后续复用。
这里的消费者是已分配 serverReady 块的 Read 核，不为 Send 核新增监听块或 M2 消费职责。

**核映射与本卡汇总：** `recvCoreNum=8*N（N=1..6）`，剩余核作为 Send 核。
初版的 Read 核范围为 `[64-recvCoreNum,64)`，故核 56..63 始终位于 Read 组末尾。
通知序号 `j=coreId-56`，范围 0..7；它在 Read 组内的编号为 `recvCoreNum-8+j`。

```text
通知核 56+j → 本卡 prepareDone[j][0:64] → sum=64.0f → 通知本 Server 的卡 j
```

每个通知核只读取 M1b 为自己准备的完整 2048B 监听区，包含 64 个 32B 槽。
只对各槽首个 FP32 求和，UB 中有效值间距为 32B（8 个 float），不能对全部 512 个 float 求和。
全部核各写入 1.0f 后，和为 64.0f，表示本卡待聚合 Token 已写入本地中转区。
通知核在完整 sum=64 后保存完成状态，并独自清零自己的全部 2048B prepareDone 接收区；其他通知核使用各自独立的副本。
本卡完成通知的发布不等待其他卡 ready，前述 8 个通知核完成通知责任后再进入卡间监听。

**向 Server 内各卡通知：** 这里的来源编号 s=0..7 是 Server 内卡号（serverLocalRank），
不是跨 Server 的编号。设本卡 s，通知核 56+j 负责目标卡 j，则它将 FP32 1.0f 发布到目标卡
当前整体 buffer 中每个 Read 核对应块的第 s 个槽，完整写入 32B，只有首 float 有效。
每个目标 Read 核都得到一份来自 8 张卡的独立记录。
本卡作为目标也执行同样的通知规则；这一步承接本卡 sum=64.0f 的完成含义。

**单份卡间同步布局：** 目标卡为每个 Read 核提供一个 512B 对齐、512B 步长的独立块。
块内的前 256B 划为 8 个 32B 槽，槽 s 的首个 4B 保存来源卡 s 的 FP32 Flag：

| 来源卡 s | 槽在块内的字节偏移 | 有效内容 |
| --- | --- | --- |
| 0 | 0 | 首 4B 为 FP32 Flag |
| 1 | 32 | 首 4B 为 FP32 Flag |
| 2..7 | 64, 96, 128, 160, 192, 224 | 各槽首 4B 为 FP32 Flag |

每槽其余 28B 和块后半段 256B 不参与完成求和，后半段不放活动 Flag。
GM 固定按 64 个块预留，实际只发布、监听本次 R=recvCoreNum 个块；当前 R 仍为 8..48。
块按 Read 组内 LocalID r 密排，`r=coreId-sendCoreNum`，范围 `[0,R)`；
不能直接用全局 coreId 索引本表，也不因预留 64 块就向未参与的块发布。索引及地址关系为：

```text
bufferStride = peerMemBytes / 2
readyReservedBytes = TILEXR_EP_SERVER_DEDUP_COMBINE_SERVER_READY_BYTES  // 固定 32KiB，按 64 个接收核预留
readyValidBytes = recvCoreNum * 512  // 仅前 recvCoreNum 个块有效
serverReadyOffset = TILEXR_EP_SERVER_DEDUP_COMBINE_SERVER_READY_OFFSET  // 固定 16384B
syncBase(target,bufferIndex) = peer[target] + uint64(bufferIndex)*bufferStride
                             + combineControlOffset + serverReadyOffset
blockAddress(target,bufferIndex,r) = syncBase(target,bufferIndex) + r*512
flagAddress(target,bufferIndex,r,s) = blockAddress(target,bufferIndex,r) + s*32
```

serverReadyOffset 相对 Combine 控制区起点，固定为 16384B；recvCoreNum 改变仅影响有效块数，
不改变 offset 或 32KiB 预留跨度。bufferIndex 是本次 Combine 所选整体 buffer。
通知核 56+j 对目标 j 的所有 r 写同一个来源槽 s，
每个来源槽由对应来源卡唯一写入；接收方按自己的 r 监听。

**卡间通知发布已确认：每个通知核一次 DataCopyPad 写目标卡的 R 个来源槽。**
来源卡 s 的通知核 56+j 仍负责目标卡 j（包括 j=s 的本卡路径）；8 个通知核合计覆盖
Server 内 8 张卡。每个核在确认本卡准备完成后，发布的是本卡 s 的完成状态，
目的槽按 s 定位，不按目标卡 j 或生产核 coreId 定位。

```text
// 均为相对目标卡当前 serverReadyBase 的偏移，r=0..R-1。
s=0: 0, 512, 1024, ...
s=1: 32, 544, 1056, ...
s=7: 224, 736, 1248, ...
writeOffset(s,r) = r*512 + s*32
```

UB 连续准备 R×32B，各槽首 FP32=1.0f，其余字节不参与接收判断。发布前完成 UB 初始化
到 MTE3 的依赖；目标地址以当前整体 bufferIndex 和目标卡基址计算。调用示意如下：

```cpp
// targetReadyGm 为目标卡当前 serverReady 的 float GlobalTensor。
// sourceLocalRank=s；R 为目标卡实际 recvCoreNum，不是预留块数 64。
const AscendC::DataCopyExtParams readyCopy{
    static_cast<uint16_t>(R), 32U, 0U, 480U, 0U};
AscendC::DataCopyPad(targetReadyGm[sourceLocalRank * 8U], readyUb, readyCopy);
```

每次完整写 32B，目的块间跳过 480B，起点步长 512B；`sourceLocalRank*8` 个 float
对应来源槽偏移 s*32B。readyUb 在这次 MTE3 完成前不能改写或释放；参数与实际效果
仍需目标编译及设备验证。一次通知不覆盖其他来源卡槽，也不写入每块后半段的空闲 256B。


**每核监听与清理：** 每个 Read 核只监听本卡当前整体 buffer 内的自身 512B 块，
每次取 8 个 32B 槽的首个 float 求和。sum=8.0f 表示本 Server 全部 8 卡的类型 3 数据已准备完成，
本核可以读取中转数据。Read 核保存 ready 状态后，沿用已确认的整块清理，清零自身完整
512B 区域（含其中 8 个 Flag 槽），不触碰其他 LocalID 的区域；M6 承接清理完成与资源回收。
核 56..63 也按其 Read 组编号监听自己的块。512B 独占块及清理职责不会因 M5 后续全核参与而扩大到 Send 核。

**两级 Flag 对照（每份整体 buffer）：** R=recvCoreNum；所有起点间距均为字节距离。

| 项目 | 卡内 prepareDone | Server 内卡间 serverReady |
| --- | --- | --- |
| 生产核数量 | 每卡 64 核 | 每张来源卡 8 核，整个 Server 64 核 |
| 单生产核目的 | 本卡 8 个监听区 | 指定目标卡的 R 个监听区 |
| 单生产核 Flag 数 / UB 大小 | 8 / 8×32B=256B | R / R×32B |
| 单 Flag GM 槽 / 写出大小 | 32B / 32B | 32B / 32B |
| 单生产核 GM 起点步长 / 块间空隙 | 2048B / 2016B | 512B / 480B |
| Flag 有效值 | 首 FP32=1.0f，其余 28B 无语义 | 首 FP32=1.0f，其余 28B 无语义 |
| 消费核数量 | 每卡 8 核，56..63 | 每张目标卡 R 个 Read 核 |
| 单消费核监听 Flag 数 / 起点间距 | 64 / 32B | 8 / 32B |
| 单消费核独占区 / 区域起点步长 | 2048B / 2048B | 512B / 512B，前 256B 放槽 |
| GM 固定预留 / 当前使用 | 8×2048B=16KiB / 全部 | 64×512B=32KiB / R×512B，当前 R≤48 |
| 完成求和 | 64 个首 float，sum=64.0f | 8 个首 float，sum=8.0f |
| 完整消费后清理 | 本监听核清自身全部 2048B | 本监听核清自身全部 512B |

消费核只累加各 32B 槽的首 float；单生产核的写入份数与单消费核等待的来源数不能混用。
卡间每生产核向 R 个 Read 核写入 R 个槽，只有 R=8 时才准备 8×32B UB。
卡间一次 DataCopyPad 的发布方式已确认；两级求和与首版独立 UB 按 CB02-01 已确认流程执行。

<a id="cb02-02"></a>
**CB02-02 已确认：M2 完成 Flag 随整体双 buffer 复制，不再独立双缓冲。**

每个整体通信 buffer 的 Combine 控制区只保留一份 serverReady，固定预留 64*512B=32KiB，
当前仍配置 recvCoreNum=8..48，只有前 recvCoreNum*512B 有效。全域两份固定预留 64KiB，
当前合计有效范围为 8..48KiB；预留 64 块不等于首版支持 recvCoreNum=64。
prepareDone 每个整体 buffer 各一份 16KiB，两份共 32KiB；两级 Flag 固定预留共 96KiB，
本次调用的两份有效记录范围合计为 40..80KiB。
单份 Combine 控制区宏为 96KiB（预留需求 48KiB 的两倍），其他必要控制状态在该预算内安排并校验，
预留余量不作为第二套 Flag，不在单个 buffer 内再次展开 M2 双 bank。

M2 的发布、监听、清理都使用本次 Combine 的整体 bufferIndex，与数据区选择一致。
沿用整体 magic 选窗，不新增 Combine 专用翻转状态；此前“相邻 Combine 必须独立交替 bank”
的描述由本条整体布局取代。连续 Combine 若选到同一半区，仍按既定模型顺序和 owner 清理
完成条件复用，不为满足独立轮换而访问另一份控制区。

两个整体 buffer 内的控制记录均须在首次使用前初始化为 0；Read 核消费后清理本次使用的
512B 块，并在该块再次复用前完成清理。初始化入口和清理依赖分别归 CB1A-02/CB02-03，
物理布局确认不等于这些并发依赖已验证。跨 rank 的 writer/listener 使用一致整体 bufferIndex。

<a id="cb02-03"></a>
**CB02-03 已确认：采用两级 Flag 消费清理的推荐方案。**

第一级为本卡 prepareDone：各核在 type3 中转写入完成后发布自身完成记录；
通知核 56+j 等待自己一行的 64 个槽首 float 的完整 sum=64，保存完成状态后，当场清自己的 2048B 行，
并据已保存的完成状态向目标卡发布 serverReady。未取得完整 sum 前，不清部分已到达槽。

第二级为 Server 内卡间 serverReady：每个 Read 核等待自身 512B 块中八个来源槽
首 float 的完整 sum=8，保存 ready 状态后当场清该块，再按本核 ready 状态读取成员数据。
本轮后续流程不重新依赖已经清零的 GM Flag；无本地拷贝/聚合任务的核仍履行相应同步及清理职责。

清理、通知及后续 DMA 分别建立依赖；相关 UB 源在对应 MTE3 写完前不得改写或释放。
所有操作使用本次整体 bufferIndex，M6 在结束前排空本核通知和清理，确保该 buffer 再用前
写回完成。具体事件与调用随模块实现落实，快慢核、空任务、两份 buffer 及同半区复用
作为验证场景保留；设计确认不等于已完成设备验证。
本项不包含 M5 的 DataAsFlag 清理；CB02-01 的统一检测及通知发布、CB1B-03 的 32B 发布布局均已确认，具体调用进入实现验证。

**已确认项：** 56..63 各汇总本卡 64 槽并通知一张卡；Read 核独占 512B，sum=8 后可读 stage 并自行清理；M2 完成 Flag 每个整体 buffer 内一份，随整体选窗使用。

<a id="cb02-01"></a>
**CB02-01 已确认：两级统一检测流程，首版 UB 独立分配、不复用。**

卡内与卡间共用同一检测流程，传入监听区 GM 基址、Flag 数量 B 和预期和。
每轮一次搬入全部有效槽，随后只累加各 32B 槽的首 FP32；不先压缩首值，也不将
槽内其余 28B 或卡间块的空闲后半段纳入归约。

| 项目 | 卡内 prepareDone | 卡间 serverReady |
| --- | --- | --- |
| 检测搬入范围 | 自身完整 2048B 监听区 | 自身 512B 块的前 256B |
| Flag 数量 B / 预期和 | 64 / 64.0f | 8 / 8.0f |
| UB 有效首值位置 | flagUb[i*8]，i=0..63 | flagUb[i*8]，i=0..7 |
| 检测输入 UB 的有效数据量 | 2048B | 256B |
| 归约 work 初步预算 | 256B | 32B |
| 归约结果区初步预算 | 独立 32B，仅首 float 存结果 | 独立 32B，仅首 float 存结果 |
| 独立清理零值 UB | 2048B | 512B |
| 完整到齐后的清理范围 | 本核独占的全部 2048B | 本核独占的全部 512B |

归约调用采用连续 mask 重载：

```cpp
// 本次 B*32B Flag 已搬入，MTE2→Vector 依赖已完成。
AscendC::ReduceSum<float>(sumOut, flagUb, work, 1, B, 1);
// mask=1：每 repeat 只选首 float；repeat=B；srcRepStride=1 个 32B 块。
// 建立 Vector→Scalar 依赖后读取 sumOut[0]，与 float(B) 比较。
```

执行顺序固定为：一次 GM→UB 搬入 → MTE2→Vector → 一次 ReduceSum → Vector→Scalar
→ 读取结果。未到齐时重新读取自身监听区，不提前清理部分已到达 Flag；开始下一轮覆盖
检测 UB 前，上一轮归约和结果读取已经完成。到齐后先保存完成状态并停止监听，再用
独立零值 UB 清理自身区域。卡内通知核依据保存状态执行已确认的卡间通知，不能再读取
已清零 GM 来决定是否发布。通知前完成发送源 UB 初始化依赖；每通知核一次 DataCopyPad
向目标卡实际 R 个块写入完整 32B，源 UB 写完才可释放；两级流程按 CB06-01/CB06-02
已定顺序推进和排空。

**UB 策略：** 两级检测各自的输入、work、sumOut、清理零值区和通知发送源均独立分配，
首版不做地址别名或跨用途复用。只有最后 8 个通知核需要卡内检测和卡间通知资源；
所有 Read 核需要卡间检测资源。反复检测同一阶段可使用该阶段自身缓冲，保持上述依赖。
零值区初始化完成后建立到 MTE3 的依赖，直到清理完成前保持不变；通知源同样保持到
对应写出完成。后续 CB1A-01 汇总这些独立资源和 Event，不在本阶段引入 UB 复用优化。
这一决定只针对 M2；M5 的检测 UB 转零值源方案仍按 CB05-03 执行。

上述 work 预算依据[官方 ReduceSum 文档](https://asc.gitcode.com/api/SIMD-API/basic_api/memory_vector_compute/reduction_compute/ReduceSum.html)
的高维切分临时空间公式：float 下 alignUp(B,8)*4B。当前
[Memory Combine](../../src/ep/kernels/tilexr_ep_combine_memory_kernel.h) 的 CheckPackedFlagRangeArrive
和 [Dispatch Count](../../src/ep/kernels/tilexr_ep_server_dedup_counts.h) 的 WaitDispatch 有相同
首 float 归约调用参考。一次 API 调用内部可以包含多次迭代，不等于一条硬件指令。
表中输入量不含目标实现可能要求的尾部访问余量；源、目的、work 的最终容量及对齐
须结合 CANN 9.1/A5 目标头文件及编译核对。该核对归实现验证，不重开已确认流程。

CB02-01 不再列为设计待定；CB06-04 验证未到齐/到齐、非首 float 干扰、首值求和、
通知邻槽保护、清理范围及多轮复用。M2 的设计待定已收口，跨模块总预算仍由 CB1A-01 承接；
本次未实现 kernel，也未运行目标编译或设备验证。

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
rowIndex，不能将筛选出的类型值 1 作为发送索引。第一阶段筛选范围最多 10000 行，任务划分
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

<a id="cb03-01"></a>
**CB03-01 已确认：单份有界表段缓存，GM 非对齐起点整段搬入，按原始行号差索引。**

每个 Send 核按自己递增的 directRows 段遍历。设缓存能容纳 W 行，从下一任务的原始
行号开始切批，使 `lastRow-baseRow+1<=W`，读取本批首尾之间的连续表 2 行段。
切批依据原始行号跨度，而非任务数量；跨度中的非 type1 行可随表段搬入，但不成为任务。
最后一批按实际范围读取，不向 GM 末尾扩读以填满缓存。缓存容量纳入 CB1A-01 总预算。

**整段 GM→UB 搬入：** 表 2 行距保持 12B，GM 起点不要求 32B 对齐。
使用支持非对齐源地址的 DataCopyPad GM→UB 路径，UB 起点保持 32B 对齐。
单个表段作为一个连续数据块搬运，`blockCount=1`、`blockLen=(lastRow-baseRow+1)*12`，
不把每行作为独立的 12B 块。无左侧 padding，有效数据从 UB 起点开始，UB 内行距仍为 12B；
只在整段末尾保留到 32B 对齐所需的空间，该尾部不作为表条目查询。
无需为了 GM 32B 对齐向前扩读，也无需把每行填充到 32B。

例如实际读取段为闭区间 `[22,66]`：

```text
table2BaseRow = 22                  // 保存的是原始起始行号，不是字节偏移
gmStart      = table2GmBase + 22*12 // 字节地址：偏移 264B
gmEnd        = table2GmBase + 67*12 // 排他结束地址：偏移 804B
copyBytes    = (66-22+1)*12 = 540
ubBytes      = alignUp(540,32) = 544
```

搬运后 UB 前 540B 是完整的 45 条连续表项，末尾 4B 为搬运补齐，不参与索引。
若本批任务最小行号本身是 23，可以直接从 23 行起读；22 只是本例实际读取起点，
不要求每批固定减 1。对实际负责的 type1 Token，按以下关系获取三个字段：

```text
row = directRows[i]                          // i 属于本核本批任务段
localRow = row - table2BaseRow
entryByteOffset = localRow*12                // 字节视角
entryWordOffset = localRow*3                 // uint32/int32 tensor 元素视角
srcRank    = table2Ub[entryWordOffset + 0]
srcTokenId = table2Ub[entryWordOffset + 1]
srcTopk    = table2Ub[entryWordOffset + 2]
payload = expertOut[row]
```

因此 row=22 查询 UB 字节 0，row=23 查询字节 12，row=66 查询字节 528 至 539。
密排列表下标不替代原始 row；一维 32 位 LocalTensor 访问必须乘 3，不能将 localRow
直接用作字段元素下标。无 DIRECT 任务时跳过表读取，不访问空列表首尾。

**缓存和发送流水独立：** 搬入表段后等待 MTE2→Scalar 可见，再读取三字段计算目的地址。
旧批次 metadata 均已使用后可覆盖单份缓存；已提交的 payload MTE3 引用独立发送 lane，
换表段不统一排空发送，也不将 lane 轮转序号重置为 0。lane 复用按 CB03-02 执行，
整个 M3 结束按 M6 排空。首版表缓存不引入额外双缓冲。

API 依据：[官方 GM→UB DataCopyPad 文档](https://asc.gitcode.com/api/SIMD-API/basic_api/memory_vector_compute/data_move/DataCopyPad_GMToUB.html)
规定 GM 源地址按 1B、UB 目的地址按 32B 对齐，非对齐 blockLen 的块尾在 UB 补齐。
当前 M1b 的 [CopyLocalPendingTokens](../../src/ep/kernels/tilexr_ep_combine_memory_server_dedup_kernel.h)
已有 `table_[cacheBegin*3]` 起点、单块 `(cacheEnd-cacheBegin)*12B` 的搬运及相对行号索引参考。
官方检索脚本精确过滤 9.1.0 本次返回 0 条、无页面获取错误；所引为当前官方页面，
不是目标版本匹配页，目标 CANN 9.1/A5 的重载和实际访问仍随实现验证，不能据此宣称 M3 已验证。
本项设计待定关闭，开发只按 CB06-04 单轮数据/Flag 与模块解析范围验收。

<a id="cb03-02"></a>
**CB03-02 已确认：沿用 Dispatch 非量化打包/发送流水，去掉包尾额外信息。**

DIRECT 的有效 payload 是 expertOut[row] 的 H 个原始 BF16/FP16 元素；partial 为 M4
加权归约后转换回同一通信 dtype 的 H 个元素。每个 512B 包块保持 480B payload+32B Flag，
Flag 沿用 Dispatch 的 ready=1.0f 设置及已确认的首 float 到达判断；无 32B 路由包头、
K*8B 转发索引或 scale，N=ceilDiv(2H,480)、Sresult=N*512。
不能仅跳过 FillTail 却继续使用 Dispatch 的 tokenStride：去掉包尾后须重新推导包长和 UB 跨度。

发送目的由表 2 的 srcRank/srcTokenId/srcTopk 确定，地址为
`resultBase(srcRank)+(srcTokenId*K+srcTopk)*Sresult`，不使用 Dispatch 的 source/expert/q 收包槽公式。
结果接收方已有表 4/5 和固定槽地址，不需要随包重复发送路由、类型或 weight。

输入及输出 lane 沿用 Dispatch 的环形复用和完成依赖：payload 就绪后由 Vector 拼接，
等待 lane 的 MTE3 释放后写入，V→MTE3 就绪后将整包写至目的 GM，写完发布 lane 可复用状态。
去掉包尾标量处理后仅保留实际生产者/消费者的依赖，不机械保留 FillTail 专用步骤。
输入读取限于 H 个有效元素；若 Vector 打包覆盖末块补齐，UB 须覆盖完整 N*480B 读取跨度。
末块无效字节沿用 Dispatch 的无语义补齐，接收和加权仅处理 H 个元素；不越界读取 expertOut。
M4 在 FP32 累加后接入同一打包/发送格式，读取、累加与输出流水按 CB04-02 已确认方案执行。

参考 [Dispatch Sender](../../src/ep/kernels/tilexr_ep_server_dedup_send.h) 的 Process/SendPacket
和 [Dispatch 布局](../../src/ep/common/ep_memory_server_dedup_data_layout.h)。上述职责和格式已收口；
尾块、lane 复用、发送完成和地址覆盖作为实现验证保留，不新增业务输入表或随包字段。

**已确认项：** type1 密排后按 Send 核均分，只缓存本核行号范围的表 2，未加权 expertOut 返回 source。

**待确定项：** M3 模块方案已确认；表缓存与 lane 的整体 UB/Event 预算归 CB1A-01，实施验收沿用 CB06-04。

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
`r=coreId-sendCoreNum`，取值 `[0,P)`；第一阶段在最多 10000 行内筛选，以总 T 均分：

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

<a id="cb04-01"></a>
**CB04-01 已确认：两表同批、同 baseRow、分别缓存，沿用 source/expert/q 成员寻址。**

**共同分批：** 表 2、表 3 各保留一份独立 UB 缓存，使用相同的批次 `[baseRow,lastRow]`。
按本核递增的 type2Rows 和原始行号跨度切批，行数上限取两份缓存可容纳行数的较小值，
不按密排任务数量估算占用，也不直接照搬 M3 的缓存行数。两表分别连续搬入，
不需要为 GM 起点的 32B 对齐而向前扩读；UB 起点分别对齐，行间不插 padding，
每份缓存仅在整段末尾保留搬运所需的对齐空间，具体方式沿用 CB03-01。

| 表 | GM 起始字节偏移 | 有效搬运字节数 | UB 内行距 |
| --- | --- | --- | --- |
| 表 2 | `uint64(baseRow)*12` | `(lastRow-baseRow+1)*12` | 12B |
| 表 3 | `uint64(baseRow)*K*8` | `(lastRow-baseRow+1)*K*8` | K*8B |

例如两表均读取 `[22,66]`、K=16，有效搬运量分别为 540B 和 5760B；
表 2 缓存计入段尾对齐需 544B，表 3 为 5760B。两份表缓存先纳入 M4 固定资源预算，
再按 CB04-02 为输入/输出 lane 分配空间，整体预算归 CB1A-01。
末批只搬实际范围；无 type2 任务时跳过表读取，但仍履行 M2/M5/M6 责任。

**共同索引：** 对本批任务列表中的原始 row，两表使用同一个 `localRow=row-baseRow`：

```text
row = type2Rows[i]
localRow = row - baseRow
dstEntry = table2UbBase + localRow*12        // 字节地址
relayRow = table3UbBase + localRow*(K*8)     // 字节地址
```

连续范围中的非 type2 行可以随搬运进入缓存，但不生成任务，也不解析其表 3 无效内容。
若使用 32 位 LocalTensor 索引，表 2 行起点是 `localRow*3`，表 3 行起点是 `localRow*K*2`；
不能将原始 row、密排下标或字节偏移直接混作元素下标。

**表 3 解析：** 第 0 个 8B 块前 4B 按完整 uint32 读取 N，后 4B 按 FP32 位型读取
selfWeight；不得将 header 前 4B 按两个 uint16 解码。第 1..N 块各解析 uint16 srcExp、
uint16 q 和 FP32 weight，第 N+1..K−1 块不消费。N 是额外成员数，范围 1..K−1，
总贡献数为 N+1；self 只计一次，K=1 时不存在 type2。
零 selfWeight/memberWeight 仍合法，首版正常读取、加权，不以 weight=0 判条目无效或跳过成员。

**成员读取地址：** 使用表 2 的原始 sourceRank 和表 3 的全局 expertId/q，沿用联合设计
[表 3 契约](ep-memory-server-dedup-dispatch-combine-design.md#表-3转发-token-读取索引表)。
以下 rankSize 是总卡数，L 是每卡专家数，不是 Read 核数：

```text
sourceRank  = DestinationIndex[row][0]
expertId    = RelayReadIndex[row][memberIndex].srcExp
q           = RelayReadIndex[row][memberIndex].q
memberRank  = expertId / L
localExpert = expertId % L
slot = (((uint64(sourceRank)+memberRank)%rankSize)*L + localExpert)*BS + q
readAddress = stageBase(memberRank) + slot*Sstage
```

所有地址乘积使用 uint64。slot 必须等于成员卡 type3 表 2 保存的 offset，
使 M1b 写入与 M4 读取闭合；sourceRank 不得替换成当前 relay 卡号。
self 从当前 expertOut[row] 读取；第 1..N 个成员统一从 stage 读取，即使 memberRank
等于本卡也不改用 expertOut 索引。本核 M2 ready 后即可读，不新增逐成员 Flag 检测。
每次只读 H*2B 原始 BF16/FP16 payload，stage 行尾对齐空隙不读取。
按 self 后表内成员顺序 FP32 加权累加，完成后转换为通信 dtype；使用表 2 的
srcRank/srcTokenId/srcTopk 返回 partial，其结果槽地址和 DataAsFlag 格式按 CB03-02。

**缓存复用：** 两份表搬入完成并对 Scalar 可见后才解析。旧批次的路由、成员索引及
权重全部消费完后才覆盖缓存，不能仅凭成员 payload 搬运已提交就认定权重也已使用完。
已发出的 partial MTE3 只引用独立输出 lane，换表批次不统一等待这些发送结束，
也不重置输入/输出 lane 轮转序号。各 lane 按 CB04-02 的所有权依赖复用，M4 结束
按 CB06-02 排空。本项设计待定关闭；开发验收遵守 CB06-04 的单轮数据/Flag 及模块解析范围，
不因本次确认增加压力测试或内部细打点，也不将方案确认记为设备验证通过。

<a id="cb04-03"></a>
**CB04-03 已确认：先转 FP32，再加权求和，完成归约后转换输出。**

M4 对自身 expertOut 和每个 stage 成员先转 FP32，再乘表 3 的对应 FP32 weight，
以 FP32 累加全部贡献。整个 Token 的局部和完成后，转换为本次通信 dtype（BF16/FP16），
按 CB03-02 打包并转发；不在每次成员累加后转回低精度。

M5 对表 4 置位的每个有效返回副本先转 FP32，再乘表 5 对应的 FP32 weight，
以 FP32 累加。全部有效副本聚合完成后，最后一次转换为目标 dtype，再拷贝到 yOut。
DIRECT 的路由权重在 M5 应用；partial 的表 5 权重为 1.0，其成员权重已在 M4 应用，不能重复加权。

```text
M4: partial32 = sum(FP32(contribution[j]) * memberWeight[j])  // 含 self
    partial = cast_to_bf16_or_fp16(partial32)
    pack_and_send(partial)
M5: result32 = sum(FP32(received[k]) * SourceWeight[p,k])     // 仅 mask 置位 k
    yOut[p] = cast_to_target_dtype(result32)
```

实现转换沿用 Memory Combine 的非量化路径作为参考：升 FP32 使用 CAST_NONE，
转回 BF16/FP16 使用 CAST_RINT；保持明确的 FP32 乘法、累加与转换边界。
参考顺序采用 M4 self 后按表 3 成员顺序、M5 按有效 k 递增处理，具体流水不得改变加权职责。
数值 oracle 必须复现 M4 的低精度 partial 转换及 M5 的最终转换，不将两级结果要求为
全程 FP32 一次全 K 归约的逐位相同值。BF16/FP16 舍入点和误差容差由 CB06-04 的验证落实，
不重新作为通信格式待选项；精度策略确认不代表目标编译和设备精度验证已经通过。

<a id="cb04-02"></a>
**CB04-02 已确认：按剩余 UB 扩展输入 lane，采用独立环形输出 lane。**

聚合主体参考 [Memory Combine](../../src/ep/kernels/tilexr_ep_combine_memory_kernel.h)
的 ProcessMoeExpert：输入保持 BF16/FP16，依次转 FP32、乘 weight、累加到独立 FP32 区。
一个 type2 Token 始终由同一核处理，按 self 后表 3 成员顺序累加，遵守 CB04-03 的舍入边界。
M2 ready 已确认成员数据完整，M4 不新增逐成员 Flag 轮询。

**输入流水：** 输入 lane 在 MTE2 与 Vector 之间轮转，编号从 0 到 inputLaneCount−1
循环。MTE2 向空闲 lane 预取 self 或 stage 成员，Vector 等待对应搬入完成后消费，
其对该 lane 的最后一次读取完成后才允许 MTE2 再次覆盖。输入 lane 不等待 partial 的
MTE3 发送，因为输出位于独立缓冲。成员可提前搬入，FP32 累加顺序不随搬入进度改变。
输入源读取 H 个有效元素；stage 行尾对齐空隙不发起读取，不照搬 Memory 从 DataAsFlag 槽跳过 Flag 的输入寻址。

inputLaneCount 根据剩余 UB 扩展，不沿用 Memory CalcReceiveBufferNum 的固定 1/2 选择。
先计入表 2/3 缓存、任务列表、FP32 转换/乘法工作区、累加区、低精度转换/打包工作区、
独立输出 lane 及其他同时存活资源，再计算可容纳的输入 lane。实际 lane 数还须满足目标
队列/Event 资源上限；不能为了增加输入 lane 挤占输出缓冲。输入/输出预算分配、具体
lane 数公式及 Event 编号归 CB1A-01 汇总，本项不新增固定双缓冲或固定 lane 数。

**输出流水：** partial 按 CB03-02 拼接 DataAsFlag 后，存放在独立环形输出 lane，
其生产者为 Vector、消费者为 MTE3。第 t 个 partial 使用 `outLane=t%outputLaneCount`。
只有在即将再次写入该 outLane（包括写其 payload 或 Flag）之前，才等待其上次 MTE3
发送完成；输出就绪后建立 Vector→MTE3 依赖并下发整包发送。写出完成后，该 lane
重新可用。首次使用的 lane 按既定阶段初始化原则处于可用状态。

不在每个新 Token 的聚合入口统一等待上一 Token 的 MTE3。上一 Token 的 FP32 累加区
被最终转换读取完后即可按 Vector 依赖清零，开始下一 Token 聚合；若另设低精度打包
工作区，须等待打包对其读取完成后才能覆盖。MTE3 只读取独立输出 lane，不再引用这些
工作区。因此上一 Token 的发送可以与下一 Token 的 MTE2 搬入和 Vector 聚合重叠。
若轮转到的输出 lane 尚未释放，仅在实际需要写该 lane 时等待，不把该等待提前到
下一 Token 开始聚合的位置。

| 缓冲 | 生产者 → 消费者 | 再次写入前的完成条件 |
| --- | --- | --- |
| 输入 lane | MTE2 → Vector | 上一次 Vector 对该输入 lane 的读取完成 |
| FP32 转换/乘法工作区 | Vector → Vector | 上一次加权/累加已完成对该工作区的读取 |
| FP32 累加区 | Vector 累加 → Vector 最终转换 | 最终转换对旧局部和的读取完成，不等待 MTE3 |
| 低精度转换/打包工作区（若使用） | Vector 转换 → Vector 打包 | 打包读取完成，不等待 MTE3 |
| 输出 lane | Vector 打包 → MTE3 | 该 lane 的上次发送完成，仅在再次写该 lane 前等待 |

Memory 当前 LocalWindowCopy 使用单个 tokenBuf_ 输出，ProcessExpert 在每 Token 开始
有 MTE3→Vector 等待；这部分不能原样迁移，否则仍会阻挡下一 Token 的聚合。
保留 Memory 的 FP32 聚合与输入队列管理参考，输出衔接 Dispatch 的环形打包发送机制。
源码已有双缓冲并不证明新主动读取/转发流水的重叠效果，需以目标设备验证为准。

CB04-02 的流水结构已确认，不再列为设计待定。表缓存和成员寻址按 CB04-01 已确认方案执行，
全核 UB/Event 总预算仍归 CB1A-01。M4 全部任务结束后按 CB06-02 排空本核读取、计算及
所有输出 lane；首版通过阶段 Event 收尾及 PIPE_ALL 建立边界，再进入 M5 的资源初始化和表加载。
公共 UB 可复用，但两模块的事件/流水状态独立；不能因取消逐 Token 的 MTE3 等待而省略阶段末排空。
CB06-04 验证多次 lane 回绕、不同成员数、不同 H、尾块、任务为空及 M4→M5 复用，
上一 Token 发送与下一 Token 聚合的重叠测量留到性能专项，不作为当前模块验收门槛。本节仅确认设计，不代表 kernel 验证通过。

**已确认项：** type2 密排后按 Read 核均分，表 2/3 局部缓存；M2 ready 后读取成员，self+成员加权返回 partial。

**待确定项：** M4 模块方案已确认；跨模块 UB/Event 总预算保留在 CB1A-01，实施验收沿用 CB06-04。

### M5 最终本卡 Token 聚合

**功能预期：** 独立函数 AggregateSourceTokens 负责本卡 BS 个原始 Token：按表 4 mask
接收有效结果槽，按表 5 加权归约并写 yOut。首版由 Read 核执行；模块通过参与核的逻辑编号和
数量划分任务，与 Send/Read 核类别解耦，保留后续改为全部 64 核参与的能力。

首版在 M4 的 Event 收尾及 PIPE_ALL 完成后才初始化 M5、覆盖复用 UB 或加载表 4/5；
UB 按 CB1A-01 的常驻/公共/阶段专用资源规划，M5 的 lane 和 Event 状态独立管理。

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
不放入 Combine 的 96KiB 控制区，也不使用 stage 的原始行 stride。

**表 4/5 局部缓存：** 开始监听前，将表 4 的 `[begin[r],end[r])` 和表 5 的
`[begin[r],end[r])×[0,K)` 搬入 UB，有效数据量分别为 `count[r]*4B`、`count[r]*K*4B`。
遍历 TokenId=p 时用 `p-begin[r]` 索引两张缓存，不加载其他核负责的表段。

<a id="cb05-01"></a>
**CB05-01 已确认：密排待处理 TokenId 表，处理完成后末项替换删除；每个 Token 的 mask 非零。**

本核负责 M 个原始 Token，范围为 `[begin,begin+M)`（即用户描述的 `[N,N+M-1]`）。
先在 UB 构造 uint32 待处理表，保存真实 TokenId，不另外分配已完成状态表：

```text
pendingTokenIds = [begin, begin+1, ..., begin+M-1]
pendingCount = M
scan = 0
```

任意时刻只有前 pendingCount 项有效，长度单调缩短。表 4/5 保持前文约定的本核原始
连续段缓存，不随待处理表的交换而重排。取 `p=pendingTokenIds[scan]` 后，以
`localIdx=p-begin` 查询 `maskUb[localIdx]`、`weightUb[localIdx*K+k]`；不能用 scan
直接索引两表。列表有效区外的旧值不读取、不要求清零。

**逐 Token 扫描与删除：** 当前 Token 未到齐时 scan++，超过有效长度后回到 0；
到齐后先保存本次 ready 结论；Scalar 完成该 Token 的参数处理及相关聚合/yOut 指令提交后，
即可从列表删除，不等待 Vector/MTE2/MTE3 执行完成。删除操作本身不新增 Scalar 与其他
流水间的 SetFlag/WaitFlag 或 PipeBarrier；pending 仅为 Scalar 调度元数据，不是 payload
缓冲的释放凭据。probe 结果读取、各 lane/工作区复用和阶段排空仍遵守各自已定依赖。
删除使用末项覆盖当前位置，再缩短有效长度，不移动其余元素。被换入当前位置的任务
仍待处理，因此删除后不递增 scan，继续检查当前位置；若删掉的是最后一项，下一次
循环按缩短后的长度回绕。每个 Token 仅聚合一次，不再扫描已删除任务。

```text
while pendingCount > 0:
    if scan >= pendingCount:
        scan = 0
    p = pendingTokenIds[scan]
    if not Probe(p, maskUb[p-begin]):
        scan += 1
        continue
    // 此次 probe 已结束，p 的全部有效副本已确认到齐。
    if pendingCount == 1:
        // 本核最后一个未处理 Token；之后不会再 probe。
        // 按 CB05-03 复用 Flag UB，允许清理与 p 的后续 payload/聚合重叠。
        StartFlagCleanupForOriginalTokenRange()
    SubmitTokenWork(p)  // Scalar 完成指令提交即可返回，不为删除额外等待流水。
    last = pendingCount - 1
    if scan != last:
        pendingTokenIds[scan] = pendingTokenIds[last]
    pendingCount = last
    // 不执行 scan++，下一次检查换入项。
```

例如 `[N,N+1,N+2,N+3]` 中 N+1 的 Scalar 提交流程完成后变为 `[N,N+3,N+2]`，有效长度从 4 变为 3；
下次在同一位置检查 N+3。跨 Token 处理顺序可以改变，但每个 Token 内仍按原始 k 升序
加权，输出始终写 yOut[p]。pending 表占 M*4B 加对齐空间，有效长度和扫描位置为核内
Scalar 状态；不新增跨核/GM 状态或公共输入，具体 UB/Event 预算归 CB1A-01。

参考 [Dispatch RemovePending](../../src/ep/kernels/tilexr_ep_server_dedup_receive_tasks.h)
的末项替换方式及 Receiver 先 ConsumeLoaded 再删除的所有权顺序；M5 列表直接存原始
TokenId，不照搬 Dispatch 的任务间接索引或批量 probe。单套 probe 缓冲仍按 CB05-02 复用。

**提前清理衔接：** 当 pendingCount=1 且该 Token probe 成功时，之前删除任务的到齐结论
及 Scalar 提交流程均已完成，但 Vector/MTE3 仍可能在途；
本核全部应到达数据已确认到齐。此时保存 ready，按 CB05-03 将 Flag UB 的完整 K*N*32B
置零并清理原始 `[begin,end)` 全部 Token 槽，允许与最后一个 Token 的剩余处理重叠。
该任务仍在 Scalar 提交流程结束后才删除，删除不等待其流水执行完成；不能提前删除它
来触发清理，也不必等 pendingCount=0
才开始清理。清理遍历使用保留的原始 begin/end，不使用已经压缩的 pending 列表。
pendingCount=0 仅表示所有任务的 Scalar 处理及指令提交已结束，不代表 Vector 或 MTE 已执行完；
实际在途任务完成由各缓冲复用依赖及 M6 排空保证。

**非零 mask 契约：** 合法输入中每个 p∈[0,BS) 的 SourceMask[p] 在 K 个有效位中至少
有一位为 1，因此 popcount(mask)>=1，probe 的 B=N*popcount(mask)>0。
不设计 mask=0 的空和/补零输出分支，不用零次数 ReduceSum 推断到齐；这不限制真实
加权结果数值为零。权重为零的有效副本仍按 mask 等待并接收，不以权重判断接收义务。
无本核任务（M=0）时跳过 M5 表缓存、probe、聚合及清理，但此前 M2 同步责任不变。
本卡 expert 输出行数 TokenNum=0 也不能跳过本卡 BS 原始 Token 的 M5 接收。

本项设计待定关闭，开发验收沿用 CB06-04 的正常轮次与模块级解析；不新增压力矩阵或细打点。

**按 mask 选择副本并密排 Flag：** 采用用户明确的新方案：在搬运阶段遍历表 4 的置位 k，
逐个副本拷贝其全部 N 个 32B Flag 区，在 UB 中连续追加，跳过未置位副本。
N 表示单个结果副本的 Flag 区数量，实现中命名为 flagBlockCount，与 M4 的成员数 N 无关。
令 `m=popcount(SourceMask[p])`、`B=m*N`，本次 probe 的 UB 有效区为 B 个连续 32B 块。
例如 K=6、mask=0b001001，UB 依次存放 k=0 的 N 块、k=3 的 N 块，共 2*N 块。
不再采用全 K 槽拷贝、按 n/k 重排及动态展开 sourceMask 的归约方案。

这里密排的是 **32B Flag 块**，块内首 float 仍相隔 8 个 FP32 元素；无需压缩成连续的 4B
标量数组。读取首 float、步进一个块即可求和，后续原始 k 与权重的对应仍通过表 4/5 获取。

<a id="cb05-02"></a>
**CB05-02 已确认：单套 probe 缓冲逐 Token 复用，按完整 K 预留，一次 ReduceSum 检测一个 Token。**

每个参与 M5 的核使用一套 Flag/work/sumOut 缓冲，首版不增加多个 Token 的并行 probe
缓冲。当前 Token 的 Flag 搬入、求和和 Scalar 结果读取结束后，才允许下一 Token 覆盖
该套缓冲。未到齐则检查下一个未完成 Token，不停在单个 Token 上反复等待；到齐后按
既定流程聚合并按 CB05-01 从密排待处理表删除；合法 mask 至少有一位置 1。

令 `N=ceilDiv(H*2,480)`、`Bmax=K*N`、`B=popcount(mask)*N`，按本次调用的完整 K
预留空间，不按当前 Token 的有效副本数缩减分配，也不乘以本核 Token 数：

| 缓冲 | 预留大小 | 使用范围与职责 |
| --- | --- | --- |
| flagUb（即 packedFlags） | `32*Bmax` 字节，另加目标实现要求的尾部访问余量 | probe 实际搬入前 `32*B` 字节；最后复用为清理零值源 |
| reduceWork | `alignUp(Bmax*4,32)` 字节 | FP32 高维 ReduceSum 中间结果，与 Flag 区独立 |
| sumOut | 独立 32B | 首 float 保存本次总和，Scalar 在 Vector 完成后读取 |

work 公式沿用官方高维 ReduceSum 工作区规则及 Memory Combine 参考；目标 CANN 9.1/A5
对齐、尾部访问余量及实际参数范围仍由实现核对。H=7168、K=16 时 N=30、Bmax=480，
flagUb 有效容量 15360B、work 1920B、sumOut 32B，共 17312B（另计访问余量和其他资源）。
具体 UB offset、Event 编号和跨模块总预算归 CB1A-01。

每次非空 probe 连续提交 m 个有效副本的多块 Flag 搬运，统一建立 MTE2→Vector 依赖；
随后**恰好一次 ReduceSum 调用**完成该 Token 全部有效副本、全部 N 个 Flag 区的首 float
求和，再建立 Vector→Scalar 依赖读取结果。不按 k 分别求和后再汇总，也不逐副本同步。
一次调用可包含内部多个计算步骤，不等同于一条硬件指令；合法非零 mask 保证 B>0。

最后一次 probe 结束且本核全部有效副本已确认到齐后，按 CB05-03 将 flagUb 的完整
`K*N*32B` 范围置零，不能只置零最后一次 probe 的 `m*N*32B`，否则清理无效副本时
可能把旧 Flag 值重新写回 GM。该零值区只供清理 MTE3 读取，与仍在使用的 payload、
累加和输出缓冲独立，全部清理写出完成前不得覆盖或释放；每 Token 一次 DataCopyPad，
只写各 32B Flag 区，不触及 480B payload。清理启动后不再 probe，沿用保存的 ready 状态。

本项设计待定关闭。稀疏/全 K mask、连续不同 B 的缓冲复用、未到齐继续扫描、
全范围置零和多轮复用进入 CB06-04 验证；方案确认不代表目标编译或硬件验证已经通过。

**已确认的首 float 完成契约：** 用户确认总线协议层保证数据写入完整性，每个 32B Flag 区
只检查首个 float，其余 7 个 float 不参与检测。各区首值为本轮 FP32 的 0/1，所有有效副本
具有同一 N，检测目标固定为：

```text
flagSum = sum(packedFlags[b][0] for b in [0,B))
expected = B = N * popcount(SourceMask[p])
ready = (flagSum == expected)
```

合法输入的 mask 非零，每次实际 Token probe 均有至少一个有效副本。

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
只有后续性能专项发现 M5 存在瓶颈时，再拆分指令下发、等待搬入、归约的代价及 probe 次数；
开发验收仅要求 M5 模块级耗时解析，不新增内部细打点。当前无真实设备耗时数据，不给出微秒级预测。

目标为 CANN 9.1/A5。此前 API 调研时，版本过滤的官方检索脚本未返回 9.1.0 ReduceSum 页面（count=0，
无页面获取错误），当时所查常用安装路径及仓库 env/cann 未找到目标头文件；这是历史调研记录，
本次文档通读未重新执行环境或 API 验证。上述官方链接为
asc-devkit master 文档，列明 Ascend 950 支持。尚未用目标版本头文件、编译或设备验证，
不能将接口语义推导视为目标环境验证通过。后续核对头文件为
`basic_api/kernel_operator_vec_reduce_intf.h`；具体实现需满足 UB 对齐、临时空间和搬运依赖。

<a id="cb05-03"></a>
#### M5 尾部统一清理 DataAsFlag（CB05-03 已确认）

清理启动点提前至本核全部有效副本确认到齐、最后一次 probe 结束，不再等待全部 payload
读取、加权聚合或 yOut 写出完成。“最后一个 Token”按实际监听进度定义，不是最大 TokenId：
本核已无仍需 probe 的 Token，相关 ready 状态已保存，后续不再检查 GM Flag。
所有实际 Token 的 mask 均非零；无 Token 任务的核不提交清理。

最后一次 Flag 搬入、归约和结果读取结束后，Flag 检测 UB 不再用于 probe，直接转为清理
零值源。先对其有效清理区置零并建立 Vector→MTE3 依赖，即可逐 Token 下发清理，
与最后一个 Token 的剩余副本搬入、FP32 加权聚合及输出处理重叠。不要求等待其他核，
也不为清理等待本核 payload 的 MTE2 读取结束。

这一重叠依赖已确认的总线到达契约：ready 表示本轮对应数据已经完整写入；全部本核有效
槽都已到齐，无效槽本轮没有 writer。清理严格只写每个 512B 块的 `[480,512)`，
不改变 `[0,480)` 的 payload。后续即使按整个 512B 块搬入，也只解包 payload，
不得再消费所带 Flag 或将其重新作为就绪条件。payload、FP32 累加和输出缓冲与此零值源
保持独立，不能因检测 UB 生命周期结束而被别名覆盖。

清理 MTE3 可与 payload MTE2、Vector 计算重叠；输出和清理共享 MTE3，仍按该引擎队列
执行，不承诺两类写出彼此同时执行或一定缩短耗时。M6 仍负责确认 yOut、清理及其他在途
写出完成后才能结束本核。

清理范围为本核连续 Token 段 `[begin,end)` 的**全部 K 个副本槽、每槽全部 N 个 32B Flag 区**。
不查询 sourceMask，不判断哪些 k 本轮实际被写入，也不只清首个 float；清理遍历原始
Token 区间，不随 pending 列表缩短。聚合阶段仍只读取有效副本；清理启动后没有新的 probe，尚未完成聚合的
Token 使用已保存的 ready 状态，清零不会让本轮重新等待或重复聚合。
此边界要求已收齐所有应到达写入，无效槽按协议没有本轮 writer；不引入额外全核清理屏障。

**已确认：每 Token 一次 DataCopyPad，Token 间无同步。** CB03-02 已确定
480B payload+32B Flag、`Sresult=N*512B` 且槽间无额外空隙，单 Token 的 K*N 个
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
// 本核全部有效副本已确认到齐；无 Token 任务时跳过，后续不再 probe。
// 最后一次 Flag MTE2/归约/结果读取已结束，ready 已保存。
// zeroUb 复用 Flag 检测 UB，32B 对齐，至少 K*N*32B；与 payload/累加/输出区独立。
// 置零后在循环前建立一次 V→MTE3 依赖；无需等待剩余 payload 读取/聚合/输出。
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
在相邻调用间同步，也不逐次重新置零。每个块仅写 32B，GM 块间跳过 480B；
不能用覆盖连续 K*Sresult 的置零替代此拷贝。最后一个 Token 的清理写入结束地址恰为
`resultBase + end*K*Sresult`，不会进入相邻核 Token 或 stage。每核提交 `end-begin` 次
DataCopyPad，与 mask 中置位数量无关；不再采用将本核所有 Token 合并成一条命令的方案。
上述连续步长由 CB03-02 已定槽格式保证；后续若扩展 wire，必须同步更新清理寻址。

**UB 复用：** 普通重载的 `srcStride=0` 表示连续读取 UB 块，零值源需求为 `K*N*32B`，
不随本核 Token 数增加。已确认复用 Flag 检测 UB，该区按最坏 m=K 预留至少
K*N*32B（另保留 probe API 所需访问余量），不能只按最后一个 Token 的 m*N 分配。
最后一次 probe 的所有读写结束后，将完整 K*N*32B 置零（不能只置零最后一次 m*N*32B），清理期间保持该源区不变，全部清理写回完成后
才允许覆盖或释放；这是从 probe 到清理的用途切换，不是提前 FreeTensor 后继续引用。按 M0 已定非量化格式，H=7168、N=30、K=16 时，需要 15360B
（15KiB）零值区，每条命令处理 480 个 Flag；即使本核负责 32 个 Token，也只是连续提交
32 次调用，共用同一块 15KiB 零值区。复用对象、容量下界和交接依赖已确认，
CB1A-01 只汇总其具体 UB offset/Event 和跨模块总预算；不新增独立清理 UB，
也不为本核全部 Token 预留成倍零值空间。

**API 依据与验证边界：** 现有 Memory Combine 的 ClearPackedTokenFlags 使用
`DataCopyExtParams{N,32,0,480,0}`，但按单 Token 的每个 k 分别调用；新设计在尾部将单 Token 的 K 个槽合为一次，Token 间循环提交。
官方 [9.1 beta2 DataCopyPad](https://www.hiascend.com/doc_center/source/zh/CANNCommunityEdition/910beta2/API/ascendcopapi/atlasascendc_api_07_0265.html)
给出 UB→GM 原型和上述步长单位，通用 blockCount 上限写为 4095；
[asc-devkit master 的 UB→GM 页面](https://asc.gitcode.com/api/SIMD-API/basic_api/memory_vector_compute/data_move/DataCopyPad_UBToGM.html)
则区分 950 的 65535 与 A2/A3 的 4095，不能把这些不同版本上限混用。
本次目标仍为 CANN 9.1.0/A5；本地未找到目标安装头文件，官方检索脚本按 9.1.0 精确筛选返回 count=0、fetch_error_count=0，
未取得最终版匹配页。单次块数上限须以目标 `kernel_struct_data_copy.h`、对应实现和编译验证定稿，
不把 uint16_t 字段宽度当作硬件上限；实现时核对的是单 Token 的 K*N，
而非本核所有 Token 的总 Flag 数。目前确认逐 Token 调用及重叠设计，未运行目标编译或设备验证。
实现验证归 CB06-04：核对目标块数限制，并在 payload 仍在读取时清 Flag，检查 480B
数据区逐字节不变、稀疏 mask/乱序到达无误，以及多轮复用和最终输出正确。
CB05-03 的设计待定关闭；整体在途排空按 CB06-02 已确认方案执行。

**已确认项：** BS 整 Token 分核，预载本核表 4/5；未到齐就检查下一个；有效 k 的 Flag 密排后一次首 float 求和。本核全部有效副本确认到齐、probe 结束后，复用检测 UB 置零，按每 Token 一次 DataCopyPad 只清全部 K 槽的 32B Flag，与剩余数据读取/聚合重叠，Token 间无同步。

**待确定项：** M5 模块方案已确认；跨模块 UB/Event 总预算保留在 CB1A-01，实施验收沿用 CB06-04。

### M6 调度、资源衔接与整体结束

**功能预期：** 串联全核 Init/本地拷贝、Send/Read 分工、ready、DIRECT/group 处理和最终
source 聚合，保证任务持续推进，并在本核所有发送、输出和在途搬运责任完成后结束。
M5 后续采用 64 核时，也由本模块安排 Send 核完成发送后进入最终聚合的路径。

**已确认项：** 串联 M1–M5，M5 在本核全部有效副本确认到齐、probe 结束后提前清理所有负责槽的 Flag，与剩余读取/聚合重叠，在本核发送、输出、清理及在途操作完成后结束；首版 M5 仅 Read 核，全核版保留映射能力。

<a id="cb06-01"></a>
**CB06-01 已确认：采用各核独立推进的阶段调度。**

- Send 核完成自身 M1b 本地拷贝及完成 Flag 发布后，直接进入 M3，不等待其他核完成准备。
- Read 核完成自身 M2 责任并确认 Server 内数据就绪后，执行全部本核 M4 汇聚转发；
  首版完成阶段 Event 收尾及 PIPE_ALL 后进入 M5，不等待其他 Read 核切换阶段。首版不交替执行 M4/M5。
- 核 56..63 先完成本核负责的 prepareDone 汇总及向目标卡的通知，再进入自身
  serverReady 监听，不能先阻塞在依赖其他通知核推进的等待中。
- M3/M4 直接写入目的卡预留结果槽，发送不依赖目的卡已经进入 M5，也不等待对方消费。
  M5 按 CB05-03 执行已确认的提前清理，与本核剩余读取/聚合重叠。
- 全部 64 核即使无 type3 拷贝任务也发布 M1b 完成 Flag；每个 Read 核即使无接收任务
  也消费自身 serverReady 的全部 8 个 Flag 并清理，不按 M4/M5 任务数提前退出。
  空任务仅跳过对应的数据循环，通知核仍完成汇总及通知责任。

上述顺序作为实现约束，CB06-01 不再列为设计待定。阶段切换及最终退出的具体排空责任
按 CB06-02 已确认方案执行；CB06-04 验证任务倾斜、空任务和通知进度，检查实际等待依赖不成环。
已有 Memory 先发后收流程可作局部参考，尚不能据此宣称新调度已通过设备验证。

<a id="cb06-02"></a>
**CB06-02 已确认：发布前建立完成依赖，阶段复用及本核退出前排空。**

| 边界 | 已确认的完成要求及处理 |
| --- | --- |
| M1b 发布 prepareDone 前 | 本核 type3 中转数据写出完成后才能发布完成 Flag；拷贝命令提交完不代表数据写入完成。 |
| M2 → M4 | 本核通知责任已履行，serverReady 已确认并保存；复用通知或清理 UB 前等待其 MTE3 读取完成。使用独立缓冲的清理可继续在途，退出前统一确认完成。 |
| M4 → M5 | 首版先完成 M4 Event 配对收尾，再执行 PIPE_ALL，确认本核全部读取、计算及 partial 发送完成；之后才允许 M5 初始化、覆盖复用 UB、重建 Event 或按需 Reset。两模块流水状态独立，不等待其他 Read 核切阶段。 |
| M5 → 本核结束 | 本核全部结果聚合完成，所有 yOut 写出及本核全部结果 Flag 清理完成；保留 CB05-03 的提前清理与剩余读取/计算重叠，末尾统一等待在途操作完成。 |
| Send 核结束 | 本核全部 DIRECT 发送和此前控制记录写出完成后即可退出，不等待 Read 核完成 M5；无 M5 任务不等于可以提前释放发送 UB。 |

分别判断“有效副本到齐”“聚合计算完成”“输出/清理写出完成”。到齐仅允许按 CB05-03
启动提前清理，不能作为结束判据；最后一条 DMA 已下发也不等于该 DMA 已完成。
Flag 检测 UB 转为零值源后，直到最后一次清理 MTE3 完成前保持不变。各阶段的其他 UB
同样按最后一个实际使用者的完成状态复用，不能只看 Scalar 已走出任务循环。

各核独立排空并退出，不新增全核结束屏障或 Server 读完握手。Send 核退出后，其已写入
的 GM 中转数据继续保留，供其他卡 M4 读取；本核退出不清理或释放该中转区。
跨算子/跨轮复用继续沿用已确认的模型调度保证，不从本核排空推断远端已消费。
空任务核仍完成所属发布、通知、监听和清理责任，再按实际在途操作排空。

具体 Event 编号、UB offset 及 Reset 接线随 CB1A-01 和模块实现落实；本项的完成边界及
资源释放原则不再作为设计待定。CB06-04 验证异步写出、任务倾斜、空任务及多轮复用，
区分本核命令提交与真实完成，确认 M5 清理未覆盖 480B payload，且返回前输出和清理均已结束。
当前 Dispatch 的阶段复用前排空、各核本地完成后结束可作为参考；本次仅确认设计，
未据此声明新 Combine 已通过编译或设备验证。

<a id="cb06-03"></a>
**CB06-03 已确认：默认 Send/Recv=32/32，沿用 Dispatch 的编译期核数配置方式。**

当前 Dispatch 的入口是
[公共 Count 配置头](../../src/ep/common/ep_memory_server_dedup_count_layout.h)中的
`TILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM`，默认 32，接收核数取 64 减发送核数；
[公开接口说明](../../src/include/tilexr_ep.h)明确同一 communicator 生命周期内所有 rank
保持相同且固定的配置。它不是运行时 Host 参数或 tiling 选择项。

Combine 采用同样的入口形式，以独立编译期宏配置发送核数，避免与 Dispatch 的配置和
取值范围耦合；以下是设计定义，尚未添加到源码：

```cpp
#ifndef TILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM
#define TILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM 32U
#endif
// Host/device 引用同一配置定义。
// sendCoreNum = TILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM
// recvCoreNum = 64U - sendCoreNum
```

总核数固定 64。Combine 的合法 sendCoreNum 为 16/24/32/40/48/56，
对应 recvCoreNum=48/40/32/24/16/8；编译期检查合法范围和 8 的倍数。
Dispatch 当前发送核数上限为 48；这里只参考配置形式，不将其上限套用到 Combine，
也不扩展首版 Combine 的 recvCoreNum 至 64。serverReady 的 64 块仅是固定 GM 预留。

默认映射为 Send core 0..31、Read core 32..63；Read LocalID=coreId-32，
通知核固定 56..63，对应 Read LocalID 24..31。其他合法配置保持前 Send、后 Read 的映射，
并由同一个核数配置驱动 M3/M4/M5 任务划分、M2 通知块数及 LocalID 地址计算。
不根据单卡 type1/type2 数量在设备端独立改变分核，也不新增自动选择公式。

同一 communicator 生命周期内所有卡必须使用相同且固定的 Combine 分核策略。
调整宏后同步重编译、部署 Host 与 kernel，使各 rank 的布局计算和实际二进制一致；
这是构建/部署契约，未增加用于协商核数的卡间通信或运行时公共参数。此一致性针对各卡
的 Combine 配置，不要求 Dispatch 和 Combine 必须采用同一个分核值。

32/32 为默认开发配置，不宣称最优。后续性能专项在同样输入与测量条件下比较合法配置，以完整
Combine 耗时选择，模块耗时用于解释瓶颈；性能实测不改变已确认的任务归属和通信布局。
本项不再列为设计待定；CB06-04 验证编译期范围、Host/kernel 配置一致、不同合法分核下的
任务覆盖及 Flag 寻址。首版运行仍固定默认配置，调优需更改构建配置并重编译。

<a id="cb06-04"></a>
**CB06-04 已确认：开发验收检查独立正确性与采集功能，性能专项和压力测试后置。**

| 模块 | 独立正确性判据 |
| --- | --- |
| M1/M2 | type3 原始行到中转槽映射及有效 payload 正确、对齐空隙保持；空任务核仍发布/消费，两级 Flag 按协议到齐及清理 |
| M3 | 单轮 type1 精确落到 source/token/k 结果槽，数据与 Flag 到达符合预期，有效 payload 保持未加权输入 |
| M4 | 单轮 self/成员无遗漏或重复，成员寻址与权重正确，FP32 累加后转换得到正确 partial，DIRECT/partial 数据和 Flag 到达符合预期 |
| M5 | mask 有效副本到齐后聚合，未到齐转向其他 Token；最终 yOut 正确，尾部清理只改 Flag，不破坏 payload |

M4/M5 参考值由测试侧根据原始路由、expertOut 和权重独立计算，不复用生产寻址/归约
函数生成期望；包括 M4 的 BF16/FP16 中间舍入和 M5 最终舍入。具体数值判据在对应模块
实现时落实，保留 Host 及目标 CANN/A5 编译检查，不把替身通过当作设备正确性证据。

**开发功能范围：** 现阶段确保 1 轮、2 轮及普通多轮功能正确，不为捕捉偶现残留额外
增加逐轮变化输入、故障注入或长时间重复执行的专项矩阵。M3/M4 尚无 M5 消费阶段，
明确例外为只检查 1 轮下数据/Flag 到达；不清理结果 Flag，不增加测试侧清理助手，
不要求在同一未清理域连续多轮发送。M1/M2 的控制区生产清理职责不变。
M3/M4 的 performance/breakdown 同样采用无预热单轮，检查采集和模块解析功能；
M5 完成后再验完整流程的 1/2/多轮。具体执行安排见[模块验收流程](../EP_MEMORY_SERVER_DEDUP_VALIDATION_FLOW.md#combine-phase2-modular-acceptance)。

**开发性能范围：** performance 能取得有效耗时数据、breakdown 能正确解析模块级耗时
和 trace 即可；按 M1a、M1b、M2、M3、M4、M5 及必要 M6 收尾划分，不要求模块内部
的 load/metadata/output、逐 Token/probe 或逐引擎细打点。不设本阶段性能基线、延迟/
带宽达标线或调优门槛。已有 Dispatch 的采集、统计、输出和错误分类逻辑沿用；
算子正常跑完但数值校验失败时，仍保留性能及 trace，终端单行汇总，详细日志写 run 路径。

**后续专项：** 功能开发完成后由性能专项负责基线、达标判断和优化，仅针对已发现瓶颈
模块增加内部打点。所有功能和性能均达标后，再开展压力测试，例如单轮测试重复执行
1 万次、双轮测试重复执行 1 万次；压力脚本和详细矩阵届时设计，当前不实现、不运行，
也不以压力验证作为当前开发交付门槛。普通轮次通过不推断已经证明低概率问题不存在。

本项设计待定关闭。其他模块引用 CB06-04 的回绕、清理、发布等检查仍是正确性判据，
在本节轮次和模块边界内验收；性能重叠测量和长时稳定性分别归后续专项，不扩张开发矩阵。
已完成的历史开发检查保留证据，本次收口不要求为了满足旧的细粒度建议追加测试或打点。

### 第一阶段 breakdown 记录隔离（已确认）

扩充标准 profiling 记录空间，为 Dispatch 和 Combine 分别提供独立 base 和不重叠的容量。
记录区独立于 peer-memory 通信窗口，不占用 Combine 96KiB 控制区或共用数据区。
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
[CB00-03](#cb00-03) 首版有权重及未实现特性只接受关闭配置均已确认；CB1A-02 的当前初始化方案、CB1A-03 的阶段内 Event/Reset 原则亦已收口；
CB02-03 两级 Flag 消费清理方案已确认，CB1B-01 的超限场景已迁至附录 C；CB03-02 的非量化 wire/发送流程与 CB04-03 的数值策略已确认；CB05-03 的检测 UB 复用及提前清理已确认；CB06-01 的各核阶段调度已确认；CB06-02 的阶段排空及退出条件已确认；CB1B-03 的 32B Flag 发布及监听区隔离已确认；CB02-01 的统一检测与独立 UB 方案已确认；CB06-03 的默认 32/32 及编译期统一配置已确认；CB04-02 的动态输入 lane 与独立输出环形流水已确认；CB1B-02 的有界表段缓存、共用环形 lane、复用前等待及发布前排空已确认；CB05-02 的单套 probe、完整 K 预算、一次 ReduceSum 及清理复用已确认；CB06-04 的独立正确性、简化开发验收及后置专项已确认；CB03-01 的有界表段缓存、非对齐整段搬入及相对行号索引已确认；CB04-01 的共同表段缓存、成员寻址和缓存复用已确认；CB05-01 的密排 pending 删除、原始 TokenId 索引及非零 mask 契约已确认；以下 **1 项仍有待确认内容**。
量化子问题已移至附录 A，不计入首版待确定项；CB1B-02/CB1B-03/CB02-01/CB03-01/CB03-02/CB04-01/CB04-02/CB04-03/CB05-01/CB05-02/CB05-03/CB06-01/CB06-02/CB06-03/CB06-04 已定策略进入实现验证，不再作为设计待定。
CB1A-02/CB1A-03 已关闭设计待定，CB1B-01 迁为专题遗留；其余条目保留原编号及风险等级。CB1A-01 已确认首版 M4/M5 保留 PIPE_ALL、生命周期独立及常驻/公共/阶段专用资源分类，具体容量、offset、lane 配额和 Event 编号仍待核算；
CB1A-02 的 Host 初始化、kernel 用后清零方向已确认；当前测试新建并清零 buffer，结果 Flag
跨布局/已用域复用列为附录 B 重点遗留，不阻塞第一阶段。其余覆盖与依赖见该节。控制区大小、
小表 offset、M2 副本数及 stage 原始 dtype/行对齐已经确认。初始化/发布/清理依赖、
返回槽具体通信格式、精度、UB/lane 和调度仍分别由原编号承接，不新增重复议题。
优先看“重点核对”项；其余为可倾向推荐方案的候选。
具体建议、证据和验证边界见编号链接。共享问题只设一个主编号，其他模块通过编号关联。

| 编号 | 模块 | 待确定项 | 风险 | 参考 | 建议处理 |
| --- | --- | --- | --- | --- | --- |
| [CB1A-01](#cb1a-01) | M1a | 各角色 UB/Event 总预算及生命周期 | 高 | 部分 | 阶段隔离与资源分类已确认；继续核算容量、offset、lane 配额和 Event 编号 |

当前待确认 1 项（讨论中的 CB1A-01）：高风险 1 项、中风险 0 项、低风险 0 项；
无参考 0 项，剩余 1 项高风险 CB1A-01 的阶段复用原则已确认，具体 UB/Event 预算仍待确认。

建议收口顺序：在 CB00-02 已确认布局下，按 CB03-02/CB04-03 已定非量化通信与数值格式，依据 CB1A-02/CB02-03 已定初始化与消费清理契约，依据 CB1B-03 已确认的 32B 发布布局，
两级检测及通知搬运按 CB02-01 已确认方案实现，之后依据 CB05-03 已定重叠清理原则及 CB06-01/CB06-02 已定调度和排空边界，
落实模块依赖并按 CB06-04 验证；按 CB1A-01 已确认的阶段边界及资源分类继续核算全流程预算。CB06-03 已确认默认 32/32 编译期配置；后续调优不改变既定任务归属。

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


<a id="deferred-large-token-num"></a>
## 附录 C：专题遗留——TokenNum 超过 10K 的筛选与密排

关联 CB1B-01。第一阶段由调用方/测试保证每卡实际 TokenNum 不超过 10000，
本专题不进入当前设计待定、实现或验收范围，后续明确扩展大行数支持时再展开。
容量 Crows/MaxTokenNum 仍可能大于 10000；只扫描实际有效行，不扫描分配尾部。
中转槽仍由 source/expert/q 定位，少量有效行也可能引用较大槽号，不能按 10K 截小中转区。

原极端容量例：R=128、E=384、BS=256、K=16，L=3，
Crows=R*BS*min(K,L)=98304，一份完整 uint32 行号列表为 393216B（384KiB）。
这是该配置的输出容量上限；实际 TokenNum 是否达到该值取决于路由分布。
该容量示例继续用于 GM 规划，不作为第一阶段 UB 全列表需求。

后续专题保留以下内容：

- 分批 Compare/GatherMask，先统计全类型任务总数以确定本核密排区间，再扫描并只保留本核行号，必要时继续分批处理。
- M1b/M3/M4 共用选择组件；保持原始行序、完整任务覆盖及按类型总数的核间均衡，不按局部批次独立均分。
- 大行数下输入/输出行号、TokenType、mask/work、表段和 payload lane 的 UB/Event 生命周期及表 2/3 跨度处理。
- 批边界、空批、任务区间跨批、倾斜路由的正确性与性能验证，以及扩展输入范围后的支持约束。

第一阶段 10K 约束不自动解决表 2/3 局部缓存跨度问题；合法范围内的缓存安排仍由
CB03-01/CB04-01 与 CB1A-01 设计。遗留迁出不代表大行数算法已经实现或验证。

<a id="deferred-m4-m5-fusion"></a>
## 附录 D：后续优化方案——M4/M5 跨阶段流水融合（首版不实现）

关联 CB1A-01、CB04-02、CB05-02、CB06-02。首版明确保留 M4→M5 的 PIPE_ALL，
优先以独立模块、独立 Event 生命周期完成全部功能，按常驻、公共、阶段专用资源规划 UB 复用。
本附录不属于首版实现要求、待确定项或开发验收门槛，也不要求首版增加跨阶段融合开关。

**启动条件：** 所有功能完成后，只有性能专项明确发现这部分的阶段排空或资源切换构成性能问题，
才进一步评估；不预先认定 PIPE_ALL 为瓶颈，不为融合提前扩大当前测试或打点范围。

后续可考虑在保持数值与通信协议的前提下，沿用同一套输入/输出环形 lane、FP32 聚合工作区
和 Event 状态，切换任务来源与输入/输出方式，用资源级完成依赖替代全流水屏障。
M5 仍需额外的 pending、表 4/5、probe 及 Flag 清理职责；M4 连续 stage payload 与
M5 DataAsFlag payload 的读取方式、partial 发包与 yOut 写出不能直接等同。

移除 PIPE_ALL 之前须重新明确跨阶段在途任务、UB 覆盖及 Event 所有权：公共区何时可写，
旧表/权重何时不再被使用，M4 输出仍在途时如何开始 M5，probe/清理与聚合如何避免资源冲突。
仅删除屏障不能代替这些依赖设计；是否进一步交替调度 M4/M5 也需另行评估，不由本条自动批准。
目标是减少已证实的阶段切换代价，并保持独立模块可定位；实际收益与正确性由后续专项验证。
