# Server Dedup Combine 性能调优

本文记录已确认的优化尝试、实现边界和验证结果。功能通过、编译通过与实际性能收益分别记录。

## 分步性能验证版本

此前 `f0d8191` 将优化点 2、3 联合提交。为分别测量收益，在已发布历史之后追加两个
连续提交。`00fc323` 为第一步，仅启用优化点 2，M3 保留基线任务顺序；`6e09ad8` 为第二步，
在其上启用优化点 3 的 server 分组预扫描和发送顺序，M4 配置保持第一步不变。

| 版本 | 实际启用范围 | 收益比较对象 |
| --- | --- | --- |
| `a2dd290` | 优化点 1：M5 移至 Send 核 | 基线 |
| `00fc323` | 优化点 1＋2：M4 240 KiB、最多 3 读、一套计算、剩余分配发送 | 对比 `a2dd290` 得到优化点 2 收益 |
| `6e09ad8` | 在上一步增加优化点 3：M3 按 server 分组发送 | 对比 `00fc323` 得到优化点 3 增量收益 |

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


## M2 两阶段 profiling（2026-09-22）

基于优化6 `0d2cd57`，保留原 `m2_control` 总区间，新增两个子区间，用于区分本卡
准备/通知与本server就绪等待的耗时，不作为新的性能优化点。

| trace 子区间 | 核范围 | 观测内容 |
| --- | --- | --- |
| `m2_prepare_notify` | 56–63 核，共8核 | `NotifyServerCard`：等待本卡64核 prepare_done，生成并下发 prepare 清零及本server目标卡的 ready 通知 |
| `m2_server_wait` | 全部 Recv 核 | `WaitServerReady`：轮询本server 8卡 ready，完成后下发对应 ready 清零 |

默认32个Recv核中，32–55核只有第二段，56–63核依次经过两段；其他合法核划分按相同
角色记录。Send核不产生M2记录。第一段较长表示本卡准备完成/通知路径较长，第二段
较长表示在该核开始等待后，本server各卡ready尚未齐备；需结合8个通知核的第一段
结束时间判断拖尾，不能仅凭第二段较长就归因网络。

仅增加Scalar打点，未增加事件同步、PIPE_ALL或MTE3排空。两个子区间结束均表示相关
函数完成指令下发，清零/通知MTE3可能仍在途；不是单独的远端写完成耗时。父子区间为
包含关系，统计时不能把 `m2_control` 与两个子区间相加。

`run.sh -m 2` 使用更新后的kernel、mapping和解析器即可，无需新增参数。沿用当前
编译/同步流程，重新构建并同步后采集；trace在各核原M2区间内展示两个子区间，报告
同时列出对应耗时和样本数。解析器兼容旧的仅模块capture；出现新子区间时，严格检查
8个通知核和全部Recv核的覆盖、重复及核角色。

验证：实际kernel profiling与M1/M2 Host测试2/2通过；测量/汇总Python回归47/47、
旧profiling回归7/7通过，包含六种核划分的两阶段样本数、trace耗时与父子关系，以及
缺阶段、缺核、缺结束点、重复和错误角色的拒绝测试。profiling开关前后Send、普通Recv
和通知核的HardEvent序列一致。blue CANN9.1/A5 OFF/ON构建均通过，送编全部非文档
输入与最终源码一致；证据在`build/combine-split-verification/m2-profile/`和
`build/m2-profile-*.log`。尚未采集此次修改的设备trace。

## M3/M5 边界与 M4/M5 逐 token profiling

基于 `6e09ad8`，新增诊断打点，用于观察各核任务下发节奏，不作为新的性能优化点。

| trace 事件 | 观测边界 |
| --- | --- |
| `m3_mte3_complete` | 本核 M3 现有所有 lane 回收及 scratch 复用 `PIPE_ALL` 之后；无 DIRECT 任务的核也记录该点 |
| `m5_final` | 从现有 scratch 复用 `PIPE_ALL` 之后开始，包含 M5 初始化、Flag 探测、聚合及末尾回收 |
| `m4_token_issue[n]` | 一个任务的 Table2/3 读取、payload 读入、加权计算、打包与发送指令下发完成 |
| `m5_token_issue[n]` | 一个已就绪任务的 payload 读入、加权计算与 yOut 写出指令下发完成；最后一个任务包含原有批量 Flag 清理下发 |

`n` 为本次 capture 中该核从 0 起的实际处理顺序，不是原始 TokenId，也不是 expertOut
行号。M5 的 pending 交换和 readiness 轮询可能改变处理顺序。每核所有 token 区间放在
同一条 `token_issue` trace 轨道，便于比较相邻任务的下发间隔；Dispatch 和 Combine
继续使用独立记录区与 trace。`run.sh -m 2` 使用更新后的 mapping 和解析器，无需新参数。

逐 token 只增加 Scalar 时间戳记录，不增加 SetFlag/WaitFlag、PIPE_ALL 或 MTE3 完成等待。
已有的依赖等待和 buffer 复用阻塞仍属于区间耗时，因此不能把区间长度当作纯计算耗时
或独立的端到端传输完成耗时。M5 尚未就绪的 Flag 探测留在 `m5_final` 内、token 区间外；
M3→M5 原有复用屏障计入 `m3_direct`，完成标记和 `m5_final` 起点均位于屏障之后。`m3_mte3_complete` 只证明本核 M3 的 MTE3
完成，不代表其他核或远端 M4 已完成。

逐 token 记录不再适合旧的 32 条固定额度。Host reservation 和 Kernel writer 共用
65536 条/核/latest launch，沿用测试 consumer 已分配的容量，未新增 UB。直接调用公开 API
采集 profiling 时，需在 communicator 初始化前设置 `TILEXR_PROFILING_RECORDS_PER_CORE=65536`
或更大合法容量；runtime 默认的 4096 不满足此 reservation，`run.sh` 已自动满足。M4 每核最多
1250 行；M5 在 190 KiB 内仅元数据就至少需要 12 B/token，每核少于 16384 个任务；
两条记录/task 加模块边界可被覆盖。重复调用仍只保留最后一轮，初始化重置 count/drop，
导出不解析旧尾部。解析器保留旧模块级 capture 的可读性，新 token 区间出现时额外检查
角色及总数（M4=type2 行数，M5=BS）。性能对比继续使用 profiling OFF；新增时间戳写入
本身有开销，breakdown 用于解释下发节奏。

维护教训：增加循环内打点时需一起检查 Host/Kernel 记录额度、重复事件配对、模块覆盖
校验和 trace 命名。仅增加事件会使旧容量截断，或被“每模块每核仅一个区间”的校验误拒；
本次通过实际 kernel 的多 token capture 和 records.v1→trace 测试覆盖这些边界。
`MTE3_V` 的目标是 Vector 队列，不能仅在其后读取 Scalar 时间戳就宣称 MTE3 已完成；
这里复用已有 `PIPE_ALL` 后再打点。blue CANN 9.1 的 `kernel_reg.h` 中 A5/3510
`PipeBarrierImpl<PIPE_ALL>` 直接发出 `pipe_barrier(PIPE_ALL)`，无须新增完成同步。

2026-09-22 开发验证：最终版本 Host CTest 11/11、profiling/trace Python 回归 29/29
通过；覆盖真实 kernel 的非空 M3、多 token M4/M5、超过旧记录额度、重复调用、区域隔离、
runtime OFF 不写记录、ON/OFF 硬事件序列一致，以及 token 事件配对/核角色/总数与 trace
展示。blue CANN 9.1.0 / ccec Clang 15.0.5 / A5 `dav-c310-vec` profiling OFF/ON
分别编译 `tilexr-ep` 及 kernels 成功（exit=0），932 个非文档输入与送编快照一致。
独立审查发现的 Scalar 完成边界问题已按上述方案修正，最终差异完成主审。
证据位于 `build/combine-split-verification/token-profile-final/`。未执行 A5 多卡运行，
实际 trace 时序和新增打点开销待验证环境确认。

## 尝试优化点 4：M5 回到 Recv 核，与 M4 共用 buffer 并延续发送流水

### 状态、约束与目标

2026-09-22：用户反馈优化点 1 的 M3→M5 调度成为新的瓶颈，要求 M5 由执行 M4 的同一批
Recv 核承担，并复用 buffer；进一步明确 **M4/M5 之间不加 PIPE_ALL，不要求 M4 写完
才能开始 M5 Flag 检查和聚合**。本节方案已基于 `c1ef152` 实现，共享布局、定向交接、
核角色和 profiling 解析同步修改。尚未执行 A5 多卡验收或取得收益数据。

Recv 对应现有 Read 核，即 `core >= SendCoreNum`。保留优化点 2 的 M4 读上限 3、
240 KiB 总预算和优化点 3 的 M3 server 分组发送。不能采用“等待 M4 全部 MTE3 完成后
重排整个 scratch”的方案；M4 尚在发送的输出必须继续存活，同时给 M5 的读、算留出
可独立使用的空间。用户暂未附本次瓶颈逐 rank 数据，根因和收益仍以 trace/A/B 为准。

### 核角色与任务划分

| 核组 | 执行顺序 | 任务所有权 |
| --- | --- | --- |
| Send `[0,S)` | M1 → M3 → M6 | DIRECT 行仍按 S 个核划分；保留 M3 完成打点 |
| Recv `[S,64)` | M1 → M2 → M4 下发 → 读/算资源交接 → M5 → M6 | M4 按 type2 行划分；M5 独立按 BS 个源 token 在 R=64−S 个核划分 |

每个 Recv 核下发完自己的 M4 即可交接给 M5，不等待其他核的 M4，不加 SyncAll 或全卡
M4 完成 Flag。跨核可以出现 M4/M5 重叠；同核可以出现 **M4 尾部 MTE3 与 M5 Flag
检查、MTE2、Vector 聚合重叠**。M5 仍只聚合实际已就绪的 token：若其依赖的本核/远端
M4 partial 尚未到达，原有 readiness 检查自然继续等待，不绕过数据依赖。

M5 调用参数改为 `participant=core−S`、`participants=R`。令 `base=BS/R`、
`extra=BS%R`，则 `count=base+(participant<extra)`，
`begin=participant*base+min(participant,extra)`。预算按 `ceil(BS/R)`；执行按真实
区间，保证 BS 覆盖无重复。不能把 M4 row/task 当作 M5 TokenId。支持原有全部允许核数
S=16/24/32/40/48/56，不能只在32/32下交换调用。

无 M4 task 的核仍执行 M2，并接收自己的 M5 token；无 M5 token 的核仍完成 M4，
随后在收尾处理未完成的发送。最后8核的 M2 通知职责不变。M4 输入来自 expertOut/M1
stage，不依赖 M5 yOut，因此不增加生产者等消费者的循环依赖。

### 共享布局：输出地址固定，其他区域在生命周期结束后复用

