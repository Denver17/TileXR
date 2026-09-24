# EP Memory Server Dedup 关闭去重特性设计与实现

## 1. 文档状态

本文用于记录“关闭去重”特性的预期、约束、方案和优化路径，随后续讨论更新。
仅用户明确提出或确认的内容记为已确认；未确定部分直接标为待定，不将代码现状、历史方案或建议视为已确认方案。
§8 保留固定编号，仅将去重改为非去重后产生的功能差异、性能影响和未闭合依赖列为待讨论项。可以直接沿用的机制按原实现保留；由已确认要求直接推出的适配及常规验证由实现阶段落实，不再逐项请求确认。当前已列出的功能方案问题均已明确；16—18的沿用/必要适配及22的验证落实不再单独确认。§9按最新的优先复用要求梳理函数级改动，已获用户授权作为实现依据，代码和验证状态随实施更新。

## 2. 特性预期

在当前 Dispatch / Combine Server Dedup 版本基础上增加去重开关。关闭去重后走新的代码分支，简化为了去重而增加的额外逻辑，保留其他特性。

已确认的模块方向见 §4，原编号对应结论见 §8；已授权按§9的复用边界开始实施；验证结果另行记录。

### 2.1 流程与差异概览

| 流程位置 | 开启去重的当前行为 | 关闭去重的方向 | 讨论边界 |
| --- | --- | --- | --- |
| Dispatch Count/cumsum | 统计全部原始专家路由 | 沿用 | 计数语义及输出行序不再讨论 |
| Dispatch Send | 同 Token 的远端同 Server 多路由可合并为首跳包；源 Token 分核 | 按目标 Server 筛选路由、均衡分核，每条路由直发；三字段包尾 | 04—08、11已确认；资源预算按已确认20落实 |
| Dispatch Recv | 拷贝专家输入，首跳生成转发表并转发 | 只拷贝直发数据、量化 scale 和表2 | 09、12已确认；探测、pending及预取机制沿用 |
| 权重及无效表 | 表3供M4使用，表5供M5使用 | 表3无效；Dispatch原样复制权重到表5，供M6使用 | 10、19、24已确认；表5仍为有效输入/输出 |
| Combine M1/M2/M4 | 准备转发区、Server内同步、局部加权合并 | 跳过相关数据和控制逻辑 | 13、15已确认；关闭路径M6不继承M4的资源或Event |
| Combine M3 | 筛选type1并返回未加权专家输出 | 全部有效行直接返回 | 14已确认，原发送调度沿用 |
| Combine最终接收聚合 | M5接收mask选中的Direct/partial槽位 | 新模块M6等待并处理全部K份，包含自身UB/Event初始化及收尾 | 15已确认；沿用M5的pending和环形预取机制，资源预算20、数值标准21保持有效 |


## 3. 已确认约束

1. 之前的 Dispatch 去重开关逻辑不必沿用，那是临时版本，后续要清理。本特性一切以当前讨论方案为准。
2. 是否开启去重不在运行时修改。通过宏 `TILEXR_EP_SERVER_DEDUP_ENABLE` 定义开关值，代码使用普通 `if/else` 判断该值，选择不同的函数或代码段；不要求使用预处理条件做编译隔离。构建入口使用 `build_all.sh dedup=0|1` 设置该值，默认 `dedup=1`。
3. 开启或关闭去重时，所有接口保持不变；关闭去重场景下，部分接口不会被 kernel 内逻辑使用。
4. 优先减少去重/非去重代码差异。若函数或模块绝大部分逻辑兼容，则复用主体，仅在有差异的部分使用普通 `if/else`。仅为任务组织或资源生命周期确有明显差异的部分增加独立入口；新增M6不意味着复制整个M5实现。

## 4. 方案

### 4.1 固定 enable 开关与构建接入

已确认（01—03）：

- 构建参数为 `dedup=0|1`，默认1；沿用宏名 `TILEXR_EP_SERVER_DEDUP_ENABLE`，由构建系统统一赋值并校验只允许0或1，移除旧无条件定义。沿用名称不代表沿用旧关闭逻辑。
- 普通 `if/else` 按差异所在位置设置：大部分兼容的函数共用主体，只分支处理差异。保留新增Dispatch发送调度函数与独立M6模块的方向；Dispatch接收、Combine M3及M5/M6可兼容的处理逻辑优先复用，不再要求分别新建整套函数。布局计算和Host校验使用同一固定开关值；具体拆分参考§9。
- 通过 `build_all → workflow → CMake` 向 Host、Dispatch/Combine kernel 和布局相关消费者传入同一宏值；构建缓存按 dedup 与 breakdown 两个维度区分。
- 安装和同步继续使用现有单套有效产物流程，manifest 记录 dedup 值、协议版本及产物校验信息；切换模式时整体替换配套产物。沿用现有多 Rank 验证框架检查配置及产物一致性，不增加 kernel 内模式协商。

### 4.2 Dispatch 关闭去重分支

#### 4.2.1 数据发送模块

已确认修改方向：

- 关闭 Server 去重时，数据发送逻辑需要大改，新增一个发送函数。
- 新发送函数不再采用按 Token 分核的方式，而是采用 Combine 中 Send 核的分核逻辑。
- 每次从专家路由表中筛选发往目标 Server 的路由任务，按 Send 核间均衡的逻辑分配，各核选择其中一部分执行发送。
- Server 遍历完整沿用 Combine 的双半区逻辑：按 Server 编号划分两个半区，先在本 Server 所属半区轮转，再在另一半区轮转；复用 `ServerDedupHalfOrder` 的起点和取模规则，两半区不等长时各按自身长度计算。
- 核间任务分配沿用 `ServerDedupFirstOwned`，`owner` 按每个 Server 的任务数累加，跨 Server 和半区切换均不重置；使用 Dispatch 自己的 Send 核数配置。
- 同一个 Token 的 top-k 专家路由不再由单个核统一负责，可分配到不同 Send 核。
- 数据发送时不再区分待转发 Token 和 Direct Token，因此不再先进行用于去重分类的 server mask 表筛选。
- 将 `expertIds` 视为长度 `bs*K` 的原始路由序列，按目标 Server 的专家编号区间筛选并保留索引 i；用 `tokenId=i/K`、`k=i%K` 恢复路由身份。首版按原始前缀中同专家出现次数计算本核负责路由的 q，不引入共享原子计数。
- 数据尾部不再拼接复杂的转发表，只包含 `srcRank`、`tokenId`、`topKIndex` 三个 uint32 字段，不再传输路由 `weight`。`topKIndex` 为原始槽位 k，取值 `[0,K)`；公开接口的总数参数 `topK` 不改名。
- 包尾从隐藏数据及可选量化 scale 之后的首个32B对齐位置开始，有效信息12B，占用32B对齐块，不保留转发表空间。padding 不写入、不读取、不处理，不要求置零。
- 保留480B数据加32B Flag协议，按新 payload 计算 `blocks=ceil(payloadBytes/480)`、`tokenStride=blocks*512`；Host 和收发 kernel 共用布局计算，保持通信区起始偏移。
- Dispatch 将源卡输入 `expertScales[bs,topK]` 原样复制到独立输出表5（`expertScalesOut`），供 Combine 的 `expertScales` 参数接入。所有FP32权重保持原值，不将任何槽位改写为 `1.0`；输入本身为 `1.0` 时也原样保留。包尾仍不传输权重。

