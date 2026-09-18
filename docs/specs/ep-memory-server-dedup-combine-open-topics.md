# EP Memory Server Dedup Combine 待确认项

## 文档状态

2026-09-18：Dispatch 优化设计阶段收口，主规格已按 `4858ce6` 的实际实现校准。
本文作为下一轮 Combine 设计入口；当前尚无 Server Dedup Combine 实现，不借用普通 Memory
或 MoonEP Combine 的实现状态。C01–C14 保留历史编号，后续完善 C15–C24。

## Dispatch 交接基线

权威字段与容量见[主规格](ep-memory-server-dedup.md#dispatchcombine-公共接口与-metadata-shape)。
以下是当前 Dispatch 已提供、Combine 必须消费的契约：

| 输入 / 身份 | 已落地的 Dispatch 契约 |
| --- | --- |
| 有效行 | tokenNum=sendCountsOut[E−1]；expertOut 必须恢复成 Dispatch 的行序，与 assistEx 一一对应 |
| assistEx | 32B/行：source、Token、k、真实 FP32 weight、角色/entry 数、relayRank/t |
| 第一跳编号 | t=row，稀疏、仅在该 relay 本轮唯一；终端继承该 (relayRank,t)，不重新编号 |
| group | Crows=R*BS*min(K,L) 行容量；firstHopRow=t；前 n 个 (recvSlot=t*K+originalK, FP32 weight) 有效 |
| group 枚举 | 在有效 assistEx 中找 forwardCount>0 的第一跳；不扫描所有容量行，不需要 group 数 X |
| sourceMask | 每 Token uint32；DIRECT 保留原始 k，group 只保留最小 k/returnK |
| source 权重 | 独立 expertScalesOut[BS,K]：DIRECT 真实权重，returnK=1.0，mask 空洞不消费 |
| Dispatch 量化 | BF16/FP16→原 dtype 或 MXFP8 E4M3/E5M2；不自动决定 expertOut 或 Combine 通信 dtype |
| 完成边界 | Host 提交返回是异步的；Dispatch 完整 kernel 完成后 metadata 可消费，调用方持有到 Combine 结束 |

原子的密排 t、8*BS 行 group 上限、核 63 清 counter、核末 SyncAll 均已退出当前 Dispatch。
**旧 relay 通信区仍只预留 8*BS*K*returnStride，Dispatch 不访问它；不能直接承载当前稀疏 t。**
沿用 recvSlot 契约的 Combine 至少要核算 Crows*K*memberStride，重新确认单窗总量、Host 布局及
caller 分配。实际 group 数少不意味着最大 t 小，不能继续使用“group 数≤8*BS”的旧安全上限替代。

已经约定的贡献关系：DIRECT 返回未加权结果，source 应用真实权重；终端返回未加权结果到 relay，
relay 从第一跳 assistEx 和 group members 取得真实权重、汇总一次后返回 partial；source 对 partial
乘 1.0。每条 expert 贡献恰好加权一次。第一跳从本地 firstHopRow 取值，其余本卡/跨卡成员共用
(t,k) 返回寻址，只等待 member 列表中的槽，不等待空洞。归约顺序为第一跳后按原始 k 升序，
source 按 mask 的 k 升序；两级括号不同，数值验收仍需 C15/C16 明确。

下一轮优先顺序：**dtype/数值与返回容量 → 核职责和 H 分块 → UB/GM 所有权、等待及完成 → 验收**。
多 stream、多个在途 plan、重复 Combine、失败重试及 Dispatch/Combine magic 配对未由当前 Dispatch
单 stream 双窗实现自动解决。相应公共契约直接更新主规格，不再将历史 R 文档当作当前事实来源。
本次只准备交接，不预先选择 Combine kernel 拆分、核数、返回 dtype 或新增窗口方案。

## 本轮迁入边界

**状态：下一轮 Combine 设计清单，尚未选择内部实现。**
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

Dispatch 持久输出、expertScales 复用、recvSlot、公共区域容量、ABI 和对外生命周期以主规格为准；
R04/R07/R08/R10/R11 仅保留历史来源。Combine 的返回 payload/stride、dtype 或跨调用协议一旦明确，
同时更新主规格的公共契约与本文内部设计，不能只修改一侧。

## C15

**议题：Combine 内部 dtype 与量化。来源：R08-03 的内部部分。**

需要明确 expert 输入转为乘法 dtype、group 累加、partial 通信、source 累加和最终输出的各次 cast/量化及 scale 生命周期。
当前布局函数中的非量化 returnStride 是历史预留，不是已完成的 Combine 返回格式。
Dispatch 的 BF16/FP16 非量化与 MXFP8 输出已实现；expertOut、member、partial 和最终输出的
公共 dtype/stride 与本项内部计算一起明确，并回写主规格，再重新核算完整返回区容量。
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
对外 event/plan 释放契约须在主规格完善（历史来源 R11），本项给出对应内部机制，不据此新增 X。

## C23

**议题：Combine 内部与端到端验收。来源：R12-02 及 R12-03/R12-04 的 Combine 部分。**

- **C23-01：** Combine-only 的合法 assistEx/sourceMask/group 和 Dispatch 输出的 expertScalesOut 输入如何独立生成，并保证 joint Dispatch-expert-Combine 测试不会同错互相抵消？有效行序和 group 识别按 R04-02/R04-03，槽位按 R04-05/R07-03，真实成员权重与 source 归约权重按 R04-07/R04-08，不新增 X 输入。
- **C23-02：** 如何验证第一跳与全部本卡/跨卡返回恰好一次、仅消费前 n 个 member、交错原始 k 的固定顺序，以及 source 按 mask 统一加权（DIRECT 真实权重、partial 乘 1.0）？需覆盖第一跳/成员保持真实输入权重、独立 expertScalesOut 保存 source 归约权重、mask 空洞不等待且不消费、真实 DIRECT weight 为 0/1 的正常贡献；误差按 C16 落实。
- **C23-03：** 哪些规模、路由和偏斜属于 Combine/端到端性能目标，允许多大退化，如何与普通 Memory 保持相同输入和数值契约？

安全矩阵覆盖零 group、稀疏 group、t>=8*BS、高 row、group 有效数与最大 t 明显不同、仅本卡转发、不同 source 同 TokenId、`tokenNum=0`、慢成员和连续轮次；可寻址上界按 Crows 和实际可达路由构造，不要求 X 字段。
同时覆盖 group 数大于旧 BS 上限、n=1 仅监听一个返回、本卡/跨卡混合及原始 k 有空洞；可达边界按所选 Rank 数和路由构造，K=1 时无 group。
合法数据面用例保持不重复 top-k 和有效 expert/mask；容量校验及错误处理需按新的 Combine Host 契约单独设计。当前 BS>0，不将 BS=0 当作已支持。

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
- **C24-02：** 累加器、发送/接收 UB、GM scratch 如何分配和复用，生命周期是否允许重叠？确定后回写主规格的通信区预算，不能在此单方面修改 Dispatch recvSlot 编码。
- **C24-03：** 新 invocation 与旧在途发送/远端消费如何隔离，内部机制如何满足主规格中待完善的完成和公共窗口复用契约（历史来源 R11）？

Magic 选取 ping-pong 窗口不等于旧 Payload Flag 自动失效；必须结合正确清理、调用次序和资源完成条件。
X 和尽力转发不是当前实现；本项须与新的可寻址容量及 Host 校验契约一致，不继承旧 8*BS 预算的免校验结论。

## 编号映射

下表为历史编号索引，条目状态按当前主规格和本文交接基线解释。

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
| [C09](#c09) | Workspace 容量、Slot 复用与流控 | 历史原子编号和 8*BS 容量已被 t=row/Crows 契约替代；t*K+k 编码保留，返回区扩容和跨轮复用待完善 | [R06](ep-memory-server-dedup-open-topics.md#r06)、[R07](ep-memory-server-dedup-open-topics.md#r07)、[R11](ep-memory-server-dedup-open-topics.md#r11) |
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

**Workspace 容量、Slot 复用与流控。** 历史原子编号和 8*BS 容量已被 t=row/Crows 契约替代；t*K+k 编码保留，返回区扩容和跨轮复用待完善。后续见 [R06](ep-memory-server-dedup-open-topics.md#r06)、[R07](ep-memory-server-dedup-open-topics.md#r07)、[R11](ep-memory-server-dedup-open-topics.md#r11)。

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
