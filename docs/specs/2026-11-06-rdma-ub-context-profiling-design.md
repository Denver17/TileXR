# RDMA WRITE：UB context、Debug 校验与已有 Profiling 接入

## 状态与范围

2026-11-06：用户已明确确认本书面设计并授权实施。实现和验证进度见配套计划，
设计获批不代表设备已经验证。
实施分支为 `codex/rdma-explicit-wait`，设计基线为 `40c6b30`。

目标：Release 裁掉调用方参数检查；显式 context 路径不在每次 WRITE、发布批次和
完成回收时读写 GM 软件状态；Debug 复用 `<tilexr/profiling.h>` 的 `DirectWriter`，
将事件追加到 GM lane，结束记录后提交，用于区分 WRITE 提交和完成等待的成本。

不改变现有按 QP SQ/WR 能力计算窗口、完整批次背压策略、批长上限、CQ 匹配、
SQ/CQ 不回绕限制或 WQE 格式。不修改 legacy all-signaled Put、Host 数据面、
QP 并发模型；不引入新的 trace 文件协议、后台采集、自动重试或定时刷盘。

## 1. Debug 与 Release

- 用数值宏 `TILEXR_RDMA_DEBUG` 区分调用方参数校验。Debug 为 1，Release 为 0；
  设备自定义编译命令必须显式收到设置，不依赖 `CMAKE_BUILD_TYPE` 隐式传递 `NDEBUG`。
- Debug 校验覆盖 `RDMAWrite`、`RDMAWriteSge`、两个 MultiQp 包装及内部实际路径中的
  offset/长度/stride/MR 范围、SGE 参数、QP 索引和 UB scratch 等调用方前置条件。
  在适当边界验证，避免内层重复检查同一事实。
- Release 仍执行必要的 ABI 版本/长度判定、真实 SQ/WR/CQ/pending 容量保护、
  CQE 错误/匹配、超时与异常 QP 状态处理。Host 初始化资源检查保持原样。
- 打点沿用 `TILEXR_ENABLE_PROFILING`，RDMA 插桩只在 Debug 且 profiling 开启时编译。
  Release 不执行 RDMA 时间戳、事件写入或 lane 提交。

## 2. 调用方持有的 UB QP context

context 由一个 AIV 独占，关联一个 QP，由该 QP 的所有 MR view 共用；多个 QP
分别持有 context。它不增加 Host/Device V3 view 字段，也不改变硬件队列 ABI。

- 工作状态和 pending FIFO 使用调用方显式分配的 UB，避免随深度增长的栈数组。
  状态头为现有 64 B；FIFO 按实际 `pendingCapacity * 8` B 预留，并按 Ascend C
  搬运要求对齐。WQE/CQE scratch 与 context 存储独立，不得相互覆盖。
- 显式初始化/恢复边界读取 GM 状态及有效 pending 项；context 内的 WRITE 和 Wait
  通过 UB 读写软件状态。局部计算量可用标量临时变量，但不把权威状态放回 GM。
- 提供四个 WRITE 入口和 Wait 的 context 接入方式。公共名称保持一致，具体参数
  排列属于实现细节；已有不带 context 的调用保持源代码可用，使用临时 context，
  在 API 边界恢复和保存，不再每个批次写回。这条兼容入口不宣称零 GM 元数据访问。
- 显式 context 可连续经历多个 WRITE/Wait；成功 Wait 不销毁 context，也不隐式
  写回 GM。调用方要复用 UB、结束 kernel 或把 QP 所有权移交给后续 kernel 前，
  显式保存状态及仍有效的 pending 项，完成所需搬运后才释放 UB。
- 保存不是 Wait，不使 NIC 工作完成；导出仍 pending 的状态后，QP、MR、payload
  和 GM 持久化状态的生命周期继续遵守已有完成语义。后续 kernel 先恢复再操作。
- 返回的超时、CQE 错误和 poisoned 状态保留在 context 中；正常错误返回路径也必须
  保存后再退出。kernel 异常终止、未成功保存时，GM 快照可能过期，不允许据此
  恢复发送或假定无在途请求，必须按已有安全清理要求使 QP 静止/销毁；不承诺异常
  中断后的透明恢复，不新增日志或恢复框架。

