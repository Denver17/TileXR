# EP Memory 后端规格

## 范围

可选 EP 库通过 `src/include/tilexr_ep.h` 导出 Dispatch 和 Combine 接口。
Memory 后端使用 communicator 的 peer 映射承载同节点和跨节点通信。
它与 UDMA 后端相互独立，不能仅根据拓扑选择后端。

已实现的 Server Dedup 通过独立的 `TileXRMoeEpDispatchMemoryServerDedup` 和
`TileXRMoeEpCombineMemoryServerDedup` 入口提供服务器级去重及返回归约。
其五表协议、布局及运行限制见[Dispatch/Combine 联合设计](ep-memory-server-dedup-dispatch-combine-design.md)
和公共头文件 `tilexr_ep.h`；不能与普通 Memory 入口的元数据混用。

Host 层负责校验公共参数、构建当前轮次的窗口布局，从 Host 库的 `tilexr/kernels`
目录加载所选 kernel，并通过已注册的 AICore signature 发起 launch。
Communicator 和 stream 由调用方提供，EP 不拥有它们；EP 仅拥有自身的 launch 元数据和 kernel 注册资源。

EP 打包在 configure 阶段不依赖具体 SoC。普通 Dispatch/Combine、Memory 及 Server Dedup
变体均仅支持 A5，同一次安装交付六个生产外置 AICore object，不生成 910B EP kernel 变体。
`libtilexr-ep.so` 在每个公共入口调用 `aclrtGetSocName()` 检查 SoC；
对于非 A5 硬件，在准备 context、加载或注册 kernel、推进 magic 值之前，
返回 `TILEXR_ERROR_NOT_SUPPORT`。

## Server Dedup 契约与部署

- Dispatch 支持 FP16/BF16 输入，以及非量化或 MXFP8 E4M3/E5M2 输出；Combine 当前仅支持
  FP16/BF16、`quantMode=0`。Dispatch 的量化支持不代表 Combine 支持量化输入。
- Dispatch 输出 tokenType、destinationIndex、relayReadIndex、sourceMask、expertScales 五表，
  有效行数由 sendCounts 前缀末项给出；表与输入行序须保留到对应 Combine 完成。
- 默认构建策略为 `dedup=auto`：保持统一去重资源布局，按原始每 rank `BS<=32` 走直接路径、
  `BS>32` 执行去重；显式 `0/1` 强制对应布局。Host、kernel 和 consumer 必须使用同一构建配置。
- Combine 完成 staging/readiness、DIRECT 返回、加权 partial 返回及源端归约（M1–M5）；
  非去重模式使用直接返回与 M6 归约。两算子复用 magic 选取的双窗口，同一 communicator
  须在同一 stream 串行提交，且所有 rank 使用一致的模式、shape/layout 与核分配。
- 独立 GM tiling 由 communicator 缓存到销毁；Dispatch/Combine profiling 使用独立记录区域。
  必须配套部署 `libtile-comm`、EP Host 库和外置 kernel，不能只替换一个算子二进制。
- 最大 rank 从 128 扩至 256 改变了 `CommArgs` 的数组尺寸、后续字段偏移和相关 credit 布局。
  所有消费这些公共结构的 Host/kernel（包括其他通信后端、collectives 和下游）均须重新编译，
  全部 rank 部署同一版本。256-rank 配置/Host 检查不代表 256P 实机正确性或性能通过。
- 关闭 overflow recording 的编译设置仅用于两个 Server Dedup kernel；其他 EP kernel
  保留原有开启设置。具体编译与设备证据见[EP 验证指南](../EP_VALIDATION_GUIDE.md)。

## Dispatch 契约

Dispatch 将输入行路由到 expert owner，并按所选 API 版本返回 expert 本地 payload、
Combine 所需的 assist 元数据和接收计数。V2 还保留 `tilexr_ep.h` 声明的
active-mask、量化、scale 和展开输出契约。

Memory kernel 在 peer window 中使用带 generation 标记的轮次。
发送、count/cumsum、本地 copy 和接收阶段使用互不重叠的状态及 workspace 区域。
每个状态单元只有一个写入者，消费者等待当前 launch 对应的 generation。

MXFP8 模式保留公共接口中的 E4M3/E5M2 选择、打包行格式和动态 scale 输出。
不受支持的量化或 dtype 组合会在 Host 校验阶段失败。

## Combine 契约

Combine 消费 expert 输出以及 Dispatch 生成的 assist/count 元数据，恢复 token 顺序，
并按所选入口的要求应用 expert weight。Memory kernel 与 Dispatch 遵循相同的
generation 和 peer window 所有权模型，同时将 Dispatch 与 Combine buffer 分开。

累加顺序和精度属于 API 行为的一部分。空 route 和 inactive token 都是有效输入，
不得读取 peer window 中未初始化的行。

## 窗口与轮次所有权

`src/ep/common/ep_memory_window.h` 和 Host 布局 helper 定义了权威的窗口分区。
Payload、status、count、同步、profiling 和 scratch 区域互不重叠。
只有所有生产者和消费者都越过该轮次的完成边界后，轮次资源才可复用；
新 generation 用于区分过期状态，无需批量清空共享窗口。

Memory 支持跨节点的映射 peer 通信。只有具体入口、communicator 能力、
payload 和已注册 workspace 契约要求使用 UDMA 时，才选择 UDMA。

## Profiling

Dispatch 和 Combine 使用各自独立的定长 profiling record，分别声明于
`src/ep/common/ep_dispatch_memory_profile.h` 和
`src/ep/common/ep_combine_memory_profile.h`。
Profiling 在编译期显式启用，不改变公共 ABI，每个核只写自己拥有的 record。
每次 profiling 调用引用的时间戳或指标都必须存在于对应的 record 定义中，
且在相同功能配置下具有有效的起始值。因此，删除 profiling 字段时必须同时删除其生产端调用；
对此类清理，至少需要完成默认关闭 profiling 的 CCE 构建验证。

通用 profiling 报告流程消费带版本的阶段时间戳和累积指标。
不完整的 rank 或 record 必须明确标注。
64-rank runner 统一了功能、性能和 breakdown 模式，但 profiler/诊断配置不属于正常算子语义。

## 失败行为与兼容性

- 非法 shape、layout、datatype、mode 或 communicator 能力必须在 launch 前被拒绝。
- Kernel status 标识首个协议失败；Host 收集结果时不得用后续 timeout 症状覆盖它。
- 公共 V1/V2 入口保持稳定，除非另行批准 ABI 变更。
- 活跃代码不得 include 或链接 `reference/` 中仅用于对比的源码。

## 验证边界

Host 测试覆盖布局、参数校验、量化 helper、注册和运行时拒绝行为。
多 rank 功能测试验证所选 memory 路径及 shape。
性能报告必须注明 backend、native status、kernel version、数据量、latency、
适用时的 bandwidth，以及跨 rank 的 median/min/max。
某一 rank 数量或拓扑上的结果不能证明其他配置也已通过验证。
