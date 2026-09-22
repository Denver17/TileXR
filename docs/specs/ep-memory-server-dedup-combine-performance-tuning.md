# Server Dedup Combine 性能调优

本文记录已确认的优化尝试、实现边界和验证结果。功能通过、编译通过与实际性能收益分别记录。

## 分步性能验证版本

此前 `f0d8191` 将优化点 2、3 联合提交。为分别测量收益，在已发布历史之后追加两个
连续提交。`00fc323` 为第一步，仅启用优化点 2，M3 保留基线任务顺序；本提交为第二步，
在其上启用优化点 3 的 server 分组预扫描和发送顺序，M4 配置保持第一步不变。

| 版本 | 实际启用范围 | 收益比较对象 |
| --- | --- | --- |
| `a2dd290` | 优化点 1：M5 移至 Send 核 | 基线 |
| `00fc323` | 优化点 1＋2：M4 240 KiB、最多 3 读、一套计算、剩余分配发送 | 对比 `a2dd290` 得到优化点 2 收益 |
| 本提交：`perf(ep): apply optimization 3 after M4 buffer tuning` | 在上一步增加优化点 3：M3 按 server 分组发送 | 对比 `00fc323` 得到优化点 3 增量收益 |

验证环境逐个 checkout 对应提交后，重新执行 `bash build_all.sh && bash sync.sh`，再使用
相同的 `run.sh` 参数分别测量。每笔先确认功能，再对比 performance；breakdown 独立采集。
固定路由、shape、核数、dtype、loops/warmup 和机器/卡配置，保留实际 kernel 哈希及结果。
`f0d8191` 包含两项优化，不能作为“未优化 3”的比较基线。

### 分步版本开发验证

2026-09-22，第一步（仅优化点 2）的 Host 回归 9/9 通过；blue CANN 9.1.0 / ccec
Clang 15.0.5 / A5 `dav-c310-vec` profiling OFF/ON 分别编译 `tilexr-ep` 及所属 kernels
成功，exit=0。已核对源码快照与工作区的非文档文件一致；相对 `a2dd290`，生产修改只有
M4 预算/分配及 Read 核 arena 容量，M3 路径与基线相同。证据保存到
`build/combine-split-verification/opt2/`。设备正确性与性能收益待用户验证。

同日，第二步基于 `00fc323` 的 Host 回归 9/9 通过；同一 CANN 9.1.0/A5 工具链的
profiling OFF/ON 分别编译 `tilexr-ep` 及所属 kernels 成功，exit=0。源码快照与工作区
非文档文件一致。相对第一步只有 M3 分组及对应测试变化，生产和测试代码与已审查的
`f0d8191` 相同；独立快照和日志保存在 `build/combine-split-verification/opt3/`。
两个版本均未运行本次 A5 多卡设备验收，功能及性能收益仍待用户分别反馈。

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

## 尝试优化点 2：M4 限制读 buffer，增加发送 buffer

### 方案与容量依据

- M4 输入 buffer 上限改为 3，保留一套 `cast/product/sum/low` 计算工作区。
  先保证至少一组输入/输出可用，再按实际剩余 UB 尽量分配发送 buffer，发送上限仍为
  每个事件方向的 8 个 lane。大 shape 放不下 3 个输入时允许更少，不能挤占最小输出槽。
