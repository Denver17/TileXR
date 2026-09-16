# EP Memory Server Dedup Combine 待确认项

> 2026-09-16 更新：AtomicAdd/密排 t/group 表 8*BS 行的历史结论已由[主设计的 Token 级 prefix 契约](ep-memory-server-dedup.md#2026-09-16dispatch-使用-token-级-prefix-编号当前契约)替代。Dispatch 第一跳使用 t=row，group 表按 C=R*BS*min(K,L) 预留；终端继承第一跳编号。Combine 返回区按 C*K*returnStride 的扩容及内部实现留到 Combine 阶段，当前旧 relay 预留不能按新编号使用。以下旧编号保留作历史追溯。


## 文档状态

第一轮 Combine 问题已与 Dispatch 共同归并到[主设计](ep-memory-server-dedup.md)及
[第二轮待确认项 R01-R12](ep-memory-server-dedup-open-topics.md)。
本文保留 C01-C14 旧编号的定位映射，并承接本轮从 Dispatch 待确认文档迁入的 Combine 内部实现议题。
Dispatch 细节和 Dispatch/Combine 公共接口仍在[第二轮待确认项 R01-R12](ep-memory-server-dedup-open-topics.md)维护；本文的 C15 之后只讨论 Combine 内部实现和内部验收。
已确认的公共契约及 R04-07/R04-08 权重复用已同步主设计，source 按 sourceMask 和 Dispatch 输出的 expertScalesOut 统一归约，
不再保留结果类型/权重载体待定的旧表述；本文继续保留 Combine 内部实现的待确认项。

Server 内逐 expert 返回、公共转发包、本卡/跨卡统一原子取号和密排 recvSlot/weight 成员表，均以主设计的新约定为准。
专家数量沿用主设计的总数上限 1024，保持 `moeExpertNum=R*L` 均匀分配；不设独立的单卡专家数限制，允许 L=1，128P 下每卡最多 8 个 expert。
R04-01 已确认 Dispatch 新增 assistEx/sourceMask/group 三个 metadata 输出，Combine 对应新增三个输入；独立权重输出见 R04-08，不引入 X。
R04-07/R04-08 已确认权重输入输出分离：Dispatch 从只读 FP32 `[BS,K]` expertScales 取真实权重，将 DIRECT 真实值及 returnK=1.0 写入独立 expertScalesOut；Combine 的既有 expertScales 参数接收该输出。
Source 按 sourceMask 只等待有效 k，统一乘 expertScales 后累加；mask 为 0 的位置不消费、无需清零，不新增 source/sourceWeight/partialMask 或权重快照 tensor。
Relay 的第一跳 assistEx 与 group members 保留真实权重，relay 对本 Server 内第一跳及全部转发贡献加权求和；终端 expertOut 返回未加权结果。
Dispatch 输出 expertScalesOut 保持到 Combine 消费完成；原始 expertScales 输入只读，可按调用生命周期复用。首版要求非空 FP32 权重输入，nullptr 直接报错。重复 Combine 规则归 R11-02。
R08-03 已确认 Dispatch 支持 BF16/FP16 输入、非量化及 MXFP8 E4M3/E5M2，量化计算和 dynamicScalesOut 沿用 Memory；路由 weight 保持 FP32。
该结论确定的是送给 expert 的 hidden/scale 格式，不自动将 expertOut 或 Combine 的 member/partial 返回设为 FP8；这些公共契约及内部数值分别在 R08-03 剩余部分和 C15/C16 讨论。
安全用例保证每个 relay 的实际 group 数不超过 8*BS，不做超限校验、状态记录、报错或超限测试；X 记录及超限优化为 R04-06/R07-04 低优先级遗留。
内部 AtomicAdd 分配 forwardTokenIdx 的逻辑保留，不要求发布计数。R04-03 已确认在有效 assistEx 行中识别 forwardCount>0 的第一跳，取 t 访问固定 stride 的 group[t]，firstHopRow 保存 dstPosition；内部筛选和分核后续在本文讨论。
R04-02 已确认 ExpertOut/assistEx 保持 Dispatch 的有效行序，首版 `tokenNum=sendCounts[L*R-1]`；expert 内部若重排，须在 Combine 前还原，不增加 public row map。
R04-04 沿用 Memory 的算子完成语义：所有核完成任务及 GM 写入后 Dispatch 才结束，后续 Combine 可读取完整 metadata 和 Dispatch 输出的 expertScalesOut，不另设发布协议、ready 或额外同步。
上述转发和 group 归约仅服务于跨 Server 去重；源 Server 内逐 route DIRECT，Combine 直接回 source，R01 已确认关闭。
R04-05/R07-02/R07-03 已确认统一中转结果区 `[8*BS,K,H]`，group 表预留 8*BS 行；全卡取号 `t`，本卡及其他卡返回共用 `recvSlot=uint64(t)*K+originalK`，不按来源卡分区。
每组即使 n=1 也预留 K 槽，只监听 members[0:n] 指定的位置；第一跳从 firstHopRow 读取，空洞不使用、不等待。
已有 assistEx 的 relayRank/t/k 足够返回寻址，无新编号或包字段；assistEx[tokenNum,8] 和 sourceMask[BS] 的 shape 保持不变。
8*BS 是本 relay 合计 group 的安全容量上限，并非八卡拓扑自然保证的接收上限；单份区域 base/offset/非量化 stride 与预算已由 R07-01 收口，跨调用复用仍归 R11。
已确认 Server Dedup 独占通信空间，按状态区（cnt/cumsum/counter）、数据接收区、中转 Token 区三块排布；不保留旧 Memory 区域位置。
R07-01 已采用第二块内 source 返回在前、Dispatch 槽位在后的独立子区；member 使用第三块，当前非量化返回沿用 Memory DataAsFlag。Combine 内部实现不另改公共大区顺序。
旧单 expert 示例不能代替当前多 route/group 契约；L=1 仍按同一协议处理，不沿用静态全 source/token 结果区或逐 Token credit。

接口复用清单见 [Dispatch 待确认文档](ep-memory-server-dedup-open-topics.md#同语义接口复核)：sendCounts 提供 tokenNum，sourceMask 提供 source 每 Token 的有效 k 和贡献数，group 的 n 给出转发返回数；这些数量不另增参数。
量化 dynamicScalesOut 与 route weight 语义不同，旧 bool mask 与 uint32 sourceMask 也不同；普通 Memory 的全 K 等待逻辑必须适配 mask 有效范围，不能因参数复用就整段沿用。

## 本轮迁入边界

**状态：待后续 Combine 讨论，本轮只迁移问题，不选择内部实现。**
C01-C14 继续保留历史入口，迁入内容以 C15-C24 单独定位；原 R 编号保留映射，不因迁移变成已确认。

| 原位置 | 本文位置 | 迁入范围 |
| --- | --- | --- |
| R08-03 的内部部分 | [C15](#c15) | 乘法/累加 dtype、cast/scale、内部量化及分阶段格式实现 |
| R08-04 | [C16](#c16) | 两级归约的确定性、误差及特殊数值 |
| R09-01 | [C17](#c17) | 四类职责的核分工和 kernel 阶段 |
| R09-02 | [C18](#c18) | Group/source owner 与 H 分块 |
| R09-03 | [C19](#c19) | 累加器驻留及等待策略 |
| R09-04 | [C20](#c20) | 进度保存与慢成员推进 |
| R09-05 | [C21](#c21) | 多 H tile 协调及 partial 发布 |
| R09-06 | [C22](#c22) | 各类任务及完整对外责任结束 |
| R12-02、R12-03/R12-04 的 Combine 部分 | [C23](#c23) | Combine-only、联合数值验证及 Combine/端到端性能验收 |
| R06-04、R07-01、R11 的内部部分 | [C24](#c24) | Combine Flag 清理、内部 scratch 和资源复用实现 |

R04 的 Dispatch 持久输出与 expertScales 复用、R07 的 recvSlot/公共区域容量、R08 的 weight/接口 dtype、
R10 的 ABI 和 R11 的对外生命周期仍在主待确认文档维护。内部实现必须消费这些公共契约；
返回 payload/stride 等跨模块格式发生变化时，应回到对应公共项共同确认，不能仅在本文件修改。

## C15

**议题：Combine 内部 dtype 与量化。来源：R08-03 的内部部分。**

需要明确 expert 输入转为乘法 dtype、group 累加、partial 通信、source 累加和最终输出的各次 cast/量化及 scale 生命周期。
当前非量化公共返回跨度已由 R07-01 按 Memory 收口；Dispatch 的 BF16/FP16 非量化与 MXFP8 输出格式也已确认。expertOut 及 Combine 各段返回的公共格式仍归 R08-03/R10-03，本项只讨论内部如何实现这些约定。
不能因普通 Memory 有某种格式的源码，就认为两级去重归约已适配或通过验证。

## C16

**议题：两级归约数值规则。来源：R08-04。**

需确定相对/绝对误差、高精度参考、确定性以及 NaN/Inf、强抵消、零 weight 等数值判据。
已确认第一跳后按原始 k 升序消费密排 member、source 按既定 k 顺序消费，但两级分组会改变浮点括号，
不能要求与单级 Memory 归约天然逐位一致。零 weight 不自行省略已约定的 route、ready 或贡献。
关闭时给出可执行参考与误差表，不能仅用“FP32 累加”代替验收规则。

## C17

**议题：四类职责与 kernel 阶段。来源：R09-01。**

Expert 发送、group 监听/归约、partial 发送、source 归约使用固定核组还是同核轮转？
在一个注册 kernel 内推进，还是拆为多个注册 kernel 阶段？
Dispatch 的 cumsum/copy/relay 同组核安排不自动适用于 Combine，普通 Memory 调度也不是新增 relay 调度的证明。
先给出职责依赖和无循环等待的推进方式，再比较方案。

## C18

**议题：Group/source owner 与 H 分块。来源：R09-02。**

Owner 按 group、source Token、H tile 或 batch 分配？H 尾部与有效行如何界定？
关闭时需给出核到工作区间的明确映射，覆盖零 group、`tokenNum=0` 和空职责核。

## C19

**议题：累加器驻留与等待。来源：R09-03。**

等待下一 k/member 时，累加器驻留 UB、存入 GM scratch，还是等全部成员到齐后开始归约？
分别核算 UB/GM 用量、额外搬运及可同时推进的 group 数，不能只用一个小 H 示例证明全范围容量充足。

## C20

**议题：进度状态与慢成员。来源：R09-04。**

如何保存 next-member/k、H tile、累加器和发送进度，避免重复累加或慢成员占满资源？
某个 relay 的慢成员尚未 ready 时，其他 expert 发送和 group/source 任务必须能继续。
本项需明确每类状态的唯一 writer、完成迁移与 UB 回收规则。

## C21

**议题：多 H tile 与 partial 发布。来源：R09-05。**

同 group 多 H tile 的 owner 如何协调，partial 分块 ready 与整组完成如何表达，空职责核参加哪些同步？
是否整行发布或逐 tile 发布仍待确认；具体 DataAsFlag/scale 排布不能与公共结果区 stride 冲突。
一块数据已发布不能直接代表整个 group 完成。

## C22

**议题：完整责任结束。来源：R09-06。**

Expert 发送、partial 发布、source 输出各何时完成，全部对外责任何时结束？
某卡 `tokenNum=0` 时仍可能需要接收本卡 BS 个输入的结果；source yOut 完成时也可能仍有对外发送在途。
关闭时给出每类发送/监听/归约任务的终止条件与安全输入范围内的退出路径。
对外 event/plan 释放契约归 R11，本项给出满足该契约的内部机制，不添加 X 超限处理。

## C23

**议题：Combine 内部与端到端验收。来源：R12-02 及 R12-03/R12-04 的 Combine 部分。**

- **C23-01：** Combine-only 的合法 assistEx/sourceMask/group 和 Dispatch 输出的 expertScalesOut 输入如何独立生成，并保证 joint Dispatch-expert-Combine 测试不会同错互相抵消？有效行序和 group 识别按 R04-02/R04-03，槽位按 R04-05/R07-03，真实成员权重与 source 归约权重按 R04-07/R04-08，不新增 X 输入。
- **C23-02：** 如何验证第一跳与全部本卡/跨卡返回恰好一次、仅消费前 n 个 member、交错原始 k 的固定顺序，以及 source 按 mask 统一加权（DIRECT 真实权重、partial 乘 1.0）？需覆盖第一跳/成员保持真实输入权重、独立 expertScalesOut 保存 source 归约权重、mask 空洞不等待且不消费、真实 DIRECT weight 为 0/1 的正常贡献；误差按 C16 落实。
- **C23-03：** 哪些规模、路由和偏斜属于 Combine/端到端性能目标，允许多大退化，如何与普通 Memory 保持相同输入和数值契约？

安全矩阵保留实际 group 数为 0/BS/8*BS-1/8*BS、group 数不超过 8*BS 且转发结果条数 Y>8*BS、仅本卡转发、不同 source 同 TokenId、`tokenNum=0`、零 group、慢成员和连续轮次；不要求 X 字段或输出断言。
同时覆盖 group 数大于旧 BS 上限、n=1 仅监听一个返回、本卡/跨卡混合及原始 k 有空洞；可达边界按所选 Rank 数和路由构造，K=1 时无 group。
不加入重复 top-k、invalid expert、mask 或中转容量超限的非法用例；BS=0 仅在 R10 接纳后才能加入。

性能需统计真实通信字节，包括 expert 返回、加权 partial、Flag、scale、对齐、持久 metadata 和监听/归约成本。
例如两份 FP16/BF16 结果合成一份 FP32 partial，hidden 字节未必减少，不能仅以 Token 数证明收益。
每阶段报告 backend、native status、kernel version、算法/通信数据量、可读 latency、适用 bandwidth，以及全部 Rank 的 median/min/max。
使用已有匹配 HCCL Test 资格记录；MoonEP 模型/replay 保留框架 profiler 和 Dispatch/Combine stage barrier，使用允许的 grouped mode。
实现结果、Host/reference 结果和 NPU 验证分别报告。

## C24

**议题：Combine 内部 Flag、buffer 与复用。来源：R06-04/R07/R11 的内部部分。**

公共基础 API 依据见 [CANN 9.1 官方 API 核对](ep-memory-server-dedup-open-topics.md#官方-api-核对)：PipeBarrier、SetFlag/WaitFlag、Event 分配/释放、DataCopyPad 及 UB 分配的能力与约束已核对，非阻塞 MTE3 完成查询未查到。
WaitFlag 会阻塞目标流水，ReleaseEventID 只释放 ID；不得将任一操作当作非阻塞完成查询。查询是回收发送 UB/Event 的一种候选，并非已确认的必选接口。该证据供后续实现使用，不在本轮选定以下 Combine 内部方案。

- **C24-01：** member/partial/source 结果的消费 owner 何时清理 Flag，使用哪些 Event/PIPE 顺序，如何保持清零 UB 到完成？公共责任域仍按 R06-04。
- **C24-02：** 累加器、发送/接收 UB、GM scratch 如何分配和复用，生命周期是否允许重叠？确定后将通信区容量需求交给 R07，不能在此修改 Dispatch recvSlot 编码。
- **C24-03：** 新 invocation 与旧在途发送/远端消费如何隔离，内部机制如何满足 R11 的完成和公共窗口复用契约？

Magic 选取 ping-pong 窗口不等于旧 Payload Flag 自动失效；必须结合正确清理、调用次序和资源完成条件。
X 超限及尽力转发仅保留为 R07-04 的低优先级备忘，本项不增加超限校验、状态、报错或降级。

## 编号映射

| 原编号 | 原议题 | 归并状态 | 当前讨论位置 |
| --- | --- | --- | --- |
| [C01](#c01) | 多 Expert 贡献身份与 Group 边界 | 统一转发/编号、三项输入、密排成员表、有效 group 识别及行引用已确认；GM 写入沿用 Dispatch 算子完成语义，无独立发布议题 | [R01](ep-memory-server-dedup-open-topics.md#r01)、[R04](ep-memory-server-dedup-open-topics.md#r04) |
| [C02](#c02) | ExpertOut 行序、有效行与 Plan 一致性 | 已随 R04-02/R04-03 确认：沿用 Memory 行序及有效 tokenNum，expert 重排在 Combine 前还原，无 public row map，firstHopRow=dstPosition | [R04](ep-memory-server-dedup-open-topics.md#r04)、[R10](ep-memory-server-dedup-open-topics.md#r10) |
| [C03](#c03) | DIRECT、Group 与 Source 寻址及 Mask | 原始 k、mask、统一 recvSlot 及 source 按 expertScales 统一加权已确认；无需 source 结果类型标记或返回计数 | [R04](ep-memory-server-dedup-open-topics.md#r04)、[R07](ep-memory-server-dedup-open-topics.md#r07)、[R08](ep-memory-server-dedup-open-topics.md#r08) |
| [C04](#c04) | Route Weight 来源与单次加权 | 包/assistEx/member 的真实权重、权重输入/输出分离及输出保持至 Combine 完成已确认；首版要求非空 FP32 expertScales，其他公共支持组合仍归 R08-03/R10 | [R04](ep-memory-server-dedup-open-topics.md#r04)、[R08](ep-memory-server-dedup-open-topics.md#r08) |
| [C05](#c05) | 两级归约顺序、精度与量化 | 公共格式由 R08 约定；内部数值待确认 | [C15](#c15)、[C16](#c16)、[R08](ep-memory-server-dedup-open-topics.md#r08) |
| [C06](#c06) | 两段回传格式与 Ready | 公共 stride/地址由 R07/R08 约定；内部发布和清理待确认 | [C15](#c15)、[C21](#c21)、[C24](#c24)、[R07](ep-memory-server-dedup-open-topics.md#r07) |
| [C07](#c07) | Combine 分核与调度 | 内部安排已迁回本文待讨论 | [C17](#c17) |
| [C08](#c08) | H 分块、累加驻留与慢成员 | 内部 H tile、累加器与进度状态待确认 | [C18](#c18)、[C19](#c19)、[C20](#c20) |
| [C09](#c09) | Workspace 容量、Slot 复用与流控 | 统一原子编号、8*BS 个 group 的共享容量及 t*K+k 槽位已确认，无超限校验；单份区域预算已收口，跨轮复用待确认，X 记录及超限优化低优先级遗留 | [R06](ep-memory-server-dedup-open-topics.md#r06)、[R07](ep-memory-server-dedup-open-topics.md#r07)、[R11](ep-memory-server-dedup-open-topics.md#r11) |
| [C10](#c10) | Plan 代际、执行轮次与并发生命周期 | Plan 与 Combine epoch 的绑定、重复/并发调用和窗口复用待确认 | [R11](ep-memory-server-dedup-open-topics.md#r11) |
| [C11](#c11) | 去重入口与跨 Rank 契约 | 三个新增输入、有效行序及复用 expertScales 已确认；同语义 Memory 参数优先沿用，无 X 输入或超限校验，具体声明/方向注释和支持矩阵待确认 | [R04](ep-memory-server-dedup-open-topics.md#r04)、[R10](ep-memory-server-dedup-open-topics.md#r10) |
| [C12](#c12) | 完成边界、错误传播与退出 | 对外完成由 R11 约定，内部完整责任结束待确认；X 超限继续延后 | [C22](#c22)、[C24](#c24)、[R11](ep-memory-server-dedup-open-topics.md#r11) |
| [C13](#c13) | 正确性参考与验证矩阵 | Combine 内部和联合验证待确认；Dispatch 验证保留 R12 | [C23](#c23)、[R12](ep-memory-server-dedup-open-topics.md#r12) |
| [C14](#c14) | 通信字节与端到端收益 | 保留真实协议字节口径；Combine/端到端性能目标待确认 | [C23](#c23) |

## 旧议题入口

## C01

**多 Expert 贡献身份与 Group 边界。** 统一转发/编号、三项输入、密排成员表、有效 group 识别及行引用已确认；GM 写入沿用 Dispatch 算子完成语义，无独立发布议题。结论见 [R01](ep-memory-server-dedup-open-topics.md#r01)、[R04](ep-memory-server-dedup-open-topics.md#r04)。

## C02

**ExpertOut 行序、有效行与 Plan 一致性。** 已随 R04-02/R04-03 确认：沿用 Memory 行序及有效 tokenNum，expert 重排在 Combine 前还原，无 public row map，firstHopRow=dstPosition。结论见 [R04](ep-memory-server-dedup-open-topics.md#r04)、[R10](ep-memory-server-dedup-open-topics.md#r10)。

## C03

**DIRECT、Group 与 Source 寻址及 Mask。** 原始 k、mask、本卡/跨卡统一 recvSlot 及 source 按 expertScales 统一加权已确认；无需 source 结果类型标记或返回计数。结论见 [R04](ep-memory-server-dedup-open-topics.md#r04)、[R07](ep-memory-server-dedup-open-topics.md#r07)、[R08](ep-memory-server-dedup-open-topics.md#r08)。

## C04

**Route Weight 来源与单次加权。** Dispatch expertScales 输入只读，expertScalesOut 独立输出 source 归约权重并由 Combine 的 expertScales 参数消费；GM 输出随 Dispatch 算子完成。包/assistEx/member 保留真实权重，输出保持到 Combine 完成。首版输入非空 FP32 `[BS,K]`；其余公共组合归 R08-03/R10。

## C05

**两级归约顺序、精度与量化。** 公共格式见 [R08](ep-memory-server-dedup-open-topics.md#r08)，内部 dtype/量化和误差判据见 [C15](#c15)、[C16](#c16)。

## C06

**两段回传格式与 Ready。** 公共地址/stride 见 [R07](ep-memory-server-dedup-open-topics.md#r07)、[R08](ep-memory-server-dedup-open-topics.md#r08)，内部发布和清理见 [C15](#c15)、[C21](#c21)、[C24](#c24)。

## C07

**Combine 分核与调度。** Expert 发送、group 归约、partial 发送和 source 归约的内部安排见 [C17](#c17)。

## C08

**H 分块、累加驻留与慢成员。** 见 [C18](#c18)、[C19](#c19)、[C20](#c20)。

## C09

**Workspace 容量、Slot 复用与流控。** 统一原子编号、8*BS 个 group 的共享容量及 t*K+k 槽位已确认，无超限校验；单份区域预算已收口，跨轮复用待确认，X 记录及超限优化低优先级遗留。后续见 [R06](ep-memory-server-dedup-open-topics.md#r06)、[R07](ep-memory-server-dedup-open-topics.md#r07)、[R11](ep-memory-server-dedup-open-topics.md#r11)。

## C10

**Plan 代际、执行轮次与并发生命周期。** Plan 与 Combine epoch 的绑定、重复/并发调用和窗口复用待确认。后续见 [R11](ep-memory-server-dedup-open-topics.md#r11)。

## C11

**去重入口与跨 Rank 契约。** 双入口隔离，assistEx/sourceMask/group 三个新增输入、有效行序及复用 expertScales 已确认；同语义 Memory 参数优先沿用，无 X 输入或超限校验。首版要求非空 FP32 `expertScales[BS,K]`，`nullptr` 直接报错。以当前独立去重 Dispatch 声明为基线，具体 Combine 声明、方向注释及支持矩阵仍见 [R04](ep-memory-server-dedup-open-topics.md#r04)、[R10](ep-memory-server-dedup-open-topics.md#r10)。

## C12

**完成边界、错误传播与退出。** 内部责任结束见 [C22](#c22)、[C24](#c24)，对外生命周期见 [R11](ep-memory-server-dedup-open-topics.md#r11)。X 超限继续低优先级延后。

## C13

**正确性参考与验证矩阵。** Combine 及联合验证见 [C23](#c23)，Dispatch 与公共输出检查见 [R12](ep-memory-server-dedup-open-topics.md#r12)。

## C14

**通信字节与端到端收益。** Combine 与端到端目标见 [C23](#c23)，Dispatch 单算子目标见 [R12](ep-memory-server-dedup-open-topics.md#r12)。

## 验证边界

本次为设计文档归并，没有实现或运行去重 Combine。
已有普通 Memory 或 MoonEP 测试不能证明本设计的行为；Combine 内部验证见 C23/C24，
Dispatch 与公共契约验证见 [R12](ep-memory-server-dedup-open-topics.md#r12)及[主设计验证边界](ep-memory-server-dedup.md#验证边界)。