Recv 只分配一块 240 KiB arena，Send 仍为190 KiB。UB 按核独立，Send 空闲后的 UB
不能转移给 Recv。引入 Host/Kernel 共用的内部 `M45WorkspaceLayout`，统一返回各区域
offset、长度、lane数和stride；阶段类接收相同布局/视图，不能再各自从scratch起点随意
重排。不重置 TPipe，不二次 InitBuffer，也不通过 GM 搬运 UB 内容。

```text
同一 Recv 核的 240 KiB arena
[常驻控制 R0][一套 FP32 工作区][阶段临时区 P][共享输入 I×L][共享输出 O×W]
                 M4 算              M4 表/low       M4 读       M4 发(仍在途)
                   ↓ V/MTE2→Scalar 交接，无 MTE3 排空 ↓             │
                 M5 算              M5 元数据/Flag   M5 读       M5 写某槽前
                                                              等该槽 MTE3_V
```

| 区域 | M4 用途 | M5 用途与交接条件 |
| --- | --- | --- |
| R0 常驻控制区 | prepare/ready/notify 等 | 保持原地址和生命周期；本次不回收控制区 |
| FP32 工作区 | 一套 cast/product/sum，共3个H长度FP32数组 | 同地址复用；M4 Vector最后一次使用完成后接管 |
| 阶段临时区 P | task行号、Table2/3缓存、低精度low | 读/算交接后覆盖成 masks/pending/weights/flags/work/probeSum；取两阶段需求最大值 |
| 输入槽 | 读expertOut或stage，每槽有效2H B | 同地址按480B payload块读入；前一阶段输入事件退休后重新初始化 |
| 输出槽 | 480B payload＋32B Flag组成的packet | **不移动、不覆盖、不统一回收**；M5逐槽等待其旧MTE3完成，再写连续yOut数据 |

“一套计算”不代表 cast/product/sum 可互相别名；它们同阶段仍需同时存活。阶段临时区
可以取max，是因为 M4 的low已完成打包、表数据和task均不再被访问；输出区不能与M5
元数据、Flag、输入或计算区重叠，因为M4 MTE3仍可能读取它。

输入槽统一使用 `L=ceil(2H/480)*480` B，M4仍只从GM复制有效2H字节；不能使用原M4
`align32(2H)`作为共享stride，否则M5尾块可能覆盖下一槽。输出槽统一使用
`W=ceil(2H/480)*512` B；M5只写槽内前2H字节并传给yOut，但保持W的物理步长，不能
将多个M5输出紧密重排到尚未完成的M4槽上。旧DataAsFlag尾部不参与M5 yOut。

M5必须重新读取sourceMask/expertScales并建立pending，不沿用M4 Table3权重或partial
中间结果，保留“partial低精度舍入后，源端再次加权”的数值契约。Flag探测区与批量清零
源维持当前独立生命周期，不能借用尚在发送的输出槽。

M4行号搬运也须适配新布局：Gather结果先按当前方式压缩到旧type数组，再将本核任务行
复制到共享阶段临时区，之后才允许覆盖旧M1布局。小H时新目标可能与旧type压缩区重叠；
目标在源地址之后，应采用反向复制等memmove语义，不能直接正向循环或提前初始化FP32
区破坏源行号。最多ceil(TokenNum/R)个标量索引，不新增整张表的临时UB。

### 分配规则与240 KiB预算

令 `A(n,a)=ceil(n/a)*a`、`B=ceil(2H/480)`、`N4=ceil(TokenNum/R)`、
`N5=ceil(BS/R)`，均使用防溢出的64位中间值：

```text
C  = 3*A(4H,256)                         # 一套FP32工作区
P4 = A(4*N4,32) + 384 + A(32*K*8,32) + B*480
P5 = 2*A(4*N5,32) + A(4*N5*K,32)
     + (K*B*32+256) + A(K*B*4,256) + 32
P  = max(P4,P5)
F  = R0 + C + P
L  = B*480
W  = B*512
I  = min(3, floor((240KiB-F-W)/L))        # 先保留至少1个输出
O  = min(8, floor((240KiB-F-I*L)/W))
M5输出数 O5 = min(O,7)                    # 沿用M5的0..6 payload事件约定
总占用 = F + I*L + O*W <= 240KiB
```

计算减法前先检查F+W是否越界，I/O至少为1，否则拒绝配置。M4/M5均使用I个输入，
M4使用O个输出，M5使用其中O5个输出；若O=8，第8个槽留到末尾回收，不作为M5 payload
槽，也不能挪给Flag工作区。M5 event7保留原Flag操作约定；各方向HardEvent需分别检查
占用，不能将同号不同方向当成同一个事件。

默认示例：S/R=32/32，TokenNum=576，BS=96，H=7168，K=6，BF16/FP16：

| 资源 | 占用/数量 |
| --- | ---: |
| 常驻 R0 | 7488 B |
| FP32 C | 86016 B |
| M4临时 P4 | 16416 B |
| M5临时 P5 | 6976 B（覆盖P4，非新增） |
| 共享输入 | 3 × 14400 B = 43200 B |
| 共享输出 | 6 × 15360 B = 92160 B |
| 合计 / 预算剩余 | **245280 B / 480 B** |
| M4读／算／发 | **3／1套／6** |
| M5读／算／写 | **3／1套／6** |

M4原输入stride14336变为14400，三槽总共增加192 B，仍保留6个发送槽。M5从当前
Send/190 KiB版本的5读/1写改为3读/6写；它不是此前整池退休方案的7读/3写，因为现在
M4输出与M5读/算确实同时存活，必须按上述布局计费。M5在有效贡献超过3时沿用现有
Cast后滚动补取；6个输出不等于6个token同时进行FP32计算，计算工作区仍只有一套。

TokenNum=10000时，默认核数下P4=17600 B、O=5、总占用231104 B，不能把3/6写为
常数。示例改为16 Send/48 Recv时总占用恰为245760 B，也应合法；预算之外既定余量
继续保留。大BS、小H、高K及非对称核数可能使P5大于P4或迫使I/O减少，必须统一计算，
不能分别验证两套旧布局后假定它们可并存。

### 事件交接：不排空发送，按槽继承所有权

输出槽从初始化到M5结束遵守同一状态契约：每槽始终有一个可消费的`MTE3_V`完成事件。
尚未使用的槽来自初始seed；已发送的槽来自最近一次DataCopy后的SetFlag。

1. **M4开始**：共享输出槽统一初始化并只seed一次。M4仍在打包覆盖某槽前Wait该槽
   `MTE3_V`，发送后Set该槽`MTE3_V`。无M4任务时由同一共享初始化路径提供可用槽，
   不能让M5等待一个从未设置的事件。
2. **M4停止下发**：删除当前Process末尾“逐输出Wait＋PIPE_ALL”的强制发送排空。
   保留/完成输入`V_MTE2`事件配对，再使用定向`MTE2_S`与`V_S`交接，确保输入队列
   及M4最后的Vector打包不再访问将复用的输入、FP32和low/表缓存。**不等待MTE3**。
   这两个定向交接也要配对，不能仅凭M4函数返回就让Scalar覆盖仍被Vector读取的low。
3. **M5开始**：重建临时区和输入事件，输出槽沿用原地址、stride和未消费的完成事件。
   删除M5当前无条件为输出重新SetFlag的做法，避免把仍在途槽误标为可覆盖。
4. **Flag/聚合先行**：M5执行Probe和Load/Cast/Muls/Add，均不触碰输出槽，不依赖
   M4写完。到某token最后的输出Cast前，复用现有`WaitFlag<MTE3_V>(outputLane)`；
   此Wait阻塞该Vector写输出动作，保证其后不会覆盖仍被M4 MTE3读取的packet。
   如果该槽已完成，就自然通过；若未完成，仅等本槽，不扫全体输出。
5. **继续轮转**：M5写yOut并Set该槽`MTE3_V`，后续M5任务沿用相同槽协议。M5首槽
   继承M4的下一输出位置：`start=(M4TaskCount % O) % O5`，随后
   `lane=(start+completed)%O5`，优先使用未用过或较早提交的槽；O=8时跳过专留收尾
   的slot7。这里既可能等到M4发送，也可能等到前一M5写出，不需要额外Host/GM状态。
   此buffer轮转位置与trace从0开始的任务序号是两个独立概念。
6. **统一收尾**：输出事件最终由共享所有者回收一次，覆盖全部O个槽，包括从未被M5
   使用的M4槽、O=8时的第8槽、以及M5 count=0的路径。M5不能只回收O5或提前return
   留下M4在途源；也不能M5先回收O5、调用层再重复Wait全部O。M6保留原最终PIPE_ALL，
   退休yOut和独立Flag清零写出后再退出。本要求取消的是M4/M5之间的屏障。

初始化/交接由调用层共享workspace明确负责；两个阶段类只执行自身任务，不能各自
初始化、回收同一组输出事件。输入事件仅在非空阶段seed和drain；空阶段不等待未seed的输入事件，下一非空阶段
独立seed。输出则在两个阶段之前始终seed，最终统一drain。逐token打点不增加同步，
以上定向交接仅发生在阶段切换处。

### 实现与profiling适配

- `ep_memory_server_dedup_combine_config.h`增加共享布局计算，Host和Kernel共用；
  M5归属改为R、预算统一为Recv 240 KiB；更新blockCount/UB界限断言和准入测试。
  Host仍按合法最大TokenNum检查，Kernel按实际TokenNum生成布局，保留公开接口。
- `tilexr_ep_combine_memory_server_dedup_kernel.h`在Recv分支调用M4→M5，集中管理
  workspace及输入/输出事件交接；Send只做M3。当前`ReturnPartialTokens`在count=0时
  早于partial.Init/Process返回，共享输出seed必须放在此早退之前的调用层，确保空M4
  也能交接。旧M1/M2→M4入口屏障保留，以安全复用其scratch；不复制到M4/M5之间。
- `tilexr_ep_server_dedup_partial.h`、`tilexr_ep_server_dedup_source_aggregate.h`接收
  同一布局的LocalTensor视图和共享output stride/数量；删除各自重复的输出seed/drain，
  将M4的低精度low放入可交接临时区。M5输出地址使用共享W步长，GM yOut仍使用2H。
- 保留`m3_mte3_complete`在Send核。`m4_partial`结束于读/算交接后、M5开始前，
  **不再表示M4尾部MTE3已全部完成**，在文档/报告attributes记录此口径；不能为了
  “完成打点”重新增加M4发送排空。`m5_final`迁到Recv，样本数改为R。
- 保留`m4_token_issue`、`m5_token_issue`及事件ID。二者同核后解析器必须按模块分别
  从0编号，不能让M5沿用M4 sequence。用M4末尾issue与M5开头issue观察接续，但不能
  单靠Scalar issue trace断言硬件实际重叠；设备流水证据/性能对比另行验证。
