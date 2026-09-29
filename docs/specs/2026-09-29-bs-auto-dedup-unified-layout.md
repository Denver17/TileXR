# BS 自动去重与统一资源布局

## 已确认目标

在 Dispatch / Combine kernel 内使用原始每 rank BS 决定执行行为：BS <= 32 关闭去重，BS > 32 开启去重。自动模式统一采用去重版本的 GM、通信包和 UB 资源布局，允许闲置空间，减少模式切换对接口和资源规划的改动。用户已要求将讨论方案落文并开始实现。

## 契约

- 保留公开算子、kernel 注册和 tiling ABI；同一轮 Dispatch / Combine 使用相同原始 BS，所有 rank 选择相同模式。不同 rank 使用不同 BS 不在本次范围。
- 新增构建策略 `dedup=auto`。显式 `dedup=0/1` 保持现有强制行为及布局，作为兼容和回归基线；auto 使用去重布局，运行时选择算法。生产默认策略改为 auto，旧 manifest 无策略字段时按其原布尔值解释。
- 将布局规格与算法开关分离。`kEpServerDedupEnable` 保留为编译期布局规格；`EpServerDedupEnabled(bs)` 是算法决策。auto 编译时布局必须为 true。Host 和设备公共头使用同一阈值。
- auto 下辅助表按去重容量申请，Host 保留相同参数校验。小 BS 不消费无效辅助表，无需为闲置空间增加清零和拷贝。
- 不承诺 auto 小 BS 与紧凑强制非去重版本性能相等；额外包长度、UB 预留、运行时分支的代价需 A5 实测。

## 实现设计

Dispatch 保持统一包步长、readIndex 槽和接收 lane 偏移。sender 在现有 SendWithDedup/SendWithoutDedup 间选择。非去重直接包明确写 Direct / 零 relay entries，不读取 `token - start` 对应的去重权重，因为 route 分工与 token 权重分工不同。无 token 分区的 core 仍可能有 route，不能提前退出；避免零字节 buffer 分配。receiver 按实际模式跳过 relay 和元数据处理，但偏移仍按统一布局。

Combine 保持去重 stage/result GM 布局，关闭去重时跳过 M1/M2、server 通知等待、M4，M3 处理全部有效行。M6 复用 M5 最终聚合 UB 视图，保留常驻区和 mask 槽位，不移动元数据偏移。直接聚合没有 M4 在途输出，需独立初始化实际输出槽事件，跳过 M4 回收等待，并按真实槽映射 drain。强制 off 保留原 M6 紧凑布局。

Profiling 容量使用实际行为计算且 Host/device/consumer 一致，报告区分构建 policy 与 case 实际模式。测试 fixture 的包布局依据布局开关，route/reference/权重/辅助表语义依据实际模式，不混用。

## 执行计划与责任边界

1. 公共策略、构建和工具：修改模式头、CMake、shell/Python workflow，使 auto 贯穿构建、manifest、运行与统计。公共接口为 `EpServerDedupEnabled(uint32_t bs)` 和布局常量；不得改 kernel 算法。新增策略/manifest 回归。
2. Kernel：修改 sender/receiver、Combine/source aggregate，保持去重布局与现有同步约定；提供小 BS 直接模式及 M6 公共布局适配。不修改 workflow。
3. 测试 consumer/reference：区分行为与布局，更新积分/校验数据、standalone Combine fixtures，增加 1/31/32/33/256 边界及缺失元数据读取回归。不得以源码字符串守卫代替行为测试。
4. 集成与验证：主代理审查全部变更，执行 forced off/on 和 auto 的 Host/行为回归、Python workflow 回归。blue 上尝试 CANN 9.1/A5 Dispatch+Combine+Host profiling OFF/ON 编译，保留配置、版本和日志。硬件验证留给实际 A5 环境，不用 CPU 结果声称 NPU 正确或性能通过。

## 验证重点

- BS=1/31/32 选择 direct，33/256 选择 dedup；相同 shape 在 auto 中所有包、UB 和 Host 容量保持去重规格。
- 小 BS 的 token 空分区仍发送所属 route，包头和权重不越界。
- 非去重不读取 sourceMask/relay 等未初始化内容；M6 输出槽与事件所有权匹配，无 M4 等待。
- 同一 communicator 32 -> 33 -> 32 连续调用，检查 magic/窗口、结果 flags、辅助表残留。真实异步 DMA 与跨机进度需 NPU 验证。
- quant=0/4，FP16/BF16，跨 server 与重复目标，profiling OFF/ON；大 BS 对照 forced on，小 BS 对照 forced off。

## 维护教训

资源布局与算法启用不能复用一个布尔含义。统一分配空间也不意味着可以继承未执行阶段的事件状态；尤其 M5 原本继承 M4 输出，直接路径必须建立自身事件所有权。

## 实现与使用

普通构建默认使用 auto；也可显式指定，随后同步并使用原有 run.sh 参数：

```bash
bash build_all.sh dedup=auto
bash sync.sh
bash run.sh -m 1 -n 8 -e 128 -l 100 50 bs=32 topk=6 h=5120 route_mode=balance_shift operator_mode=both quant_mode=4 fp8_format=e4m3 seed=42
```

将 bs 改为 33 即进入去重流程。采集内部打点时使用 `bash build_all.sh dedup=auto breakdown=1`，同步后改用 `-m 2`。强制基线仍用 `dedup=0` 或 `dedup=1` 重新构建。历史 checkpoint 验收默认沿用强制 1，以保留其阶段语义。

manifest 新增 `server_dedup_policy`（auto/0/1）；manifest 的原 `server_dedup_enabled` 继续表示构建布局规格。运行配置单独保存 `server_dedup_layout_enabled`，实际 rank 记录的 `server_dedup_enabled` 按 BS 解析。不要将 manifest 的布局布尔值直接作为小 BS 的实际行为。旧 manifest 无 policy 字段时解释为强制模式。

## 开发侧验证记录

- 完整 Python 工作流回归：397 项通过，无跳过。
- 原有强制去重 Host/CPU 回归：60 项通过；最初 7 项因容器内存不足被并行测试终止，串行重跑均通过。
- 强制非去重发送、接收、Combine、打点及布局：8 项通过；auto Host 参数与布局：6 项通过。
- auto sender/receiver、完整 Combine kernel 的 CPU 行为测试通过；Combine 覆盖原始 BS=1/31/32/33/32、五种发送核数、FP16/BF16、K=6/32，检查结果、无效辅助表不读取及输出事件回收。
- auto Combine/M5 配置与 profiling 矩阵 12 项通过，auto Dispatch profiling 2 项通过；consumer 3 项矩阵测试通过，包括 K=32，测试 stub 的全位 mask 已避免 32 位移位溢出。
- 最终源码在 blue 使用真实 CANN 9.1 / Bisheng Clang 15.0.5、A5 `dav-c310-vec` 完成 Dispatch、Combine kernel 与 `tilexr-ep` Host 库编译，profiling OFF/ON 均返回 0；341 个生产文件摘要与本地最终源码一致。简化后受影响的强制去重 sender、Combine 和 profiling 4 项回归重新通过。
- 真实设备上的异步 DMA、跨机通信、性能，以及同一 communicator 动态 BS 的实机表现尚未验证。上述 CPU 模型和 Host 替身不构成 NPU 验收。

本地验证日志与构建产物保留在 `build/bs-auto-dedup/`，补充自动模式矩阵位于 `build/auto-policy-tests/`。这些临时构建产物不进入提交。
