# AIV RDMA WRITE：按 QP 容量自适应的在途窗口

## 状态与范围

2026-11-06：用户已确认默认窗口按每个 QP 的 SQ/WR 能力自适应，采用下述
WQEBB 换算公式，不再使用固定 128 WR 或两个 pending batch 的限制。
用户随后确认完整方案并授权开始实现；设备验证仍单独等待恢复通知。

本文修订 `2026-11-05-rdma-explicit-completion-design.md` 中的双批次状态方案，
保留 WRITE 提交 / 应用显式 `RDMAWait` 的完成语义。继续使用
`codex/rdma-explicit-wait` worktree、C++14 和现有 CANN 支持边界。

不修改 Host Put/Get、legacy all-signaled API、WQE 数据格式、UB/MTE3 发布和
`st_dev` doorbell 顺序，不引入 SQ/CQ 物理环绕复用或多 AIV 共享 QP。
绿区部署、编译及设备测试仍暂停，不因本设计获批而自动恢复。

## 默认窗口与批次

对当次 WRITE 的 SGE 数，使用现有 `RdmaWire::WqeBlocks`：

```text
L = ceil((48 + 16 × numSge) / 64)
sqWrCapacity = floor((sqDepth - 1) / L)
defaultWindow = min(sqWrCapacity, maxSendWr)
effectiveWindow = min(defaultWindow, applicationLimit)
```

- `sqDepth` 的单位是 64-byte WQEBB，预留一个 invalid stop WQEBB；
  `maxSendWr` 和窗口的单位是 WR，不以 payload 字节数换算。
- `RDMAWriteOptions` 和内部 `PipelineOptions` 默认不施加额外应用限流；
  用 `UINT32_MAX` 表示这一默认值，在获得实际 view 后计算 `effectiveWindow`。
  显式设置的较小正值仍是应用上限；零值保持非法，不新增配置开关。
- `postBatch` 保留 1..64，表示一次发布的最大 WR 数，不表示在途批次数。
  实际批长上限为 `min(postBatch, effectiveWindow)`，最后一批可更短。
  窗口小于两批、甚至小于原始 `postBatch` 时，不再仅因此拒绝请求。
- 提交下一批前检查实际 pending WR、SQ WQEBB 和 CQ/pending 记录容量。
  空间足够就提交；不足时仅回收足以继续发送的最老完成，不无条件排空。
  窗口是安全上限，不承诺每个时刻都达到上限；完整批次可能留下不足一批的余量。
- 跨调用改变 SGE 数时，SQ 占用使用实际 `producer - completed`，不能用
  pending WR 数乘以本次 `L` 推测此前批次的占用。

每个发布批次仍仅给最后一个 WR 设置完成信号，正常完成路径下一批对应一个
CQE。CQ/pending 容量独立约束，不把 CQ 深度直接当作 WR 深度。

## 每个 QP 的 pending 存储

`TileXRRDMADeviceState::reserved[2..3]` 不再作为两个记录槽。
Host 为每个 QP 配置独立、由 transport 拥有的 HBM pending 记录缓冲区：

```text
recordCapacity = min(maxSendWr, sqDepth - 1, cqDepth - 1)
recordBytes = recordCapacity × 8
```

此分配覆盖最坏的一 WR 一批；普通固定批长的实际 pending 记录数随窗口和
批长变化。不能只按当前 `ceil(window / postBatch)` 分配，因为同一 QP 的
后续调用可以改变批长，也可以连续提交单 WR 的尾批。

每项继续编码批次结束 SQ 位置、CQ 匹配的最后 WR 低 16 位和该批 WR 数。
完整 next-WR ordinal、pending 生产序号及累计 pending WR 数保留在 QP state。
状态头继续为 64 字节，不在 AIV 栈上引入随队列深度增长的数组。

- 记录按 FIFO 消费；软件记录缓冲区可复用已确认完成的槽位，不代表硬件
  SQ/CQ 可以环绕复用。
- 只有成功匹配 CQE 后，才推进 `completed` / `cqConsumer`、扣除该批 WR 数
  并回收记录槽位；错误时保留 pending 和 sticky error。
- pending 缓冲区与 QP state 一样由所有 MR views 共享，随 QP 生命周期管理，
  不在 MR 注册或每次 WRITE/Wait 时重置，不交由 NIC 注册或读取。
- 复用 provider 现有 allocation ledger 和清理机制，不增加独立资源管理框架。

## Host/Device 兼容边界

`TileXRRDMADeviceQueue` / view 需要携带 pending 地址和容量，Host 导出时验证
对应分配；Device 在使用前验证版本、结构长度和地址范围。

更新 device-view 版本及结构大小断言，旧 Host 或旧 Kernel 不得将新布局作为
原 V2 view 使用。Host 库和调用 Kernel 必须配套重新构建；不增加旧布局回退。
单 view 和数组 view 的 Host C 导出符号也升级为 V3，仅保留源代码别名，
防止旧调用方的较小输出缓冲区被新库覆盖。
UBNIC V2 硬件队列格式不变，不借此修改其 queue ABI 标识。