取消 server mask 表筛选针对去重分类步骤；上一条已确认的按目标 Server 筛选路由任务、在 Send 核间均衡分配的逻辑仍保留。

这里的发送任务指原始 Token 的一条专家路由，不是把某个专家的全部 Token 固定交给一个核。

代码参考：当前 Combine 的 `ReturnDirectTokens` / `SelectDirectServer`
（[kernel](../../src/ep/kernels/tilexr_ep_combine_memory_server_dedup_kernel.h)），
以及 `ServerDedupHalfOrder` / `ServerDedupFirstOwned`
（[分配助手](../../src/ep/kernels/tilexr_ep_server_dedup_select.h)）。
上述遍历与分核逻辑已确认复用；引用不代表同时确认复用 Combine 的其他实现细节。

已确认（11）：首版逐路由加载 Token，并在量化场景逐路由执行现有 MXFP8 量化，不引入跨核缓存；保留 scale 输出和发送槽位流水，按新包长度重算 UB 与 lane 数，删除分类及 FirstHop/转发包尾专用缓冲。重复加载/量化留作后续优化。

表5写出和Combine接入已明确，见10、24；具体拷贝分工按常规实现处理，保证完整覆盖且不重叠写入，不另列讨论项。

#### 4.2.2 Count / cumsum 模块

已确认（06）：首版保留当前统计语义、输出格式、参与核规则、阈值及完成同步，不另建计数协议；实现时检查新 Send/Recv 与 Count scratch 复用的生命周期。

#### 4.2.3 数据接收模块

已确认修改方向：

- 关闭去重时，接收核只需处理直发 Token，不会有需要处理的转发 Token。
- 检测到 Token 到达后，将 Token 数据直接拷贝到本卡输出。
- 剥离包尾的 `srcRank`、`tokenId`、`topKIndex`，写入表2；接收侧不处理路由 `weight`，也不读取或处理 padding。
- 无需执行 Token 转发，也无需填写转发表。
- Dispatch 不必记录 Token Type（表1）；表2全部为直发 Token 的查表信息，按 `[srcRank,tokenId,topKIndex]` 三个32位字段保存，行距12B，仅写有效行。
- 保留 localExpert/source 前缀决定的固定输出行和 q 映射；乱序到达只影响完成先后，表2与 `expandXOut` 按原始行号一一对应。五表有效性见 §4.4。

已确认（12）：保留每批最多16个槽位的 Flag 探测、pending 列表和有限 lane 预取，按新布局判断完整到达；本核任务完成且相关搬运退休后清理负责槽位的 Flag，保留清理完成屏障，不新增逐 Token 全流水屏障。

表1/3/4允许传空；表2、表5仍为有效输出，按19保留其分配及校验要求。

### 4.3 Combine 关闭去重分支

#### 4.3.1 本卡转发区准备模块（M1 中的数据拷贝）

已确认（13）：关闭路径跳过 M1 转发数据搬运、prepare-done 发布及整个 M2 Server-ready 通知/等待，去掉其专用控制 UB。保留有效行数读取、窗口选择、必要的公共初始化及最终搬运完成处理；Send 核进入 M3，Recv 核进入包含独立初始化的新模块 M6。跨算子的既有顺序保障保留。

已确认（15）：M6自行管理UB及Event，不依赖M4；资源缩减及预算原则按20执行。

#### 4.3.2 直发模块（M3）

已确认（14）：只有直发这一种 Token 类型，不读取表1、不做 type1 压缩；处理有效行区间 `[0,sendCounts[experts-1])`，按原始行号读取表2生成源卡列表。沿用按目标 Server 筛选、跨 Server owner 均衡及 M3 多 lane 搬运，返回未加权专家输出。有效行数为0只表示发送侧无任务，不跳过本卡源 Token 的结果接收。

#### 4.3.3 partial 模块（M4）

已确认：关闭去重时，Combine 接收核不需要执行 M4，直接跳过该模块。

关闭路径由M6承担接收聚合及其全部资源初始化，不再执行依赖M4的M5，见15。

#### 4.3.4 独立接收聚合模块（M6）

已确认（15）：新增M6用于关闭去重路径，可视为不依赖M4、能够独立运行的M5接收聚合模块。开启去重路径继续使用原M4/M5流程；允许为M5/M6共用逻辑做必要提取，保持开启路径行为。

- M6包含接收逻辑、UB布局与初始化、Event分配及初始化、权重加载、加权求和、Flag清理和最终收尾；自身资源生命周期完整，不依赖M4调用或其在途输出状态。
- 接收全部K份未加权专家输出，按k顺序从Dispatch输出的表5读取原始权重并加权；不读取表4，不执行mask相关逻辑。
- 沿用M5现有pending/就绪优先调度和有限槽位环形预取，不要求按TokenId串行等待或同时装下全部K份。lane上限沿用原M5，实际容量按M6布局核算。
- 探测期望值为 `K*blocks`；沿用仅清Flag、保留payload的原则。空任务及重复调用保持原有语义，M6自行保证Event配对及全部在途操作完成。
- 不使用 `partialTasks` 决定资源布局，不回收M4输出槽，也不依赖原跨M4/M5的共享输出drain。

数值计算与验收按21执行，资源预算按20执行。M6是Combine内部新模块，公开接口保持不变。

#### 4.3.5 其他模块

关闭路径跳过M1转发准备、M2和M4；Send核执行M3，Recv核执行M6。必要的公共初始化、既有调用顺序和kernel最终完成保障保留；M6独立处理其接收、计算及清理资源。

### 4.4 未使用接口的处理

关闭去重时，五表有效性已确认如下；所有接口保持不变。

| 表 | 对应数据 / 接口 | 关闭去重时的用途 |
| --- | --- | --- |
| 表1 | TokenType（`tokenTypeOut` / `tokenType`） | 无效；Dispatch 不必记录类型，Combine 不需要据此筛选 type1 |
| 表2 | DestinationIndex（`destinationIndexOut` / `assistInfoForCombine`） | 有效；全部为直发 Token 的查表信息 |
| 表3 | RelayReadIndex（`relayReadIndexOut` / `relayReadIndex`） | 无效；无需填写转发表 |
| 表4 | SourceMask（`sourceMaskOut` / `sourceMask`） | 无效 |
| 表5 | Dispatch `expertScalesOut` → Combine `expertScales` | 有效；源卡原样复制FP32 `[bs,topK]` 权重，供关闭路径M6加权，不改写为 `1.0` |

