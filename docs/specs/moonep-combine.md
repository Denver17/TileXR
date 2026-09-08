# MoonEP Combine 规格

## 范围

MoonEP Combine 将 expert 行还原为 token 顺序，并对 top-k 贡献进行归约，
可选用 FP32 route weight。Combine V2 是唯一受支持的后端；
运行时没有后端版本选择器，也没有 V1 回退路径。

V2 公共 ABI 声明于 `src/include/tilexr_moonep_combine_v2.h`。
调用方拥有已保存的 plan、已注册 workspace、输出、stream 和 communicator。
Combine 负责布局校验、通过已注册入口发起 launch、调度状态、传输完成和归约。

## Route 契约

Planner V3 提供 `dstLocal`，将 expert 接收 slot 反向映射到 source rank、token 和 top-k 位置。
Padding 和未使用 slot 使用约定的 sentinel，并在处理时跳过。
Route 解码、调度映射、数据搬运和归约均使用同一 generation 的 plan。

可插拔的 step 映射根据调度模式、rank、core、step 和 rank size 计算完整的 `StepRoute`。
发送目的端、接收源端、credit 等待 owner 和 credit 发布 owner 一并推导。
映射 helper 不读取 workspace、QP、CQ、payload 或可变 kernel 状态。

当前生产映射在 128 ranks 时使用 Group，其他受支持规模使用 Single Ring。
其他已编译映射仍作为需显式选择的备选项，不会自动选用。

## 数据路径

每个 step 必须且只能属于以下一种情况：

- 通过 CLOS UDMA 传输到远端 peer；
- 通过 fullmesh UDMA 传输到远端 peer；
- 通过本地 GM/MTE 执行 self-copy；
- Payload 为空，但仍遵循控制协议。

Fullmesh 和 CLOS 维护独立的资源视图与队列 frontier。
Self-copy 参与相同的 step 生命周期，但不伪造 CQ 完成事件。
Memory 与 UDMA 的选择遵循能力、注册和 payload 策略，不能仅依赖拓扑。

## Credit 与完成协议

Credit 是当前使用的复用协议。历史上的 grant 和 full-sync/phase barrier 方案不作为现行回退路径。
每个存储或队列 frontier 都有唯一发布者和明确的等待者：

1. 等待保护下一个可复用区域的 credit；
2. 构建并提交该 step 的 payload 和控制任务；
3. 等待该 transport 要求的准确 completion frontier；
4. 只有受保护数据可见后，才发布 done/control 状态；
5. 只有接收端消费者释放对应区域后，才发布下一个 credit。

最后一个 step 与之前各 step 遵循相同的完成和 credit 所有权规则。
跨轮次安全性由带 generation 标记的状态和已完成的复用边界保证，
不能依赖清空所有共享状态。

## WQE 与 UB 契约

Route chunk 加载到 UB 后，在 UB 中完成选择、压紧并转换为完整的 payload WQE。
可以预取 peer/QP 不变字段，但每个 WQE 都必须在通过 MTE3 复制到 SQ 前构造完整。
Doorbell 仅能在 MTE3 完成后使用 `st_dev` 触发。
队列记账必须跟踪准确的提交 frontier。

用于 route 选择、WQE 批处理、self-copy、weight 收集和归约的 UB 分配，
必须具有明确且互不重叠的生命周期。
只有所有相关 MTE/vector 任务完成后，才允许执行 `TPipe::Reset` 或复用 buffer。

## Hidden 与 Weight 融合路径

融合路径在同一个 Combine 阶段中，通过所选 transport 搬运 BF16/FP16 hidden 行，
并通过规定的内存域处理可选的 FP32 route weight。
Weight 存储在 peer window 中按 generation 隔离布局，且每个位置具有唯一写入者。
Hidden 与 weight 的完成遵循同一阶段顺序，存储不得与 payload、profile 或 scratch 区域发生别名重叠。

Python 返回 tuple、zero-copy 生命周期、可选 weight 行为及 async-event 契约保持不变。

## 归约

归约按 route 顺序消费接收行，先使用 FP32 累加，再生成声明的输出 dtype。
只有 buffer 所有权和 event 能证明各行相互独立时，
流水线才允许重叠执行 MTE2 输入、vector 累加和 MTE3 输出。
较大的 hidden dimension 通过分块处理，不改变逻辑累加顺序。

空 route 和 padding route 不读取 payload。
只有所有入站数据、self-copy、route-weight 数据和归约写入完成后，输出才可见。

## 失败行为与诊断

Host 校验拒绝不受支持的 rank size、plan version、layout、dtype、workspace 范围，
以及不可用的已注册 transport。
Kernel 失败时应尽可能记录首次失败的 stage/peer/core/frontier/generation。
Profiling record 带有版本，且与 payload 和正确性状态区域互不重叠。

## 验证边界

Host 测试穷举比较 route 映射，并验证布局、注册、模式选择、source invariants，
以及 empty/self/remote 情况。
硬件正确性测试必须覆盖所选 rank、schedule、backend、连续多轮、
支持范围内的 skew/sparse/empty route、可选 weight 和归约。
性能报告包含 backend、native status、kernel version、算法或通信数据量、latency、
适用时的 bandwidth，以及跨 rank 的 median、minimum 和 maximum。
Host 或 simulator 证据不能证明 UDMA 顺序或 bandwidth。

退役某个 stage backend 时，应在同一次变更中移除其源码 target、公共 ABI、
运行时 selector、报告中的来源信息、demo 和专用测试。
保留其中任意一项都会造成错误的兼容性承诺；
移除后须独立验证保留的后端。