“移除 GM 状态同步”仅指软件工作状态和 pending bookkeeping。硬件 SQ/CQ、payload、
doorbell 仍在原来的 GM/MMIO；必须保留 UB 构造 WQE、MTE3 发布完成后 `st_dev`
响铃、CQE 新鲜读取及正确流水依赖，不能用 UB 同步替代对 NIC 的必要可见性。

## 3. 复用 `<tilexr/profiling.h>`

入口包含 `<tilexr/profiling.h>`，直接使用已有 `TileXR::Profiling::DirectWriter`，
不扩展公共 profiling 模块。沿用 `DeviceView`、event ID、lane、
count/dropped/completion 和现有 mapping/records 协议。

- 每个参与采集的 kernel/AIV 初始化独立 lane 的 writer，trace 借用该 writer。
  `RecordAt` 将每个事件追加到 GM；已有固定 slot 的 `Recorder` 不适合保留反复
  执行的批次事件。无需新增 recorder、UB buffer 或公共 API。
- Debug 为每个 AIV/QP 独立采集；入口/出口事件标识四种 WRITE，嵌套路径有明确
  层级，报告不能把外层 inclusive 耗时与内层耗时相加成总耗时。
- 内部事件覆盖 view/状态准备、参数校验、WQE 编码、stop/尾部/首部发布、
  doorbell、容量背压导致的 CQ 回收，以及单独的显式 Wait；保存/恢复和记录导出
  放在各自独立区间，不混入 submit-only 区间。
- 事件容量由 GM descriptor 指定；反复执行的事件按调用顺序追加。容量满后沿用
  DirectWriter 的 dropped 语义，保留已有前缀，不将丢失事件的采集报告为完整结果。
- 记录结束后调用已有 `Publish()` 提交 lane；Host 通过现有 `AclCapture` 在绑定
  stream 成功同步后读取，沿用现有工具生成 Perfetto、Markdown 和 HTML。
- 使用已有设备 timer，不为打点额外插入全流水屏障。先报告原始 tick，换算时间
  需要目标 timer 的可信换算。逐点写 GM 会影响 Debug 阶段耗时；无插桩 Release
  用于性能基线，不能将 Debug 结果当作其绝对性能。

## 4. 验证与验收

1. CPU：分别编译 Debug/Release 路径，Debug 非法参数测试、两种构建的合法请求、
   队列边界、背压与错误处理均覆盖；不依赖测试目标中的 `-UNDEBUG` 证明设备宏裁剪。
2. 用可观测 I/O 替身验证显式 context 多次 WRITE/Wait 不访问 GM 软件状态/FIFO，
   保存/恢复时才发生相应访问；验证跨 MR 共用 context、跨 kernel 序列模拟及异常状态。
3. 验证原有发布批次、CQE 计数和队列上限未变，不做短批次调度改造。
4. RDMA CPU 测试验证四种 WRITE 的事件记录和 lane 提交；复用 profiling SDK
   测试验证独立 lane、容量溢出和协议，Python 测试验证配对及导出兼容。
   这些测试不证明设备内存可见性、MTE 顺序或真实耗时。
5. 目标 CANN 编译：Debug/Release 配套重建库与 kernel，检查编译宏和产物；确认
   context UB 预算、设备编译兼容及 Release 插桩消失，不仅检查源代码文字。
6. 后续硬件验证仅经绿区 EXEC，在用户授权的 184 机器 0/2 卡空闲时执行；不重跑
   用户已确认通过的环境基线。先正确性和容量边界，再采集同参 Debug/Release，
   最后汇总阶段成本和端到端性能；本设计文档不代表已派发或运行任何测试。

维护经验应记录在 `docs/RDMA_API.md`：UB 状态不会跨 kernel 保留；显式保存不是
完成等待；GM 状态同步与 NIC 必要发布同步是不同边界；打点丢失不得报告为完整采集。