已确认（19）：关闭路径表1/3/4允许免分配、传空，也兼容继续传有效地址；Host跳过这些无效参数的非空、对齐及容量检查，kernel不访问或清零。表2、表5及其他有效参数保留校验；表5按原接口约定提供独立的有效输出及Combine输入，容量为FP32 `[bs,topK]`，持有至Combine完成。开启路径保留五表要求。

### 4.5 数据布局、资源与同步

Dispatch发送/接收流水已确认，Combine的M6沿用原M5 pending和环形预取；M6独立管理UB/Event，完整方案见15。

已确认（20）：关闭路径取消Combine转发stage区、M4专用预留及已删除的M1/M2控制UB；无效表最低容量按19处理。Dispatch通信槽位按新包长度计费，新Send按路由列表及搬运槽位核算UB，M6预算与其独立布局一致。Host/kernel使用配套预算；保持共享区基址、窗口切换和对齐规则，peer memory满足Dispatch/Combine需求的较大值，首版不主动扩展支持形状。

### 4.6 历史开关清理

已确认（02）：实现关闭路径时同步替换旧临时关闭逻辑及公开说明，保持开启路径算法；未涉及的接口保留字段不顺带删除。

## 5. 优化路径

普通实施顺序不列为需求决策。

## 6. 验证方案

开启路径回归、关闭路径正确性及边界、重复调用、Flag清理和构建组合验证由实现阶段落实，不再作为待确认方案。22保留为验证落实记录。

已确认（21）：保留输入转FP32、独立乘法、按k顺序FP32累加及最终RNE转换；关闭路径使用相同运算次序和逐步舍入的参考，在现有有限数值测试域逐bit一致，`atol=rtol=0`。开启路径沿用原参考，两模式之间不要求逐bit相等，不因模式差异放宽正确性阈值。权重由Dispatch原样复制到表5，再通过Combine现有参数交给M6，见24。

## 7. 差异对应的实现检查

以下风险已有明确处理方向，属于实现与验证检查，不重新列为待确认项。

| 差异 | 实现检查 | 已确认依据 |
| --- | --- | --- |
| 非去重没有partial权重修正 | Dispatch表5逐元素等于输入权重；不写成 `1.0`，M6读取该表加权 | 10、24 |
| 跳过M4后不能继承其Event和缓冲 | M6自管UB、Event、搬运及收尾；不依赖M4输出槽回收状态 | 15 |
| 表1/3/4无效，表2/5仍有效 | Host按模式跳过无效参数检查，保留有效参数检查 | 19 |
| 分核、包长和阶段资源改变 | Host/kernel预算匹配，新Send与M6不超UB；不读取或校验padding | 08、20 |
| 去掉M4局部和及中间舍入 | M6按关闭路径顺序计算，使用对应参考验收，不要求与开启路径逐bit相同 | 21 |

Count、q语义、输出行序、量化算法、pending调度、有限槽位预取、Flag清理原则和既有调用顺序直接沿用。具体拷贝分工、资源偏移和Event编号在实现时确定并核查，不转化为新的需求确认项。

代码依据：[Dispatch收发与q](../../src/ep/kernels/tilexr_ep_server_dedup_send.h)、[接收任务行映射](../../src/ep/kernels/tilexr_ep_server_dedup_receive_tasks.h)、[当前Combine调度](../../src/ep/kernels/tilexr_ep_combine_memory_server_dedup_kernel.h)、[供M6复用逻辑的当前M5](../../src/ep/kernels/tilexr_ep_server_dedup_source_aggregate.h)、[当前共享UB布局](../../src/ep/common/ep_memory_server_dedup_combine_config.h)、[公开接口](../../src/include/tilexr_ep.h)。M6尚未实现，以上链接仅用于核对现有实现。

## 8. 方案结论（保留原编号）

当前已列出的方案问题均已明确：01—15、19—21、24已确认；16—17直接沿用，18是必要适配，22是验证落实。原编号保留，不重新编号。
本节记录方案要求，不代表代码已实现；后续如发现新的功能冲突或风险，再按实际问题讨论，不为可直接沿用的机制增加确认项。

### 8.1 开关、构建与代码组织

#### 01：固定 enable 开关定义（已确认）

**确认范围：** 宏定义固定开关值，运行时不修改；构建参数、宏名及默认值均采用下述方案。

**已确认方案（采用参考建议）：** 使用 `build_all.sh dedup=0|1`，默认 `dedup=1`；沿用宏名 `TILEXR_EP_SERVER_DEDUP_ENABLE`，由构建系统统一赋值并校验只能为0或1，移除旧的无条件定义。普通 `if/else` 判断由该宏得到的固定布尔值，不增加运行时参数。沿用宏名仅为了减少构建与消费者改名，不沿用旧关闭路径的实现。

#### 02：新分支组织与旧逻辑清理（已确认）

**确认范围：** 普通 `if/else` 的分支组织、模块拆分复用及旧逻辑清理采用下述方案，具体函数命名由实现阶段确定。

**已确认方案（按最新复用要求更新）：** 用普通 `if/else` 判断宏定义的固定enable值，优先在差异局部进行分支，共用兼容的代码主体。Dispatch保留新增发送调度函数，Combine保留独立M6入口与资源生命周期；不再要求为非去重另写整个接收类、Combine调度类或复制M5计算主体。布局及Host校验同样局部分支。实现时同步清理旧临时关闭逻辑和说明，保持开启路径行为；不顺带删除未涉及的接口保留字段。具体提取与命名由实现阶段处理，函数级建议见§9。

#### 03：构建传递与产物一致性（已确认）

**确认范围：** 模式如何贯通 workflow、Host、两个 kernel 和布局相关消费者；如何区分安装产物、处理切换模式后的缓存，以及识别各 Rank 的模式一致性。

**已确认方案（采用参考建议）：** 通过 `build_all → workflow → CMake` 向 Host、Dispatch/Combine kernel 和依赖布局的消费者传入同一0/1定义。构建缓存按 dedup 与 breakdown 两个维度区分；安装和同步继续使用现有单套有效产物流程，在 manifest 记录 dedup 值、协议版本及产物校验信息，切换模式时整体替换配套产物。沿用现有多 Rank 验证框架检查各节点的配置及产物一致性，不增加 kernel 内模式协商。

### 8.2 Dispatch

#### 04：Server 遍历与核间均衡细节（已确认）

**确认范围：** 延续 Combine 当前的遍历与分核逻辑，包括双半区逻辑。

