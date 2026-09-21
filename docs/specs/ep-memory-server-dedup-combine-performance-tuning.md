# Server Dedup Combine 性能调优

本文记录已确认的优化尝试、实现边界和验证结果。功能通过、编译通过与实际性能收益分别记录。

## 尝试优化点 1：M5 移至 Send 核，在 M3 之后执行

### 背景与目标

用户实测发现 M5 接在 Read 核的 M4 后面成为耗时瓶颈。将最终源 Token 聚合迁移至
Send 核，使其在本核 DIRECT 返回完成后即可探测结果，与其他核的 M4 partial 返回重叠。
默认仍为 32 Send / 32 Read，M5 跟随 Combine 编译期 Send 核数宏，不新增运行参数。

### 已确认方案

| 核组 | 执行顺序 | 任务所有权 |
| --- | --- | --- |
| Send | M1 本地暂存 → M3 DIRECT 返回 → M5 最终聚合 → M6 排空 | DIRECT 行按 Send 核均分；源 BS Token 独立按 Send 核均分 |
| Read | M1 本地暂存 → M2 同步 → M4 partial 返回 → M6 排空 | type2 行按 Read 核均分；最后 8 核继续负责 M2 通知 |

- 每核完成自己的 M3 即可进入 M5，无 DIRECT 行的 Send 核同样执行 M5；不等待其他 Send
  核，不增加 SyncAll，不给 Send 核增加 serverReady 控制块或 M2 消费职责。
- M5 使用 Send 组内核号和 Send 核数划分 BS，余数分给前面的核。一个 Token 的全部
  有效副本、yOut 写出及结果 Flag 清理始终归同一核；没有源 Token 的核正常收尾。
- M5 继续按 sourceMask 检测 DataAsFlag，单个 Token 全部有效副本到齐后才聚合；未到齐
  则扫描其他 pending Token。M5 不要求本卡或远端 M4 整体结束。
- M1 的本核 stage 写出和 prepare 发布先于 M3/M5；M4 的执行不依赖 M5 返回，避免
  将提前启动的接收等待引入生产者依赖环。
- M3 配对回收其所有事件，包括预置但未使用的 lane；在 M5 首次覆盖 scratch UB 前保留
  一次本核 PIPE_ALL。常驻控制区不复用，M5 重新初始化自身 lane 和事件；M6 排空剩余
  Flag 清理及 yOut 写出。PIPE_ALL 不代表其他核完成或远端结果到齐。
- M5 每核元数据与 lane 预算改按 ceil(BS / SendCoreNum) 计算，Host 与 Kernel 共用同一
  预算函数。默认 32/32 恰好等大，不能据此省略对非对称配置的检查。
- M5 内部 probe、按 k 顺序的 FP32 加权聚合、输出精度、通信布局及公开接口保持现有契约。
- breakdown 的 m5_final 事件归属 Send 核，样本数为 SendCoreNum；M4/M2 仍归属 Read 核。

### 实施与验证顺序

1. 修改 `tilexr_ep_combine_memory_server_dedup_kernel.h` 的调度和参与核映射，调整
   `ep_memory_server_dedup_combine_config.h` 的 M5 预算及相关生命周期注释。
2. 调整 `tools/validation/measurements.py`、`operator_measurements.py` 的 M5 核归属和
   样本数；事件 ID 沿用，历史 M3/M4 阶段规则保持原含义。
3. 更新 M5 数值/Flag/双窗口测试和实际 kernel 事件测试，覆盖无 DIRECT、无源 Token、
   BS 不整除核数、BF16/FP16、K=1/6/32、M3→M5 资源交接及延迟到达。验证非对称核数
   的任务覆盖、预算和 breakdown，防止 32/32 隐藏错误。
4. 运行受影响的 Host/统计回归，并在 blue CANN 9.1 A5 环境分别编译 profiling OFF/ON
   的所属 Host 和 kernel，记录源码身份、配置、编译器和日志。
5. 设备侧使用同一 shape、路由、核数、dtype、loops/warmup 对比修改前后整体 Combine
   性能；breakdown 单独检查 M5 核归属及 M4/M5 的重叠，不以模块耗时相加作为总耗时。

### 收益边界与维护要点

预期收益来自更早开始 M5 并重叠 M4；M5 仍受真实副本到达时间约束，M3 较重时也可能
延后 M5。此尝试不承诺固定收益，以 A5 实测决定是否保留。

迁移核角色必须一起修改运行分区、Host/Kernel 共用 UB 预算、breakdown 核归属及其测试。
只移动调用在 32/32 下可能通过，但非对称配置会出现元数据容量和统计归属不一致。

### 验证记录

2026-09-21：已实现，基于 `0e059cb` 加本次修改，开发侧验证如下：

- Host CTest 9/9 通过：M1/M2、M3、M4、M5、实际 kernel profiling、Host 准入及核数配置。
  M5 数值/清理用例在 32/32、16/48、48/16 三种 Send/Read 分配下通过；配置编译覆盖
  全部允许核数；覆盖空任务、非整除分区、延迟到达、DIRECT→M5 交接和三轮双窗口复用。
- 模块性能统计测试 11/11 通过，包含 M5 所有允许核数的角色/样本数检查，以及错误核归属拒绝。
- blue CANN 9.1.0、ccec/Clang 15.0.5、A5 `dav-c310-vec`：profiling OFF/ON 分别编译
  `tilexr-ep` 及所属 kernel 成功，exit=0。这是开发侧两种编译配置验证，不改变普通入口的
  单安装产物约定。当前非文档源码摘要与送编快照一致。
- 源码快照身份、diff、Host 日志、编译配置/命令、编译器版本和产物 SHA256 保留在
  `build/combine-m5-send-verification/`；本地 CTest 构建目录为 `build/combine-m5-send-host/`。

未运行本次 A5 多卡设备用例，Host 替身不证明真实异步流水或设备数值正确性。
实际功能及性能收益仍需用户环境比较；当前状态为“实现与开发侧验证完成，设备收益待验证”。
