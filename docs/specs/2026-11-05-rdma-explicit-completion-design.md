# AIV RDMA WRITE：提交与完成等待分离

## 状态与目标

2026-11-05：用户已确认使用独立 worktree，按“WRITE 提交、应用显式等待”方向优化。
用户已确认本文并授权按此实现。设计随实现一起提交，不单独提交设计文件。

目标是在应用需要的位置等待 RDMA 完成，而不是每次 WRITE 调用结束都排空 CQ。
保留 C++14、CANN 9.1 和当前 Ascend950 AIV 路径；不扩大到 Host RDMA、
legacy all-signaled API、物理 SQ/CQ 环回或新的异步执行框架。

## 接口与返回值说明

- `RDMAWrite`、`RDMAWriteSge`、`RDMAWriteMultiQp` 和
  `RDMAWriteSgeMultiQp` 保持参数不变；成功表示本次 WR 已提交，不再表示传输完成。
  它们调用的 `RDMAPutPipeline` / `RDMAPutSgeListPipeline` 同步采用该约定。
- 新增 `RDMAWait(view, scratch, maxPolls = 1000000U)`，等待所选 QP 上此前提交的
  pipeline 批次全部完成；不是只等待当前 MR view 发出的 WR。
  复用 WRITE 已有的 UB scratch，不新增 scratch 分配接口。
- multi-QP 调用方在自己持有的 QP 上调用 `RDMAWait(views + qpIndex, ...)`。
  不引入隐式 QP 遍历、跨 AIV barrier、WaitAll 或自动数据分片。
- 没有 pending 批次时，合法的 Wait 成功返回且不轮询 CQ；重复 Wait 同样成立。
  原有参数边界、错误码和架构支持限制继续生效。
- WRITE 为满足两批次窗口和 WR 容量限制，仍可能等待最早的批次以形成背压。
  本次取消的是调用末尾的无条件排空，不承诺严格 non-blocking。
- Host `TileXRRDMAPut/Get` 以及非 pipeline 的 legacy device API 不改变完成语义。
  pipeline 模式仍不可切回 legacy 模式；同一 QP 不混用不同版本的 device 实现。

## 跨调用状态与发送流程

当前 `PutSgeListPipeline` 用栈上的 `ends[2]` / `lastWrs[2]` 跟踪最多两个批次，
末尾 drain 后信息即消失；直接删除 drain 会丢失完成记录，并使下一次调用返回 BUSY。

沿用 QP 共享的 `TileXRRDMADeviceState`，不改变其 64-byte 大小、字段偏移和 V2 view ABI：

- `producer` / `completed` 继续表示 SQ WQEBB 序号，`cqConsumer` 继续表示已消费 CQ 槽位。
- `reserved[0]` 保留 pipeline mode tag，`reserved[1]` 保留完整的 next-WR ordinal。
- `reserved[2]` / `reserved[3]` 保存最多两个 FIFO pending 批次。每项包含结束 SQ
  位置、最后 WR 的 CQ 匹配序号和 WR 数量；空项为零。
  非环回 SQ 的位置可用 32 bits；CQE 本就只匹配 WR ordinal 的低 16 bits；
  1..64 的批次 WR 数可用 7 bits，因此每项可放入一个现有的 64-bit 字段。
  完整 WR ordinal 不截断；WR 数不能用 WQEBB 差值代替，因为不同 SGE 数占用不同 SQ 空间。

发送与等待只共享这一组 pending 记录，不新增 Host handle 或独立队列对象：

1. 在任何 drain、状态预留和 WQE 发布前，完成本次参数与整段 SQ/CQ 非环回容量检查。
   CQ admission 包括此前 pending 批次及本次将产生的 CQE，不能只看 `cqConsumer`。
   保持原有行为：非法请求不发布到队列。
2. 每批发送前，根据已有 pending 批次数和 WR 数检查两批次窗口、`maxInFlight`
   与 `maxSendWr`。不足时消费最早批次；支持相邻调用使用不同 postBatch 或 SGE 数。
3. 继续使用 UB 构造 WQE、MTE3 发布 tail 再发布 first WQE，以及完成发布后才
   `st_dev` doorbell 的顺序。保留 stop WQE、fresh CQ snapshot 和 CQ CI 更新逻辑。
4. 将该批次的进度与 pending 记录写回 QP 状态；最后一批提交后直接返回。
   后续 WRITE 可以接着提交，Wait 则按 FIFO 消费所有 pending 批次。
5. 只有成功消费对应 CQE 才推进 `completed` 并移除 pending 记录。
   保持现有有限轮询与 sticky error；不增加自动重试、取消或队列复位。

QP 仍由一个 AIV 独占，跨调用、MR view、stream 和 kernel 的使用顺序由应用保证。
这是现有资源归属约定的延续，不新增并发访问能力。

## 资源与失败语义

WRITE 返回后，已完成 WQE 发布的 UB scratch 和 kernel-local SGE 描述符可复用；
NIC 仍可能读取的 source HBM、MR、QP、共享状态及相关 view 必须保留到完成。
应用可以先执行独立工作，再显式 Wait，不能修改尚未完成的发送数据。

背压和 Wait 都可能暴露 CQE 错误或 timeout。保持 QP 的 sticky error，失败不代表
发送已取消，也不允许立即释放可能仍被引用的内存；沿用 quiesce / destroy 的清理规则。
成功 Wait 提供所选 QP 的发送完成边界，不替代远端 consumer 的应用同步和缓存依赖。
仅对 submit-only kernel 调用 `aclrtSynchronizeStream`，不能替代显式 RDMA 完成等待。

## 调用方迁移与修改范围

修改 production pipeline、device adapter、public WRITE header 及相关状态说明；
复用现有 `tests/rdma` CPU fixture，不增加源码文本匹配测试或新的测试框架。

`tools/rdma_compare/multiqp_kernel.cpp` 在 WRITE 成功后、记录结束时间前调用 Wait，
并传播等待错误。benchmark 仍报告 completed-transfer 耗时，不把提前返回当作带宽提升。
更新 `docs/RDMA_API.md`、`docs/RDMA_INTEGRATION.md`、benchmark 指南及所有受影响的
pipeline 示例，明确 submit / wait / teardown 的边界。

## 验收与交付边界

- CPU 行为测试覆盖：小批次不等待尾部 CQE、跨调用 pending 保留、显式 Wait 排空、
  空与重复 Wait、两批次及 WR 背压、不同 batch / SGE、选定 QP 与共享 QP 状态。
- 保留并扩展非法参数、SQ/CQ 非环回边界、timeout / CQE error / mismatch、sticky
  error 及 ABI 测试；关键断言直接运行 production algorithm / adapter。
- CPU 测试不能证明 MTE / doorbell 顺序或 RDMA 数据面正确性。后续使用匹配版本的
  CANN / Ascend950 编译和设备验证，检查传输数据、完成状态、超时及 teardown。
- 性能对比使用相同负载、设备和完整完成边界；没有设备证据前不声明性能收益。
- 继续在 `codex/rdma-explicit-wait` worktree 实现。绿区 clone 与调试必须使用蓝区
  最终交付的同名临时分支和同一 commit SHA，不用 main 替代，也不验证未同步的源码。
  跨区恢复及调试授权单独处理；本设计不包含机器地址、私钥或部署配置。