**已确认方案：** 沿用 Combine 的 `ServerDedupHalfOrder` 双半区遍历顺序和 `ServerDedupFirstOwned` 分配规则。设 Server 数为 S、分界为 `floor(S/2)`，先在本 Server 所属半区轮转，再在另一半区轮转；起点及取模行为直接复用现有 helper，包括两半区不等长的情况。`owner` 随每个 Server 的任务数累加，跨 Server 和半区切换均不重置。Dispatch 使用自己的 Send 核数配置，不写死为 Combine 的核数。这样既复用现有均衡逻辑，也避免多个小 Server 任务组总落在低编号核。

#### 05：路由筛选及 Token/k/q 获取（已确认）

**确认范围：** 目标 Server 路由列表如何构造，如何恢复原始 Token/k，以及是否保留按原始路由序列计算 q 的方法。多个核不能对同一专家独立从零编号并写入同一槽位。

**已确认方案（采用参考建议）：** 将 `expertIds` 看作长度 `bs*K` 的原始路由序列，按目标 Server 的专家编号区间筛选并保留原始索引 i；用 `tokenId=i/K`、`k=i%K` 恢复路由身份。首版沿用原始前缀中同专家出现次数计算 q，仅为本核负责的路由求 q，不按核局部发送次序编号。首版不引入共享原子计数；后续再评估 q 的批量预计算，以保持当前槽位映射和行序。

#### 06：Count/cumsum 修改范围（已确认）

**确认范围：** 是否保留当前统计、前缀输出及分核方式；若调整，明确调整范围。当前计数已经覆盖全部原始专家路由。

**已确认方案（采用参考建议）：** 首版保留当前 Count/cumsum 的统计语义、输出格式、参与核规则、阈值及完成同步。它已统计全部原始专家路由，无需因取消去重另建计数协议；只检查新 Send/Recv 与 Count scratch 复用的生命周期是否仍正确。

#### 07：包尾 `topK` 字段含义（已确认）

**确认范围：** 是否明确为本条路由的原始 top-k 槽位索引 k，而不是总专家数 K；文档和代码是否相应改名。

**已确认方案（采用参考建议）：** 明确该字段是原始 top-k 槽位索引 k，取值 `[0,K)`，内部变量及包尾说明命名为 `topKIndex`；总数 K 继续由现有参数提供。公开函数签名及其 `topK` 参数名不改，避免把路由索引与总数混淆。

#### 08：三字段包尾及通信布局（已确认）

**确认范围：** `srcRank/tokenid/topK` 的类型、字段顺序、对齐、padding；是否保留480B数据加32B Flag的分块协议，以及新包尾的偏移和槽位步长如何计算。

**已确认方案（参考建议经修正）：** 字段顺序为 `uint32 srcRank、uint32 tokenId、uint32 topKIndex`，有效信息12B，包尾占用32B对齐块。padding 不写入、不读取、不作处理，无需置零；不为 padding 增加初始化、解析或校验操作。包尾从隐藏数据及可选量化 scale 之后的首个32B对齐位置开始，不保留转发表空间。继续采用480B数据加32B Flag的分块协议，按新 payload 长度计算 `blocks=ceil(payloadBytes/480)`、`tokenStride=blocks*512`；Host 和收发 kernel 共用布局计算。先保留现有通信区起始偏移，避免同步改动控制区协议。

#### 09：接收输出行与表2布局（已确认）

**确认范围：** 是否沿用 Count/cumsum 决定的固定输出行，以及表2的 `[srcRank,tokenId,k]` 三列布局；乱序到达如何映射到输出行，保证数据、表2和专家计数对应。

**已确认方案（采用参考建议）：** 保持当前 localExpert/source 前缀决定的固定输出行和 q 映射，接收乱序仅影响完成先后，不改变最终行号。表2保持每行三个32位字段 `[srcRank,tokenId,topKIndex]`、行距12B，和 `expandXOut` 一一对应；仅有效行需要写入。

#### 10：表5原样拷贝（已确认）

**已确认方案：** 非去重路径仍由Dispatch输出表5：将输入 `expertScales[bs,K]` 原样复制到独立的 `expertScalesOut`。所有权重保持原值，不执行去重路径把第一跳首k权重改成FP32 `1.0` 的逻辑；原始权重本身为 `1.0` 时保持不变。Combine继续通过原 `expertScales` 参数接收这张表，供M6使用。

拷贝由源卡本地完成，与路由包尾无关；包尾仍只保留三项索引信息。具体拷贝分工属于实现细节，确保每个元素正确写入及完成顺序即可，不新增需求讨论。

#### 11：发送 payload 与量化流水（已确认）

**确认范围：** 新分核下是否逐路由加载/量化，或要求复用同 Token 数据；UB、发送槽位及 Event 如何组织。已有量化 scale 的传输和输出属于需保留的特性。

**已确认方案（采用参考建议）：** 首版逐路由加载 Token，量化场景逐路由执行现有量化流程，不引入跨核 Token 缓存。保留现有 MXFP8 算法、scale 输出及发送槽位流水，按新包长度重算 UB 和可用 lane 数；删除分类 mask、FirstHop 和转发包尾构造缓冲。重复加载/量化作为后续独立优化点，先保证路由分核正确。

#### 12：接收检测、搬运与清理（已确认）

**确认范围：** 是否沿用当前批量 Flag 探测、pending 任务及预取流水；新包格式下何时清 Flag、如何完成收尾，保证重复调用。

**已确认方案（采用参考建议）：** 保留当前每批最多16个槽位的 Flag 探测、pending 列表和有限 lane 预取，使用新布局计算完整到达条件。完成某 Token 的接收后只写 payload、可选量化 scale 和表2。继续在本核全部任务完成并退休相关搬运后清理所负责槽位的 Flag，清 Flag 后保留必要的完成屏障；不新增逐 Token 的全流水屏障。

### 8.3 Combine

#### 13：M1 控制部分与 M2（已确认）

**确认范围：** 在已确认不搬运转发 Token 的基础上，是否同时删除 prepare-done 发布、Server-ready 通知与等待；哪些公共初始化仍需保留或迁移。

**已确认方案（采用参考建议）：** 关闭路径同时跳过 M1 转发数据搬运、prepare-done 发布以及整个 M2 Server-ready 通知/等待；去掉它们专用的控制 UB。Send核直接进入M3，Recv核进入自带UB/Event初始化的新模块M6。保留有效行数读取、通信窗口选择、必要的公共初始化和最终搬运完成处理，不把跨算子的既有顺序保障一起删除。

#### 14：M3 查表与发送组织（已确认）

**确认范围：** 去掉 type1 筛选后，如何从全部有效行提取目标源卡；是否保留现有按 Server 均衡分核及发送流水。有效行数与容量不能混用。

**已确认方案（采用参考建议）：** 直接使用有效行区间 `[0,sendCounts[experts-1])`，按原始行号读取表2并生成源卡列表，不读取表1、不做 type1 压缩。沿用当前按目标 Server 筛选、跨 Server owner 均衡和 M3 多 lane 搬运；返回未加权的专家输出。有效行数为0时仅发送侧无任务，不能让负责源 Token 接收的核退出。

