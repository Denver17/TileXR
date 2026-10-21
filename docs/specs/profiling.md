# Profiling 当前实现设计

本文说明已实现的独立采集 SDK、离线分析器及 TileXR 接入边界。面向接手代码和编写测试的人与 AI；测试命令、验收矩阵及证据模板见 [TESTING.md](../../tools/profiling/TESTING.md)。代码变化时同步维护本文，测试结果另记，不把设计合同当作运行证据。

## 1. 阅读入口与范围

| 目的 | 入口 |
|---|---|
| 安装、最短用法、迁移入口 | [profiling README](../../tools/profiling/README.md) |
| 理解组件、状态和所有权 | 本文第 2–6 节 |
| 查具体方法、错误码、调用前提 | [API_REFERENCE.md](../../tools/profiling/API_REFERENCE.md) |
| 查磁盘字段、mapping、时钟与统计定义 | [FORMAT.md](../../tools/profiling/FORMAT.md) |
| 选择测试、复现、记录结论 | [TESTING.md](../../tools/profiling/TESTING.md) |
| 理解迁移目标和未关闭整改 | [统一计划](../plans/profiling.md)、[整改计划](../plans/PROFILING_CLEANUP.md) |

当前交付包含：header-only C++ SDK、可独立安装的 Python 包、collectives 调用方迁移、独立 Add4 示例。SDK 不管理 communicator、业务 kernel 启动、rank 协调或测量轮次；离线分析器不依赖 CANN、设备或 TileXR 动态库。

版本独立演进：SDK `1.0.0`、DeviceView ABI `1`、磁盘协议 `mapping.v1/records.v1`、Python 包 `0.1.0`。权威版本分别在 [SDK VERSION](../../src/profiling/VERSION)、[types.h](../../src/include/tilexr/profiling/types.h)、[pyproject.toml](../../tools/profiling/pyproject.toml)。内存 ABI 变化不自动改变历史文件格式。

## 2. 组件与数据流

```mermaid
flowchart LR
    App[调用方：context / stream / kernel] --> Capture[AclCapture]
    Capture --> GM[独立 GM workspace + DeviceView]
    App --> Kernel[业务 kernel]
    GM --> Kernel
    Kernel --> Writer[Recorder / DirectWriter]
    Writer --> GM
    GM --> Collect[等绑定 stream 完成后 Collect]
    Collect --> Snapshot[Host CaptureSnapshot]
    Snapshot --> Export[EncodeV1 + 无覆盖文件发布]
    Export --> Records[records.v1]
    Mapping[mapping.v1 + 已确认的时钟换算] --> Python[tilexr_profiling]
    Records --> Python
    Attributes[调用方 provenance attributes] --> Python
    Python --> Reports[Perfetto / Markdown / HTML]
```

| 层 | 主要源码 | 职责与依赖 |
|---|---|---|
| 公共入口 | [profiling.h](../../src/include/tilexr/profiling.h) | 命名空间 `TileXR::Profiling`；AICore 分支只引入设备层；Host 分支引入布局/文件层，存在 ACL 头时提供 ACL 层 |
| ABI/布局 | [types.h](../../src/include/tilexr/profiling/types.h)、[host.h](../../src/include/tilexr/profiling/host.h) | 固定宽度描述符、预算检查、snapshot 和 v1 编码；纯 Host 部分仅依赖 C++ 标准库 |
| 设备记录 | [device.h](../../src/include/tilexr/profiling/device.h)、[direct.h](../../src/include/tilexr/profiling/direct.h)、[detail/device.h](../../src/include/tilexr/profiling/detail/device.h) | 记录时间戳/非零 event ID，提交有效前缀；无通信库依赖 |
| ACL/文件 | [acl.h](../../src/include/tilexr/profiling/acl.h)、[file.h](../../src/include/tilexr/profiling/file.h) | 借用 context/stream，拥有 GM 与 snapshot；Linux 文件发布 |
| SDK 安装 | [src/profiling](../../src/profiling/CMakeLists.txt) | `TileXR::profiling` INTERFACE target，只传播 include 路径及 C++14，不隐式链接 ACL/通信库 |
| 业务接入 | [profiling_capture_queue.h](../../src/collectives/host/profiling_capture_queue.h)、[collective_launcher.cpp](../../src/collectives/host/collective_launcher.cpp) | 有界采样、UUID/输出路径策略及安全清理点，属于 collectives |
| 离线包 | [tilexr_profiling](../../tools/profiling/python/tilexr_profiling/__init__.py) | `mapping/records` 解析 → `pairing` → `analysis` → provenance 与三种 renderer → `cli` 发布 |
| 独立使用方 | [examples/profiling](../../examples/profiling/README.md) | Add4 自建 ACL 对象、自行注册/启动外置 kernel，使用公共 SDK |

