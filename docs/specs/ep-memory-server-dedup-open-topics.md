# EP Memory Server Dedup 第二轮待确认项

## 使用方式

第一轮已经确认的约束已合入[主设计](ep-memory-server-dedup.md)。
本文集中记录 Dispatch 仍有歧义、设计不完整或需要目标 API 证据的事项，以及 Dispatch/Combine 之间的接口和公共内存契约。
Combine 内部的核分工、H 分块、归约状态、数值实现和内部验收项迁移到[Combine 待确认文档](ep-memory-server-dedup-combine-open-topics.md)，本文只保留其对 Dispatch 可见的接口要求。
第二轮使用 R01-R12；旧 D/C 编号保留映射，讨论时优先引用 R 编号。
[Combine 旧议题](ep-memory-server-dedup-combine-open-topics.md)保留旧 C01-C14 索引，并承接本轮迁入的 Combine 内部议题；公共接口仍在本文维护。

各待确认点使用 `Rxx-01`、`Rxx-02` 等子项编号，每个 Rxx 独立从 01 开始，不跨议题连续。
子项确认后保留编号并更新状态；后续新增点接续本议题编号，不重排既有编号。关联议题引用同一问题时标明对应子项。
R01 已关闭；R02-01 已确认采用 serverRouteMask + directMask，两遍扫描的分类、路由顺序和 Memory 复用结论均已收口。低优先级遗留同样编号，但明确标记延后，不计入初版关闭条件。
R04 输入清单中的第 1-3 项是信息类别，与 `R04-xx` 待确认子项编号分开使用；原第 4 项 X 已移入超限优化遗留。
R03、R04 已确认内容及 R05 最新结论已同步[主设计](ep-memory-server-dedup.md)：
R03 的 16B entry、固定 K-1 容量和无效区域不清零；R04-07/R04-08 的权重输入/输出分离及 Combine 参数复用；
R05-01/R05-02 的固定 64 核、48/16 分工、DataCopyPad 写回和 UB 相对槽位双表；
R05-03 的一份 1KB counter、首 4B 取号及算子末尾 SyncAll 后核 63 清零。
R05-04 的本卡源端数据发送已按 Memory 流水合入主设计，Token 尺寸使用去重版 T，并发数按剩余 UB 计算为 1 至 8 路，安全输入保证至少 1 路，无需校验。
R05-04 已按 Memory 收口：lane 显式 ID、独立输出 UB、核末 S_MTE3/DataCopyPad/MTE3_S；数据转发的监听/UB/Event/在途进度保留 R05-05/R05-06 后续设计，不套用源端发送流水。
Cnt 发送/接收/cumsum 整体复用 Memory，适配固定 48/16 核组；零 cnt/零 tokenNum 的既有行为已确认。
本轮确认 recvData 前两部分包括完整前缀就绪等待、读取和 Flag 清理；expertTokenNumsOut 由接收组内核 0（物理核 48）沿用 Memory 计算写出。
完整前缀仅由 16 核消费，发布副本、就绪 Flag 和清理均使用接收组内索引 0..15；源端 48 核不消费。
R07-01 已按 Memory 记录格式给出三块区域的完整单份布局；第二块沿用 source 返回在前、Dispatch 槽位在后的独立子区，第三块为 relay 返回。
R08-02 已确认首版要求非空 FP32 `expertScales[BS,K]`，nullptr 直接报错；其他库的 nullptr 语义不纳入首版。
R08-03 的 Dispatch 部分已确认 BF16/FP16 非量化及 MXFP8 E4M3/E5M2，量化计算、scale 布局和输出沿用 Memory；R10-01 同步补齐已有量化参数的 launch 传递，待代码接入。
专家数量约束更新为总数 `0<moeExpertNum<=1024`、`moeExpertNum=R*L`；不设独立的单卡专家数限制，允许 L=1，128P 下 L 至多为 8。
基础 API 依据见[官方 API 核对](#官方-api-核对)，设计确认与目标设备验证状态分别记录。

“待确认”表示需选择具体机制；“实现核对”表示逻辑方向已经确定，但 API/资源/可见性尚无足够证据。
各项候选建议只作为讨论起点。关闭议题时应将明确的输入、输出、所有权与完成条件合入主设计，
不能把“文档有候选”当作机制已经闭合，也不能把设计确认当作硬件验证通过。

## 遗留信息导致的过设计复核

本轮接口确认后，对早期讨论中的名词和机制做一次反向清理。以下内容不再作为初版新增设计：

| 遗留信息 | 初版处理 | 仍需保留的最小内容 |
| --- | --- | --- |
| X 计数 | 不作为 Dispatch 输出、Combine 输入或持久 metadata；不发布、不遍历 `[0,X)`、不做超限校验 | 第一跳 owner 使用一次 AtomicAdd 取得 `forwardTokenIdx`，用于关联同一 group |
| `forwardPlan`/第一跳 descriptor | 不新增独立 tensor、descriptor buffer、注册包或控制信号 | 发送时临时使用 Token 尾部的 `forwardEntries`，持久内容落入 `assistEx` 和 `group` |
| Token 尾部 | 不作为 public output/input；不跨 Dispatch/Combine 生命周期保存整份 Payload | 作为同一通信记录的内部 Payload，接收端按有效长度剥离并生成 metadata |
| 旧 Memory `assistInfoForCombine` 的三字段语义 | 保留基础接口形态时可保留参数，但去重逻辑不依赖旧三字段；不再复制一份同语义表 | 新增 `assistEx[tokenNum,8]`，与 Combine 的 `expertOut` 行一一对应 |
| relay cnt/转发 cnt | 不增加代理 cnt、二次 cumsum 或 cnt 控制包 | 原始 source 统计并发送 cnt，relay 只做数据转发 |
| 重开 cnt 接收/cumsum/计数输出设计 | 整体复用 Memory 的统计分支、记录格式、同步、清理及输出语义；零 cnt/零 tokenNum 同样沿用 | 适配固定核组、取消双 Group 和去重区域地址，不另造 raw-count/prefix 接口 |
| 保留旧 Memory 通信区域及绝对 offset | 两种算法不混用，通信空间按 Server Dedup 自身需要重新排布 | 依次为状态区（cnt/cumsum/counter）、数据接收区、中转 Token 区；只核算本版本的容量和生命周期 |
| 双 Group/两批发送屏障 | 不增加阶段间 `PIPE_ALL`、`SyncAll` 或单独等待 MTE3 | 每个发送核内部先处理第一跳，再处理 DIRECT，任务可连续下发 |
| 卡级多 expert 去重 | 不建立卡内 route 合并表，不省略同卡不同 expert 的贡献 | 统一按跨 Server 去重后的正常转发流程记录每条 route |
| 重复 Top-k、invalid expert、mask 分支 | 不增加扫描、过滤、哨兵或错误恢复路径 | 依赖已确认的安全输入和不支持场景直接报错 |
| `memberMask`/`groupCount`/哨兵 | 不增加有效位、独立 group 数或清零流程 | group 固定容量，前 `n` 项有效，尾部由 `n` 忽略 |
| 独立 metadata 发布机制 | 不增加逐表/逐 group ready 或额外同步，R04-04 关闭 | 沿用 Memory 算子完成语义，Dispatch 所有核完成任务及 GM 写入后，后续 Combine 才消费输出 |
| 按返回发送卡划分七/八块中转区 | 撤回来源卡分区，统一预留 8*BS 个 group，每组 K 个结果槽位 | 全卡唯一 t 与组内唯一 originalK 已足够定位贡献；group 表同步预留 8*BS 行，本卡及跨卡返回共享容量 |
| source 类型与重复权重载体 | 三项 metadata 之外独立输出 expertScalesOut；不再增加权重快照或结果类型位图 | expertScales 输入只读；Combine 通过已有 expertScales 参数读取输出，按 sourceMask 统一加权 |
| source 返回数量输出 | 不新增 per-Token recvCount 或通知 | sourceMask 已给出有效位置，popcount 可得有效贡献数；到达状态仍由这些位置的 Flag 决定 |

因此，初版 Dispatch/Combine 新增 metadata buffer 仍只有 `assistEx`、`sourceMask`、`group`；
Dispatch 使用独立 expertScales 输入和 expertScalesOut 输出；“三项 metadata”不再表示 Dispatch 仅有三个新增输出参数。Combine 的权重参数继续复用。
其余名称要么是内部临时变量，要么是未来超限优化的遗留备忘，不再形成额外接口或同步协议。

## Memory 复用判断

**状态：除明确标为已确认的子项外，以下仍为建议，等待用户最终确认。** 编号正文继续作为问题定义，下面的表格只提供建议索引，不产生另一套待确认编号。
q/cnt、统一 Payload/DataAsFlag、32B 头部及 R01/R02 已确认的内容不重新表决。Combine 内部实现已迁出本轮。

### 源码依据与复用边界

| 依据 | 当前 Memory 实现 | 对去重版本的含义 |
| --- | --- | --- |
| E01 [分核与参数计算](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L482) | BS<48 全核发 cnt，否则仅 cumsum 核组发送；cumsum 核数由既有公式限制，最多 8 核；`SplitToCore` 使用连续区间和余数均分 | R05-01 已确认默认 64 核，48 核源端数据发送、16 核 cnt 接收/cumsum/copy/relay；沿用 BS<48 分界，不沿用动态核数公式或 8 核上限。源端 BS 在 48 核连续均分、余数优先分配，接收 tokenNum 在 16 核均分；不原样复用旧全核 LocalWindowCopy 分区 |
| E02 [输出行与元数据](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L1506) | `dstPosition=prefixBefore(localExpert*R+srcRank)+q`，首个分区的 prefixBefore 为 0，其余取前一项前缀；hidden 和三字段 assist 同行，`CalRecvAndSetFlag` 写出完整前缀 | R04-02/R04-03 已确认保留行序、前缀和 caller-owned 输出 GM；assist 改为已确认的 32B assistEx，不能继续直接复制旧三字段 |
| E03 [发送资源](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L861) | 按剩余 UB/通信长度计算发送 buffer 数，上限 8；发送和接收复用 buffer 时有阻塞 `WaitFlag` | 本卡源端数据发送可复用容量计算方法及最后使用者保护；不能据此推导数据转发的监听/发送解耦 |
| E04 [核末 Flag 清理](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L1569) | `WaitAndFormatOutput` 收完负责的记录并等 buffer 后，调用 `ClearLocalWindowDataFlags`，只写每块 32B Flag | 推荐保留核末清理粒度；地址遍历改为去重的 tokenPos 所有权，不能沿用旧整段 source/expert 清理范围 |
| E05 [Host 校验](../../src/ep/host/ep_dispatch_memory_host.cpp#L54) | 普通 Memory 对 expertScales 有不同限制；无量化 FP16/BF16，MXFP8 为显式组合 | 去重输入 expertScales 必须非空、FP32 `[BS,K]` 且只读；归约权重写独立输出，nullptr 直接报错 |
| E06 [注册启动](../../src/ep/host/ep_kernel_launch.cpp#L390) | 查询设备核数、检查布局、取得 `TileXRCommNextMagic`，在给定 stream 上异步启动注册 kernel | 推荐复用提交方式；Host 返回成功不等于设备完成。新增持久表在 Dispatch kernel 完成后供后续 stream 任务使用 |
| E07 [窗口与预算](../../src/ep/host/ep_memory_layout.cpp#L98) / [轮次选择](../../src/ep/common/ep_memory_window.h) | Host 用有溢出检查的尺寸运算校验 UB/窗口；magic 选择 ping-pong 窗口，Payload Flag 仍需要清理 | 复用预算方法，新增区域需重新计算；magic 取模选窗不能独自使同一窗口中的旧 Payload Flag 失效 |
| E08 [Combine 权重接口](../../src/ep/kernels/tilexr_ep_combine_memory_kernel.h#L849) | ExpertScaleCopy 从本 source 的 FP32 `[BS,K]` expertScales 读取权重 | Combine 的 expertScales 指向 Dispatch 的 expertScalesOut：DIRECT 真实权重、partial 为 1.0，sourceMask 为 0 的槽位跳过 |
| E09 [参考行序](../../tests/ep/demo/tilexr_ep_dispatch_demo.cpp#L787) | `BuildExpectedMemoryRoutes` 独立按 localExpert、srcRank、原 route 顺序产生结果，另行累计前缀 | 可复用测试组织及原始 route oracle，新增去重通信路径和 metadata 断言，不能复用 kernel 生成的表当唯一正确答案 |
| E10 [Combine 返回寻址](../../src/ep/kernels/tilexr_ep_combine_memory_kernel.h#L552) | 以 `tokenId*slotCount+topkId` 定位目标卡结果槽位，不额外乘发送卡数 | R04-05/R07-03 已确认沿用二维展平思路，以 relay 的 t 和原始 k 定位中转结果，`recvSlot=t*K+k` |

### 同语义接口复核

**已确认原则：** 去重与非去重入口隔离，去重入口无需兼容旧算法的全部限制；在用途、数据形状和消费含义不变时，优先沿用 Memory 的既有参数名称与传递方式。
Dispatch expertScales 提供真实路由权重，expertScalesOut 提供 source 归约权重；Combine 继续用 expertScales 参数按原始 Token/k 索引消费输出。
source 只是原始输入所属 Rank 的角色名；权重参数以 expertScales / expertScalesOut 区分输入输出，二者存储独立。

| 需求或遗留名称 | 已有接口与源码依据 | 复核结论及状态 |
| --- | --- | --- |
| source 最终归约权重 | Memory Dispatch 已有 expertScales 输入，Memory Combine 已有 float* expertScales 输入 | Dispatch 新增独立 expertScalesOut，由 Combine 既有 expertScales 参数消费；两个输入接口并不自动提供 Dispatch 权重输出 |
| tokenNum / 本卡 expert 输出有效行数 | [ReadSendCount](../../src/ep/kernels/tilexr_ep_combine_memory_kernel.h#L429) 读取 sendCounts 最后一项；Dispatch 已有 sendCountsOut | 已确认：首版 tokenNum=sendCounts[L*R-1]；沿用 sendCountsOut -> sendCounts，不新增 tokenNum 标量、计数 tensor 或通知；它不表示 source 的逐 Token 返回数量 |
| 每个 expert 的行数或前缀 | [SetValidExpertInfo](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L1433) 写 expertTokenNumsOut，expertTokenNumsType=0 为累计前缀、1 为逐 expert count | 沿用既有计数契约，不再建立同义 expert 行数/offset 输出；精确参数声明仍归 R10-01 |
| Dispatch hidden 输出、expert 结果与最终 source 输出 | [公共 Memory 参数](../../src/include/tilexr_ep.h#L29) 已有 expandXOut、expertOut、yOut；[CopyInAndOut](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L1506) 按 dstPosition 写 hidden | Dispatch 已确认沿用 expandXOut 紧密行及 R08-03 的 BF16/FP16/FP8 输出格式；expertOut 是 expert 计算结果，不假定与 expandXOut 为同一块内存，其 dtype/返回格式仍归 R08-03/R10-03 |
| 每个 source Token 的最终贡献数 | 已确认 sourceMask[BS]，每项 uint32 表示有效 k 槽位 | 已确认：有效数量可由 popcount(sourceMask[token]) 得到，实际消费直接依据位集合；无需额外 recvCount，且不能用权重为 0 判断是否等待 |
| 量化 scale 输出 | [Memory CopyInAndOut](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L1540) 将量化 scale 写入 dynamicScalesOut | R08-03 已确认 MXFP8 直接沿用 dynamicScalesOut，按 [tokenNum,S] 紧密输出，S=align2(ceil(H/32))；它不承载 route weight，scales/expandScalesOut 仍为未使用占位 |
| comm、shape、dtype、stream 与调用完成 | [公共参数](../../src/include/tilexr_ep.h#L29) 已有相应参数，R04-04 已确认算子完成覆盖 GM 写入 | 同语义参数沿用；不为 metadata 单独新增完成事件或公开 plan handle。并发、跨轮生命周期仍归 R11，不因接口复用视为已验证 |
| 旧布尔 mask 与三字段 assist | Memory 的 xActiveMask 表示 Token/expert 活跃性；[旧 assist](../../src/ep/common/ep_memory_window.h#L17) 每行只有三个 int32 | 语义或布局不同，继续采用已确认的 sourceMask、assistEx、group；不把 uint32 k 位图塞入 bool mask，也不同时生成重复的旧 assist 记录 |
| workspace 参数或布局查询 | Memory [Dispatch 包装](../../src/ep/host/tilexr_ep_memory_dispatch.cpp#L70) 和 [Combine 包装](../../src/ep/host/tilexr_ep_memory_combine.cpp#L55) 将内部 workspace 设为 nullptr，两个 Memory 公共签名没有 workspace 参数 | 不能把其他 backend 的 workspace 参数称为可直接复用的 Memory 参数；内部区域布局及所需字节数查询归 R07-01/R10-01，通信域扩容和 peer 映射由框架负责，不先引入统一 plan/workspace handle |

复核结果：可沿用的重点是 expertScales、sendCounts、expertTokenNumsOut 及既有数据/shape/stream 参数；
sourceMask 已能替代额外返回计数，但仍是去重新增的位图输出。接口复用不自动意味着沿用 Memory 全 K 槽等待逻辑，去重消费范围必须受 sourceMask 限定。
当前[公共头文件](../../src/include/tilexr_ep.h#L53)已有独立去重 Dispatch 声明，可作为 R10-01 核对基线；声明或空 kernel 的存在不证明权重改写、路由或 Combine 已实现。

**通信框架边界（已确认）：** 通信域后续扩大到 4GB 上限，由通信框架独立修改；容量上限及扩容实现不属于本方案。
R07-01 只落实三块区域的内部 offset、对齐、所需字节数和复用，不以旧固定容量拒绝新布局。
例如 R=128、L=3、BS=256、H=7168、K=6 时 Dispatch 数据区为 1488MiB，该值用于区域寻址计算。

### 可合并确认的建议组

建议按以下八组讨论，同组内相关编号可以一次确认；它们保留原编号，未确认前均不关闭。
目标 API 证据和容量核算是实现核对项，不需要反复重开字段语义。

| 建议组 | 相关子项 | 推荐方向 |
| --- | --- | --- |
| 尾部格式 | R03-01 至 R03-03 | 全部已确认：16B entry、固定 K-1 容量、未用 entry/纯传输填充不清零，由有效长度限定解释范围 |
| 输出与行引用 | R04-01 至 R04-04、R10-01/R10-02、R11-04 | 三个 metadata 输出/输入、有效行序和 group 行引用已确认；GM 写入沿用 Dispatch 算子完成语义，R04-04 无独立设计项；具体参数声明与资源释放仍归 R10/R11 |
| 类型与权重 | R04-07/R04-08、R08-01/R08-02 | 已确认 source 用 expertScales 统一加权：DIRECT 为真实 weight，partial 为 1.0，sourceMask 限定消费范围；独立输出 expertScalesOut，不新增类型标记；首版 expertScales 必须非空，nullptr 直接报错 |
| 数据发送与数据转发资源 | R05-01 至 R05-06 | 分核、UB 双表、原子取号和核末清零已确认；源端 1 至 8 路环形发送基线已确认，保留具体 ID/权重写回核对；数据转发 UB/Event 和在途进度后续设计 |
| Flag 清理 | R06-01 至 R06-04 | 核末统一清理本核消费的 Flag，保持下发后剔除 |
| 槽位与有效 group | R04-03/R04-05、R07-01 至 R07-03 | 已确认通过第一跳 assistEx 定位 group；统一中转区预留 8*BS 个 group，recvSlot=t*K+originalK，不引入 X 或来源卡分区；完整单份区域预算见已收口的 R07-01 |
| 支持范围与生命周期 | R08-03、R10-03 至 R10-06、R11-01 至 R11-03/R11-05 | 精简首版格式，复用异步启动和跨 Rank 调用次序约定，明确持久输入寿命 |
| Dispatch 验收 | R12-01/R12-03/R12-04 | 沿用 Memory 独立行序参考，补去重 metadata、并发和完整字节统计 |

R03、R04 的初版设计、R05-01/R05-02、R07-02/R07-03、R08-01 及关联的 R10-02 已关闭；R04-07/R04-08 已确认权重语义和 expertScales 复用。
R04-06 的 X 记录及 R07-04 的超限处理低优先级延后。R08-04、R09 和 R12-02 的迁入项不参加本轮确认。

### 各子项推荐选项

“备选”只列存在实际取舍的方案；没有必要为了形式再设计一种机制。采用推荐项同样需要用户确认。
“沿用”指约定或算法可复用，“适配”指已有部件需调整，“新增”指 Memory 没有对应协议。

| 子项 | 判断与推荐选项 | 备选或需保留的核对 |
| --- | --- | --- |
| `R02-01` | **已确认并关闭：** 第一遍以当前 Token 的 serverRouteMask 分组并发第一跳，在 UB 为每 Token 留一个 directMask；第二遍按 directMask 发 DIRECT | 每核复用 64B 分组表，directMask 每 Token 4B；q/cnt/cumsum 及发送流水继续复用，不重新选择 |
| `R03-01` | **已确认采用推荐方案：** entry 按 `{int32 destinationExpertId,int32 destinationQ,int32 originalK,FP32 weight}`，offset 为 0/4/8/12，合计 16B | 本项关闭，保留编号 |
| `R03-02` | **已确认采用推荐方案：** 每包固定 K-1 个 entry 容量，由 n 指示有效前缀，统一 T | 本项关闭，保留编号；K=1 时 entry 容量为 0，固定头部仍存在 |
| `R03-03` | **已确认：** `[n,K-1)` 的未用 entry、两段纯传输对齐填充和末个 480B Payload 块余量不清零，不要求哨兵；由 n/有效数据长度限定解释范围 | 本项关闭；T 和全部 Flag 保持，有效头部/entries、hidden/scale 正确写入，量化计算补齐仍按算法处理 |
| `R04-01` | **已确认：** Dispatch 新增 assistEx、sourceMask、group 三个 metadata 输出，Combine 对应新增三个输入；不引入 X 输出或输入 | 本项 metadata 清单关闭；独立权重输出见 R04-08，具体参数声明归 R10-01，行关联及算子完成语义见已关闭的 R04-02/R04-03/R04-04 |
| `R04-02` | **已确认采用推荐方案：** 沿用 E02 行序，ExpertOut 保持 Dispatch 的有效 `tokenNum` 行；`assistEx[row]` 与 `expertOut[row]` 一一对应，首版 `tokenNum=sendCounts[L*R-1]` | 本项及关联 R10-02 关闭；expert 内部重排须在交给 Combine 前还原，不增加 public row map，不消费 padding 行 |
| `R04-03` | **已确认采用推荐方案：** group[t] 保持固定 stride，firstHopRow 保存 dstPosition；通过有效 assistEx 行中 forwardCount>0 的第一跳记录取得 t，再访问 group[t] | 本项关闭；不盲扫未写入的 8*BS 行，不新增 X/groupCount/validMask/哨兵或初始化整张 group 表；Combine 核分工在其文档讨论 |
| `R04-04` | **无需独立设计，关闭：** 沿用 Memory，Dispatch 所有核完成全部任务及 GM 写入后算子才结束，后续 Combine 可读取完整 metadata | 不增加 metadata 发布协议、逐表/逐 group ready 或额外同步；核内 UB/Event 复用仍归 R05，算子完成与 Host 异步提交返回沿用既有区别 |
| `R04-05` | **已确认并关闭：** Dispatch 按 `recvSlot=uint64(t)*K+entry.originalK` 产表；终端 Combine 从 assistEx 取得 relayRank/t/k，按同一公式返回，本卡相同 | 不需要 senderRank 维度、新编号或额外包字段；完整 base/offset 与返回 stride 仍归 R07-01/R08-03 |
| `R04-06` | **低优先级遗留：** 初版不生成对外 X 记录，也不在 Combine 接收 X；X 仅留作后续超限场景优化的讨论项 | 不影响已确认的内部 AtomicAdd 取号；超限处理方向与 R07-04 合并讨论，旧 X 输出建议撤回 |
| `R04-07` | **已确认并关闭：** source 的 DIRECT 权重保留真实值，group 的 returnK 权重设为 1.0，按 sourceMask 对有效结果统一乘权重累加 | 不新增 partialMask/类型标记，也不通过 weight 是否等于 1.0 推断类型；relay 的第一跳及成员仍保留真实 route weight |
| `R04-08` | **已确认并更新：** Dispatch 只读 expertScales 输入，生成独立 FP32[BS,K] expertScalesOut；Combine 通过已有 expertScales 参数消费输出 | 两个 GM buffer 不重叠，输入保持真实值；输出保持到 Combine 完成，核末写出及 Event 已由 R05-04 收口，ABI 契约见 R10-01 |
| `R05-01` | **已确认并关闭：** 默认 64 核，固定 48 核源端数据发送、16 核 cnt 接收/cumsum/localwindowcopy/转发。BS<48 时全部 64 核发 cnt，数据核先完成本核 cnt 再发数据；BS>=48 仅 16 核发 cnt，48 核直接处理数据发送 | 源端 BS 在 48 核连续均分、余数优先分配，接收 tokenNum 在 16 核均分。sourceMask 核末用 DataCopyPad 写回本核有效字节，最小 4B，GM 起点/长度无需 32B 对齐；不使用 DataCopy 将 UB 填充写入邻核区间。旧动态核数公式和 8 核上限不沿用 |
| `R05-02` | **已确认并关闭：** tokenPos[j] 为 uint32，保存相对本卡本轮 Dispatch 接收区第 0 个 Token 槽位的偏移；地址为 base+uint64(tokenPos[j])*T。tokenPos/pendingIdx 两张表均仅存 UB，pendingIdx 继续末项覆盖删除 | 按 BS*K 和接收不均衡度估算每核条目数；1024 项时单表 4KiB、双表 8KiB。撤回 GM 存表/分批加载方案；数据转发 UB 预算归 R05-05，不因估算值另增固定 1024 项的公共限制 |
| `R05-03` | **取号及清零方案已确认：** uint32_t AtomicAdd 返回旧值作为 t；保留一份 1KB counter 区，仅首 4B 使用；算子末尾全核 SyncAll() 后由核 63 清零首 4B | 首次零值沿用现有 peer memory 初始化；不清未用的 1020B，不新增 X。具体 offset 归 R07-01，算子间生命周期归 R11-05 |
| `R05-04` | **Memory 复核已收口：** 两类发送事件均按 lane 固定 ID，新增尾部沿用原拼包流水；源端输入/输出 UB 独立，核末 DataCopyPad 写出 mask 和权重 | S_MTE3 即时配对发布 Scalar 输出，核末 MTE3_S 完成写出；无新增发送事件池、阶段屏障或设计选择，待代码实现/验证 |
| `R05-05` | **数据转发后续设计：** 保留一次取号、公共副本和任务下发后剔除；讨论 t、nextEntry、转发 UB、metaUB/group 行 UB、监听双表及 Event 的资源与进度组织 | 最后使用者完成后才能复用 UB；不因 pending 删除重复取号或提前改写，不直接套用源端 1 至 8 路流水 |
| `R05-06` | **数据转发后续设计：** Token 尚未到达或转发资源暂忙时，如何继续监听并保持进度 | 固定接收槽位不增加逐 Token 目标端 ready/credit，资源暂忙不作为错误，不增加控制包 |
| `R06-01` | 沿用 E04 的核末粒度：本核负责的 Token 全部完成任务下发后，统一清理其接收 Flag | 逐 Token/小批次清理作为备选；两种方式都不改变已确认的“下发完成即可剔除 pending”，不等待 MTE3 完成再删除 |
| `R06-02` | PipeBarrier 的同流水前后序及 PIPE_ALL 核内完成语义已有 9.1 官网依据；推荐核末以 PIPE_MTE3 排序清理写入，独立零值 UB 保持到完成 | API 语义不再待确认；清理粒度及零值 UB/Event 组织仍待选择。若复用发送 UB，仍须建立最后使用者到新写入者的跨流水依赖 |
| `R06-03` | 适配 E04：以保留的 tokenPos 遍历已消费 slot 的每个 32B Flag，跨 480B Payload 间隔写零；每 Token 的 slot 仅一个清理 owner | DataCopyPad 的 GM/UB 对齐及 stride 单位已核对；具体地址遍历仍待确认，不清 Payload 或另一核尚未消费的本卡转发目标 |
| `R06-04` | 公共契约：Dispatch 接收 Flag 由该接收 owner 清理，Combine 返回 Flag 由 Combine 消费者清理；两者地址域独立，复用受 invocation 完成约束 | Combine 内部的清理时机/Event 迁入 C24；不在本轮指定 |
| `R07-01` | **单份布局已收口：** 状态区按 Memory 记录和 16 核重算 offset；第二块 source 返回与 Dispatch 槽位各自预留，第三块为统一 relay 结果区 | 主设计列出 base/offset/stride、字节公式和完整示例；当前非量化返回沿用 Memory DataAsFlag，后续 dtype/并发分别归 R08/R11，框架扩容不在本方案内 |
| `R07-02` | **已确认并关闭：** 一块统一中转区，预留 8*BS 个 group、每组 K 个槽位；group 表同步预留 8*BS 行，全卡共用 `[0,8*BS)` 的 t | 七/八来源卡分区候选撤回；本卡与其他卡返回共享容量，实际 group 数合计不超过 8*BS 由安全用例保证 |
| `R07-03` | **已确认并关闭：** `recvSlot=uint64(t)*K+originalK`，地址为目标 relay 本轮中转区 base 加 recvSlot*stride，本卡及跨卡返回相同 | 每组结果区按原始 k 留槽，members 前 n 项密排；n=1 仍预留 K 槽，只监听一个返回，第一跳由 firstHopRow 读取；完整区域字节布局归 R07-01 |
| `R07-04` | 已按用户要求低优先级延后，本轮无新增方案选择 | 保留尽力转发备忘；不做 X 超限检查、报错、降级或测试 |
| `R08-01` | **随 R04-07/R04-08 确认并关闭：** ExpertOut 为未加权 expert 输出，relay 用真实权重归约；source 按 sourceMask 统一乘 expertScales，其中 partial 槽为 1.0 | 不新增结果类型载体；本轮确定公共职责，Combine 内部乘法/累加实现仍归其文档 |
| `R08-02` | **首版已确认：** 包/assistEx/group 使用真实输入权重，expertScales 必须非空 FP32 `[BS,K]`；expertScalesOut 独立输出 | nullptr 直接报错；测试端固定种子随机生成正权重，按 Token 归一化为和 1，不在算子中新增归一化或求和检查 |
| `R08-03` | **Dispatch 部分已确认：** BF16/FP16 输入，quantMode=0 非量化及 quantMode=4 MXFP8，沿用 Memory 的 E4M3/E5M2；量化计算、scale 布局、输出接口复用 | expandXOut 紧密 [tokenNum,H]，MXFP8 每元素 1B，dynamicScalesOut 为 [tokenNum,align2(ceil(H/32))] 字节；expertOut/Combine 返回公共格式仍待讨论，内部数值见 C15/C16 |
| `R10-01` | **权重及量化 ABI 已收口、待代码接入：** public 增加 expertScalesOut；量化复用已有 dynamicScalesOut/quantMode/expandXOutDtype，并传入 launch/Device | 共 13 个 GM_ADDR + 10 个 int64，184B；expertScalesOut offset=96、bs offset=104、quantMode offset=152、magic offset=176，无新增 shape/handle |
| `R10-02` | **随 R04-02 确认并关闭：** 保持 Memory 行序，`tokenNum=sendCounts[L*R-1]`；按 capacity 分配、按 `tokenNum` 消费，不读取 padding 行 | 不新增 tokenNum 参数或通知；expert 内部若重排，由调用方在交给 Combine 前还原，不另设行映射 |
| `R10-03` | 沿用 Memory 连续紧密行接口：输入 [BS,H]、Dispatch/expert 有效输出 `[tokenNum,H]`、最终 [BS,H]；32B 是 UB/通信对齐，不强制用户 H 字节已对齐 | 支持 dtype 依 R08-03，公开任意 stride 作为备选扩展；不能因 Memory 某格式有源码就视为去重已验证 |
| `R10-04` | 沿用 BS>0、H>0；建议首版不扩展 BS=0。`tokenNum=0` 是合法接收结果，仍输出全零前缀并完成本 Rank 其他职责；其他输入输出建议不允许别名，expertScales/expertScalesOut 同样要求独立存储 | BS/H 上限依新的 UB/窗口/字段宽度预算，不能原样使用旧容量拒绝条件；其他零长度指针规则可作为单独扩展，expertScales 必须非空 |
| `R10-05` | 沿用调用方共同约定及 Host 本地检查：R、BS、H、K、dtype、布局版本和调用顺序一致；globalBs 可取 0 自动推导为 R*BS | 不新增跨 Rank BS/top-k 扫描，不校验已保证的安全 route 或中转容量，不引入 X 输入 |
| `R10-06` | 去重能力表：mask 为 NONE/null，shared 参数为 0，TP world 为 0/1 且 rank=0，shardType=0；支持的 weight 输入单列，其他未使用 scale/TP 输出按默认空值 | 非默认不支持配置在 Host 返回错误；不能泛化为“所有非空 expertScales 都拒绝”，也不回退旧入口 |
| `R11-01` | 沿用 E06 的每次通信调用新 magic；持久 metadata 绑定原 Dispatch 身份、comm 和 layout，后续 Combine invocation 使用自己的通信轮次 | Plan 身份与窗口 magic 分开；固定版本首版可只在 Host 描述对象保存，无需每个 Token 重复携带 |
| `R11-02` | 推荐允许同一不可变路由计划顺序 Combine，多次调用使用新 magic；expertOut 可更新数值但 row/H/dtype 不变，消费同一份 expertScalesOut | 可选一次 Dispatch 配一次 Combine；原始输入 expertScales 保持真实值，失败重试支持仍待确认 |
| `R11-03` | 推荐首版同一 comm 单 stream、同一时刻一条 Dispatch/expert/Combine 调用链，调用方顺序一致 | 多 stream/多个未完成 plan 延后；旧 Memory 没有证明多份新 relay 状态可以共享窗口 |
| `R11-04` | Dispatch 算子完成包含全部 GM 写入已由 R04-04 收口，含 expertScalesOut 的 source 归约权重写出；本项仅保留 Host 异步调用及各资源释放时点的生命周期约定 | 本地 event 不自动证明所有远端已消费；metadata/expertScalesOut 到最后 Combine 消费完成才可释放，公共窗口跨 Rank 复用约束保留 R11-05 |
| `R11-05` | Counter 清零方案已确认：本轮所有核结束任务后执行 SyncAll()，核 63 清零唯一 counter 的首 4B；首次零值沿用 peer memory 初始化 | 不再有 counter 乒乓或交替选窗映射问题；窗口旧 Flag、通信数据和 metadata 的复用边界仍保留，Combine 内部机制迁入 C24 |
| `R12-01` | 适配 E09：独立遍历原始 expertIds 生成 route/q/期望行，另推导首跳和成员；检查 metadata/权重/引用及每条原始 expert 行恰好一次 | 测试不能要求 AtomicAdd 的 t 按原 Token 顺序；只检查唯一性和身份关联，避免以 kernel 生成表验证自身 |
| `R12-03` | 沿用本节安全范围矩阵，覆盖 K=1/32、`tokenNum=0`、尾部、source 内 DIRECT、本卡/跨卡 relay、实际 group 数不超过 8*BS、乱序和跨轮 metadata 读取 | Dispatch 覆盖 BF16/FP16 的非量化及 MXFP8 E4M3/E5M2 六种组合；非量化按原值/位一致，MXFP8 沿用 Memory hidden/scale golden 检查，Combine 数值矩阵在 C23 |
| `R12-04` | 适配 Memory demo/profile：在同配置同路由下报告 Dispatch 完整 latency 与真实字节，覆盖两类发送、relay、本地 copy、metadata 和 Flag，统计各 Rank median/min/max | 先保留典型 64/128P、384 expert、K=6/8/16、BS=256/H=7168 场景，确认容量后再设收益阈值；不把未测建议当作性能结论 |

## 已归并结论

| 已确定内容 | 主设计位置 | 来源 |
| --- | --- | --- |
| 8P/Server、16P 至 128P、总 expert 数不超过 1024、连续均匀映射、允许 L=1、K<=min(32,moeExpertNum)、同 BS、安全输入及阶段一排除功能 | [支持范围](ep-memory-server-dedup.md#支持范围) | D01、D02、D08 |
| 只做 Server 级去重，Server 内逐 expert 发送；source 直发所有原始 cnt | [路由与计数](ep-memory-server-dedup.md#路由与计数) | D01、D02 |
| 仅对远端目标 Server 去重，源 Server 内逐 route DIRECT；远端按 top-k 首目标选 relay，本卡/跨卡转发统一取号和 Combine 返回 | [路由与计数](ep-memory-server-dedup.md#路由与计数) | R01 |
| 源端按 Token 分核，各核先下发第一跳再下发 DIRECT，切换不加屏障或单独等待 MTE3；q/cnt 沿用 Memory，取消双 Group | [源端两阶段发送](ep-memory-server-dedup.md#源端两阶段发送) | R02 |
| Memory cnt/数据分区、source/expert/q 地址、第一跳占实际 expert route 槽位 | [Dispatch 接收布局](ep-memory-server-dedup.md#dispatch-接收布局) | D02 |
| 保留原始 entries，同一 Token 的终端发送共用 UB；接收端按 expert 匹配 k/weight | [Token 格式与转发](ep-memory-server-dedup.md#token-格式与转发) | D03、D11 |
| 拷贝/量化后的数据与补充信息分别按 32B 对齐，信息追加在 Token 后，随同一条记录发送并占用同一接收槽位 | [随 Token 存放](ep-memory-server-dedup.md#随-token-存放) | R03 |
| Token 数据与转发信息统一作为 Payload；Flag 拼接、检测和剥离沿用 Memory Dispatch 的 DataAsFlag | [Payload 与 DataAsFlag](ep-memory-server-dedup.md#payload-与-dataasflag) | R03 |
| Cumsum/本地 copy/转发同组核；先 cumsum、按 `tokenNum` 均分、双表、末项覆盖剔除及退出前 PIPE_ALL | [分核与监听](ep-memory-server-dedup.md#分核与监听) | D05、D11 |
| Relay 首次转发前按不同 Token 原子取号一次，包含仅有本卡转发；编号随所有副本发送，Dispatch 生成持久监听信息 | [中转编号与持久表](ep-memory-server-dedup.md#中转编号与持久表) | D06、D08、R01 |
| relay 对本 Server 内同一 Token 的第一跳和转发副本执行加权求和，回 source 后直接累加；DIRECT 在 source 加权 | [Combine 数据流](ep-memory-server-dedup.md#combine-数据流) | 主设计、D06、C01/C03/C04、R08 |
| relay group 固定预留 K-1 个 `(recvSlot,weight)`，前 n 项密排，第一跳单独引用 | [Relay Group 成员表](ep-memory-server-dedup.md#relay-group-成员表) | R04 |
| 一块统一中转区，预留 8*BS 个 group、每组 K 槽，recvSlot=t*K+originalK；group 表预留 8*BS 行；安全用例保证合计不超容量，不做超限校验/处理/测试，X 及超限优化延后 | [容量与生命周期](ep-memory-server-dedup.md#容量与生命周期) | R04-05、R07-02/R07-03 |

不重新讨论已经确定的 top-k 查重/invalid expert 处理、卡级去重、转发 cnt、逐 Token 控制包、
cumsum 前监听或普通路径自动回退。共享专家继续延后。
“远端单目标”按一条 route 理解；一张卡上的多条 expert route 已确认采用第一跳加本卡转发。
远端 relay 的本卡转发同样分配 forwardTokenIdx 并走 Combine 返回；源 Server 内直发已由 R01 确认。
持久表与权重职责已在 R04 关闭；独立输出 ABI 已由 R10-01 给出具体结论，剩余为代码接入。

## 第二轮索引

| 编号 | 议题 | 类型 | 需要形成的结论 | 来源 |
| --- | --- | --- | --- | --- |
| [R01](#r01) | Relay 选择与统一转发规则 | 已确认，关闭 | 仅跨 Server 去重、首目标 relay、本卡/跨卡统一转发和编号；源 Server 内逐 route DIRECT | D04、D06；C01 |
| [R02](#r02) | 源端两阶段发送与 Memory 复用 | 已确认，关闭 | 各核先第一跳后 DIRECT，无阶段屏障；R02-01 已确认 serverRouteMask + directMask，cnt/cumsum/q 复用 | D04、D05 |
| [R03](#r03) | 公共 Token 的字节布局与到达判定 | 已确认，关闭 | 16B entry、固定 K-1 容量、不清零无效区域及按有效长度解释均已确认，Payload/对齐/DataAsFlag 保持 | D03；C06 |
| [R04](#r04) | 持久 Plan 布局与写入分工 | 初版已确认，关闭 | metadata、行关联、槽位及 GM 完成语义已确认；source 复用 expertScales 统一加权，无独立结果类型标记；仅 R04-06 低优先级遗留 | D06、D08；C01/C02/C03 |
| [R05](#r05) | Dispatch 分核、原子与 UB/Event 资源 | 数据转发部分待确认 | R05-01 至 R05-04 的核组、取号、源端 UB/Event 与写出方案已收口；R05-05/R05-06 数据转发流水另行设计 | D05、D06、D11 |
| [R06](#r06) | Flag 清理时机与顺序 | 待确认 + 实现核对 | 核末或 Token/批次清理、具体排序与范围 | D07、D11；C06/C09 |
| [R07](#r07) | 完整 Workspace 与槽位布局 | 单份布局已收口；超限低优先级遗留 | 三块通信区的具体 offset、非量化 stride、完整预算已列出；跨调用复用归 R11，通信域扩容归框架；超限处理延后 | D07、D10；C09/C12 |
| [R08](#r08) | 加权位置、权重与公共数值契约 | Dispatch 部分已确认 | 权重及 BF16/FP16 非量化/MXFP8 Dispatch 已收口；剩余为 expertOut/Combine 返回公共 dtype/stride，内部数值规则在 C15/C16 | D09；C03/C04/C05 |
| [R09](#r09) | Combine 内部调度与 H 分块归约 | 已迁移 | 仅保留迁移索引；四类职责、累加器、慢成员和完成条件见 Combine C17-C22 | C07/C08/C12 |
| [R10](#r10) | 双入口的具体 ABI 与 ExpertOut 契约 | 部分确认 | 有效行数/行序及独立权重输出 ABI 已收口；其余 dtype/stride、调用约束和不支持参数规则继续讨论 | D08、D10；C02/C11 |
| [R11](#r11) | Plan、执行轮次与异步生命周期 | 待确认 | generation/epoch、重复调用、并发与跨轮复用 | D08；C10/C12 |
| [R12](#r12) | Dispatch 与公共契约验收标准 | 待确认 | Dispatch 路由/元数据/通信字节和公共生命周期验收；Combine 内部验证见 C23/C24 | D10；C13/C14 |

R01/R03 及 R04 初版设计已确认关闭，R02 已确认的发送顺序及 Memory 复用保持。
R02-01 已确认；源端 UB/Event、单份区域布局及权重 ABI 已给出可直接实现的结论，不再以这些项阻塞 sendData 和 recvData 前两部分。数据转发继续讨论 R05-05/R05-06。
其余执行机制仍在 R05/R06/R07/R11，R09 转由 Combine 文档讨论，R12 随相关决定补齐；不重开已确认的 cnt/cumsum 设计。

## R01

**议题：Relay 选择与统一转发规则**

**状态：已确认，关闭。** 跨 Server 适用范围、relay 选择、本卡/跨卡统一转发、编号和 Combine 处理均已确认，
已合入[主设计](ep-memory-server-dedup.md#路由与计数)，本项无独立遗留问题。
持久表布局和写入分工见 R04；本卡/跨卡统一槽位见已确认的 R07-02/R07-03，完整单份区域预算见已收口的 R07-01。

**已确认：**

- 去重仅适用于原始 source 与目标位于不同 Server 的 route，目的是降低 Server 间通信量。
  源 Server 内逐 route DIRECT，包含本卡多 expert 和同 Server 多 Rank；不选 relay、不创建 group、不取转发编号，
  不计入中转 group 数或转发结果条数，Combine 直接返回 source 对应的原始 k 槽位。
- 阶段一固定 top-k 首目标为 relay。按每个远端目标 Server 分别取属于该 Server 的最小原始 k，
  该 route 同时确定 relay Rank、第一跳 expert 和 returnK；负载均衡选点留到进阶版本。
- 该 Server 有 m>1 条 route 时，只有第一条由 source 发送，`forwardCount=m-1`；
  其余 m-1 条全部列入 forwardEntries，由第一跳的后续转发逻辑发送。
- 后续 route 即使指向 relay 本卡也记为一次转发，写入对应 expert 的独立接收槽位。
  所有后续包均为 `forwardCount=-1`，不为单卡多 expert 设置另一套 Dispatch 处理流程。
- 只要 m>1，就由第一跳 owner 在首次转发前原子分配一次 forwardTokenIdx。
  仅有本卡转发也使用同一计数器、同一索引域，所有副本携带相同的 relayRank/forwardTokenIdx。
- 本卡终端接收按相同方式记录转发信息，Combine 同样作为转发 Token 返回并监听其结果。
  不因接收卡等于 relay 而省略取号、退回 DIRECT 或改为直接读取额外本地 expert 行。
- 每个 group 对应本 relay 的一个不同转发 Token，包含仅有本卡转发的情况。
  Y 表示全部转发结果条数，包含本卡返回，不含第一跳贡献。这里描述数量关系，不要求对外输出 X/Y 计数。

**统一处理示例：** Group 是同一原始 Token 在该目标 Server 的 expert 贡献集合。
全部 expert 都在 relay 本卡也按相同的 group 处理，不再作为独立待定的编号场景。

例如 L=3，R0/T8 在 Server 1 的 route 按原始 k 顺序为 expert25、expert26，二者都位于 R8：

```text
Dispatch:
R0/T8 -> R8/expert25       first hop, forwardCount=1
R8 allocates forwardTokenIdx=g once
R8    -> R8/expert26       forwardCount=-1, relayRank=8, forwardTokenIdx=g

Combine:
R8/expert26 -> R8 return slot(sender=8, idx=g, k=expert26's original k)
R8/expert25 + returned expert26 result -> R8 partial -> R0/T8[returnK]
```

这里有一个含两份 expert 贡献的 group，Dispatch 转发一次，Combine 本卡返回一次，
实际 group 数增加 1，转发结果条数 Y 增加 1。本卡返回写入统一中转区的 `t*K+originalK` 槽位。
若将第二个 expert 改为 R9/expert28，取号和记录规则完全相同，仅发送卡和返回槽位随目标变化。
若保留 expert26 并再增加 R9/expert28，则仍只取一个编号，group 数增加 1，Y 增加 2。

**当前分类：** m 是该 Token 在目标 Server 的 route 数；源 Server 由原始 srcRank/8 确定。

| 场景 | Dispatch 行为 | Combine 行为 | 新增 group 数 / 关联编号 |
| --- | --- | --- | --- |
| 目标为源 Server，m>=1 | 全部逐 route DIRECT，forwardCount=0 | 各结果直接回 source 的原始 k 槽位 | 0；不创建 group、不取号 |
| 远端 Server，m=1 | DIRECT，forwardCount=0 | 单条结果直接回 source | 0；无 group 返回编号 |
| 远端 Server，m>1，全部 expert 在 relay | 第一跳加 m-1 次本卡转发 | 第一跳贡献加 m-1 份本卡返回，回一份 partial | 1；首次转发前原子分配一次 forwardTokenIdx |
| 远端 Server，m>1，含其他卡 expert | 第一跳加 m-1 次转发，包含全部额外本卡 route | 第一跳贡献加全部转发返回，回一份 partial | 1；首次转发前原子分配一次 forwardTokenIdx |

适用范围根据原始 srcRank 判定，不根据上一跳发送卡判定。
例如 R0 -> R1/R2 均在源 Server 内，两条 route 直接发送；R0 -> R8/R9 跨 Server，
则由 R8 接收一份第一跳再转发给 R9。后者的 R8 -> R9 虽为 Server 内发送，仍是跨 Server 去重后的必要展开。

**关联议题：** R04 已确认同一编号下的持久记录、输出行定位及写入分工，GM 写入沿用 Dispatch 算子完成语义；
R07-02/R07-03 已确认本卡及跨卡返回共享统一中转区，完整单份预算见已收口的 R07-01。关联设计均以本项统一转发规则为前提，
不重新讨论纯本卡 group 是否取号或是否走 Combine 转发处理。

**关闭结论：** 本项的适用范围、relay 规则、统一转发和编号均已确认；持久表及统一槽位分别见 R04/R07 的已确认结论。

## R02

**议题：源端两阶段发送与 Memory 复用**

**状态：已确认，关闭。** 各发送核对本核 Token 区间扫描两遍，先第一跳后 DIRECT，阶段之间不插入屏障或单独等待 MTE3。
按 Token 分核、q/cnt/cumsum 整体沿用 Memory 及取消双 Group 均已确认；R02-01 的 serverRouteMask + directMask 分类与 UB 临时存放亦已确认。

**已确认：**

- 原始 source 的发送分两阶段：先筛选并发送需要目标 relay 展开的第一跳包，再发送无需展开的 DIRECT 包。
- 按原始 Token 分核，由其 owner 联合处理全部 K 条 route，两阶段维持同一 Token 的处理归属。
- 每个发送核独立将自己负责的第一跳任务全部下发后，继续处理自己的 DIRECT 任务，不等待其他核。
  阶段切换不插入 PIPE_ALL、SyncAll 或其他阶段屏障，不单独等待 MTE3 完成；无第一跳任务的核直接处理 DIRECT。
- q 沿用 Memory 的原始路由前缀计算：以 `index=srcTokenId*K+originalK` 调用 `CalTokenSendExpertCnt`，
  统计完整原始 expertIds 在 `[0,index)` 中命中目标 expert 的条数。对每个 source/expert，其 q 自然覆盖 `[0,cnt)` 且唯一，
  与发送分核和实际发送顺序无关；阶段一不采用新的分块统计加前缀算法。此项已确认，不再作为算法选择题。
- Cnt 核按 Memory 分配的 expert 范围独立读取完整原始 expertIds，沿用 `CalAndSendCntByRankAndExpert` 统计，
  不消费源端去重预处理结果，因此不为共享该预处理结果新增 workspace 或同步协议。
  既有范围筛选计数/逐 expert 计数分支、cnt 记录、接收、cumsum 计算、必要同步、状态清理及输出语义整体沿用 Memory，不只复用数值结果。
  不因模块改名而拆开或重写既有函数；固定 48/16 核组、取消双 Group 和本版本区域地址是已确认适配。此项不再讨论算法或统计输入选择。
- Cnt 始终由原始 source 为所有原始 expert route 发送，包含 DIRECT、第一跳和后续 relay route；不按两阶段拆分或只统计实际第一跳包。
- 全核/非全核 cnt 的 BS<48 分界沿用 Memory；具体核数按 R05-01 的新配置：默认 64 核，BS<48 时 64 核发 cnt，BS>=48 时仅 cumsum 的 16 核发 cnt。
  Cnt 接收/cumsum 始终由固定 16 核负责，不沿用旧动态核数计算或最多 8 核的限制。
- 去重版取消双 Group 的 32 Rank 条带过滤、cnt/数据双轮发送及其 SyncBeforeSendRound/SyncAll。
  Cnt 按现有核组分工覆盖全部目标 expert，每个 expert/source 记录发送一次，无需等待旧轮次同步。
  本设计的第一跳优先、DIRECT 随后的两阶段发送保留。

**原表述的含义：**

| 名称 | 具体指什么 |
| --- | --- |
| sourceMask | Source 在 Combine 最终要读取哪些原始 k 槽位；DIRECT 的 k 保留，每个远端 group 只保留 returnK |
| 分组 | 根据本 Token 的 top-k，把属于同一远端 Server 的 route 归在一起，确定首目标及 forwardEntries |
| 第一跳 descriptor | 第一跳发送所需的 relay/expert/q、k/weight、转发次数及 entries；正文改称“第一跳发送信息” |

“Descriptor”不是已经决定的新结构体、tensor 或控制包；可用临时信息直接构造第一跳 UB。
SourceMask 已在 R04 确认由该 Token 的数据发送核在 UB 生成，每 Token 一个 32bit 值，核末一次写出到 sourceMask 输出 GM；本项沿用该约定。

**与 R03 字段表的衔接：** 第一跳发送信息的随包内容已展开为主设计的[字段表](ep-memory-server-dedup.md#尾部字段字节表)和
[各类包有效值](ep-memory-server-dedup.md#各类包的字段值)。以下明确发送职责；固定头部、16B entry 编码、K-1 容量及无效区域不清零均已确认：

- 原始 source 准备 srcRank/srcTokenId、当前 DIRECT 或第一跳 route 的 k/weight，以及 forwardCount。
  对第一跳，source 还准备全部后续 entries，其中每条 destinationQ 沿用原始 source/expert 前缀计算，originalK/weight 对应该条 route。
  已确认第一跳的 forwardEntryCount=n，DIRECT 为 0；有效长度不改变原始 cnt，也不构成另一轮计数通知。
- forwardTokenIdx 由目标 relay 的第一跳 owner 在首次转发前分配一次，source 不提前取号。
  relay 统一设置 forwardCount=-1、relayRank 和 forwardTokenIdx，保留原始身份、头部 k/weight、有效长度及全部 entries；
  这属于接收核组的转发职责，不是原始 source 的第三个发送阶段，也不要求 source 等待 relay 完成。
- q/cnt 和 source/expert 分区公式继续沿用 Memory，但 Token 加长后的通信 stride T 由 R03 统一确定。
  DIRECT、第一跳和终端包均使用新 T 计算地址，不能沿用旧 Token 的字节跨度，也不能按当前有效 entry 数改变槽位跨度。

**两阶段的分类单位：** 一个原始 Token 可同时有需要转发和 DIRECT 的发送包。
例如 R=64、L=3、K=4，R0/T1 命中 `[25,28,4,50]`：

| 阶段 | source 发送内容 |
| --- | --- |
| 第一阶段 | R0 -> R8/expert25，forwardCount=1，entry 指向 R9/expert28 |
| 第二阶段 | R0 -> R1/expert4；R0 -> R16/expert50，均为 DIRECT |

SourceMask 为 `0b1101`，保留 k0/k2/k3。Source 第二阶段不再发送已交由 relay 处理的 expert28。
不能把整个原始 Token 永久划入某一阶段而丢掉另一类 route；每核先遍历其第一跳任务，再遍历其 DIRECT 任务。

**Memory 源码核对：**

1. `SendToMoeExpert` 当前按扁平 `index=aivId; index<BS*K; index+=moeUsedAivNum` 分核。
   新入口改按 Token 分核，但每条 route 仍按 `index=srcTokenId*K+originalK` 调用 `CalTokenSendExpertCnt`。
   后者统计原始 expertIds 的 `[0,index)` 前缀。同一 Token 内 expert 不重复，因此等价于更早 Token 命中该 expert 的数量。
   例如 T0 对 expert28 是 DIRECT，而 T1 对 expert28 是需转发成员，先发送 T1 不会把它的 q 从 1 变成 0。
   原始 expertIds 必须保持完整和原顺序，筛选任务表不能替代该前缀。此处未发现算法冲突。
2. `CalAndSendCntByRankAndExpert` 按 expert 范围分配 cnt 任务，每核读取完整原始 expertIds，
   用现有范围筛选计数或逐 expert 计数生成原始 cnt；`SendExpertCntRange` 按目标 expert/source 状态位置发送。
   `WaitDispatch` 等待 cnt 标志，`CalRecvAndSetFlag` 计算并发布 cumsum；不依赖 Token 按 q 顺序到达。
   因此更改数据发送顺序不要求修改 cnt 数值、接收分区或 cumsum 算法。
3. `BS<48` 时当前 Memory 全核发送 cnt；否则仅 cumsum 核组发送 cnt。
   在无共享专家、R=64/128 且 BS>=48 时，`CalSendRound` 开启两轮发送，按 `(rank/32)&1` 划分条带。
   这些条带每组含多个 8P Server，与去重的目标 Server group 含义不同。
   Cnt 和数据发送各在第二轮前调用一次 `SyncBeforeSendRound`，其中有 `PIPE_ALL` 和 `SyncAll`。
   Cnt 核完成两轮发送后才进入接收/cumsum；其全核同步目前依赖数据发送核到达匹配位置。
   **该双 Group 行为已明确不继承到去重版**，上述内容仅说明当前 Memory 的实现边界。

以上依据为当前[Memory Dispatch](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h)的实际调用路径及
[Memory 常量](../../src/ep/common/ep_memory_window.h)，属于源码核对，未编译或在设备上验证新调度。

**双 Group 的复用边界：** 去重方案在 cnt 和数据两侧同时取消旧双 Group 轮次，
不会保留“cnt 等待第二轮全核同步、数据核已不参加”的混用路径。
沿用 cnt 算法不等于保留其条带发送调度；接收、cumsum、UB/Event 依赖以及结束前的必要流水等待仍保留。

**核内执行边界：** 一个发送核下发完其全部第一跳任务后即可下发 DIRECT，先前的 MTE3 可以仍在执行。
其他核此时可以仍处理第一跳；本 Rank 不建立统一阶段切换点。
两类任务使用发送资源时仍遵循每份 UB/Event 的正常复用依赖，R05 负责具体安排；
该资源依赖不构成第一阶段结束时的单独 MTE3 等待，也不要求目标 relay 展开完成。

**已确认结论：** Token 分核、两遍扫描及核内第一跳优先顺序、无阶段屏障/单独 MTE3 等待、q/cnt/cumsum 复用及取消双 Group 均已确认。
sendCountsOut 的 int32[L*R] 含当前项前缀、expertTokenNumsOut 的 int64[L] 计数/前缀语义均沿用 Memory，不另行选择 shape。
零 cnt 和整卡 tokenNum=0 仍完成既有通知、等待、cumsum 输出与状态清理，后续不生成空区间的监听任务；这些均不再待定。
随包编码与新 T 归 R03，持久格式归 R04；R02-01 的分类及 R05-04 的源端 UB/Event 均已收口。
后续实现验证需覆盖混合 route、无第一跳任务和跨核进度不同的情况。

### R02-01 两遍扫描的 route 分类（已确认，关闭）

**已确认边界：** 两遍指对本核的完整 Token 任务区间先后扫描，第一遍下发所有远端多 route group 的第一跳，第二遍下发所有 DIRECT。
不是对一个 Token 发完第一跳就立即发其 DIRECT，也不限制构造单个包时对其 K 条 UB 路由的局部访问次数。
Cnt 和 q 继续使用 Memory 的原始 expertIds，不消费这里的临时分类结果。

**已确认方案：当前 Token 的 Server 位图分组，加每 Token 一个临时 directMask。** 不新增 public 输出。

| 临时值 | 存放/大小 | 用途与有效期 |
| --- | --- | --- |
| serverRouteMask[16] | 本核 UB，16 个 uint32，共 64B | 仅为当前 Token 分组，bit k 表示该 Server 命中原始第 k 条 route；每个 Token 使用前清零，可在下一 Token 复用 |
| directMask[count_i] | 本核 UB，每 Token 一个 uint32，按 align32(4*count_i) 分配 | 第一遍记下第二遍需要直发的 k，保留到第二遍消费；不写 GM、不传给 Combine |
| sourceMask[count_i] | 已确认的源端 UB 输出暂存 | 第一遍置已发 group 的 returnK 位，第二遍置 DIRECT 位，核末按已有规则写出 GM |

第一遍处理每个 Token：

1. 从 Memory 已加载的原始 expertIds UB 读取 K 个 ID，以 `serverId=(expertId/L)/8` 计算目标 Server，将 `uint32(1)<<k` 放入对应 serverRouteMask。
2. 源 Server 的全部位归 DIRECT；远端 Server 的单 route 位也归 DIRECT，合并到本 Token 的 directMask。
3. 对远端多 route 位图，取最小原始 k 作为第一跳，其余置位的 k 按升序构造 forwardEntries；同卡其他 expert 与跨卡 expert 同样进入 entries。
4. 头部和每条 entry 的 q 分别按 `index=srcTokenId*K+originalK` 调用 Memory CalTokenSendExpertCnt；携带真实 route weight，发送第一跳并置 sourceMask 的 returnK 位。
5. 该 group 的全部位（包括第一跳位）均不进入 directMask，第二遍无需再识别 relay 或重新分组。

位图为 0 表示未命中；非零位图满足 `(bits & (bits-1))==0` 时为单 route。原始 expertIds 保持不变，不把筛选结果覆盖回输入 UB。
取首位和按 k 升序枚举均是本核至多 K=32 的局部操作；不依赖新的跨核表、排序、原子计数或额外通知。
这里的分类属于去重新增逻辑，不替换 Memory 的 cnt 范围筛选计数，也不添加重复 Top-k/非法 expert 的安全检查。

第二遍遍历同一 Token 区间，只对 directMask 的有效 k 构造 DIRECT 包（forwardCount=0），q 仍按原始 index 计算。
每发出一个 DIRECT，将其 k 位并入 sourceMask；不能直接用 sourceMask 筛选第二遍任务，因为其中还包含已经发出的 group 第一跳。
两遍共用原发送池及包序号，无阶段屏障。随包权重始终从只读 expertScales 输入取值，source 归约权重写独立 expertScalesOut，输入不被覆盖。
临时 serverRouteMask 可以复用的前提是有效头部/entries 已构造进受发送流水保护的 UB；在途发送不读取该分组表。

**示例：** L=3，R0/T0 的 K=6 路由为 `[4,25,28,50,26,5]`。

| Server | 原始 k / expert | 第一遍 | 第二遍 |
| --- | --- | --- | --- |
| S0（源 Server） | k0/E4、k5/E5 | 记入 directMask | 两条均 DIRECT，即使同卡多 expert 也不合并 |
| S1 | k1/E25、k2/E28、k4/E26 | R8/E25 为第一跳，entries 为 R9/E28、R8/E26，forwardCount=2 | 三条均不再由 source 发送 |
| S2 | k3/E50 | 记入 directMask | DIRECT |

第一遍之后 directMask=`0b101001`，sourceMask=`0b000010`；第二遍之后 sourceMask=`0b101011`。
无远端重复 Server 时第一遍无包，DIRECT 全在第二遍；K=1 同样成立。全为远端 group 时 directMask=0，第二遍无包。
每个 Token 可包含多个远端 group，分别选择各自首 k、生成 entries，不将整个 Token 限定为单一目的 Server。

**已确认的代价：** 用每 Token 额外 4B 的 UB 保存分类，第二遍不再重建 Server 分组。
BS=256 时每核 5 或 6 个 Token，directMask 有效 20B/24B，按 32B 分配；连同可复用的 64B 分组表，额外 UB 为 96B/核，不含已有 sourceMask 和发送 buffer。
采用保存 directMask 的方案，不再保留第二遍重建分组的设计选择；设备性能收益仍需实际测量。

**关闭结论：** 用户已确认上述 serverRouteMask + directMask 方案，已合入主设计；cnt/cumsum/q 和发送 Event 流程不重新讨论。

## R03

**议题：公共 Token 的字节布局与到达判定**

**状态：已确认，关闭。** 字段含义与三类包的处理方式已有完整表达，随 Token 存放、两段分别 32B 对齐、
统一 Payload 及 Memory Dispatch DataAsFlag 的 Flag 拼接、检测和剥离均已确认。
固定头部的八字段 32B 编码已随 R04 的 assistEx 布局确认；本轮 R03-01/R03-02 已确认每条 entry 16B、每包固定预留 K-1 条。
R03-03 已确认采用不清零策略，通过 n/有效数据长度排除无效字节影响，已同步回写主设计。

**已确定前提：** 保留原列表、公共转发 UB 和终端匹配 expert 已确定，目前未发现完整示例在已确认范围内存在功能冲突。
主设计已补充[尾部字段字节表](ep-memory-server-dedup.md#尾部字段字节表)、
[各类包的字段值](ep-memory-server-dedup.md#各类包的字段值)、
[entry 布局](ep-memory-server-dedup.md#forwardentries-条目字节表)及
[容量计算](ep-memory-server-dedup.md#尾部容量与通信长度)；其中每条 16B entry 和固定 K-1 容量已在本轮审核确认。

**本轮确认：** 补充信息直接接在 Token 数据后，随同一条加长记录发送，位于该 Token 的接收槽位内。
不额外划分转发信息区域，不单独发送 metadata 包或尾部 ready 通知。DIRECT、第一跳及本卡/跨卡终端包使用相同方式。
数据拷入 UB 后先完成所需量化，以此时待发送数据的有效字节跨度 D 向上对齐到 32B；
补充信息自身也按 32B 对齐，追加在 `align32(D)` 处。量化 D 包含随包 scale 及该格式内必须占用的间隔，不能使用量化前输入长度代替。
布局为 `[处理后数据][数据段对齐填充][补充信息][信息段对齐填充]`，两段分别对齐后相加；整条 Token 使用统一的地址跨度和到达协议。
32B 对齐作用于两段整体，不要求每个 4B 字段或每条 16B entry 单独扩充为 32B。
接收后为 Combine 保存的持久表继续归 R04，这不引入额外的随包信息通信区域。
计算公式与 Memory 源码依据见主设计[尾部容量与通信长度](ep-memory-server-dedup.md#尾部容量与通信长度)，对齐方式不再列为待确认项。

**统一 Payload 与 Flag：** 上述对齐后的 Token 数据及全部补充信息共同组成一个 Payload。
沿用 Memory Dispatch DataAsFlag：每连续 480B Payload 数据后拼接 32B Flag，末块数据补足 480B，
`blockCount=ceil(tokenPayloadBytes/480)`，通信跨度 `T=blockCount*512`。
接收端按整个 Payload 的 blockCount 检测 Flag，确认到达后逐块取出 480B 数据、跳过 32B Flag，恢复连续 Payload。
剥离后每 Token 的 UB 跨度为 `packedPayloadBytes=blockCount*480`，再从 tailOffset 解析转发信息。
具体流程见主设计 [Payload 与 DataAsFlag](ep-memory-server-dedup.md#payload-与-dataasflag)；
不再把 Flag 拼接方式、检测范围、剥离方式或 T 的计算公式列为待确认项。后续实现适配与设备验证归 R12。

**字段表已覆盖的问题：**

- 当前格式用 forwardEntryCount 保留有效长度，因此 forwardCount=-1 后仍能恢复 n。
- 公共头部携带 relayRank/forwardTokenIdx，终端可保存完整 Combine 返回引用，原始 entries 无须被覆盖。
- 终端在 `[0,forwardEntryCount)` 中按自身 expert 匹配，取得自己的 originalK/weight；本卡转发与跨卡转发一致。

这些问题已有具体表达，不再作为“缺少信息”重复提出。固定头部的 int32/FP32 类型、offset、
未使用返回字段填 -1 和 DIRECT 有效长度为 0 已随 R04 确认；entry 字节布局、预留容量和无效区域不清零已由 R03-01/R03-02/R03-03 确认。
统一 Payload、整体对齐和 DataAsFlag 规则保持已确认，设备验证状态另行记录。

**具体场景：** K=6 的 Token 在某个 Server 只有三条 route，则 n=2。
按当前字段表，forwardEntryCount=2，终端只匹配两条有效 entry。
按已确认的 K-1=5 预留，有效尾部为 64B，预留原始长度为 112B，经 32B 对齐后占用 128B。
其中 48B 是未使用 entry，另有 16B 对齐填充；不能因为当前只有两条 entry 就改变该 Token 接收槽位的跨度。

**Dispatch 子项状态：**

| 子项编号 | 项目 | 当前基线与需要落实的内容 | 类型 |
| --- | --- | --- | --- |
| R03-01 | Entry 编码 | 顺序为 destinationExpertId、destinationQ、originalK、weight，前三项 int32_t、最后一项 FP32，各 4B，offset 为 0/4/8/12，合计 16B | 已确认，关闭 |
| R03-02 | 预留容量 | 每包固定预留 K-1 条，n=forwardEntryCount 指示有效前缀；按固定容量计算 Payload 和 T，不随 n 改变 | 已确认，关闭 |
| R03-03 | 无效区域处理 | 不清零未用 entry 和纯传输填充，接收端只按 n/有效数据范围使用；计算补齐和 Flag 单独处理 | 已确认，关闭 |

**无效区域处理（R03-03，已确认）：有效范围内解释，无效字节不要求初始化。**

`n=forwardEntryCount` 已明确有效 entries，接收端匹配 expert、读取 originalK/weight、生产 relay group 都只使用 `[0,n)`。
因此未用容量不必写 0 或 -1，复用 UB 后也不必为该区域单独清零。DIRECT 的 n=0，不解释任何 entry。

| 区域 | 已确认处理 | 读取边界 |
| --- | --- | --- |
| 未用 entries `[n,K-1)` | 不要求初始化；可随整包搬运其已有字节 | 不参与匹配、寻址或加权；若 Vector 对齐加载覆盖额外项，结果必须按 n 限定有效范围 |
| 数据段/信息段的纯传输对齐填充 | 不要求初始化，不解释内容 | hidden/scale 和 metadata 分别按自身有效范围提取；不参与任何数值计算 |
| 末个 480B Payload 块中的分块填充 | 不要求初始化；保持既定块数及传输长度 | 剥离 Flag 后跳过有效 Payload 之外的部分 |
| 固定 32B 头部、前 n 条 entry、有效 hidden/scale | 按现有协议正确写入 | 头部中已约定的 0、n、-1 等值不能因 entry 无效而省略 |
| 每个 32B DataAsFlag 信号区 | 继续按完整 blockCount 发送和检测，Flag 值满足现有协议 | n 不缩减 Flag 数；不要求每包重新初始化已经具有正确值的发送 UB Flag，接收区旧 Flag 的清理仍按 R06 |
| 量化算法会读取的计算补齐区 | 按具体量化算法初始化，不属于可忽略的传输填充 | Memory 的量化路径以 quantComputeCount_ 执行运算并准备零值补齐，不能将这类初始化一并删除 |

固定容量仍占用相同的通信字节，UB/GM 也仍需按完整 T/packedPayloadBytes 分配，并保持到 MTE3 使用完成。
不清零策略省去无效区域的初始化写入，不改变固定 slot、Flag 协议或已确认的公共副本转发规则。
后续验证只比较有效数据/字段及 Flag，不要求无效区域的字节一致；此处对 n 的使用是正常解析，不新增输入安全校验。

源码依据：[CheckDataArriveWithFlag](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L1479)只检测 Flag；
[CopyInAndOut](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L1506)按有效 hidden/scale/assist 长度写出；
[量化输入准备](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L881)与[量化运算](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L636)说明计算补齐需另行处理。

已确认机制的实现验证包括：新 Payload 长度和块数贯穿发送、全部 Flag 检测及剥离路径，尾部能够完整恢复。
验证按 R12 执行，不作为本项新的协议选择或重新确认要求。

**关联事项：** Combine member、partial 和 DIRECT 结果的 payload/scale/Flag 布局及 stride，
需结合 R08-03 的公共数值格式及 R07-01 的区域布局继续收口；Combine 内部 H 分块发布见 [C21](ep-memory-server-dedup-combine-open-topics.md#c21)，这些不属于 Dispatch 尾部字段缺失。
Flag 清理归 R06-01 至 R06-04，UB 复用归 R05-04/R05-05，不在本项重开。

**关闭结论：** 无效区域不清零、32B 固定头部、16B entry、K-1 容量、两段 32B 对齐及 DataAsFlag 均已确认，
配置级 Payload 长度和 T 已可确定，不受无效区域填充值影响。Combine 关联的公共格式由 R08/R07 约定，内部发布方式由 Combine 文档承接。
已有三类包字段语义、同一副本多次转发及 Flag 拼接/检测/剥离逻辑不再作为待定功能。
设计确认与后续编译/设备验证分别记录。

## R04

**议题：持久 Plan 布局与写入分工**

**已确定前提：** R01 已确认本卡/跨卡转发统一取号、记录和 Combine 返回，group 均以同一 forwardTokenIdx 关联。
Dispatch 产出 Combine 所需的三类持久信息：`assistEx`、`sourceMask` 和 `group`；旧 `forwardPlan` 仅作为历史名称保留，不对应独立 tensor。
已确认 assistEx 的 32B 行布局、sourceMask 的单核 UB 生成与核末写出，以及 relay group 的固定容量密排成员布局。
R04-01 已确认 Dispatch 新增 assistEx/sourceMask/group 三个 metadata 输出，Combine 对应新增三个输入；独立权重输出见 R04-08。不引入 X 输入/输出，X 记录移入超限优化遗留。
R04-02/R04-03 已确认沿用 Memory 的有效行序，以及通过第一跳 assistEx 定位固定 stride 的 group[t]；firstHopRow 保存 dstPosition。
R04-04 无需独立设计：沿用 Memory 的算子完成语义，所有核完成任务及 GM 写入后 Dispatch 才结束，后续 Combine 直接消费完整输出。
R04-05/R07-02/R07-03 已确认一块统一中转区、8*BS 个 group 容量及 `recvSlot=t*K+originalK`；group 表同步扩为 8*BS 行。
R04-07/R04-08 已确认复用 expertScales：DIRECT 保留真实 weight，partial 的 returnK 权重改为 1.0，source 按 sourceMask 统一加权。
**状态：初版设计已确认，关闭。** R04-06 的 X 记录低优先级延后；核内资源、完整区域预算及具体 ABI 分别归 R05/R07/R10。

### Dispatch 到 Combine 的输入清单

以下按 Dispatch 交给本卡 Combine 的逻辑信息编号。各维度分别标明已确认的功能、布局及生产规则，
与仍需在 R05/R07/R08/R10 确定的参数声明、资源、完整区域布局及公共数值格式；设计确认不表示已实现或已通过设备验证。
这里的“新增输出/输入”仅指跨 Dispatch/Combine 生命周期保存的 GM metadata buffer；Token 尾部 header/entries 是内部通信 Payload，
不作为额外 public output/input 参数。`assistEx` 的第一维与 Combine 实际消费的 `tokenNum` 相同，不与 group 数或原始 `BS` 混用。

| 编号/信息 | 功能定义 | 数据结构 | 生产逻辑与存放 | Combine 使用逻辑 |
| --- | --- | --- | --- | --- |
| 1. `assistEx` | 已确认：描述每条本卡 expert 输出行的原始 Token、k、角色、返回引用和 weight；作为 Dispatch 新增输出和 Combine 对应新增输入。 | 已确认：逻辑 shape 为 `[tokenNum,8]`，每行 32B、按 32B 对齐；整数按 int32、weight 按 FP32 位模式解释。首版 `tokenNum=sendCounts[L*R-1]`，且与 `expertOut` 按有效行一一对应，expert 重排须在 Combine 前还原。 | 已确认：当前 expert 行的接收 owner 复制头部到独立 metaUB，按角色转换后一次写出完整 32B，保持 UB 到完成；写入随 Dispatch 算子完成。具体核内 Event、参数声明归 R05/R10。 | 已确认：解析本行身份/角色/返回引用。group 在 relay 加权，终端返回未加权输出，DIRECT 在 source 加权；有效行中 forwardCount>0 的第一跳记录通过 t 定位 group[t]。 |
| 2. `sourceMask` | 已确认：source 最终消费的原始 k 槽位，group 保留 returnK、DIRECT 保留原始 k；作为 Dispatch 新增输出和 Combine 对应新增输入。 | 已确认：[BS]、每项独立 uint32、stride 4B，按 srcTokenId 索引，仅最低 K 位有效。 | 已确认：Token 的唯一数据发送核在 UB 维护 mask，两阶段共用，核末用 DataCopyPad 按本核有效字节写回 GM，最小 4B；每元素一个 writer，无原子/跨核合并。连续分配及尾部处理见已关闭的 R05-01，UB/Event 生命周期归 R05 其余子项。 | 已确认：只等待和消费有效 k 槽，统一乘 expertScales 后累加；DIRECT 权重为真实值，partial 权重为 1.0，无需额外结果类型标记。 |
| 3. `group` / 中转监听记录（此前归入 forwardPlan 的逻辑信息） | 已确认：关联第一跳、返回成员的位置与权重，供 relay 归约后返回 source；作为 Dispatch 新增输出和 Combine 对应新增输入。 | 已确认：group[t] 固定 stride，每行 firstHopRow=dstPosition 加固定 K-1 个 (recvSlot,weight)，前 n 项按原始 k 密排，完整行按 32B 对齐，预留 8*BS 行。有效 group 由第一跳 assistEx 的 t 标识；recvSlot=t*K+originalK。 | 已确认：第一跳 owner 保存 dstPosition，沿用 forwardEntries 下标生成 members，按 t/entry.originalK 算槽位，不新增密排原子或跨 group 前缀；写入 groupBase+t*rowStride，随 Dispatch 算子完成。参数声明归 R10。 | 已确认：通过有效第一跳 assistEx 的 t 访问 group[t]，取得 n/身份/第一跳 weight，仅消费前 n 项；本卡和跨卡返回按同一槽位公式写入；不输入 X，不盲扫未写入 group 行。 |

这三类信息的关系是：`assistEx` 解决“每条 expert 输出属于谁”，`group` 解决“同一 Token 的多条贡献如何汇聚”，
`sourceMask` 解决“source 最终读取哪些槽位”。X 不在新增输出/输入清单中，也不以 groupCount 或 group 头部计数等别名补回接口。
有效 group 的识别方式已由 R04-03 确认；已有 sendCounts/expertTokenNumsOut 语义保持不变。
Token Payload 是生成这些持久信息的临时输入，不能替代它们跨 Dispatch/Combine 生命周期保存。

### 第 1 项：AssistEx 32B 布局与写入前转换（已确认）

已确认 `assistEx` 每行使用与 Token 尾部固定头部相同的八字段布局。
此前 `[tokenNum,6]` 的必要信息已全部保留，物理行采用 `[tokenNum,8]`，复用两个转发计数字段的位置。
字段大小、offset、整数 int32 语义及 FP32 weight 已确认；offset 相对一行 `assistEx` 起点，与 Token 尾部固定头部完全一致。

| 字段 | 大小 / offset | `assistEx` 中的含义与字段来源 | Combine 消费方式 |
| --- | --- | --- | --- |
| `srcRank` | 4B / 0 | 原样保留原始输入所属 Rank | 原始 source 定位 |
| `srcTokenId` | 4B / 4 | 原样保留原始输入 Token 行号 | 原始 source 的 Token 定位 |
| `k` | 4B / 8 | 统一表示当前 expert 行的 originalK；DIRECT/第一跳取头部，终端取匹配 entry.originalK | DIRECT 返回槽位、转发成员槽位；第一跳行中也表示 returnK |
| `weight` | 4B / 12 | 当前 expert route 的 weight；DIRECT/第一跳取头部，终端取匹配 entry.weight | 第一跳权重供本 relay 读取；终端仍保留本行权重但返回未加权输出，relay 使用其本地持久保存的成员权重 |
| `forwardCount` | 4B / 16 | DIRECT 为 0，第一跳保留原始正数 n，终端为 -1；作为持久角色及初始数量，不记录发送进度 | 区分 DIRECT、第一跳贡献、需返回 relay 的终端贡献，包含本卡转发 |
| `forwardEntryCount` | 4B / 20 | 原样保留原列表有效长度；DIRECT 为 0，第一跳及终端为 n | 第一跳给出后续转发成员数；终端处理自身一行，不据此重复展开成员 |
| `relayRank` | 4B / 24 | DIRECT 为 -1；第一跳取号后填本 Rank；终端原样保留 | 第一跳关联本 relay，终端定位返回 Rank |
| `forwardTokenIdx` | 4B / 28 | DIRECT 为 -1；第一跳填首次转发前分配的 t；终端原样保留 | 第一跳与终端共同关联同一中转 Token |

**已确认物理存储：** `assistEx` 基址及每行按 32B 对齐，row stride 为 32B，总容量为 `tokenNum*32B`。
采用 `uint32` 的 `[tokenNum,8]` 存储视图；整数按 int32 语义解释，weight 保留 FP32 原始位模式，不能数值转换为整数。
与六个 4B 字段紧密存储的 24B 相比，每行增加 8B，换取与通信头部相同的布局和完整 32B 行。
新增输出及行关联已确认：`expertOut[row]` 与 `assistEx[row]` 对应，首版 `tokenNum=sendCounts[L*R-1]`，只消费有效行。
expert 内部若重排，须在交给 Combine 前还原 Dispatch 行序，不增加 public row map；具体参数声明仍归 R10，Ascend C 拷贝 API 已在文末核对，资源/Event 配置仍归 R05。

**已确认生产过程：** 由当前 expert 行的 Dispatch 接收 owner 生成。
完成 Flag 剥离、头部及所需 entries 在 UB 可读后，先将固定头部复制到独立的 32B 对齐 metaUB，再按包角色进行写出前转换：

1. DIRECT：metaUB 八字段原样保留，无须重排；两个计数为 0，返回引用为 `(-1,-1)`。
2. 第一跳：metaUB 保留接收头部的 k/weight 和正数 n；首次转发前取号一次，将本 Rank/t 补入 offset 24/28。
   同一个 t 也填入公共转发 UB；发送 UB 的 forwardCount 改为 -1，metaUB 的 forwardCount 保持 n，不能先向 GM 写未分配的编号再回填。
3. 终端：以收到的头部为基础，仅用当前 expert 匹配 entry 的 originalK/weight 替换 offset 8/12 两项，再保存 32B 行。
   这只修改接收端生成的本行元数据，不修改 relay 共用的发送 UB，也不要求 relay 按目标 expert 构造不同副本；本卡转发同样处理。

同布局不等于三类包均可逐字节原样持久化：终端公共头部中的 k/weight 仍属于第一跳，第一跳收到的返回编号尚未分配。
三类包转换完成后，统一将完整 metaUB 写到 `assistExBase + uint64(dispatchRow)*32`，每个 Dispatch 输出行仅由对应接收 owner 写一次。
字段转换对 MTE3 可见后下发，metaUB 到写出完成前保持只读，不与仍在转发的 Token UB 共用可写存储。
独立 metaUB 允许第一跳元数据写出与公共 Token 转发各自推进，不引入逐 Token PIPE_ALL；具体核内 Event 配置和参数声明仍归 R05/R10。
全部 GM 写入沿用 Dispatch 算子完成边界，不需要额外元数据发布机制。
主设计已同步[完整转换流程](ep-memory-server-dedup.md#assistex-布局与写入前转换)。

**信息边界：** 每行只保存固定 32B，不在后面隐含附带 `forwardEntries`；不能用 forwardEntryCount 从下一行继续读 entries。
两个计数字段只补充角色/数量，不能恢复实际返回成员的监听位置及权重，第 3 项中转监听信息仍需持久保存。
终端 `assistEx` 表示“返回 relay 的一条 expert 贡献”，不表示已归约、返回 source 的 partial；source 的归约权重由本卡 expertScales 提供，无需访问远端 assistEx 或增加结果类型标记。

### 第 2 项：SourceMask 的 UB 生成与核末写出（已确认）

数据发送核为其负责的每个原始 Token 在 UB 中保存一个独立 uint32 sourceMask；每个 Token 的全部 K 条 route 都属于同一核。
每个值初始化为 0，第一阶段只置各远端 group 的 returnK 位，第二阶段置各 DIRECT 的原始 k 位；两阶段使用同一个 UB 值，不重新初始化。
Group 后续 entries 不置位，源 Server 内所有 route 均为 DIRECT；置位使用 `uint32(1)<<k`，k 最大为 31。

各发送核处理完自己的两阶段发送任务后，一次批量写出本核记录的全部 mask 到 sourceMask 输出 GM：
GM shape 为 `[BS]`，元素按 srcTokenId 定位，每项 4B，每个元素仅由 Token 的唯一发送核写入一次。
无需逐 route 更新 GM、跨核合并、原子 OR 或等待全部核到齐，也不新增两阶段之间的同步/全部 MTE3 等待。
Mask UB 到对应写出完成前保持不变，Dispatch 对外完成包含该写入；每核 UB 有效容量为 `本核 Token 数*4B`。
R05-01 已确认源端 Token 在 48 核连续均分；核末使用 DataCopyPad 写回 `本核 Token 数*4B`，最小写入一个 uint32（4B）。
GM 目标起点和有效长度无需补齐到 32B；UB 保持自身对齐要求，填充不写到其他核拥有的 GM 元素。此处使用 DataCopyPad，不使用 DataCopy。
主设计已同步[生成与写出流程](ep-memory-server-dedup.md#sourcemask-生成与写出)，包含连续分配及 DataCopyPad 最小 4B 写回。

该确认解决 sourceMask 的数据结构、生成核和存放时机；它表达有效槽位，实际到达仍由这些槽位的 Flag 判断。
结合已确认的 expertScales 权重转换，source 无需另行区分 DIRECT/partial 类型。

### 复用 ExpertScales 的 source 归约权重（R04-07/R04-08，已确认）

Dispatch 权重输入和输出分离：expertScales 是非空只读 FP32 `[BS,K]` 输入，expertScalesOut 是同形状的独立输出。
调用方将 expertScalesOut 传入 Combine 已有的 expertScales 参数，沿用原始 Token/k 索引；不新增 Combine 权重参数。
三项 metadata 为 assistEx/sourceMask/group，独立权重输出另列，public/launch ABI 的具体实现归 R10-01。

| 维度 | 已确认契约 |
| --- | --- |
| 功能定义 | 输入为真实 route weight，输出为 source 最终归约权重；relay 使用真实权重完成本 Server 内加权求和 |
| 数据结构 | 两个不重叠的 FP32 `[BS,K]` buffer，每个共 `BS*K*4B`；输出 DIRECT 为真实值，returnK 为 1.0，sourceMask 为 0 的位置不消费、无需赋值或清零 |
| 生产逻辑与存放 | 原始 Token 的唯一发送核读取 expertScales 构造通信包，在本核 UB 生成 expertScalesOut，核末写到独立输出的本核 Token 区间 |
| Combine 使用逻辑 | 将 Dispatch expertScalesOut 作为 Combine expertScales 输入，只对 sourceMask 有效 k 统一加权；relay 第一跳/成员取 assistEx/group 的真实权重 |

**生产与生命周期：**

1. 两遍发送都从只读输入 expertScales 取真实权重，第一跳 header、DIRECT header 和有效 entries 使用各自真实值。
2. 在输出 UB 中生成 DIRECT 真实值和 returnK 的 1.0，不修改输入；输出构造不依赖输入全部读取结束。
3. 源端核末写出 sourceMask 和 expertScalesOut。发送 UB 与权重输出 UB 分别保持到对应 MTE3 完成；按 R05-04 每核 DataCopyPad 有效区间，并用即时 S_MTE3/MTE3_S 发布和完成写出，不增加两遍之间的屏障或全部 MTE3 等待。
4. Dispatch 算子完成包含输出 GM 写入。expertScalesOut 保持到 Combine 消费完成；输入只需遵守 Dispatch 输入生命周期，可继续复用其真实权重。

是否支持同一份 metadata 顺序多次 Combine 仍归 R11-02；若支持，复用同一份 expertScalesOut 及真实成员权重。
首版不支持 expertScales=nullptr；R08-03 已确认 BF16/FP16 非量化及 MXFP8 Dispatch，expertOut/返回公共 dtype 继续讨论。

**例子：** R0/T0 的 k0 在源 Server 内 DIRECT，weight=0.4；k1 在 R8 为第一跳，weight=0.3；
k2 转发到 R9，weight=0.2；k3 在另一 Server 的 R17 DIRECT，weight=0.1。

| 位置 | Dispatch 输入真实权重 | Dispatch 后 sourceMask 位 | Dispatch 输出 expertScalesOut | 返回内容 |
| --- | --- | --- | --- | --- |
| k0 | 0.4 | 1 | 0.4 | 未加权 y0 |
| k1 / returnK | 0.3 | 1 | 1.0 | R8 的 partial=0.3*y1+0.2*y2 |
| k2 | 0.2 | 0 | 不消费，可保留 0.2 | y2 仅返回 R8，source 不等待本槽 |
| k3 | 0.1 | 1 | 0.1 | 未加权 y3 |

R8 第一跳 assistEx 保存 0.3，group member 保存 0.2；sourceMask=0b1011，
source 统一计算 `0.4*y0 + 1.0*partial + 0.1*y3`。DIRECT 的真实 weight 即使等于 1.0 或 0，也保持其 route 和等待职责；不以 weight 判断有效性或结果类型。

**三个不同的数量：**

| 数量 | 已有信息来源 | 本例 |
| --- | --- | --- |
| Source 每个原始 Token 等待的有效贡献数 | `popcount(sourceMask[token])`；位集合直接给出需等待的 k，不新增 recvCount | T0 等待 k0/k1/k3，共 3 份 |
| Relay 单个 group 等待的转发返回数 | 第一跳 `forwardEntryCount=n`；另有本地第一跳输出，参与归约的 expert 贡献总数为 n+1 | R8 等待 1 份返回，加本地第一跳共 2 份 |
| 本卡交给 expert/Combine 的有效输出行数 | `tokenNum=sendCounts[L*R-1]`，沿用 sendCountsOut -> sendCounts；不是上述逐原始 Token 的返回数 | 由本卡收到的所有 expert routes 共同决定 |

普通 Memory 的全 K 等待/归约循环必须改为 sourceMask 指定的消费范围；复用接口不意味着照搬全 K 的等待条件。

### 第 3 项：固定容量、行内密排的 Relay Group（已确认）

已确认每组固定预留 K-1 个 member，实际前 n 项密排，成员仅保存 `(recvSlot,weight)`。
RecvSlot 是本 relay 中转结果区的相对槽位编号，位置换算在 Dispatch 完成；Combine 只需监听该位置，不单独读取 senderRank。
第一跳通过 firstHopRow 关联本卡输出及 assistEx，从中取得 srcRank/srcTokenId、returnK、第一跳 weight 和 n，不占 member 位置。
安全用例保证本 relay 的实际 group 数不超过 8*BS，初版不做超限校验。
取消 X 接口后，原先“读取 X 后遍历 [0,X)”不再是消费契约；按 R04-03 已确认规则从有效第一跳 assistEx 取得 t，访问 group[t]，不直接读取 8*BS 行中的未写入记录。

| 信息 | 逻辑 shape / 大小 | 行内位置与消费方式 |
| --- | --- | --- |
| `firstHopRow` | `[8*BS]` 个 int32 行引用 | 每行 offset 0，占 4B，保存第一跳的 dstPosition；是行内字段，不另要求一份连续数组 |
| `members[i].recvSlot` | 成员 shape `[8*BS,K-1,2]` 中的第一个 32 位 word | 行内 offset `4+8*i`，uint32 相对槽位编号 |
| `members[i].weight` | 成员 shape `[8*BS,K-1,2]` 中的第二个 32 位 word | 行内 offset `8+8*i`，FP32 原始位模式 |

`n=assistEx[firstHopRow[t]].forwardEntryCount`。各 group 使用固定 stride，不跨 group 压缩空间；完整行的 32B 对齐布局为：

```text
logicalWordsPerRow = 1 + 2 * (K-1)
rowStrideBytes = align32(4 * logicalWordsPerRow)
physicalShape = uint32[8*BS, rowStrideBytes/4]
```

BS=256、K=6 时每行有效 44B、对齐后 64B，预留 2048 行，物理 shape 为 `uint32[2048,16]`，共 128KiB。
K=8/16/32 时每行对齐后分别为 64/128/256B。该元数据表不含 H 维度，不额外乘发送卡数。
每组只解析 `[0,n)`，不扫描无效尾部、不按原始 k 留洞，也不要求填写 `{-1,-1}` 哨兵。

**已确认生产逻辑：** Source 按原始 k 升序生成 forwardEntries；relay 的第一跳 owner 沿用已有 entry 下标 i，
用 `recvSlot=uint64(t)*K+entry.originalK` 计算相对槽位，将 `(recvSlot,weight)` 写入 members[i]；不再为返回寻址计算 senderRank。
同一 owner 保存 `group[t].firstHopRow=dstPosition`，完整行写入 `groupBase+uint64(t)*rowStrideBytes`。
不额外分配密排编号、不增加原子计数或跨 group 前缀，也不在 relay 排序。
在独立元数据 UB 中构造完整行后写出持久 GM，元数据 UB 保持到写出完成；原 Token UB 按其 copy/转发依赖回收。

**已确认消费逻辑：** Combine 通过第一跳 assistEx 取得 n/身份/第一跳权重，先处理第一跳，再按 member 顺序监听并加权归约前 n 项。
条目保持原始 k 升序，不另保存 originalK 或 memberMask；每个 recvSlot 都必须与返回端实际写入的位置一致，包含本卡转发。
数据和 Flag 位置从本轮结果区基址、recvSlot 及 slot stride 恢复，不在表中保存跨调用绝对 GM 地址。

例如 K=6、R0/T8 经 R8、t=7，五条有效 member 为 `(43,w1),(44,w2),(45,w3),(46,w4),(47,w5)`，
分别对应 `(R8,k1)、(R9,k2)、(R9,k3)、(R9,k4)、(R10,k5)`；第一跳 w0 从 firstHopRow[7] 对应的 assistEx 取得，槽位 42 不使用、不等待。
终端 Combine 从 assistEx 取得 relayRank/t/k，按同一公式写入目标 relay；本卡返回也执行写入和监听，不新增编号或包字段。
成员表前 n 项密排，结果区按原始 k 预留 K 槽，两者不混用；n=1 时仍预留 K 槽，只监听一个返回。
recvSlot 与本卡返回寻址已确认；参数声明归 R10，当前非量化结果区 base/offset/stride 已由 R07-01 收口，其他 dtype/scale 扩展归 R08-03。
主设计已同步[成员表布局与生产/消费流程](ep-memory-server-dedup.md#relay-group-成员表)。

### X 接口移出初版（已确认）

原第 4 项 X 不再作为 Dispatch 输出或 Combine 输入，不生成独立持久 X 记录，不为其增加发布流程。
X 的记录和使用仅作为 R04-06/R07-04 后续超限场景优化的低优先级遗留，既有“X 输出建议”撤回。
这不取消已确认的内部 AtomicAdd 取号：每个 relay group 仍取得唯一 forwardTokenIdx，内部计数器只承担编号分配及本轮初始化职责，API 核对归 R05-03。
安全用例仍保证实际 group 数不超过 8*BS，不比较、校验、报错或降级，也不加入超限用例。

**无 X 的衔接（R04-03，已确认）：** Combine 在有效 `[0,tokenNum)` 行 assistEx 中识别 `forwardCount>0` 的第一跳记录，
从该行的 forwardTokenIdx 取得 t，读取 group[t]，其中 `group[t].firstHopRow=row=dstPosition`。
每个实际 group 恰有一条已确认的第一跳 assistEx，因此可以不依赖单独的 group 总数输入；forwardCount=-1 的终端行不重复产生 group。
不新增 group 有效位/哨兵、不初始化整张 group 表；只规定可供消费的公共识别依据，Combine 如何分核筛选归其内部议题。

### GM 写入与算子完成（R04-04，无需独立设计，关闭）

沿用 Memory 的算子完成语义：Dispatch 所有核完成全部任务及 GM 写入后算子才结束，
因此后续 Combine 开始消费时，assistEx、sourceMask、group 及 expertScales 的 source 归约权重已完整写入 GM。
不另设 metadata 发布协议、逐表/逐 group ready、额外同步或新的完成信号。
这里的完成指设备侧 Dispatch 算子完成，Host 异步提交返回仍按 Memory 既有语义处理。
核内 UB 保持到最后使用者完成等资源依赖仍归 R05，不构成新的跨算子发布议题。

**具体场景：** R0/T8 命中 R8 的 expert25/26。第一跳由核 A 处理，expert26 的接收槽位可能由核 B 监听。
两核需要关联同一 Token，但不能各自分配编号或竞争覆盖同一份成员记录。
核 B 消费第一跳 owner 已分配并随包携带的编号，按普通转发 Token 保存元数据，供 expert26 的 Combine 返回使用。

**子项状态：**

- **R04-01：新增 metadata 输出与对应输入，已确认并关闭。** Dispatch 新增 assistEx、sourceMask、group 三个 metadata 输出，Combine 对应新增三个输入；独立权重输出见 R04-08，不引入 X。具体函数声明和参数顺序归 R10-01，行关联及算子完成语义见以下已关闭子项。
- **R04-02：Dispatch 与 ExpertOut 行关联，已确认并关闭。** 沿用 Memory 行序及 `tokenNum=sendCounts[L*R-1]`，expertOut[row] 与 assistEx[row] 一一对应；内部重排须在 Combine 前还原，不增加 public row map，不读取 padding 行。关联 R10-02 同步关闭。
- **R04-03：Relay Group 存放与第一跳引用，已确认并关闭。** group[t] 固定 stride，firstHopRow 保存 dstPosition；有效 assistEx 行中 forwardCount>0 的第一跳通过 t 定位 group[t]。不输入 X，不读取未写入 group 行，不新增 groupCount/validMask/哨兵或整表初始化。
- **R04-04：完整元数据发布，无需独立设计，关闭。** 沿用 Memory，所有核完成任务及 GM 写入后 Dispatch 算子才结束，后续 Combine 直接消费完整输出；不增加 metadata 发布协议、ready Flag 或额外同步。核内 UB/Event 复用归 R05，资源最终释放归 R11，均不重开本项。
- **R04-05：RecvSlot 与返回地址一致，已确认并关闭。** Dispatch 按 `recvSlot=uint64(t)*K+entry.originalK` 填写 member；终端 Combine 从 assistEx 取得 relayRank/t/k，按同一公式返回。本卡/跨卡共用一块预留 8*BS 个 group、每组 K 槽的区域，不增加来源卡维度、新编号或包字段。关联 R07-02/R07-03 同步关闭。
- **R04-06：X 记录，低优先级遗留。** 初版不引入 X 输入/输出或持久记录；是否及如何记录 X 留待后续优化超限场景时与 R07-04 一起讨论。此项不作为本轮关闭条件，内部 AtomicAdd 取号仍归 R05-03。
- **R04-07：Source 统一加权，已确认并关闭。** DIRECT 权重保留真实值，group 的 returnK 权重设为 1.0；source 按 sourceMask 对有效结果统一乘 expertScales 累加，无需 partialMask 或其他结果类型标记。relay 第一跳和成员权重仍为真实值。
- **R04-08：权重输入输出分离，已确认。** Dispatch 使用非空只读 FP32[BS,K] expertScales 输入及独立 expertScalesOut 输出；唯一 Token 发送核构造真实随包权重和 source 归约权重，核末写出输出。Combine 的 expertScales 参数接收该输出，保持到消费完成。输入不被覆盖，具体 ABI 归 R10-01。

**后续衔接：** 沿用已确认的逐行 writer、字段转换、sourceMask 核内生成、有效行序、group 识别、recvSlot 编码及 expertScales 复用，落实 R05/R07/R08/R10 的资源、区域、公共格式和具体声明。
Plan 只保存 Combine 所需的稳定信息，不复制整个通信包，也不指向可回收 UB/descriptor。
字段精简须保留从任一 expert 行到其 source/group 的完整路径。

**关闭状态：** R04 的 metadata 与权重输入输出分离已确认；独立权重输出 ABI 已在 R10-01 收口，待代码接入，无额外 source 类型标记；X 记录和超限优化低优先级延后。
权重复用结论已同步主设计；已确认设计不等于实现或设备验证完成。

## R05

**议题：Dispatch 分核、原子与 UB/Event 资源**

**已确认边界：** 默认总 AIV 核数为 64，固定分为 48 核源端数据发送及 16 核 cnt 接收/cumsum/localwindowcopy/转发。
16 核先完成 cumsum，再按最终 `tokenNum` 均分监听任务；中转包的检测与数据转发嵌入 localwindowcopy，复用同一组核。
源端连续均分及 DataCopyPad 精确写回已确认，R05-01 关闭。公共转发包、一次原子取号和下发后剔除均已确定。
R05-02 的相对槽位编码和双表仅存 UB、R05-03 的取号及核末同步清零已确认。
R05-04 源端 1 至 8 路环形发送、具体 ID 和权重写回均已收口；R05-05/R05-06 数据转发的资源预算与在途模型后续另行设计。

### R05-01 核组、分配与写回（已确认，关闭）

以下核数为去重 Dispatch 初版默认配置，核组职责在本轮调用期间固定。
其中“数据发送 48 核”负责原始 source 的第一跳/DIRECT 发送；接收侧的转发 MTE3 由另外 16 核执行。

| 核组 | 固定核数 | BS<48 | BS>=48（包含 BS=48） |
| --- | --- | --- | --- |
| 源端数据发送 | 48 | 先完成本核分配到的 cnt 发送，再按 R02 先发第一跳、后发 DIRECT | 不承担 cnt 发送，直接按 R02 处理本核两类数据发送任务 |
| cnt 接收/cumsum/localwindowcopy/转发 | 16 | 参与全 64 核的 cnt 发送分工；之后接收 cnt、计算 cumsum，再均分监听任务并执行 copy/转发 | 在本组 16 核内分配全部 cnt 发送任务；之后接收 cnt、计算 cumsum，再均分监听任务并执行 copy/转发 |

**任务归属与顺序：**

- Cnt 按目标 expert 范围分配，仍由原始 source 统计所有原始 route 并发送一次；BS<48 时在 64 核分配，BS>=48 时在 16 核分配。
  Cnt 任务与源端 Token 任务独立；小 BS 下即使某个数据核没有源端 Token，也须处理其可能分配到的 cnt，不能按 Token 数提前退出。
- 小 BS 的“先 cnt 后数据”是每个数据核完成自身 cnt 发送流程后继续自身任务，不新增等待全部 64 核 cnt 发送完成的阶段屏障。
  Cnt buffer/Event 按既有资源依赖回收；R02 的第一跳/DIRECT 之间仍无 PIPE_ALL、SyncAll 或单独 MTE3 等待，旧双 Group 条带/双轮发送不恢复。
- 只有固定 16 核负责 cnt 接收与 cumsum；全核发 cnt 不意味着全核算 cumsum 或全核接收 Token。
  Cumsum 结果就绪后，按最终 tokenNum 为这 16 核分配监听位置，负载差值至多 1；监听到第一跳中转包时，在同一 localwindowcopy 流程内处理取号及后续本卡/跨卡转发。
- Localwindowcopy 的监听与数据转发按已确认的解耦目标设计：监听使用 MTE2/Vector/Scalar，数据转发使用可用的 MTE3 资源。
  同组核执行不意味着逐 Token 串行等待转发完成，具体数据转发 UB/Event 池与在途进度继续由 R05-05/R05-06 单独设计。

**已确认的源端分配：** BS 在 48 核中采用 Memory `SplitToCore` 的连续区间和余数均分，余数优先分配给组内序号较小的核。
每个 Token 的全部 K 条 route 及两阶段始终归同一 owner。设发送核的组内序号为 i，范围 `[0,48)`：

```text
base = floor(BS / 48)
remainder = BS % 48
count_i = base + (i < remainder ? 1 : 0)
start_i = i * base + min(i, remainder)
Token 范围 = [start_i, start_i + count_i)
```

例如 BS=256 时，前 16 个组内核各处理 6 个 Token，其余 32 核各处理 5 个。
接收端 tokenNum=2048 时，16 核各监听 128 个位置；双表按已确认的 R05-02 保存在各核 UB。

**已确认的核末写回：** sourceMask 由源端 owner 在 UB 生成；使用 DataCopyPad 将本核 `count_i*4B` 有效数据写至 GM 的 `sourceMask[start_i]`。
本方案最小写回粒度为一个 uint32（4B），GM 目标起点及有效长度不受 32B 对齐约束；UB 按自身要求对齐分配，GM 写回不包含 UB 填充。
例如 count_i=5 时只写 20B，count_i=1 时只写 4B；此处明确使用 DataCopyPad，不使用 DataCopy，也不另增尾部拼接、邻核合并或 32B read-modify-write。
count_i=0 的核跳过 sourceMask 写回，但仍按前述规则完成其 cnt 职责；非空写回的 UB 保持到 MTE3 完成，算子结束覆盖该写入。
源端分配和非整块尾部写回均已确认，不再作为 R05-01 的遗留项。

**Memory 复用核对：** 当前 [BS 阈值与核数上限](../../src/ep/common/ep_memory_window.h#L15)分别为 48 和 8；
[Host 核数推导](../../src/ep/host/ep_memory_layout.cpp#L131)与 [kernel 核数推导](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L518)均使用旧公式。
去重版保留 cnt/q 的数值算法和 BS 分界，将本组核数固定为 16；相应 cumsum 状态/Flag 布局、分区索引和 UB 尺寸需要按 16 核统一核对，不能只改 launch 总核数后继续受旧 8 核上限截断。
这些属于新配置的实现适配与 R07 区域预算核对，不重新讨论 cnt/q 算法，也不改变原 Memory 入口。

总专家数不超过 1024 后，`rscvStatusNum=L*R<=1024`，16 核接收 cnt 时每核至多 64 条记录。
每卡 L 的上界为 `floor(1024/R)`，16P 下可以达到 64；复用代码不能继续以固定 L<=32 限制或分配逐 expert 资源。
该 cnt 上界不约束 tokenNum，监听 UB 双表仍按 R05-02 的实际接收量核算。

### R05-02 相对槽位编码与 UB 双表（已确认，关闭）

每个接收核用 `tokenPos[j]` 记录本核第 j 个待监听 Token 的通信槽位位置，元素类型为 uint32，单位是完整 Token 通信槽位。
`base` 为本卡本轮 Dispatch 接收区第 0 个 Token 通信槽位的起始地址，固定指向数据槽位区，不以最先到达的 Token 作为基准。
各表项保存相对该首槽的槽位偏移；不保存字节偏移、绝对 GM 地址、srcTokenId 或密排 expert 输出行号。

```text
slotAddress(j) = base + uint64(tokenPos[j]) * T
```

T 沿用 R03 确定的完整通信跨度，包含 Token Payload（数据及尾部信息）、DataAsFlag 的 Flag 和填充；不使用纯 hidden 长度。
乘法前先扩为 uint64，再加 base，避免在 32 位中间结果中计算字节偏移。
表的条目数取决于本核任务量，条目值对应的槽位偏移可跨越接收区中未使用的槽位，两者不混用。

**表的生成与使用：** 16 个接收核完成 cumsum、取得各自的有效 Token 区间后，按已确认的 source/expert/q 通信布局枚举对应槽位，在本核 UB 生成以下两张表：

| 表 | 类型与有效长度 | 初始化与生命周期 |
| --- | --- | --- |
| tokenPos | uint32[m]，m 为本核负责的 Token 数 | 保存每个监听位置的相对槽位偏移；生成后保持原表，供监听、数据读取和 Flag 清理使用 |
| pendingIdx | uint32[m]，初始 pendingCount=m | 初始填写 0 至 m-1；用 tokenPos[pendingIdx[i]] 定位任务，全部 copy/转发任务下发后以末项覆盖删除，并将 pendingCount 减一 |

两张表均只存 UB，不输出到 GM，也不采用 GM 存表、溢出转存或分批加载方案。
删除 pendingIdx 不修改 tokenPos 原表，后者保留到本核负责区域的 Flag 清理完成；具体清理时机继续按 R06 讨论。
pendingIdx 剔除不表示对应 MTE3 已完成，公共转发 UB 的最后使用者及回收仍归 R05-05。

**容量口径：** 均衡情况下每卡最终接收 `BS*K` 个 expert Token，16 核均分后，每核约 `BS*K/16` 项。
不均衡时使用该卡实际的 tokenNum 计算，每核 m 最大为 `ceil(tokenNum/16)`；两张表分别按有效长度占 `4*m` 字节，UB 分配各自按 32B 对齐。
当前预计每核约 1K 项以内；1K 按 1024 项计算，对应单表最多约 4KiB，双表合计约 8KiB，不能把单表占用当作全部监听表占用。

| 场景 | 本卡 tokenNum | 每核 m | tokenPos / 核 | pendingIdx / 核 | 双表合计 / 核 |
| --- | --- | --- | --- | --- | --- |
| BS=256，K=6，均衡 | 1536 | 96 | 384B | 384B | 768B |
| BS=256，K=16，均衡 | 4096 | 256 | 1KiB | 1KiB | 2KiB |
| BS=256，K=32，均衡 | 8192 | 512 | 2KiB | 2KiB | 4KiB |
| BS=256，K=32，本卡接收量为均衡值两倍 | 16384 | 1024 | 4KiB | 4KiB | 8KiB |

撤回此前用全局 route 集中到单卡的理论极值推导 GM 分批方案的建议；本阶段按目标 BS/K 和接收不均衡度核算 UB。
约 1024 项是当前规模估算，不新增固定 1024 项的接口限制，也不将其视为所有配置和任意不均衡下的容量保证。
双表与 Flag scratch、转发 buffer、metaUB/group 行 UB 的总预算继续归 R05-05，支持的 BS/H 等配置范围归 R10；存放位置和编码不重新讨论。

### R05-03 原子取号与计数器清零（方案已确认）

采用 CANN 9.1 的 Scalar `AscendC::AtomicAdd`，头文件为 `basic_api/kernel_operator_atomic_intf.h`。
官网原型为 `template <typename T> __aicore__ inline T AtomicAdd(__gm__ T *address, T value)`；
address、value 和返回值类型相同，支持 uint32_t。返回修改前的旧值，并将旧值加 value 写回 GM。

```cpp
// 前提：本 Rank 本轮共享 counter 已初始化为 0，初始化完成且满足缓存/同步要求。
// 当前第一跳 owner 仅首次取号；恢复部分发送时复用保存的 t。
uint32_t t = AscendC::AtomicAdd(counterGm, uint32_t{1});
```

因此第一个 group 得到 0，第二个得到 1；并发取得的 t 唯一，但不保证与 Token 输入顺序一致。
不减一、不在加一后普通读取 counter；该内部取号不引入 X 输出或超限校验。

官网产品表明确支持 **Ascend 950PR/950DT**，不支持 Atlas A2/A3 等列出的其他系列；本结论限于本设计的 A5/950 目标，不能据此扩展到整个 TileXR 支持范围。
AtomicAdd 绕过 DCache，且与 MTE2/MTE3 存在 GM 数据依赖时编译器不会自动补齐同步；必须手动处理相关依赖和缓存一致性。
初始化写入须在所有取号前完成，清理旧脏缓存须避免覆盖已开始的原子更新，不能把“原子”解释成对所有流水和缓存的通用屏障。
DataCacheCleanAndInvalid 的接口和 64B cache line 规则见文末；该 64B 规则不是 AtomicAdd 的地址对齐要求。

**已确认的存放位置：** 在本 Rank 的 cnt/state 通信区划出一块 1KB 专用预留区，共预留 1KB。
仅使用起点处的一个 uint32（4B）作为全卡共享 counter，剩余 1020B 不使用、不清零；不会为每个核分别建立计数器。
具体 offset 已在 R07-01 的完整区域布局中给出，不与 cnt、cumsum、DFX 等数据重叠。
预留区按 1KB 边界放置以隔离相邻数据，这是内部布局选择，不是官网规定的 AtomicAdd 对齐要求。

| 执行阶段 | counter 使用 |
| --- | --- |
| 算子执行期间 | 所有需要创建 relay group 的核对同一首 4B 执行 AtomicAdd(counterGm, 1)，返回值直接作为 t |
| 算子末尾同步 | 全部 64 个逻辑核完成自身任务后，执行一次全核 SyncAll()；同步返回后由核 63 将同一首 4B 写零 |
| 算子退出后 | 计数器保持为 0，下一次 Dispatch 继续使用同一 1KB 区域 |

核 63 是本 Rank 的逻辑 AIV 核号，仍承担原有职责；即使本核没有 Token 任务，也必须参与算子末尾 SyncAll()，并在同步返回后执行清零。
清零可沿用已确认的 DataCopyPad：从 32B 对齐的零值 UB 精确写出 4B，不清整个 1KB，也不在其他核仍可能取号时写 counter。
核 63 保持零值 UB 到这笔 MTE3 写出完成，退出前确认首 4B 清零；Dispatch 算子完成覆盖该写入。
本轮不在清零后再增加第二次全核同步；下一次 Dispatch 使用该 counter 前，须满足既有算子调用顺序和 stream 完成边界。
Combine 从 assistEx/group 取得已保存的 t，不读取 counter；counter 清零与旧 group/通信槽位能否覆盖分别判断，持久数据寿命仍归 R11。
取号核只用 AtomicAdd 的返回值保存 t，不通过普通 GM 读写参与累加，也不在原子更新期间对该区写回旧脏缓存。

**首次使用：** 当前 [TileXRComm::InitMem](../../src/comm/tilexr_comm.cpp#L1204) 在 peer memory 分配后已执行零初始化；专用预留区不被其他用途覆盖时，可沿用该初值启动轮次 0，不新增每轮 Host 清零。
这是已有分配初始化的复用，不要求 Dispatch 清理未使用的 1020B。R07-01 的 counterOffset 首 4B 沿用通信区首次初始化的零值。

**已确认与剩余边界：** API、1KB 预留、首 4B 使用范围、算子末尾 SyncAll()、核 63 清零和清零完成边界已确定，不再作为候选选择。
AtomicAdd 正文未明确额外地址对齐数值的事实仅保留为官网证据边界；具体预留区 offset 归 R07-01，算子结束后的窗口/数据生命周期归 R11-05。

### R05-04 本卡数据发送流水（Memory 复核已收口）

“数据发送”仅指固定 48 核从本卡原始输入下发第一跳或 DIRECT；不包含目标卡接收后执行的数据转发。
已同步主设计的[本卡数据发送流水](ep-memory-server-dedup.md#本卡数据发送流水)，完整 UB 分配及 Set/Wait 时序以该节为准。
**单包顺序已确认：** CopyIn → 可选量化 → 在数据 Tensor 尾部填信息 → DataAsFlag 打包 → MTE3 发送，直接沿用 Memory。
非量化尾部写在 xInTensor，量化尾部写在 quantTensor_；随后通过原有 Vector Copy 打包到 outBuf，不另增拼尾部 Tensor 或整包中间拷贝。
该流水的变化是尾部字段更多、预留长度扩大，相关 Payload/分块数/Tensor 容量/T 随之重算；发送 Event 配对保持原方案。
Flag 位置由发送池初始化准备，Vector 按 480B 数据 / 32B Flag 间隔打包；“拼接 Flag”不表示另加一套发布操作。
接收端监听、localwindowcopy、assistEx/group 的 metaUB 和转发副本属于另 16 核的数据转发设计，继续由 R05-05/R05-06 讨论。

| 已确认内容 | 当前结论 |
| --- | --- |
| 单 Token 尺寸 | `dedupTokenStride=T=ceil((align32(D)+align32(32+16*(K-1)))/480)*512`，同配置的第一跳和 DIRECT 使用相同跨度 |
| 并发数 | `min(floor(remainUbSize/T),8)`；上限 8，随 UB 缩减，安全输入保证最小为 1，不增加 0 并发校验或错误分支 |
| UB 申请 | 沿用 Memory 输入队列及连续 outBuf；非量化 xInTensor、量化结果 quantTensor_ 按新 packedPayloadBytes 扩容，量化原始输入仍按 hAlignSize_；先计入输入/量化/路由/输出 UB，再按剩余容量申请 `[sendTokenBufNum,T]` |
| UB 复用 | 本核源端包序号 s 选择 `s%sendTokenBufNum`；使用前消费该槽位 MTE3_V，Vector 拼包后配对 V_MTE3，MTE3 下发后置 MTE3_V，环回时才等待复用 |
| 事件边界 | MTE3_V 保护 Vector 改写 outBuf；Scalar 在 xInTensor 或 quantTensor_ 尾部改字段并建立 S_V，沿用 Memory 的 Tensor 组织与依赖 |
| 两阶段与核末 | 第一跳优先、DIRECT 随后，保持同一池和序号，无阶段屏障或单独 MTE3 等待；核末消费末次事件后收敛写出，全部核再参与 counter 清零同步 |

Memory 源码依据为 [CalcSendTokenBufNum](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L861)、
[TokenToExpert/量化路径](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L655)及
[SendToMoeExpert](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L793)。本版本不使用双 Group，不带入其条带或轮次机制。

**本轮 Memory 复核结论：没有发现需要新方案的 UB/Event 冲突。** 先前把常规输出写回保留为待确认，范围过宽；按以下规则收口。

| 范围 | 直接采用的结论 |
| --- | --- |
| 环形发送事件 | MTE3_V/V_MTE3 均以 lane 为固定显式 ID；尾部先经 Scalar→Vector 拼包，不改变 outBuf 的写入流水，无新增 Event 池 |
| 权重输入 UB | 参考 Memory Combine ExpertScaleCopy，仅加载本核连续 Token 区间，align32(count_i*K*4)；MTE2_S 后两遍只读 |
| 权重/分类输出 UB | 独立 sourceWeightOut 为 align32(count_i*K*4)，sourceMask/directMask 各 align32(count_i*4)，serverRouteMask 为 64B；在计算 outBuf 剩余容量前计入 |
| 核末写出 | Scalar 产出的 sourceMask 和 expertScalesOut，经即时 S_MTE3 Set/Wait 后分别 DataCopyPad 有效 count_i*4B 和 count_i*K*4B；一次核末 MTE3_S 完成写出再释放 UB |
| 依赖边界 | 输出的 S_MTE3/MTE3_S 与发送 lane 的 V_MTE3/MTE3_V 不同类；即时配对不占用新的在途 lane，不增加两遍间屏障或单独 MTE3 等待 |

完整分配与核末顺序已写入主设计「源端输出 UB 与核末写出」。尚未实现或未做设备验证，不等于还存在待用户选择的设计。

源端按 Memory 环形 Set/Wait 复用即可，不要求非阻塞 MTE3 完成查询，也不把避免所有资源等待设为额外条件。
此前提出查询是为了探索数据转发中“监听与 MTE3 在途重叠”时的资源回收；官网未查到该候选接口的事实保留，
但它既不阻塞本卡数据发送方案，也不是数据转发后续设计的必选 API。

**需要确认：**

- **R05-01：已确认并关闭。** 默认 64 核、固定 48 核源端发送和 16 核 cnt 接收/cumsum/localwindowcopy/转发；BS<48 时 64 核发 cnt，数据核先完成本核 cnt 再发数据，BS>=48 时仅 16 核发 cnt。源端 Token 连续均分、余数优先分配；sourceMask 核末使用 DataCopyPad 按有效字节写回 GM，最小 4B，无需 GM 起点/长度 32B 对齐。原核数、任务分配、cnt 时序及非整块尾部问题全部关闭。
- **R05-02：已确认并关闭。** tokenPos 为 uint32 相对通信槽位偏移，按 base+uint64(tokenPos[j])*T 定位；tokenPos/pendingIdx 均仅存 UB，保留原位置表和末项覆盖删除规则。按 BS*K、接收不均衡度及 16 核分摊核算条目数，1024 项时单表 4KiB、双表 8KiB；不采用 GM 存表或分批加载，具体数据转发 UB 预算归 R05-05。
- **R05-03：取号及 counter 清零方案已确认。** uint32_t AtomicAdd 的旧值作为 t；保留一份 1KB counter 区，仅首 4B 使用，算子末尾全核 SyncAll() 后由核 63 清零，未用区域不清零。首次零值沿用 peer memory 初始化；具体 offset 归 R07-01，算子间生命周期归 R11-05。
- **R05-04：源端设计已收口。** 按去重 T 计算 1 至 8 路，直接沿用 lane 的 MTE3_V/V_MTE3；独立权重/分类 UB、核末 S_MTE3/DataCopyPad/MTE3_S 的写出安排已明确，无剩余设计选择，待实现及设备验证。
- **R05-05（数据转发后续）：** 转发任务的 next-entry、已分配编号和公共副本在哪里保存；最后一个 MTE3 使用者如何释放共享 UB？这些问题不从本卡源端数据发送流水推导。
- **R05-06（数据转发后续）：** Token 尚未到达或转发资源暂忙时，如何继续其他 Token 并保证本卡转发所依赖的接收任务可推进？不增加目标端 ready/credit；本项不属于源端数据发送的并发确认。

**后续讨论边界：** 源端数据发送按主设计的环形 Set/Wait 及核末写出执行，具体 ID 和权重写回不再列为待确认。
相对槽位编码已由 R05-02 确认；relay 的转发 buffer、元数据 UB 和在途进度仍需另行设计，不从源端流水推导。

**实现核对：** 当前 ReduceGrad 的 `AscendC::AtomicAdd(status_,0)` 只提供源码用例；加一取号的返回值依据现已补齐为 CANN 9.1 官网正文。
后续实现仍需验证实际 SDK/SoC 编译、counter 生命周期和并发唯一性；这些不影响已确认的 API 语义。
参考依据及未覆盖范围见文末，不把 SetAtomicAdd 的累加写入等同于返回旧值的原子取号。

**剩余关闭条件：** 数据转发的资源尺寸、部分转发状态与公共 UB 最后使用者判定；
目标 API 核对与设备重叠/前进性验证分别记录，未验证性能不作为已确认收益。

## R06

**议题：Flag 清理时机与顺序**

**仍待选择：** 原先“核末统一清理”和“Token/批次发送后用 PIPE_MTE3 排序清理”都保留，
尚未选定执行粒度。PIPE_MTE3 的前后序语义已确认，不重新作为选择题。

**具体场景：** 同一核已下发 Token A 的多笔转发，Token B 仍在监听；
此时清理 A 的接收 Flag 不应等待 B 到达，也不能清掉转发到本卡后仍待消费的目标槽位。

**需要确认：**

- **R06-01：** 采用核末集中清理，还是逐 Token/小批次清理；清理任务是否在剔除 pendingIdx 前一起下发？
- **R06-02：API 语义已核对，组织方式待确认。** PIPE_MTE3 保证本核前序 MTE3 完成后才执行后续 MTE3，PIPE_ALL 收敛本核已提交流水；核末/随发送方案如何组织零值 UB、跨流水依赖并减少不必要排序？
- **R06-03：搬运 API 已核对，地址遍历待确认。** 清理分散的 tokenPos 时如何限定每个 32B Flag 的范围，避免覆盖相邻核或尚未消费的记录？DataCopyPad 对齐及 stride 单位见文末，不重新讨论 API 能否精确写 GM。
- **R06-04：** Dispatch Flag、Combine 返回 Flag 的公共地址域和清理 owner 如何划分，跨轮复用与 R11 如何衔接？Combine 内部的清理时机和 Event 顺序迁入 [C24](ep-memory-server-dedup-combine-open-topics.md#c24)。

**讨论起点：** 两种时机都只清理本核已消费记录，通过 MTE3 写零并由核末 PIPE_ALL 覆盖完成。
PIPE_MTE3 约束该核所有前序 MTE3，不能只计算为当前 Token 的屏障，
也不能当作 Scalar 可提前覆盖发送 UB 的证明。

**关闭条件：** 选定一种首版策略，给出发送、清理、剔除、UB 释放及核退出顺序，
并说明分区跨核、同卡转发和连续轮次下的所有权。

## R07

**议题：完整 Workspace 与槽位布局**

**单份布局已按 Memory 复核收口。** 详细子区表、地址公式、生命周期和字节示例见[主设计 R07-01](ep-memory-server-dedup.md#r07-01-单份通信数据域布局)。

```text
[状态区：已有 profiling / cnt / cumsum 交换 / 16份完整前缀 / 就绪Flag / 1KB counter]
[数据接收区：source 返回子区 | Dispatch source/expert/q 子区]
[中转 Token 区：Combine 的 8*BS*K 个返回槽位]
```

Memory 的 SetDataStatus 已在 Dispatch 槽位前预留 Combine source 返回空间；本版直接采用两个独立子区，不设计覆盖复用。
Cnt 保持 `[L,R,32B]`；cumsum 交换为 `[16,16,32B]`，完整前缀仅保留 16 份，ready 为 `[16,16,32B]`。
记录内容、发布/读取/清理代码继续复用，所有具体 offset 根据上述区域大小重算。已有 profiling 在状态区预留当前最大记录容量，避免空入口遗留的 offset=0 与 cnt 混用。
Counter 仍是一份 1KB、首 4B 有效，算子末全核 SyncAll 后核 63 清零；不引入 counter 或中转窗口乒乓。

记 N=R*L，T 为 Dispatch 去重包跨度；当前 FP16/BF16 非量化返回沿用 Memory 的 `U=ceil(H*2/480)*512`：

| 大区/子区 | 字节数 | 寻址 |
| --- | --- | --- |
| 状态区 | 主设计逐项计算并按 512B 对齐 | cnt 按 expert/source；cumsum 按接收组内核号 0..15 |
| source 返回 | BS*K*U | 原始 Token/k；DIRECT 与 partial 使用相同槽位格式，按 sourceMask 消费 |
| Dispatch 接收 | N*BS*T | 保留 `(((srcRank+dstRank)%R*L+localExpert)*BS+q)*T` |
| relay 返回 | 8*BS*K*U | 全卡统一 t，recvSlot=uint64(t)*K+originalK |

后续公共通信 dtype/scale 若扩展，R08 只需提供对应 U_s/U_m；slot 数量、公共地址公式和已确认成员表不重开。
R=128、L=3、BS=256、H=7168、K=6 时，状态区 66KiB，source 返回 22.5MiB，Dispatch 1488MiB，relay 返回 180MiB；
单份数据域合计 1772685312B（约 1690.564MiB）。assistEx/sourceMask/group/expertScalesOut 在普通 GM 另算，不计入通信大区。
所有地址和大小乘积采用 uint64，4GB 通信域扩容及分配/映射由框架负责，不处理容量上限问题。

**子项状态：**

- **R07-01：单份布局已收口，待代码接入。** 子区表已给出确定 base/offset/stride 和所需字节数；正常单次调用中各数据子区独立，不要求为本轮 sendData/cnt 实现另选复用方案。多 stream/跨调用生命周期仍归 R11，Combine 内部调度和结果清理归 Combine 文档。
- **R07-02：统一中转容量，已确认并关闭。** 一块区域预留 8*BS 个 group，每组固定 K 槽；全卡统一取号，group 表预留 8*BS 行，不按来源卡分区。合计 group 数不超过 8*BS 由安全用例保证，不新增 X、校验、超限状态或错误路径。
- **R07-03：统一 RecvSlot，已确认并关闭。** 终端使用 assistEx 的 relayRank/t/k，地址为 `B_relay+relayOffset+recvSlot*U_m`，本卡与跨卡相同。第一跳从 firstHopRow 读取，只监听 members[0:n] 的返回位置，空洞不等待；K=1 无 relay group。

**关闭范围：** 单份数据域空间布局、非量化返回跨度、正常数据存活和来源卡无关寻址已经明确。代码/硬件验证仍需完成；
其他 dtype 与跨调用并发分别在 R08/R11 讨论，超限优化低优先级延后，不重新要求框架容量确认。

### X 超限遗留

**R07-04：X 超限处理与尽力转发方向。**

**优先级：低。状态：延后，不阻塞初版。** 初版仅给入安全用例，保证实际 group 数不超过 8*BS；不实现超限校验、状态、处理或测试。
X 的记录和使用与 R04-06 一同留待超限优化时讨论，初版不提供对应输入/输出，也不为它新增持久字段或发布流程。
之前讨论的“超限后由 Combine 报错”不再是初版要求；当前安全容量为 8*BS 个 group，超限后的行为统一留到后续重新讨论。

**后续方向备忘：** 考虑“尽力转发”：当 Combine 中转 buffer 达到容量限度后，后续 Token 记录为直接发回原始源端，不再转发。
这只是可能的处理方向，不是已确认方案，也不在初版增加满额判断或动态切换分支。
何时判断满额、容量按累计 Token 还是在途槽位计算、“不转发”作用于哪一段通信，以及 sourceMask、assistEx/返回引用和加权责任如何一致更新，均在后续处理超限时再讨论。
备忘不修改初版的 group 编号、固定成员表、直接返回规则或通信流程。

## R08

**议题：加权位置、权重与公共数值契约**

**已确定边界：** Dispatch 包携带各 route 的 weight，公共终端包保留原列表，接收端匹配自己的值。
每条 expert route 的 weight 保存到其所在 Rank 的 `assistEx`；头部及 assistEx 中的 FP32 字段编码已确认。
每条贡献必须在合并前加权且只加权一次；group 和 source 仍按已有 k 顺序消费。
Group 由 relay 对第一跳及全部本卡/跨卡转发成员执行加权求和；转发 expert 返回未加权输出，relay 逐项乘对应 weight 后累加。
Relay 向 source 返回已完成加权归约的 partial；Dispatch 已将 source expertScalesOut 的 returnK 位置设为 1.0。
Source 对 sourceMask 指定的有效结果统一乘 expertScales 后累加：partial 乘 1.0，DIRECT 乘真实 weight，不重复乘 returnK 的原始 weight。
源 Server 内逐 route DIRECT 不改为 group。调用方交给 Combine 的 expertOut 为未加权 expert 输出。

**Dispatch 格式已确认：** BF16/FP16 输入，非量化或 MXFP8；计算、scale 和输出布局按下节复用 Memory。
**剩余公共项：** expertOut dtype 以及 Combine 各段返回的通信 dtype/stride；首版权重必须非空，不支持 `expertScales=nullptr`。
R04-07/R04-08 已确认 expertScales 输入和 expertScalesOut 输出分离及各自生命周期；Combine 复用既有权重输入参数，不另设类型标记或权重快照。
包、assistEx 和 group 保存 Dispatch 输入时的真实权重，source 使用本卡 Dispatch 输出的 expertScalesOut，不能在两阶段之间或 Combine 前随意换权重。
relay 成员表布局、新增输出、行引用、recvSlot 编码及 GM 写入随 Dispatch 算子完成的语义已确认；两级归约的内部 dtype 和数值判据已迁入 Combine C15/C16。

**具体场景：** k0/k3 形成一个 partial，k1/k2 为 DIRECT，sourceMask 只保留 k0/k1/k2。
已确认 `partial = w0*y0 + w3*y3`；Dispatch 输出 expertScalesOut 为 `[1.0,w1,w2,x]`，x 不消费、无需清零。
Source 统一计算 `output = 1.0*partial + w1*y1 + w2*y2`，其中 yk 是未加权 expert 输出；不等待 k3，也不再计算 `w0*partial`。

**需要确认：**

- **R08-01：随 R04-07/R04-08 确认并关闭。** expertOut 未加权，relay 对第一跳和转发返回使用真实权重，source 按 sourceMask 统一乘 expertScales，其中 partial 权重为 1.0。无需 source 类型载体，具体接口注释归 R10。
- **R08-02：首版已确认。** expertScales 必须非空 FP32 `[BS,K]`，nullptr 直接报错；包/assistEx/group 使用真实输入权重，expertScalesOut 独立生成并持有到 Combine 消费完成。测试端按固定种子构造随机正权重，逐 Token 归一化使 Top-K 和为 1；不在算子增加归一化或求和校验。
- **R08-03：Dispatch 部分已确认并收口。** 支持 BF16/FP16 输入、quantMode=0/4，MXFP8 沿用 Memory 的 E4M3/E5M2、量化计算和 dynamicScalesOut 布局；当前尾部加长按去重公式重算 T。剩余仅为 expertOut 和 Combine 返回区的公共 dtype/stride；Combine 内部计算仍在 [C15](ep-memory-server-dedup-combine-open-topics.md#c15)。
- **R08-04：已迁移。** 确定性、误差、高精度参考及 NaN/Inf/强抵消/零 weight 等 Combine 数值规则迁入 [C16](ep-memory-server-dedup-combine-open-topics.md#c16)，不作为本轮 Dispatch 关闭条件。

**讨论起点：** 加权职责、权重载体和 BF16/FP16 非量化/MXFP8 Dispatch 已确认；后续仅继续明确 expertOut/Combine 返回的公共 dtype/stride。
R04 已确认 recvSlot/weight 成员表的行引用、槽位编码及算子完成语义，沿用独立 expertScalesOut 输出，不引入 source 类型标记。
所有 route 保持完整传输，零 weight 不自行省略 ready 或贡献。
本轮只确认 Dispatch/Combine 可见的 Payload、ExpertOut、返回区 dtype/stride；Combine 内部数值规则见 C15/C16。

**剩余关闭条件：** 完成 R08-03 的 expertOut/Combine 返回接口可见 dtype/stride；Dispatch 的六种组合、量化算法和 scale 布局不再待定，代码/设备验证另行完成。
权重传递方式已确定，不再把 Combine 时另发一轮 weight 作为默认缺失项。

### R08-03 Dispatch 量化复核结论

**已确认：BF16/FP16 输入，同时支持非量化和 MXFP8。** 此前“先非量化、MXFP8 后续确认”的建议撤回，直接采用 Memory 已有组合。

| quantMode | 输入 | 输出 hidden | scale 输出 |
| --- | --- | --- | --- |
| 0 | BF16 或 FP16 | 与输入同 dtype，[tokenNum,H]，行长 H*2B | dynamicScalesOut=nullptr |
| 4 | BF16 或 FP16 | FP8 E4M3 或 E5M2，[tokenNum,H]，行长 HB | dynamicScalesOut 必须非空，[tokenNum,S] 字节，S=align2(ceil(H/32)) |

MXFP8 由 expandXOutDtype 显式选择 E4M3/E5M2，其他 quantMode 不支持；不将 RESERVED 自动当作 FP8。
每 32 个 hidden 元素共享一个 1B E8M0 scale，按两个 scale 补齐；scale 不复用 expertScales/expertScalesOut 的 FP32 路由权重存储。

| 复核范围 | 结论 |
| --- | --- |
| 量化算法 | 直接复用 QuantMxfp8 及 TileXRMxfp8Quant 的 ComputeMaxExp/ComputeScale/ComputeFp8Data，包括 BF16/FP16 分支、舍入、饱和和特殊值处理 |
| 计算 UB | Q=align256(H)，原始量化输入 Q*2B，计算 scale 数 Q/32；输入补零和工作区继续按 Memory 处理 |
| 通信数据段 | FP8 hidden 从 0 开始，scale 从 Q 开始，S 字节有效跨度；D=Q+S，尾部在 align32(D) 处追加 |
| 拼包/发送 | 量化完成后写去重尾部，复用 R05-04 的 DataAsFlag/Vector/Event/MTE3 流水；packedPayloadBytes、quantTensor_ 和 outBuf 按去重 T 重算 |
| 接收 public 输出 | 沿用 Memory 的 hidden/scale 提取位置与 DataCopyPad 紧密行输出；按新 packedPayloadBytes 调整源 stride。监听调度和 assistEx 仍按去重方案 |
| 转发 | 沿用到达包中的 FP8 hidden 和 scale，不在 relay 重新量化；具体转发流水仍在 R05-05/R05-06 |
| Host/launch 接入 | 已有 public 参数足够；空入口尚未传 dynamicScalesOut/quantMode，且 layout 仍固定 2B hidden，需要按同语义接入，具体 ABI 已同步 R10-01 |

H=7168、K=6 时，S=224、D=7392、去重尾部 128B、Payload=7520B、T=8192B；非量化同配置 T=15872B。
详细尺寸及支持矩阵已合入[主设计](ep-memory-server-dedup.md#dispatch-dtype-与-mxfp8r08-03)。
源码依据为 [Memory 类型分支](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L1930)、[量化 helper](../../src/ep/kernels/tilexr_ep_mxfp8_quant.h)、
[Host 参数组合](../../src/ep/host/ep_dispatch_memory_host.cpp#L74)、[hidden/scale 拆出](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h#L1506)。
可以复用的是量化算法及这些公共约定；普通 Memory 对非空 expertScales 的拒绝条件、旧三字段 assist 和旧 LocalWindowCopy 调度不适用于去重入口。
当前去重代码仍为空入口；本轮完成方案核对，不声称已接入或验证六种组合。后续验证沿用 Memory MXFP8 golden，覆盖 hidden 和 scale、H 非 32/256 整倍数时的尾部及去重路径。

## R09

**议题：Combine 调度与 H 分块归约**

**状态：已迁移，不在本轮 Dispatch 讨论范围。** 原子项编号保留用于历史定位：

| 原子项 | Combine 新位置 | 内容 |
| --- | --- | --- |
| R09-01 | C17 | 四类职责与 kernel 阶段 |
| R09-02 | C18 | Group/source owner 与 H 分块 |
| R09-03 | C19 | 累加器驻留与等待策略 |
| R09-04 | C20 | 进度状态与慢成员推进 |
| R09-05 | C21 | H tile 协调和 partial 发布 |
| R09-06 | C22 | 完整责任结束条件 |

详情见 [Combine C17-C22](ep-memory-server-dedup-combine-open-topics.md#c17)。
本文仅通过 R04/R07/R10/R11 约定公共输入、返回位置和 public completion，不限定 Combine 内部调度。

## R10

**议题：双入口的具体 ABI 与 ExpertOut 契约**

**已确定边界：** 原 Memory 和去重入口分开，去重实现独立 .h，不满足条件报错，不兼容回退。
所有 Rank 的 BS 相同作为调用契约，不新增专门的跨 Rank BS 扫描或交换校验流程。
总专家数满足 `0<moeExpertNum<=1024` 且能被 R 整除，L 由商值推导并允许为 1；K 还须不超过总专家数，以满足原始 route 互不重复的前提。
当前 cnt/cumsum 增量的 Host/layout 已采用总专家数约束，允许 L=1 和 16P/L=64；计数 UB 与状态区容量按实际 shape 核算。
已具备 Host/CPU 计数测试，CANN/A5 编译和设备通信须单独验证，不据此宣称完整 Dispatch 已支持这些组合。
初版实际 group 数不超过 8*BS 同样由安全用例保证，不作为上述拒绝逻辑的检查项；不提供 X 输入/输出，超限状态与处理见 R07 低优先级遗留。

**本轮收口：** 权重输入输出方向、独立输出的 public/launch ABI 以及内部 layout helper 计算方式已有结论，见 R10-01。
**仍未明确：** 其余公共参数的支持矩阵、Combine 完整声明及 R10-03 至 R10-06 中尚未关闭的调用契约。
新增元数据清单已由 R04-01 确认，expert 计算到 Combine 的有效行数及行序已由 R04-02 确认，expertScales 复用已由 R04-07/R04-08 确认。
两类入口互不承担兼容义务；语义不变的 Memory 参数优先沿用，语义变化的方向及支持范围显式标注。去重入口必须显式要求非空 FP32 expertScales，不沿用普通 Memory 的空指针处理。

**需要确认：**

- **R10-01：权重及量化 ABI 已收口，待代码接入。** 公共签名在 groupOut 后、dtype 前加 float* expertScalesOut；量化直接沿用已有 public quantMode/dynamicScalesOut/expandXOutDtype。launch/Device 在 expandXOut 后加入 GM_ADDR dynamicScalesOut，在 groupOut 后、bs 前加入 GM_ADDR expertScalesOut，在 expertTokenNumsType 后、dtype 前加入 int64_t quantMode。共 13 个 GM_ADDR + 10 个 int64，184B，expertScalesOut/bs/quantMode/magic offset 分别为 96/104/152/176。权重输出按 layout.weightBytes 独立分配，Combine 复用已有 expertScales 输入；量化 hidden/scale 容量按 R08-03 分别计算，不新增布局查询 handle。
  沿用 Memory 的数据、weight/count、shape/dtype/stream 参数；新增 metadata 仍只有 assistEx/sourceMask/group，权重输出为 expertScalesOut，不再增加重复的 sourceWeight 参数、权重快照、partialMask、tokenNum、返回计数或 X。
  Memory 公共接口没有 workspace 参数，不能将其他 backend 的参数当作复用依据；持久 tensor 不自动要求 public plan/workspace handle。旧 assistInfoForCombine 占位的实际使用规则需在支持矩阵明确，不要求重复产出旧三字段记录。
- **R10-02：随 R04-02 确认并关闭。** ExpertOut 保持 Dispatch 行序和 `tokenNum=sendCounts[L*R-1]` 个有效行，与 assistEx 按 row 对应；expert 内部重排须在 Combine 前还原，不新增 row map，不消费 padding 行。
- **R10-03：** 输入、expert 输出和最终输出的 H/stride/dtype/连续性是否必须一致，哪些由 plan 固定，哪些由调用参数提供？
- **R10-04：部分确认。** 零 cnt/整卡 tokenNum=0 的通知、等待、cumsum 输出及状态清理均沿用 Memory，后续无 Token 时无监听任务，不再待定。剩余为 BS/H 的允许范围、BS=0、零长度时空指针及其他输入输出的别名边界；expertScales 输入与 expertScalesOut 输出分离已确认，且必须为非空 FP32 `[BS,K]`，nullptr 直接报错。
- **R10-05：** 哪些参数需跨 Rank 共同约定，如何在现有调用模型内保证，而不把已保证的 BS/top-k 重新变成额外扫描？
- **R10-06：** Mask、共享专家、TP 或其他未支持参数接受哪些空值/默认值；不合要求如何在可判断阶段拒绝？`expertScales` 不属于可选空值，必须提供非空 FP32 `[BS,K]` 输入。

**讨论起点：** 沿用已确认的 ExpertOut/assistEx 行关联、有效 `tokenNum`、expertScales 复用及 R10-01 的权重 ABI，按前述“同语义接口复核”表明确其余参数和支持范围。
有效行数及行序不重新讨论，也不从 Token 数值恢复身份。

**关闭条件：** 其余 Dispatch/Combine 签名、支持矩阵和错误返回方式与已收口的有效长度/行序、权重 ABI 及内部字节计算契约一致。
现有去重 kernel 仅实现 cnt/cumsum；非计数输出及完整 Dispatch/Combine ABI 仍需后续落实。

## R11

**议题：Plan、执行轮次与异步生命周期**

**仍未明确：** Dispatch plan 是持久路由信息，普通 Memory Combine 每次 launch 会取得新的 magic。
保存同一 plan 不自动意味着允许任意重复调用、多 stream 并发或窗口提前复用。

**具体场景：** Combine A 的 source 输出已完成，但同卡还有 partial 在途；
此时提交 Combine B 或复用同一 peer window/持久表，可能覆盖 A 仍引用的数据；counter 本身不由 Combine 读取，重置 counter 不等于覆盖旧 group。
另外，两个调用复用相同 plan 时，不能将上一轮的 ready 当作本轮结果。

**需要确认：**

- **R11-01：** Plan 的 communicator、布局版本、Dispatch 身份与本次 Combine invocation/epoch 如何绑定？
- **R11-02：** 同一 plan 是否允许顺序重复 Combine、保持 row/H/dtype 但更换 expertOut 数值；失败后是否允许重试？若重复 Combine，沿用保持不变的 expertScalesOut 和真实成员权重。Dispatch 原始 expertScales 输入始终只读。
- **R11-03：** 阶段一是否允许多 stream、多个未完成 plan、多个 microbatch，哪些资源需要隔离？
- **R11-04：** Host 返回、stream event、所有对外发送结束及远端消费结束，分别释放哪些 buffer/plan？
  Dispatch 算子完成包含全部 GM 写入已由 R04-04 收口，含 expertScalesOut 写出；metadata 与 expertScalesOut 保持到最后一次 Combine 消费完成。本项只讨论异步调用及资源释放时点，不再设计元数据发布条件或额外信号。
- **R11-05：部分已确认。** R05-03 已确定所有逻辑核在算子末尾执行一次全核 SyncAll()，同步返回后核 63 清零唯一 counter 的首 4B，首次零值沿用已有初始化。仍需落实整个窗口旧标志失效、数据存活期和各 Rank 调用次序；不重新选择 counter 容量、owner 或清零方式。

**讨论起点：** 分离不可变 plan 身份与可变通信轮次，沿用 TileXRCommNextMagic；
局部 Flag 清理不能代替轮次绑定。首先限定可支持的调用并发范围，再设计对应资源释放。

**关闭条件：** API 到设备完成的生命周期表、跨轮状态绑定和支持的重复/并发规则；
说明空职责与失败轮次也能结束，不以源端 yOut 完成代替整卡全部责任完成。

## R12

**议题：Dispatch 与公共契约验收标准**

**仍未明确：** 路由例子已能检查地址与数量，但 Dispatch 的可执行正确性矩阵、公共输出核对方式和性能目标尚未确定；Combine 数值判据由 C16/C23 承接。
此项是后续实现的验收设计，本轮文档更新不要求运行 NPU 测试。

**需要确认：**

- **R12-01：** 使用何种独立 oracle 验证原始 route、q、group/source 选择和每条贡献恰好一次；
  expert 输出应能区分 source/token/k/expert，不能全部原样返回 hidden 掩盖错配。
- **R12-02：已迁移。** Combine-only 的合法 plan 和内部独立验证迁入 [C23](ep-memory-server-dedup-combine-open-topics.md#c23)；本轮只验 Dispatch 输出的公共契约。
- **R12-03：** 哪些配置和路由构成 Dispatch 首版必须通过的正确性矩阵？Combine 数值矩阵迁入 C23。
- **R12-04：** 哪些规模/BS/H/K/路由分布用于 Dispatch 性能比较，如何固定 Payload/metadata/Flag 计量口径？Combine 内部及端到端收益目标迁入 C23。

**最低覆盖方向：**

| 范围 | 后续应验证的行为 |
| --- | --- |
| 路由 | 源 Server 内本卡/跨卡多 expert 均 DIRECT、同 Token 混合 DIRECT 与远端 group 的两阶段发送、先发较晚 Token 时 q 仍保留原始前缀、远端 relay 本卡/跨卡转发及交错 k；D03 的原 T8 例子见主设计 |
| 边界 | K=1/32（K 不超过总专家数）、L=1、总 expert 数上限 1024；覆盖 (R,L)=(16,64)/(64,16)/(128,8) 及 24P/L=42 的整除边界，不将这些 shape 一律视作通信容量已满足；另含零 cnt/整卡 `tokenNum=0`、H 尾部及共同支持的 BS/H，BS=47/48 的 cnt 核组切换、无源端 Token 的核仍完成 cnt 职责，以及固定 16 核接收/cumsum（每核至多 64 条 cnt） |
| 中转容量 | 实际 group 数为 0/BS/8*BS-1/8*BS、group 数不超过 8*BS 且转发结果条数 Y>8*BS、仅本卡转发也占一个 group、不同 source 相同 TokenId、本卡与跨卡 Token 的正常合计；不包含超限用例或 X 输出断言 |
| 统一槽位 | 实际 group 数大于旧 BS 上限但不超过 8*BS；n=1 只列出一个返回位置、不等待其他 K 槽；同一卡返回多条不同 k、本卡/跨卡混合及交错 k 均按 t*K+k 唯一落槽 |
| 并发 | 原子取号唯一且每 Token 一次、部分发送恢复、公共 UB 最后使用者、pendingIdx 末项换入不遗漏、各核第一跳/DIRECT 进度不同及在途 MTE3 跨逻辑阶段 |
| 可见性 | 按统一 Payload 长度执行 Memory DataAsFlag 拼接/全块检测/剥离、恢复完整尾部、算子完成后 GM 输出完整、Flag 清理范围、MTE3 在途与接收乱序/延迟 |
| 公共输出 | assistEx、sourceMask、group 三个新增输出能被独立解析，group 内的 recvSlot、行引用和返回地址一致；有效 group 识别按已确认的 R04-03 验证，不要求 X；Combine 内部消费/归约验证见 C23 |
| 权重输入输出 | 输入 expertScales 始终保持真实值；expertScalesOut 的 returnK 为 1.0、DIRECT 为真实值；随包/assistEx/group 使用真实权重，sourceMask 空洞不消费。测试输入随机归一化，K=1 时权重为 1，FP32 行和允许 1e-6 误差 |
| 生命周期 | 支持范围内连续多轮、重复/并发调用、空职责与全部发送责任完成、支持范围内失败退出、旧 ready 不被复用；排除中转容量超限场景 |

不增加重复 top-k、invalid expert、空 route、输入 mask 或中转容量超限的错误输入测试。
初版安全用例保证每个 relay 的实际 group 数不超过 8*BS，不验证超限校验/报错/降级；X 记录、超限用例及处理均为 R04-06/R07 低优先级遗留。
BS=0 只有在 R10 接纳后才纳入行为测试。
Host/reference、编译检查与目标硬件结果分别报告，不以匹配源码文本的 guard 代替行为测试。

**性能口径：** 统计原始 expert route 数、source 跨 Server 第一跳/DIRECT 包数、relay 展开次数和真实通信字节。
Combine partial 的字节收益与两级数值比较迁入 C23。
Dispatch 比较须包含完整 Token 尾部、Flag/填充、Server 内转发、weight/plan、原子计数和监听开销；Combine 的归约开销见 C23。

每阶段报告 backend、native status、kernel version、算法/通信数据量、可读 latency、适用 bandwidth，
跨所有 Rank 的 median/min/max。诊断插桩与性能基线分开；
复用已有匹配 HCCL Test 资格记录。MoonEP 模型/replay 保留框架 profiler 和 Dispatch/Combine stage barrier，
使用允许的 grouped mode，默认 group_credit。

**关闭条件：** 确定 Dispatch 独立参考、公共输出契约、必测矩阵、通信字节口径、性能基线和目标场景。
之后的实际测试结果单独记录，不将本项确认视为验证通过。

## 延后事项与参考边界

共享专家继续低优先级延后，不阻塞阶段一。当前 Memory 的参考映射为：

```text
sharedRanks = sharedExpertRankNum
sharedExperts = sharedExpertNum
L = moeExpertNum / (R - sharedRanks)

MoE destinationRank = sharedRanks + expertId / L
MoE localExpertId = expertId % L

ranksPerShared = sharedRanks / sharedExperts
shared expert j destinationRank = j * ranksPerShared + srcRank % ranksPerShared
shared expert j assistK = K + j
```

共享 Rank 位于最前面；普通 expertIds 仍从 0 开始，source 为每个共享 expert 选择一张承载卡。
例如 sharedExperts=2、sharedRanks=8、srcRank=11，共享目标为 3 和 7。
当前 Host 要求 shared 参数同时为零或同时非零，非零时 sharedRanks 可被 sharedExperts 整除、
sharedRanks<R，moeExpertNum 可被 R-sharedRanks 整除。
共享 rank 的本地 expert 数为 1；共享 count 的 rank 索引、普通 expertIds 与 demo oracle 的编号域不能混用。
以上只保留源码参考，去重阶段一 shared 参数仍必须均为 0；是否支持共享去重、K+j mask 和 partial 均留到扩展阶段。

## 官方 API 核对

**核对日期：2026-09-10；依据：CANN Community Edition 9.1（官网路径 `910`）完整正文及 Ascend 950PR/950DT 产品列。**
用户提供的 [AtomicAdd latest 链接](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/latest/API/ascendcopapi/docs/zh/api/SIMD-API/basic_api/atomic_operations/AtomicAdd.md)当前跳转到 9.2.0-beta.2；下表使用项目目标 9.1 的固定版本链接。
此前按“9.1”过滤检索未命中，不能据此认定目标版本无文档；本次已通过官网目录取得正文，搜索结果中的版本标签为 CANN 9.1.X。
本节确认基础 API 的能力和约束；R10 的 TileXR 公共 ABI、并发资源数量及调度选择仍需按各议题讨论，官网不会替项目决定这些内容。

| 关联子项 | 官网接口与确认结论 | 剩余事项 |
| --- | --- | --- |
| `R05-03` | [AtomicAdd][api-atomic]：Scalar 对 GM 原子加，uint32_t 可用，返回旧值；950PR/950DT 支持 | 取号及单计数器清零方案已确认，见 R05-03；官网未明确额外地址对齐数值不再作为独立方案选择 |
| `R05-03、R07-01、R11-05` | [DataCacheCleanAndInvalid][api-cache]：支持 DCache/GM 一致性处理，950 支持 CACHELINE_ATOMIC；Scalar 原子与搬运的依赖需手动同步 | 一份 1KB 专用区、首 4B 及核 63 清零 owner 已确定；按实际访问落实可见性，具体 offset 已由 R07-01 给出 |
| `R05-04 至 R05-06` | [SetFlag/WaitFlag][api-flags]：同步指定的源/目标流水，WaitFlag 会阻塞目标流水；[AllocEventID][api-alloc]、[FetchEventID][api-fetch]、[ReleaseEventID][api-release]管理 ID | API 语义已确认；非阻塞查询这一候选未查到，源端 1 至 8 路及 lane EventID 已收口，数据转发的进度/资源组织仍需讨论，不要求必须采用查询接口 |
| `R05-01、R03、R04、R06-03` | [DataCopyPad UB→GM][api-copy-out] / [GM→UB][api-copy-in]：GM 无 32B 起点/长度约束，UB 起点须 32B 对齐，blockLen 以字节计且为 sizeof(T) 的倍数 | R05-01 的 uint32 最小 4B 写回已有官网依据；清理地址遍历仍按 R06 选择 |
| `R04、R05-05` | [DataCopy UB→UB][api-copy-ub]：连续拷贝的 count 是元素数，两端及总字节数须 32B 对齐；拷贝 32B uint32 头部使用 count=8 | 支持的逻辑通路为 VECIN→VECCALC、VECCALC→VECOUT；具体 Tensor 位置和 Event 需与资源布局匹配 |
| `R05-02/R05-04/R05-05` | [TPipe::InitBuffer][api-init]：TBuf 的 len 以字节计，分配向上补齐 32B；TPipe 析构释放内存 | 双表仍只存 UB；总 UB、buffer 数及同时存活范围须核算，不能由单个 API 推导并发路数 |
| `R06-02、R05-05` | [PipeBarrier][api-pipe]：PIPE_MTE3 排序本核前后 MTE3，PIPE_ALL 等待本核所有先前提交流水完成 | 语义已确认；清理粒度、跨流水 UB 复用及 Event 组织仍待确认 |
| `R05-01、R05-03、R11` | [SyncAll][api-sync]：核间同步；硬同步是全核同步，软同步另有 usedCores 参数 | 本版本仅在算子末尾由全部 64 个逻辑核执行一次，用于 counter 清零前收敛；不在第一跳/DIRECT 间或其他发送阶段新增屏障。不能让 16 核单独进入要求全核参与的硬同步；具体窗口复用仍归 R11 |

### 原子与缓存的使用边界

AtomicAdd 的手动同步要求来自其“约束说明”，官网示例在原子操作后用配对的 `S_MTE2` SetFlag/WaitFlag 再进行 MTE2 读取。
这只示范同核 Scalar→MTE2 的依赖，不能用来证明其他核已经完成 counter 初始化，也不要求取号后额外读回 counter。

DataCacheCleanAndInvalid 的 GM 原型为：

```cpp
template <typename T, CacheLine entireType, DcciDst dcciDst>
__aicore__ inline void DataCacheCleanAndInvalid(const GlobalTensor<T>& dst);
```

`SINGLE_CACHE_LINE` 按 64B cache line 处理；非 64B 对齐时，官网规定只处理传入地址至下一 64B 边界的部分。
`CACHELINE_OUT` 用于 GM 一致性，`CACHELINE_ATOMIC` 在 950 上支持，另有 LocalTensor/CACHELINE_UB 形式。
Clean 会写回脏数据，Invalid 会使缓存失效；官网多核示例明确提示：各核即使写同一 cache line 的不同位置，后续 clean 也可能相互覆盖。
本设计已用一份 1KB 的专用区隔离 counter，算子末尾全核 SyncAll() 后由核 63 仅清零该区首 4B；落实具体 offset 时保持该所有权，不把每次 AtomicAdd 后无条件刷整个 DCache 当成已确认方案。
32B 搬运对齐、64B cache line 与原子接口的地址要求是不同约束，不能互相替代。

### Event 与发送 UB 生命周期

| 操作 | 官网语义 | 在本设计中的边界 |
| --- | --- | --- |
| `SetFlag<evt>(id)` | 源流水此前读写完成后置位；不阻塞源流水的下一条指令 | 可以建立发送完成信号，但置位指令的下发不等于 MTE3 已完成 |
| `WaitFlag<evt>(id)` | 目标流水等待置位，成功后将该标志清零；返回类型为 void | 不是 bool 完成查询。MTE3→V/MTE2 的等待可能阻塞公共监听所需流水，MTE3→S 的等待会阻塞 Scalar |
| `AllocEventID<evt>()` | 申请并占用 TEventID，直至对应 Release | 多个同时在途的同类事件应保持各自 ID 所有权；源端直接沿用 R05-04 的显式 lane ID，数据转发资源归 R05-05 |
| `FetchEventID<evt>()` / `FetchEventID(evt)` | 返回可用 ID，但不占用 | 适合短期配对同步；连续用于多个未完成的同类事件可能取得同一 ID，官网提示会出现卡死等未定义行为 |
| `ReleaseEventID<evt>(id)` | 释放 Alloc 申请的 ID | 不提供 MTE3 完成检测或等待；应在对应同步消费完成、该所有者不再使用后释放 |

SetFlag/WaitFlag 的 HardEvent 类型及 ID 必须一致，同类型同 ID 在前一次配对消费前重复 Set 属于未定义行为。
950 的 eventID 范围为 0–7；静态 Tensor 文档建议使用 0–5，6 为系统规划、7 用于框架自动同步，不能据此承诺有 8 路可自由使用的发送资源。
TPipe 编程须通过其分配接口取得 ID，具体预算分别核对源端与数据转发核的同步占用。上述 API 约束用于实现核对，不重新选择已确认的源端 1 至 8 路算法范围。
发送 UB 的最后使用者完成后，仍须建立到下一写入流水的依赖；pendingIdx 删除、Event ID 释放和 UB 可改写是不同状态。
PIPE_MTE3 只排序 MTE3，不能独自允许 Scalar/Vector 提前改写发送 UB；PIPE_ALL 的核内完成也不等于远端已消费或跨核屏障。

### 非对齐搬运与元数据拷贝

本设计使用 DataCopyExtParams 形式；UB→GM 为三参数，GM→UB 另需 DataCopyPadExtParams，不混用两个方向的调用形式。

```cpp
// UB→GM：maskUb 起点按 32B 对齐；count_i>0，GM 只写有效 uint32 元素。
AscendC::DataCopyExtParams copy{1U, count_i * 4U, 0, 0, 0};
AscendC::DataCopyPad(maskGm[start_i], maskUb, copy);
```

| DataCopyExtParams 字段 | UB→GM | GM→UB |
| --- | --- | --- |
| blockCount | 连续数据块数量，0..4095 | 同左 |
| blockLen | 每块字节数，0..2097151，且是 sizeof(T) 的倍数，不超过 UB 空间 | 同左 |
| srcStride | 块间空隙，单位 32B | 块间空隙，单位字节 |
| dstStride | 块间空隙，单位字节 | 块间空隙，单位 32B |
| rsv | 保留字段置 0 | 保留字段置 0 |

例如 count_i=5 写 20B，count_i=1 写 4B；UB 填充不会被写到 sourceMask 的相邻核区间。count_i=0 仍按已确认逻辑跳过写回。
GM→UB 的 `isPad=false` 表示不使用 paddingValue 指定填充值，不代表没有物理补齐；dummy/未用 entry 继续按 R03 的有效长度忽略，不额外清零。
32B metaUB 的 UB→UB DataCopy 则使用元素数 count=8（uint32 视图），不能把上述字节参数直接复用为 count。
完成拷贝/Scalar 字段修改后再按真实流水依赖发布 MTE3，并保持相关 UB 到最后使用者完成；本节不增加逐 Token PIPE_ALL。
InitBuffer 的 TBuf 原型为 `InitBuffer(TBuf<bufPos>& buf, uint32_t len)`；分配字节数按 32B 向上补齐，官网约束一个 kernel 中使用的 buffer 总数不超过 64。该数量上限不等于 Event 数或允许的并发发送数。

### 官网未查到的内容与后续边界

- **非阻塞 MTE3 完成查询（早期 R05-04 候选，供后续数据转发讨论）：未查到。** 已检查 9.1 基础 API、核内同步、TPipe/TQue 和 C-API 同步目录，并检索 CheckEvent、CheckFlag、QueryEvent、GetEvent、TryWaitFlag、非阻塞/MTE3 等词，未找到可读取该完成状态的公开接口。本卡源端已采用 Memory 环形 Set/Wait，不依赖该查询；该候选仅供后续数据转发 UB/Event 回收讨论，不是已确认必选 API；允许继续讨论满足相同并行和生命周期要求的其他机制，不能虚构查询接口或将 WaitFlag 解释成非阻塞查询。
- **额外原子地址对齐数值（R05-03）：AtomicAdd 正文未明确。** 本设计已选择在对齐的 1KB 专用区首 4B 取号；该官网缺省说明仅保留作实现依据边界，不继续占用独立方案待确认项，也不表示支持任意未对齐地址。
- SyncAll 的能力已查明；纯 Vector 硬同步要求 `__mix__(0,1)`，且软/硬同步的参与核与 workspace 条件不同。本版本将全核 SyncAll() 放在算子末尾，仅用于 counter 清零前的收敛；不在发送阶段或阶段切换处新增 barrier，窗口复用仍归 R11。

**核对经验：** 官网 latest 会跨版本，搜索索引和正文目录可能不一致；应固定版本、读取完整原型/约束/产品支持表，再将结论映射到已有议题。
本轮仅核对文档，未编译或运行 NPU；后续验证仍需覆盖实际 SDK/SoC、并发取号、缓存可见性及监听/发送前进性，不能将文档支持等同于已验证的重叠性能。

[api-atomic]: https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/ascendcopapi/docs/api/SIMD-API/%E5%9F%BA%E7%A1%80API/%E5%8E%9F%E5%AD%90%E6%93%8D%E4%BD%9C/AtomicAdd.md
[api-cache]: https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/ascendcopapi/docs/api/SIMD-API/%E5%9F%BA%E7%A1%80API/%E7%BC%93%E5%AD%98%E6%8E%A7%E5%88%B6/DataCacheCleanAndInvalid.md
[api-flags]: https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/ascendcopapi/docs/api/SIMD-API/%E5%9F%BA%E7%A1%80API/%E5%90%8C%E6%AD%A5%E6%8E%A7%E5%88%B6/%E6%A0%B8%E5%86%85%E5%90%8C%E6%AD%A5/SetFlag_WaitFlag_ISASI.md
[api-alloc]: https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/ascendcopapi/docs/api/SIMD-API/%E5%9F%BA%E7%A1%80API/%E8%B5%84%E6%BA%90%E7%AE%A1%E7%90%86/TPipe/AllocEventID.md
[api-fetch]: https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/ascendcopapi/docs/api/SIMD-API/%E5%9F%BA%E7%A1%80API/%E8%B5%84%E6%BA%90%E7%AE%A1%E7%90%86/TPipe/FetchEventID.md
[api-release]: https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/ascendcopapi/docs/api/SIMD-API/%E5%9F%BA%E7%A1%80API/%E8%B5%84%E6%BA%90%E7%AE%A1%E7%90%86/TPipe/ReleaseEventID.md
[api-copy-out]: https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/ascendcopapi/docs/api/SIMD-API/%E5%9F%BA%E7%A1%80API/Memory%E7%9F%A2%E9%87%8F%E8%AE%A1%E7%AE%97/%E6%95%B0%E6%8D%AE%E6%90%AC%E8%BF%90/DataCopyPad_UBToGM.md
[api-copy-in]: https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/ascendcopapi/docs/api/SIMD-API/%E5%9F%BA%E7%A1%80API/Memory%E7%9F%A2%E9%87%8F%E8%AE%A1%E7%AE%97/%E6%95%B0%E6%8D%AE%E6%90%AC%E8%BF%90/DataCopyPad_GMToUB.md
[api-copy-ub]: https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/ascendcopapi/docs/api/SIMD-API/%E5%9F%BA%E7%A1%80API/Memory%E7%9F%A2%E9%87%8F%E8%AE%A1%E7%AE%97/%E6%95%B0%E6%8D%AE%E6%90%AC%E8%BF%90/DataCopy_UBToUB_continuous.md
[api-init]: https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/ascendcopapi/docs/api/SIMD-API/%E5%9F%BA%E7%A1%80API/%E8%B5%84%E6%BA%90%E7%AE%A1%E7%90%86/TPipe/InitBuffer.md
[api-pipe]: https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/ascendcopapi/docs/api/SIMD-API/%E5%9F%BA%E7%A1%80API/%E5%90%8C%E6%AD%A5%E6%8E%A7%E5%88%B6/%E6%A0%B8%E5%86%85%E5%90%8C%E6%AD%A5/PipeBarrier_ISASI.md
[api-sync]: https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/910/API/ascendcopapi/docs/api/SIMD-API/%E5%9F%BA%E7%A1%80API/%E5%90%8C%E6%AD%A5%E6%8E%A7%E5%88%B6/%E6%A0%B8%E9%97%B4%E5%90%8C%E6%AD%A5/SyncAll.md

## 源码参考

Memory 参考代码：
[Dispatch](../../src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h)、
[Combine](../../src/ep/kernels/tilexr_ep_combine_memory_kernel.h)、
[Host Layout](../../src/ep/host/ep_memory_layout.cpp)、
[公共 API](../../src/include/tilexr_ep.h)、
[窗口](../../src/ep/common/ep_memory_window.h)、
[Kernel Launch](../../src/ep/host/ep_kernel_launch.cpp)、
[Data-as-flag](data-as-flag.md)。
项目环境与验证约束见 [AGENTS](../../AGENTS.md) 和[构建验证](../BUILD_VERIFICATION.md)。

## 第一轮编号映射

以下 D 锚点仅用于旧讨论定位，不再保存另一份议题正文。

## D01

支持范围与 Server 映射已合入[主设计](ep-memory-server-dedup.md#支持范围)，无独立遗留项。共享专家见上面的延后参考。

## D02

Cnt/数据接收布局、同 BS 和 cumsum 语义已合入[主设计](ep-memory-server-dedup.md#dispatch-接收布局)，无独立遗留项。

## D03

公共 Token 方向已合入主设计；字节布局、有效长度与到达判定转 [R03](#r03)，数值责任转 [R08](#r08)。

## D04

Relay 与跨 Server 适用边界已由 [R01](#r01) 确认；Token 分核、各核独立发送顺序、q/cnt 复用及取消双 Group 已由 [R02](#r02) 确认。

## D05

同组核与先 cumsum 后均分已合入主设计；具体分核、同步和资源转 [R05](#r05)。

## D06

本卡/跨卡统一原子取号、持久输出行关联及算子完成语义已合入主设计，结论见 [R04](#r04)；统一中转区和槽位见 [R07](#r07) 已确认结论，原子 API 转 [R05](#r05)，X 超限为低优先级遗留。

## D07

固定中转容量及初版实际 group 数不超过 8*BS 的安全输入契约已合入主设计；完整预算转 [R07](#r07)，X 记录及超限优化低优先级遗留，清理转 [R06](#r06)，跨轮复用转 [R11](#r11)。

## D08

双入口、持久元数据布局及行关联已合入主设计，结论见 [R04](#r04)；具体 ABI 转 [R10](#r10)，生命周期转 [R11](#r11)。

## D09

Weight 传递、成员记录布局及 relay/source 加权责任已合入主设计；[R04](#r04) 明确权重输入/输出分离，Combine 参数复用，首版输入非空。公共数值格式继续在 [R08](#r08) 讨论。

## D10

初版实际 group 数不超过 8*BS 的安全输入、不校验超限已合入主设计；X 记录及超限优化低优先级遗留转 [R04](#r04)/[R07](#r07)，不支持配置的接口检查转 [R10](#r10)，安全用例验收转 [R12](#r12)。

## D11

双表、共享发送副本和下发后剔除已合入主设计；执行资源转 [R05](#r05)，Flag 清理转 [R06](#r06)。
