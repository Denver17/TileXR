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
