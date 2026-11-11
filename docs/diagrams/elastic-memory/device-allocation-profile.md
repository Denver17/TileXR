# Device 池化分配流程与实测

[可编辑 draw.io](device-allocation-profile.drawio) · [PNG 预览（内嵌图源）](device-allocation-profile.drawio.png)

图对应当前单 owner 分段环形池。Device 入口是
`src/include/device/tilexr_elastic_memory_device.h::AllocateBundle<128>`；
调用与核间交接位于 `src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h`。
最终计数核持有实际 rows 并计算 bundle bytes，本组 64 AIV 配置下 owner 是 core 63。

## 热路径阶段

CANN 9.2.0 / Ascend950DT，BF16、bs16/h128/topK2、2 ranks、每 rank 64 AIV。
仓库 `TileXR::Profiling` SDK STAGES 每 rank 20 次预热、20 次采样，无增长。
下表为逐 rank 中位数，单位为近似 µs：

| 区间 | rank 0 | rank 1 | 计入的工作 |
|---|---:|---:|---|
| 完整 AllocateBundle | 1.451 | 1.431 | 初始化、读取、计算、提交、结果发布及必要同步 |
| 状态读取 | 0.381 | 0.356 | MTE2 读取 64B header + 64B 当前 RingState |
| TryAllocateRing | 0.315 | 0.321 | 应用连续可回收前缀水位、对齐、容量判断、绕回 padding |
| 状态提交 | 0.177 | 0.177 | 成功且非空时将 64B RingState 写到 mailbox committed，等待 MTE3 完成 |
| 结果发布 | 0.334 | 0.334 | 先发布 sequence=0 的 64B Output，再携本轮 sequence 发布完整描述符 |
| owner rank gate（分配接口之外） | 2.083 | 2.097 | 本地描述符发布及 peer 错误汇聚；可与其他核的输出复制重叠 |

子阶段嵌套在完整分配区间内，中位数不能相加当作总时长。只有实际 output 核在复制前等待本地描述符；
prefix/expert-map/count 准备与分配重叠，不能把所有核画成先完成分配再开始准备或复制。

两 shape（bs16/h128、bs256/h4096）、两 rank、普通命中及非空绕回四组共 160 个 owner 样本：
约 **1.301–1.671µs**，本次无超过 2.5µs。每组每 rank 20 次预热、20 次采样，
120 次普通命中、40 次非空绕回，growthCount 均不变。
10,240 条 lane 流和 67,200 个 marker 完整，无 overflow/drop。

## 完整算子与测量边界

移除设备诊断插桩后，以 msprof Task Duration 比较相同路由、BF16/topK2、2 ranks 的完整 Dispatch。
各模式保持相同 Host stage barrier/framework profiler，每 rank 20 次预热、100 次采样；池测量期间 growthCount=1→1。
下面为两个 rank 中位数的中位数，单位 µs：

| Shape | 最大输出预分配 | Host 实际输出对照 | Device pool |
|---|---:|---:|---:|
| bs16/h128 | 19.503 | 19.085 | 13.905 |
| bs256/h4096 | 93.645 | 93.628 | 92.332 |

完整逐 rank min/median/max/P95、冷启动、backend/kernel、bytes 和有效输出带宽见
[验证记录](../../validation/elastic-memory.md#final-complete-kernel-comparison)。
大 shape rank 1 的 pool P95 为 101.574µs，对照为 89.976µs；中位数改善不代表每个样本或尾部均改善。

阶段换算使用相对 msprof 拟合的 **1000.8934146536867 ticks/µs**，不是已确认的物理时钟频率。
不做跨 lane/rank 时间戳相减，不宣称短阶段具备亚微秒绝对精度。
STAGES 和 OFF 为独立批次，受 rank 到达时间与插桩扰动影响，不直接相减作校正；完整算子比较只用 OFF。

## 慢路径、错误与回收

图示为有效请求的容量分支。当前段不足先检查其他 segment；仍不足时，当前 kernel 发布一次请求并驻留等 Host 真实分配。
成功后 owner 在新段申请一次并从原分配点继续；不重启或重放前序副作用。段切换与增长未在这些热样本中计时，不表示 0µs。
非 Capacity 错误直接发布失败；Host 增长失败也使整调用失败，不重试、不回滚。
peer 错误汇聚与本地复制重叠，因此失败时本地成功的核可能已写 output，不能承诺未发生写入。

Host 在调用间依据 Storage/view 引用及生产者、通信、登记 stream 的完成状态整理连续可回收前缀，
Device 应用已发布水位。A 活跃而 B/C 已释放时不能越过 A；padding 和被队头阻塞的空间仍占容量。
空段只在安全边界实际归还。`Pending::Wait` 等待完整完成；`materialize` 等元数据并建立消费 stream 依赖，最终错误由 `synchronize` 检查。图中性能为异步改造前批次，不证明新增异步收益。

## 证据与维护

实测源码为 `d847c22956966e4e0805981ba5039fad894e047b`。
头文件迁移及注释修订为 `fc8499455e22690329b49a6f85bccb8a47da2986`，没有新增性能采样。
源码、加载 binary、SDK mapping/records 和复验命令统一见[验证记录](../../validation/elastic-memory.md#final-provenance-and-reproduction)。

本地证据根：
`/workspace/TileXR-worktrees/elastic-memory/.elastic-memory-evidence/kernel-increment-20261109/`。

- `stages-verified-small/sdk-stage-analysis/summary.json`：图中逐 rank 阶段值。
- `stages-verified-{small,large,ring-small,ring-large}/`：四组原始 records、mapping 和 SDK 报告。
- `independent-verified-stage-audit/summary.json`：lane、marker、普通/绕回及零增长复核。
- `verified-summary.json`、`independent-verified-off-audit/report.md`：完整 kernel 对照与统计复核。
- `provenance.json`：源码及加载 binary 对应关系。

维护时同步更新 draw.io 图源、嵌入式 PNG 和配套说明；依据同一批原始记录标注阶段，避免图源与预览呈现不同实现或性能数据。