- 更新`measurements.py`、`operator_measurements.py`角色/样本数与测试。源码、kernel
  和mapping统一构建同步；旧版本trace用对应版本解析器，不恢复checkpoint开关。
- latest每核65536条仍足够：同核M4≤1250 tasks，M5以240 KiB/最少12 B元数据估算
  <20480 tasks，两条/task加模块边界<43500。需改写旧的“仅M5且190 KiB”容量说明。
  Dispatch/Combine继续分区采集，性能OFF与breakdown ON分别运行。

### 开发和验收

作为一笔优化点4实验版本实现与交付，不改写此前2/3的独立提交。实现顺序为共享布局和
预算、共享槽事件所有权、M5核迁移、profiling/测试适配。与`c1ef152`固定配置进行A/B。

1. **布局**：全部允许核数；BS小于/等于/不整除R；TokenNum=0/极小/10000；K=1/6/32；
   H尾部和预算临界值。检查M4输出在途时与M5各live区不重叠，输入/输出共享stride，
   P取max，地址对齐和真实总占用。检查task转存的重叠搬运，禁止用源码正则充当验证。
2. **关键异步用例**：替身刻意保持M4 MTE3在途，同时供应不依赖该尾部的已ready源token，
   证明M5已发起Flag读取、payload读入及聚合；首次复用相应输出槽才等待并退休M4写。
   比较原partial字节/Flag及最终yOut，检查待发送源未被M5破坏。CPU替身只证明访问
   顺序与所有权，不宣称证明A5的物理重叠。fake已改为按MTE3完成事件的提交位置
   退休队列前缀，等待未使用的seed不会清空其后才提交的写。补充8个M4槽全部占用、
   O5=7时进入M5清Flag的用例；模拟器仍立即处理非MTE3完成方向的Wait，不能证明
   设备事件队列的物理执行时序。真实MTE3队列中更早提交的写也可能已一起完成。
3. **事件/空路径**：有/无M4与有/无M5的四种组合；M4少于输出槽数、M5少于槽数、
   O=8但O5=7、长轮转与未使用seed。确保事件每次恰好消费一次，输入交接不丢事件，
   所有遗留M4发送和Flag清零最终被M6退休。
4. **功能**：生产M4→M5同核替身、远端延迟到达、BF16/FP16、非对称核数；保持partial
   舍入参考语义。A5覆盖combine/both单轮/多轮、双窗、最终yOut/Flag及五表连接，
   Dispatch单跑回归，保留量化Dispatch→非量化Combine的现有衔接用例。
5. **profiling**：核归属、M4末尾不等待全体MTE3的口径、M4/M5各自token序号、两组
   总数、无丢记录、多轮latest与独立区域；token打点ON/OFF不增加硬事件序列。
6. **编译**：blue CANN9.1/A5，OFF/ON分目录构建kernel和Host，保存来源、工具链、
   配置、日志、退出码与产物哈希；核对目标平台定向同步和继承事件的实际调用。
7. **收益**：先固定32/32及shape、路由、dtype、机器/卡和loops/warmup，performance
   OFF比较每rank及min/median/max整体延迟、算法字节和带宽；breakdown ON看M4→M5
   下发接续、等待区间及跨核分布。模块时长不能相加为总耗时。M5输入从5到3可能增加
   滚动补取等待，输出从1到多槽也可能收益有限，均需实测，不只凭单个模块变短判定成功。

维护要点：跨阶段复用并不等于跨阶段全部排空。要按访问者生命周期拆分可交接区域与
仍在途的输出，用原地址和完成事件把输出槽的所有权传给下一阶段。只迁调用而遗漏
stride、空任务seed、最后未使用槽的drain，都会造成踩踏或等待丢失。


### 实现中的事件修正与维护经验

M5清Flag不能直接沿用旧的`V_MTE3(7)`：即使Scalar已下发M4的Wait，MTE3队列中的
Wait也可能尚未消费该方向的slot7通知。现在通过`V_S`确认Vector零值已生成，再用
`S_MTE3(7)`发布给MTE3；两个方向均不要求MTE3完成，保留尾部发送与M5读/算重叠。
M5 payload继续使用0..6，M4第8槽的`MTE3_V(7)`由共享所有者最后回收。

已对照blue安装的CANN9.1 `kernel_event.h`核查A5/3510 AIV：Set/Wait分别生成
`set_flag/wait_flag(srcPipe,dstPipe,id)`，`V_S`、`S_MTE3`和`V_MTE3`是不同方向。
跨阶段交接时必须区分“Scalar已下发Wait”与“目标流水已执行Wait”，不能仅凭函数返回
就重复发布同方向同ID的事件。Host fake不能替代这个队列方向检查或A5运行验证。

生产代码经主审及独立只读审查；共享fake补充事件提交位置模型后，针对空任务、8槽
继承、小H行号重叠搬运及非对称核数更新了测试。验证结果和设备边界见下节。

### 实现验证记录（2026-09-22）

- Host C++14 构建通过；EP Host CTest 串行运行 **51/53通过**，本次Combine布局、
  Host准入、M1–M5、profiling及共享fake涉及的Dispatch count/send/receive测试均通过。
  M5覆盖BF16/FP16、16/48、32/32、48/16核划分、8个在途partial槽、空任务与长轮转。
  初次并行运行曾被本地4 GiB cgroup OOM杀死；改为串行后的完整结果没有此问题。
- 两项失败为`test_tilexr_ep_server_dedup_layout`的“shared data follows both fixed
  control regions”和`test_tilexr_ep_server_dedup_combine_boundary_launch`的固定探针
  地址断言。将未修改`c1ef152`的src/tests独立导出、构建并执行同两项，复现相同失败。
  记录为基线遗留，本次不据此宣称完整Host套件全绿，也不修改无关通信布局。
- Python测量/trace回归 **73/73通过**：覆盖M5迁移到Recv、非对称核数、M4/M5模块内
  独立token序号、M4未排空MTE3的报告属性，以及既有M1–M4和算子独立采集。
- blue CANN9.1.0、ccec Clang15.0.5、A5 `dav-c310-vec`：profiling OFF/ON分目录
  构建`tilexr-ep`及kernels均通过（exit=0）。最终337个生产源文件与送编快照一致；
  送编后仅调整了Host M5异步用例的模拟DMA推进，已由最终Host回归覆盖。
- 实现完成主审、简化检查和独立生产代码审查。证据在
  `build/combine-split-verification/opt4-final/`，Host和Python日志分别在
  `build/opt4-host-final-tests.log`、`build/opt4-python-tests.log`；基线复现日志在
  `build/opt4-baseline-tests.log`。

设备验收状态：**待A5多卡功能、performance OFF和breakdown ON验证**。本轮只进行了
Host测试和真实CANN编译，没有测量硬件并发度或性能收益。对比基线使用`c1ef152`，
两侧保持相同shape、核数、路由、机器/卡及loops/warmup；使用各自版本的kernel与
trace解析器。拉取后按现有build/sync流程更新，`run.sh`参数不变，不新增checkpoint。

## 尝试优化点 5：M3 逐 server 扫描、均衡筛选并立即发送

以下为初版方案及讨论记录；`693a00d`已报告负优化。当前筛选方式以文末
“优化5/6修订：Vector筛选与密排计数”为准，禁止恢复逐候选Scalar遍历。

### 状态与问题依据

2026-09-22，用户反馈优化点4已有一定性能提升，但收益有限；尚未提供本轮量化数据和
完整验收日志。用户进一步明确当前发送过程的瓶颈是MTE3，希望把筛选工作尽可能与传输
重叠。本节方案已按用户授权实现优化点5，设备功能和性能待验证。实验以优化点4版本
`2800dac`为比较基线，保留M4/M5共享buffer方案。

Combine输入沿用Dispatch的专家排序，同一专家内部还具有源端rank局部性。当前
`ReturnDirectTokens`先按连续区间分核，再由`GroupDirectTasks`在本核内按server排序。
若本核区间只包含某个server的行，局部重排无法改变通信对象，其他核也不能协助处理，
因此优化点3在这种分布下效果受限。实际影响大小仍需结合路由分布和trace评价。

### 用户已明确的约束

- 只调整单卡S个Send核的M3任务分配及发送顺序，不改变发送流水、数据格式或接口。
- 各核按`(ownServer+step)%serverCount`、step=1..serverCount推进，本server最后。
  **server轮次之间不得新增任何核间或核内同步**，不等其他核，不等本轮MTE3完成。
- 不先筛完所有server再发送。完成一个server的扫描、筛选和计数后，立即下发本核
  该server的发送任务，随后扫描下一个server，争取与上一轮尾部MTE3重叠。
- 不要求核间锁步；快核可以提前进入下一server。某核在当前server没有任务时直接
  继续扫描下一server，不能等待其他核。Recv核不参与M3重分配。

据此撤销前一版“全server计数一遍、全server筛选一遍后才发送”的首版建议，不再保留
跨server阶段屏障或共享GM建表同步作为本优化点候选。现有`SelectRows(1)`仍先产生
完整DIRECT行号列表；取消的是随后全server分类/筛选造成的发送启动前等待。

### 推荐分配规则：桶内命中序号轮询，跨 server 延续轮转位置

设D为本卡DIRECT总行数，S为Send核数，c为本核Send组内编号。每核独立维护P，表示
已经扫描过的所有server的DIRECT任务总量，初始为0。对当前server从完整DIRECT列表
开头单调扫描；只有目标server匹配时，才递增本server的命中序号j：

```text
P = 0
sendSequence = 0
for step = 1..serverCount:
    server = (ownServer + step) % serverCount
    matched = 0
    owned = 0
    for row in 全部DIRECT原始行号（升序）:
        triple = Table2[row]                 # sourceRank / tokenId / topk
        if sourceRank / 8 != server:
            continue
        owner = (P + matched) % S
        if owner == c:
            本轮任务[owned++] = (row, triple)
        matched += 1
    P += matched                            # 全server总数，不是本核owned
    for task in 本轮任务:
        沿用现有读/打包/发送及buffer复用依赖
        lane = sendSequence % lanes
        sendSequence += 1
# 所有server处理完后，才执行现有M3收尾
```

不需要预知N[server]，当前轮扫描结束时matched自然就是N。实现时可用循环核号递增并
在S处绕回，避免每个命中任务都做取模；P的语义及任务归属保持上述公式。

每个核扫描相同的只读DIRECT列表和Table2，会独立得到一致的P、matched和owner，
因此不需要共享计数、原子抢任务或核间通信。不能只扫描旧的本核连续分片；也不能在
本核owned为0时跳过必要计数，否则后续P和任务归属会分歧。D=0可整体跳过M3数据发送。

