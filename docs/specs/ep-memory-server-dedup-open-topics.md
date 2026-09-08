# EP Memory Server Dedup 第二轮待确认项

## 使用方式

第一轮已经确认的约束已合入[主设计](ep-memory-server-dedup.md)。
本文集中记录仍有歧义、设计不完整或需要目标 API 证据的事项，不再重复把已确认原则列为选择题。
第二轮使用 R01-R12；旧 D/C 编号保留映射，讨论时优先引用 R 编号。
[Combine 旧议题](ep-memory-server-dedup-combine-open-topics.md)仅保留索引，不再维护另一套决策正文。

“待确认”表示需选择具体机制；“实现核对”表示逻辑方向已经确定，但 API/资源/可见性尚无足够证据。
各项候选建议只作为讨论起点。关闭议题时应将明确的输入、输出、所有权与完成条件合入主设计，
不能把“文档有候选”当作机制已经闭合，也不能把设计确认当作硬件验证通过。

## 已归并结论

| 已确定内容 | 主设计位置 | 第一轮来源 |
| --- | --- | --- |
| 8P/Server、16P 至 128P、连续普通 expert 映射、多 expert、K=1..32、同 BS、安全输入及阶段一排除功能 | [支持范围](ep-memory-server-dedup.md#支持范围) | D01、D02、D08 |
| 只做 Server 级去重，Server 内逐 expert 发送；source 直发所有原始 cnt | [路由与计数](ep-memory-server-dedup.md#路由与计数) | D01、D02 |
| Memory cnt/数据分区、source/expert/q 地址、第一跳占实际 expert route 槽位 | [Dispatch 接收布局](ep-memory-server-dedup.md#dispatch-接收布局) | D02 |
| 保留原始 entries，同一 Token 的终端发送共用 UB；接收端按 expert 匹配 k/weight | [Token 格式与转发](ep-memory-server-dedup.md#token-格式与转发) | D03、D11 |
| Cumsum/本地 copy/转发同组核；先 cumsum、按 M 均分、双表、末项覆盖剔除及退出前 PIPE_ALL | [分核与监听](ep-memory-server-dedup.md#分核与监听) | D05、D11 |
| Relay 首次转发前按不同 Token 原子取号一次；编号随所有副本发送，Dispatch 生成持久监听信息 | [中转编号与持久表](ep-memory-server-dedup.md#中转编号与持久表) | D06、D08 |
| Server 内归约后回 source；保留原始 k、returnK、成员及 source slot 选择语义 | [Combine 数据流](ep-memory-server-dedup.md#combine-数据流) | 主设计、D06、C01/C03 |
| 七个发送卡分区共 `7*BS*K` 槽位；X 为本 relay 合计不同监听 Token 数，X>BS 时 Combine 报错 | [容量与生命周期](ep-memory-server-dedup.md#容量与生命周期) | D06、D07、D10 |

不重新讨论已经确定的 top-k 查重/invalid expert 处理、卡级去重、转发 cnt、逐 Token 控制包、
cumsum 前监听或普通路径自动回退。共享专家继续延后。
原文中的“远端单目标”必须区分一条 route 与一张卡上的多条 expert route，剩余问题见 R01。

## 第二轮索引

| 编号 | 议题 | 类型 | 需要形成的结论 | 来源 |
| --- | --- | --- | --- | --- |
| [R01](#r01) | Relay 选择与仅含本地贡献的 Group | 待确认 | 生产 relay 规则、单远端卡多 expert 的 group/编号规则 | D04、D06；C01 |
| [R02](#r02) | 源端 Token 分工与 q 生成 | 待确认 | Top-k owner、ordinal 算法、cnt 数据来源 | D04、D05 |
| [R03](#r03) | 公共 Token 的字节布局与到达判定 | 待确认 + 实现核对 | 有效长度、返回引用编码、stride 与 Flag 覆盖 | D03；C06 |
| [R04](#r04) | 持久 Plan 与多本地 Expert 行关联 | 待确认 | 元数据布局、逐字段 writer、行关联及发布边界 | D06、D08；C01/C02/C03 |
| [R05](#r05) | Dispatch 分核、原子与 UB/Event 资源 | 待确认 + 实现核对 | 核数、原子接口、位置表容量、部分发送进度和 UB 回收 | D05、D06、D11 |
| [R06](#r06) | Flag 清理时机与顺序 | 待确认 + 实现核对 | 核末或 Token/批次清理、具体排序与范围 | D07、D11；C06/C09 |
| [R07](#r07) | 完整 Workspace 与超限轮次处理 | 待确认 | 区域总预算、Dispatch 超限行为、Combine 通信前一致拒绝 | D07、D10；C09/C12 |
| [R08](#r08) | 加权位置、结果类型与数值格式 | 待确认 | Weight 应用端、DIRECT/partial 区分、dtype/量化/误差 | D09；C03/C04/C05 |
| [R09](#r09) | Combine 调度与 H 分块归约 | 待确认 | 四类职责的分核、累加器存放、慢成员与完整结束条件 | C07/C08/C12 |
| [R10](#r10) | 双入口的具体 ABI 与 ExpertOut 契约 | 待确认 | 参数/有效行/stride/行序、调用约束和不支持参数规则 | D08、D10；C02/C11 |
| [R11](#r11) | Plan、执行轮次与异步生命周期 | 待确认 | generation/epoch、重复调用、并发与跨轮复用 | D08；C10/C12 |
| [R12](#r12) | 正确性与性能验收标准 | 待确认 | 独立参考、覆盖矩阵、目标场景及误差/收益标准 | D10；C13/C14 |

建议先讨论 R01/R03/R04/R08/R10，确定逻辑格式和调用边界；
再讨论 R02/R05/R06/R07/R09/R11 的执行机制。R12 随相关决定补齐。
本顺序不要求重开第一轮已确认内容。

## R01

**议题：Relay 选择与仅含本地贡献的 Group**

**仍有歧义：** 示例使用 top-k 首目标 relay，尚未确认生产策略。
远端 Server 只有一张目标卡但有多个 expert 时，旧主设计的“单目标直发”与新的逐 expert/Server 去重语义不能直接混用。

**具体场景：** R0 的一个 Token 在 Server 1 只命中 R8 的 expert25/26。
可以只有一份跨 Server hidden，随后在 R8 完成两条 expert 输入；Combine 则没有其他七张卡的返回，
因此 X=0。若仍沿用 n>0 创建 group，不能使用远端监听计数器为该 group 取号后又要求计数器等于 X。

**需要确认：**

- 生产是否固定 top-k 首目标作为 relay；若另选，source 如何确定同样的第一跳、forwardEntries 和 returnK？
- 单远端卡多 expert 是否统一采用第一跳加本卡逐 route 接收；其仅含本地贡献的 group 如何标识并完成 partial？
- 仅含本地贡献的 group 与需要远端返回的 group，是否共用逻辑记录但采用不同索引域？
  不能因无需远端监听而遗漏本地归约，也不能占用/放宽已确认的 X 上限。

**讨论起点：** 先采用首目标 relay，保持 Server 内逐 expert 的统一接收处理；
为纯本地 group 定义不消耗 X 的关联方式，与 R04 一并明确。
不将平均负载作为容量保证，不在执行中加入原 Memory 回退。

**关闭条件：** 给出远端单 route、单卡多 expert、多卡多 expert 的分组/编号分类表；
说明第一跳绑定 route、所有 expert 行及 source returnK 的唯一责任。

## R02

**议题：源端 Token 分工与 q 生成**

**仍未明确：** 当前 Memory 按扁平 route 分核，同一 Token 的多个 k 可由不同核处理。
Server 去重需要在 source 联合处理该 Token 的 top-k，而按 Token 分核本身并不能生成正确的 q。

**具体场景：** R0/T8 到 expert26 的 q 取决于 T0..T7 是否命中 expert26，
不是 T8 的 8，也不是它在本 Token entries 中的序号。不同核处理前后 Token 时，不能各自从 q=0 开始。

**需要确认：**

- 是否由一个逻辑 owner 处理 Token 的全部 K 条 route，并写 sourceMask、分组和第一跳 descriptor？
- q 沿用原始路由前缀计算，还是采用分块统计加前缀；如何保持各 source/expert 的 q 连续且唯一？
- Cnt 核独立遍历 expertIds，还是消费源端预处理结果；共享结果如何同步、占用多少 workspace？
- 全核/非全核 cnt 模式与数据发送核何时切换，哪些共享数据必须在发送前可见？

**讨论起点：** 以完整 Token 为源端逻辑任务，沿用 q 的原始 source/expert 语义；
先明确算法及唯一 writer，再评估是否共享统计结果。Cnt 仍由原始 source 直发。

**关闭条件：** 明确源端 owner、q/cnt 的计算输入、临时数据和同步阶段；
验证跨核同 expert 的 q 无重叠、无空洞，且与接收 cumsum 一致。

## R03

**议题：公共 Token 的字节布局与到达判定**

**仍未明确：** 保留原列表、公共发送 UB 和终端匹配 expert 已确定，实际字节格式未确定。
forwardCount 改为 -1 后丢失原始 n；relayRank 也不能由原始 srcRank 或终端接收槽位唯一推导。

**具体场景：** K=6 的 Token 在某个 Server 只有三条 route，则 n=2。
终端不能假定扫描五条 entry，也不能使用公共头部的第一跳 k/weight 作为自身值。

**需要确认：**

- 是否采用独立 forwardEntryCount，及独立于 entries 的 relayRank/forwardTokenIdx 位置？
- 字段位宽、顺序、对齐、DIRECT/第一跳未使用字段、最大预留 entry 数及统一 T 如何定义？
- 终端解析匹配的固定范围、源端是否初始化 padding、第一跳/终端有效字段如何纳入到达 Flag？
- Dispatch、Combine member、partial、DIRECT 各自的 payload/scale/Flag 布局与 stride 如何确定？
  整条结果或 H 分块发布的选择与 R08/R09 联动，不能仅凭第一块 Flag 读取尾部。

**讨论起点：** 使用独立有效长度和公共返回引用，保持定长可定位头部及配置级统一 stride。
Dispatch 接收目标 expert/q 仍由槽位表达，不为当前包重复增加同义目的字段。
可以参考 Data-as-flag，但须证明新尾部和跨设备写入满足可见性条件。

**关闭条件：** 一份字段/offset/长度/单位表、各模式有效范围、stride 计算公式及 Flag 发布/读取顺序。
只修改 forwardCount 和公共返回字段即可多次转发，所有接收端能唯一恢复自己的 k/weight。

## R04

**议题：持久 Plan 与多本地 Expert 行关联**

**仍未明确：** 已有 assistEx 与 forwardPlan 的逻辑约定，并已确定 Dispatch 产出中转监听信息，
但原来的单一 localRow 不能表达多本地 expert，也未确定监听表、group 和 source 元数据的具体打包。

**具体场景：** R0/T8 命中 R8 的 expert25/26。第一跳由核 A 处理，expert26 的接收槽位可能由核 B 监听。
两核需要关联同一 Token，但不能各自分配编号或竞争覆盖同一份成员记录。

**需要确认：**

- 每行 assistEx 和 relay 记录的字段/dtype/有效长度是什么；如何在两个元数据 tensor 中直接定位监听表？
- 需要远端返回的 group 按 forwardTokenIdx 索引后，纯本地 group 的记录存放在哪里、如何被本地行引用？
- 本地多 expert 是按原始 k 保存 localRow，还是另一种行引用；每个字段由谁写，何时补齐及发布？
- 远端等待集合如何记录发送卡/k，并排除本地直接读取的行；memberMask 保留完整贡献集合。
- Source 除 sourceMask 外如何保留数值方案需要的 DIRECT/partial 类型信息，且不在 Combine 重建路由？

**讨论起点：** 用随包返回引用及稳定输出行建立关联；按成员独占字段或明确补齐协议避免多写入者。
Plan 只保存 Combine 所需的稳定信息，不复制整个通信包，也不指向可回收 UB/descriptor。
字段精简须保留从任一 expert 行到其 source/group 的完整路径。

**关闭条件：** 元数据布局、索引域与容量公式、逐字段 owner、跨核补齐及 Dispatch 完成条件。
ExpertOut 行域与 R10 一致，source 的加权责任与 R08 一致。

## R05

**议题：Dispatch 分核、原子与 UB/Event 资源**

**仍未明确：** 同组核职责、先 cumsum、按 M 均分、公共转发包、一次原子取号和下发后剔除均已确定，
具体核数、目标 API、表的存放方式与在途资源模型尚未闭合。

**具体风险：** M 的上界与 X 不同。R=128、BS=256、L=K=32 时，
`M<=1048576`；若用 16 核，两张 uint32 表按上界共需 512KiB/核，还未计发送 UB 或 Flag scratch。
这只是最大表长估算，尚未证明目标 UB 可容纳，不能套用 2048/16 示例中的 1024B。

**需要确认：**

- 源端数据发送与 cumsum/监听核分别多少核，全核 cnt 启用条件和跨阶段同步参与集合是什么？
- tokenPos 使用 slot 编号还是其他相对位置；最大表长放 UB、GM 或分批加载？不能在容量不足时丢弃监听项。
- CANN 9.1/A5 使用哪个原子取号接口，返回旧值还是新值，计数器如何对齐、初始化并对各核可见？
- 多少路发送 UB/Event；如何检测完成而不让公共监听路径阻塞在一个忙 Event 上？
- 部分发送的 next-entry、已分配编号和公共副本在哪里保存；最后一个 MTE3 使用者如何释放共享 UB？
- 一个目标仍未 ready 或发送资源暂满时，如何继续其他 Token，并保证本卡转发所依赖的接收任务可推进？

**讨论起点：** 每个第一跳 owner 保存一次取号结果和独立进度，公共 UB 在全部发送完成前只读；
把“逻辑上允许并行”转为具体的 buffer/Event 生命周期。
位置相对编码可用 uint32，但恢复字节地址使用足够宽的运算，并对允许的 BS/H 范围核算容量。

**实现核对：** 当前 ReduceGrad 使用 `AscendC::AtomicAdd(status_,0)` 读状态，
但加零不能区分接口返回旧值还是新值，不能证明加一取号语义。
需要目标版本的头文件/编译证据；先原子加一再普通读取共享值不能给当前核分配唯一编号。
参考依据及未覆盖范围见文末，不把 SetAtomicAdd 的累加写入等同于原子取号。

**关闭条件：** 分核时序、资源尺寸、无重复取号的状态转换、共享 UB 最后使用者判定；
目标 API 核对与设备重叠/前进性验证分别记录，未验证性能不作为已确认收益。

## R06

**议题：Flag 清理时机与顺序**

**仍待选择：** 原先“核末统一清理”和“Token/批次发送后用 PIPE_MTE3 排序清理”都保留，
尚未选定执行粒度。PIPE_MTE3 的前后序语义已确认，不重新作为选择题。

**具体场景：** 同一核已下发 Token A 的多笔转发，Token B 仍在监听；
此时清理 A 的接收 Flag 不应等待 B 到达，也不能清掉转发到本卡后仍待消费的目标槽位。

**需要确认：**

- 采用核末集中清理，还是逐 Token/小批次清理；清理任务是否在剔除 pendingIdx 前一起下发？
- 核末方案如何确保依赖均结束，随发送方案如何组织 PIPE_MTE3 和零值 UB，减少不必要排序？
- 清理分散的 tokenPos 时如何保持正确对齐和范围，避免覆盖相邻核或尚未消费的记录？
- Dispatch Flag、Combine member/partial Flag 的清理责任分别是什么，跨轮复用与 R11 如何衔接？

**讨论起点：** 两种时机都只清理本核已消费记录，通过 MTE3 写零并由核末 PIPE_ALL 覆盖完成。
PIPE_MTE3 约束该核所有前序 MTE3，不能只计算为当前 Token 的屏障，
也不能当作 Scalar 可提前覆盖发送 UB 的证明。

**关闭条件：** 选定一种首版策略，给出发送、清理、剔除、UB 释放及核退出顺序，
并说明分区跨核、同卡转发和连续轮次下的所有权。

## R07

**议题：完整 Workspace 与超限轮次处理**

**已确定边界：** 中转结果区为七分区共 `7*BS*K`，X>BS 由 Combine 报错。
这里不再讨论无约束路由的全 source/token 结果区预留，也不重新选择压缩或循环槽位。

**仍未明确：** 157.5MiB 只包含一个假设格式下的中转结果区；
Dispatch 接收区、持久表、其他结果区和同时存活的轮次仍未形成完整预算。
第 BS+1 次取号发生在 Dispatch，而报错责任放在后续 Combine，失败轮次的执行链尚未闭合。

**具体场景：** BS=256，某个核取到编号 256。此时它不能写入仅有 256 行的监听表，
但其他核/Rank 可能仍在等待它负责的 Dispatch expert 输入。
只在本核停止，或继续携带越界编号进入 Combine，均不能形成可执行协议。

**需要确认：**

- 各区域的 base/offset、slot stride、容量和同时存活区间是什么；现有 peer window 是否足够？
- Dispatch 超限后保留完整小元数据，还是有限表加可信超限标记；如何避免表越界且不让其余 Dispatch 等待悬空？
- Combine 在哪个阶段获得可靠 X/超限状态并协调各 Rank 拒绝，确保尚未开始中转结果写入？
- 采用什么错误码/状态回传、谁发布首错、等待者如何退出，失败轮次输出与窗口如何处置？

**讨论起点：** 配置时检查实际物理字节容量，Dispatch 记录运行时 X 与超限状态，
Combine 通信前完成一致的准入判断。一次调用/阶段的协调不属于逐 Token 控制包。
不得取模、覆盖有效行或把 X 截断为 BS；不能让一个 Rank 报错、其他 Rank 仍写入或无限等待。

**容量核对提示：** 若采用主设计中的 112B 尾部候选，且与 FP16/BF16 的 H=7168 hidden
一起按每块 480B 有效数据打包，则 T=`ceil((14336+112)/480)*512=15872B`。
R=128、L=3、BS=256 的 Dispatch 数据槽位区域单独就有
`128*3*256*15872=1560281088B=1488MiB`，不含状态等其他区域。
此数依赖该尾部与打包假设，不是最终分配结论，说明总预算不能只看 Combine 的 157.5MiB。

**关闭条件：** 一份完整区域预算与生命周期表，以及从首次超限到各 Rank 退出的可执行状态流程。
X=BS 接受、X=BS+1 拒绝，Dispatch/Combine 均无越界或遗漏等待。

## R08

**议题：加权位置、结果类型与数值格式**

**已确定边界：** Dispatch 包携带各 route 的 weight，公共终端包保留原列表，接收端匹配自己的值。
每条贡献必须在合并前加权且只加权一次；group 和 source 仍按已有 k 顺序消费。

**仍未明确：** Weight 在哪里应用、保存到哪里，以及 source 如何识别已经加权的 partial。
固定顺序也会因两级分组改变浮点括号，通信 dtype 与累加 dtype 尚未确认。

**具体场景：** k0/k3 形成一个 partial，k1/k2 为 DIRECT，sourceMask 只保留 k0/k1/k2。
它不能单独告诉 source：k0 是已经加权的组内和，不能再乘 w0，而 k1/k2 仍需各自加权。

**需要确认：**

- 由 relay 保存原始 weight 并统一加权，还是 expert 卡发送前加权？调用方已加权 expertOut 是否受支持？
- Source 在 plan 中保存何种 DIRECT/partial 类型信息；relay 本地行和远端结果的加权责任如何一致？
- Weight 是否为 Dispatch 时的固定快照，dtype 是否 FP32，无 weight 的语义是什么，是否允许之后更新？
- Hidden/scale 转发、expert 输出、乘法、group 累加、partial 通信、source 累加及最终输出分别采用什么 dtype/量化？
- 确定性、相对/绝对误差、高精度参考及 NaN/Inf/强抵消/零 weight 等数值规则是什么？

**讨论起点：** 延续 relay 持久保存权重并统一加权的候选，source 显式识别 partial；
所有 route 保持完整传输，零 weight 不自行省略 ready 或贡献。
先确定非量化数值契约，再决定阶段一量化范围；不因普通 Memory 支持某格式就认为新协议支持。

**关闭条件：** 路径加权责任表、source 结果类型编码、分阶段 dtype/cast/scale 表和明确数值验收标准。
权重传递方式已确定，不再把 Combine 时另发一轮 weight 作为默认缺失项。

## R09

**议题：Combine 调度与 H 分块归约**

**仍未明确：** Combine 需要 expert 发送、group 监听/归约、partial 发送、source 归约四类职责。
Dispatch 的三角色同核方案并未同时确定 Combine 的分核与流水方式。

**具体场景：** 某卡 M=0，没有 expert 行要发，但自己的 BS 个输入仍需其他卡返回结果；
另一个 relay 等待一个慢成员时，不应占尽发送/累加资源使该成员无法继续。

**需要确认：**

- 四类职责使用固定核组还是同核轮转；一个 kernel 内推进还是多个注册 kernel 阶段？
- Group/source owner 如何分配，H 按整行、tile 或 batch 处理，尾部如何界定？
- 等待下一 k 时累加器驻留 UB、存入 GM scratch，还是等成员全部到齐后开始归约？
- 如何保存 next-k、H tile、累加器和发送进度，避免重复累加或慢成员阻塞无关任务？
- 同 group 多 H tile 的 owner、partial 发布和整组完成如何协调；空职责核参加哪些同步？
- Expert 发送、partial 发布和 source 输出各何时完成，全部对外责任何时结束？

**讨论起点：** 保持每个结果元素的 k 消费顺序，以明确 owner 的工作单元组织任务。
先给出没有循环等待的职责依赖，再比较整组就绪与增量归约；不能用全局阻塞等待掩盖缺失生产者。
既有模型 stage barrier 与内部调度分别处理，不改变注册 kernel 的 Host 启动规则。

**关闭条件：** 核角色/阶段图、H 分块及累加状态、每类资源的完成/释放条件，
覆盖零 expert 行、零 group、偏斜和慢成员的完整推进路径。

## R10

**议题：双入口的具体 ABI 与 ExpertOut 契约**

**已确定边界：** 原 Memory 和去重入口分开，去重实现独立 .h，不满足条件报错，不兼容回退。
所有 Rank 的 BS 相同作为调用契约，不新增专门的跨 Rank BS 扫描或交换校验流程。

**仍未明确：** API 名称/参数/返回元数据/容量查询尚未固定，expert 计算到 Combine 的行域也未约定。
需要区分原入口中保留的参数与新入口实际支持的语义。

**需要确认：**

- Dispatch/Combine 如何输入输出 assistEx、forwardPlan、weight、count、状态和 workspace？
  元数据 tensor 的逻辑约定不等同于已经确定函数签名或 plan handle。
- ExpertOut 是否必须保持 Dispatch 行序和有效行数；允许重排/padding 时谁提供明确映射？
- 输入、expert 输出和最终输出的 H/stride/dtype/连续性是否必须一致，哪些由 plan 固定，哪些由调用参数提供？
- BS/H 的允许范围、BS=0、M=0 时空指针、输出别名等边界如何定义？
- 哪些参数需跨 Rank 共同约定，如何在现有调用模型内保证，而不把已保证的 BS/top-k 重新变成额外扫描？
- Mask、共享专家、TP 或其他未支持参数接受哪些空值/默认值；不合要求如何在可判断阶段拒绝？

**讨论起点：** 首版以 ExpertOut 保持 Dispatch 行序为候选，按有效 M 排除 padding；
不能从 Token 数值恢复身份。先固化必需的逻辑参数和生命周期，再确定 public ABI。

**关闭条件：** Dispatch/Combine 签名、参数支持矩阵、有效长度/行序契约、workspace 查询和错误返回方式。
不修改原 Memory ABI 来迁就未完成的新接口。

## R11

**议题：Plan、执行轮次与异步生命周期**

**仍未明确：** Dispatch plan 是持久路由信息，普通 Memory Combine 每次 launch 会取得新的 magic。
保存同一 plan 不自动意味着允许任意重复调用、多 stream 并发或窗口提前复用。

**具体场景：** Combine A 的 source 输出已完成，但同卡还有 partial 在途；
此时提交 Combine B、重置原子计数器或复用同一 peer window，可能覆盖 A 仍引用的数据。
另外，两个调用复用相同 plan 时，不能将上一轮的 ready 当作本轮结果。

**需要确认：**

- Plan 的 communicator、布局版本、Dispatch 身份与本次 Combine invocation/epoch 如何绑定？
- 同一 plan 是否允许顺序重复 Combine，是否可更换 expertOut/weight；失败后是否允许重试？
- 阶段一是否允许多 stream、多个未完成 plan、多个 microbatch，哪些资源需要隔离？
- Host 返回、stream event、所有对外发送结束及远端消费结束，分别释放哪些 buffer/plan？
- Ping-pong 复用、计数器重置、旧标志失效及各 Rank 调用次序的一致性如何保证？

**讨论起点：** 分离不可变 plan 身份与可变通信轮次，沿用 TileXRCommNextMagic；
局部 Flag 清理不能代替轮次绑定。首先限定可支持的调用并发范围，再设计对应资源释放。

**关闭条件：** API 到设备完成的生命周期表、跨轮状态绑定和支持的重复/并发规则；
说明空职责与失败轮次也能结束，不以源端 yOut 完成代替整卡全部责任完成。

## R12

**议题：正确性与性能验收标准**

**仍未明确：** 路由例子已能检查地址与数量，但数值误差阈值、可执行正确性矩阵和性能目标尚未确定。
此项是后续实现的验收设计，本轮文档更新不要求运行 NPU 测试。

**需要确认：**

- 使用何种独立 oracle 验证原始 route、q、group/source 选择和每条贡献恰好一次；
  expert 输出应能区分 source/token/k/expert，不能全部原样返回 hidden 掩盖错配。
- Combine-only 的合法 plan 如何生成，联合 Dispatch-expert-Combine 如何独立核对两端，避免同错互相抵消？
- 哪些配置和路由构成首版必须通过的正确性矩阵，数值条件按 R08 如何验收？
- 哪些规模/BS/H/K/路由分布是性能目标，允许何种退化；与普通 Memory 比较时如何固定相同数值契约？

**最低覆盖方向：**

| 范围 | 后续应验证的行为 |
| --- | --- |
| 路由 | 同卡多 expert、relay 多本地行、单远端卡多 expert、多个 Server、交错 k；D03 的原 T8 例子见主设计 |
| 边界 | K=1/32、每卡最多 32 expert、零 cnt/整卡 M=0、H 尾部及共同支持的 BS/H |
| 中转容量 | X=0/BS-1/BS/BS+1、X<=BS 且 Y>BS、不同 source 相同 TokenId、跨多发送卡合计超限 |
| 并发 | 原子取号唯一且每 Token 一次、部分发送恢复、公共 UB 最后使用者、pendingIdx 末项换入不遗漏 |
| 可见性 | 新尾部完整到达、表项发布与取号区分、Flag 清理范围、MTE3 在途与接收乱序/延迟 |
| Combine | 本地贡献排除网络等待、部分成员不重复累加、DIRECT/partial 加权正确、空职责与全部对外责任完成 |
| 生命周期 | 支持范围内连续多轮、重复/并发调用、超限与失败退出、旧 ready 不被复用 |

不增加重复 top-k、invalid expert、空 route 或输入 mask 的错误输入测试；
这些已被第一轮安全输入和功能范围排除。BS=0 只有在 R10 接纳后才纳入行为测试。
Host/reference、编译检查与目标硬件结果分别报告，不以匹配源码文本的 guard 代替行为测试。

**性能口径：** 同时统计原始贡献数、跨 Server partial 数和真实通信字节。
例如两份 FP16/BF16 结果合成一份 FP32 partial，仅 hidden 字节可能相等，不能只以行数减少证明收益。
比较须包含完整 Token 尾部、Flag/填充、Server 内转发、weight/plan、原子计数、监听及归约开销。

每阶段报告 backend、native status、kernel version、算法/通信数据量、可读 latency、适用 bandwidth，
跨所有 Rank 的 median/min/max。诊断插桩与性能基线分开；
复用已有匹配 HCCL Test 资格记录。MoonEP 模型/replay 保留框架 profiler 和 Dispatch/Combine stage barrier，
使用允许的 grouped mode，默认 group_credit。

**关闭条件：** 确定参考计算、数值判据、必测矩阵、性能基线和目标场景。
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

目标 API 的证据边界：

- [ReduceGrad DeviceStatus](../../src/moonep/reduce_grad/kernels/tilexr_moonep_reduce_grad_kernel.cpp)
  有 AtomicAdd 加零读状态的源码用法，未提供本设计加一取号在 CANN 9.1/A5 上的验证。
- 官方 AtomicAdd 检索按 9.1 过滤无结果；此前取得的
  [9.2.0-beta.1 条目](https://www.hiascend.com/document/detail/zh/CANNCommunityEdition/920beta1/API/ascendcopapi/docs/zh/api/SIMD-API/%E5%9F%BA%E7%A1%80API/%E5%8E%9F%E5%AD%90%E6%93%8D%E4%BD%9C/AtomicAdd.md)
  仅有摘要/标题，不能替代目标版本签名、返回值和 SoC 支持证明。
- PIPE_MTE3 的前后序语义已在讨论中确认；此前取得的
  [PipeBarrier 条目](https://www.hiascend.com/document/detail/zh/canncommercial/80RC22/apiref/opdevgapi/atlasascendc_api_07_0264.html)
  属于 8.0.RC2.2 且未取得完整正文，不能证明目标 9.1 的 Event 配置或实际重叠性能。

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

Relay 边界转 [R01](#r01)，源端 owner 与 q/cnt 算法转 [R02](#r02)。

## D05

同组核与先 cumsum 后均分已合入主设计；具体分核、同步和资源转 [R05](#r05)。

## D06

原子取号方向已合入主设计；纯本地 group 转 [R01](#r01)，持久关联转 [R04](#r04)，原子 API 转 [R05](#r05)，超限转 [R07](#r07)。

## D07

固定中转容量已合入主设计；完整预算与超限转 [R07](#r07)，清理转 [R06](#r06)，跨轮复用转 [R11](#r11)。

## D08

双入口及持久元数据原则已合入主设计；布局转 [R04](#r04)，具体 ABI 转 [R10](#r10)，生命周期转 [R11](#r11)。

## D09

Weight 传递方向已合入主设计；加权位置、类型与数值格式转 [R08](#r08)。

## D10

拒绝原则和 X 阈值已合入主设计；错误退出转 [R07](#r07)，接口检查转 [R10](#r10)，验收转 [R12](#r12)。

## D11

双表、共享发送副本和下发后剔除已合入主设计；执行资源转 [R05](#r05)，Flag 清理转 [R06](#r06)。