## 3. 内存协议与设备 writer

### 3.1 布局与身份

`DeviceArgs()` 返回 GM 描述符地址。不得传 Host 栈上的 `DeviceView`；也不得把旧 `CommArgs` 中的 profiling 槽位当作描述符。

| 对象 | 当前约束 |
|---|---|
| `DeviceView` | 72 bytes，8-byte 对齐；8 个 uint32 元信息 + 5 个 uint64 GM 数组地址；字段偏移由编译期断言固定 |
| SDK 默认布局 | 描述符位于 workspace 起始处，timestamps 从 offset 128 开始，随后为 eventIds/counts/dropped/states；数组地址按 32 bytes 对齐 |
| record stride | 以记录元素计；当前 Host 布局等于 `recordsPerStream`，容量 8…65536 且是 8 的倍数 |
| state stride | 以 uint32 元素计，当前为 8；每 lane 的 count/dropped/state 位于各自数组的 `lane * stateStride` |
| 有效 count | 真正有效的 `[0,count)`，可以不是 8 的倍数；不得按存储对齐向上补成有效事件 |
| 身份 | rank 0…65535；lane 数 1…65536，索引 0…65535；磁盘 stream ID 为 `(rank << 16) | lane` |
| 预算 | 初始化前检查 workspace 和最坏编码大小；默认 workspace 上限 1 GiB，文件上限 256 MiB，实际可用配置同时受两者限制 |

### 3.2 lane 提交

正常路径为 `EMPTY → WRITING → COMMITTED`。`INVALID` 表示协议错误；不是可继续分析的普通“不完整”。

| 状态/记录 | Host 行为 |
|---|---|
| COMMITTED，count > 0 | 读取有效前缀；校验容量和非零 event ID；按时间戳稳定排序，相同时间戳保留提交顺序 |
| COMMITTED，count = 0 | 导出完整空 block；分析器仍会诊断缺少 critical interval |
| EMPTY / WRITING | 忽略其 count、dropped 和 payload；保留该 lane，导出不完整的空 block |
| INVALID / 未知状态 | Collect/BuildSnapshot 失败，不生成成功 snapshot |
| 溢出 | 保留容量内前缀；dropped 饱和到 UINT32_MAX；磁盘标记 overflow，报告为 incomplete |

每个 capture 测量一次 kernel invocation，每个 lane 只有一个 writer。状态字不是锁，不支持并发抢占或复用已提交 lane。Host 只能在绑定 stream 同步成功后读取；最后一次标量状态写入不构成可供 Host 实时轮询的 release/acquire 协议。

`Recorder<N>` 的 N 为 1…256。构造时仅初始化有效性 ID；`RecordMarker<Slot,EventId>()` 写一个本地槽，重复写同槽会替换旧值；Flush 按 Slot 顺序压紧有效槽，跳过条件分支未写的槽，并提交一次。Slot 应按执行顺序分配，时间戳零是合法值。

`DirectWriter` 接收显式时间戳，适合已有计时逻辑：`Initialize → RecordAt → Publish`。Initialize 先清本地状态，失败后不能误写旧 lane；同一 writer 重复 Publish 为幂等，另一 writer 重用已提交 lane 会使其 INVALID。event ID 零使 lane 无效。

`TILEXR_ENABLE_PROFILING` 未定义或为 0 时禁用设备插桩，1 时启用，其他值编译失败。Host 生命周期与磁盘格式不随此宏变化。运行时传 null 可以跳过写出，但编译启用的 Recorder marker 仍可能读取 timer；因此 null 不能代表“无插桩开销”。marker 不自动插入业务流水屏障，测量完成边界由算子负责。

## 4. Host 生命周期、所有权与失败重试