把按server顺序连接起来的任务看成一条虚拟序列，owner就是其序号模S，因此同时保证：
每个server的任务数在核间差≤1，所有server合计在核间差≤1，且每个DIRECT行归属唯一。
每核总量为floor(D/S)或ceil(D/S)。跨server不能将轮转起点重置为0，否则每轮余数都
可能落到低核号，破坏总量均衡。N[server]<S时不可能每核都有任务，允许空份额。

这里采用桶内交错领取，不再坚持先前建议的桶内连续区间，因为后者需要先知道桶大小，
会增加计数/筛选遍数或缓存需求。桶内任务仍按原始行号递增，但相邻归属本核的任务可能
跨越S个命中项；输入读取局部性的变化应一并测量。目标server本身的流量偏斜不因分核而消失。

示意使用4核（生产S仍为16/24/32/40/48/56）：

| 发送次序及任务数 | core0 | core1 | core2 | core3 |
| --- | ---: | ---: | ---: | ---: |
| server+1：6，轮转起点0 | 2 | 2 | 1 | 1 |
| server+2：5，轮转起点2 | 1 | 1 | 2 | 1 |
| 本server：2，轮转起点3 | 1 | 0 | 0 | 1 |
| 合计13 | 4 | 3 | 3 | 3 |

### 流水与buffer生命周期

发送lane初始化、Flag初始化及事件seed只在M3级别做一次，不能每个server重复执行。
跨server延续sendSequence，保留既有逐槽输入/输出复用依赖；不重置lane游标、TPipe
或buffer，不插入server级`PIPE_ALL`、`SyncAll`、事件drain/等待或完成Flag。
所有server处理完毕后，才统一执行现有M3结束回收和完成打点。

当前server的Table2和任务列表只被Scalar用于生成payload地址与目标地址，MTE3实际
读取独立resultUb。当前server的所有发送指令下发后，可覆盖这些元数据来扫描下一server，
无需等resultUb的旧发送结束；在途payload/result槽仍由原有事件保护，绝不能作为筛选临时区。

继续保留完整packedUb行号列表；indexUb缓存**当前server本核任务的绝对原始row**，
typeUb缓存对应三元组，下一server复用相同位置。单server本核最多ceil(N/S)个任务，
不超过ceil(D/S)，三元组容量继续满足优化点3预算。扫描必须由packedUb原始列表驱动，
不能覆盖后续轮次仍需要的行号，也不能沿用旧的`packedUb[start+original]`发送寻址。
计划不增Send arena、不减少payload lane、不侵占M1常驻控制区。

每个server重新开始单调Table2扫描并重置缓存范围；沿用现有表搬运及消费所需的
MTE2→Scalar依赖，不能为了取消server屏障而读取尚未完成DMA的表。该数据依赖不是新增的
server完成等待，但表扫描次数增加会增加其执行次数，并可能受既有流水依赖间接限制。
首版倾向Scalar筛选，避免引入额外Vector结果交接；若后续改为Vector筛选，仍须遵守
无server边界同步及不改发送流水的约束，单独核查其与payload打包的资源竞争。

### 重叠机会与代价

逻辑下发顺序为`扫s1 → 发s1 → 扫s2 → 发s2 → …`。发s1仅表示任务指令已下发，
其尾部MTE3可继续执行，同时Scalar/MTE2推进s2扫描；不在两轮之间等s1完成。这个安排
创造重叠机会，不能保证扫描全部被掩盖。第一轮扫描尚无本次M3数据发送可供覆盖；后续
可覆盖窗口主要取决于上一server**仍在途的尾部发送**，不能使用整个server发送耗时估算。

各server扫描整份DIRECT序列，Scalar工作量约O(serverCount*D)每核。按表缓存覆盖T行、
每行12B粗估，全卡表请求量量级为`12*T*S*serverCount` B（不含对齐，缓存实际流量另测）。
T=10000、S=32、8个server时约30.72 MB，16个server时约61.44 MB。元数据搬运还会
竞争payload使用的MTE2资源；MTE3是当前主瓶颈，并不自动保证这些新增成本免费。

先采用每server一遍、扫完即发的简单版本。如果后续证据表明扫描造成明显的MTE3空隙，
再讨论缓存目标server索引或更细粒度筛选；不能为减少重复扫描恢复“先完成全server
筛选才发送”，也不在本轮引入新的双缓冲预取或核间调度协议。

### 后续验证边界

本次只核查任务分配数学模型，不执行kernel测试或设备实验。已重新验证本版逐server
扫描、在线owner判定和累计P推进：全部允许核数、1/2/3/4/8/16个server、排序/乱序及
空任务共7200组通过，确认唯一覆盖及两项均衡界限；未沿用上一版连续区间分配的结果。

后续实现覆盖专家/源rank长连续段、周期性分布、偏斜/空server、D<S、N[server]<S、取模
绕回和最大TokenNum；检查每个DIRECT原始row恰好发送一次、payload/三元组关联、yOut、
Flag及双窗保护。所有允许核数下检查每server及每核总量差≤1、各核非空server顺序一致。

通过延迟MTE3替身验证：上一server发送仍在途时允许开始下一server筛选，旧payload不被
元数据覆盖；第一server发包下发先于第二server筛选，未等待所有server任务表建立。
检查lane跨server连续轮转，初始化/回收只发生一次，空份额不引入等待；通过实际事件和
调用行为验证，不用源码文本匹配充当同步契约测试。

performance固定shape/路由/核数/机器做A/B；breakdown单独观察首包启动、筛选成本、
MTE3空隙与整体耗时。现有M3总区间不足以证明扫描已被传输覆盖；需要相应流水证据，
任何新增观察点不得增加同步，也不能把任务更均匀直接视为性能通过。

## 尝试优化点 6：M4 聚合任务按目标 server 均衡分配并轮转发送

以下为初版方案及讨论记录；当前筛选方式以文末“优化5/6修订：Vector筛选与密排计数”
为准，Table3按本核选中token单行读取及M4→M5共享输出契约继续保留。

### 状态与目标

2026-09-22，用户要求记录M4采用类似优化点5方案的可能性，统筹各核发出的聚合token，
按server+1、server+2直至本server轮转。**已按授权实现，设备功能与性能待验证。**
本项与M3优化点5分别记录，后续独立比较增量收益，不能同时修改后将组合收益归因给M4。

用户进一步明确：**当前M4场景同样已知是MTE3瓶颈**，以此作为本项优化的设计前提。
通过多个发送buffer保持MTE3在途，让筛选、成员表读取和聚合尽可能在传输期间完成，
预期覆盖大部分上述工作；本节未新增设备测量或给出具体覆盖比例。

当前`ReturnPartialTokens`对全部type2行按连续区间均分给R个Recv核，
`ServerDedupPartial::Process`沿本核行序执行读成员、加权聚合和partial发送，未按目标
server重分配。目标server取Table2的`sourceRank / 8`，即partial最终返回的源server；
不能用Table3成员expert所在server或执行聚合的当前卡替代。

只重新分配完整聚合任务及其执行顺序：一个type2原始row的self和全部成员仍由同一核
处理，维持成员顺序、FP32权重与累加、BF16/FP16舍入、目标token/topk和DataAsFlag。
本核M2 ready之后才能开始成员读取；M1/M2职责不变。

### 候选调度：逐 server 筛选，再立即聚合并发送

将优化点5的分配公式中的Send核数S替换为Recv核数R，核号采用`core-S`：

```text
P = 0                                   # 前面server的type2总任务数
for step = 1..serverCount:
    server = (ownServer + step) % serverCount
    分块读取Table1(tokenType)和Table2，筛出type2且目标server匹配的候选
    当前server第j个匹配任务的owner = (P+j) % R
    只保存owner等于本核Recv编号的原始row
    P += 当前server的总匹配数
    对本核本轮任务逐个：
        单独读取该原始row的Table3 topK聚合表
        读self/成员 → 聚合 → 打包并下发partial
    # 不等发送完成，继续下一server扫描
全部server完成后，按优化点4的原有读算交接进入M5
```

不先筛完所有server。每server核间任务数差≤1，全部server合计每核任务数差≤1；每个
type2原始row只归属一个核。P累计全体匹配数而非本核数，空server不改变P，空份额核
直接前进。轮转起点不能在每个server重置，否则余数任务集中在低核号。

这里均衡的是**partial包数量**：相同H下每包字节数相同，有利于均衡MTE3发送量。
以当前MTE3瓶颈为前提，优先均衡发送包数，不为了按成员数加权分配而提前读取全部
Table3。每个任务仍需读取和计算N+1个贡献，成员数差异作为重叠效果的观察指标，
不改变本轮以发送量均衡为主的分配目标。

### 两级元数据访问：先确定任务，再按 token 读取聚合表

筛选阶段只根据Table1判定type2、根据Table2确定目标server和路由，结合owner公式
确定本轮归属本核的原始row；**不读取未选中任务的Table3，不先建立全体topK成员表**。
当前server筛选结束后立即逐任务处理，不等待其他server筛选。

处理某个已选中任务时，才从`Table3 + uint64(row)*K*8`单独搬入其K个8B表项。
首项包含完整uint32成员数N和selfWeight，其余有效项提供成员expert/q/weight；保持
原表契约，仅消费self及有效N个成员。按单token读取整行K*8B，不恢复旧的多行跨度
Table2/Table3成批共同加载，也不为了只读有效项而先读N、再发起第二次成员表读取。

任务关联始终使用原始row，不能用server内匹配序号、本核任务序号或sourceTokenId
寻址Table3。Table2路由若已保留则直接使用；若受元数据容量限制只保存row，可按row
取回所需路由，但不能因此连带预读其他token的Table3。路由缓存的具体容量留待实现预算。

旧`CacheTables`共同baseRow/localRow的索引契约需要拆开：Table2服务筛选和路由查询，
Table3单独服务当前token，聚合表从本地第0项解析，不再继承旧多行缓存的localRow偏移。
当前token的成员索引和权重已被消费后才能复用该表buffer；其partial已打包到独立输出槽
后，MTE3只引用独立输出槽，因此下一token取表无需等待旧partial发送完成。

### 与优化点5相同的流水约束

- server轮次之间不新增核间或核内同步，不增加`PIPE_ALL`、全核屏障、server完成Flag
  或输出drain；各Recv核独立推进。M3和M4采用相同方向，不要求两组核处于相同server轮次。
- M4初始化一次，输入事件与输出Flag不按server重复初始化。输入inputSequence按已读
  贡献数连续推进，输出序号按已发partial数连续推进，逐槽等待和原有读/算/发依赖保留。
  不能简单地对每server重新Init/Process整个阶段对象，导致游标、seed或收尾重复。
- 末轮之后只做原有M4→M5定向读算交接，不排空M4尾部MTE3。共享输出地址、stride及
  完成事件继续由M5继承；`partialTasks_`必须是本核所有server的累计任务数，用于确定
  M5首个输出槽，不能误写成最后一个server的数量。空M4仍保留共享输出seed/收尾契约。
