# EP Memory Server Dedup Combine 设计

## 设计推进方式

**M1–M5 的用户细化补充已完成，当前进入整体通读、待确定项分级与收口阶段。**

保留已确认的模块行为，补全跨模块衔接；各模块末尾维护待确定项、推荐设计及依据，文末统一汇总。
M0 接线/布局及 M6 调度/收尾也纳入核对，避免模块各自成立而整体链路不闭合。
细化补充完成不等于所有实现参数已确定，也不代表 Combine 已实现或通过设备验证。

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

沿用已确认的选窗和模型调度契约，Combine 内部流程在这一边界下设计。
新共享中转区的分配、映射和布局归入 M0，实际结果涉及共同通信域时同步联合设计。

## 已确认项通读补全

| 范围 | 已确认契约及跨模块约束 |
| --- | --- |
| 接口与有效范围 | Combine 参数内容保留 Memory 24 参数并追加三表，Host/GM TilingData/launch 形式参照 Dispatch Server Dedup（CB00-01）；TokenNum 取 sendCounts[E−1]，五表有效到 Combine 完成 |
| 索引身份 | 表 1–3 按 expertOut 的 Dispatch 输出 row，表 4/5 按本卡原始 TokenId；密排序号、row、中转 slot、source TokenId 分开使用 |
| 均衡 | M1b/type3 按 64 核，M3/type1 按 Send 核，M4/type2 按 Read 核，M5/BS 按参与核均分；不可整除时相差至多 1，不按成员数重新划分 M4 |
| 局部表缓存 | 表 2 行距 12B、表 3 行距 K*8B；区间搬运可带入无效行，但不解析或消费无效条目；表 3 仅消费 type2 第 0..N 块 |
| 加权职责 | M1b 暂存未加权结果，M3 发送未加权 DIRECT；M4 使用表 3 的 self/memberWeight，M5 使用表 5；partial 的最终权重为 1 |
| 成员寻址 | M4 第一跳自身取 expertOut[row]；其他成员含同卡成员均取 stage，用表 2 source 和表 3 expert/q 定位；自身只计一次 |
| 空职责核 | 全 64 核均发布 M1b 完成，最后 8 核均履行 M2 通知；TokenNum=0 不代表该卡无 BS 接收任务，不能跳过控制协议 |
| M5 最新方案 | 按 mask 置位 k 搬运，UB 按 32B Flag 块密排，统一等待后一次首 float 求和；完整性采用用户确认的总线保证，阈值 N*popcount(mask) |
| 复用边界 | M2 控制 Flag 两份乒乓；payload 沿用现有 D/C 选窗及用户确认的模型顺序，不重开跨轮重叠、新公共轮次参数或额外读完握手 |

实现中将 M4 的 N 命名为 memberCount，M5 的 N 命名为 flagBlockCount；M2 分核的
recvCoreNum=8*N 中 N 仅是核数倍数，三者不同。expertOut 输入仍为 FP16/BF16，quantMode
控制通信格式。stage 与结果区同时存活，DIRECT/partial 在同一调用内须统一可解码；具体格式待收口。

## 模块边界：M0–M6

M1 拆为两个独立函数 M1a/M1b。各模块的功能预期如下；模块编号表示设计职责，
具体运行时调度由 M6 串联。

| 模块 | 作用范围 | 功能结果 |
| --- | --- | --- |
| M0 接口接线与公共布局 | Host / 公共配置 | 为所有模块提供一致的参数和内存视图 |
| M1a Init | 全部 64 核 | 初始化每核执行上下文和资源 |
| M1b 本地待聚合 Token 拷贝 | 全部 64 核 | 类型 3 行密排后均分，数据写入本卡共享中转区 |
| M2 本卡汇总与 Server ready | 核 56..63 通知，每个 Read 核独立观察 | 两级求和确认本 Server 中转数据就绪，卡间 Flag 使用乒乓缓冲 |
| M3 DIRECT 返回 | Send 核 | 类型 1 行密排后按 Send 核均分，数据直接返回原始 source |
| M4 读取、局部聚合与 partial 返回 | Read 核 | 类型 2 行密排后按 Read 核均分，本地贡献和成员贡献合为一份返回结果 |
| M5 最终本卡 Token 聚合 | 首版 Read 核，可扩展为 64 核 | 本卡 BS 个原始 Token 的 yOut |
| M6 调度、资源衔接与整体结束 | 全部 64 核 | 串联模块、推进任务并完成所有核的责任 |