#### 15：新增独立接收聚合模块M6（已确认）

**已确认方案：** 非去重路径新增M6，可视为能够独立执行、不依赖M4的M5。M6包含原M5的接收与加权求和逻辑，以及自身的UB初始化、Event分配/初始化和完整收尾；开启去重路径继续使用原M4/M5流程；允许为M5/M6共用逻辑做必要提取，保持开启路径行为。

M6职责：

1. 独立计算并初始化所需UB布局，包含pending、表5权重、Flag探测、FP32计算区、输入与输出槽位；自行分配、初始化并配对退休Event。
2. 沿用M5的pending调度、环形预取和输入/输出复用保护；按 `K*blocks` 判断到齐，处理全部k，不加载或判断mask。
3. 从Dispatch输出的表5读取原始FP32权重，按k顺序加权聚合，并执行最终dtype转换与写回。
4. 完成自身Flag清理、输入/输出搬运及退出处理；不依赖M4执行或其Event置位，不继承M4在途输出，不使用 `partialTasks`、M4输出回收掩码及原共享输出drain。
5. Host按M6独立布局检查资源；空任务核及重复调用仍保持Event和DMA生命周期正确。

M6是当前Combine内部的新模块，独立性指不依赖M4的资源状态，不新增公开接口或要求新增独立kernel调用。M6可拥有独立初始化和收尾入口，同时与M5共用接收、预取和计算函数；具体函数拆分及Event编号在实现阶段确定。

#### 16：M6沿用M5的Token处理顺序（沿用，不再讨论）

直接沿用 pending/就绪优先调度和按原 TokenId 写回的机制。关闭去重不要求改成按 TokenId 串行等待；每个 Token 内仍按 k 递增处理。删除 mask 的差异见18。

#### 17：M6沿用M5的副本搬入方式（沿用，不再讨论）

直接沿用有限输入槽位的环形预取、边搬边算及输入/输出复用保护，不要求全部 K 份驻留UB。保持现有M5输入最多7个、输出最多2个的上限；15、20只核算新布局下的实际容量与Event，不重新选择预取策略。

#### 18：M6到达检测及生命周期（必要适配，不再讨论）

删除 mask 的加载、检查和筛选，将就绪条件从 `popcount(mask)*blocks` 调整为 `K*blocks`，按顺序消费全部K份，零权重不省略返回数据。pending/Probe框架和仅清32B Flag、不覆盖payload的原则沿用；本核所有Token确认到齐后清理其负责范围，最终完成清理与输出DMA。本卡专家有效行数为0不影响接收本卡原始Token结果；空任务核的Event配对按15落实。这些是既定全部直发方案的必需适配，不是新的功能选择。

### 8.4 接口、资源与验收

#### 19：无效表参数的调用约定（已确认）

**已确认方案：** 关闭路径表1/3/4允许免分配、传空，也兼容调用方继续传有效地址；Host跳过这些无效参数的非空、对齐及容量检查，kernel不访问也不清零。表2、表5和其他有效参数保留校验；开启路径保留原有五表要求，公开签名与参数顺序不变。

表5继续使用原有输出/输入约定：Dispatch `expertScalesOut` 是独立的有效FP32 `[bs,K]` 输出，原样复制输入权重；Combine `expertScales` 接入该输出并保持至执行完成。表5不属于允许省略的无效表，保留原非空、对齐及容量检查。

#### 20：资源缩减及 Host 容量检查（已确认）

**适用范围：** 同步缩减失效资源并重算Host/kernel预算；实现时检查新布局下原支持形状的UB与lane容量，接收聚合使用15已确认的M6独立布局。

**已确认方案（采用参考建议）：** 首版同步取消关闭路径资源计算中的 Combine 转发stage区和M4专用预留，无效表最低容量按19处理；已确认删除的M1/M2控制UB不再计入预算。Dispatch通信槽位按新包长度计费，新Send按路由列表及搬运槽位计算UB，M6预算与其独立布局一致。保留共享通信区的基址、窗口切换和对齐规则，不为删几个控制字段重新压缩所有偏移；共享 peer memory 按 Dispatch/Combine 各自需求的较大值满足容量。首版不主动扩大当前 bs/H/K/专家数和有效行数的支持范围。

#### 21：数值验收口径（已确认）

**模式差异：** 去重路径先由M4计算Server内局部和并舍入回FP16/BF16，再交给M5；关闭路径由M6处理全部K份。求和分组和中间舍入发生变化，即使输入和数学目标相同，也不保证两模式逐bit一致。

**适用范围：** 数值计算与验收口径已确认，M6使用Dispatch原样输出的表5权重。

**已确认方案（采用参考建议）：** 保留原有输入转FP32、独立乘法、按k顺序FP32累加及最终RNE转换。按关闭路径的实际运算次序建立逐步舍入参考，在现有有限数值测试域要求与该参考逐bit一致，`atol=rtol=0`；开启路径仍用原参考。两模式之间只报告误差，不以逐bit相同作为通过要求；不因模式差异自行放宽正确性阈值。

#### 22：验证与统计适配范围（验证落实，不再讨论）

沿用现有验证方式，随实现完成开启路径回归、新路由分核与包格式、表2、表5原样拷贝及M6加权、空任务、量化及重复调用验证；无效参数用例按已确认19更新。去重与breakdown的构建组合和目标A5多Rank验证属于实现验证，不另行请求方案确认。

只适配因模式改变而失效的断言和统计：不校验padding或无效表内容，通信字节数采用新布局，关闭路径M1/M2/M4/M5标为不适用，记录M3/M6及总耗时；开启路径保留原有模块标识。数值标准以已确认21为准。验证不以修改公开调用契约来迁就实现。

#### 24：表5权重输出与Combine接入（已确认）

**已确认方案：** 非去重情况仍需传表5。Dispatch把源卡输入的原始FP32 `[bs,K]` 权重拷贝到独立的 `expertScalesOut`，不修改权重值、不改写为 `1.0`；调用方将该输出传给Combine现有 `expertScales` 参数，供M6对全部K份未加权专家输出加权。

不新增权重接口，不采用跳过Dispatch输出副本、直接把原始输入交给Combine的方案；原公开签名及有效表5的传值约定保持不变。表3在关闭路径无效，包尾仍不传输weight。

**当前接口依据：** `TileXRMoeEpCombineMemoryServerDedup` 中 `relayReadIndex`（表3）包含原M4使用的内嵌权重，`expertScales`（表5）提供原M5使用的FP32 `[bs,K]` 权重，没有第三个权重入口。新M6继续使用表5对应参数。

依据：[公开签名](../../src/include/tilexr_ep.h)、[Host参数传递](../../src/ep/host/tilexr_ep_memory_server_dedup_combine.cpp)、[当前M4权重读取](../../src/ep/kernels/tilexr_ep_server_dedup_partial.h)、[当前M5权重读取](../../src/ep/kernels/tilexr_ep_server_dedup_source_aggregate.h)。