- 下一server扫描仅复用已消费的元数据区，不占用在途输出、输入或FP32/low工作区。
  扫描表DMA仍要满足原有数据就绪依赖，不能将“不加server同步”解释为无依赖覆盖。

### 主要实现差异：M4没有可长期保留的完整Gather列表

当前实现将本核少量行号转存到共享phase区，随后M4的FP32/phase/输入/输出布局覆盖
原M1/Gather scratch。优化点5的“保留完整packedUb、多轮扫描”不能直接照搬到M4。
不能只在新调度中继续引用旧packedUb地址；也不能为保留全type2列表而无预算地扩展UB。

优先评估**每server分块重读GM tokenType和Table2**：从原始row=0到TokenNum−1扫描，
只有type2行才解析路由并参与j计数，保存本核本轮原始row。扫描时不加载完整Table3成员
信息，避免所有核为被丢弃任务反复读取成员表；只有处理已选中token时才单独加载该行
Table3。单server本核列表容量不超过ceil(TokenNum/R)，可沿用现有rows容量。

可评估将扫描tokenType的小块临时放在当前未用于聚合的Table3缓存内，Table2使用原表
缓存，或在原多行Table3区域内重新划分扫描临时区和单token聚合表区；这只是候选复用
方式，实施前必须明确块大小、对齐、权重消费时点及DMA依赖。原Table3固定区域若可缩减，
先满足筛选元数据需要，再评估剩余空间，不预先承诺增加输出槽数。
不要增加额外常驻全量行表：默认优化点4的共享arena仅余480B，保留数百至上万行索引
可能减少发送槽，改变优化2/4的收益条件。方案目标是保持240KiB及既有读/算/发槽预算。

**缓存失效必须显式处理：** 每个server扫描都从小row重新开始；Table2扫描/路由缓存
按用途切换重置，不能让旧`row < cacheEnd`命中已被覆盖的内容。Table3按当前token行号
独立装载，不再与Table2共享cacheBegin/cacheEnd。这是软件缓存管理调整，不是新增
server流水屏障；仍需满足表DMA可读以及旧权重已被消费的生命周期要求。

### 收益判断与后续验证

在已知MTE3瓶颈下，保留多个在途发送槽，使下一任务的Table3读取、成员读取和聚合与
此前partial发送重叠；下一server的Table1/2筛选同样可与上一server尾部发送重叠。
重点是保持MTE3持续获得已聚合的packet，而不是为了进入筛选/取表阶段排空发送。

首轮筛选和首个token聚合仍属于启动成本。后续验证关注重复扫描是否造成MTE3空隙、
单tokenTable3读取的开销是否被覆盖，以及M5启动和整体耗时；这些是优化效果的测量项，
不再把当前场景是否为MTE3瓶颈列为待确认问题。

后续验证检查type2唯一覆盖、每server和总任务量均衡、目标server环形顺序、无新增
server等待、Table2/Table3独立寻址及在途输出保护；验证筛选不加载Table3、只有已选中
token才读取其K*8B聚合表，且不存在额外的先读N再读成员的取表流程。覆盖空任务、
N差异大、N[server]<R、稀疏行、非对称核数和取模绕回。保持逐token成员顺序和数值参考，验证最终partial、yOut及Flag。
检查M4结束时累计发送数、M5首槽继承及无M5任务路径。M3优化5、M4优化6应分别开销
归因；记录每核partial数、成员总数、M4/M5推进和整体性能，不以静态分配公式代替设备验证。

设计重点是将Table1/2筛选与单token Table3读取解耦，在不减少既有发送槽的前提下，
利用MTE3多buffer覆盖筛选和聚合工作。实现与Host/编译验证见下方记录，设备实验待执行。

### 优化5/6分批执行计划（2026-09-22，已授权实现）

1. 第一笔仅实现优化5：M3按server扫描、全局轮询归属、本轮筛选完立即发送；复用原
   元数据和payload槽，跨server保持事件/游标。更新M3覆盖与顺序/重叠Host测试，回归
   M1–M5和profiling；A5 OFF/ON分别构建，检查最终差异后按SilentEmotion提交推送。
2. 第二笔基于第一笔实现优化6：M4按Table1/2逐server筛选、单token读取Table3，保持
   数值顺序及M4→M5共享输出继承。更新M4/M5关联测试、配置边界与核划分覆盖，分别
   完成Host回归和A5 OFF/ON构建，再独立提交推送；不得把第二笔生产改动混入第一笔。
3. 重点检查旧缓存状态失效、元数据与在途DMA源不重叠、全server累计任务/发送游标、
   空任务和非对称分核。不得新增server同步或减少既有发送槽，接口和M2逻辑不变。
4. 主代理负责生产代码、文档、编译及Git；可独立委派对应测试文件或只读审查，禁止
   并行修改同一文件。保留用户已有四份其他文档改动；硬件功能和性能交由验证环境确认。

### 优化5初版实现记录（2026-09-22，已报告负优化）

第一笔基于`2800dac`，生产改动仅在M3：`SelectDirectServer`每轮扫描完整DIRECT行号，
按持续轮转owner筛选本核原始row/三元组；当前server发送下发后才开始下一轮扫描。
跨server沿用sendSequence与原有事件/槽，M4/M5和M2实现不变。三元组与行号缓存均复用
已有Scalar元数据，完整packed行号与在途result输出保持独立。完成主审、简化检查及独立审查。

Host针对性回归10/10通过；M3另外按Send=16/24/40/48/56分别编译运行通过（默认32
包含在回归中），覆盖长源端连续段、周期分布、10000行、空份额、双窗、精确payload/Flag、
唯一覆盖与两项均衡界限。延迟MTE3测试观测发包下发先于下一server扫描，扫描开始时
前一轮尾包仍在途，lane连续轮转且只seed一次。测试不证明硬件物理并发。

blue CANN9.1/A5 profiling OFF/ON构建`tilexr-ep`及kernels成功，exit=0；最终生产文件
与送编快照一致。编译期间仅Host M3测试继续完善，已单独记录快照差异并在最终Host测试
覆盖，未混用不同生产源码。证据在`build/combine-split-verification/opt5/`以及
`build/opt5-host-tests.log`、`build/opt5-m3-*.log`。初次Host大用例因4GiB cgroup OOM
退出，改用16rank覆盖最大行数、较小用例单独覆盖128rank，最终上述各配置均通过。

设备功能及收益待验证。维护要点：每轮都重新建立单调Table2扫描范围，owner和发送lane
游标则必须跨轮保留；元数据扫描不能清空发送事件，也不能覆盖完整DIRECT列表。

### 优化6初版实现记录（2026-09-22，筛选方式由下方修订替换）

第二笔基于已提交推送的优化5 `693a00d`，增量只调整M4调度及其M5交接计数。Recv核
每轮以32行小块扫描GM Table1/tokenType与Table2，按跨server持续轮转的owner选取
本核type2原始row；当前server筛选结束后立即逐token聚合发送，再扫描下一server。
移除旧Gather列表的分段分配、压缩和倒序搬移；不持有会被M4共享工作区覆盖的旧列表。

保持240KiB arena布局与读/算/发槽数量不变：rows仍为ceil(TokenNum/RecvCoreNum)
个int32，Table2沿用32行缓存；原Table3缓存的前128B在筛选阶段暂存32个type标签。
处理本核选中token时按原始row重读12B路由，再读取一次完整K*8B Table3，不预读其他
token的聚合表，也不拆成先读N再读成员。单行Table3从本地第0项解析，Scalar消费完
索引和权重后才复用该区域；在途MTE3只引用独立输出槽。

输入inputSequence、输出completed和owner跨server连续推进；事件和输出Flag仅初始化
一次，不新增server屏障或输出drain。Process返回本核全部server累计发送数，由M5继续
共享输出环。保留原M4→M5读算定向交接，不等待M4尾部MTE3全部完成。完成简化检查、
主审及独立生产/测试审查，未发现需扩大范围的改动。

验证证据：

- Host针对性回归10/10通过，包含M1/M2、优化5 M3、M4、M5默认/Send16/Send48及
  profiling；日志`build/opt6-host-tests.log`。
- M4默认及Send16/24/40/48/56独立编译运行均通过，覆盖全部合法核划分。独立参考先
  按server稳定排序再交错分核，验证精确任务顺序、唯一覆盖、每server与总量均衡、
  单行Table3关联、FP16/BF16数值、双窗、空任务、10000行及240KiB边界。延迟MTE3
  模型检查下一server筛选时前轮尾包仍在途、输入/输出槽不重置；日志
  `build/opt6-m4-default.log`、`build/opt6-m4-send*.log`。
- M5交接用例覆盖每核先发远端server 5包、再发本server 3包；验证累计8次的首槽
  继承，且M5读算/清Flag下发先于M4尾包退休，最终payload与yOut保持正确。
- blue CANN9.1/A5 profiling OFF/ON分别构建`tilexr-ep`及kernels通过，exit=0。
  送编快照与最终全部非文档输入一致；源码SHA、构建日志和产物SHA保存在
  `build/combine-split-verification/opt6/`。

Host M4路由用例为两server，更多server的真实通信、硬件物理重叠和性能收益待验证。
验证时分别比较`2800dac`→`693a00d`（优化5）和`693a00d`→本笔（优化6），使用相同
构建配置、输入与轮次；本笔不修改run.sh或验收入口，也不将Host替身结果视为设备验收。

## 优化5/6修订：Vector筛选与密排计数（2026-09-22）

用户设备测试确认优化5 `693a00d3162cd7befd7aeb1160703dff3ea239ab`为负优化，未提供
具体耗时差值。初版每核每server用Scalar遍历全部DIRECT，并重复读取Table2；576个
输入、约403个DIRECT、8个server、32个Send核时，每核每轮检查403个候选却通常只发
1–2个token。之前Host检查只证明轮次间允许在途发送，不能证明筛选成本已被掩盖。
上述初版Scalar扫描方案和实现记录保留用于实验归因；**当前实施方案以下述修订为准**。

用户已授权同时修正优化5/6并合并一笔提交。保持server+1轮转、本server最后，保持
跨server/分块的累计owner，不新增核间屏障、server完成Flag或MTE3排空。核内任务
仍按紧凑序列交错分配：`first=(localCore+coreCount-owner)%coreCount`，之后每隔
coreCount取一项；无份额时不逐项读列表，只按匹配总数更新owner继续下一轮。

### M3：一次准备常驻数组，逐server向量压缩

1. Vector比较tokenType==DIRECT产生bitmask；Table2按最多512原始行读取一次，
   使用Vector生成byte offset及Gather抽取sourceRank，替代Scalar逐行读表。
