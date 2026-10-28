# TileRelay：共享 GEMM 流水线与 FP32 push 归约

## 状态与范围

2026-10-27 用户批准了“共享 Python GEMM 双缓冲流水线 + AR push 通信”的
设计方向，并随后确认书面 spec、要求继续实现。

验收更新：两项均已实现并通过组合产物的 24-specialization 数值回归。
正式双种子对照确认共享 GEMM 有收益，但 FP32 push 相对原 pull 慢约 10%，
三组减少读回/合并 DMA 的实验未恢复收益。用户随后批准收敛交付为共享 GEMM，
保留原 pull，删除 push 专用布局、workspace、artifact 标识及相应测试。
最终源码只涉及 `device/gemm.py` 和两个算子 generator；原 ABI/Plan/peer
协议不变。下文的 push 相关设计保留为未采纳实验记录，不是生产合同。
最终 GEMM-only 24 种形状、双卡 48 轮 execution 与 bindings 回归均已通过；
两 rank 各验证 96/48 次调用，29 项 Python 合同测试、12 项分析器测试和
C++14 Host mock 1/1 通过，独立复审未发现具体问题。完整结果见
[验证记录](../TILELANG_ALLGATHER_GEMM_VALIDATION.md#2026-10-27-gemm-pipeline-and-fp32-push-acceptance-experiment)。

基线为 `3994dd34dc822a59d42c9834d24143ff7073ec60`。本设计扩展
[Core 双槽协议](2026-10-14-tilerelay-core-design.md#two-slot-lookahead-pipeline)，
保留 [ABI-v1-only](2026-10-19-tilerelay-v1-only-design.md) 与
[artifact-bound peer flags](2026-10-21-tilerelay-fixed-peer-layout-design.md)。
覆盖两个 rank、910B、CANN 9.1、当前固定编译器；不据此声称 A5/UDMA 已验证。

目标是减少大形状的 GEMM 加载串行和 AR 远端读取开销，同时保持小形状性能、
公开数学语义和每次调用一个 mixed kernel。实验假设不是收益承诺。

不做：Host pipeline、ABI-v0、额外通信/转换 kernel、动态任务调度器、20 核
AG gang 重分配、FP16 partial、第三方编译器修改、按 shape 维护算法回退表。
`reference/` 仅供比较，不成为生产依赖。

## 证据与设计取舍

[验证指南](../TILELANG_ALLGATHER_GEMM_VALIDATION.md#2026-10-26-fine-grained-profiling)
记录本次调查边界。大形状为每 rank rows/N/K = 1024/4096/4096：

- AG 插桩后的稳态 AIC 周期约 65% 在 GM→L1 加载及同步，等待 gathered-ready
  不足 0.1%。当前每 K 批次调用 gemm_v0 并完整排空，未实现跨批次 L1 预取。
- AR 插桩后 AIC 等待 Vector 归还 credit 约 26%；进一步分离读取的诊断中，
  AIV 远端读取占约 59–64%，本地读取约 12%。新增计时屏障改变了调度，不能
  把这些比例当作生产耗时精确分解，也不能据此承诺 push 更快。
- 两个随机种子的对照否定了三个直接替换：16→20 核没有加速，chunk 8→32
  行慢约 9–10%，tile-packed partial 没有稳定收益。本设计不采用这些替换。
- 官方 AR 精度探针与 FP16 partial 路径吻合，而 Tile 的合同是 FP32 partial
  SUM 后一次 FP16 转换。保留此合同，不通过减少精度降低通信量。

因此分别优化共享计算管线与 AR 的数据传输方向，先独立测量，再验证组合效果。

## 模块职责

| 模块 | 责任与边界 |
|---|---|
| `device/gemm.py`（新增） | 小型、可内联的 Python GEMM 宏；L1/L0 buffer、事件 credit、MMA 与 FIX 顺序；不感知 rank、peer flags 或算子 ID |
| `device/peer.py` | 保持 epoch publication/ACK 和双槽跨核 handoff；复用现有有序 DMA 宏，确有复用需要时才增加最小 push 原语 |
| `ops/allgather_gemm.py` | 保留 local-first、gang、gathered/output 布局与就绪条件；调用共享 GEMM |
| `ops/gemm_allreduce.py` | 调用共享 GEMM；组织 FP32 partial push、本地 SUM、ACK 和 local credit 生命周期 |
| `ops/plans.py` | compiler-free 算子布局与资源数学；生成 AR 私有 partial、incoming peer payload 和槽位绑定 |
| `scripts/build_tilexr_relay.py` | 从算子层取得固定布局描述，编译并生成匹配 artifact/Plan；不另设可独立编辑的通信配置 |
| native runtime / `device/abi.py` | 不改 ABI、注册、缓存、launch、prepared execution 或通用参数填充 |

不引入策略类层次、插件注册表、通用 DAG 或新的性能运行时。共享抽象只覆盖
两个算子实际共同使用的 GEMM；算子保留通信与调度所有权。

## 共享 GEMM 管线

保持输出 tile 128×256、FP16 A/B、FP32 L0C、`block_k=gcd(K,256)` 和 K64 MMA。
每个 AIC 使用两份 L1 A/B staging、两份 L0 A/B staging 和一份 L0C：

- 最大 L1 占用 `2*(128+256)*256*2 = 384 KiB`。
- L0A 为 32 KiB，L0B 为 64 KiB，L0C 为 128 KiB。编译及生成产物检查确认
  总实际分配，没有 helper 隐式再分配一套缓冲。
- 在有效 tile 序列外初始化事件 credit，退出时配对排空；不在每个 K 批次
  重新初始化/排空完整管线。零工作 worker 也不得出现无来源的 wait。
- 当前 L1 stage 的最后一次 L1→L0 消费完成后才允许该 stage 被重新装载；
  当前 L0 stage 的 MMA 消费完成后才允许覆盖。预取下一 K 批次与当前 MMA 重叠。
- 每个输出 tile 仅第一次 MMA 清空累加器；所有 K 项按原顺序参与 FP32 累加。
  保持 K=64/128/192/256/384 等情况，不能照搬上游示例的 `kk==3` 假设；
  microtile 的 ping-pong 索引与释放条件须覆盖只有一个 K64 子块的 L1 批次。
- L0C 在前一次 FIX 消费结束前不清空/覆盖；写出 AG FP16 或 AR FP32 由
  调用算子指定。完成通知必须仍受 FIX 完成顺序约束。
- 热 K 循环用明确的生产/消费事件替代 PIPE_ALL，不只是删除屏障或套一层
  T.Pipelined。单次初始化/结束处可保留必要的完成同步。

宏接口使用显式 tensor/buffer 与 row/column offset，构建期内联；实现以固定
编译器已有的低层 MMA 和 copy 能力为限。算子提供有效输入范围和输出位置。
AG remote 输入只有在对应 tile 的 ready 被消费之后才能预取；本期不预取
尚未就绪的未来通信 tile，也不更改双槽 outstanding-work 上限。

## AR push 数据布局

数学语义仍是 `C=FP16(FP32(A0@B0)+FP32(A1@B1))`。每个 rank 持有：

- private `partial[rows,n]`，FP32，由本 rank AIC 写入；每次执行完整覆盖有效
  区域，使用 `UNINITIALIZED` workspace，不为计时或正确性增加 Host 清零。
- owner peer payload `incoming[rows,n]`，FP32，仅由对端 AIV 写入。
- peer payload 映射 `outgoing`，指向对端的 incoming；不是本地 partial 的别名。

peer payload 大小保持 `rows*n*4`，新增 private workspace 同样为 `rows*n*4`。
当前大形状每 rank 增加 16 MiB。保留完整行主序 staging，不引入 GM ring、
打包转换或 partial 压缩；workspace 生命周期遵循现有 Plan/Execution 合同。

canonical ABI 槽位如下，所有指针仍由通用 runtime 填充：

| Slot | 含义 | Plan 来源 |
|---:|---|---|
| 0 / 1 / 2 | A / B / output | 现有三个 USER bindings，不变 |
| 3 | local FP32 partial | WORKSPACE 的 private region |
| 4 | outgoing FP32 partial | PEER_PAYLOAD |
| 5 / 6 | owner / peer epoch flags | 原 flag slots 和固定 publication/ACK planes |
| 7 | incoming FP32 partial | OWNER_PAYLOAD |
| 8–15 | unused | 保持 padding |

在 `ops/plans.py` 定义最小不可变 AR push 布局值，由经过校验的 artifact 派生。
它统一给出 buffer 角色、shape、dtype、slot 和容量；planner 与 generator
消费它，不能各写一套资源公式。现有 `PeerMemoryLayout` 继续负责固定 flags，
引用同一个 artifact-bound 对象，不为 push 虚构新 channel。

### Artifact 与旧产物边界

新 AR artifact 的 specialization 增加固定 `payload_layout="ar_push_v1"`
标识，由算子层设置，不提供 CLI 算法选择开关。生成器及公开 AR planner 必须
拒绝缺失/不匹配的标识，避免用旧 pull ELF 生成新的 workspace/指针 Plan。
这不是从文件名猜测布局，也不增加 ELF 解析、重算哈希或 native 算子分支。

已生成的完整旧 ABI-v1 Plan+ELF 可继续经通用 JSON/runtime 路径加载；不得
把它们交给新 AR planner 重新解释。重新构建生成匹配的新产物，不重写历史
artifact/install 目录。两 rank 必须使用匹配的 artifact 与 Plan，不能混跑
pull/push。本期不维护两个生产 generator 或自动回退路径。

## AR 每 tile 的执行与同步

保持 worker 波次和两个本地通知槽，`s=wave%2`，两 AIV 分别负责 64 行：

1. AIC 在 wave≥2 时先收回 wave−2 的 local credit；完成 GEMM，FIX 写入
   private partial，随后 `cube_done(s)`。
2. AIV 等待 `vector_wait_done(s)`，从 private partial 读自己的半 tile，
   经 UB 用 MTE3 写到 outgoing 的相同坐标。两边都先 send、再等 peer，
   不允许先等待 incoming 而双方均未发送。
3. 本 AIV 半 tile 的全部 payload MTE3 完成后，发布 owner flag 的本 epoch。
   然后等待 peer flag 的本 epoch；它证明对端已完成写入本地 incoming。
4. 从本地 partial 与本地 incoming 读取，执行 FP32 SUM，再转换一次 FP16
   写 output。若有收益，可在 UB 中保留发送的本地值以减少重复 local read；
   必须满足实际 UB 容量及 producer/consumer 依赖，不能新增 FP16 中间舍入。
5. incoming 消费及 output 写回完成后发布 ACK；观察 peer ACK 后，归还
   `vector_ready(s)`。AIC 按实际 worker waves 排空最后至多两个 credit。

保留两 AIV 的 mode-2 handoff，publication/ACK 仍按全 tile ID 寻址，不按
slot parity 压缩。payload 不与 flag 内存重叠。前一代所有有效 tile 的 peer
ACK 均已观察后，才能退出并允许下一代复用 peer payload，包括混合算子/形状。

通过完成依赖证明顺序，不能把指令已下发当作 DMA 已完成。现有双槽协议不变，
不得为了修复设备同步问题添加 Host 每 tile barrier 或额外 kernel。

## 验证与验收

### 正确性

- 扩展现有 `tests/relay/test_python_contracts.py`：AR layout 标识拒绝、
  workspace/slot 合同、JSON round-trip。只增加不能由数值 E2E 证明的边界测试；
  不新增 source-text guards、单独 MMA 测试框架或重复的算术 unit tests。
- 在目标固定工具链编译 AG/AR，复用 `tests/relay/hardware.py` 验证 one-shot、
  prepared bindings/execution、双 stream、反复 epoch、混合算子与形状、延迟
  rank、父子对象生命周期。正确性序列不得新加逐代 Host barrier 掩盖复用问题。
- 保留 FP32 partial 区分探针、NaN 输出覆盖检查和 CPU FP32 reference。
  覆盖 K=64/128/192/256/384/32768、零工作 worker、一/二/奇数/偶数波次及尾波。
  优先复用已保留的 22 个 specialization，按未覆盖的风险补充而非重复罗列。
- 核查编译资源、事件配对和一调用一 mixed kernel；不改第三方 compiler/helper
  来绕过失败，不把 Host/mock 验证视为设备通信证明。

### 性能

隔离产物目录对照原基线、仅共享 GEMM、仅 push、两项组合，以及安装包官方
融合算子；中间方案仅是任务内归因产物，不成为生产选项。保留可复现的历史
串行基线结果列，不能把历史数据标作本轮重测；若重测，明确来源与统一口径。

至少覆盖 rows/N/K 为 128/256/256、256/2048/2048、512/2048/2048、
1024/4096/4096。每组至少 10 warmup + 30 samples、两个随机顺序种子，
相同 profiler/barrier 状态，逐次校验。记录各 rank 分布及每轮最大 rank
再汇总的 median/min/max；报告 Host 完成延迟与 device span，官方 AR span
包含 AICPU 与相关通信，不能只拿 AICore 子任务冒充完整融合延迟。

PMU 和阶段插桩另跑诊断产物；纯性能产物关闭 kernel 计时/DFX。没有元数据的
PMU 字段标为缺失，不当作零；通信算子不做 kernel-only replay。

验收以大形状两组种子均有可重复收益为必要条件，不预先承诺追平官方。
以任一回归点较基线慢 `max(3us,5%)` 作为必须重测/解释的阈值，而非允许
掩盖回归的误差带。push 还必须相对“仅共享 GEMM”有稳定收益，才能用性能
理由引入其额外 workspace。若条件不成立，继续定位或报告未达标，不保留
无收益的备用分支，也不通过改变精度、删校验或选择性展示样本达标。

测试前检查设备占用，复用既有环境资格记录，不重复 HCCL Test。新设备故障
按仓库环境基线规则处理，不通过算子改码绕过未经证明的环境问题。

## 实施顺序与交付

书面 spec 批准后形成执行计划，按以下依赖推进：

1. 固定基线证据；落实 AR 不可变布局、artifact 标识及现有合同测试。
2. 实现共享 GEMM 宏并接入两个算子，先验证数值、事件生命周期和单项性能。
3. 实现 AR push，分别验证原 GEMM+push 与组合产物，保留独立归因结果。
4. 完成小中大形状与尾波/epoch 回归、细粒度复测及正式性能对照。
5. 简化代码、独立审查同步/布局合同，更新 integration/validation 指南。
   保留已验证的唯一生产路径，spec 随实现一同提交，不做 spec-only commit。

“统一修改”指一次一致的生产设计与最终交付，不省略中间的隔离测量和失败验证。