### M0 接口接线与公共布局

**功能预期：** 将已确认的公开参数传递到内部配置、tiling 和 kernel，为各模块提供统一的
shape、五表、expertOut/yOut、通信地址与布局信息。公共签名在联合设计维护，此模块负责内部接线。

**已确认项：** 接口、五表映射和选窗不变；stage 为共享可读区域，source 结果槽按原始 TokenId/K 组织。

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

**标量传递边界：** BS、H、TopK、moeExpertNum、dtype、quantMode 纳入 Combine tiling；
Host 仅用于校验的配置无需机械传入 kernel。rank/rankSize/peer 映射以 CommArgs 为依据，
TokenNum 仍在设备端取 sendCounts[E−1]，不能在 Host 构造 tiling 时额外同步读回该计数。
stream/comm 为 Host 提交与资源管理信息；magic 参照 Dispatch 保留为独立 launch 标量。
若分核选择采用运行时配置，recvCoreNum 也通过 tiling 传递；其选择规则归 CB06-03。
M2 bank 的状态来源归 CB02-02，此处不将它先固定为静态 tiling 字段。

**内部 ABI 落实：** Combine tiling 使用 Host/device 共用定义，字段先校验再收窄，保留字节
初始化为零；以 sizeof/offsetof/标准布局断言和 launch 测试约束参数布局。Dispatch 当前
32B tiling、120B launch args 仅是参考实例，Combine 不照搬其专属输出字段、保留字段或总大小。
最终字段表、位宽和布局随 CB00-02/CB00-03/CB06-03 收口后落实；本项的接口组织方向已确认，
无需再次作为待定架构方案，但实现仍须验证公开签名、GM tiling 读取与注册入口参数一致。

具体源码见 [Dispatch dedup Host](../../src/ep/host/tilexr_ep_memory_server_dedup_dispatch.cpp)、
[参数封装](../../src/ep/host/ep_memory_server_dedup_host.h)、
[tiling 定义](../../src/ep/common/ep_memory_server_dedup_tiling.h)、
[launch 构造](../../src/ep/host/ep_memory_server_dedup_launch.cpp)、
[launch ABI](../../src/ep/host/ep_memory_server_dedup_launch.h)和
[设备入口](../../src/ep/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.cpp)。