2. GatherMask分别生成DIRECT的紧凑sourceRank数组和原始row数组。复用已有三份
   row UB：typeUb保存rank、packedUb保存原始row，indexUb留给各server压缩输出。
   不增加常驻UB，不改变payload/output槽数。无DIRECT时直接返回。
3. 每server用两次Compares判断`sourceRank>=server*8 && sourceRank<(server+1)*8`，
   And合成server bitmask，GatherMask一次得到该server密排原始row与数量。比较rank
   范围等价于目标server比较，避免Scalar整除、逐候选判断和再次读Table2。
4. 按first/stride只读取本核命中行号，按原始row取12B路由，再沿原读/打包/发送流水
   处理。owner和sendSequence跨server延续；本轮发送下发后才筛选下一server。

候选筛选仍有O(serverCount*DIRECT)的**Vector元素工作量**，但没有对应的Scalar逐项
循环，也没有每server整表MTE2读取。Scalar热路径仅处理本核实际发送任务。元数据准备
仍有一次性的起步成本；本次不提前构建全部server子列表，不声称已有设备性能收益。

### M4：有界Vector块，不保留会挤占发送槽的全量数组

M4共享240KiB工作区不能额外常驻多份TokenNum数组。每server仍按原始row分块读取
Table1/tokenType和Table2，**块内全部筛选由Compares/And/GatherMask完成**；不逐候选
GetValue。仅将当前块紧凑列表中归属本核的行号追加到已有rows区，累计owner跨块延续，
因此分块不会改变server排序、核归属和总量均衡。当前server扫描完成后立即处理本核任务。

筛选阶段复用连续的Table2缓存、Table3缓存和low打包区，不占用rows、输入、FP32计算区
或在途输出槽。按可用字节选择32/64/128/256/512行块；H=7168时使用512行，576输入
只需两块。每块包含Table2 triples、临时rank/types、Vector索引及三份mask；小数组
按256B保留完整Vector读取余量。所有原M4/M5 offset、输入/输出lane数量和240KiB
上限均不变。

筛选不读取Table3 GM。选中本核token之后仍只读取该行完整K*8B聚合表，原成员顺序、
FP32乘加及最终舍入不变。M4总发送数仍由M5继承，M4尾部发送不排空。

### 事件和边界

- Compares、Gather与GatherMask之间保留Vector依赖；Scalar只在Gather结果/count
  可读之后取本核行号。sourceRank合法性用Vector范围mask/count校验，非本模块类型
  的Table2字段不按路由解释；负值和越界活跃rank仍Trap。
  新Compares长度按64个int32对齐，满足CANN调试检查；GatherMask只消费真实数量，
  UB填充不会成为有效row，也不扩大GM读取范围。
- M4扫描覆盖low前必须退役其Vector读者。使用独立`V_MTE2(7)`及`MTE2_V(7)`保护
  扫描区，避开已seed的payload输入0..2。这是同一UB地址的复用依赖，不等待最后
  partial的MTE3完成；MTE3仍只访问独立输出槽。该方向7与M5其他方向的7独立。
- 此方案增加了Vector筛选，与打包共享Vector流水，不能宣称硬件成本为零。是否比初版
  快以及是否优于优化前，必须在相同数据/核数/轮次下测量。

### 执行与验证计划

1. 主线程修改M3、M4及共享Vector mask helper，更新Host替身的Gather/int32 Muls。
2. 保留独立稳定排序/交错归属oracle，验证403 DIRECT/64P少命中、稀疏、空server、
   10000行、尾块、所有合法核划分、逐token Table3及跨server在途输出；确保没有
   在server间新增输入输出seed或重置游标。
3. 串行运行Combine回归与共享替身影响的Dispatch count/send/receive；独立只读审查
   API/alias/事件边界，按最终生产源码在blue执行CANN9.1 A5 OFF/ON构建。
4. 记录真实验证边界，保留用户其他四份设计/验收文档改动；以SilentEmotion合并一笔
   提交推送。设备单/多轮功能及性能留待用户验证环境执行。

### 修订验证记录

- 本机最终Combine针对性Host回归10/10通过。扩大到Dispatch及非对称核划分时，
  因本机4GiB cgroup内存紧张出现进程被杀；未将这些中断视为通过，转到blue以相同
  最终源码快照执行CPU替身测试，Combine与Dispatch count/send/receive共15/15通过。
- M3/M4默认32/32及Send16/24/40/48/56全部编译运行通过，涵盖最小Recv=8、403
  DIRECT/64P、128P、最大10000行、稀疏和尾块、server轮转、精确数值、双窗、Flag及
  在途输出保护。验证M3初始化后只读本核路由三元组，M4仍仅取本核token单行Table3。
- 独立审查核对CANN9.1/3510的Gather byte offsets与完整Vector尾读、Compares调试
  对齐、掩码范围、UB复用和事件方向。修正了初次实现的M4扫描事件与已seed输入槽
  冲突，以及空DIRECT不应读取Table2的问题；最终Host证据覆盖修正后的代码。
- 最终生产kernel已完成blue CANN9.1/A5 profiling OFF/ON编译。新增helper已加入
  Combine kernel的CMake DEPENDS，后续仅修改helper也会触发重建。
- 源码及构建证据在`build/combine-split-verification/vector-select-delivery/`，包括
  `blue-evidence/`、`blue-host-evidence/`、原始快照及补充CMake依赖的最终源码清单。
  本机日志为`build/vector-select-combine-tests.log`，全部核划分结果为
  `build/vector-select-blue-host.log`。

本次没有新增设备测试或性能数字。用户可对照`4861815`验证本次筛选修正的增量，并与
优化5之前的`2800dac`比较最终整体收益；后者同时缺少优化6和后续M2细分打点，不能
将跨版本总差值全部归因于单独一个模块。

## 尝试优化点 7：M5 放宽 Flag 预读依赖，一次搬入全 TopK Flag

### 状态与目标

2026-09-22：最初按用户要求记录方案，随后获授权开始实现。以优化5/6 Vector筛选
修订版本`7d737b6`为基线；本节方案已落实到kernel，开发验证记录见本节末尾，
设备功能和性能收益仍待验证。

目标是减少每次probe的Scalar循环及MTE2搬运指令下发，并允许下一token的Flag读取
与上一token的Vector聚合/最终Cast重叠。仅改变M5检测路径，payload仍按SourceMask
精确读取，权重、FP32聚合顺序、输出槽复用、pending调度及M4→M5交接保持原逻辑。
本节记录本次性能实验对主设计CB05-02按mask密排Flag方案的修订；本次kernel改为
全TopK检测，实验正确性前提和验证边界以本节为准。

### 1. 去掉probe入口的过强同步

基线`EpServerDedupSourceAggregate::Probe()`入口执行`Sync<V_MTE2>()`，会让Flag
读取等待此前Vector队列中的聚合和最终Cast，甚至间接受输出槽`MTE3_V`等待影响。
本次删除这一入口同步，保留搬入后的`MTE2_V`和ReduceSum后的`V_S`。

正确性依据：检测的flags/work/probeSum与payload输入、计算和输出区域不重叠；
上一probe返回前已有`V_S`，保证Vector不再读取检测buffer；首次probe之前的
M4→M5阶段scratch复用已有MTE2/Vector到Scalar的交接；ClearFlags仅在最后一个
就绪token之后复用flags区，此后不再probe。删除入口同步不应删除这些真实生命周期
依赖，也不增加PIPE_ALL、核间同步或输出排空。

效果边界：下一token的Flag MTE2可提前，但其ReduceSum仍与上一token聚合共用
Vector队列，不会并行执行；输出槽复用导致的Vector阻塞也不会因此消失。当前token
的yOut MTE3本来就可与后续检测重叠，本项不是首次开启该重叠。

### 2. 全TopK Flag一次跨步读取，按mask的置位数量判断

定义`N=ceilDiv(H*2,480)`，每个副本有N个512B包块，每块前480B为payload，后32B
为Flag。一个源token的K个副本槽连续，因此它的K*N个Flag区形成统一512B步长。
从该token的第0槽首个Flag开始，一次带步长DataCopy读取全部K槽的Flag：

```cpp
// 方案示意；结果GM按float寻址，packetBytes = N * 512。
const uint32_t totalFlags = K * N;
const uint32_t requiredCopies =
    static_cast<uint32_t>(AscendC::GetBitCount<1>(static_cast<uint64_t>(mask)));
const uint32_t expectedFlags = requiredCopies * N;
auto src = resultFlags[(uint64_t(token) * K) * packetBytes / 4U + 120U];
DataCopy(flagsUb, src, {uint16_t(totalFlags), 1U, 15U, 0U});
Sync<HardEvent::MTE2_V>();
ReduceSum(probeSum, flagsUb, work, 1U, totalFlags, 1U);
Sync<HardEvent::V_S>();
const bool ready = probeSum.GetValue(0U) == float(expectedFlags);
```

这里归约范围是`K*N`，期望值是`popcount(mask)*N`，两者不能混淆。每个32B Flag区
只取首个FP32求和，其余7个float不参与判定；不按k拆成多次搬运或多次归约。
“一次搬运”指一次DataCopy API提交，设备仍访问K*N个分散的32B区域，并非一个
连续GM事务。实现时核对合法K/H下blockCount和ReduceSum参数上限，不能窄化截断。

检测通过之后，Aggregate/Load仍只遍历mask置位的k，读取对应payload并乘对应
weight；不读取无效槽payload，不使用weight是否为零代替mask，也不更改求和顺序。
保留mask非零且无K范围外置位的输入检查；实际mask当前为uint32_t，零扩展为uint64_t
即可使用位计数接口，不扩大接口位宽。

### 3. 位计数接口与UB预算

已直接核对blue目标CANN9.1.0安装树中的：

- `ascendc/include/basic_api/interface/kernel_operator_scalar_intf.h`：
  `template<int countValue> __aicore__ inline int64_t GetBitCount(uint64_t valueIn)`；
  同文件注明旧`ScalarGetCountOfValue`应改用GetBitCount。
- `ascendc/include/basic_api/impl/kernel_scalar.h`：`GetBitCountImpl<1>`调用`bcnt1`，
  `<0>`调用`bcnt0`。本方案使用`GetBitCount<1>`，不使用查找首置位位置的接口。

本次是头文件核实，不代表修改后的A5 kernel编译或性能验证已通过。实现优先在每次
probe根据当前UB mask用Scalar位计数得到expected，无需新增每token常驻计数数组。

当前`EpCombineM5FlagBytes/WorkBytes`已按完整K*N预留，并包含Vector完整读取所需
尾部余量，故全K读取无需增加Flag/work容量，不挤占M4/M5共享payload输出槽，继续
遵守240KiB预算。实现时仍需覆盖所有合法核划分和最大K/H布局。

### 4. 全量求和成立的必要前提