- M4 总预算为 **240 KiB**，包含常驻控制区、任务/表缓存、计算区和读/发 buffer。
  CANN 9.1.0 `tikcpp/tikcfw/impl/utils/kernel_utils_constants.h` 的 `__NPU_ARCH__ == 3510`
  分支定义 `TOTAL_UB_SIZE = 248 * 1024`；同版本 Ascend950 平台 ini 的 `ub_size=253952`。
  物理 256 KiB 中默认预留 6 KiB VF 栈和 2 KiB Ascend C 空间，240 KiB 再额外留 8 KiB
  余量。参见[华为 asc-devkit 3510 架构说明](https://asc.gitcode.com/guide/programming_guide/advanced_programming/hardware_implementation/architecture_spec/npu_arch_3510.html)
  （master 预览文档，容量已与服务器安装的 CANN 9.1 交叉核对）。
- 仅 Read 核的 arena 扩至 240 KiB；M1 的 lane 算法、Send 核的 M3/M5 预算仍为原有
  190 KiB。Host 和 Kernel 共用 M4 预算函数，避免允许的 shape 和实际分配不一致。
- 32 Send / 32 Read、H=7168、K=6、TokenNum=576 时：固定占用 109,920 B，
  3 个输入共 43,008 B，6 个输出共 92,160 B，总计 245,088 B，预算内剩余 672 B，
  此外保留上述 8 KiB 余量。TokenNum=10000 时输出降为 5 个；不把 6 个写成固定值。
- 输入在 Cast 消费完成后复用；输出只在自身 MTE3 完成后复用。保留原 FP32 乘加顺序、
  最后一次低精度转换和 DataAsFlag 格式，不增加逐 task 的 PIPE_ALL。

### 收益与维护边界

目标是减少发送尚未完成时对 Vector 打包及后续计算的阻塞；不保证读带宽仍能完全隐藏。
需要以相同参数比较整体 Combine、M4 耗时和流水等待。190 KiB 是旧软件预算，不能再作为
A5 物理上限引用；改动单个阶段预算时必须同时检查 arena 实际容量、Host 准入、边界测试
及事件上限。Host 替身按 CANN 可用 248 KiB 检查分配，而算子自行保留 240 KiB 预算。

## 尝试优化点 3：M3 按目标 server 分组，跨 server 优先发送

### 方案与所有权

1. DIRECT Gather 后沿用原来的 Send 核均分，每核仅处理自己实际分到的 `count` 个任务。
2. 发送前顺序扫描这些任务的 Table2，按目标 `sourceRank / 8` 计数，形成每个 server
   子 list 的有效长度及连续索引区间。缓存完整的 source/token/topk 三元组。
3. 使用稳定分组：组内保持原任务顺序；组间按 `(本 server + step) % server数`，
   `step=1..server数` 遍历，所以本 server 最后。空 list 自动跳过。
4. payload 始终通过索引映射回原始 expertOut 行，目标 rank、token/topk 槽位和 Flag
   不变；lane 按实际发送序号轮转，跨 server 不额外排空流水。
5. 本核所有 M3 发送回收后，仍按优化点 1 进入 M5；分组不引入跨核等待。

Gather 的 V→S 同步完成后，复用已不再使用的 `typeUb_` 缓存本核 Table2，复用 `indexUb_`
存放分组任务索引，保留 `packedUb_` 的原始行映射。由于 Send 核数至少为 16，
`12 * ceil(TokenNum / SendCoreNum)` 可放入按 256 B 对齐的 `4 * TokenNum` 数组，
极小任务由最小 256 B 分配覆盖。分组使用至多 16 个 server 的标量长度/游标数组，
不新增 UB 分配或通信区，也不减少 payload lane。

预扫描仍使用原来的有序 Table2 缓存，避免按重排后的行复用仅支持单调前进的缓存。
发送阶段直接消费已缓存三元组，不再读 GM Table2；维护时不能仅重排 packed 行号而
继续使用 `row >= cacheEnd` 的旧缓存判断，否则绕回较小行号会命中错误表项。

### 收益边界

目标是让跨 server DIRECT 更早发出、覆盖远端传输时间；每核预扫描和稳定分组增加了标量
工作，收益须实测。历史联合版本 `f0d8191` 只能报告组合收益；分别归因时使用上方
分步版本，固定其他变量进行逐步 A/B。

### 历史联合版本 `f0d8191` 验证记录

2026-09-22：基于 `a2dd290` 加本次修改，240 KiB 最终版本开发验证完成，设备功能和性能
收益待验证。已覆盖：

- M3：2/3/4/16 server、本 server 为首/中/末、取模绕回、空 list、仅本 server、
  空任务/极小任务、每核 12 个任务及余数、稀疏行和缓存换块；检查实际发送顺序、
  Table2 先扫描后发 payload、原始行内容/目标槽位/Flag 和双窗保护。
- M4：BF16/FP16、3 读 buffer 滚动补取、发送槽轮转、常驻控制区和工作区边界，
  K=1/6/32 及全部允许核数的容量检查；M1/M2/M5 和实际 kernel profiling 回归。
- Combine Host CTest 9/9 通过，包含 M1/M2、M3、M4、M5、M5 的 16/48 和 48/16
  非对称核数、Host 准入、配置预算及实际 kernel profiling。M4 扩容后，旧 H=10145
  拒绝断言失效；更新为该形状接受，并检查 K=6 下 M5 的 H=11008/11009 准入边界。
- 共享 Host 替身的 Dispatch counts/source_send/receive_packet 回归 3/3 通过。
- blue CANN 9.1.0 / ccec Clang 15.0.5 / A5 `dav-c310-vec`，profiling OFF/ON 分别
  编译 `tilexr-ep` 及所属 kernels 成功，exit=0；非文档输入已与送编快照逐文件核对。
- 主审与独立审查未发现需修复缺陷。源码快照、差异、配置/命令、工具链、产物哈希和
  编译/Host 日志保存在 `build/combine-opt23-verification/`，本地 Host 构建目录为
  `build/combine-opt23-host/`。送编后仅补充本文验证记录。

未运行本次 A5 多卡设备用例，Host 替身和交叉编译不能证明设备异步重叠或加速。
设备验收继续检查单轮/连续多轮 yOut 与 Flag，并分别采集 performance 和 breakdown；
M3 breakdown 区间包含新增预扫描成本。比较时固定 shape、路由、核数、dtype、loops/warmup
及构建配置，记录最终加载的 kernel 哈希。