## 9. 按主要函数划分的实现改动梳理

本节遵循§3的优先复用要求，描述已确认的实现组织及注意项。现有函数名用于定位，新增函数名仅为示意，不新增用户必须确认的命名要求；实现进度及验证边界见§10。

处理方式分为：**沿用**（主体无需改变）、**局部分支**（同一函数只隔离差异）、**提取共用**（少量整理后由两路径调用）、**新增入口/布局**（任务组织或资源生命周期确有差异）。不按文件或整个类机械复制去重/非去重版本。

### 9.1 Dispatch：入口、计数和发送

依据：[Dispatch入口](../../src/ep/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.h)、[Count](../../src/ep/kernels/tilexr_ep_server_dedup_counts.h)、[Sender](../../src/ep/kernels/tilexr_ep_server_dedup_send.h)、[Server选择助手](../../src/ep/kernels/tilexr_ep_server_dedup_select.h)。

| 主要函数 / 代码段 | 建议处理 | 改动与复用边界 |
| --- | --- | --- |
| `DispatchMemoryServerDedupKernel::Init / Process` | 沿用主体 | Count→Send/Recv→Finish的调度、Recv前完成cumsum DMA的屏障、profiling发布保持共用。发送模式在Sender内部选择，不新增第二个Dispatch kernel或调度类。 |
| `ServerDedupCounts::Init / SendCnt / RecvCnt / CalcCumsum / CompleteCumsum / Finish` | 沿用 | 计数、分核、前缀、控制区及结束时恢复状态不因关闭去重改变。Send继续使用其scratch pool时保留原生命周期限制。 |
| `ServerDedupSender::Init` | 局部分支 | 公共指针、shape、窗口和数据布局共用。原 `start_/count_` 是Token分段，只在去重发送及独立的表5拷贝分工中适用；不能用它描述非去重路由归属。关闭模式不需要绑定/使用表4。 |
| `ServerDedupSender::Process` | 提取共用＋新增发送调度函数 | 保留公共输入队列、原始expertIds加载、payload lane初始化和最终发送完成处理。把原Token分类/FirstHop/Direct发送循环保留为去重调度；新增非去重发送调度函数（如 `SendWithoutDedup`），入口用 `if/else` 选择。UB中公共搬运/q/量化区共用，分类/FirstHop与非去重路由筛选缓冲按模式分配。 |
| Sender UB预算（当前内联在 `Process`） | 提取预算助手 | 新筛选索引、比较缓冲、q/量化工作区及payload lane都纳入预算，供Host与kernel共用。不能只依赖当前Host的Count UB检查推定新Sender一定放得下；计算剩余空间前检查固定用量，防止无符号减法下溢。 |
| 新非去重发送调度：Server筛选、owner分配 | 新增函数，复用助手 | 按Server专家区间筛选原始路由索引i，双半区遍历与owner规则复用 `ServerDedupHalfOrder / ServerDedupFirstOwned`。区间比较可复用现有向量比较/GatherMask模式，仅将候选值改为expertId。任务生成方式与原Token分类实质不同，适合新增函数；发包本体不复制。 |
| `Classify` | 仅去重调用 | 删除旧临时关闭分支；新非去重发送调度不调用它，也不分配其server状态/FirstHop mask。保留开启路径的分类算法。 |
| `CalTokenSendExpertCnt` | 沿用 | 输入仍为expertId及原始展平索引i，q仍按原始前缀计算。非去重仅对本核拥有的路由调用，不因Send核变化重定义q。 |
| `SendPacket` | 共用，整理Token索引 | 输入加载、量化、480/512打包、lane轮转、目的槽位寻址及DMA/Event链基本一致，应共用。建议显式传全局tokenId，去重调度在调用前换算局部Token索引，非去重从i恢复；避免在共用函数内继续无条件使用 `start_ + token`。权重局部索引换算仅留在去重包尾部分。 |
| `FillTail` | 局部分支 | 三项源索引写入共用；开启模式额外写type、内嵌weight及relay entries，关闭模式写完三字段即结束，不构造转发表对象、不触碰padding。两模式都使用公共布局算出的包尾位置。 |
| `QuantMxfp8` 及payload打包代码 | 沿用 | MXFP8算法、scale位置及有效payload拷贝方式不变，块数/步长来自对应布局。量化计算所需的输入补零仍保留，它不同于已明确不处理的包尾padding。 |
| 表5加载/写出（目前嵌在 `Process` 内） | 提取共用，局部分支 | 权重GM→UB加载、表5写回和完成同步可复用：去重写调整后的 `sourceWeights_`，非去重把原始权重直接写回。非去重拷贝按源权重连续区间独立分工，不绑定路由任务owner；表4写出仅开启模式执行。无需新增一套通信模块来复制表5。 |

**需要明确改掉的边界：** 当前 `Process` 开头的 `if (count_ == 0) return` 只适用于旧Token分核。非去重模式下，某核即使没有分到源Token拷贝区间，也可能被owner分配到路由发送任务，不能因此提前退出。权重拷贝任务数与路由发送任务数应分别处理。

新增Server筛选列表不能与q/量化临时区发生存活期冲突：q函数和量化均会覆盖现有工作缓冲，尚未发送的路由索引必须保持有效。此处是UB设计检查，不要求另建完整Sender类。

### 9.2 Dispatch：接收与包格式

依据：[Receiver](../../src/ep/kernels/tilexr_ep_server_dedup_recv.h)、[接收包助手](../../src/ep/kernels/tilexr_ep_server_dedup_receive_packet.h)、[接收任务](../../src/ep/kernels/tilexr_ep_server_dedup_receive_tasks.h)、[包布局](../../src/ep/common/ep_memory_server_dedup_data_layout.h)。

