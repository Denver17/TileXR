# Server Dedup cnt 发送掩盖优化方案

状态：方案 A 已实现（2026-09-17）；Host 回归和 CANN 9.1 / A5 OFF、ON 编译通过，设备正确性与收益待 A5 实测。保持现有计数、跨核求和、双窗和输出协议。

## 目标与边界

发送核在下发 cnt 后立即进行 send_prepare；接收核立即轮询 incoming cnt，计算局部和及局部相对前缀。cnt 发送源在 MTE3 完成前保持不变。全局前缀仍依赖各接收核通过 MTE3 发布局部和，因此不承诺整个 cumsum 独立于本核 cnt 发送完成。

不改跨核求和协议，不复制全量 cnt 到每个接收核，不改首次 senddata lane 等待，不改 FP8 模式和对外 API。

## UB 生命周期

将发送记录与筛选 scratch 分开。记录长度为 `32 * align8(ceil(experts / countCores))`；384 专家、64 个计数核时保留 256B，其中有效记录为 192B。histogram 的 32B 放入 scratch。

独立记录区之后申请一个共享 UB pool，count scratch 与后续 sender / recv-count 各使用独立 pool 视图。筛选结束已有 V_S 依赖保证 scratch 的计算结束；下一阶段可以覆盖 scratch，但不能覆盖记录区。池对象在使用期间保持有效，不通过 Reset 切换视图。sender 的 lane 预算扣除记录保留区。

核对 blue 安装的 CANN 9.1 `dav_3510/kernel_tpipe_impl_c310.h` 和 `kernel_tpipe_impl_c310_vec.h`：TPipe::Reset 包含 MTE2/MTE3 屏障；UB TBufPool::Reset 包含 MTE2/MTE3/V 屏障。因此两者均不能用来实现重叠窗口内的无等待回收。除 UB 地址外，也须正确分配池的 buffer 元数据及队列 mutex 资源。

实现使用 stagePool 的 16 个 buffer 描述槽和 countPool 的 5 个槽。sender 首先直接在 stagePool 中分配两个输入 queue buffer，再经 TPipe::InitBufPool 创建覆盖该前缀的 cleanPool。CANN 9.1/A5 实编译发现，不同模板特化之间的嵌套 TBufPool::InitBufPool 会因 protected 成员访问失败；不要仅凭同名通用 API 或 Host 替身推断它可用。此次使用的 TPipe 共享视图与直接 queue 分配已通过 A5 OFF/ON 编译。

## 调度和所有权

1. SendCnt 保留 S_MTE3 发布依赖，发出所有记录后直接返回，删除尾部 PIPE_ALL 和 TPipe::Reset。CTRL[60] 延续 Init 设置的 0；Finish 保留原模式恢复。未参与计数或 expert 区间为空的核不保留发送记录。
2. sender 使用共享 scratch 布局完成准备；现有首次 lane 等待发生在准备之后，允许在此承担未覆盖的发送尾延迟。记录保留到后续完成边界，不为提前回收这几百字节插入同步。
3. receiver 读齐自己负责的 incoming cnt 后，先 Gather/Sum，再在 UB 中形成相对前缀，然后发布局部和。局部前缀计算移到等待跨核结果之前。
4. cnt 状态清理移到局部和发布之后；删除清理前后的 MTE3_S。清理源 waitStatusBuf 的 ready 字段置零后保持只读，直到现有 count/receiver 边界完成等待。
5. 删除发布局部和后立即执行的 MTE3_V。sumCoreBuf 在真正复用为 prefix-ready 标志源之前等待 MTE3，避免覆盖尚未读取的局部和。GetCumSum 返回本核全局起始偏移，再加到相对前缀上。
6. 保留进入 recvdata 之前的完成屏障及 receiver Reset；cnt、状态清理、局部和和最终前缀输出均在此退休。发送核保留末尾完成保证。有效分段即使所有 cnt 都为零也参与求和；未分配 cnt 记录的接收核跳过局部和与 cumsum，仅等待最终 prefix。

MTE3 队列顺序仍为 cnt、局部和、清理和前缀输出。可覆盖窗口来自 recv_cnt、局部求和与局部前缀计算；残留 cnt 等待近似为 `max(0, cnt 发送耗时 - 可独立推进时间)`，实际覆盖比例由 A5 测量决定。

前置局部前缀后需要再遍历一次本核分段以加上全局偏移；当前默认 E384/32 接收核时为前 24 核各 16 项。此计算代价与 pool 管理开销也必须计入完整阶段比较。