| 阶段 | 动作 | 仍需调用方负责 |
|---|---|---|
| Initialize | 验证布局/预算、取得当前 context、分配清零 GM、上传描述符；未指定 UUID 时生成随机 UUID | 创建并保持 context/stream；多 rank 统一分发非零 UUID |
| Ready | 暴露 DeviceArgs，交给一次 kernel invocation | 启动 kernel；保证每 lane 单 writer；不在 warmup 使用 capture |
| Collect | 首次调用即封口，等待绑定 stream，D2H，校验并建立只读 snapshot；成功后幂等 | 当前线程必须处于原 context；失败后不可用旧描述符追加任务 |
| Export | 仅从已有 snapshot 编码/发布，不调用 ACL；失败保留 snapshot | 提供新目标路径及可用文件系统 |
| Close | 封口，必要时同步，再释放 GM；成功后幂等，snapshot 保留 | 在 context/stream 销毁前显式检查返回值 |

`Save = Collect + Export`。Close 不自动 Collect，也不导出；未采集就 Close 不会补建 snapshot。Close 后可以导出已有 snapshot，再 Initialize 会清除旧 snapshot。已初始化对象必须先 Close 才能重新初始化。

所有 Host 调用由调用方串行化。复制和移动赋值禁用，移动构造转移 GM/snapshot 所有权。`WorkspaceBytes()` 表示仍拥有的分配：成功关闭/移出后为零，同步或 free 失败且仍持有 GM 时非零。

失败重试的原则：

- context 不匹配：恢复原 context 后重试 ACL 方法；Export 不要求当前 context。
- 同步失败：保留 GM，不能提前释放；D2H 失败可重试 Collect，成功同步无需重复等待。
- 初始化部分失败：尝试清理；若 free 失败，保留所有权并要求 Close 重试。
- 文件失败：保留 snapshot，可改用另一路径；发布成功但临时名清理失败返回 `PublishedCleanupWarning`，`ok()` 为真，不得把它当成未发布再次写同路径。
- 析构不抛异常、不导出；无法确认安全完成时只诊断并保守保留 GM。显式 Close 是可恢复错误的边界。

`enabled=false` 的 capture 不分配、不上传，DeviceArgs 为 null，Collect/Export/Save/Close 成功但不生成文件。它与设备编译宏是两个独立开关。

Linux 发布路径为同目录独占临时文件 → 完整写/fsync/close → hard link 创建最终名 → 清理临时名。已有目标不可覆盖；不支持 hard link 的文件系统报错。该实现不承诺断电后的目录持久性。完整状态码见 API reference。

## 5. collectives 与 Add4 的接入策略

### 5.1 collectives

业务资源 owner 持有 `ProfilingCaptureQueue`，普通/A5 Host 参数携带独立 `profilingGM`，配套 kernel 消费同一 DeviceView。comm 中旧采集 runtime/exporter 已移除；旧公共 `tilexr_profiling.h` 只转向新入口，CommArgs 字段保留 ABI 空间，没有采集语义。

- 构建开关：`TILEXR_COLLECTIVES_ENABLE_PROFILING=ON`。运行时设置 `TILEXR_PROFILING_OUTPUT_DIR`；多 rank 使用同一 fresh `TILEXR_PROFILING_CAPTURE_ID` seed。
- 首次 Acquire 准备最多 4 个 capture、总计最多 64 MiB，可能分配并同步上传。后续使用已准备项时不 Collect、不 D2H、不导出、不 free。
- 池按 stream/lane 数匹配，pending 项只使用一次。当前首次准备会尽量占满预算；池满后新 stream/lane 形状、更多调用都可能跳过采样，没有后台回收重填机制。
- 预算耗尽或可选采样初始化失败记录诊断，业务 kernel 使用 null 继续。各 rank 的启动顺序/采样配置必须一致；分析器不会自动补齐采样不一致的 rank。
- 每项派生独立 UUID，输出 `<output>/<capture_uuid>/rank-<rank>.records.bin`。一次分析只提供同一 UUID 的文件；不同 run/owner 使用新 seed。
- 普通路径 lane 数为实际参与 AIV block 数；A5 为 48 个逻辑 lane（32 send + 16 receive），不能用 blockDim=33 代替。Host/Kernel 参数必须成对构建部署。
- 仅在原有安全完成/资源清理点 Drain；逐项失败保留，已发布/已关闭项重试时跳过。逐项 Drain 不做 rank collective；固定共同清理阶段汇合状态，某 rank 没有可选 owner 也必须参与共同阶段。

采样预算耗尽可跳过；清理失败需要保留资源并返回失败供重试，两者不可混为“任何 profiling 错误都忽略”。warmup 不采集，perf driver 已处理输出环境变量抑制。旧 `TILEXR_PROFILING_RECORDS_PATH` 和 communicator 多 launch 聚合不再支持。