**待确定项：** 分级见[规则](#待确定项分级规则)，推荐均待确认。

| 编号 | 风险 / 参考 | 待确定内容 | 推荐设计 | 依据与核对重点 |
| --- | --- | --- | --- | --- |
| <a id="cb00-02"></a>CB00-02 | 高 / 部分 | stage/control/result 物理归属与容量 | 推荐在选中 Combine 窗口内划分 control、E*BS 个 stage 槽和 BS*K 个结果槽；Host/kernel 共用 uint64 布局。核算两份 M2 Flag、初始化区域、对齐和尾部保留；不足时报告不支持，不隐式新增公共 workspace。 | [R1](#参考依据目录)/[R6](#参考依据目录)；新 pull 布局尚无直接参考。旧 relayBytes=8*BS*K*stride 是历史预留，不能代替 E*BS*Sstage。 |
| <a id="cb00-03"></a>CB00-03 | 中 / 部分 | 首版支持矩阵与可选加权 | 推荐首版固定使用五表权重；保留所有参数，未实现 active mask/TP/共享专家只接受关闭配置。无权重开关及表 3/5 同时忽略规则单独确认，不静默解释 nullptr；明确各 quantMode 支持范围。 | [R1](#参考依据目录)/[R2](#参考依据目录)；参数保留已确认，无权重模式的 dedup 完整语义未定。 |

### M1a Init

**功能预期：** 全部 64 核执行 Init，建立本核参数、地址、状态和资源；从 sendCounts 末项取得
TokenNum，为后续本地拷贝及 Send/Read 处理提供执行上下文。

**已确认项：** 全 64 核执行 Init，在设备侧读取计数末项，建立角色、地址、当前 bank 及资源上下文。

**待确定项：** 分级见[规则](#待确定项分级规则)，推荐均待确认。

| 编号 | 风险 / 参考 | 待确定内容 | 推荐设计 | 依据与核对重点 |
| --- | --- | --- | --- | --- |
| <a id="cb1a-01"></a>CB1A-01 | 高 / 部分 | 各角色 UB/Event 总预算及生命周期 | 推荐列出 M1b/M3/M4/M5 的固定区、索引、表缓存、Flag/work、累加区和 lane 的同时存活图；lane 由剩余 UB 及 Event 计算，保持合法输入至少一 lane 的前提。不得在在途 DMA 未完成时复用空间。 | [R3](#参考依据目录)/[R4](#参考依据目录)；有动态 lane 参考，新模块组合的最坏 H/K/行跨度需重新核算。 |
| <a id="cb1a-02"></a>CB1A-02 | 高 / 部分 | 控制区首次清零入口与完成时机 | 推荐在通信区建立或调用前受流顺序约束的准备步骤清零 M1b 记录、M2 两 bank 和 M5 结果 Flag，后续 owner 消费后清理。必须避免 Init 清零覆盖其他核已发布的本轮 1。 | [R1](#参考依据目录)/[R3](#参考依据目录)；清理模式有参考，新区域首次初始化的入口与依赖未落实。 |
| <a id="cb1a-03"></a>CB1A-03 | 中 / 直接 | Reset、寄存器与 Event 初始状态 | 推荐 Reset 前排空本核操作，之后重建事件和量化控制状态；空闲 lane 初始 Event 设置一次，M6 统一回收。 | [R3](#参考依据目录)/[R4](#参考依据目录)/[R9](#参考依据目录)；已有 Reset 丢失 FP8 溢出模式的证据，核对每阶段恢复点。 |

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

当前 Memory Dispatch 的 [TokenActiveMaskCal](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h)
已有比较 mask、CreateVecIndex 和 GatherMask 压缩行号的组合，可作为实现参考。
[官方 GatherMask 文档](https://asc.gitcode.com/api/SIMD-API/basic_api/memory_vector_compute/compare_and_select/GatherMask.html)
说明该操作按 pattern 筛选源元素，rsvdCnt 给出保留数量；本设计以行号序列作为源元素。
官方检索脚本未返回 9.1.0 的对应页面，上述链接用于语义参考，不代表已验证目标重载；
具体数据类型、分批参数及资源排布在本模块继续细化。

**本核完成发布：** 本核全部类型 3 payload 拷贝提交后，执行 `PipeBarrier<PIPE_MTE3>()`，
在 MTE3 流水上建立 payload 写入先于完成 Flag 写入的顺序，随后用一次多块 DataCopyPad
发布本核完成状态。Flag 暂定为 FP32 `1.0f`；无拷贝任务的核也发布完成。

本卡 GM 控制区划出 8 个接收区，每区 64 个槽。逻辑布局为 `prepareDone[8][64]`，
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
| <a id="cb1b-02"></a>CB1B-02 | 中 / 直接 | 本地拷贝 lane 与表 2 获取 | 推荐表 2 按本核 type3 行号范围分批缓存；MTE2/MTE3 环形 lane 按 UB 配置，复用槽前等待 MTE3 释放。stage 首版建议保持 expertOut dtype，stride 归 CB00-02。 | [R3](#参考依据目录)/[R4](#参考依据目录)；沿用缓冲所有权模式，核对尾块及发布前排空。 |
| <a id="cb1b-03"></a>CB1B-03 | 高 / 部分 | 64 核紧凑 4B 完成记录的精确发布 | 保留 prepareDone[8][64] 和一次 DataCopyPad 写八槽；确定源 UB 初始化依赖、4B blockLen/252B dstStride 的目标重载与 MTE3 屏障。验证各核精确写自身 4B、不覆盖邻槽，payload 先于通知可见。 | [R7](#参考依据目录)；文档支持参数语义，完整 64 核新协议尚无设备证据。M5 首 float 保证不替代此处地址/顺序核对。 |

### M2 本卡汇总与 Server ready

**功能预期：** 最后 8 个核（全局 coreId 56..63）各自汇总本卡 64 核的类型 3 拷贝完成记录，
sum=64.0f 后分别通知 Server 内的 8 张卡（包括本卡）。每个 Read 核独立监听本卡属于自己的
512B 卡间同步块，提取 8 个来源卡的完成 Flag，sum=8.0f 后进入 M4，并独立清理自己的 Flag 区。
卡间同步区采用两份乒乓缓冲。

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
当前 bank 中每个 Read 核对应块的第 s 个槽。每个目标 Read 核都得到一份来自 8 张卡的独立记录。
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
bankBytes = recvCoreNum * 512
blockAddress(target,bank,r) = syncBase(target) + bank*bankBytes + r*512
flagAddress(target,bank,r,s) = blockAddress(target,bank,r) + s*32
```

syncBase 为目标卡同步区的 512B 对齐基址。通知核 56+j 对目标 j 的所有 r 写同一个来源槽 s，
每个来源槽由对应来源卡唯一写入；接收方按自己的 r 监听。

**每核监听与清理：** 每个 Read 核只监听本卡当前 bank 的自身 512B 块，
每次取 8 个 32B 槽的首个 float 求和。sum=8.0f 表示本 Server 全部 8 卡的类型 3 数据已准备完成，
本核可以读取中转数据。各核自行消费、清零自己监听的 Flag 区，M6 承接清理写入完成与资源回收；
核 56..63 也按其 Read 组编号监听自己的块。512B 独占块及清理职责不会因 M5 后续全核参与而扩大到 Send 核。

**乒乓缓冲：** 上述布局为一份 bank，预留 bank 0 / bank 1 两份，
相邻 Combine 调用交替选择，通知 writer 与目标 listener 对同一次调用使用一致的 bank。
两份区在首次使用前初始化为 0；每个 Read 核在消费后清理本次使用的区，并在该区再次复用前完成清理。

```text
单份容量 = recvCoreNum * 512B        // 4..24 KiB
两份总容量 = 2 * recvCoreNum * 512B  // 8..48 KiB
```

例如 recvCoreNum=48 时，一份 24KiB，两份共 48KiB。M1b 的 prepareDone[8][64] 是另外的
本卡 2KiB 控制区，不计入这两份卡间同步区。其布局在本次保持不变。

此乒乓是 M2 卡间完成 Flag 的内部双缓冲，不改变已有 Dispatch/Combine 通信窗选择或 payload
布局。bank 必须在相邻 Combine 调用间交替，不能直接假定 Dispatch/Combine 共用 magic 的最低位
会在连续两次 Combine 之间变化；具体选择状态由 M0/M1a 衔接，不新增公共接口参数。
跨调用中转数据复用仍按已确认的模型调度契约执行。

**已确认项：** 56..63 各汇总本卡 64 槽并通知一张卡；Read 核独占 512B，sum=8 后可读 stage 并自行清理；两 bank 交替。

**待确定项：** 分级见[规则](#待确定项分级规则)，推荐均待确认。

| 编号 | 风险 / 参考 | 待确定内容 | 推荐设计 | 依据与核对重点 |
| --- | --- | --- | --- | --- |
| <a id="cb02-01"></a>CB02-01 | 中 / 部分 | 两级求和与通知 fanout 搬运 | 推荐本卡 64 个紧排 float 一次求和，卡间八槽按首 float 求和；通知核多块 DMA 写目标所有 Read 块的同一来源槽，起点相距 512B。源缓冲到写完才复用，4B/32B 写法以目标重载定稿。 | [R3](#参考依据目录)/[R7](#参考依据目录)；有求和及步长搬运参考，新 fanout 需地址覆盖测试。 |
| <a id="cb02-02"></a>CB02-02 | 高 / 无 | 独立 Combine bank 状态与存储位置 | 推荐通信域内部维护 Combine 专用 bank 翻转状态，在实际调用边界交替 bank，不直接用 D/C magic 最低位、不新增公共参数。确认 syncBase 的窗口/持久区域、跨 rank 初值一致及更新位置；若沿用图执行，核对重复执行不会固定 bank。 | 只有现有 magic 选窗，没有该独立控制 bank 的直接参考；不扩展并发或失败重试协议。 |
| <a id="cb02-03"></a>CB02-03 | 高 / 部分 | 两级 Flag 消费清理的完成依赖 | owner 不变：推荐通知核清自己的 256B prepareDone，Read 核清自身当前 bank 的 512B 块；清理、通知及后续 DMA 分别建依赖，bank 再用前排空。未取得完整 sum 前不清局部已到达槽。 | [R3](#参考依据目录)/[R4](#参考依据目录)；单 owner 清理有参考，新组合需验证快慢核、空任务和两 bank 时序。 |

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
| <a id="cb03-02"></a>CB03-02 | 高 / 部分 | DIRECT/partial 统一 wire 及发送流水 | 推荐沿用 Memory 的 480B payload+32B Flag 和量化 scale 组织；M3/M4 同一调用统一 Sresult、解码 dtype、flagBlockCount，M5 无需猜类型。复用发送 lane，明确 payload/scale 与 Flag 发布边界，partial cast 联动 CB04-03。 | [R1](#参考依据目录)/[R3](#参考依据目录)/[R4](#参考依据目录)；直发有参考，本地部分和再量化返回的新协议未验证，须核对尾块及 scale。 |

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
srcRank/srcTokenId/srcTopk 定位返回槽并发送 partial。两表局部缓存不改变五表 ABI 或权重语义。

**已确认项：** type2 密排后按 Read 核均分，表 2/3 局部缓存；M2 ready 后读取成员，self+成员加权返回 partial。

**待确定项：** 分级见[规则](#待确定项分级规则)，推荐均待确认。

| 编号 | 风险 / 参考 | 待确定内容 | 推荐设计 | 依据与核对重点 |
| --- | --- | --- | --- | --- |
| <a id="cb04-01"></a>CB04-01 | 低 / 直接 | 两表缓存和成员地址实现 | 推荐两表共用批次边界，各自保留 baseRow 与 12B/K*8B stride；header.N 按 uint32，成员按 uint16 expert/q 与 FP32 weight。沿用联合 source/expert/q→slot 公式，同卡成员也读 stage。 | [R1](#参考依据目录)/[R2](#参考依据目录)；协议和生产规则已存在；验证 type3 offset 与读取 slot 闭合、0 权重和高 slot。 |
| <a id="cb04-02"></a>CB04-02 | 中 / 部分 | 成员预取、累加及 partial 输出流水 | 推荐单个 type2 保持同一 owner，按表内顺序累加；多个 MTE2 输入 lane 预取，与 Vector 累加并行，partial 输出 lane 独立交 MTE3。H 分块不引入跨核归约。 | [R3](#参考依据目录)/[R4](#参考依据目录)；有 lane/加权参考，新主动读取与 partial 输出组合需排依赖。 |
| <a id="cb04-03"></a>CB04-03 | 高 / 部分 | 累加精度、partial cast 和量化边界 | 推荐 M4/M5 FP32 累加、stage 保留输入 dtype，partial 使用统一返回格式。必须确定量化/非量化的 cast、舍入、scale 计算顺序，以两级归约独立参考明确误差标准；不宣称与一次全 K 求和逐位相同。 | [R3](#参考依据目录)；FP32/MXFP8 组件有参考，新的归约树改变舍入位置，需重点核对。 |

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

目标为 CANN 9.1/A5。版本过滤的官方检索脚本未返回 9.1.0 ReduceSum 页面（count=0，
无页面获取错误），本机常用安装路径及仓库 env/cann 未找到目标头文件；上述官方链接为
asc-devkit master 文档，列明 Ascend 950 支持。尚未用目标版本头文件、编译或设备验证，
不能将接口语义推导视为目标环境验证通过。后续核对头文件为
`basic_api/kernel_operator_vec_reduce_intf.h`；具体实现需满足 UB 对齐、临时空间和搬运依赖。

**已确认项：** BS 整 Token 分核，预载本核表 4/5；未到齐就检查下一个；有效 k 的 Flag 密排后一次首 float 求和。

**待确定项：** 分级见[规则](#待确定项分级规则)，推荐均待确认。

| 编号 | 风险 / 参考 | 待确定内容 | 推荐设计 | 依据与核对重点 |
| --- | --- | --- | --- | --- |
| <a id="cb05-01"></a>CB05-01 | 低 / 直接 | 任务状态、表段缓存与空 mask | 推荐沿用 Memory tokenStatus+循环扫描，完成后不再 probe；两表按 p-begin 索引。mask=0 建议按空和写零，标为本轮推荐边界行为；TokenNum=0 仍按 BS/sourceMask 执行。 | [R1](#参考依据目录)/[R3](#参考依据目录) LocalWindowCopy/ExpertScaleCopy；验证快慢交错、bit31、空核与零 mask。 |
| <a id="cb05-02"></a>CB05-02 | 中 / 直接 | Flag/work 预算及 probe 指令组织 | 推荐每个有效 k 一次 N 块 DMA，统一 MTE2_V；ReduceSum(mask=1,repeat=m*N,stride=1) 后一次 V_S。按 m=K 预算 Flag/work 和目标尾部访问余量，N/计数/步长由公共布局计算并校验 API 范围。 | [R3](#参考依据目录) CheckPackedFlagRangeArrive、[R4](#参考依据目录) ReadServerDedupFlags、[R7](#参考依据目录)；算法有参考，真实延迟与 H/K 资源上界待验证。 |
| <a id="cb05-03"></a>CB05-03 | 高 / 部分 | 结果消费、加权输出及 Flag 清理 | 推荐复用 Memory 解包/解量化、FP32 加权及 cast 输出，仅消费置位槽，partial 权重为 1。Token owner 在 payload 读取不再依赖结果槽后清已消费 Flag，M6 排空输出及清理；首次全域清零归 CB1A-02。权重仍按原始 k 索引。 | [R3](#参考依据目录)；算术与清理有参考，跳槽/partial/在途 lane 的组合需核对，不能在仍有读取时释放槽。 |

### M6 调度、资源衔接与整体结束

**功能预期：** 串联全核 Init/本地拷贝、Send/Read 分工、ready、DIRECT/group 处理和最终
source 聚合，保证任务持续推进，并在本核所有发送、输出和在途搬运责任完成后结束。
M5 后续采用 64 核时，也由本模块安排 Send 核完成发送后进入最终聚合的路径。

**已确认项：** 串联 M1–M5，在本核发送、输出、清理及在途操作完成后结束；首版 M5 仅 Read 核，全核版保留映射能力。

**待确定项：** 分级见[规则](#待确定项分级规则)，推荐均待确认。

| 编号 | 风险 / 参考 | 待确定内容 | 推荐设计 | 依据与核对重点 |
| --- | --- | --- | --- | --- |
| <a id="cb06-01"></a>CB06-01 | 高 / 部分 | 运行时阶段顺序与进度依赖 | 推荐 Send 在自身 M1b 发布后进入 M3；Read 完成自身 M2 责任后处理全部本核 M4，排空再进入 M5，不等其他 Read 切阶段。验证发送不依赖对方先跑 M5、通知核不被其他监听阻塞；首版暂不交替 M4/M5。 | [R3](#参考依据目录) 有先发后收参考；两级 ready+分核的新组合需检查等待依赖图无环。 |
| <a id="cb06-02"></a>CB06-02 | 高 / 部分 | 结束判据与在途资源排空 | 推荐分别跟踪 payload、Flag、yOut、清理责任，全部相关引擎完成才返回；Send 无 M5 任务不代表 MTE3 可提前释放。沿用模型调度、不加 Server 读完握手，额外核内阶段屏障按依赖图定稿。 | [R3](#参考依据目录)/[R4](#参考依据目录)；核末排空有参考，新控制区与输出/清理需合并核对，不能仅看 token 计数。 |
| <a id="cb06-03"></a>CB06-03 | 中 / 无 | recvCoreNum 初值及选择策略 | 推荐首轮固定 recvCoreNum=32/sendCoreNum=32 作为未调优起点，再在已确认 8..48 范围内按负载及实测选择。Host/tiling/kernel 配置一致，不新增公共分核参数。 | 新 Combine 尚无分核性能参考；32/32 只是工程建议，需用户核对，不是最优结论。 |
| <a id="cb06-04"></a>CB06-04 | 中 / 直接 | 验证梯度与诊断/性能口径 | 推荐先 Host 地址/容量及独立数值 oracle，再 A5 编译、单轮非量化、量化、多轮和倾斜任务。记录阶段耗时/probe 次数、后端/版本/数据量及跨 rank 统计；细打点只做诊断，performance 与 breakdown 分开。 | [R8](#参考依据目录)/[R9](#参考依据目录)；覆盖同卡成员、多成员、空任务、bank 交替、尾块及不同 expertOut，不沿用普通 Memory/CPU 设备结论。 |

## 初版完成后的整体通读

2026-09-20：依据用户确认，M1–M5 的模块细化补充阶段结束，本轮已执行整体通读。
已确认项保留正文；待确定项在各模块原位维护。M0/M6 的跨模块衔接一并纳入，
不等待全部实现细节完成后才暴露容量、同步、数值和结束条件方面的缺口。

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

原 24 项中，[CB00-01](#cb00-01) 的接口组织方向已确认：参数语义沿用 Combine，
Host/GM TilingData/launch 形式参照 Dispatch Server Dedup。以下剩余 **23 项待确认**。
优先看“重点核对”项；其余为可倾向推荐方案的候选。
具体建议、证据和验证边界见编号链接。共享问题只设一个主编号，其他模块通过编号关联。

| 编号 | 模块 | 待确定项 | 风险 | 参考 | 建议处理 |
| --- | --- | --- | --- | --- | --- |
| [CB00-02](#cb00-02) | M0 | stage/control/result 物理归属与容量 | 高 | 部分 | 重点核对 |
| [CB02-02](#cb02-02) | M2 | 独立 Combine bank 状态与存储位置 | 高 | 无 | 重点核对 |
| [CB02-03](#cb02-03) | M2 | 两级 Flag 消费清理的完成依赖 | 高 | 部分 | 重点核对 |
| [CB03-02](#cb03-02) | M3 | DIRECT/partial 统一 wire 及发送流水 | 高 | 部分 | 重点核对 |
| [CB04-03](#cb04-03) | M4 | 累加精度、partial cast 和量化边界 | 高 | 部分 | 重点核对 |
| [CB05-03](#cb05-03) | M5 | 结果消费、加权输出及 Flag 清理 | 高 | 部分 | 重点核对 |
| [CB06-01](#cb06-01) | M6 | 运行时阶段顺序与进度依赖 | 高 | 部分 | 重点核对 |
| [CB06-02](#cb06-02) | M6 | 结束判据与在途资源排空 | 高 | 部分 | 重点核对 |
| [CB1A-01](#cb1a-01) | M1a | 各角色 UB/Event 总预算及生命周期 | 高 | 部分 | 重点核对 |
| [CB1A-02](#cb1a-02) | M1a | 控制区首次清零入口与完成时机 | 高 | 部分 | 重点核对 |
| [CB1B-03](#cb1b-03) | M1b | 64 核紧凑 4B 完成记录的精确发布 | 高 | 部分 | 重点核对 |
| [CB06-03](#cb06-03) | M6 | recvCoreNum 初值及选择策略 | 中 | 无 | 重点核对 |
| [CB00-03](#cb00-03) | M0 | 首版支持矩阵与可选加权 | 中 | 部分 | 倾向采用推荐设计 |
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

当前待确认 23 项：高风险 11 项、中风险 9 项、低风险 3 项；
无参考 2 项（与风险等级交叉），合计 12 项建议重点核对。

建议收口顺序：先 CB00-02/CB03-02/CB04-03 确定布局与数值格式，再 CB1A-02/CB1B-03/
CB02-02/CB02-03 明确初始化、发布和控制 bank，之后 CB1A-01/CB05-03/CB06-01/CB06-02
核对资源、消费和结束依赖。CB06-03 的分核起点单独确认；后续调优不改变既定任务归属。

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
| R9 | [进度与 Reset 经验](../EP_MEMORY_SERVER_DEDUP_PROGRESS.md)、[当前 Dispatch 规格](ep-memory-server-dedup.md)：FP8 状态重建、打点和性能口径 | 同类历史问题就是新实现的已知故障 |

## 参考与验证边界

基于[当前 Memory Combine kernel](../../src/ep/kernels/tilexr_ep_combine_memory_kernel.h)及
[Host 入口](../../src/ep/host/tilexr_ep_memory_combine.cpp)进行实现设计；
接口与五表以[联合设计](ep-memory-server-dedup-dispatch-combine-design.md)为准。

本轮完成设计通读、已确认项补全和待确定项分级，尚未实现 Server Dedup Combine，未运行 NPU 验证。
后续模块初版、整体通读和实现验证分别记录状态，不将设计完成视为实现通过。