涉及的主要实现边界：

- `src/comm/rdma/tilexr_rdma_nda.cpp`：QP pending 分配、导出验证及生命周期。
- `src/include/tilexr_rdma_device_types.h`：view 版本、pending 描述和状态说明。
- `src/include/tilexr_rdma_device.h`：view 读取及 pending 元数据 I/O。
- `src/include/tilexr_rdma_pipeline.h`：容量换算、FIFO、跨调用背压与显式等待。
- `src/include/tilexr_rdma_write_options.h` 和 `tilexr_rdma_write.h`：默认值与使用要求。
- `tools/rdma_compare`：多 QP 基准的 admission/说明改为实际窗口；计时仍包含
  显式 Wait，CPU-only describe 不伪造只有创建 QP 后才能知道的实际容量。
  旧 matched QP1/NDA 对比程序显式设置应用 cap=128，保留双方原有比较条件；
  这不是公共默认值，也不限制多 QP 基准。

## 保留的发送与失败边界

1. 在 drain、状态预留、pending 写入及 WQE 发布之前，完成参数和整次调用的
   SQ/CQ no-wrap admission；CQ 预算包括已有 pending 和本次将产生的批次。
2. 原有 producer / completed 的绝对 WQEBB 计数及物理 stop 预留继续生效。
   Wait 只能回收在途窗口，不能使已走过的硬件物理区间重新可用。
3. WRITE 成功仅表示提交；`RDMAWait` 排空所选 QP 的全部 pending，包含此前
   调用和其他 MR view；空/重复 Wait 不额外轮询。
4. timeout、CQE error、mismatch 保持有限轮询和 poisoned-QP 语义，不自动重试、
   复位、取消已发布 WR，或提前释放仍可能被访问的内存。
5. 一个 AIV 独占一个 QP；成功 Wait 不替代远端 consumer 同步。

## 验收与验证

复用 `tests/rdma` 的真实 production algorithm / adapter 和现有 fake provider，
不增加源码文本匹配或新的测试框架。

- 不同 SQ 深度、`maxSendWr`、SGE 数产生公式要求的默认窗口；应用上限被保留。
- batch=16、window=128 等场景能保留超过两批，不因固定槽位过早读取 CQ。
- 默认在合适队列上能保留超过 128 WR；在 WR/SQ/CQ 限制下不会越界。
- 跨调用改变 batch、SGE、应用窗口；混合 WQEBB 长度，记录槽复用后仍按 FIFO
  匹配完成，producer/completed/WR/CQ 单位保持独立。
- 小窗口、尾批、显式排空、空/重复 Wait、MR 共享状态和多 QP 独立性。
- 参数和整次调用 no-wrap 拒绝仍无 drain、发布或状态变更；保留错误路径覆盖。
- Host 分配/导出、C header、版本/ABI 拒绝及 provider 清理行为。
- 运行现有 RDMA CPU 回归；CPU 证据只证明算法和 Host/Device adapter 模型。
  配套 CANN 构建和实际数据面/性能验证须等用户另行恢复绿区 EXEC 测试后执行。

仅新增 pending 记录不足以宣称性能提高；未取得设备证据前不报告吞吐提升。
设计文档随后续实现一起交付，不单独创建 spec-only commit。

## 本次实现验证（2026-11-06，蓝区 CPU）

- 新增 batch16/window128 回归先在旧实现失败：旧固定两槽逻辑提前读取 CQ；
  新实现可保留 8 批。默认窗口的 1000 WR 用例也不在提交期间轮询 CQ。
- `cmake -S tests/rdma -B <cpu-build>`、构建及完整 `ctest`：14/14 通过。
  包含公式、SQ stop 边界、混合 SGE 跨调用、FIFO 槽复用、显式 Wait、错误保留、
  多 QP adapter、旧 view 拒绝和 C header 检查。
- Release 加 `-D_GLIBCXX_DEBUG -D_GLIBCXX_ASSERTIONS` 后重建 pipeline/API
  两项核心回归：2/2 通过。该检查不是 sanitizer，也不模拟 NPU 缓存/MTE。
- `nm` 检查 Host API object：单 view 和数组 view 均为 V3 C 符号；
  `git diff --check` 通过。主流程进行了最终 diff 审查，未进行独立审查。
- 当前可访问的环境缺少匹配 `ibv_extend.h`，完整 NDA fake-provider 用例未启用；
  配套 NDA/CANN 编译、多 QP benchmark 编译及硬件运行尚未验证。ASan/UBSan
  配置在编译器链接自检阶段因缺少 `libasan`/`libubsan` 失败，未执行 sanitizer。
- 未向绿区派发任务，未访问 184 或使用 NPU；设备正确性和性能不在上述证据范围内。

维护要点：pending 记录容量必须覆盖变化的 batch 和单 WR 尾批；WR、WQEBB、CQE
不能混用单位。扩大 Host 输出 POD 时，除 view 版本与长度外，还须同步版本化所有
写入该 POD 的 C 导出符号，防止旧调用方提供较小缓冲区。