## cnt 检测按 512B 分配（2026-09-24）

同一 512B 块被多个核轮询会增加对端写入开销。原紧凑 GM 布局因此将接收分段设为 16 条的最小分配粒度；下述 GM 槽位扩展后仍保留该分工。令 E 为本 rank 的 cnt 条数、C 为接收核数：

```
recordsPerCore = alignUp(ceilDiv(E, C), 16)
activeCores = ceilDiv(E, recordsPerCore)
receiver r < activeCores: [r * recordsPerCore, min((r + 1) * recordsPerCore, E))
```

cnt 表起点及窗口基址已有 512B 对齐约束；末段只读取实际记录，不能等候补齐位置的 ready。E384/C32 使用前 24 核，每核 16 条；E16/C32 仅第 0 核处理 16 条；E24/C32 使用前两核，分别处理 16、8 条。SendCnt 的专家分工保持原协议，GM 地址使用下述 512B 槽位。

只跳过空闲核的 cnt 轮询还不够：若它们继续发布零总和，或活跃核仍等待 C 个发布者，就没有真正退出 cumsum，甚至会在删除零发布后挂起。因此分段总和只在前 activeCores 个核之间交换，发布数、监听数、求和数和清理数同步缩减；矩阵行距仍为 C*32B。最终 prefix 分段及其 ready 仍发布给全部 C 个接收核，但每个消费者只等待前 activeCores 个发布者，然后读取自己的完整 prefix 副本。空闲核不分配 cnt/cumsum 工作缓冲，也不访问分段总和矩阵。

Host 回归通过实际 GM 读取检查每个 512B cnt 块只有一个监听核，覆盖尾段、单核及非 8 整数倍活跃核、全零流量、双窗多轮、空闲核无 cnt/cumsum IO、全部接收核的完整 prefix 和就绪清理。延迟 MTE3 用例继续验证 cnt 在途时接收 scratch 的复用。该模型不模拟缓存争用，性能收益须由 A5 实测确认。

## GM cnt 独占 512B 槽位（2026-09-24）

双方 GM 中每条原始 cnt 的起点间隔改为 512B，只有槽首 32B 有效；发送和接收 UB 都保持 32B 紧凑排列。`CountRecordBytes=32` 与 `CountGmStride=512` 必须分开，不能扩大 UB 预算或 cumsum/ready 矩阵中的 32B 单元。

目标地址为 `cntBase + (localExpert * worldSize + sourceRank) * 512`。SendCnt 的 UB 源连续，DataCopy 每块仍为 32B，目标块间跳过 `worldSize*16-1` 个 32B 块。轮询使用 `srcStride=15,dstStride=0` 把 GM 槽首聚集至紧凑 UB；清理反向使用 `srcStride=0,dstStride=15`。480B 空隙既不读取也不写入。BS1/E384/64P 下每核仍发送 6 条、192B，UB 记录区仍保留 256B；每个活跃接收核扫描 16 个 GM 槽位、覆盖 8KiB 槽位区，但每轮只请求读取 512B 有效记录。

固定 cnt 区从 32KiB 扩为 512KiB；其后的 Dispatch 控制表及 Combine 控制区顺延 480KiB，共用数据起点从 1MiB 改为 1.5MiB。每个半区都采用新布局，容量校验计入新的完整前缀。Dispatch/Combine 的 Host、kernel、consumer 和边界验证工具须配套重建；工作流布局标识为 `r13-count-gm512`。具体偏移见联合设计 §6。

回归必须检查全部来源/专家槽位的计数值，保护每条 cnt 后的 480B 间隙及未使用槽位，并观测 DataCopy 的有效块长、双向 stride 和 UB 紧凑性；仅比较最终 prefix 无法证明没有多搬运 GM 空隙。双窗多轮和延迟发送源测试继续覆盖清理及 UB 生命周期。Host 检查不证明实际缓存流量或性能改善。

迁移共享数据前缀时，同时更新 Host 校验测试的合法通信容量和独立预期字节数；只改地址期望、遗漏 fixture 容量，会使合法请求在进入 launch 前被容量校验拒绝。本次 Combine fixture 改为显式组合共享前缀、结果槽、中转行和 runtime 保留区。

## 打点语义