- 每个有效槽的N个Flag首值只能由本轮0单调变为1，并保留既有DataAsFlag发布契约。
- mask无效槽在整轮内必须为0，M3/M4不能向这些槽发布；有效槽必须与mask一致。
- 首次调用的结果区完成初始化清零；后续轮次继续执行已有全K槽Flag清理及窗口复用
  完成约束，不只清理mask有效槽，不在每轮额外添加Host清零或阶段同步。

在这些前提下，全部K*N个首值之和达到expected，当且仅当mask要求的所有分块到齐。
不能把这个方案扩展成任意脏窗口也正确：例如无效槽残留1，可能抵消一个尚未到达的
有效分块，导致假就绪；把等号改成大于等于并不能解决。该优化会让无效槽零值成为
检测正确性的直接前提，必须在验证中显式覆盖。

沿用现有Flag遗留边界：未重新初始化的通信域改变BS/K/H、数据角色或wire布局，
不属于当前已确认复用范围。本项不关闭或扩张该遗留问题。

### 5. 成本与验证计划

设有效副本数m=popcount(mask)：旧probe为m次DataCopy、搬入32*m*N字节并归约
m*N个首值；新probe为1次DataCopy、搬入32*K*N字节并归约K*N个首值。H=7168、
K=6时N=30，读取固定5760B；若m=2，旧路径为2次/1920B，新路径1次/5760B；
若m=6，读取量不变，调用数由6降至1；若m=1，调用数不变但读取量增加6倍。
故减少指令下发不等同于必然降低总耗时，尤其要单独观察稀疏mask。

后续实现验收应覆盖：

1. 稀疏/全K/最高有效位、零权重但mask有效、乱序和分块部分到达；缺任一有效分块
   时不得提前ready，未就绪仍继续扫描pending，yOut与原有oracle一致。
2. 输入契约验证无效槽初始为零且没有发布者，全K清理后仍为零；反例说明脏无效槽
   可能产生假就绪，不把违反前提的场景标记为受支持。
3. 单轮、连续多轮、双窗口及合法mask变化，确认上一轮Flag不会干扰当前轮检测；
   同时覆盖单token核、空任务核和最后一次probe转ClearFlags的buffer生命周期。
4. Host替身核对全K跨步搬运范围、首float归约、payload仍仅按mask读取及精确数值；
   Host同步替身不能证明真实MTE2/Vector/MTE3重叠，需目标编译及设备trace补充。
5. CANN9.1/A5 profiling OFF/ON构建；使用现有run.sh的combine/both功能、多轮性能
   和breakdown流程，与同配置基线比较M5及端到端时间。实验分析分别观察同步删除、
   全K读取和二者组合，区分指令下发收益与额外Flag读取/规约成本，不预先宣称收益。

### 实现及开发验证记录

本次实现只调整M5 Probe：删除入口V_MTE2，GetBitCount<1>计算有效副本数，一次跨步
读取全K*N Flag并做一次首float ReduceSum；payload和清理路径未改变。实现使用
原有完整K预算，不新增buffer或同步。既有配置对全K清理的4095 blockCount上界
也覆盖本次全K读取，合法参数不会在uint16_t转换时截断。

- 本机最终M5默认32/32、Send16/Recv48、Send48/Recv16及profiling替身共4/4通过。
  覆盖K1/6/32、BF16/FP16、最高位mask、超过255个Flag、部分分块未到齐、pending
  乱序完成、mask有效但weight为零、payload精确读取、M4尾部和M5交接。
- 补充同一窗口第三轮mask变化：前两轮分别使用双窗，第三轮复用窗口0，仅发布本轮
  active槽；inactive槽不由夹具重新清零，先检查上一轮生产清理留下的完整32B零值。
  包含旧active变inactive及旧inactive变active；全K检测不能依赖每轮测试重置窗口。
- 旧M5夹具曾向inactive槽的Flag首值写7，现按新方案的零值前提改为0，其他7个float
  仍保留毒值用于验证只归约首float。profiling夹具也显式建立合法的零值结果窗口。
- 独立只读审查未发现可达实现缺陷；已补齐审查指出的同窗mask变化验证缺口。主线程
  完成差异和简化检查，未引入新的抽象或改变其他模块行为。
- 同一最终源码在blue的Combine及共享替身Dispatch回归共15/15通过，覆盖M1–M5、
  profiling、count、source send和receive packet。
- blue真实CANN9.1/A5的Combine及Dispatch kernel均完成profiling OFF/ON编译。
  最终933个非文档源码/构建文件与远端快照摘要一致。证据、编译器版本、配置、产物
  哈希与日志保存在`build/opt7-verification/`；本机记录为`build/opt7-host-tests.log`。

这些检查不证明设备流水重叠或性能收益；用户A5设备上的单/多轮功能、performance和
breakdown仍待验证。未将无效槽残留非零的通信窗口纳入支持范围。

## 尝试优化点 8：M5 最多双输出，剩余UB扩展输入并按槽回收M4

### 已确认方案与执行计划

2026-09-22：用户确认记录并实现本项，以优化7 `406e474`为基线。M4保持最多3读及
原输出布局，M5改为最多2个输出槽，剩余可用UB尽量分配完整payload输入buffer。
仍使用240KiB、单套FP32计算，保留优化7的全TopK Flag检测；不增加跨token预取、
跨核同步或M4→M5 PIPE_ALL，不改变数据路由、权重及数值顺序。

1. 扩充Host/kernel共用布局：保留M4偏移及槽数，记录M5元数据实际末端；在M4输出
   槽中保留最后下发的至多两个槽给M5输出。空/不足两个M4任务时补充未使用槽。
2. 在M5元数据末端到240KiB预算末端内，避开保留的输出槽，按地址遍历可用连续段，
   分配尽可能多的完整输入buffer，上限7（MTE2_V事件7留给probe）。每个输入记录
   与M4输出区域重叠的槽位bitmask。不能只减少finalOutputLanes而继续占用全部旧槽。
3. M5保留原物理输出槽的地址及MTE3_V事件，不对在途M4输出做整池重排；用完一槽
   等待其MTE3_V后Cast/下发。输入只在首次覆盖M4输出时回收对应事件，随后用
   Vector→MTE2事件桥接：MTE3_V等待→V_MTE2发布/等待→覆盖。单独MTE3_V不足以
   保护MTE2写入。此桥接只发生于首次回收，不增加M4逐发送事件开销。
4. 记录已回收M4槽mask，最终DrainSharedOutputs只等待未消费的物理槽；包含M5输出
   及从未使用的旧槽。空M5仍由调用层回收全部M4槽，不能遗留或重复Wait。
5. 更新布局/异步Host测试并独立审查：重点覆盖7读/2写、不同部分发送计数和环绕、
   一个输入跨两个旧输出槽、同一旧槽覆盖多个输入、最大/最小核划分、空M4/M5、
   最后Flag清理与在途M4源保护。针对性Host回归后执行blue CANN9.1/A5 OFF/ON
   编译，记录实际结果；以SilentEmotion单笔提交并推送。

M4读/算在既有阶段交接时已经结束，可立即复用其元数据、计算、输入区。M4尾部MTE3
仍可能读取输出，只有即将被覆盖的旧槽必须先完成。按槽等待可能让MTE3队列较早的
任务也完成，但不主动等待保留的两个尾槽；不保证硬件在此期间恰好只完成一个任务。

默认TokenNum=576、BS=96、H=7168、K=6、32/32核时，M4保持3读/1套计算/6发；
M5可容纳7读/1套计算/2写。输入每槽14400B，保留输出每槽15360B；M5实际写出H*2B。
K=6时单token最多预读6个副本，7是buffer容量而非7路物理DMA，也不是7个并行token。
输出槽位置随本核M4发送数决定，碎片/事件上限会影响其他形状的实际输入数；不超过
7槽后剩余UB不再为无对应事件的buffer强行分配。不同形状以共用布局函数结果为准。

风险与验收边界：更多预读可能抢占MTE2，双输出也可能增加尾部搬出等待；本项是实验，
不预先断言性能收益。必须检查M4在途packet未被M5覆盖、所有输入/输出事件仅回收一次、
240KiB边界与yOut精确值。Host模型不能证明真实设备重叠，最终仍需用户环境的单/多轮
功能、performance和breakdown对照优化7基线。

### 实现记录

共用配置新增M5布局计算：依据本核M4累计发送数选择保留的物理输出槽，以M5元数据
实际末端作为输入分配起点，在其余连续空段内按完整输入stride分配，最多7槽。保留
M4全部偏移/3读/原发送槽数量，M5输出数量为min(M4输出槽数,2)。每个输入记录所有
重叠旧输出槽，支持一个输入跨两个旧槽、多个输入与同一个旧槽相交。

M5首次加载该输入时才消费尚未回收的MTE3_V事件，并通过V_MTE2桥接到读取DMA；
后续同槽覆盖由MTE2队列顺序及输入lane复用事件保护，不重复消费M4事件。保留输出
直接沿用原物理槽及事件ID，包含slot7；其V_MTE3/MTE3_V与probe的MTE2_V(7)、
clear的S_MTE3(7)方向不同。Process返回已回收槽mask，调用层最终等待其余输出。
本次没有修改M4每token发送指令或添加M4逐槽新事件，也没有M4/M5间PIPE_ALL。

验证中特别保留的经验：MTE3_V只约束Vector，改作MTE2输入必须补齐方向桥接；
仅修改buffer数量而不维护物理偏移和事件所有权，会提前覆盖M4在途packet或导致
末尾重复等待。输入分配必须避开保留输出的完整wire stride，不能仅按M5写出的H*2B
把同一物理槽尾部挪作输入，因为旧M4仍会读取这些尾部字节。

### 开发验证记录

- 独立只读审查未发现生产实现缺陷，另以独立程序枚举5,970,343个合法布局，确认
  对齐、240KiB边界、输入互斥、保留输出保护及重叠mask精确。
- 维护的配置测试覆盖所有合法Send核数、不同K/H/BS/TokenNum及发送环绕，使用
  空闲区间容量oracle验证最多7个完整输入。默认形状的M4仍3/6，M5为7/2。
- 异步Host用例保留M4旧packet到事件回收时才读取源UB，校验原始payload未遭覆盖；
  覆盖6/7/11次M4发送、物理槽环绕、K6/K32输入复用、两种dtype和最后Flag清理。
  读取输入时检查方向桥接已发生，保留的两个尾发送未被主动排空。
- 扩展K32后，本机默认、Send48及profiling/布局通过，Send16变体被系统终止；当时
  本机4GiB cgroup接近上限且存在OOM记录，未将该中断当作通过。同一源码移交blue
  完成最终Host检查，不修改或终止其他会话进程。
- blue已完成最终生产源码的CANN9.1/A5 profiling OFF/ON完整tilexr-ep构建，包含
  Host、Combine和Dispatch kernel；额外Send24/40/56的M5编译及执行通过。
