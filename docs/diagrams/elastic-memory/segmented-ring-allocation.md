# 单 owner 分段环形池：流程与实测

[可编辑 draw.io](segmented-ring-allocation.drawio) · [PNG 预览（内嵌图源）](segmented-ring-allocation.drawio.png)

图对应 [ElasticMemory 设计](../../elastic-memory.md) 第 3、10 节：单 core、无并发 allocator，
一个算子内不反复申请释放，主要为较大 output 分配连续空间。每个 bundle 完整落在一个 segment 的连续区间内。

## 当前流程

1. 最终计数核取得实际 rows，计算 bundle bytes，进入 `AllocateBundle<128>`。
2. 用 MTE2 读取 64B header 和 64B 当前 RingState，应用已发布的连续退休前缀水位。
3. 计算对齐、容量与绕回 padding；成功且非空时通过 MTE3 提交一个 64B RingState，再有序发布 Output。
4. 当前段不足则尝试其他 segment；仍不足时发布一次增长请求，原 kernel 驻留等待 Host 真实申请独立 HBM。
   owner 在新段取得本次区间后原地继续；错误使整调用退出，不重试、不重放、不回滚。
5. 实际 output 核在复制前等待本地描述符。prefix/expert-map 准备与分配重叠，peer 错误汇聚与复制可重叠。
   Host 等完整数据完成后才返回 Tensor；metadata-only materialize 尚未实现。

Host 在调用间根据 Storage/view 和登记 stream 生命周期整理每段的安全退休前缀。
A 活跃而 B/C 已释放时，不能越过 A 回收；这部分阻塞空间及 padding 仍占容量。
单 owner 不免除异步生命周期检查，空段须满足引用和在途访问条件后才能实际归还。

## 实测与边界

CANN 9.2.0 / Ascend950DT，BF16/topK2，2 ranks × 64 AIV，owner 为 core 63。
两 shape（bs16/h128、bs256/h4096）各有普通命中及非空绕回组；每组每 rank 20 次暖机、20 次采样。
160 个完整分配区间约 **1.301–1.671µs**，本次无超过 2.5µs；全部热样本无增长。
计时包含初始化、读取、计算、64B 状态提交、结果发布及必要同步。
图中子阶段区间为四组逐 rank 中位数的范围；不是独立分项预算，也不能相加。

SDK 使用相对 msprof 的经验换算 1000.8934146536867 ticks/µs，未确认为物理时钟频率。
短阶段精度、插桩扰动及跨 lane epoch 的边界仍保留，观测最大值不是所有工作负载的保证。
段切换、Host 增长及等待在这些热样本中未计时，不按 2.5µs 热命中目标声明。

关闭设备诊断后的完整 Dispatch 跨 rank 中位数为 **13.905 / 92.332µs**，
同批最大输出预分配为 **19.503 / 93.645µs**。保持相同 Host stage barrier/framework profiler，
每 rank 20 次暖机、100 次采样，growthCount=1→1。大 shape rank 1 的 pool P95 仍高于对照，
不宣称尾部一致改善。详细阶段、每 rank 分布及证据见 [profiling 说明](device-allocation-profile.md)
和[验证记录](../../validation/elastic-memory.md#segmented-ring-device-and-complete-kernel-measurements-november-9)。

维护时以当前源码与加载 binary 对应的原始记录同步更新图源、预览和说明，明确区分分配区间与完整 kernel 时间。