| 主要函数 / 代码段 | 建议处理 | 改动与复用边界 |
| --- | --- | --- |
| `ServerDedupReceiver::Init / Process` | 局部分支，主体共用 | 前缀加载、tasks构建、批量探测、pending遍历、预取、退休及清Flag的主循环全部复用。仅按模式调整metadata临时空间，关闭模式不初始化失效表的访问状态。无需新增非去重Receiver类或复制 `Process`。 |
| `ServerDedupReceiveTasks::Build` 及pending/行号助手 | 沿用 | 两模式的Count输出、q和最终行序相同。 |
| `Slot / Probe / IssueLoad / ClearFlags` | 沿用 | 已通过 `data_.tokenStride / blockCount` 工作，消费新布局即可；槽位映射、Flag协议和DMA保护一致。 |
| `ConsumeLoaded` | 局部分支 | 公共payload/scale拷贝、表2写出及lane释放保留一份。开启模式解析type、构造原metadata并按需转发；关闭模式只读取三字段生成表2。表1写出、表3写出及forward循环整体放在开启分支，不能先读不存在的type再判断。 |
| `BuildServerDedupMetadata` | 局部分支或提取三字段复制 | 关闭模式只需把包尾三字段映射到表2；开启模式保留Terminal的特殊行映射。三字段复制可共用，不需要一套新的完整包解析框架。 |
| `ServerDedupPacketTail / PrepareServerDedupForward` | 保留去重专用部分 | 公共代码只持有有效header视图；relay index视图、type字段读取和转发header修改只在开启分支构造/执行。不能用新包padding假装旧type或relay表。 |
| `ReadServerDedupFlags / WriteServerDedupPayloadRange / ServerDedupWireOffset` | 沿用 | 数据与Flag分块规则未变，无需分别实现。 |
| `TileXREpServerDedupDataLayout` | 局部分支 | payload/scale、header起点和最终block/stride计算共用；只在“包尾有效长度及relay区布局”处分支。关闭模式不预留K相关relay段；同一函数供Host/Send/Recv使用。 |
| `TileXREpServerDedupDispatchOffset` | 沿用 | 原source/destination/localExpert/q公式不变，步长来自新布局。 |

注意Receiver当前每lane尾部还预留type和destination两个临时块，关闭模式可只保留destination所需空间，但应把对应偏移一起计算，不能只删除表1输出而遗留错误的UB索引。

### 9.3 Combine：公共入口与M3/M4/M6调度

依据：[Combine主类](../../src/ep/kernels/tilexr_ep_combine_memory_server_dedup_kernel.h)、[UB配置](../../src/ep/common/ep_memory_server_dedup_combine_config.h)。

| 主要函数 / 代码段 | 建议处理 | 改动与复用边界 |
| --- | --- | --- |
| `CombineMemoryServerDedupKernel::Init` | 提取共用＋资源局部分支 | args/shape、有效行数读取与合法性检查、结果块长和窗口选择共用；M1/M2控制区、stage与M45 workspace仅开启模式初始化。关闭模式Send核准备共用M3资源，Recv核交由M6初始化自己的资源，不先做M45分配/容量检查。 |
| `CombineMemoryServerDedupKernel::Process` | 局部分支 | M3和公共最终完成/profiling保留一份。开启模式执行M1/M2及Recv核M4→M5；关闭模式跳过这些步骤，Recv核调用新M6入口。无需复制整个Combine Process。 |
| `CopyLocalPendingTokens / NotifyServerCard / WaitServerReady / ReturnPartialTokens` | 保留，仅开启时调用 | 函数主体不需要改造成两份；模式分支放在调用处，关闭模式不分配相应资源。 |
| `PrepareDirectRows` | 局部分支 | 共用表2加载、源Rank提取及合法性校验。开启模式保留type1筛选和压缩；关闭模式生成全部有效行索引，`selected_=tokens_`，不读取表1、不执行类型压缩。关闭模式仍需输出后续发送所用的行号/Rank列表。 |
| `SelectDirectServer` | 沿用 | 输入只要保持上述行号/Rank列表契约，Server区间筛选无需改变。 |
| `ReturnDirectTokens`（M3） | 沿用主体 | 调用调整后的 `PrepareDirectRows` 后，双半区顺序、owner均衡、表2查表、未加权payload打包和结果槽位发送均相同，无需新增“非去重M3”。 |
| `AggregateSourceTokens`（现M5入口） | 保留入口，调用共用聚合主体 | 继续使用M4交接得到的布局及Event。为M5/M6共用核心做必要参数/视图整理，不改变开启模式的资源顺序和计算行为。 |
| 新M6入口（如 `AggregateSourceTokensM6`） | 新增模块入口 | 负责独立布局/UB初始化、权重和pending准备、Event初始化、调用共用接收聚合主体及独立收尾；不复制M5的整套探测、预取和计算。 |
| `DrainSharedOutputs` | 保留去重调用＋M6独立收尾 | 原函数按M4/M5共享输出及退休掩码处理，仍用于开启模式。M6只退休自己的输出/Event；可提取纯lane等待助手，但不让M6构造假的M4状态来调用旧drain。 |
| `EpCombineM45Layout / EpCombineM5Layout` | 保留去重算法 | M4输出槽回收规划仅服务开启模式。 |
| M6布局计算（如 `EpCombineM6Layout`） | 新增布局函数 | 按pending、表5权重、Flag、FP32区和输入/输出lane独立预算；可复用对齐、Token分段、Flag/work容量公式。提供Host/kernel共同使用的布局，不让M6经过 `EpCombineM45Layout(..., partialTasks=0)`。 |
| `EpCombinePayloadLaneCount`（M3预算） | 局部分支 | M3发送流水共用，但当前固定UB公式包含M1/M2控制预留；模式变化后需同步调整该固定部分，避免Host预算与实际M3初始化不一致。 |

两个容易误删的公共资源：当前 `prepareSum_` 最初用于读取有效行数，并不只是M2缓冲；`typeUb_` 后续还被复用为源Rank列表，`maskUb_` 也用于Server筛选。关闭类型逻辑时不能仅按变量名把这些存储全部删除，可重命名或拆清用途后复用。

### 9.4 M5与M6：复用聚合主体，仅隔离资源和mask差异

依据：[当前 `ServerDedupSourceAggregate`](../../src/ep/kernels/tilexr_ep_server_dedup_source_aggregate.h)。M6独立指资源生命周期独立，不意味着算法源码必须独立。建议保留一套聚合核心，并提供去重初始化与M6初始化两个入口；也可以由两个薄封装调用同一核心，避免复制后分别维护。

| 主要函数 / 代码段 | 建议处理 | 真正需要隔离的差异 |
| --- | --- | --- |
| `ServerDedupSourceAggregate::Init` | 提取共用＋新增 `InitM6` 入口 | Token分段、result/yOut指针、表5加载及pending初始化共用；去重入口按M45交接布局绑定缓冲并加载mask，M6入口使用独立布局、自行初始化Event，不分配/读取mask。仅共用缓冲视图绑定和常规准备，不把M6绑定到旧M45布局对象。 |
| `Process` | 共用主体，局部分支 | pending扫描、就绪后聚合、任务移除、按dtype调用及输入退休共用。只有开启模式读取并检查mask；关闭模式不伪造全1 mask表。profiling选择当前模块对应的事件标识。 |
| `Probe` | 共用，只改期望值 | 现实现本来就读取全部 `K*blocks` 的Flag；开启模式期望值为 `popcount(mask)*blocks`，关闭模式为 `K*blocks`。Flag加载、归约和比较主体不复制。 |
| `Load` | 共用主体，局部分支 | 输入lane轮转、`V_MTE2`保护、有效payload搬入及 `MTE2_V`通知共用；只有开启模式执行M4旧输出区首次回收的等待/退休掩码逻辑。M6不读取或生成该回收状态。 |
| `Aggregate<T>` | 共用预取/计算/写回 | 开启模式按mask跳过无效k，关闭模式按所有k递增；用局部 `if/else` 处理下一有效k的选择。Cast、权重索引、Muls/Add、输入槽释放、输出槽等待和最终写回共用一份，保持现有算术顺序。 |
| `ClearFlags` | 共用 | 两模式都清本核负责Token的完整 `K*blocks` Flag，保留payload；现有同步方式可用于M6自身操作，无需为去重/非去重复制清理函数。M4相关注释应限定适用范围。 |
| 输入/输出退休及profiling `Mark` | 局部分支或小助手 | M5仍交回共享输出退休信息，M6只完成自身资源。共用基础Event操作和打点工具，模块标识区分M5/M6，不能把M4交接状态作为M6的隐藏前置条件。 |