### 5.2 独立 Add4

Add4 当前只支持显式 Ascend950/Linux。Host 为 C++14，现有设备编译路径为 `dav-c310-vec`、`gnu++17`；不能据此声称已验证设备 C++14 或其他 SoC。

4 个 AIV block 各自负责 1 个 lane，每 lane 20 条 marker；应用检查数值、4 个完整 lane、无 dropped 和事件数。纯 AICore binary 单独安装到 lib 目录下 `tilexr/kernels`，Host 从自身位置相对加载，调用 `rtDevBinaryRegister/rtFunctionRegister/rtKernelLaunchWithFlagV2`。不嵌入 binary、不依赖 TileXR 通信库，不使用 Host 三尖括号 kernel wrapper。

runner 生成填写了时钟换算的 mapping、环境证据、业务日志、records 和三种报告。模板中的 `cycles_per_us=null` 故意不可直接分析；必须提供目标 timer 的可信换算及依据。具体构建、搬迁和运行命令见 [Add4 README](../../examples/profiling/README.md)。

## 6. 离线解释与报告合同

Python 包唯一入口为 `tilexr_profiling`，不是 `tools.profiling`。公开 API 为 `generate_reports(mapping, records, output_dir, attributes=None)`；输出 `perfetto_trace.json`、`analysis.md`、`report.html`，均可离线阅读。目标目录必须不存在或为空，先 staging 再发布；致命输入错误不发布一组成功报告。

| 合同 | 对测试结论的影响 |
|---|---|
| 严格 v1 parser | 拒绝未知/重复字段、非法 ID、坏长度/保留位、重复 stream ID、混合 UUID；磁盘时间戳非递减，相同值保序 |
| 单次 critical interval | 每 lane 恰好一个配对 critical；普通阶段可顺序重复，重叠实例需不同事件对；不从事件顺序猜多轮 kernel |
| 时钟 | 原始整数差值先算后换算；同 rank/device 组保留 core 相对偏移，各 rank 独立归零；无跨设备校准，不推断真实跨 rank 先后关系 |
| inclusive | 样本时长之和，重叠可重复计数，share 可超过 100% |
| union/exclusive | 每 lane 先裁剪到 critical 再求并集；exclusive 只减显式直接子 stage 的覆盖并集；不跨 lane/rank 求并集 |
| wait/sync pressure | 最慢 critical lane 上 wait+sync 的覆盖并集占比，嵌套时间只计一次；不是两类 inclusive 相加 |
| rank latency | 各 rank 最慢 lane 时长，再汇总 rank 间 min/median/max |
| completeness | 仅针对提供的输入；缺失未提供的 rank 无 manifest 可核实，退出 0 不证明所有参与者齐全 |
| provenance | 三种报告共用校验模型；调用方属性与协议事实分开，unknown 不补猜，attributes 不能覆盖时钟/身份/完整性 |

CLI 退出：0=提供输入 complete，1=致命输入/输出失败，2=incomplete 且有报告。argparse 用法错误也为 2，但没有报告。测试必须同时检查状态、诊断及产物，不能只看退出码。API 非法 attributes 即便为空列表/字符串/标量也要失败；`None` 才代表未提供。

多轮容器、参与者 manifest、通用数据量/带宽、机器汇总比较、流式内存控制、增强 HTML、选择性采样、自动时钟校准、通用借用 workspace 等 E01–E09 保持延期。当前报告不等于这些能力已实现。

## 7. 维护与测试边界

改 writer/ABI 时同时检查 Host snapshot 和 Python reader；改 launcher 时同时检查 kernel 参数布局与资源清理阶段；改统计时同时检查三种 renderer 的 provenance。具体回归选择见 TESTING 的矩阵。

可复用的检查规则：对齐空间不等于有效 count；空提交不等于未运行；运行时 null 不等于编译无插桩；文件已发布的清理警告不等于发布失败；本地可选资源缺失不能改变 rank 共同清理阶段；两端各自合成 fixture 不能替代真实 writer→encoder→reader 往返。

CPU stub 仅证明普通 C++ 行为，ACL mock 仅证明替身下生命周期；真实 CANN 编译、设备可见性、数值、异步保持、marker/flush 开销及 spill 都需对应环境证据。Host、Add4 或 910B fallback 不能证明 A5 UDMA 数据面。当前已知验证与待验收项统一维护在 [TESTING.md](../../tools/profiling/TESTING.md)。