旧 SendCntSend 事件保持原身份，供历史 capture 解析。新实现追加 SendCntIssue / send_cnt_issue，表示依赖发布及指令下发，不表示 MTE3 已完成。SendCntFilter 保持语义。recv_cnt 现在包含局部前缀计算，cumsum 包含跨核结果等待及偏移修正；比较完整 recv_cnt+cumsum 和 kernel 时间，不能把下发区间缩短当作通信提速。

## 实施顺序与验证

1. 修改 counts 的 UB 分离、共享池和接收调度；修改 sender 分配入口及主 kernel 的交接，保持 sender 输入队列的正确 alias。
2. 追加下发事件和 mapping，更新解析回归及计时文档。
3. 更新 Host API 替身的真实 pool 地址复用模型，验证延迟读取发送源时记录仍不变、预算和分段前缀正确，覆盖空分段、零流量、全核/接收核计数、双窗多轮和 profiling OFF/ON。Host 模型不证明设备异步时序。
4. 在 blue 执行聚焦 Host 回归和 CANN 9.1 / A5 kernel OFF/ON 编译，保留源码身份、编译器和日志。独立审查 UB 所有权与同步顺序。
5. 用户 A5 环境验证全核 cnt + BF16→MXFP8 E4M3 的历史敏感配置及非量化路径；固定 shape、核数、阈值、profiler 状态，对比各 rank median/min/max 的总耗时和分段。无实机证据时不宣称正确性或性能收益已验证。

## 验证记录

基线 `8194752` 加本次修改，blue 使用 CANN 9.1.0 / Bisheng Clang 15.0.5，A5 `dav-c310-vec` kernel profiling OFF、ON 编译链接通过。默认 48/16、阈值 48 的 counts OFF/ON、source_send OFF/ON、receive_packet OFF/ON 和 profile 共 7 项 Host 检查通过；32/32、阈值 128 和 40/24、阈值 0 各 4 项 counts/source_send OFF/ON 检查通过。21 项 Python trace 检查通过，覆盖历史发送区间和新增下发区间。

新增延迟 DMA 模型在实际生产 SendCnt→sender、SendCnt→RecvCnt 路径复用 scratch 后才读取 cnt 源，核对记录与输出，并检查准备/接收阶段在 cnt 未完成时能继续执行。独立审查未发现阻断问题，确认 sender 最多占 stagePool 的 14/16 个条目、receiver 最多 10 个条目，以及真正覆盖 sumCoreBuf 前的完成等待。

源码清单、补丁、编译配置、日志和二进制摘要保留于忽略目录 `build/cnt-overlap-verification/`。未运行 NPU；Host 模型不证明真实 MTE 时序、FP8 数值行为、通信带宽或覆盖比例。

2026-09-24：512B cnt 分段变更基于 `2feae84` 加工作区修改。默认 32/32 的 counts OFF/ON、source_send、receive_packet 共 4 项 Host 回归通过；16/48、24/40、40/24、48/16 的 counts 各 1 项通过，共 8 项。blue 本机共享 CANN 9.1.0 / Bisheng Clang 15.0.5 的 A5 `dav-c310-vec` kernel（profiling OFF）编译链接通过，配置和构建退出码均为 0。源码摘要、补丁、配置、测试与编译日志保存在 `build/cnt-block-verification/`。未运行 NPU，512B 争用改善及实际耗时尚待 A5 验证。

2026-09-24：GM cnt 槽位扩为 512B，基于 `9925446` 加工作区修改。默认分核的 15 项聚焦 Host 检查在更新 Combine 容量 fixture/断言后全部通过，覆盖 counts OFF/ON、source_send、receive_packet、Dispatch/Combine 布局与 Host 校验、边界和 Combine M1–M5。其余 4 种分核 counts 各通过 1 项；dedup 关闭模式的 counts/source_send/两方布局/Combine Host 校验共 5 项通过。Python 3.9 的 6 项边界工作流检查通过。额外运行的旧 `test_single_artifact` 和 `test_build` ABI 用例在基线源码复现相同失败：fixture 缺少模式/协议字段、沿用旧单构建目录，未归为本次回归；完整 manifest 的独立检查确认 r13 被接受、r12 被拒绝。

本机 CANN 9.1.0 / Bisheng Clang 15.0.5 的 A5 `dav-c310-vec` Dispatch、Combine kernel（profiling OFF）及所属 `tilexr-ep` Host 库编译链接通过。源码、补丁、构建配置、日志、失败/修复对照及二进制摘要保留在 `build/cnt-stride-verification/`。未运行 NPU；512B 布局的设备正确性、缓存流量及性能收益仍需 A5 实测。