共用内部接口可以接受已计算好的buffer视图、输入/输出offset和lane数；M4专属信息只在开启分支消费。不要为了源码相同给M6分配假的mask/relay表、运行空M4，或保留无效的M4同步。

### 9.5 Host、构建与验证的配套改动

| 主要函数 / 文件 | 建议处理 | 改动范围 |
| --- | --- | --- |
| Dispatch/Combine `TileXREpValidateBasic…Params`、`TileXREpValidate…Config` | 同函数局部分支 | 开启模式保留五表检查；关闭模式仅跳过表1/3/4检查，表2/5仍有效。Dispatch权重输入/输出不重叠等现有契约保留，并使用与新Sender一致的UB预算。Combine UB检查选择M45或M6对应预算，M3预算同步更新。 |
| `TileXREpBuildMemoryServerDedupLayout`、`TileXREpBuildMemoryServerDedupCombineLayout` 及 `BuildDataLayout` | 同函数局部分支 | Dispatch使用共用的新包布局；关闭模式不要求失效表容量，Combine不计转发stage空间。结果区、形状、对齐、容量溢出和窗口校验共用。 |
| `TileXREpValidateServerDedupAllocations` 的调用侧 | 调整有效分配列表 | 该通用校验拒绝空指针及零requiredBytes；关闭模式应在构造分配列表时排除无效表，不能只把其长度置0再原样送入检查。通用校验本身保持严格。 |
| 两个公开API wrapper、Host launch、kernel参数包 | 沿用 | 参数、ABI、magic/窗口选择及异步生命周期不变。M6属于现有Combine kernel内部模块，不另建公开launch接口或权重入口。 |
| `build_all.sh`、workflow、EP CMake及公共宏定义 | 配套修改 | 接入已确认dedup参数和统一宏，删除旧无条件定义及临时关闭逻辑。Host、两个kernel、布局消费者及manifest一致；不按模式复制构建脚本。 |
| profiling枚举/解析与现有测试参考 | 共享框架，更新模式差异 | M6有对应模块标识；关闭路径不读取无效表或校验padding，表5原样复制，数值参考按全部K份聚合。覆盖开启路径回归、无效参数传空、空拷贝区间但有路由任务、重复调用及资源边界。不新增源码文本匹配测试。 |

### 9.6 新增代码的实际边界

必须体现新行为的主要入口只有两类：**Dispatch非去重发送调度**，以及**独立M6的资源生命周期入口**。与之配套可能新增Server路由筛选小助手、M6布局/初始化/收尾函数，并提取表5拷贝或公共聚合准备代码；这些不等于新增整套算子。

Dispatch接收主循环、Combine M3发包主体和M5/M6接收聚合主体都适合共享。新增发送函数仍调用同一套 `SendPacket / FillTail / QuantMxfp8`，M6仍调用共享的 `Process / Probe / Load / Aggregate / ClearFlags`（具体归属与命名可整理）。普通 `if/else` 使用固定宏值，优先放在实际差异处，不添加运行时可变模式。

首要核查点是全局Token索引与局部拷贝索引分离、条件分支之前不访问无效metadata、UB工作区存活期，以及M6的Event确由自身初始化并完成。共用代码改变时同时验证开启路径，避免把“行为保留”误解为“共用函数不能整理”。


## 10. 首版实现记录

本轮已按§9开始并落实代码修改，公开API和kernel参数包保持不变。

- `SendWithoutDedup` 按256条原始路由分块做Server区间筛选。分块只限制临时UB，保序及owner跨分块、Server、双半区连续推进；与q/量化工作区分开，继续调用共用 `SendPacket`。表5仍按原连续Token区间独立拷贝。
- 共用 `ConsumeLoaded` 只在开启分支读取type/relay；关闭分支只读三字段并写表2，每lane metadata从64B减到32B。输入量化计算所需补零保留，包尾padding不解释、不额外初始化。
- Combine通过 `AggregateSourceTokensM6` → `InitM6` →共用聚合主体→ `DrainM6` 执行关闭路径；`EpCombineM6Layout` 独立预算，输入最多7槽、输出最多2槽，不调用M4布局/回收规划。M3只将有效行选择改为全部行，发送主体保留。
- `build_all.sh dedup=0|1` 与 `breakdown=0|1` 独立，Host、两个kernel及生产消费者使用相同数值宏。构建目录区分两者，manifest新增 `server_dedup_enabled` 和 `server_dedup_protocol=2`；运行/同步从安装包读取并核对模式，旧manifest要求重建。
- profiling新增 `m6_final` / `m6_token_issue`，关闭模式不要求M1/M2/M4/M5事件或控制流量。历史验收阶段字段 `implemented_through=M5` 保留为完整Combine契约标识，实际接收模块另以 `combine_receive_module=M6` / `receive_module=M6` 标识；历史 `m6_drain` 仍表示kernel最终drain，与本次M6接收聚合区分。

### 10.1 实现中确认的边界

- 没有表5拷贝任务的Send核仍可能拥有路由任务；不能提前退出，也不能给 `TBufPool` 分配0字节权重缓冲。关闭测试通过空表1/3/4、BS小于Send核数的场景覆盖此边界。
- 布局查询继续报告空间需求；新Sender UB准入检查放在Host启动校验，kernel在分配前检查预算，避免将“大空间布局可计算”误当作“kernel UB可执行”。
- M6在源Token数为0的Recv核上也要配对退休自己初始化的输出Event；有效专家行数为0不能跳过有源Token的M6。
- 历史M1/M2/M4专用单元fixture及旧消费者生命周期fixture明确以开启模式执行；配置为关闭时，发送、接收、Host、布局、完整M3/M6及profiling kernel测试实际编译为关闭模式，不能把历史fixture通过当作M6验证。

### 10.2 验证边界

验证覆盖Host C++14、CPU AscendC行为替身、普通及profiling编译、两模式的权重/路由/表访问、双窗口复用和事件生命周期。CPU替身不证明AICore可见性、DMA时序或MXFP8设备指令正确性，不能代替CANN 9.1 A5编译及设备运行；具体执行记录保存在验证日志中。