- blue最终Combine/Dispatch Host回归15/15通过，包括本机中断的Send16/K32用例；
  加上单独的Send24/40/56运行，M5覆盖全部六种合法分核。完整构建/测试返回码为0，
  最终1112个非文档源码/构建输入（含spdlog子模块）摘要匹配。
- 最终快照及证据保留于`build/opt8-verification/`。首次快照后只调整测试夹具的BS
  以容纳Recv=8下的远端目标token；生产代码未变。最终源码清单单独保留，不将
  测试夹具的越界数据作为合法算子输入。设备功能和性能仍待用户验证。

## 尝试优化点 9：M3/M4 按半区优先轮转选择目标server

### 状态与范围

2026-09-22：按用户确认的奇数/偶数server通用半区顺序完成代码接入。
2026-09-30：128P 及以上的 server dedup 改为半区交替，见下文补充；本节原顺序仍用于较小规模及非去重 M3。
本项基于优化8 `579c5cb`；设备功能与性能结果待用户环境验证。
仅替换M3 DIRECT和M4 partial遍历目标server的顺序，沿用现有逐server Vector筛选、
累计均衡分配、payload流水和所有现有资源生命周期依赖；不新增同步代价。
半区按逻辑server编号（rank/8）划分，不修改rank映射或数据目的地址。

### 奇偶通用公式

令server总数N>0，当前server为s，0<=s<N：

```text
H = floor(N / 2)
base[0] = 0       length[0] = H
base[1] = H       length[1] = N - H
g = (s < H) ? 0 : 1
r = s - base[g]

依次选择目标半区 q = g、1-g：
    跳过 length[q] == 0 的空半区
    j = 1 ... length[q]
    dst = base[q] + ((r + j) % length[q])
```

第一半区是[0,H)，第二半区是[H,N)，分界H属于第二半区。N为奇数时，前半区比
后半区少一个server；取模使用目标半区自己的长度，不能对两个半区一律模H。
N=1直接返回s0，避免对空半区取模；N=0非法。公式支持单server不代表本项扩大
算子Host准入：当前Combine要求至少16 ranks，仍沿用该限制。

| N | 当前s | 本半区轮转 | 另一半区轮转 |
| --- | --- | --- | --- |
| 8 | 2 | 3、0、1、2 | 7、4、5、6 |
| 8 | 6 | 7、4、5、6 | 3、0、1、2 |
| 7 | 2 | 0、1、2 | 6、3、4、5 |
| 7 | 6 | 3、4、5、6 | 1、2、0 |
| 2 | 0 | 0 | 1 |
| 1 | 0 | 0 | 空 |

连续L个j对L取模形成一个排列，因此每个目标半区内无重复、无遗漏；两个半区互不
重叠且覆盖[0,N)，整体恰好访问N个server。本半区长度与r的取值范围相匹配，故自身
server在本半区最后处理。另一半区不要求存在与r一一对应的位置，长度不同时自然
按目标长度绕回；偶数N时退化为此前确认的等长半区公式。

### 接入方式和同步边界

已增加M3/M4共用的纯Scalar函数`ServerDedupHalfOrder`，继续使用一次全局server遍历序号。
给定全局序号t=0...N-1，若t<length[g]，取q=g、j=t+1；否则取q=1-g、
j=t-length[g]+1，再代入上述dst公式。无需先生成完整任务表或等待全部server筛选。

- M3仅替换`SelectDirectServer((ownServer+step)%servers)`的目标编号计算。
  owner、sendSequence及已seed的输入/输出事件在两个半区间连续，不清零、不重建。
- M4仅替换`SelectServer`的目标编号计算。owner、completed、inputSequence和输出
  环形游标跨半区连续；rank合法性校验只在全局第一轮执行，不能在第二半区重复
  触发当前`step==1`对应的校验。
- “完成本半区”指本核完成该半区任务筛选及发送指令下发，不是等待其MTE3完成，
  也不要求其他核同时到达半区边界。空server仍可扫描，计数为零后立即进入下一轮。
- 半区/server切换不新增PIPE_ALL、SyncAll、Host barrier、Flag握手、Set/Wait事件
  或MTE3排空。原有表缓存复用、Vector依赖、payload槽复用等待及模块末尾回收保留。
- 各server内稳定行序、M4单token的Table3读取与聚合顺序保持原规则；目标GM地址
  仍来自原Table2路由。M5和优化8的共享UB/事件交接按原逻辑运行。

累计owner不重置意味着相邻server的任务继续接续分配，所有核总任务数仍相差至多1。
具体token对应的core可能随server遍历顺序改变，这是同一累计分配规则的自然结果，
不另加重分配阶段，也不承诺每个token固定由原core处理。

### 可行性结论和验证边界

已审视当前M3 `ReturnDirectTokens`及M4 `ServerDedupPartial::Process/SelectServer`：
两者的筛选入口都接受独立目标server，读算发循环与server编号计算分离，均衡计数及
buffer生命周期可以跨半区直接延续。该方案可通过替换遍历映射实现，无需新增同步。
Scalar分支/取模按server轮次计算，不放进逐token热循环；不宣称计算开销绝对为零。
发送对象和任务顺序变化可能影响已有等待的实际耗时，“无新增同步”不等于现有等待
时长或性能结果必然不变，收益需与优化8基线实测比较。

本次公式检查枚举N=1...128的全部当前server，共8256种情况，验证整体排列、本半区
优先、自身在本半区最后、偶数公式等价，并以含空server的任务数量验证7种核数下
跨半区累计owner的均衡性质。证据位于`build/opt9-plan-verification/summary.json`；
这是公式/分配模型验证，不是生产实现、CANN构建或设备运行通过。

实现验证重点：奇偶server（至少N=2/3/7/8/16）的精确顺序、空server和稀疏任务、
跨半区buffer游标连续及在途发送源保护、M3/M4实际发送覆盖和yOut精确值；Host独立
参考及顺序断言已按新规则更新。设备侧保留相同的run.sh功能、多轮性能和breakdown流程，

比较M3/M4、M5及整体耗时，不把局部通信顺序收益预先等同于端到端收益。

### 实现与回归记录

生产变更仅为`tilexr_ep_server_dedup_select.h`中的半区映射及M3/M4两处调用替换。
保留原全局step，M4合法性检查仍只在step==1执行；无新UB、事件或同步操作。
N=1时唯一合法ordinal在自身非空半区分支返回，不对另一个空半区取模。

Host独立参考通过构造两个递增server列表后rotate得到顺序，不调用生产映射函数。
M3枚举N=1...128的8256组当前server，另执行奇偶拓扑下的真实筛选、DIRECT数据
打包和发送；M4扩展既有数值夹具到N=3/7/8/16的两半区，保留逐成员FP32累加、
输出位值、窗口边界、唯一发送、累计owner均衡和Table3按需读取检查。
延迟MTE3模型在每次server切换时检查上一轮输出仍在途，并检查输入/输出游标连续，
覆盖半区切换；M4还检查每块只多出两次全局首次合法性compaction。
M5继续验证优化8的共享输出槽交接及最终yOut，按新两server顺序更新夹具说明。

维护注意：发送顺序变化也会改变两server场景（变为本server先发），独立参考和
错误说明不能继续假定“本server总在最后”；M4→M5必须继承全模块累计completed，
不能使用最后server或最后半区的任务数推导在途槽。

验证结果（2026-09-22）：

- blue上CANN9.1.0、ccec clang15.0.5，`dav-c310-vec`/`CATLASS_ARCH=3510`：
  Combine及Dispatch kernel的profiling OFF/ON分别构建通过，退出码0。
- 15项Host回归全部通过（含M1/M2、M3、M4、M5及Dispatch回归）。M3/M4分别覆盖
  Send核数16/24/32/40/48/56，全部通过；默认32纳入Host回归，其余为C++14独立构建运行。
- M5测试错误说明更新后重新构建并执行默认、Send16、Send48三项，3/3通过。
- 本地M3/M4/M5构建通过，但执行进程被终止；本地4GiB cgroup接近上限且有OOM
  kill记录，不将本地执行算为通过，采用blue上相同源码的完整运行结果。
- 源码基线、逐文件SHA256、最终源码一致性检查、编译器版本、OFF/ON构建日志、
  测试日志和退出状态保留于`build/opt9-verification/`。独立只读审查未发现阻断问题。

以上为交叉编译和Host替身执行证据，尚未运行本版本的A5多卡功能/性能测试。
后续按既有流程与优化8 `579c5cb`做相同输入、轮次和profiling配置的对照，重点观察
M3/M4及总耗时；半区优先的性能收益仍待实测，不作为已达成结论。

### 128P 及以上：交替访问两个半区

2026-09-30：在 `codex/SilentEmotion` 上，仅对启用 server dedup 的 Combine，
当 `rankSize / 8 >= 16` 时将 M3 DIRECT 和 M4 partial 的目标遍历改为
“本半区一项、另一半区一项”交替。每个半区仍从自身相对位置的下一项开始轮转。
128P 的 server 0 顺序为 `1,9,2,10,3,11,4,12,5,13,6,14,7,15,0,8`；
server 8 顺序为 `9,1,10,2,11,3,12,4,13,5,14,6,15,7,8,0`。
server 数为奇数时，先交替访问等长部分，再处理较大半区剩余的一项。

较小规模和非去重 M3 继续使用本半区优先顺序。M3 需显式传入运行时
`enableDedup_`：它与 M4 共用排序函数，但自动模式的非去重分支也调用 M3，
仅判断 server 数会意外改变该分支。Dispatch 不使用此函数，本次没有修改。

改动仅映射目标遍历序号，不重置跨 server 的 owner、sendSequence、completed
或 lane 游标，不增加同步、排空或控制区存储。空目标仍正常跳过；各目标内保持
原任务顺序、数据地址及数值聚合顺序。

验证以 `483a52a` 加本次修改为源码基线，证据保存在
`build/combine-half-interleave/`。C++14 Host 回归 5/5 通过，覆盖 M3/M4/M5、
自动模式及 profiling kernel。排序测试枚举 1 至 128 个 server 的所有源位置及
去重开关，与独立的列表旋转/交织参考比较；实际 M3 发送覆盖 120P、128P
两半区和 128P 非去重，M4 128P 两半区验证实际输出与在途缓冲保护。
blue CANN 9.1.0 / ccec Clang 15.0.5 下，A5 `dav-c310-vec` / `CATLASS_ARCH=3510`
的 Combine kernel 在 auto 策略、profiling OFF/ON 两种构建中均通过，退出码 0。
送编快照包含 1179 个文件并逐项校验 SHA256；最终非文档源码与该快照一致。
这些结果不证明 NPU 并发可见性或性能提升；A5 多卡功能及性能仍需实测。
