# EP 验证与人工测试

## 日常构建、同步和运行（不使用 checkpoint）

普通入口已弃用 `TILEXR_VALIDATION_CHECKPOINT`；旧 shell 导出、旧部署配置及旧验收 manifest
均不参与算子或版本选择。默认执行当前源码的完整 Dispatch / Combine M1–M5。
普通构建固定覆盖 `install/server-dedup`，不保留 ON/OFF 两份包，
与 `run_commit_validation.sh` 的阶段验收包隔离。兼容结果 JSON 仍保留 `checkpoint: null`，
它不是可设置的运行参数。需要重验某笔历史提交时，切换到该提交运行其 `run_commit_validation.sh`。

```bash
bash set_env.sh && bash build_all.sh && bash sync.sh
bash run.sh -m 1 -n 8 -e 384 -l 100 50 bs=96 operator_mode=combine

bash build_all.sh breakdown=1 && bash sync.sh
bash run.sh -m 2 -n 8 -e 384 -l 100 50 bs=96 operator_mode=both
```

`-m 0/1` 要求当前包关闭 profiling，`-m 2` 要求开启 profiling；不匹配会在启动前拒绝，须重新编译和同步。`operator_mode` 支持
`dispatch/combine/both`，`-l 正式轮数 预热轮数` 控制批次。Combine 仅非量化；Dispatch
量化开关独立。both 的两个算子分别记录耗时并生成各自 trace。`case_analysis=1` 可用于
包含 Dispatch 的运行，在全部设备调用结束后分析路由去重率。

`sync.sh` 和独立 `run_host_validation.sh` 使用同一个固定目录的安装包，无需设置 breakdown。
构建开始时失效旧 manifest/同步回执，构建失败后不能把旧包当作本次成功产物。
同步使用完整 hostfile，运行使用前 `-n` 台；新增服务器后须再次同步。
`dry_run=1` 使用当前包中的 Host consumer，检查参数/rank 映射，不加载 CANN、不 SSH、
不分配设备或创建 communicator；无需另建 C3 包。仅做本机配置检查也可先
`TILEXR_VALIDATION_BUILD_MODE=host bash build_all.sh`，此 Host 包不能用于真实设备运行。

维护教训：只修改 shell 的默认 checkpoint 不能解除 manifest、consumer、结果校验之间的阶段耦合，
会使独立 shell 构建/同步/运行选到不同包。普通路径必须贯穿无阶段身份，并保留完整业务/Flag/trace
校验；提交验收阶段只由专用入口内部选择。另一个 shell 无法继承 `build_all.sh breakdown=1` 的
子进程变量，同步应始终分发固定目录中的 manifest 和对应产物。回归覆盖旧变量干扰、独立 shell、打点开关切换、远端参数、
三算子模式、多轮调用和 dry-run；Host 替身及编译通过不能替代真实多机 A5 验证。

开发侧验证（2026-09-21）：blue CANN 9.1 / A5 实际构建依次覆盖同一个
`install/server-dedup`，profiling OFF/ON 均通过，安装包 Host 回归各 10/10；
三种算子 dry-run 各自通过 16/16 rank 配置检查。独立纯 Host 构建及回归也通过 10/10，
可直接使用该包做 dry-run。新增 consumer 替身回归覆盖无 checkpoint 的多轮 combine/both
及 functional/performance/breakdown，仍检查最终 yOut/Flag，不是只检查返回码。
完整 Python 工作流回归 374 项中 373 通过、1 项 Windows 专属检查跳过；Shell 语法检查通过。
证据保留在开发工作区 `build/normal-workflow-verification/`。上述未启动真实 NPU 集群，
不外推为本次入口修改已完成设备性能或跨机通信验收。

### run.sh 墙钟耗时与 trace 路径

日常命令不变，每次运行自动追加三类耗时（单位为秒，不是设备 kernel 延迟）：

- `RUN_WALL(s,1s-resolution)`：shell 总耗时，以及参数准备 `setup`、可选 Host 回归
  `host_tests`、环境加载 `env` 和 Python 工作流 `workflow`。以整秒统计。
- `WORKFLOW_WALL(s)`：本机包检查 `local_check`、所选节点检查 `remote_check`、
  节点运行 `nodes`、结果收集 `collect`、汇总/导出 `summary`，使用单调时钟。
- `RANK_WALL_MAX(s,observed=N)`：已取得记录的 rank 中，每阶段最大耗时及其 rank。
  `process` 包含 consumer/msprof 的初始化、预热、正式调用、校验和退出；
  `profile_wait` 是等待本机 trace 解析锁，`profile_decode` 是解析/导出，`validate` 是 Python 结果校验。
  各 rank、各节点相互并行，最大值不可相加作为总耗时，节点运行也包含 trace 解析。

shell 统计保存在 case 根目录的 `launcher_timing.json`；工作流统计保存在
本轮 `summary.json` 的 `wall_time`，逐节点/逐 rank 统计位于
`nodes[].reported_result.wall_time` / `rank_wall_times`。正常失败退出仍保留已经历阶段的耗时；
启动前检查失败写入 `failure.json`。强制杀死进程时可能没有完整统计。
Python 统计截止最终 `summary.json` 写出前；最终结果文件写入及终端渲染包含在 shell 的 `workflow` 中。
`dry_run=1` 仅有 shell 统计。终端原有算子微秒级性能数据的统计口径不变。

breakdown 每个启用算子的 trace 目录单独一行，例如（路径为格式示例）：

```text
TRACE dispatch | trace=/.../traces/dispatch/chrome | status=complete | fastest_rank=0 | slowest_rank=8
TRACE combine | trace=/.../traces/combine/chrome | status=complete | fastest_rank=3 | slowest_rank=9
```

目录下每个 rank 一份 JSON。解析不完整时仍显示已有目录并标记 `status=incomplete`，
不能据此认为所有 trace 已生成；没有 trace 汇总时不虚构路径。
维护教训：Combine 接入后结果从 `traces` 扩展为 `operator_traces`，终端摘要漏读新字段会使
路径仅留在 `workflow.log`。渲染同时兼容旧 Dispatch 字段和新字段，并按当前算子模式显示，避免重复。

运行阶段目前复用汇总所需的同一批已校验 rank 记录，减少重复校验；不跨运行缓存结果。
breakdown 保留每台机器一次解析一个 rank 的锁以控制内存，用上述计时判断它是否为当前瓶颈。
首版仅修改脚本，无需重新编译算子；更新后同步脚本即可。Host 替身测试不能证明实际 64P 提速，
需回传上述计时后再决定下一步优化。

开发侧验证（2026-09-21）：Python 工作流回归 379 项中 378 通过、1 项 Windows 专属检查跳过，
`bash -n run.sh` 通过。覆盖计时分段累计、异常退出保留耗时、三算子模式独立 trace 行、
旧 Dispatch 字段兼容及不完整 trace；含替身多节点采集/汇总，未执行真实 NPU。
证据保留在开发工作区 `build/run-wall-timing-verification/validation.log`。

后续 consumer 计时与 Host 优化（2026-09-21）：用户回传 performance 模式
workflow 121.540s，其中 nodes 112.892s、最慢 rank process 111.151s；
profile_decode 最大 0.003s，算子 server 平均延迟 65.255μs。
因此先检查 consumer / profiler 包裹阶段，不能将这 111s 归因于 kernel 或 trace 解析。

consumer 在退出时追加 `EP_SERVER_DEDUP_WALL`，使用 Host 单调时钟，
终端新增 `CONSUMER_WALL_MAX(s,observed=N)`，每个值仍是独立 rank 最大值：

| 字段 | 覆盖范围 |
| --- | --- |
| startup / communicator | consumer setup、ACL/device 初始化 / communicator 创建及映射检查 |
| allocation / inputs | 设备 buffer 分配 / 输入、五表及 expertOut 构造和拷贝 |
| reference | M1–M5 Host 参考结果生成、通信窗口初始保护值写入、调用方分配检查 |
| barriers | chain、batch、旧单轮 combine Host barrier 等待 |
| execution | Host 提交及 stream 同步，**不是**设备 kernel 延迟 |
| validation | 张量快照和最终 count、Dispatch、Combine 结果校验 |
| cleanup | buffer、communicator、stream、device、ACL 清理及结果输出 |
| outside_consumer | 同 rank process 减 consumer total；含进程加载/退出、msprof 外层工作及调度等，不能全部归为 profiler |

逐 rank 原始分段保存在 `nodes[].reported_result.rank_wall_times[].consumer`。
退出时才能得到总计；超时强杀可能无此记录，旧二进制也没有，均不伪造为零。
不完整或非法诊断记录标记 `invalid` 且不参与统计，不改变原始功能/清理判定。
边界探针和 dry-run 不产生此 consumer 统计。M1/M2 历史 per-round 诊断归并在 execution 中，
日常完整 M5 使用 batch。

已确认的 CPU 开销：workflow 的 Debug 构建原先使 consumer 的全局路由及数值 oracle
不优化运行。仅两个 consumer target 启用 `-O2 -fno-fast-math -ffp-contract=off`，
保留 Debug 符号和严格浮点运算顺序，生产库/kernel 构建参数不变。
本机 GCC 12.2、64P/E384/BS96/K6 的三遍全 producer 路由构造，O0 23.0352s、O2 3.33968s；
两者共 110592 行，路由及权重摘要相同。这是 CPU 子任务证据，不是服务器整体提速结果。
维护教训：性能测试的 Host fixture/oracle 也需要合理优化；先分离初始化、参考构造和 profiler
开销，不能通过省略数值/Flag 校验、关闭 profiler 或延长 timeout 来掩盖测试准备成本。

128P Combine 的提交前排查（2026-09-24）：`dedup=1` 使用原 M4→M5，M6 不参与。
当所有 rank 均没有 `EP_SERVER_DEDUP_CHAIN_PASS` 时，先定位 consumer 初始化和
提交前 barrier；`exit=-15` 只证明进程收到 SIGTERM，不能据此认定 kernel 卡死或确定信号发送方。
所有运行模式现在都在阶段切换前刷新 `EP_SERVER_DEDUP_PROGRESS`，附带 PID 和
Host 单调时间 `elapsed_s`，区分 communicator、CommArgs 校验/拷贝、allocation、
input-fixture、expert-fixture、reference-build、chain-barrier 及算子提交。
日志表示即将进入该阶段；相邻记录的时间差才反映上一阶段耗时。

已复现的 Host 扩展性问题：每个 producer 的 `DispatchRows` 都重新生成所有 source
的输入，而 `balance_shift` 的单 source 生成又遍历全局路由，导致重复计算随 rank 数近似立方增长。
现在线程内仅保留一组不可变 source IDs/weights，按 ranks、experts、BS、K、route mode、seed
完整区分配置；切换配置替换缓存，行/packet 构造和独立数值参考校验仍照常执行。
128P/BS256/K6 时这部分缓存约 1.5 MiB，不缓存设备输出。
本机 `dedup=1`、E384/BS256/H7168/K6/seed42，三轮全 producer 路由参考扫描：
64P 由 10.167s 降至 0.566s，128P 由 78.271s 降至 2.105s；修改前后各自的
行数、路由/packet 字段及 FP32 权重摘要相同。仅为 CPU 子任务测量，尚未证明用户的
128P 故障由这项开销触发，也不代表 NPU 运行已通过。需重新构建、同步 consumer 后复测，
若仍失败，用最新阶段日志定位，保留原始首错；不要只检索 `_PASS` 或把后续取消当作首因。
本次 Host 验证通过：Server Dedup CTest 40/40、非去重缓存配置切换 1/1、Dispatch
consumer 25/25、Combine M5 consumer 5/5；`dedup=0/1、profiling=OFF` 的真实 CANN 9.1
A5 kernel/生产库构建及 consumer 编译链接通过。证据在开发工作区
`run/validation/combine-128p-host/`，未执行 NPU。

此次涉及 consumer 二进制，需要重新构建并同步；性能模式示例：

```bash
git pull
bash build_all.sh breakdown=0 && bash sync.sh
bash run.sh -m 1 -n 8 -e 384 -l 100 50 bs=96 operator_mode=combine
```

breakdown 使用 `build_all.sh breakdown=1` 和 `run.sh -m 2`。重跑后比较上述 wall 统计；
设备 kernel 微秒统计口径保持原样。

开发侧验证：GCC 12.2/C++14 优化 consumer Host 替身回归 30/30；独立 CMake Debug
consumer 构建及 C3/C4/C5/C6/M3/M4/M5 七组 CTest 7/7，编译命令确认
`-g -O2 -fno-fast-math -ffp-contract=off`。Python 工作流 382 项中 381 通过、1 项跳过，
覆盖旧 consumer 无计时、非法计时不改变判定、失败进程保留部分计时、同 rank 分组后再取最大值。
证据在 `build/consumer-wall-verification/`。未执行本次 CANN 生产链接或真实 64P NPU 复测，
不能将本机路由子任务加速倍数外推到服务器端到端耗时。
本次还修正了历史 Dispatch 测试 fixture：普通入口去除 checkpoint 后，该套测试仍断言旧阶段的
marker，须在测试内部显式选择旧验收阶段；无阶段普通运行由 M5 测试覆盖。
旧 fixture 的失败已在前一提交 `700a70c` 复现，此调整不恢复普通入口的 checkpoint 参数。

## 提交验收入口：Combine P2.4（M5）

M4/P2.3 已按用户回传通过 F/P/B 各 4/4、regression/connection 各 2/2，共 16/16。
结果：`/home/h00580772/TileXR-git-check/run/validation/commit-c44caafd510d495e9c2eb67e5cef2604/summary.json`。
该目录后缀为验证运行标识，M4 独立代码提交为 `f770c829202f1b41dcb2bee502c173c7dd419b0d`。
M5 增加 source 端完整 yOut 归约及结果 Flag 清理，恢复连续多轮；已按用户回传通过 24/24。
F1/F2/Fmulti/P/B 各 4/4，regression/connection 各 2/2；结果：`/home/h00580772/TileXR-git-check/run/validation/commit-88e6f60b915444958a04f6149a354671/summary.json`。
关联代码提交为 `cc0477d9934caa3123ab30718f432eaa746de629`，性能项通过表示采集成功，不代表达到性能阈值。

```bash
bash run_commit_validation.sh
```

当前 checkpoint 为 `ep-sdedup-combine-p2-m5`。前两台各 8P，共 16P，
BS96/H7168/K6/E96，Send/Read=32/32；24 个串行用例如下。

| 分组 | 用例数 | 正式/预热轮数 |
| --- | --- | --- |
| F1：单轮功能 | 4 | 1/0 |
| F2：双轮复用 | 4 | 2/0 |
| Fmulti：多轮功能 | 4 | 5/2 |
| P：多轮性能 | 4 | 5/2 |
| B：多轮模块耗时/trace | 4 | 5/2 |
| Dispatch 回归 | 2 | 5/2 |
| Dispatch quant4 → Combine quant0 接线 | 2 | 5/2 |

前五组均为 BF16/FP16 × combine/both。先关闭打点构建并完成 20 例，再开启打点重新构建并完成 4 例；
两次构建更新同一个安装目录 `install/validation-combine-p24`，要求同一源码摘要，逐次保存 manifest 和结果。
量化接线保留 Dispatch 五表/count，Combine 使用独立低精度 expertOut；Combine 不支持量化。

人工性能和 trace 共用安装目录，切换模式时重新构建并同步：

```bash
bash build_all.sh breakdown=0
bash sync.sh
bash run.sh -n 2 -e 96 -m 1 -l 5 2 operator_mode=combine
bash build_all.sh breakdown=1
bash sync.sh
bash run.sh -n 2 -e 96 -m 2 -l 5 2 operator_mode=combine
```

日常 `build_all.sh`、`sync.sh` 和 `run.sh` 不需要设置 checkpoint；workflow 会选择当前
生产 Combine 包，不依赖前一个 shell 的环境变量，也不会被旧的
`install/validation/manifest.json` 覆盖。Dispatch 单跑仍可直接使用同一生产包。
checkpoint 仅由 `run_commit_validation.sh` 内部用于验收提交，不作为日常运行参数。

`operator_mode=both` 测联合调用；D/C 分别输出 trace，Combine 包含 M5 final 区间。
统计区分单次、正式轮累计、含预热累计字节；trace 保存最后一次调用（7 次调用时 index=6）。
性能及模块拆解仅确认采集链路，无耗时阈值。数值校验失败但执行完成时仍保留测量和 trace，最终 FAIL。

数值判据预先固定为本次有限正常数/零 fixture 的逐位一致：先模拟 M4 self 后成员顺序
FP32 乘加和通信 dtype 舍入，再按原 token 有效 k 升序做 M5 FP32 乘加及 ties-to-even 输出。
首乘积直接初始化累加区，保留 signed zero；零权重仍等 Flag 和读取 payload。
不得以全 K 一次 FP32 累加替代两级舍入，失败后不放宽容差。
检查最终 yOut、有效 DIRECT/partial payload、未写槽、输入、stage、控制区和各已使用窗口。
结果区全部 K*N 个 32B Flag 均须为零；Host 只在初始分配时清零，不插入逐轮清理助手。

实现边界：Read 核按 BS 连续均分原 token；pending 列表保存实际 TokenId，移除后交换末项，
表 4/5 索引始终使用原 token。每次探测有效 k 的所有 Flag，只调用一次 ReduceSum；
最后一个 token 就绪后，对原始整个负责区间的全部 Flag 发起清理，再完成最后的 payload/yOut。
M4 末尾排空后才复用 UB，退出前排空 MTE3，不再保留 50μs。
190KiB UB 保留默认 7488B M2 常驻区，先预算表、Flag、归约工作区和三份 FP32 工作区，
再分配输入/输出 lane；默认 BS96/H7168/K6 为输入 5 / 输出 1，K32 为输入 3 / 输出 1。
Host 与 kernel 共享预算准入；探测使用 Event 7，payload lane 仅使用 0..6，避免事件串扰。

开发教训：CANN 9.1 dav_3510 ReduceSum 即使每个 32B block 只取首个 float，底层仍可能整段
256B 向量加载。Flag UB 须预留尾部读取余量，工作区须按 256B 向上对齐，不能只按有效标量数分配。
数值 fixture 必须传 TileXR 命名 dtype 枚举，不能假设 FP16/BF16 为 0/1；本次数值测试捕获并修正了此类测试构造错误。
多轮验收必须由生产 M5 清理全部结果 Flag；测试侧清零会掩盖窗口复用错误。
CPU 替身只能验证调度/地址/数值约定，跨 rank 可见性与并发清理时序仍须 A5 16P 验收。

开发侧最终验证（2026-09-21）：blue CANN 9.1、ccec/Clang 15.0.5，A5 `dav-c310-vec`，
正式 workflow OFF/ON 构建安装成功，安装包 Host 各 10/10，额外 M1–M5/config/profile/Host 准入 8/8。
OFF build_id=`b9b89471b6ca40408a0234ebc1553148`，ON build_id=`3fd025ab4cbf428198106939de816766`；
两包源码摘要均为 `f1b3cef371ce35c89de77ff33cc208dde57ae7d75e1405b592ac6241a35306a4`。
本次为冻结文件快照构建，manifest commit 为空；待提交生产/测试/脚本与快照逐文件一致，随后仅补文档。
Python 验收工具 366 项中 365 通过、1 项 Windows 专属检查跳过；M3/M4/M5 consumer 替身各 4/4。
主 shape 六组 16-rank consumer 替身各 2 预热+5 正式调用通过，覆盖 N=1..5 与同卡成员，
检查完整双窗/最终 yOut，并实际确认本次 fixture 数值域为有限正常数或零。
证据保留在开发工作区 `build/combine-p24-verification/`，包括编译配置、退出码、产物哈希和源码 provenance。
以上开发侧证据本身不证明真实 A5 跨 rank 可见性、连续调用时序或设备数值；后续用户已回传上述 P2.4 24 例设备验收通过。
该结论限于既定矩阵，不外推至任意配置、压力场景或性能达标。

<a id="当前入口combine-p23m4"></a>
### P2.3（M4）历史开发及验收证据

P2.2 已按用户回传通过 F/P/B 各 4 项、regression/connection 各 2 项，共 16/16。
结果：`/home/h00580772/TileXR-git-check/run/validation/commit-f59416872bdb49aa9387c398c779c522/summary.json`。
P2.3 累计 M1/M2/M3，增加 Read 核 self + stage 成员 FP32 加权及 BF16/FP16 partial 返回。
M5 未接入、yOut 保持；不保留 50μs，不清结果 Flag。M4 已按用户回传通过 16/16，结果见下文 P2.4 入口。

```bash
bash run_commit_validation.sh
```

当前 checkpoint 为 `ep-sdedup-combine-p2-m4`。前两台各 8P，共 16P，BS96/H7168/K6/E96、
Send/Read=32/32；共 16 例：F/P/B 各 4（BF16/FP16 × combine/both）、Dispatch 回归 2、
D quant4 → C quant0 独立低精度 expertOut 连接 2。**全部 0 预热 + 1 正式调用**。
OFF 12 例与 ON 4 例分别安装到 `install/validation-combine-p23-profile-off/on`，源码摘要须一致。
不复用前序 checkpoint 的通过记录。性能只确认数据可获取，breakdown 只验模块解析，均无达标阈值。
DIRECT、partial 的有效 payload 和每块 ready Flag、未写槽、M1 stage、M2 控制清理、输入/yOut、
地址边界同时校验；数值失败但 kernel 正常完成时继续保存可用性能/trace，最终 FAIL。

手动性能/trace 使用相同编译、同步、运行包：

```bash
export TILEXR_VALIDATION_CHECKPOINT=ep-sdedup-combine-p2-m4
bash build_all.sh breakdown=0
bash sync.sh
bash run.sh -n 2 -e 96 -m 1 -l 1 0 operator_mode=combine
bash build_all.sh breakdown=1
bash sync.sh
bash run.sh -n 2 -e 96 -m 2 -l 1 0 operator_mode=combine
```

`operator_mode=both` 测联合调用；D/C 独立 trace，M4 记录 Read 核模块区间，
stage、DIRECT、partial 分别统计逻辑 payload 与实际 wire 字节。单次数据不能外推为稳定性能基线。

M4 数值判据在上机前固定：有限正常数/零输入，以 self 后成员顺序，独立执行 FP32 乘法、
FP32 加法（self 乘积直接初始化累加区，保留 signed zero），最后 ties-to-even 转为通信 dtype；
有效 payload 逐位相同（atol=rtol=0）。
standalone 包含正负非整数/零权重以及不同 expert 值，both 保留真实 Dispatch 五表及权重；
padding 不参与数值比较，不将 M4 partial 与全 K 一次 FP32 最终和混比，失败后不临时放宽误差。

实现与复用边界：190KiB UB 中 M2 常驻资源默认 7488B；M4 筛选后压缩为本 Read 核任务行号，
重用其余筛选空间，表 2/3 各缓存相同原行号跨度最多 32 行。三份 FP32 工作区、低精度打包区、
至少一个独立输出 lane 先入预算，再分配最多 8 个输入 lane，余量分配最多 8 个输出 lane。
默认 tokens10000/H7168/K6 使用输入 4 / 输出 1，Host/kernel 共用准入预算。
输入释放取决于 Vector 最后读取，输出仅在重用对应 lane 前等待 MTE3；换表不统一排空发送。
M4 末尾 Event 收尾及 PIPE_ALL 为未来 M5 复用建立边界，M5 具体预算仍不在本次实现范围。

开发教训：CANN 的 `GM_ADDR` 是带指针的宏，多个成员不能写为同一句逗号声明；
后续名字会被解析为带 GM 地址空间的非指针字段，CPU 替身可能掩盖此问题。
每个 GM 地址字段单独声明，并以真实 CANN 9.1 A5 OFF/ON 编译确认。

开发侧最终验证（2026-09-21）：blue CANN 9.1、ccec/Clang 15.0.5、A5 `dav-c310-vec`，
正式 workflow OFF/ON 构建及安装成功，安装包 Host 各 10/10 通过。
OFF build_id=`051af7a2dd5b45999a5f31eed12463a0`，ON build_id=`5cc236965b9b42b182f868af587df41e`；
两包源码摘要均为 `94559804fe08720eb843b08be7d5bb89984de1ea5b9aeb147bd0eb4e5c9f9276`。
构建采用冻结文件快照，manifest commit 为空；生产及测试文件与快照逐文件一致，之后仅补本文证据。
`python3 -m unittest discover -s tools/validation -p 'test_*.py' -q`：357 项中 356 通过，
1 项 Windows PowerShell/MSYS 专属检查跳过。额外 M1/M2/M3/M4、准入/config/profile 聚焦 Host 7/7 通过；
最终 M4 测试包含 L=2 寻址及完整 uint32 N 解析，由两包 Host 再次验证通过。
同一主 shape 六组 16-rank consumer Host 替身全部通过（两 dtype × combine/both，以及两 dtype Dquant4），
额外成员 N 覆盖 1..5、同卡成员共 361，逐元素确认本次 fixture 的 FP32/低精度 partial 为有限正常数或零。
standalone 覆盖正负/零与非整数权重；量化连接的独立专家结果使用正值，避免无关的 subnormal 数值域。
源码 provenance、编译器/配置/退出码、产物哈希、构建与 Host 日志保存在开发工作区
`build/combine-p23-verification/`。以上不证明 M4 的 A5 跨 rank 可见性或设备数值正确；
这些仍由用户在本次提交执行 16P P2.3 矩阵确认，不将 M3 通过记录外推到 M4。

<a id="当前入口combine-p22m3"></a>
### P2.2（M3）历史开发及验收证据

P2.1 的 M1/M2 已按用户回传登记通过。P2.2 增加 M3：Send 核将 type1 的原始
BF16/FP16 expertOut 按表 2 返回 source/token/k 结果槽，每块 480B payload + 32B Flag；
保留 M1/M2，移除所有核末尾 50μs 等待。M4/M5 尚未实现，type2 不生成 partial，yOut 保持。
P2.2 已按 2026-09-21 用户回传通过 16/16；结果见调试进度，以下命令须在 M3 历史提交执行。

最新 CB06-04 覆盖原“四类多轮”计划：M3/M4 的功能、性能、breakdown 均为 **0 预热 + 1 正式调用**。
检查到达数据/Flag 后结束，不增加测试侧结果 Flag 清理助手；M5 接入生产结果清理后再验证完整多轮复用。
性能只验数据可获取、模块解析正常，不设置时延门槛，不将单次数据当作稳定性能基线。

```bash
bash run_commit_validation.sh
```

当前 checkpoint 为 `ep-sdedup-combine-p2-m3`，沿用 hostfile 前两台各 8P，共 16P；
BS96/H7168/K6/E96，Send/Read=32/32。共 **16 个串行用例**：

| 分组 | 用例数 | 配置 |
| --- | --- | --- |
| F：单轮功能 | 4 | BF16/FP16 × combine/both，profiling OFF |
| P：单轮性能采集 | 4 | 同上，profiling OFF |
| Dispatch 回归 | 2 | BF16、FP16，profiling OFF |
| 量化接线回归 | 2 | Dispatch quant=4，Combine quant=0，独立低精度 expertOut |
| B：单轮模块耗时/trace | 4 | BF16/FP16 × combine/both，profiling ON |

OFF/ON 独立构建安装且源码摘要必须相同，目录为
`install/validation-combine-p22-profile-off` / `install/validation-combine-p22-profile-on`。
三个测试模式共用 consumer；both 保留五表/count 的原 tensor 接线及 D/C 各自 magic/窗口。
M3 校验包含 DIRECT 有效字节及每块 ready Flag、未写槽、M1 stage、M2 控制区、输入/yOut 和边界保护。
若 kernel 正常结束但结果校验失败，仍保留可用性能和 trace，最终汇总为失败。

手动测试也须显式使用单轮，构建、同步和运行选择同一包：

```bash
export TILEXR_VALIDATION_CHECKPOINT=ep-sdedup-combine-p2-m3
# 性能采集：OFF
bash build_all.sh breakdown=0
TILEXR_VALIDATION_BREAKDOWN=0 bash sync.sh
bash run.sh -n 2 -e 96 -m 1 -l 1 0 operator_mode=combine
# 模块 trace：ON
bash build_all.sh breakdown=1
TILEXR_VALIDATION_BREAKDOWN=1 bash sync.sh
bash run.sh -n 2 -e 96 -m 2 -l 1 0 operator_mode=combine
```

将 `operator_mode=combine` 改为 `both` 可测试联合调用；输入 dtype、量化模式仍沿用共享脚本参数。
单独脚本调用中的环境变量不会自动传给下一次调用，一键验收负责完整选择。
D/C trace 分别保存；模块事件为 M1a Init、M1b、M2（Read 核）、M3（Send 核）、M6 及 kernel total，
异步区间不可相加冒充总耗时。P2.2 不复用 P2.1 的 `--resume` 通过记录。

实现注意：去掉 Dispatch 包尾后必须重新计算结果包 stride，不能继续沿用 Dispatch 的包长。
M3 的 GM 输入只读 `2H` 字节，但 Vector 打包会覆盖 `ceilDiv(2H,480)*480` 字节 UB，
因此 Host/kernel 共用的准入预算同时计入该输入跨度及 `N*512` 输出 lane；仅预算有效输入会越界。
M1 排空后复用输入 lane，M3 输入由 Vector 完成释放、输出由 MTE3 完成释放。
CPU 测试覆盖尾块、稀疏表跨度和延迟 MTE3 的 lane 回绕；真实设备可见性仍须 16P 验收。

开发侧验证（2026-09-21）：blue 使用 CANN 9.1、ccec/Clang 15.0.5、A5 `dav-c310-vec`，
正式 workflow 的 OFF/ON 构建、安装均成功，安装包 Host 验收各 10/10 通过。
最终 `python3 -m unittest discover -s tools/validation -p 'test_*.py' -q` 共 349 项：
348 通过、1 项 Windows PowerShell/MSYS transport harness 因环境条件跳过；Shell 语法与 diff 检查通过。
OFF build_id=`8f2cead3edd14d9586232c1c76d42d32`，ON build_id=`d85cf289c8624fa0978e69bec54291f4`；
源码摘要均为 `1811119abe0d6e77f6a7ba63c444ad0034f3a16d1346c18a348690d6da56704a`。
构建使用文件快照，manifest commit 为空；不将快照基点 SHA 冒充交付提交的构建证据。
生产代码与该快照逐文件核对一致；构建后仅补充 Python 回归测试及文档，最终测试按更新后的文件执行。
逐文件 provenance、编译器/配置/退出码、产物哈希及 Host 日志保留在开发工作区
`build/combine-p22-verification/`。当前证据不包含 NPU 实际运行、跨卡可见性或性能达标。
本次集成曾在编译、安装成功后因 manifest 生成路径的 Python 命名错误失败；已修复并增加
执行到 manifest 导出的构建行为回归，不能只凭编译器退出码判构建链路通过。
数值 INVALID 为完成性能/trace 导出可正常退出进程；验收必须读取结构化校验状态并最终判 FAIL，
不能仅凭进程 exit=0 登记通过。

### P2.1 历史验收及续跑

以下为 P2.1 专用流程，复验需切换相应历史提交（`acb826b` 实现、`4ffc700` 续跑）。
P2.1 保留 50μs，23 例及旧 OFF/ON 包不作为 M3 通过证据。

P2.1 源码摘要排查（2026-09-21）：用户回传 OFF 的 19 例通过、ON 构建成功，但在两包源码
一致性检查处停止，4 个 breakdown 未运行；两份清单均为 965 个同名文件，期间被改写的候选为
编辑器探测文件 `.vscode_io_test.tmp`。原清单会纳入该文件，内容变化可复现摘要变化。
现由 `tools/validation/source.py` 按精确文件名在各层目录排除，同时用于摘要和同步清单，
`.gitignore` 也排除它；仅修改 rsync 或 Git 忽略规则无法修复普通文件部署的摘要采集。
回归覆盖探测文件创建/改写/删除不影响摘要和同步、其他 `.tmp` 输入仍保留、真实源码修改仍改变摘要。
此修复不改 Host/kernel，不取消全量运行的 OFF/ON 一致性门槛；不能将 19 例通过登记为 P2.1 整体通过。

针对此次“19 项 OFF 已通过、ON 已构建但因源码摘要差异停止”的运行，可显式续跑：

```bash
bash run_commit_validation.sh --resume /绝对路径/原commit运行目录/summary.json
```

保留原报告、各用例 summary 及 `install/validation-combine-p21-profile-off` / `-on`，更新脚本后
直接执行，**不要先重建安装包**。续跑核对矩阵、参数、完整 16 rank、host/device、原 OFF Host
通过证据，以及两包 build_id/全部产物哈希；条件不符即停止，不自动回退重跑 19 项。
核对通过后显示 19 条 `SKIP (reused PASS)`，跳过两次构建，只补原 ON 包的 Host 检查、同步及
`[20/23]`–`[23/23]` 四项 breakdown。无参数仍全量运行，旧环境变量不能隐式触发续跑。

新 summary 快照保存历史证据，逐项标记 `reused` 和来源，并保留原 OFF/ON 摘要及原错误；
`resume.source_consistency=historical-mismatch-explicit-resume` 表示用户显式接续已有产物。
旧 manifest 无逐文件摘要，不能追溯证明其差异仅来自编辑器文件；该入口不重建任一包，也不宣称
两包源码摘要一致或新 checkout 的算子已重新验证。四项新增验收也通过后才输出总通过标志。
本入口仅覆盖上述特定中断点，不接受任意失败记录或自动搜索“最近一次通过”。

**续跑验收已通过（用户回传）**：F1/F2/P/B 各 4/4，regression 2/2、connection 2/2、edges 3/3，合计 23/23；
总标志为 `COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p2-m12`。复用历史 OFF 19 项，补齐 ON 4 项，
总报告及身份核对边界见[进度记录](EP_MEMORY_SERVER_DEDUP_PROGRESS.md)。范围为 M0/M1/M2、必要排空和 50μs 等待，
不包括 M3–M5 或 yOut 业务数值，也不代表已给出具体性能达标结论。

## Combine 第一阶段：C1–C7 用户回传验收通过

C1–C7 均已按用户回传登记通过，第一阶段空算子验收收口。各点的通过结论仅覆盖对应验收范围；
真实 Combine 通信、业务数值及 Flag 复用遗留不在通过范围内。完整七点记录见[进度](EP_MEMORY_SERVER_DEDUP_PROGRESS.md)。
C7 回传 `COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p1-memory-boundary`，3/3 用例通过。
总报告：`/home/h00580772/TileXR-git-check/run/validation/commit-93bd127f77944513acd404a57c244c76/summary.json`。
按用户回传登记，未独立读取外部报告或核对实际运行身份；末例报告及详细边界见进度文档。

| 点 | 提交 | 验收范围 | 状态 |
| --- | --- | --- | --- |
| C5 | `4ef0439640b016727287bebec84ef5cc0a521445` | 连续预热/多轮、三种算子模式、magic/生命周期 | 用户回传通过 |
| C6 | `abee43c70502de88c18ddd0700b0afde528ecc93` | OFF performance / ON breakdown、独立记录区和双 trace | 用户回传通过 |
| C7 | `5ce1a927802a05ab5455a55ec30fe449d99e3d0d` | 双窗合法边界探针、真实 Dispatch 回归 | 用户回传通过 |

如需复验历史空算子 C7，切换其提交后执行：

```bash
git fetch origin
git switch --detach 5ce1a927802a05ab5455a55ec30fe449d99e3d0d
bash run_commit_validation.sh
```

C7 checkpoint=`ep-sdedup-combine-p1-memory-boundary`，使用独立
`build/validation-combine-c7` / `install/validation-combine-c7`，profiling OFF。
一键构建并执行 Host 回归后，沿原 hostfile/sync/run 路径使用前两台固定 8P，串行三组：

| 次序 | 用例 | 配置 |
| --- | --- | --- |
| 1/3 | 双窗本卡边界探针 | 8GiB 通信域，两窗各四个 32B 目标，独立测试 kernel |
| 2/3 | 真实 Dispatch BF16 | functional，D quant=0，warmup=2/loops=5 |
| 3/3 | 真实 Dispatch FP16 | functional，D quant=0，warmup=2/loops=5 |

各组新建进程和通信域，probe 不运行 D/C、不消费 magic；后两组使用既有 Dispatch 完整输出和通信 Flag 校验。
每组业务 shape 仍为 BS96/H7168/K6/E96。首错停止，日志显示 `[i/3]`，总报告保留各组 summary。
构建期限 900 秒，Host 总期限 180 秒/单项 120 秒，设备每组 180 秒。
全部判据通过后输出 `COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p1-memory-boundary`。

探针在每窗 Combine 控制区首尾、共享数据起点及 EP 合法区尾部读写，并检查邻接 guard。
两窗步长 4GiB，统一排除各自末 4MiB；所有访问均止于该合法上界，绝不读取或写入 runtime 保留区。
每窗/每点模式不同，最终再次读回全部点位，发现跨窗别名或地址高位截断。
测试内部 `TILEXR_VALIDATION_BOUNDARY_PROBE=1` 只由 C7 workflow 探针用例启用；
普通 `run.sh` 强制为 0，保持 dispatch/combine/both 的正常算子调用。

C7 不包含 Combine 数据面、Flag 协议或 yOut 业务数值验收；空 Combine 的 50μs 等待不变。
C5 多轮、C6 profile 及 C7 边界均已分别获用户通过确认。当前 Flag 复用问题继续作为设计遗留，未在本点扩展。

2026-09-21 C7 开发验证：CANN 9.1.0 / ccec clang 15.0.5（2026-05-18）、A5
`dav-c310-vec`、profiling OFF，通过正式生产构建、安装及真实 consumer 编译链接。
`build_id=e0b0dd104d46465f8b2f720d0fb65999`，安装包 Host 验收 14/14 通过，
报告 `host-4ce75a0fce2647a8ac19bfb17134bca9/summary.json`。最终 C7 driver 7 组（含缺省 selector）
另用该安装包复验通过；已有 Dispatch 25 组、C4 9 组、C5 8 组回归通过。
控制脚本执行 319 项，318 通过、1 项因 Windows transport harness 条件不足跳过；
入口 selector 修正后重跑相关 24 项通过，环境隔离 9 项通过。独立审查发现的入口污染问题已修正，
probe 注册/Kernel 及最终代码审阅无剩余发现；Shell 语法和 diff 检查通过。
blue 为无 `.git` 源码快照，manifest 如实记录 commit 未知；构建源码摘要、最终文件 SHA256、
工具链和构建/Host/脚本日志保留在开发工作区 `build/combine-c7-verification/`。
这些是开发侧编译及 Host 证据；后续用户已回传三组真实 16P 用例通过，见上文验收记录。

## Combine 第一阶段 C6：用户回传验收通过

已登记用户回传 `COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p1-profile`，6/6 用例通过。
总报告：`/home/h00580772/TileXR-git-check/run/validation/commit-06f978880a8340f78a48a51176ad30a6/summary.json`。
通过范围及未独立核验的运行身份见[进度](EP_MEMORY_SERVER_DEDUP_PROGRESS.md)。如需重现 C6：

```bash
git fetch origin
git switch --detach abee43c70502de88c18ddd0700b0afde528ecc93
bash run_commit_validation.sh
```

C6 checkpoint=`ep-sdedup-combine-p1-profile`。脚本使用 hostfile 前两台、每台 0..7，共 16P，
按以下顺序执行两个构建批次，每批先完成 11 项 Host 检查，再同步并运行三个算子模式：

| 批次 | 打点 | 运行模式 | 用例 |
| --- | --- | --- | --- |
| 1 | OFF | performance（msprof） | dispatch、combine、both |
| 2 | ON | breakdown | dispatch、combine、both |

六组均为 BF16、D quant=0、C quant=0、BS96/H7168/K6/E96、warmup=2/loops=5。
显示 `[i/6]`，首次失败停止；每次构建 900 秒、Host 总期限 180 秒/单项 120 秒，每个设备用例 180 秒。
分别使用 `build/validation-combine-c6-profile-off`、`build/validation-combine-c6-profile-on`，
安装到 `install/validation-combine-c6`；总报告保留两份 manifest、build_id 和源码内容摘要，
不接受构建中源码变化或两阶段源码不一致。旧阶段报告不因后续安装更新丢失。
成功终端结论为 `COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p1-profile`。

D/C 使用独立 GM 记录地址、计数器及 latest 状态，未启用算子不分配、不导出、不解析。
每 rank 的记录分别为 `rank-R.dispatch.records.v1`、`rank-R.combine.records.v1`，
报告分别位于 `profile-rank-R/dispatch` 和 `profile-rank-R/combine`，联合模式保留两份 trace。
两区共用 run/capture UUID，各自绑定 operator、kernel SHA、rank 与最后一次 launch；
某区解析失败时保留另一有效区的报告，整体仍判失败。
performance 从同一 msprof 采集分别匹配启用算子；Combine 报告 `empty/framework`、
算法字节 0、算法带宽 N/A，延迟包含 50μs 空等待，不代表 Combine 算法性能。

16B Combine tiling 原保留字节 13 现为 profilingEnabled，14/15 仍为零；27 参数 API、88B launch 不变。
CommArgs 在尾部追加 Combine capture 字段，旧字段 offset 保持；主机库和 kernel 必须使用同一构建安装包。
开发验证和设备验收分开记录；C5 多轮、C6 profile、C7 边界各自需要对应提交的外部结果。

2026-09-21 C6 开发验证：CANN 9.1.0 / ccec clang 15.0.5（2026-05-18）、A5
`dav-c310-vec`，OFF/ON 分开构建生产 Host/runtime/Kernel 和安装包 consumer，均通过。
OFF build_id=`4d07c76f2bce494da0f545d615a9c476`，ON build_id=`5814b6b13ed840599ec0652478cf96a6`；
各自安装包 Host 验收 11/11 通过。控制脚本执行 312 项，311 通过，1 项因缺 Windows transport harness 跳过；
既有 Dispatch consumer 25 项通过。独立审查后的故障路径保留另一算子有效报告，Shell/diff 检查通过。
blue 使用无 `.git` 源码快照；manifest 如实记录 commit 未知，源码与产物哈希、工具链及日志
保留在开发工作区 `build/combine-c6-verification/`。这些是编译和 Host 证据；后续用户已回传六组真实 16P 用例通过，见上文验收记录。

## Combine C5：用户回传验收通过

已登记 C5 用户回传 `COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p1-chain-multi`，
8/8 用例通过。总报告：`/home/h00580772/TileXR-git-check/run/validation/commit-22f77fd2236149a09405863e25bc9543/summary.json`。
通过范围及未独立核验的运行身份见[进度](EP_MEMORY_SERVER_DEDUP_PROGRESS.md)。如需重现 C5，先切回其交付提交：

```bash
git switch --detach 4ef0439640b016727287bebec84ef5cc0a521445
bash run_commit_validation.sh
```

C5 提交的 checkpoint 为 `ep-sdedup-combine-p1-chain-multi`，使用独立
`build/validation-combine-c5` / `install/validation-combine-c5`。
脚本先构建并执行 Host 回归，再在 hostfile 前两台固定 8P 运行 **8 个串行设备用例**，
每组均为 BS96/H7168/K6/E96、functional、loops=5、warmup=2：

| 用例 | 输入 | Dispatch quant | Combine quant |
| --- | --- | --- | --- |
| Dispatch 单跑（2组） | BF16、FP16 | 0 | 未启用 |
| Combine 单跑（2组） | BF16、FP16 | 未启用 | 0 |
| 联合非量化（2组） | BF16、FP16 | 0 | 0 |
| 联合量化 Dispatch（2组） | BF16/E4M3、FP16/E5M2 | 4 | 0 |

终端显示 `[i/8]` 和当前配置/结果路径；重复拉起进程是串行执行不同用例。
构建期限 900 秒，Host 总期限 180 秒/单项 120 秒，每个设备用例 180 秒；首个失败即停止。
全部用例与 Host 判据通过后才输出
`COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p1-chain-multi`，总报告链接各组结果。

同一 stream 连续提交全部 warmup/正式轮次，每轮 both 为 D→C，仅批末一次同步和 Host barrier。
单算子轮换两窗；both 的 D magic 为 1/3/.../13，C 为 2/4/.../14，各占一窗符合预期。
统计分别记录预热/正式提交完成数；magic 由成功公开调用序列推导，不额外递增来观察。
Combine 单跑比较七输入和 yOut 全容量快照；both 校验最后 Dispatch 输出与其窗口 Flag，
并比较 yOut 及独立 BF16/FP16 expertOut（D4）。共享五表由 Dispatch 每轮重写，
报告明确标记最终参考校验，不宣称逐轮输入快照保持；C4 Host 回归继续覆盖完整空行为。
空 Combine 的 50μs 等待保持；本点不验证 Combine 通信或 yOut 业务数值，不输出性能结论。
双算子 profiling 分区与 trace 属于 C6；2026-09-21 用户授权在 C5 待验证时提前开发。
详见[验收流程 C5](EP_MEMORY_SERVER_DEDUP_VALIDATION_FLOW.md#c5-当前交付连续批次各算子-magic-和生命周期)。

2026-09-20 C5 开发验证：基于 `c86e1efe8fd598119112074df420fc1cce7ee4aa` 加本次改动，
blue 的 CANN 9.1.0 / ccec clang 15.0.5（2026-05-18）、A5 `dav-c310-vec`、profiling OFF
通过正式 C5 production workflow 构建、安装、真实 consumer 编译链接及 ELF/导出/依赖检查。
`build_id=365c2125c0d44f2f9ba5bb3ec6568070`，构建退出码 0；安装包 Host 验收七项全部通过，
报告 `host-7b8582024df04b14b9a89b55cafe49f4/summary.json`。
共享 consumer 的 C5 连续批次/故障注入 8 组、C4 生命周期 9 组及既有 Dispatch 25 组通过；
控制脚本回归执行 294 项，293 通过，1 项因缺少 Windows PowerShell/MSYS transport harness 跳过。
独立审查未发现需修正的具体缺陷，Shell 语法及 diff 检查通过。
blue 使用无 `.git` 的源码快照，manifest 不冒认 commit；全部变更 C++/CMake 与已编译源码 SHA256 一致。
编译配置、工具链、构建/Host 日志与 manifest 保留在开发工作区 `build/combine-c5-verification/`。
这些仅为 Host 和编译证据；开发交付时未执行八组真实 16P 多轮矩阵；后续用户已回传该矩阵通过，见上文验收记录。

## Combine C4 历史交付与验收

已登记 C3 用户回传通过（交付 `b68b56e`），报告路径见[进度](EP_MEMORY_SERVER_DEDUP_PROGRESS.md)。
拉取本次提交后仍只需执行：

```bash
bash run_commit_validation.sh
```

当前 checkpoint 为 `ep-sdedup-combine-p1-chain-single`。本次恢复真实设备验证，需要已配置的
CANN 9.1、driver、A5 设备、SSH/rsync 及 hostfile；使用清单前两台、每台设备 0..7。
本机先构建安装 `install/validation-combine-c4`，完成六项 Host 回归，然后沿既有同步/运行流程
执行六个 16P 单轮用例：Combine BF16/FP16，both+D0 BF16/FP16，both+D4 BF16/E4M3、FP16/E5M2。
固定 BS=96/H=7168/K=6/E=96、functional、loops=1、warmup=0，每 rank 8GiB 通信域。
构建总期限 900 秒，Host 总期限 180 秒、单项 120 秒；每个设备用例使用现有 180 秒运行期限。
不会跳过缺少配置、依赖、设备、rank 或判据的场景；首次失败停止矩阵并保留已有 case 日志。
总报告为终端打印的 `run/validation/commit-*/summary.json`，其中链接 Host 和每组设备报告。

非量化联合模式直接使用 Dispatch 五表、完整 sendCounts 和 expandXOut；量化 Dispatch 的
Combine expertOut 独立分配为共享 dtype 的 BF16/FP16，Combine quant 始终为 0。
本点同步后比较 Combine 七个输入和 yOut 全容量快照，空算子应完全不改写；不检查 yOut 业务数值。
空 Combine 每个 AIV 通过 `GetSystemCycle()` 等待 50000 ticks（A5 上约 50μs），单跑和连跑统一生效。
真实 Dispatch 仍执行完整输出和对应窗口 Flag 校验；不将 Combine 窗口交给 Dispatch 包解析器。
C4 仅支持单轮 functional，连续多轮与双 trace 分区分别留 C5/C6，六组全部通过后再推进 C5。

以下为 C4 历史实现：当时实际运行读取 C4 包、dry-run 使用 C3 包。当前日常入口均使用本页首节的无 checkpoint 普通包。
详见[验收流程 C4](EP_MEMORY_SERVER_DEDUP_VALIDATION_FLOW.md#c4-历史交付单轮接线和输入生命周期)。

2026-09-20 C4 开发验证：基于 `b68b56e02e1f49975549515f2d7f805ee4e1e0c8` 加本次改动，
blue（CANN 9.1.0 / ccec clang 15.0.5、A5 `dav-c310-vec`、profiling OFF）完成正式 workflow 的
C4 production build、安装与 Host 验证六项全部通过，包含已安装生产 consumer 的编译链接、ELF/导出/RPATH 检查。
共享 consumer 的 C4 生命周期/故障注入 9 项通过，C3 dry-run 7 项和原 Dispatch Host 生命周期 25 项通过；
受控时钟下的 50μs 阈值/溢出/无写测试通过。工具回归运行 287 项，286 通过、1 项因缺少
Windows PowerShell/MSYS transport harness 跳过。
审查修正单 C 的不完整 Dispatch epoch 限制，并补充同 C4 安装包的 Dispatch 单跑解析回归。
这些是 Host 替身与交叉编译证据；本次没有执行真实 NPU 或六组 16P 矩阵，也未实测 kernel 时长。
之后用户已回传外部 C4 `COMMIT_VALIDATION_PASS`，按报告范围登记，不扩展为多轮通过。
blue 使用无 `.git` 的源码快照，报告 commit 为 null；源码 SHA256 与本次合入代码逐文件一致。
manifest、工具链版本、构建/Host 日志及摘要保存在开发工作区 `build/combine-c4-verification/`。

使用同一 C4 安装包手动跑 Dispatch 单轮时，可显式选择检查点（其他参数沿用 `run.sh`）：

```bash
TILEXR_VALIDATION_CHECKPOINT=ep-sdedup-combine-p1-chain-single bash run.sh -n 2 -e 96 operator_mode=dispatch
```

## Combine C3 历史交付与验收

C1/C2/C3 已收到用户通过确认；报告路径和登记范围见[进度](EP_MEMORY_SERVER_DEDUP_PROGRESS.md)。
拉取本次提交后继续执行：

```bash
bash run_commit_validation.sh
```

C3 交付时 checkpoint 为 `ep-sdedup-combine-p1-dry-run`。脚本重新构建独立 Host 安装包
`install/validation-combine-c3`，执行 C1 三项回归和真实共享 C++ consumer 的 dry-run 配置测试。
需要 Python 3.9+、CMake、Make、g++；不需要 CANN/NPU/hostfile/SSH。
C3 四项全部通过才输出 `COMMIT_VALIDATION_PASS`；用户已回传通过，本页当前入口已推进 C5。
不会执行 C2 设备测试，不把本次结果当作通信或业务数据验证。

现有手动 `run.sh` 增加 `operator_mode=dispatch|combine|both`（默认 dispatch）、
`dispatch_quant_mode=0|4`、`combine_quant_mode=0`、`dry_run=0|1`。
`quant_mode` 保留为 Dispatch 别名，别名冲突拒绝；dtype 与 fp8_format 共用，非量化时格式无效。
例如完成一键构建后，在现有 hostfile 上本机验证联合模式配置：

```bash
bash run.sh -n 2 -e 96 -m 0 -l 1 0 operator_mode=both dispatch_quant_mode=4 combine_quant_mode=0 dtype=bf16 dry_run=1
```

该手动入口逐 rank 调用同一源码编译的 C3 consumer，仅读 hostfile，不 SSH、不加载 CANN、不分配设备内存。
一键验收自带 rank fixture，无需准备 hostfile。真实 Combine/both 执行 C4 前拒绝；
C3 的输入来源字段仅为配置计划，五表原指针接线、量化 Dispatch 的独立 BF16/FP16 输入归 C4。
完整范围见[验收流程 C3](EP_MEMORY_SERVER_DEDUP_VALIDATION_FLOW.md#c3-历史交付共享-consumer-的三模式-dry-run)。

2026-09-20 C3 开发验证：基于 `3c39e9902957ade9b802da5284e0559eb8b1fe35` 加本次改动，
blue 源码快照完成一键 Host 构建、安装和四项验收，输出 `COMMIT_VALIDATION_PASS`；
快照没有 `.git`，summary 的 commit 为 null，不能视作原样 Git SHA 上的外部验收。
共享 consumer 的 7 组 dry-run 测试通过（含 16/64/128 共 208 个 rank 映射）；
原 Dispatch consumer 25 项 Host 生命周期回归通过；手动三模式 dry-run 各完成 16/16 rank。
集成后的工具测试运行 278 项，277 通过、1 项因本地无 Windows PowerShell/MSYS transport harness 跳过。
CANN 9.1.0 A5 `dav-c310-vec`、profiling OFF 的 `tilexr-ep`/kernel 编译安装及链接安装包的
生产 chain consumer 编译通过；此次未运行 NPU，不代替用户 C3 回传或后续 C4 数据连接验收。
源码摘要、编译配置/日志、Host 输出及报告保存在开发工作区 `build/combine-c3-verification/`；
独立审查及集成配置复查未发现缺陷。

## Combine C2 历史交付与验收

C1（交付 `01c9b1d`）已收到用户通过确认。拉取本次 C2 提交后仍执行：

```bash
bash run_commit_validation.sh
```

C2 交付时入口选择 `ep-sdedup-combine-p1-empty-kernel`。本机需要 CANN 9.1.0、匹配 driver 及至少 64 AIV 的 A5，
加载现有 `validation.local.sh` 中的环境配置，无需 hostfile、多机同步或 HCCL Test。
设备默认为逻辑 0；可在环境配置或终端指定 `TILEXR_EP_SERVER_DEDUP_TEST_DEVICE`。
脚本重新构建、安装并核对产物，执行三项 C1 Host 回归、生产 launch 替身和真实 A5 注册/提交测试。
仅输出全部五项通过才产生 `COMMIT_VALIDATION_PASS`，Host 通过不会跳过设备测试。
报告位于脚本打印的 `run/validation/commit-*/summary.json`，失败时同时回传指向的日志。

真实 consumer 通过已安装库注册独立 Combine artifact，固定 64 核提交两个 dtype 的空 kernel，
同步后检查 GM 哨兵不变；不创建真实多卡 comm。公开接口和 comm 生命周期联调留给 C4/C5，
分离 profiling/trace 留给 C6。空 kernel 不生成 yOut 数值，不验证真实 Combine 通信或性能。
C2 已按用户回传登记通过；以下命令/依赖为 C2 历史范围，当前入口以本页 C3 为准。

2026-09-20 C2 开发验证：基于 `d073844` 加本次改动，blue 使用 CANN 9.1.0 / ccec clang 15.0.5（2026-05-18），
A5 `dav-c310-vec`、profiling OFF 构建 `tilexr-ep` 及独立 Combine kernel 通过。
通过正式 workflow 的 C2 production build，编译并安装五项 consumer，ELF、导出、RPATH 与真实 driver 依赖检查通过；
其中四项 Host 测试在该新安装包通过，额外 profiling ON 的 Host launch 替身通过（确认 C2 不占 Dispatch 记录）。
首次 Host 失败来自加载器替身漏列新 artifact，修正后重跑；手工安装首次遗漏 BUILD_TESTING=OFF，
随后按正式脚本的配置完成构建安装，不修改生产代码绕过这些检查。
开发环境未运行真实 A5 注册/提交/同步；随后用户回传 C2 检查点通过，报告路径和证据边界见进度文档。
完整源码摘要、编译/安装日志和 Host 输出保留在开发工作区 `build/combine-c2-verification/`。

## Combine C1 历史交付与验收

检查点为 `ep-sdedup-combine-p1-abi-host`。以下为 C1 交付时的入口；当前脚本已更新为 C3：

```bash
bash run_commit_validation.sh
```

脚本重新构建并运行 `combine_public_abi`、`combine_host_validation`、`combine_layout` 三项 C++ 测试；
打印最终 `COMMIT_VALIDATION_PASS/FAIL` 和 `run/validation/commit-*/summary.json` 路径。
用户已回传 `COMMIT_VALIDATION_PASS`，C1 通过；外部 summary/实际 SHA/环境明细未提供。
本点不需要设备、CANN、hostfile、HCCL Test 或同步到多节点；需要 Python 3.9+、CMake、Make、g++。
构建/运行期限、各点范围及结果登记规则见[七点验收](EP_MEMORY_SERVER_DEDUP_VALIDATION_FLOW.md#七点交付与外部验收门槛2026-09-20)。

C1 提交的生产入口不启动 kernel，校验后返回 `NOT_SUPPORT`；该提交的 Host PASS 表示符合 C1 契约。
实际空 kernel、注册/launch、三种算子模式和独立 trace 将分别在后续检查点交付。
C1 固定区容量校验与参数化 result/stage 地址助手不表示第二阶段 wire 格式已确定。

2026-09-20 开发验证：配置的 blue 环境使用 CANN 9.1.0、ccec clang 15.0.5（2026-05-18），
A5 `dav-c310-vec`、profiling OFF 构建 `tilexr-ep`（包含所属 kernel）通过，动态符号表包含新公开入口。
一键 C1 Host 三项通过；Dispatch 五项 ABI/Host/layout 回归和原 kernel launch 一项通过。
Python 新入口行为 5 项、batch 3 项、build 16 项、workflow 100 项通过。
本工作区没有 cmake/g++，本机一键执行正确失败并保留报告；成功的 C++ 检查是在 blue 的源码快照完成。
源码基线为 `4dd2fd7309949fc37a3fd74d503d3934b5271bb0` 加本次 C1 改动，
快照 SHA256、实际命令/工具链、构建退出码和原始日志保存在开发工作区 `build/combine-c1-verification/`。
首次编译的缺头文件问题、测试汇总漏传失败及一个非法输入用例均已修正，旧结果作废后重跑；
测试汇总必须检查累计断言失败再输出 PASS，不能单独添加成功 marker。
以上为 C1 开发验证、没有执行 NPU；随后用户明确回传通过，已据此启动 C2。


## 当前提交：完整 Dispatch（设计验证流程 §2.5）

2026-09-24 cnt GM 槽位升级：每条 cnt 起点间隔 512B，仅槽首 32B 有效，发送/接收 UB 保持紧凑。
每半区的 Dispatch/Combine 控制区为 1244KiB/96KiB，196KiB 留空后，共用数据从 1.5MiB 开始。
工作流 layout 标识更新为 `r13-count-gm512`；须配套重建并同步 Host、Dispatch/Combine kernel、consumer 和边界验证工具。
16 条的接收分配粒度继续保留；cumsum/ready 矩阵的 32B 单元及 prefix 副本大小不变。
下面旧布局的构建和设备记录保留为历史证据，不作为该升级已经上机验证的结论。

2026-09-20 共用通信布局：Dispatch 已使用实际 `peerMemBytes/2` 选窗，控制区固定
764KiB/52KiB，数据从 1MiB 开始；prefix 总区域固定预留 384KiB，副本继续按实际 E*4 紧凑排列。
Host 按每半区减 4MiB runtime 保留区检查，允许恰好容纳；不再强制 8GiB。
工作流 layout 标识为 `r12-shared-peer-buffer`，须配套重建并同步 Host、kernel 和 consumer。

验证基线为 `d122b3f` 加本次修改，记录位于 `build/shared-buffer-verification/`：blue 上
CANN 9.1.0 / ccec clang 15.0.5 的 A5 `dav-c310-vec` kernel 与 `tilexr-ep` Host 库编译通过
（profiling OFF）；默认配置 13 项 Server Dedup Host/替身测试及 1 项 launch 测试通过，
其余发送核 16/24/40/48 各 3 项布局/Count/Host 测试通过；consumer 集成 25 项、workflow
100 项通过。619 个生产、测试与构建输入摘要与 blue 实际编译快照一致。
测试覆盖 6GiB/10GiB 实际寻址、多轮双半区、prefix 空余及 Combine 控制区哨兵、
精确容量边界、少留一份 runtime 尾部、对齐及地址溢出拒绝。未运行 NPU，A5 多卡正确性与性能待验证。

布局维护：固定整表起点不应改变表内 stride 或复制长度；扩大 prefix 预留时同时固定后续
ready 起点。两份需求加一次 runtime 保留并不能保证第二半区安全，必须逐半区校验。
上述替身哨兵验证地址归属，不证明实机 DMA 顺序。

2026-09-20 五表迁移：设计提交 `3b0ba92` 确认 T02 后，Server Dedup Dispatch
公开接口及 kernel 改为 TokenType、DestinationIndex、RelayReadIndex、SourceMask、
expertScalesOut；旧 assistInfoForCombineOut/assistEx/group 参数已从该入口删除。
包头仍为 32B，四个有效字段为 source rank/token/k/type；8*K 索引段在源端生成，
第一跳按原始字节保存。本地类型先保存为 2，仅转发包头改为 3。必须一起重建并同步
Host、kernel 和 consumer，旧产物不能混用；普通 Memory API 不变。

验证证据保存在 `build/five-table-validation/`：CANN 9.1.0/A5 的 profiling OFF/ON
kernel 与 Host 库编译通过，安装后的公共头文件/生产库 consumer 编译链接通过；
相关 Host CTest 14/14、C++ consumer 集成替身 25/25、Python chain/workflow 113/113
通过。独立源码副本将编译期去重宏改为 0 后，发送/Host/launch 回归 6/6 通过；
默认源码保持宏为 1。替身覆盖新行距和相邻行保护、转发仅改 type、索引段字节保留、
故障注入及多轮复用。438 个生产/测试/构建文件摘要与 blue 默认宏源码快照一致。
没有运行 NPU；真实异步执行、量化计算、多卡正确性和性能仍待设备验证。

T02 校验在 NPU 批次完成后执行：各 Rank 从相同全局路由独立构造参考，比较本卡五表，
同时验证参考中的 relay 读地址与终端写地址及完整成员身份/重数匹配；沿用已有结果汇总，
要求同一构建/case 的全部 Rank 均通过。无需新增 Rank 间通信或交换实测表，旧 relay_links
及 group/prefix 判据已删除，不把缺失 Rank 或局部通过判为全局通过。

维护经验：紧密存放的 4B/12B 输出行须分别使用 32B 对齐的 UB staging 和精确字节数
写出，不能按 32B 覆盖邻行；角色 staging 必须与将被改写的 wire header 分离，
并与数据/转发共用 lane 完成边界。ABI 迁移应同步核对生产入口、调用方、launch 参数槽位、
独立预期和汇总判据；仅修正生产签名会留下错误断言或旧校验口径。

2026-09-18 编译开关调整：EP AICore kernel 将
`-mllvm -cce-aicore-record-overflow` 设为 `false`。该选项仅控制 AICore
overflow 检测代码生成，不改变用户 kernel 参数 ABI；CANN 9.1/A5 对当前
Server Dedup kernel 的 true/false 编译均为 120B 用户参数，关闭后主 kernel
代码段缩短 120B。此项未进行 NPU 性能结论，实际收益仍需在目标卡上对比验证。

去重开关使用固定构建参数：`bash build_all.sh dedup=0` 编译关闭版本，默认 `dedup=1`。
它与 `breakdown=0|1` 独立，可以组合为 `bash build_all.sh dedup=0 breakdown=1`。
普通 `if/else` 根据数值宏选择差异部分，不通过run.sh或公开API在运行时切换。
关闭模式按Server筛选原始路由并均衡分核，使用三字段包尾和独立M6；表1/3/4无效，表2/5有效。
完整约束见[关闭去重特性](specs/ep-memory-server-dedup-disabled-mode.md)。

构建目录分别记录dedup与profiling，安装仍是一套配套产物。manifest记录
`server_dedup_enabled`、`server_dedup_protocol=2`及原有产物摘要；同步/运行读取安装包模式，
核对各Rank记录。切换模式必须整体重建并同步Host、kernel和consumer；旧manifest需重建。

下面是旧临时关闭实现的历史验证记录，不作为当前三字段包尾/M6实现的编译或设备验证证据：

本次 review 修正了关闭宏后被 workflow 错判的问题，以及 launch 单测仍期待旧发送核数字节的
断言。launch 单测使用二进制加载/注册替身，负向用例会故意输出 registration failed；
不能据此认定缺少安装产物，必须检查后续 FAILED 断言及最终退出码。
验证记录见 `build/server-dedup-compile-switch-review/`：完整 EP CTest 27/27、C++ consumer
Host 集成 21 项、Python workflow/measurement/chain/runtime/summary/build 共 168 项通过。
头文件宏为 0 的发送/Host/launch 回归 6/6，通过宏值 1 和 0 的 CANN 9.1/A5 kernel/Host
编译；411 个生产与验证源码文件的摘要和 blue 默认宏快照一致。未运行 NPU，性能收益须在
用户 A5 环境实测。

2026-09-17 cnt 发送掩盖方案 A：见 [cnt 发送掩盖优化方案](specs/ep-memory-server-dedup-count-send-overlap.md)。
cnt 发送源独立保留，筛选 scratch 与 sender/recv-count 共享，SendCnt 不再排空 MTE3 或 Reset。
receiver 在等待跨核局部和前计算相对前缀，局部和源的等待延后到真正复用位置。
当前 `recv_cnt` 包含局部前缀，`cumsum` 包含跨核等待和全局偏移修正，比较时使用两者合计及 kernel 总耗时。
新 `send_cnt_issue`（`0x53000220/21`）只记录依赖发布和下发；旧 `send_cnt_send` 保持历史含等待语义，不能混用两者计算通信提速。
本次没有运行 NPU；A5 收益和全核 cnt + FP8 正确性仍需设备验证。

当前检查点为 `ep-sdedup-p2-dispatch`：默认 32 核执行第一跳及循环 DIRECT，另外 32 核完成 cnt/cumsum 后接收、转发并写最终 expert 行及三张接收侧表；核末完成本核写出及当前窗口的数据 Flag 清理后独立返回，不再执行 SyncAll；输出使用 Token 级 prefix 行序，终端记录逻辑 slot，不执行 AtomicAdd 或 counter 清零；不再发布/等待跨 Rank 结束握手。Combine 数据面尚未实现（C1 Host 接口已接入）。完整 Dispatch 的设备验收使用下面的多卡运行流程，`EP_SERVER_DEDUP_ABI_HOST_PASS test=receive_packet` 只表示 Host 替身测试通过。

默认发送/接收核数为 32/32，Count 全核参与阈值为 512：BS<512 时 64 核统计并发送 cnt，
BS>=512 时仅接收核承担该步骤。阈值和收发核比例均为编译期配置；
修改后须同时重建并同步 Host 与 kernel，Host UB 预算和核内 Count 分支必须使用相同阈值。
本次基于 `689d12b` 验证：CANN 9.1/A5 kernel 与 Host 库 OFF/ON 编译通过，7 项 Host 回归通过；
Count 在五种分核下覆盖 BS511/512/513，实测约 126 秒，测试时限由 120 秒调整为 240 秒。
日志及源码摘要见 `build/count-threshold-512-verification/`；未运行 NPU，不据此宣称实机性能或正确性。

2026-09-17 运行时分核验证记录（历史实现，现已回退为编译期配置）：blue CANN 9.1.0 / Bisheng
Clang 15.0.5 的 A5 kernel 与 `tilexr-ep` Host 库 profiling OFF/ON 均编译链接通过。
该记录仅用于追溯运行时分核版本，不代表当前接口。
集成替身 23 项中 22 项通过；`test_repeated_calls_reject_wrong_bank_readback` 失败：
注入错误窗口返回读错误 92，用例却期待退出 0 的结果校验失败记录。在未修改的 `fa099d1`
上单独复现相同失败，因此登记为基线问题，不在本次分核变更中调整。验证快照、编译参数、
日志及基线对照保存在 `build/runtime-core-split-verification/`。本次没有运行 NPU；
五种分核的实机异步正确性、连续多轮及性能仍须 A5 验证，不能由 Host 回归或编译推断。

2026-09-17 核末 SyncAll 移除：保留本核流水收尾和 Flag 清理，新 trace 不再产生 `finish_sync`，
旧记录仍可解析。基于 `9f3a1ab` 加本次修改，blue 的 CANN 9.1.0 / Bisheng Clang 15.0.5
完成 A5 `dav-c310-vec` kernel 的 profiling OFF/ON 编译链接；本地 21 项 trace 回归通过，
独立并发审查未发现需修正的问题。源码摘要、配置、编译器版本、返回码和日志保存在
`build/finish-sync-verification/`。本次未运行 A5 多卡测试；连续多轮双窗口复用、空任务核及
负载倾斜场景仍待设备验证，性能比较应使用 msprof 整个 kernel 时长，不以各核结束打点的
均值/中位数下降推断整体收益。

2026-09-16 编号契约更新：第一跳 `t=row`，终端继承 relay 的 t；`groupOut` 按
`C=R*BS*min(K,L)` 行重新分配。原 `t<8*BS` 和任意密排编号不再有效；consumer 验证第一跳
`t==row`，跨 Rank relay_links 继续验证所有终端关联。工作流要求 `prefix_indices_validated`，
不再要求已取消的 `counter_cleared`。清单 layout 为 `r07-token-prefix`，旧安装须重新 build/sync。
新增用例用 `BS=2/R=16/L=2/K=2` 构造第一跳 row 32..47（超过旧上限 16），终端位于 row 0..15，
覆盖接收核逆序执行、三轮 magic、旧 counter 保持污染值及未写 group 空洞保护。
当前仍只验收 Dispatch，旧 Combine relay 预留区未扩容；新编号不能直接用于该区。

2026-09-14：用户确认完整 Dispatch 在 **16P、64P 的单轮和多轮功能测试均通过**。按最近交付的 [8bc2348](https://github.com/LingquLab/TileXR/commit/8bc2348471614c97b7d020aed65a70885aa3e263) 登记；本次回传未附原始 summary、kernel SHA 或编译版本，尚未独立核对产物来源。该结果覆盖已运行配置的完整 Dispatch 功能及多轮复用；专家数、BS/H/TopK、dtype/量化格式和具体轮次未回传，不沿用此前 source-send 的量化覆盖，也不据此登记完整安全矩阵、128P、breakdown v5、性能目标或 Combine 通过。

固定基线为 **16P、两台各 8 卡 A5、单卡 6 专家（E=96）、BS96/H7168/TopK6/BF16/balance_shift/seed2026**。每 rank 申请 **8 GiB 通信域**（8188 MiB 数据 + 4 MiB runtime Flag），底层另有 1 MiB 地址偏移余量。两个固定 4 GiB 窗口由 `magic % 2` 选择；每窗前 2 MiB 是 cnt/cumsum/prefix/counter，后续 4090 MiB 可用于 Token 区，最后 4 MiB 保留。第一窗尾部空置，第二窗尾部归 runtime；任何 shape 均不得改变窗跨度或越过单窗上限。总专家数 `0<E<=1024` 且能被 rank 数整除。

每个发送核先发第一跳，再从本 server 开始绕回 +1 发送 DIRECT；64P server7 的顺序为 `7→0→1→2→3→4→5→6`，空目标跳过。同 server 内保留原 Token/k 顺序和缓存 q；各核独立推进，不增加 server 边界同步或 MTE3 排空。

接收以完整前缀确定连续输出行，在本次接收核组按 floor 分区；tokenPos/pendingIdx 均只存 UB。每批独立探测最多 16 个 Token 的全部块、全部 8 个 Flag word，记录稳定索引，按原 pending 位置逆序消费就绪项并末项覆盖删除。整批未就绪时继续扫描；有删除则重查批起点的换入项。

`recv_tasks` 表构造从每核的首个有效记录开始：对非递减的 inclusive prefix 二分查找首项 `prefix[record] > begin`，随后遍历到本核 `end`。从第 0 项重复扫描会让后部核即使拥有相同任务数也产生准备拖尾。必须使用严格大于跳过零计数平台，并取原始 `prefix[record-1]`（record=0 时为 0）计算 `q=row-previous`；不能以 begin 代替记录起点，否则跨核切分同一 source/expert 时会重用错误槽位。空分区直接返回；UB 双表、pending 稳定下标和核末清理所有权保持原契约。

2026-09-15 此优化的本地 counts、receive_packet、receive_profile 三项 Host 替身回归通过，新增前导零、相等前缀、区间内部起点、全零/不足 16 行及 1024 总专家的槽位对照。已尝试通过配置的 blue SSH 连接进行 A5 编译，但连接返回 255 且无远端输出，未进入 CANN 编译；日志保存在本地 `build/recv-tasks-verification/`。本地测试不证明 A5 性能收益；用户复测时保持相同配置，对比 48～63 核的 recv_tasks 及完整 kernel 耗时。

每路只分配一份 T 字节带 Flag 包、32B metaUB 和对齐 group 行，数据池按扣除前缀、双表及 `512*blockCount+832+sumTmpBytes` 字节监听区（`sumTmpBytes=16*align32(ceil(blockCount/8))*4+256`，对齐单位为 float 元素）后的剩余 190KiB 预算选择 1～8 路。`MTE3_MTE2` 保护复用，`MTE2_MTE3` 发布搬入，`MTE2_S/S_MTE3` 保护 Scalar 元数据访问。hidden/scale 用 DataCopyPad 跳过 Flag 直接写 GM，同一 Token 的转发连续读取这一份包；完成事件在最后一笔写出后记录，下一次复用 lane 才等待。核末回收 lane 后统一清理本核稳定 tokenPos 的所有 Flag。

第一跳先生成保留正 forwardCount 的独立 assistEx/group，再把公共包改为终端角色；终端按 expert 匹配原 k/weight。权重按位复制，group 按 `t*K+originalK` 保存返回槽位，t 由并发取号产生，不预设其顺序。完整元数据关联由所有 rank 结果汇总后验收。

functional/performance/breakdown 都支持 `loops>=1、warmup>=0`，默认 1/0。调用复用同一 communicator/stream/输入输出，连续提交 warmup+loops 轮，整批末尾只同步一次并执行 Host barrier。各轮接收、转发及 Flag 清理在本卡 kernel 退出前完成；最终按最后一次实际 magic 选择窗口读取输出/Flag/counter，不再读取完成代次，不保存每轮输出快照。

实现维护约束：CompleteCumsum 的计数写出可能仍引用 UB，必须 PIPE_ALL 后再 Reset，并用 `windowOffset+cntOffset+prefixOffset+receiver*prefixStride` 重载 GM 前缀。尾部是逻辑偏移，字段位置必须逐项映射为 `p/480*512+p%480`，不能假定跨 Flag 的 entries 连续。DataCopyPad 完整块源 stride 为 1 个 32B、GM 目标 stride 为 0 字节；group 的字节 stride 用于 uint32 Tensor 时除以 4。批次稳定索引快照不能随着 pending 删除重新读取身份，pending 删除也不表示 lane 已释放。

本地接收测试覆盖量化/非量化字节布局、K1/6/32、24 项任务跨满批/尾批、整批未到达后继续推进、1/3/8 路复用、packet/meta/group 在途只读、空接收、同卡/跨卡转发及其他核槽位保护。Host 模型和地址空间检查不能证明真实 CANN API、异步设备流水或 NPU 性能；设备功能结果以上述用户回传记录为准，尚未取得实际编译版本及性能数据。标准打点迁移的 blue 编译与本地验证证据见下文。

生产多卡执行需要 CANN 9.1、匹配 driver（25.1.rc1 或更新）及每卡至少 64 AIV 的 Ascend910A5/Ascend950。复用相同环境已保留的 HCCL Test 资格记录，不因本次构建重复运行；环境规则见 `AGENTS.md`。

开发者完成 Server Dedup 代码修改后，先尝试在已配置的 **blue 编译环境**使用真实 CANN 9.1 编译，再交付用户。Server Dedup **只编译 A5**：现有 EP CMake 固定 `dav-c310-vec` / `CATLASS_ARCH=3510`，无需增加 SoC 参数，也不由 blue 实际安装的 NPU 型号选择其他版本。纯 kernel 修改只构建对应目标；打点相关修改分别检查 OFF/ON。先加载 blue 已配置的 CANN 9.1 环境，在待验源码目录执行：

```bash
source scripts/common_env.sh
cmake -S . -B build/server-dedup-a5-off -DTILEXR_BUILD_EP=ON \
  -DTILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING=OFF
cmake --build build/server-dedup-a5-off --target tilexr_ep_dispatch_memory_server_dedup_kernel -j8
cmake -S . -B build/server-dedup-a5-on -DTILEXR_BUILD_EP=ON \
  -DTILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING=ON
cmake --build build/server-dedup-a5-on --target tilexr_ep_dispatch_memory_server_dedup_kernel -j8
```

涉及 Host/launch 修改时还需构建 `tilexr-ep`。保留待验源码版本、编译器版本、配置、返回码和编译日志；核对编译命令确实包含 A5 目标。blue 连接信息缺失、不可达或工具链不完整时，说明具体阻塞；本地替身或 Host Clang 通过不记为 CANN 编译通过。blue 编译不作为设备功能/性能通过证据，用户侧仍按下文手动编译、同步、运行三个脚本验证。

`command -v ccec` 失败或默认 `/usr/local/Ascend/ascend-toolkit` 无访问权限，只能说明当前搜索路径不可用，不能据此判定本机未安装 CANN。继续检查可访问的共享工具链和用户安装目录，读取安装版本，并实际执行 `bisheng --version` / `ccec --version`；通过 `TILEXR_CANN_ENV_SCRIPT` 指定对应 `set_env.sh` 后再配置独立构建目录。使用本机交叉编译时仍须核对 A5 编译参数，并单独记录设备运行尚未覆盖。

## 手动编译、同步、运行

### 比较 Send/Recv 核数配比

核数是编译期配置，总 AIV 数保持 64。可以独立配置两个算子，无需修改头文件：

```bash
bash build_all.sh dispatch_send_cores=28 combine_send_cores=28
bash sync.sh
bash run.sh -m 1 -n 4 -e 384 -l 100 50 bs=128 topk=6 h=7168 route_mode=balance_shift operator_mode=combine quant_mode=4 fp8_format=e4m3
```

Dispatch send 支持 `16..48`、Combine send 支持 `16..56`，均以 4 核为步长；
Recv 自动取 `64-send`。例如 `28/36` 表示 28 个 send 核和 36 个 recv 核。
下一组比较时重新构建并同步，例如 `dispatch_send_cores=32 combine_send_cores=40`。
breakdown 构建可同时传 `breakdown=1`，对应运行模式 `-m 2`。不要把核数作为 `run.sh`
参数：改变运行时声明不能改变已经编译的 kernel。

未传核数时继续使用两个共享头文件的默认值（上库默认均为 32），也兼容直接修改头文件。
构建先编译、执行 `tools/validation/core_config.cpp`，取得实际宏值，再把同一组定义传给
Host、A5 kernel 和测试 consumer 的 CMake 构建。直接使用 CMake 时，对两个构建目录传入相同的
`TILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM` / `TILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM`。

安装清单的 `server_dedup_core_config` 保存生效核数，Combine contract 同步记录 send 核数；
运行时以已安装包为准加载配置，结果首行及 `summary.json` 记录两组配比。
缺少该字段的旧包必须重新构建和同步；不要手动修改 manifest 绕过产物检查。

维护注意：只改 C++ 宏而仍用 Python 的 32 核常量，会把合法 `24/40` 配置报告的
`combine_control_publish_bytes=26624` 与旧预期 `24576` 比较，产生
`Combine traffic mismatch: control_publish_bytes`，且可能显示“执行阶段失败 / ranks=0”。
字节数校验和 breakdown 核角色解析必须使用清单中的实际核数，不能关闭检查或伪造报告值。
Host/CPU 回归只能证明配置与验证契约；不同配比的数值正确性和性能仍需 A5 多 rank 实测。

Dispatch Count 的 recv 分工按 cnt 记录总数做连续商余数划分，不再把每核记录数向上补齐到 16。
每条 cnt 仍占独立 512B GM 槽，因此任意记录边界都保留独占的 512B 轮询/清理范围。
例如 E=384、recv=36 时，24 核各处理 11 条，12 核各处理 10 条；有工作量的核全部参与 cumsum。
注意区分任务划分与 UB 对齐：cumsum 的两个向量发布/清理缓冲仍按 8 条 32B 记录补齐，
以覆盖完整向量 repeat；GM 矩阵步长、通知数量和等待条件必须继续使用真实核数。
仅放宽核数宏会使 36 核的 1152B 缓冲被 1280B 向量写越界。回归需覆盖新增的
20/28/36/44 核 Dispatch 配置、Combine 52 核、空任务、非整除 cnt 分段及 BS512 两侧路径。

首次部署时准备环境文件与主机清单；已有文件只检查，不覆盖：

```bash
git submodule update --init --recursive
test -f validation.local.sh || cp validation.local.sh.example validation.local.sh
test -f hostfile || cp hostfile.example hostfile
```

`validation.local.sh` 只保存 CANN/driver、Python/msprof、SSH 用户和 hostfile 等环境配置。
主节点地址默认取 hostfile 首项（必须属于当前执行机器），通信端口为 10177，barrier 端口为 10274。
文件中遗留的 `TILEXR_COMM_ID` / `TILEXR_DEMO_BARRIER_ADDR` 不再生效，避免配置同步后指向旧主节点；
需要覆盖时使用 `run.sh comm_id=IP:PORT barrier_addr=IP:PORT`，只指定 comm_id 时 barrier 默认同 IP、端口 +97。
控制端将最终地址传给全部远端，远端不能各自使用本机 IP。切换控制端仍须将本机通信 IP 放在 hostfile 第一项，
多网卡机器使用清单中的网卡地址。`hostfile` 每行只填写一个 IPv4；每台固定使用 device 0..7，允许空行和注释。
脚本向其余节点同一绝对路径分发，目标必须为空或有 `.tilexr-validation-root` 的受管理目录。

按顺序逐条执行，每步成功后再执行下一步：

```bash
bash build_all.sh
```

```bash
bash sync.sh
```

```bash
bash run.sh -n 2 -e 96 -m 0 -l 1 0
```

构建应输出 `BUILD_PASS`，同步输出 `SYNC_PASS`，运行输出 `VALIDATION_PASS`。三个脚本各自负责一个步骤；`run_commit_validation.sh` 已按 Combine 七点流程恢复，目前一键验收 Combine C5 连续多轮，不代替这里的 Dispatch 多卡流程。

`build_all.sh` 默认关闭设备打点，普通包包含完整 Dispatch/Combine；旧 `TILEXR_VALIDATION_CHECKPOINT` 已弃用并忽略，当前安装目录与流程见本页首节。内部 kernel launch ABI 为 120B；去重开关不进入 launch 参数，修改宏后须同步重建 kernel、Host 库和 consumer。

单轮通过后，用同一产物检查完整 Dispatch 多轮复用：

```bash
bash run.sh -n 2 -e 96 -m 0 -l 3 0
```

再手动测量完整 kernel：

```bash
bash run.sh -n 2 -e 96 -m 1 -l 100 10
```

每 rank 检查 msprof 恰好采到 warmup+loops 个目标 kernel 样本；剔除预热后保留全部 loops 个原始样本，模式 1 的最终统计只取每卡最后 `N=min(loops,50)` 个测量样本。每台 server 固定 8 卡，server 代表耗时是这 `8×N` 个样本的算术平均；最终输出各 server 代表耗时的 min/mean/max（单位 us）。例如 `-l 100 50` 采集 150 次，仅最后 50 个测量轮次参与统计，每台 server 共 400 个样本；`-l 20 50` 每台只统计 8×20 个测量样本，不用 warmup 补足 50 轮。

测量范围为完整 Dispatch，包含 cnt/cumsum、源端发送、接收转发、输出、清理和轮末完成等待。有效带宽使用每次调用的第一跳/DIRECT/relay 完整 T（含本卡拷贝）及远端 cnt 记录量，排除 polling 和完成代次元数据，按 server 汇总 8 卡每次调用的字节数，再除以上述 server 平均 kernel 耗时，得到各 server 的有效 GB/s，最终也输出跨 server min/mean/max。该值是 server 聚合吞吐，不能和旧逐卡带宽直接比较；不把流量乘 loops 后除以单次延迟。原始样本和 kernel SHA256 随结果保留，此值不是纯网络带宽。

缩小统计窗口不等于放宽采集完整性：仍验证所有 warmup+loops 样本的数量、合法耗时和唯一开始时间；缺卡、缺轮或损坏的早期样本不能借“只看最后 50 轮”绕过验证。此统计转换由 Host Python 完成，替身回归只证明解析与汇总，不证明 NPU 正确性或性能。

sendData 细打点按当前事件定义解析；recv_data 包括接收准备、等待、输出、转发与本核 Flag 清理，finish 只包含本核收尾，不再包含全核或跨 Rank 屏障。

## 按需执行 Host/Python 回归

在当前机器已有 `build_all.sh` 安装产物后，独立执行：

```bash
bash run_host_validation.sh
```

`run.sh` 默认 `host_validation=0`，跳过 Host/Python 回归；显式开启时先在主节点调用同一入口，
通过后再启动 NPU 工作流，失败则保留退出码并停止：

```bash
bash run.sh -n 2 -e 96 -m 0 host_validation=1
```

该参数只接受 `0` 或 `1`。开启后 Host 总日志为本次 case 目录下的 `host-validation.log`，
逐项日志和报告在该目录的 `host-<id>/` 中，不计入后续 NPU 工作流的执行期限。

此入口检查安装 manifest、CPU 架构和产物摘要，随后执行其中的 Host C++ 与 Python 测试，
排除需要真实设备的 registration 测试。无需 hostfile、sync 凭据、SSH、通信地址或 CANN 环境加载；
不会启动 NPU rank。默认 production 安装可直接运行；若安装的是 host 模式产物，调用时保持
`TILEXR_VALIDATION_BUILD_MODE=host`。不接受运行用例参数，单项测试期限为 180 秒。

成功输出 `HOST_VALIDATION_PASS`；非零退出、FAIL marker、缺失或重复 marker、超时均使回归失败，
空测试集也不会记为通过。每次结果保存到 `run/validation/host-<id>/summary.json`，同目录保留每项原始日志；
设置 `TILEXR_VALIDATION_RESULT_ROOT` 可改变结果根目录。报告记录 build_id、已完成测试及首个失败，
测试进程退出或中断时清理其所属进程组。该结果只覆盖本机安装包，不代表跨机通信或 NPU 通过。

Host/Python 回归保护协议/内存布局、公共 ABI、Host 参数和 launch 包装、Planner、日志和性能解析工具等契约。
建议在修改这些模块后或完整交付验收时执行；纯 NPU 参数调试不必每次重复。
`build_all.sh` 继续编译安装测试并登记完整清单，不新增强制执行步骤。
Server Dedup 的 functional/performance/breakdown 默认均不执行这套回归，也不要求已有 Host PASS 报告。
`run.sh` 仍检查各节点安装包、实际 NPU 初始化/建链、rank 结果、正确性和对应模式的测量数据。

维护边界：原流程在每个节点启动 NPU rank 前串行执行完整 Host/Python 清单，增加了启动等待。
安装清单与本次运行的必需结果集合必须分开维护：NPU 报告不再声称 ABI/Host 回归通过，
也不为跳过的测试伪造 PASS；节点执行与结果汇总必须使用同一测试选择逻辑。
历史显式 `ep-sdedup-p1-abi-host` 检查点仍保留原 Host 验证行为；empty-kernel 节点仅保留真实设备注册检查，
其 Host 替身测试移至独立入口。memory-boundary 安装包原本不含 Host 测试，不能用于该入口。

容量迁移维护：Dispatch 改用 8 GiB 通信域后，C++ consumer 上报 8589934592，而 Python 仍期待 4 GiB，
会在设备程序成功退出后误报 RESULT_FAILED，继而取消其他 rank。容量必须按 checkpoint 验收：
Dispatch 为 8 GiB，独立 memory-boundary 为 4 GiB；同步更新合法记录夹具并覆盖旧容量被拒绝。
容量失败单独记录 expected_bytes/actual_bytes，避免与 launch 轮数或 cleanup 错误混淆。
本地验证仅覆盖 Python 判据和脚本编排，不能替代 NPU 全 rank 正确性验证。
2026-09-15：37 项针对性回归通过（chain 9、runtime_configuration 15、workflow 集成 9、结果协议 4），
并通过 Bash 语法与 Python 编译检查。集成使用模拟 rank/传输；未执行真实 SSH 集群或 NPU 验证。

## 整服务器调度与参数归属

需要清除历史日志和 trace 时，在首节点的当前测试目录执行：

```bash
bash clean_run.sh --dry-run       # 可选：查看各节点目标和顶层条目数
bash clean_run.sh                 # 清理 hostfile 全部节点，包括本机
# bash clean_run.sh hostfile=cluster/hosts   # 可选：覆盖清单
bash sync.sh                     # 清理同步回执后，重新同步再运行测试
```

清理范围固定为脚本所在部署目录的 `run/` 内容，包括隐藏文件、日志、trace 和
`run/validation/sync.json`，保留 `run/` 本身，不使用 `TILEXR_VALIDATION_RESULT_ROOT` 扩大范围。
沿用 `validation.local.sh` 的 hostfile、SSH 用户和本机 Python 配置；各远端路径与首节点相同，
通过 SSH 标准输入发送清理逻辑，远端只需 Python 3，不要求先同步新脚本或加载 CANN。
清理按节点报告结果，任一节点失败则返回非零；已经成功清理的节点不会回滚。

使用前停止测试。脚本拒绝非受管理目录（本机 Git checkout 也可使用）、符号链接形式的
部署路径或 `run/`，并检查 `run/` 内的 `.operation.lock` / `node.lock`；发现锁时保留该节点全部内容。
目录内的符号链接只解除链接，不删除链接目标。维护时不能用 `run/*` 遗漏隐藏条目，
也不能解析链接后对外部目录递归删除。任务锁检查不是与启动器共享的原子互斥，不支持边跑边清。
这些边界由临时部署目录和模拟 SSH 的行为测试覆盖；真实集群清理不作为自动测试执行。

同一 A5 产物同步到 8 台服务器后，可以连续运行 16P/32P/48P/64P 和不同专家数。
`hostfile` 是分发清单；`sync.sh` 使用完整清单，`run.sh -n N` 只选择前 N 台。
每台固定使用 0..7 卡，按清单顺序分配连续 rank。未选中节点不执行本次用例。

当前生产构建仅维护一个安装目录，`breakdown=0/1` 更新同一份产物；同步不选择打点模式，
只分发该目录及 manifest。运行前检查 `server_dedup_profiling`：`-m 2` 要求开启，`-m 0/1`
要求关闭，不匹配时在启动任何 NPU 进程前失败并提示重建/同步。每次重建清除旧 manifest
和同步凭据，构建失败不能回退到旧产物。历史带模式后缀的目录不参与当前生产包选择。
子 shell 的 export 不会影响后续独立脚本，不能再依赖构建脚本的环境变量传递来选同步目录。
Host 回归覆盖独立 Bash 调用、模式切换、同步清单及错误模式在启动前被拒绝；不替代设备验证。

`sync.sh` 保留跨主机并发，只增量更新安装产物、验证/性能解析工具和环境加载脚本，
不删除远端安装目录或退休文件。Host 检查点还携带 Python 测试及其现场编译所需的源码。
`hostfile` 只在首节点读取，远端运行通过 SSH 参数接收清单，不再同步该文件。
`validation.local.sh` 仍需同步：远端依赖它加载 CANN、driver、Python 等环境；
首节点不存在此文件时保留远端已有配置。rsync 保留 checksum，避免仅按时间戳漏掉内容变化。
远端校验以当前 manifest 为准，允许保留旧文件，但当前清单内产物缺失或哈希错误仍拒绝运行；
保留的旧文件不代表当前安装包，也不允许据此运行旧测试。`--debug` 使用同一运行依赖清单。

同步出现 `Broken pipe` / exit 255 时，在同一用户、容器和目录运行
`bash sync.sh --debug`。它按有效 hostfile 顺序逐个检查所有 IP（包括首项），
分别执行带 `ssh -vvv` 的远端工具探测和 `rsync --dry-run`，失败后继续检查其他节点。
日志位于 `run/validation/sync-debug-<id>/<零起始节点索引>/`，`summary.json`
记录目标用户/IP、各阶段状态、错误与日志路径；存在失败节点时退出非零。
该模式只写本地诊断日志/文件清单，不执行远端 prepare、安装目录清理、实际文件传输，
也不更新 `.tilexr-source.json` 或 `sync.json`；有安装产物时纳入扫描，无需先构建。
短 SSH 命令成功不能证明 rsync 连接正常；dry-run 成功也不能证明实际数据传输、
远端写权限或并发同步稳定。`Broken pipe` 本身只证明连接中断，根因仍需结合失败阶段
和远端 SSH 日志定位，不能直接归因于超时或 Kernel 代码。

```bash
bash run.sh -n 2 -e 96 -m 0 -l 3 0
bash run.sh -n 4 -e 192 -m 0 -l 3 0
bash run.sh -n 6 -e 288 -m 0 -l 3 0
bash run.sh -n 8 -e 384 -m 1 -l 100 10
bash run.sh -n 8 -e 512 -m 0 -l 3 0
```

| 参数 | 含义 | 默认值/约束 |
| --- | --- | --- |
| `-n N` | 服务器数，总卡数为 8N | 必填，2..16，清单必须有足够机器 |
| `-e E` | 总专家数，单卡专家数为 E/(8N) | 必填，1..1024，须被 8N 整除 |
| `-m M` | 0=functional、1=performance、2=breakdown | 默认 0 |
| `-l L W` | 测量 L 轮、预热 W 轮 | 默认 1 0；L>=1、W>=0，均为 uint32 |
| `case_analysis=0\|1` | NPU 阶段结束后分析各 rank 的跨 server token 去重率 | 默认 0，显式传 1 开启 |
| `route_mode=random\|balance\|balance_shift` | 本次输入的专家生成模式 | 默认 balance_shift |
| `seed=N` | 路由和权重生成的随机种子 | 默认 2026，十进制 uint32（0..4294967295，无前导零） |

`bs=... h=... topk=... dtype=... quant_mode=... fp8_format=... block_dim=...`
继续作为可选覆盖项，默认 BS96/H7168/TopK6/BF16/非量化/auto。每 rank 通信域固定
8 GiB，分为两个 4 GiB 窗口。
`comm_id=... barrier_addr=... msprof=... hostfile=...` 可覆盖本次部署配置；
相对 hostfile 路径相对于仓库根目录。`-h`/`--help` 不加载 CANN。
`run_case.sh` 仅兼容转交到 `run.sh`，`run_batch.sh` 统一调用同一入口。
旧 `run_mode=... loops=... warmup=... moe_expert_num=...` 改用上述短参数。

三种路由模式都保持 `单卡专家数 × 卡数 = 总专家数`，专家 ID 到目标卡的映射为
`expert_id / 单卡专家数`，每个 token 的 TopK 专家互不重复：

- `random`：逐 token 随机选取 TopK 个专家，不保证各专家负载相同。
- `balance`：按目标 rank 交错排列专家并循环分配；完整周期严格均匀，尾部不完整时
  专家路由数最多相差 1。专家列表不受 seed 影响，路由权重仍使用 seed。
- `balance_shift`：每个 epoch 打乱专家顺序后均匀分配；要求全局 token 数可被
  `E/gcd(E,TopK)` 整除。该约束不应用于 random 或 balance。

例如 128P、每卡 BS256、每卡 3 专家（总 E384）、H7168、TopK6、BF16 非量化：

```bash
bash run.sh -n 16 -e 384 -m 0 -l 1 0 \
  bs=256 h=7168 topk=6 dtype=bf16 quant_mode=0 route_mode=balance seed=42
```

将 `route_mode` 改为 `random` 或 `balance_shift` 即可。首次升级须重建并同步包含新参数
解析的 C++ 测试程序，此后切换模式或种子无需重新编译。seed 同时控制随机路由和权重，
默认 2026 保持历史输入；多轮使用相同输入。实际 mode/seed 随本地和远端每个 rank
的 marker、summary 和专家列表导出保存并校验，错误或陈旧的模式/种子不能作为通过结果。

维护要点：生成器的 `moeRankNum` 表示部署专家的卡数，必须传 `rankSize`，不能传
`E/rankSize`。旧 shuffle 路径没有使用该字段，直接替换生成函数会暴露错误的专家排序。
输入、Count 参考、Dispatch 参考和 stub 应共用模式选择入口；Count 必须累计实际路由，
不能以均匀分布假设校验 random。脚本/Host 替身回归不证明 128P A5 数据面正确性。
仅对 Dispatch 应用专家整除和 epoch 校验；不生成路由的 memory-boundary 检查点保持豁免。

2026-09-18 验证：本地 139 项工作流、参数、环境回归通过；blue 的 C++14 专家生成器测试
和 25 项 C++ consumer 替身集成回归通过，包括三种模式、seed 两端边界、重复生成、
每卡三专家、部分 epoch、Count 损坏检测及 memory-boundary 豁免。consumer 另使用真实
CANN 9.1 头文件完成 Host 对象编译；没有运行 NPU，128P 数据面与性能仍需目标环境验证。
源码摘要与日志保留在 `build/route-mode-seed-verification/`。

发送/接收核数由 `src/ep/common/ep_memory_server_dedup_count_layout.h` 中的
`TILEXR_EP_SERVER_DEDUP_SEND_CORE_NUM` 编译期宏配置，默认 32（即 32/32），支持
16/24/32/40/48。修改后须同时重建并同步 Host 与 kernel；通信布局和容量按该宏生成。
核内直接使用编译期常量，不保留运行时分支或外部参数。
BS/H/TopK 组合不兼容。每种分核应先做 functional 多轮测试，再按同 shape 比较性能。

例如 `bash run.sh -n 2 -e 96 -m 1 -l 100 10 case_analysis=1`。
此开关适用于完整 Dispatch 的 functional、performance 和 breakdown 模式。
只有所有节点运行进程退出、结果及输入文件收集完成后，主节点才读取本轮留存的
`inputs/rank-<rank>-ids.bin`（远端位于 `nodes/<host_index>/inputs/`）进行统计；
不在 NPU 运行阶段计算，不增加设备打点，也不重建一份假定的 balance_shift 路由。

对每个源 rank，按当前固定 8 rank/server 的映射统计：

- A（`before_tokens`）：每个 token 的 TopK 专家中，属于其他 server 的路由条数之和。
- B（`after_tokens`）：每个 token 命中的不同远端 server 数之和。同一 token 发往同一远端 server
  计一次；不同 token 不能合并，本 server 内跨卡发送与转发也不计入 A/B。
- 去重率（`dedup_percent`）：`100 * (A-B) / A`。A=0 时定义为 0%，仍参与 rank 汇总。
  若一个 token 的 A 条远端路由合并为 1 条，去重率为 `100 * (1-1/A)`，而 `1/A` 是剩余比例。

终端追加 `dedup(%) min/max/avg=.../.../...`，分别为所有参与 rank 的最小值、最大值和
算术平均值；avg 不是先汇总全局 A/B 再相除，也不按服务器或测量轮次平均。
本用例多轮复用相同输入，因此输入路由只统计一次。`summary.json.case_analysis` 保留三个
百分数、每 rank 的 A/B/去重率、输入文件摘要和零跨 server 流量的 rank 数。
关闭时不读取专家列表进行分析，也不输出这些统计。

维护要点：分析必须放在所有节点运行及收集完成之后，不能在某个 rank 提前退出时开始，
以免 CPU 统计与其他卡的 NPU 测量重叠。缺失完整 rank 完成记录、输入缺失/长度错误或专家 ID
越界时报告分析失败，不用部分 rank 或推算数据填补；已完成的 NPU 测量结果仍保留。
本次 36 项分析/配置/摘要定向测试及 104 项工作流/环境回归通过，包含较晚结束的 rank、
远端输入收集和输入缺失场景；使用 Host 与模拟多机流程，未运行真实 NPU。

参数每次从默认值和命令行重新解析，不继承终端遗留的 shape、模式、轮次或机器选择。
`-l` 缺少第二个值、重复参数、未知参数或非法整数在加载运行环境前报错。
运行时再校验选择范围、TopK、balance_shift 全局 epoch 整除条件；算子 Host
保留公共布局、UB 与固定通信域容量检查。启动前的 `RUN_CONFIG` 显示 IP 列表、
卡数、专家数、模式/轮次、shape 和 build_id；summary 保留 server_count、
expected_hosts、rank_mapping 和完整 chain_configuration。

| 阶段 | 参数与验证边界 |
| --- | --- |
| build | 仅编译参数、工具链和产物；不要求 hostfile、运行地址或有效用例。`breakdown=0/1` 直接选择编译开关。 |
| sync/verify | 分发清单、SSH、路径、安装包与摘要；不加载 CANN，不解析运行参数，也不核验专家数/卡数关系。 |
| run | 本次选中机器、用例合法性、编译打点兼容性、各参与节点产物一致性及实际结果。 |

同步凭据记录 build_id、安装路径、产物摘要和机器列表，不再记录用例签名。
run 在任何 rank 启动前核验所选节点的实际安装包；缺少同步凭据本身不阻断已正确部署的产物。
改变规模、专家数、shape、轮次或端口无需重新同步；缺失/陈旧产物会指出对应 IP。
普通产物用于模式 0/1，breakdown 产物用于模式 2；切换编译打点开关后仍需编译并同步。

曾因共用 Config 在 build/sync 中解析运行用例，导致专家数不能被分发卡数整除时同步失败，
完整用例签名又使换参数触发无必要的重新同步。配置现在按 action 解析，分发清单和运行子集
分别处理；远端接收首节点解析好的子集，不再自行使用整个分发清单。回归以真实 Bash/Python
进程和模拟 SSH/rsync 验证编排、产物身份及故障边界，不作为真实多机 NPU 功能/性能证据。

## 普通 Memory 性能对照的接入边界

当前 `build_all.sh + sync.sh + run.sh` 尚未提供普通 Memory 算子的切换入口。构建会安装 `tilexr_ep_dispatch_memory_kernel.o`，但明确关闭 `BUILD_TILEXR_EP_DEMO`；运行固定启动 `test_tilexr_ep_server_dedup_chain`，consumer 固定调用 Server Dedup API。`measurements.py` 的 kernel 名、结果 marker、ABI/布局和 breakdown reader 也绑定 Server Dedup。JSON 中 `backend=memory` 表示 peer-memory 传输，不能据此认定执行了普通 Memory Dispatch。

仓库已有普通 Memory 的 `tests/ep/demo/tilexr_ep_dispatch_demo.cpp`、`tests/ep/cluster/run_memory_dispatch_node.sh` 和独立 msprof/trace 工具，可复用其 API 调用及最终输出校验；它们尚未接入当前三脚本流程。进行对照前需统一以下条件：

- 输入：旧 demo 的 `uniform/random/explicit` 没有选择当前 `balance_shift` 的分支；应共用实际 expert IDs 和 hidden 输入，不能只匹配 shape 或 seed。
- 通信域与轮次：旧 demo 使用默认 2 GiB、双分区 communicator，性能模式在预热后额外同步/barrier，functional 逐轮同步；当前是固定 4 GiB、预热和测量连续提交、整批末尾同步。普通 Memory 可使用满足其容量下限的 4 GiB 域，但需保持自己的双分区布局并单独验证复用。
- 测量：模式 1 取最后最多 50 个测量样本，先算每服务器 8 卡全部选中样本平均，再汇总服务器均值的 min/mean/max。与旧集群工具比较时仍需统一 warmup、样本选择、kernel SHA、通信字节和带宽定义。
- 打点：普通 Memory 使用独立的 `TILEXR_EP_DISPATCH_MEMORY_ENABLE_PROFILING` 和 v2 reader；当前 `build_all.sh breakdown=1` 只打开 Server Dedup 打点。性能对照两侧关闭设备诊断打点、保留 msprof；阶段对照使用各自 reader，不能仅替换 kernel 名。

Server Dedup 已接入完整 Dispatch 接收路径，但新设备功能与性能仍须先验收。普通 Memory 对照需要统一输入、4GiB 域和样本口径后接入上述流程；当前尚无可直接使用的 `run.sh impl=memory` 参数，不能把现有单侧结果报告为加速比。

## 输入类型与验证内容

六种已接入类型为 BF16/FP16 × 非量化/E4M3/E5M2；默认只运行上述 BF16 非量化基线。类型参数是运行参数，无需更改 kernel 构建：

```bash
bash run.sh -n 2 -e 96 -m 0 -l 1 0 dtype=fp16 quant_mode=0
bash run.sh -n 2 -e 96 -m 0 -l 1 0 dtype=bf16 quant_mode=4 fp8_format=e4m3
bash run.sh -n 2 -e 96 -m 0 -l 1 0 dtype=bf16 quant_mode=4 fp8_format=e5m2
```

FP16 的量化组合使用相同命令并设置 `dtype=fp16`。两种 FP8 均为 `quant_mode=4`，由输出 dtype 选择格式，不使用 `quant_mode=3`。设备调用沿用 Memory 的 MXFP8 函数；Host 保留 BF16/FP16 输入，使用现有 MXFP8 golden 比较包内结果。

完成 stream 同步及 Host barrier 后，每 rank 必须同时通过：

- communicator 的 Host/device 映射及固定 4 GiB 容量检查。
- 原始全路由的 cnt/cumsum 数值检查。固定基线每源卡每 expert 为 6，每卡前缀 `6,12,...,576`，6 个 expert 的 count 均为 96。
- 依据原始 expertIds 独立生成 expert/source/q 行序，逐行比较紧密 hidden、量化 scale、8 字段 assistEx。
- DIRECT 保留原 k/weight 且计数为 0、返回字段为 -1；第一跳保留正 n 并关联有效 group，终端为 -1 且引用同一 relay/t。只读 group 前 n 项，不消费空洞/padding。
- sourceMask 保留各 group 首 k 和全部 DIRECT k；expertScalesOut 对应为 1.0 和真实权重，原始权重逐字节不变。
- 所有原始 route 接收槽的全部块 Flag 均为零，唯一 counter 首 4B 为零。
- 每 rank 输出校验通过且正常清理后输出 `EP_SERVER_DEDUP_DISPATCH_PASS`，包含 `submitted=completed=loops`、预热完成计数和 `cleanup=complete`。汇总进一步检查跨 Rank 第一跳/终端的 `relay_links`、t 一致性及 route 完整性，最后才输出 `VALIDATION_PASS`；仅有 Host marker 或旧 SOURCE_SEND_PASS 不算完整 Dispatch 通过。

固定 H/K 下非量化 T=15872B，MXFP8 T=8192B。`first_hop_*`、`direct_*`、`relay_*` 分别记录三类实际路径包数与完整 T 字节；`data_bytes` 为三者之和。`local_data_bytes/same_server_data_bytes/cross_server_data_bytes` 互斥并覆盖全部发送字节。全 Rank 三类发送与接收数各自守恒，最终总接收数等于原始 route 数。远端 cnt 每源 rank 为 `(E-E/R)*32`，固定16P/E96时是2880B。

`relay_links` 每项为 `[source,token,first_k,relay,t,original_k,n]`，第一跳 n 为正、终端 n=-1。汇总要求第一跳 t 在 relay 上唯一、恰有 n 个对应终端、每个 original_k 唯一且都引用同一 t。并发调度可改变分配顺序；只在本卡检查 t 范围不足以证明跨卡关联。

公共 EP 函数签名不变；内部 kernel launch 参数为 13 地址 + 11 个 int64（192B），最后一项为 profiling launch slot，magic 仍在原偏移。调用方为真实权重输入和 expertScalesOut 分别分配内存；完整输出都在当前 checkpoint 比较，未使用的 metadata 空洞/padding 不作为判据。

运行会在本次 `<run_id>/inputs/` 保存 `rank-N-x.bin`、`rank-N-ids.bin`、`rank-N-weights.bin`（小端原始位型）。远端输入随日志收集到 `nodes/<节点序号>/inputs/`；shape、dtype、量化、seed、commit/build_id 在同次 summary 中。直接运行 C++ 可用 `TILEXR_VALIDATION_INPUT_DIR` 指定已存在目录。

在收集结果的首节点执行以下命令，可将实际专家列表汇总为同目录的 `expert-ids-<卡数>ranks.json`，无需编译或运行 NPU：

```bash
python3 tools/validation/export_expert_ids.py /path/to/run/summary.json
```

卡数、BS、TopK 和专家总数取自该 summary，默认输出紧凑 JSON，保留 `expert_ids[rank][token][original_k]` 顺序及运行身份；需要人工阅读时追加 `--pretty`。
分析 relay 选择时应使用本次保存的列表并保留原始 k 顺序；跨环境仅凭 seed 重建可能受 `std::shuffle` 实现影响。
脚本仅扫描同次运行的 `inputs/` 和 `nodes/*/inputs/`，缺 rank、重复 rank、字节数错误或专家编号越界均报错，不导出部分结果；失败运行在输入齐全时也可导出用于诊断。汇总只证明输入可解析，不证明设备路由或中转负载均衡。

## Breakdown：标准 records.v1 打点

在首节点逐条执行：

```bash
bash build_all.sh breakdown=1
bash sync.sh
bash run.sh -n 2 -e 96 -m 2 -l 3 2
```

编译打开 `TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING`，并为该 kernel 打开库的 `TILEXR_ENABLE_PROFILING`。manifest 使用 `server_dedup_profiling=true`、`server_dedup_profile_schema=tilexr_profiling_records.v1` 和 192B 内部 launch ABI；仍为 `r07-single` 布局及完整 `dispatch` 范围。旧 v6 自定义记录不再生成或解析，需要重新 build/sync。普通 `build_all.sh` 关闭打点，performance 继续使用 msprof。

每核只持有一个库提供的 `TileXR::DirectProfilingWriter`。每次真实命中读取一次系统时钟，以 Scalar stores 直接写独立 GM 的 `uint64 timestamp` 和 `uint32 event_id`，有效载荷 12B；核身份由记录流表示，不在每个点重复写入。核末由标准 `Publish()` 对齐并发布数量/丢失计数，Host exporter 写标准 records.v1（每条 16B，其中 4B 为保留字段）。不再使用性能打点 UB、时间戳数组、512B detail 状态、算子自定义 DCCI 或核内累计/最大值计算。库 writer 仍持有少量指针及游标；真实寄存器和局部存储开销以编译结果为准，不能宣称零开销。

保留 `ba67a77` 的全部真实取时位置，包括循环中每次 probe/acquire/metadata/atomic/output/relay、逐 peer 等待和每次 packet issue。未执行分支不复制虚拟时间戳，也不再记录 token/k、peer 身份、轮询次数或包分类计数。事件名仅由安装的 `share/tilexr/profiling/server_dedup.mapping.json` 描述，解析、配对和可视化由通用 `tools/profiling` 完成。

| 记录类别 | 当前报告方式 |
| --- | --- |
| main 0..8 | kernel 起止为 `kernel_total` 区间，其余真实边界为瞬时点 |
| recv/finish 的 14 个 detail 点 | 保留 prefix/tasks/prepare/loop/drain/clear 和 local/sync/counter/publish/wait 边界瞬时点 |
| 7 类循环操作 | `probe`、`acquire_load`、`metadata`、`atomic`、`output_issue`、`relay_issue`、`round_peer_wait` 的每次实际区间 |
| packet issue | 每个物理包 `DataCopy` 后的瞬时点；没有 token/k 和 first-hop/DIRECT 分类 |

边界点可用于观察相邻阶段的时间差，但当前 mapping 不将它们自动派生为旧 v6 的命名时长；零时长的 instant 也不能解释为阶段没有开销。循环操作的次数、总和、中位数、最短及最长由 Host 报表计算。atomic 嵌套在 metadata 内，不能将两者重复相加；核、轮次和 rank 之间的累计时长也不是一条连续关键路径。

原粗粒度 `Mark()` 自带的 profiling-only `PIPE_ALL` 已移除；通信代码原有 SetFlag/WaitFlag、PIPE_ALL、SyncAll 和完成协议保留。时间戳表示 Scalar 到达点位的时间，只有已有完成 fence 才赋予相应完成语义：`acquire_load` 的终点在已有 MTE2_S 之后，packet/output/relay 的 issue 不代表 DMA 完成，drain issue 也不表示流水已经排空。新旧 breakdown 数值不能直接作为无扰动性能 A/B；直接 GM 写和读时钟仍可能影响调度，普通性能基线关闭设备打点。

整批执行固定为每核预留 65,536 条，共 64 核，即每 rank 48MiB GM 有效载荷预算，另有约 4KiB 的计数区。每次 launch 覆盖本 core 的上一轮记录，consumer 在 communicator 初始化前将容量固定为 65,536，不再乘 `warmup+loops`，也不再受原 1,023 轮槽位上限限制。例如 50 次预热加 100 次测量，打点 GM 和当前 Host 原始回读均由每 rank 7,200MiB 降为 48MiB；8 卡服务器共 384MiB，64 卡共 3GiB。这是缓冲区容量，导出文件只含最后一轮实际命中记录，报告内存仍随该轮事件数增长。

Host 使用标准库 `TileXRProfilingReserveLatestLaunch` 复用 slot 0，独立于通信 magic；kernel 使用 `DirectProfilingWriter::InitializeLatest` 在入口重置本 core 的有效条数和丢点计数，然后沿用标准 Record/Publish。只重置计数，不清空整块 GM；latest exporter 只扫描发布的有效范围，所以较短或空的最后一轮不会混入旧尾部，上一轮 overflow 也不会污染本轮丢点数。最后一轮槽满仍报告 incomplete，通信继续执行；先前轮次的详细记录和丢点信息已被覆盖，不对其完整性作结论。

同一 capture 必须保持固定核集合、固定槽宽，并按 stream 顺序执行，不能并发覆盖。两个 Host reservation API 会拒绝混用 latest 和普通 reserved 模式；旧 collectives 的独立槽位和 append-style `ProfilingRecorder` 不受该 allocator 管理，调用方仍须保证 communicator 专用。普通 reserved/legacy 导出保持原语义，只有显式 latest capture 才将有效条数作为扫描上限。

R07 原始起始 12,800B 预留区保留为布局 padding，cnt/数据偏移和 4GiB 通信域不变；新打点既不占用该区，也不占用通信域 payload 尾部。删除自定义 flush 路径，从结构上去掉此次 DCCI tensor extent 错误所在的算子记录逻辑；这不等于已经证明原 A5 卡死在新产物上消失。

整批 stream 同步及输出校验完成后，communicator 销毁时导出每 rank 的 `rank-N.records.v1`。各 rank 共用 capture UUID，文件路径独立，只保留最后一次提交的打点（`captured_launch=warmup+loops-1`，从 0 开始）。各服务器为本卡解析一次并生成标准 Perfetto、HTML 和统计；主节点默认只收集已生成的报告及小摘要，不收远端原始 records，不重新解析或生成每卡/全局大 trace。原始文件留在所属服务器，摘要保留来源，供显式离线重处理。节点内报告锁仍限制 Python 解析并发，不限制各卡 C++ 回读并发。

通用报告检查每核唯一完整的 kernel 起止和本轮配对，不能再按预热次数剔除唯一 capture。采集不完整与通信输出正确分开记录，失败节点中其他已完成卡的报告仍保留；不能把截断 trace 当作完整性能证据。

成功结果的主要路径如下（rank 文件可能位于本节点目录或归集的 `nodes/<节点序号>/`）：

```text
run/case-<时间>-<PID>/<run_id>/summary.json
run/case-<时间>-<PID>/<run_id>/traces/chrome/rank-N.json
run/case-<时间>-<PID>/<run_id>/traces/index.json
```

`chrome/` 集中放置每 rank 一份 Chrome Trace JSON；`index.json` 记录文件路径，并按最后一轮 kernel 时长选择 fastest/p50/slowest rank。每核时间归零不构成跨核或跨 NPU 时钟同步。`measurements.breakdown.kernel_us` 取每 rank 最后一轮的核最大值，再汇总跨 rank min/median/max；保留各 rank 值，不先合并服务器中位数。操作统计使用该轮实际配对区间；多次操作的中位数不能称为多轮执行中位数。performance 仍使用关闭设备打点的 msprof 多轮样本，统计口径独立。

需要从原始记录重新解析时，显式取回各服务器的 `rank-N.records.v1` 到同一本地目录，再用 `--records-dir` 生成每 rank 一份 JSON；输出目录须为空或尚不存在。默认执行仅归集已完成的 JSON：

```bash
python3 tools/validation/measurements.py /path/to/run/summary.json --records-dir /path/to/copied-records --output-dir /path/to/new-report
```

维护要点：保持事件位置与记录机制分离；新增点位只扩展数字 ID 和 mapping，不重新引入 UB 暂存或算子专属 cache flush。复用槽位必须同时处理有效范围和丢点状态，不能只把 slot 改成 0。跨 Host/kernel/reader 改动需同时校验 launch ABI、固定分配预算、长轮后短轮/空轮、截断状态及安装产物身份。多机 profiling 应在数据所在节点完成解析，再收集成品，避免每卡本地解析后主节点又解析两遍。

2026-09-17 历史同步版本：为区分 SendCnt 的统计和通信开销，追加两个区间（旧事件 ID 保持不变；异步方案使用上文的新下发事件）：

| 区间 | 事件 ID（begin/end） | 计时范围 |
| --- | --- | --- |
| `send_cnt_filter` | `0x53000216/17` | `CountByVectorRange` 或逐 expert 统计，包括筛选、histogram/归约及 ready/count 记录构造 |
| `send_cnt_send` | `0x53000218/19` | 已有 S→MTE3 依赖、`SendExpertCntRange` 下发及已有 `PIPE_ALL` 等待；终点在 TPipe Reset 之前 |

两者都位于原 `send_cnt` 内；UB 分配、expertIds 加载和 Reset 等开销仍只归入外层，因此两项之和不等于整个 `send_cnt`。新增点位是被动 Scalar 观察，不增加同步；MTE2_V 不阻塞 Scalar，加载的剩余等待可能体现于统计区间已有的 V_S 等待，不能把 `send_cnt_filter` 解释为纯 Vector 指令耗时。发送区间包含原有发送排空，不等于远端接收核已消费 cnt。未参与 Count 或 expert 区间为空的核不产生这四条记录；旧 capture 也不补造细分耗时。重新编译 profiling ON kernel 并同步安装的 mapping 后，标准 trace 和 `stages_us` 自动显示这两个名称，无需更改 records 协议或采集容量。

验证：基于 `47563bd` 加本次修改，blue 上 CANN 9.1.0、Bisheng/Clang 15.0.5 的 A5 `dav-c310-vec` kernel OFF/ON 编译链接均成功；Count OFF/ON、profile、source_send OFF/ON 共 5 项 Host 替身回归通过，本地 19 项 trace 测试通过。源码摘要、配置、编译与测试日志保留于 `build/sendcnt-profile-verification/`。未运行 NPU，实际耗时及新增四条记录的采集开销需 A5 实测。

原报错中的 SSH 退出、收集超时和“结果汇总超过总期限”不能证明主节点最终报告生成是首因：该期限检查在最终报告分支前，已有错误时该分支被跳过。此次修复控制 capture、节点报告和归集的数据量，不修改执行期限；是否消除原 A5 超时仍需用户环境运行证据。

### 单轮覆盖与分布式报告的验证边界（2026-09-14）

标准 runtime 和设备 ON/OFF 三个 C++14 程序以 `-Wall -Wextra -Werror` 编译执行通过，覆盖一万次固定槽预约、模式混用拒绝、长轮溢出后短轮/空轮、旧尾过滤及相邻 core 保护。EP 聚焦 CTest 7/7、consumer 替身 19 项、独立地址空间检查 1 项及通用 Python 报告 81 项通过。上述均为 Host/替身证据。

本次最新生产源码以 `36ec5dd` 加工作区修改在 blue 的真实 CANN 9.1/A5 工具链完成 OFF/ON kernel、`tilexr-ep` 及 `tile-comm` 编译。532 个生产/构建文件与快照一致，源码清单、日志和资源诊断保存在 `build/profiling-latest-verification/`。静态 kernel 栈 OFF=2,504B、ON=2,576B，差额 72B；VF 最大栈均为 0B，最大 VReg/PReg 均为 10/4，未取得完整 Scalar spill 证据。本次没有运行 NPU，不宣称原卡死或多机处理时限已经实机验证修复。

### 前一版标准打点迁移的验证边界（2026-09-14）

本地 EP Host/设备替身聚焦检查 14/14 通过，覆盖打点开关、GM guard、重复 launch、发送/接收字节正确性及内部 launch ABI。接收 ON 测试随后更新为标准宏对应的 PASS marker，重编并运行通过。通用 Python 报告 81 项通过，包含重复/嵌套区间、截断、预热选择、容量上限及已解析模型复用。真实 C++14 consumer 配合 ACL 替身的 19 项集成检查通过；这些测试不执行 NPU。

验证脚本完整首轮运行 170 项，其中 165 通过、3 项因 MSYS symlink 限制跳过、2 项旧夹具不匹配；两项随后均已修复并定向复跑通过。最终相关检查包括 runtime configuration 12 项、build 14 项通过/1 项跳过、measurements 18 项、标准 trace 4 项，以及多轮归集、缺失/陈旧记录和混合节点 overflow 的定向 workflow 检查。最后的 overflow 检查同时断言该节点存在成功 rank，并验证成功及不完整 capture 的全部本地归集链接；错误结果保留 `communication_completed=true`、`profile_status=incomplete` 和原通信记录，不能因采集失败抹掉已验证的通信证据。完整 170 项套件未在这些夹具修正后整体重跑，不能表述为最终全量零失败。

blue 使用真实 CANN 9.1（bisheng clang 15.0.5，2026-05-18）分别配置 A5 `dav-c310-vec` 的 OFF/ON，Server Dedup kernel 和拥有它的 `tilexr-ep` Host 库均编译通过。源码身份为 `16a7b79` 加本次工作区变更，快照文件摘要、配置、编译返回码和日志保存在本地 `build/profiling-verification/`；该目录是验证产物，不纳入代码提交。

同一 CCEC 编译命令增加资源诊断选项后，`-fstack-usage` 报告 kernel 静态栈 OFF=2,504B、ON=2,584B，增量 80B；`--cce-res-usage` 报告所有 SIMD VF 栈为 0B，最大 VReg=10/PReg=4，两版相同。没有获得完整 Scalar 寄存器或 spill 统计，不能据此声称整个 kernel 没有 spill。这里也没有与旧 v6 二进制做资源 A/B。

独立 review 核对了 `ba67a77` 的真实点位、必要同步和 192B 参数顺序，未发现当前 Server Dedup 专用 capture 路径新增功能回归。此次没有启动 A5 设备功能/性能测试；编译和替身通过不能证明原 breakdown 卡死已消失，也不能量化直接 GM 打点的实际性能扰动。

### 历史记录：2026-09-14 slowesttrace 分析与后续判断

输入为用户提供的 [slowesttrace.json](https://github.com/SilentEmotion/hallo-world/blob/master/slowesttrace.json)，schema5、选中 rank3，64P/E384、BS96/H7168/TopK6、BF16→MXFP8 E4M3、balance_shift/seed2026、100 轮测量＋50 轮预热，记录最后 generation150。run_id=`6944e3210ddb46af89f4c7dec5aa20ad`，kernel SHA256=`43071633e430aac4bb4109da4902560635618ab4299d8306b4ebf439054a1666`。此处仅统计该 rank 的核，单位 μs（A5 1000 ticks/μs），不代表所有 rank 或 100 轮分布，也不以此单卡记录计算跨卡带宽。

| 区间/核集合 | min | median | max |
| --- | ---: | ---: | ---: |
| send_data，core0～47 | 30.507 | 37.982 | 42.615 |
| recv_data，core48～63 | 44.380 | 53.704 | 58.164 |
| finish，core48～62 | 0.171 | 4.357 | 13.991 |

core60 的 recv_data 从 24.997 到 83.161，是接收阶段最慢核；core63 从 24.839 到 78.535，接收结束后 finish 耗时 29.539，到 108.074 才结束。其余核在约 83.33 结束，因此 core63 形成约 24.74 的额外拖尾。core63 的 finish 在接收核中最长；源端核更早进入同步等待，例如 core18 的 finish 为 50.601，不能说 core63 的 finish 时长大于全部 64 核。旧 v5 没有内部记录，无法把拖尾直接归因于 counter、通知或某个 peer。

先根据新 trace 判断：若 `finish_sync` 长，应检查其他核接收与转发结束时间；若 `finish_publish` 长，可评估将各 peer 通知分配给多个核；若 `finish_peer_wait` 长，结合最慢 peer 与该 peer 的本地接收结束情况判断。单纯更换完成 owner 只会移动拖尾；并行通知必须保证所有数据/Flag 完成、counter 清零后才能公布本轮完成，额外协调也有成本。counter 只写 4B，但仍包含 pipe/buffer 和同步开销，是否值得卸载应由细分实测决定。本次保留原有 48/16 分工和 core63 完成协议。

### 旧 trace 的兼容边界

旧 v3/v4/v5/v6 trace JSON 可继续用原可视化工具打开。当前 `measurements.py` 只接收标准 records.v1，旧自定义 summary reader、trace renderer 和 `sort_trace_tracks` 已移除；仅有旧 summary 时需使用对应历史版本工具，不能据此补出当时未采集的点位。上面的 slowesttrace 分析属于历史证据，不代表当前新框架的运行结果。

## 失败定位与本地检查

| 步骤 | 日志 | 首先查看 |
| --- | --- | --- |
| 编译 | `run/validation/build-*/production-*.log`、`host-*.log` | 终端指出的编译/链接命令及错误 |
| 同步 | `run/validation/sync-*/<节点>/prepare.log`、`transfer.log`、`verify.log` | SSH、分发路径和安装包身份 |
| 启动前产物检查 | `run/case-*/<run_id>/verify-<IP>.log` | 对应节点安装包及 build_id |
| 运行环境 | `run/case-*/environment.log` | CANN、Python、配置加载 |
| 运行 | `run/case-*/workflow.log` 和终端指定的 `<run_id>/summary.json` | primary_error、errors、失败 rank 的日志与 stage |

`stage=count-read` 或 `stage=dispatch-read` 表示 stream 与 Host barrier 已完成，随后数值/包校验失败，不代表等待超时。错误保留首个不匹配字段、source/token/k/q 或 count 邻域。count 输出预填 -1，mask/权重输出预填哨兵；保留实际原始输入供复现。提交、同步或 barrier 失败而完成状态未知时，不释放可能仍在使用的设备资源，交由进程退出和调度器收尾。

全核 Count 阈值调整后若仅量化 hidden 失败，应先区分发送数据与接收输出，不能仅凭非量化通过认定量化公式或 `TPipe::Reset()` 有错。hidden 首次不匹配会报告 byte/expected/actual、差异字节数、scale 比较和 assist 原值/期望，并在整批同步后读回该行接收槽：`wire_hidden_match` 比较槽内 hidden 与 golden，`wire_output_match` 比较槽内 hidden 与最终输出，`wire_scale_match` 比较槽内 scale 与 golden。读取跳过每块的 Flag，清 Flag 不清 payload；额外读回失败仍保留原 hidden 错误。槽正确而输出错误优先查接收搬运；槽和输出一致但都错误则向发送/量化或错误槽位归属追溯。该诊断只观察最终状态，不能单独证明设备时序或根因；首个 hidden 错误也不代表后续行和 metadata 已校验通过。

### 只有 CHAIN_PASS 后超时

`EP_SERVER_DEDUP_CHAIN_PASS` 在第一次 Dispatch 提交前输出，只证明建链和启动前检查通过。`primary_error` 为 `EXIT_FAILED / 集群达到统一执行期限 / stage=execution` 时，表示控制端期限到达并取消集群任务，不能据此认定 kernel 已运行、recv_data 死锁或环境故障。统一执行预算为 `min(配置总期限 - 20 秒, 120 秒)`，从控制端 run 开始计入产物检查、启动及测试，不是单个 kernel 的等待时长。

breakdown 模式在 Host 阶段入口刷新 `EP_SERVER_DEDUP_PROGRESS`，每 rank 输出有界进度行，与轮数无关。warmup 和 measurement 各只在第一次提交前打印一次 `stage=launch`；该行的 submission/submitted 是**阶段入口快照**，不能解释为后续卡在第 1 轮。functional/performance 不打印进度；不增加设备同步或 Host 计时。

| 最后一条 progress 的 stage | 下一条尚未出现时的排查范围 |
| --- | --- |
| `launch` | 当前 warmup/measurement 批次的 Host 提交路径，含运行时排队；不能确定具体阻塞轮次 |
| `stream-sync` | 所有提交已返回，进入整批 stream 同步；需结合所有 rank 和设备日志检查异步执行/等待 |
| `batch-barrier` | 本 rank stream 同步已成功，正在等待其他 rank 的 Host barrier |
| `round-read` / `count-read` / `dispatch-read` | 整批 stream 和 Host barrier 已完成，正在读取/校验对应输出 |
| `cleanup-communicator` | 释放 communicator，同时由标准 runtime 读取并导出 records；大 capture 的 Host 内存、拷贝及文件写入也发生在此处 |
| `cleanup-*` | 当前对应资源释放阶段；最终 DISPATCH_PASS 或 DISPATCH_INVALID 中 cleanup=complete 确认资源收尾；INVALID 仍表示输出校验失败 |

在本次运行的 `<run_id>` 结果目录执行以下只读命令，保留全部 rank 的最后阶段和已有错误：

```bash
find . -type f -name 'rank-*.log' -exec sh -c 'for f do printf "\n%s\n" "$f"; grep -E "EP_SERVER_DEDUP_(VALIDATION_)?FAIL " "$f"; grep "EP_SERVER_DEDUP_PROGRESS " "$f" | tail -n 3; done' sh {} +
```

2026-09-14：用户回传 16P/E384、BS96/H7168/TopK6、BF16/MXFP8 E4M3、breakdown 100+50 轮运行达到统一期限，rank 0 最后一条为 CHAIN_PASS。旧代码仅在异常或最终成功时输出后续状态，外部取消使具体阻塞边界丢失，因此补充调用前刷新的有界阶段日志。当前尚未确认设备根因；`3fa9d1b` 仅修正 Host 测试标记，设备 recv/finish 打点变化位于 `ba67a77`。本地以真实 C++14 consumer 配合 ACL 替身暂停 launch、stream-sync 和 communicator destroy，确认 API 尚未返回时日志已可读，解除暂停后全部 16 rank 完成；四项聚焦测试通过，还覆盖异步错误、清理失败和普通模式六种类型组合。这只验证 Host 定位能力，不表示卡死已修复或 CANN/A5 已通过。

2026-09-14：A5 plog 进一步确认 breakdown 的 `aclrtSynchronizeStream` 返回 507035 源于 AIV error 271（`The address for scalar to access the internal buffer is out of bounds`），所有相关 core 在相同 kernel PC 报错。根因是 detail profile flush 对 `GlobalTensor<uint8_t>` 使用 `bytes[offset]` 的索引 DCCI 时未设置 tensor extent；该 bounds-sensitive overload 不能依赖无长度的 `SetGlobalBuffer`。修复为传入 `sizeof(EpServerDedupDetailRecord)` 后才可重新验证 breakdown；functional 不执行该 flush 路径，因此不能作为此修复证据。CANN/A5 编译和运行结果须以修复后的新 build/sync/run 为准。

不带 CANN/NPU 的聚焦检查：

```bash
cmake -S tests/ep -B build/server-dedup-host
cmake --build build/server-dedup-host --target \
  test_tilexr_ep_server_dedup_protocol test_tilexr_ep_server_dedup_layout \
  test_tilexr_ep_server_dedup_host_validation test_tilexr_ep_server_dedup_public_abi \
  test_tilexr_ep_kernel_launch test_tilexr_ep_server_dedup_counts \
  test_tilexr_ep_server_dedup_profile test_tilexr_ep_server_dedup_receive_packet \
  test_tilexr_ep_server_dedup_receive_profile \
  test_tilexr_ep_server_dedup_source_send \
  test_tilexr_ep_server_dedup_source_send_profile test_tilexr_ep_server_dedup_host_profile_validation -j8
ctest --test-dir build/server-dedup-host --output-on-failure \
  -R 'test_tilexr_ep_server_dedup_|test_tilexr_ep_kernel_launch$'
python3 tests/ep/integration/test_server_dedup_chain.py
```

本地 sender 测试执行真实非量化生产逻辑，覆盖原始 q、全部隐藏数据/Flag/尾表、UB/输出边界和环形池复用；在 GM 搬运替身记录实际发送，核对第一跳优先、DIRECT 从本 server 开始循环且同 server 内保持 Token/k 顺序、每个槽位恰好一包及跨阶段 lane 序号连续。64P/E384 乱序路由覆盖源 server0/3/7、本卡及同 server 跨卡、空目标 server、每核多 Token、第一跳与 DIRECT 混合及三轮复用，profiling OFF/ON 均执行。入口六种类型分支做替身编译，Host consumer 六种类型做夹具及错包注入；这些不证明真实 MXFP8 计算、异步 MTE、SyncAll 或 CANN/A5 通过。服务器结果按实际 shape/模式登记到 [调试进度](EP_MEMORY_SERVER_DEDUP_PROGRESS.md)。

逐包打点的本地回归还核对每条记录与实际发送对应、空核头、容量/同步标志区边界、Host 容量拒绝、损坏记录拒绝、三个子阶段及最后一包的 trace 语义。纯 CPU 替身只验证记录和导出契约，真实 CANN 9.1 编译、A5 可见性及诊断开销仍需服务器运行确认。维护时不要在热路径逐包写 GM/DCCI，也不要为获取完成时间新增同步；相邻下发点才能保持这里约定的测量含义。

2026-09-14 v6 本地验证：9 项聚焦 CTest 通过（接收和发送各含 profiling OFF/ON、计数、profile、Host 容量和独立 GM 地址空间）；19 项 C++14 consumer 集成测试、33 项测量/导出测试通过；15 项构建脚本测试中 14 项通过、1 项因 MSYS 模拟 symlink 跳过；2 项 breakdown 多节点编排测试通过，覆盖 v6 manifest、同步后运行、全部 rank 记录、核排序及缺失/旧代次拒绝。consumer 覆盖 43 种记录损坏、零时长样本、H7168 量化多块包。主审和独立 AscendC 审查未发现确定性缺陷，检查了事件顺序、lane 生命周期、GM 地址空间和诊断区隔离。本地没有 CANN/A5；这批证据不证明 v6 真实编译、异步执行、可见性或打点性能，用户先前完整 Dispatch 的功能通过记录仍按其原始边界保存。

维护经验：通信域由双份计数布局迁移到单份数据布局时，必须同时更新 Host 容量、设备地址、profile reader、manifest 和当前检查点，禁止复用旧计数通过标记。旧 source-send 检查点结束时数据 Flag 为1；完整 Dispatch 结束时所有消费槽 Flag 必须为0，因此必须一起切换 kernel、consumer、manifest、marker 和打点版本并重新 build/sync，不能沿用旧成功包。量化输入尾部必须按 Memory 显式补零，并在复用输入 UB 前完成初始化写入；非量化传输填充无需清零。

单份 cnt/发送槽位的连续调用必须有跨 rank 完成边界；本卡 SyncAll 不能阻止快 rank 覆盖慢 rank 的上一轮记录。当前核63在已有全核收敛、counter 写零完成后，将本次 NextMagic 的四个 int64 副本写入各 rank 的专属 32B 行，再等待本地所有 rank 行均 >= 本代次。记录位于 4MiB communicator Flag 区的最后 4KiB（最多128行），不移动 payload、cnt 或 profile；这些行不清零，允许更晚代次满足已完成的旧代次。仅支持同一 communicator 上所有 rank 同序、同 stream 的 Dispatch 批次；不得与其他未完成操作并发复用此区域。本地回归覆盖迟到/部分/旧代次、后续代次越过等待、四轮复用和边界，设备可见性及实际等待开销仍须 A5 验证。

新增 Host CTest 必须安装并输出与测试名对应的 marker（如 `EP_SERVER_DEDUP_ABI_HOST_PASS test=source_send`）。EP 自定义链接命令必须依赖 `$<TARGET_OBJECTS:...>`，保证对象重编后最终外部 kernel 重链；生产 Host 库继续相对加载、注册独立 AICore 二进制。CPU 编译/链接和故障注入只证明这些边界，不替代设备验证。

2026-09-14 v6 Host 标记修复：同一源文件生成 `receive_packet` 和 `receive_profile` 两个程序时，后者仍打印 `test=receive_packet`，导致各节点在 Host 测试阶段报 `LOG_PARSE_FAILED`，工作流退出 16。该故障不是设备 trace 解码失败；此前 CTest 只检查退出码，模拟工作流又使用自带正确标记的夹具，未覆盖真实程序到 reader 的边界。现按 profiling 编译开关输出各自标记，与 source_send 测试保持一致。维护时应让实际编译出的变体经过工作流 marker 校验，不能仅用程序 exit=0 或自造 PASS 日志证明兼容。本地将两个真实接收程序接入现有 node 夹具：修复前复现同名测试的退出 16，修复后节点通过，两个接收 CTest 也通过；其他四项 Host 程序为编排夹具，该验证不涉及 CANN 或 NPU。

## 输出和验收范围

以下保留历史检查点说明，原文中的“当前提交/当前阶段”指记录形成时的代码。当前完整 Dispatch 操作步骤及通过条件以上文为准；不运行下面的旧空 kernel 或 breakdown 验收。

### 每 Rank 4 GiB 通信域与边界访问

`ep-sdedup-p1-memory-boundary` 是独立的 functional 补充验收。当前提交入口只运行这一个 case，
不回归旧检查点、不运行 Dispatch、不采集性能或 breakdown。在已配置环境的首节点执行：

```bash
bash run_commit_validation.sh
```

该入口构建并同步新的通信库、测试 Host 库和独立 A5 kernel。相同安装产物再次执行只需
`bash run.sh -n 2 -e 96 -m 0 -l 1 0`，不必重新构建/同步。

- `TileXRCommInitRankMemoryDomainWithSize(0, 4092, ...)`：4092 MiB 数据区 + 4 MiB Flag 区，
  `peerMemBytes` 必须恰为 `4294967296`。底层另有 1 MiB 地址偏移余量，实际分配 4097 MiB/Rank。
  原来的 MemoryDomain 初始化入口保持 8 MiB 数据区默认值；不修改普通 Memory 的固定分区。
- Host 初始化整个分配为零，各 rank 核对 Host/device CommArgs 和全部 peer 指针，建链 barrier 后开始测试。
- 每卡只启动一个 AIV，通过自己 `peerMems[rank]` 的 `uint64_t` 偏移 `4294967264`（`4 GiB - 32 B`）
  读取初值，再用 DataCopy 写入连续 32 B `0xFF`，清空 UB 后从同一位置 DataCopy 读回独立结果区。
- Host 逐字节检查设备初读为零、设备读回为 `0xFF`、边界原址为 `0xFF`，以及前邻 32 B 仍为零。
  读回区预置不同哨兵，未 launch/未读回不能误报 PASS。全部卡通过完成 barrier 和清理后才能汇总 PASS。

结果仍保留在 `run/case-<时间>-<PID>/` 的 `summary.log`、`summary.json` 和各 rank 日志中。
`nodes[].reported_result.ranks[]` 记录 `peer_mem_bytes`、`boundary_offset`、`probe_bytes` 及各项比较结果；
失败日志包含 `ERROR`、rank、stage 和首个不匹配字节。该验收证明分配、映射和本卡末端访问，
不证明整个 4 GiB 区间逐字节可用、跨卡 payload 传输或去重算法。

维护时须区分逻辑窗口、底层分配和算子分区，4 GiB 的字节数及地址运算必须保持 64 位。
边界探针保留在 `tests/ep/integration/memory_boundary_*`，只在验收构建中启用，勿塞入空 Dispatch。
复用 consumer 头文件时也须保持 C++14：即使未调用，inline 函数中的 `std::gcd` 仍会阻断编译。
本地检查覆盖 C++ consumer 的 16 进程行为、错误注入和汇总协议；CANN 编译、4 GiB 分配/IPC 映射及
真实 DataCopy 必须以 A5 上机结果为准，不把 Host 替身通过登记为硬件通过。

### breakdown Chrome Trace

breakdown 完成后，Chrome 可导入文件集中在 `run/case-<时间>-<PID>/<run_id>/traces/chrome/`：

- `rank-0.json`、`rank-1.json`……：每张卡独立一份，每份包含该 rank 最后一轮的 64 核耗时分段和实际重复操作。
- 父目录 `traces/index.json`：记录各 rank 状态、排序、fastest/p50/slowest 代表卡及 JSON 路径，同时写入 `summary.json` 的 `traces` 字段。

在 `chrome://tracing` 点击 Load，选择任意 `rank-N.json`；文件使用标准 `traceEvents`，其中 `ph: "X"`、`ts`、`dur` 描述区间，时间单位为微秒。`run.sh` 终端摘要打印 JSON 目录及 `fastest_rank=N | slowest_rank=M`，对应目录下的 `rank-N.json` / `rank-M.json`；完整代表卡路径保留在 workflow 日志。排名直接使用 trace 索引的 `selected_ranks`，没有有效选中项时不打印排名，rank 0 仍正常显示，避免与 trace 选择口径不一致。HTML 仅作为节点辅助报告，主节点不生成表格入口或全卡合并 JSON。

主节点校验各节点的 capture 身份、摘要与文件摘要后，将已经生成的 `perfetto_trace.json` 原样复制并重命名为 `rank-N.json`，不重新解析 records，不加载全部 trace 到内存。重复导出时清除当前结果中不可用的旧 rank JSON，避免将旧文件误认为本次结果。原始打点、事件配对及 64 核分段信息保持不变。

代表卡筛选依据为每 rank 最后一轮最慢核耗时，按耗时、rank 升序排列；p50 选择实际的下中位卡，跨 rank 数值 median 仍按普通中位数计算。每核保持通用 reader 的局部时间原点，不能用此图判断跨核或跨卡的绝对先后关系。

已有完成采集的 breakdown summary（包括输出校验失败的实验）可直接归集 JSON，无需重新编译或启动 NPU：

```bash
python3 tools/validation/measurements.py /path/to/run/summary.json --output-dir /path/to/run/trace-export
```

结果位于 `trace-export/chrome/`，保留 summary 中所引用的节点报告目录即可，不需要原始 records。显式提供 `--records-dir` 或 `--mapping` 时才重新解析原始文件，仍按 rank 分文件输出。

缺卡、损坏文件或不完整采集继续保留错误状态，不将该 breakdown case 标为 PASS。`summary.json` 的 `traces.trace_dir`、`traces.ranks`、`traces.selected` 指向输出；耗时汇总保留在 `measurements.breakdown`，每核详细区间由各 JSON 文件展示。

functional、performance、breakdown 均区分执行完成与输出正确性；performance 和 breakdown 允许结果错误的实验继续完成采集。例如将转发 group 的原子分配替换为固定 `t=0` 会破坏 group 唯一性，可能触发 `group firstHopRow mismatch` 或重复 group 检查。仅当整批 kernel 同步、批末 Host barrier 和资源释放均成功时，consumer 将 count/Dispatch 输出不一致记录为 `EP_SERVER_DEDUP_DISPATCH_INVALID`（`status=FAIL`，保留首个错误），同时完成资源收尾；模式 1 的 msprof 完成导出，模式 2 完成标准打点导出。节点运行状态描述执行与采集是否完成，另以 `validation_status` 标记输出正确性；不能将节点进程退出 0 当作算子正确。

此类输出错误不取消其他 rank/节点：所有节点先各自解析，主节点再归集每 rank JSON，最后打印 `VALIDATION_FAIL` 并返回结果校验失败退出码。`traces/index.json` 的每 rank `validation_status`/`validation_error` 与采集完整性 `status` 分开；全部 rank 的计时完整时，即使输出校验失败，也保留 `measurements.performance` 的跨 server min/mean/max 或 `measurements.breakdown` 的跨 rank min/median/max；错误结果或因日志收集失败等原因未完成跨 rank 校验时，通信量为 null、带宽为 N/A，不使用未通过完整校验的流量。缺卡、样本不完整或损坏时不生成部分 rank 的性能汇总。所有模式的算例最终状态仍为 FAIL、退出码非零。设备/API 错误、超时、释放失败以及旧运行身份或损坏 capture 仍属于执行/采集失败，不能通过输出错误标记绕过。

维护此流程时，须分别验证“真实 consumer 输出错误后完成导出”和“节点/主节点不因该错误取消同伴”；只验证原始 records 文件存在会漏掉上层提前退出。若把输出不一致直接作为 consumer 执行异常退出，msprof 或调度器可能提前退出/取消同伴，模式 1、2 的完整耗时因此丢失。维护时应保持 INVALID 与执行失败分离，并验证最终 case 仍失败。Host 替身可覆盖上述失败处理和 JSON 归集，不证明 NPU 上固定 `t=0` 实验本身能正常结束或其性能收益。

每次运行在仓库 `run/` 下生成独立目录。`run.sh` 终端先输出一行最终状态汇总，再为每个实际运行且已有完整测量的算子单独输出一行性能，并原样保存为 `result.txt`：dispatch 显示 `dispatch:`，combine 显示 `combine:`，both 按 Dispatch、Combine 顺序各显示一行；functional 不输出性能行。PASS 携带可用的性能数据或 trace 路径；FAIL 分为“执行阶段失败”“执行卡死（超时）”“结果校验失败”，有完整计时/trace 时一并保留。执行超时不等价于已证明 kernel 死锁。环境详情在 `environment.log`，可选 Host 回归在 `host-validation.log`，调度输出在 `workflow.log`，入口重定向等错误在 `launcher.log`，逐卡日志和结构化结果在汇总指定的 `<run_id>/summary.json` 同级目录；配置错误可能指向 `failure.json`。不再向终端展开各卡错误栈。只统计 kernel 耗时，不采集或汇总 Host submit/sync/barrier/batch_total 时间。模式 1 的统计口径写入 `measurements.performance.selection/reduction`，`samples_per_rank`/`samples_per_server` 记录有效样本数，`server_mean_us` 按 hostfile 的 server 顺序记录代表耗时；`mean_us` 是这些 server 均值的算术平均。

终端汇总同时消费 Dispatch 的 `measurements` 和多算子路径的 `operator_measurements`，
后者以算子名区分，不能只检查前者而把已经采集的 Combine 耗时漏掉。性能模式继续使用
各算子 `performance` 内的 server min/mean/max，breakdown 使用各自 kernel 的 min/median/max；
不混合两个算子的样本。Combine 行保留报告给出的阶段范围、等待时长及逻辑 payload 数据量/带宽，
不将阶段 payload 处理速率标为跨 Server 带宽。统计不可用时不构造虚假的性能行。
回归覆盖三种 operator_mode、两种统计模式、失败时保留有效测量及真实 run.sh 的分行输出；
这些 Host 测试验证展示与路由，不重新证明 NPU 采集数值。

2026-09-17 错误窗口读回用例维护：`ae5bb5bf` 将输出不一致的退出约定推广到所有模式时，
误将 `test_repeated_calls_reject_wrong_bank_readback` 也改为期待退出 0 和 DISPATCH_INVALID。
该故障实际由 `aclrtMemcpy` 替身返回 92，属于 API 执行失败；父提交 `88712484` 的用例通过，
`ae5bb5bf` 的用例失败。修复只恢复非零退出断言，并验证具体读错误及不产生
DISPATCH_PASS、DISPATCH_INVALID、BREAKDOWN_PASS；保留已有 wrong-bank-read 调用轨迹检查。
维护故障测试时须按注入发生的边界区分“读取失败”和“读取成功但输出错误”，不能统一改成
INVALID。这类 Host 替身测试验证错误分类，不证明设备通信窗口或 NPU 异步执行正确性。
修复后在 blue 重跑 `test_server_dedup_chain.py`，25/25 通过；日志与源码差异保存在
`build/wrong-bank-fix-verification/`。

同一 stream 连续提交整批，批末才 stream 同步与 Host barrier；删除 Host 计时不改变同步策略。functional 不采集 profiling；performance 使用 msprof；breakdown 读取最后一轮各核的设备记录。

当前阶段 1 是空 Dispatch 框架，PASS 不证明去重通信或输出数值。64P/128P 仍待环境具备后验收，16P 通过不扩大到未测规模。提交验收仅保留本次受影响 case；测试脚本重构不要求重新执行已经通过的硬件检查点。

## 后续维护约束

接收解包曾在 A5 `dav_3510` 编译时触发 `SupportBytes<unsigned char, 2, 4>()`：用户日志中的 CANN 9.2.0 `CopyImpl` 只支持 b16/b32，`recv.h:127` 是实例化提示，真正调用错误在 `Copy<uint8_t>`。用 b32 视图按 mask 64/56 分两次复制每包的 480B 数据，源/目标 repeat stride 分别为 16/15 个 32B block；repeat 是包数，不是 `包数*15`（H=7168 时后者为 450，转 uint8 会截断）。这是此前完整 UB 解包的修复；当前单副本接收已改为 UB→GM DataCopyPad 跳过 Flag，生产接收不再执行 Vector 整包剥离/重打包。CPU 替身同步限制元素位宽，完整入口 profiling=0/1 编译和包布局回归验证类型、内容与 UB 边界；不能替代 CANN 9.1/A5 实编译或异步设备验证。判断工具链版本应依据真实 include 路径和版本信息，不能仅凭项目默认版本。

分步开启 DIRECT 时必须一起更新检查点、Host 包集合、sourceMask/权重预期和流量统计，否则已发送的 DIRECT 会被旧校验误报为非目标槽位，或性能 JSON 仍把其流量记为零。最终槽位内容无法识别同址同值的重复发送；本地在搬运替身观察每包地址、身份和发送池顺序，补充次数与核内顺序证据，不在生产两阶段之间加同步。该证据不证明真实异步 MTE 顺序或 A5 性能。

breakdown 编译曾在 profile record 转换处报 `casts away qualifiers`：CANN 将 `GM_ADDR` 定义为指针宏，`const GM_ADDR` 实际限定指向的数据为只读；CPU 替身的 `using` 定义却限定指针本身，掩盖了问题。可写 profile 区应显式使用 `__gm__ uint8_t *const`，替身同步采用指针宏语义。

接收侧 `PayloadReady(slot, flags)` / `ClearFlags(slot, flags)` 又暴露了地址空间遗漏：`slot` 来自 GM，但形参曾使用普通 `const uint8_t *`，`const_cast<uint8_t *>` 还试图去掉 GM 地址空间。沿槽位地址、Flag 读取/清零和转发目标始终保留 `__gm__`，只固定指针时写 `__gm__ uint8_t *const`；`CommArgs` 自身保留 const，读取其中的可写 peer 指针无需去除 const。普通 CPU 替身将 `__gm__` 置空会漏报此类错误；`test_server_dedup_address_space` 用 Host Clang 独立地址空间对完整生产入口做 profiling=0/1 语法编译，未修复源码可复现这两处调用错误，修复后通过。可运行 `python3 tests/ep/python/test_server_dedup_address_space.py`；缺少 Host clang++ 时显式跳过。此检查验证 C++ 地址空间/限定符，不证明 CANN API、设备行为或性能。接收头文件同时列入外部 kernel 构建依赖，防止只改头文件却复用旧产物。

第一跳 Flag 初始化曾在 `Duplicate<float>` 处报 `no matching function`：[官方 asc-devkit v9.1.0 接口](https://gitcode.com/cann/asc-devkit/blob/e5451c7feba11f9e259f4fbc19c4bf82f8c0496c/include/basic_api/kernel_operator_vec_duplicate_intf.h) 的位掩码参数为 `uint64_t mask[]`，不能传 `const uint64_t[]`。应使用可写局部掩码，仍仅初始化每个 512B 块中的 32B Flag；CPU 替身也必须保持该重载的参数限定符和整数宽度。收紧替身后旧调用可复现同一编译错误，修复后用现有 sender/count/profile 测试检查包内容、边界和计数；该检查不替代真实 CANN 编译或 A5 运行。

- 环境配置只读，不用追加参数/备份恢复方式传递 case；那会造成双份配置、覆盖用户修改和多机不一致。
- 核心参数由调用方提供，批测只组合 case，不隐式编译或同步。提交验收按实际需要决定部署步骤。
- checkpoint 只在提交验收入口选择；普通运行使用当前完整实现，结果记录 checkpoint=null。
- 只对受影响的配置传递、结果解析和输出做脚本行为测试，不重复旧硬件验收。

下面保留历史实施记录作为排查参考；其中旧七步配置操作以本页上述三层入口为准。

## 维护与验证边界

- hostfile 属于一次性部署配置，环境入口只能读取和提示错误。曾自动生成 localhost 默认文件，这会在 Mutagen 等同步流程中引入错误清单传播的风险，因此已撤销自动创建；回归覆盖缺失文件不落盘、已有多机清单不变、自定义路径缺失及空文件拒绝。
- `git -C <子模块目录>` 会向父目录查找仓库。普通同步遗漏子模块 Git 元数据时，以子模块 commit 查询父仓库会报 `fatal: not a tree object`。源码清单直接枚举实际文件；只有当前目录有 Git 元数据时才尝试读取顶层 HEAD 标签，不遍历子模块对象库。回归须覆盖普通复制、缺失子模块元数据、空 spdlog 和固定提交导出，不要求服务器补 `.git`。
- 不能由 `.gitmodules` 或 CMake include 路径推断依赖是必需的。此前源码复制预检错误地要求 spdlog 头文件，阻断了已有的无 spdlog 日志路径；应核对实际 include 条件、链接目标及替代实现。spdlog 子模块和可选日志封装分别来自 `b063b61`、`b6c88d8`（2026-05-29），均早于本次 Server Dedup。回归需覆盖空子模块仍进入生产配置阶段，并实际编译、运行日志替代实现；这不代替 CANN/NPU 验证。
- 导出检查必须遵循公共接口的链接约定。`tilexr_ep.h` 中两个 Dispatch 入口声明为 `extern "C"`，`nm -C` 也不会给它们加参数括号；原先匹配 `TileXRMoeEpDispatchMemoryServerDedup(` 会在正常导出时误报缺失。现在读取 POSIX 格式的原始动态符号，精确匹配已定义的全局函数名；相似名称、C++ mangled 名、未定义引用和数据符号不能替代接口。真正缺失时显示本次库路径和 `symbols.log`，便于确认产物来源。
- 普通文件清单排除 `.git`、部署配置、build/install/run、Python 缓存、env 等环境或生成物，以及根级 `reference/`、`third_party/` 比较目录；包含实际子模块源码、文档和测试脚本。源码只用于列出同步路径，不校验内容，也不通过文档改动阻断运行；安装包的 build_id 和产物摘要仍用于拒绝旧包或不完整分发。旧源码清单的摘要字段在迁移时忽略，不把它升级为构建依赖。
- 比较目录排除规则必须限定目录层级。无 Git 四入口验证曾因全层排除 `reference` 漏掉 `src/moonep/planner_v2/reference/ep_plan_reference.cpp`，导致 CMake 找不到必需源文件。现在只排除根级 `reference/`、`third_party/`，保留 src/tests 内同名目录；回归复制真实 Planner 源并检查其可分发路径，不能用带导出清单的测试代替普通目录枚举。
- 同一份二进制的符号/RUNPATH 不需要在每个节点重复检查，本流程把它们和运行库检查统一放在生产构建完成后执行一次。各节点安装文件摘要相等不证明它们的 CANN/driver 环境相等；本次取消了远端运行库预检，实际节点执行仍按退出、结果和日志边界报告。
- 固定构建目录清理前必须确认它及安装目录没有越界或 symlink；开始新构建立即作废旧 manifest/同步凭据。否则编译失败后继续 sync/run 可能把上次成功包当作本次结果。日志目录保留独立 build_id，用失败重建行为测试验证此边界。
- 解析后的 hostfile 和有效配置决定同步记录是否失效；不要对配置文件原始字节做签名，也不要用去注释正则解析可执行 Shell。环境加载允许重新配置，同时去重 PATH/LD_LIBRARY_PATH，取消操作使用不依赖完整 CANN 环境的最小入口。
- CTest 注册不等于安装。测试构建成功但未安装时，同步后的节点不能运行；新增测试必须有安装规则，构建脚本核对全部注册命令的可分发路径。本次补齐五个已有测试的安装规则。
- CMake 测试清单可能引用构建机绝对路径。分发时将 C++ 程序映射到安装目录、Python 测试映射到源码目录，用实际子进程执行验证，不能复制构建树后假定远端可用。
- 父进程退出或 SSH 退出不代表测试子进程退出。节点设置自身期限，各测试使用独立进程组，TERM 后必要时 KILL；只清理本次拥有的进程。身份无法确认、清理或日志收集不完整时，整体不得输出 PASS；同时保留已完成测试状态和首个失败，不能用清理/收集错误覆盖它。
- `chain-single` 使用 MemoryDomain 的实际 8MiB 数据区加 4MiB flag 区，不能套用旧 Dispatch 的 2GiB 双分区公式拒绝有效 communicator。测试比较真实 Host/device CommArgs 的身份、peer 指针和分配边界；这不证明 peer-memory 数据传输。launch 或同步失败且轮末完成未确认时，不手工释放仍可能被设备引用的 buffer/comm/stream；进程非零退出，由现有有界进程组清理回收，不能打印单轮 PASS。
- 最新设计可能在工程验证期间继续更新。此次 `group` 从 BS 扩至 8*BS 行后，旧 helper 会少分配 7/8；容量测试先复现旧实现失败，再核对 K=1/6/8/16/32 的字节数及旧容量被 caller helper 拒绝。只同步已确认的公共容量，后续返回槽位、超限策略和 Combine 算法仍不提前实现。
- 连续多轮测试不能以成功提交次数代替完成证据。批内保持连续提交，只有批末同步和 Host barrier 均成功才确认整批 completed；中途失败停止提交并保留预热/测量计数，不提前释放可能在用的资源。consumer 行为测试检查同批资源复用、无中间同步、提交/同步失败和旧 barrier 报文；Host runtime 测试单独验证 magic，breakdown 再核对设备最后一轮 generation。
- 空 kernel 的多轮打点覆盖同一批记录，只能汇总最后一轮，不能伪称多轮设备样本统计。performance 从每 rank 独立 msprof 目录检查样本数、正耗时与单 stream 唯一开始时间，再剔除预热；仅检查数量会让重复样本补掉缺失轮次。breakdown 的 ticks 换算必须有目标平台官方依据，不能套用其他芯片频率。替身检查覆盖字段/统计和失败行为，实际采集与打点可见性仍需 CANN/A5 验证。
- SSH 建连前计算相对运行预算，远端建连后才开始计时，会延长集群执行阶段。dry-run 回归曾在执行预算 10 秒、SSH 延迟 5 秒、rank 运行 7 秒时于 13.30 秒误报 PASS；控制端现在等待节点结果时同时检查统一截止时间，到期主动调用原有跨节点取消，再收敛退出和收集日志。回归使用传输替身和真实子进程，不作为目标 SSH 集群证据。
- 退出失败与日志损坏可能同时发生：解析非 UTF-8 或损坏 JSON 时，必须附加解析错误，保留已有退出码及清理失败。构建、同步、运行任何阶段遇到清理失败都保留操作锁，避免下一轮删除仍被进程使用的固定构建目录；普通编译失败且进程已退出则释放锁。组合故障回归验证这两个边界。
- 构建、同步和运行使用本目录锁，避免相互覆盖。异常断电可能留下锁；这种异常恢复必须先确认没有活动进程，不自动删除未知任务的锁。
- `TILEXR_CANN_ENV_SCRIPT` 是显式环境选择，`common_env.sh` 不再用仓库内另一套 CANN 环境覆盖它。没有设置此变量的既有调用保持原来的环境发现规则。
- 传输支持带空格的目录，因此环境入口及公共工具中的 `dirname`、`realpath`、`source`、目录创建也必须引用路径；只测 tar/SSH 引号不能证明真正的环境加载可用。远端 watchdog 覆盖环境加载和节点入口，不能仅对测试程序设置超时。
- 前三个 checkpoint 不触达设备通信，不需要执行 HCCL Test。`chain-single` 的设备建链调试按 AGENTS.md 复用匹配环境的一次性资格记录，不将其增加为每次测试的预检；不能把 Host PASS 当作通信基线。

脚本维护测试：`python3 -m unittest discover -s tools/validation -p 'test_*.py' -v`。真实 C++ consumer 的 Host 生命周期测试为 `python3 tests/ep/integration/test_server_dedup_chain.py -v`，编译实际 consumer 并使用 ACL/comm/launch 替身，覆盖 FP16/BF16 各 16 个进程、真实 TCP barrier、失败边界和清理；它不计入每节点 17 项 Host 测试。这些是开发者测试，不是用户七步流程之外的必执行命令。真实 CANN 构建和真实 SSH 集群通过情况必须以服务器运行日志为准。

维护构建/续跑测试夹具时，模拟安装应使用实际 CMake 命令中的构建目录和安装前缀，
目录保护测试应在本次配置的真实构建路径上放置文件或符号链接。构建目录按 profiling/dedup
隔离，生产安装目录仍共用；切换模式不能删除其他配置的构建缓存，但必须使旧 manifest 和同步凭据失效。
有效 manifest 夹具必须包含 `server_dedup_enabled`、`server_dedup_protocol=2` 和
`server_dedup_core_config`；负例应从有效夹具中单独破坏目标字段。旧路径和缺失协议字段曾使
44 个失败项在到达目标校验前就失败或误判目录行为，修正夹具时应保留 ABI、摘要、模式、续跑完整性和目录保护断言。
这些脚本测试不执行 NPU，不能代替设备正确性与性能验证。

以下三段保留历史版本的开发证据，不代替本次精简后的回归。2026-09-09 初始实现验证：Windows/MSYS2 上 41 项脚本行为测试中 40 项通过，1 项因文件系统未创建真实 symlink 而跳过；PowerShell 5.1 同步入口通过模拟传输执行了实际导出、Bash 解包、刷新和拒绝覆盖检查。全新且路径含空格的部署目录中，四个服务器入口以 `mode=host` 完成实际 C++14 Debug 构建、安装和 17/17 Host/Python 测试。CANN 生产构建、真实 SSH 集群同步及 NPU 执行未在本地验证。

同日普通文件同步修复：临时子模块移除 `.git` 后复现 `fatal: not a tree object`。修复后 52 项脚本回归中 51 项通过，1 项因 MSYS 未创建真实 symlink 而跳过；包含普通同步清单跨模拟节点分发及结果摘要检查。无 `.git`、无导出清单且路径含空格的普通复制目录中，首次构建和修改源码后的重建均通过四个服务器入口，每轮 17/17 Host/Python 测试成功；用 Git 拦截程序确认两轮服务器流程均未调用 Git，hostfile 字节不变。hostfile 保持手动配置，不再自动生成。本地没有 CANN/NPU，生产编译及真实 SSH 集群验证仍需以服务器日志为准。

同日可选 spdlog 修正：新增回归先复现 production 模式被空 spdlog 目录阻断，移除预检后确认能进入生产 CMake 配置调用（测试在调用边界停止，不代表生产构建成功）。52 项脚本回归中 51 项通过，1 项 symlink 跳过。在临时目录仅复制真实 `tilexr_log.h`，不提供 spdlog 头文件、不定义 `TILEXR_LOG_DISABLE_SPDLOG`，按项目默认 GNU C++14 模式编译并运行现有日志测试通过，编译断言确认使用自动替代路径；现有 fake-spdlog API 测试亦通过。MSYS 严格 `-std=c++14` 下现有测试的 POSIX `setenv`/`unsetenv` 声明不可见，故使用与 CMake 一致的 `-std=gnu++14`；未修改生产日志代码或测试源码。CANN 生产编译与 NPU 执行未在本地验证。

## 七步流程检查审计与落实（2026-09-09）

以下为 2026-09-09 的历史审计快照；表中的“每节点全部 Host/Python 测试”和“当前 17 项”描述当时实现。
现在的测试数量以安装 manifest 为准，执行归属以本文“按需执行 Host/Python 回归”为准。

> 下列历史审计表中的 hostfile device 字段、全配置同步签名及 run_case 参数描述已由上文“整服务器调度与参数归属”替代。

原审计源码版本为 `04e16e98cf2c3502d765436c87be43616a172192`。保留以下 50 项 ID 便于逐项核对；“原审计位置”和“原操作”描述该历史提交，代码链接固定到该提交，不随当前行号变化。“本次决定与当前行为”描述本次精简后的实现。范围覆盖根目录入口、环境/源码/工作流工具、可选 PC 导出和 CMake；没有访问实际服务器、用户 Mutagen 配置或已安装 CANN 环境脚本，也没有测量真实集群耗时。

本次已执行的决定包括：取消全部源码内容校验；符号/RUNPATH/运行库集中到每次生产构建末尾一次；固定目录清理重建；保留目录检查；按配置语义判断同步；保留当前全部测试并改为必需集合检查；失败按边界分类。测试项和每节点测试范围是否精简继续标为待定。未涉及的工具约束和兼容行为保留现状。

### 已落实的精简

- `nm`、`readelf`、`ldd -r` 在每次生产构建完成后各执行一次，后续只校验安装包身份与产物摘要。
- 七步入口不调用 `npu-smi`/`lspci`；保留真正的 CANN 加载，去掉无消费者的路径设置和重复 PATH 项。
- hostfile 注释/空白及不改变实际配置的 Shell 注释不使同步失效。
- 源码 SHA256、源码清单内容比较、构建前后源码一致性检查和源码总摘要全部取消；文档继续可以同步，但其变化不阻断验证。
- 保留当前全部 17 项测试，检查必需集合及安装状态，允许总数增加；测试精简待定。
- 固定 `build/validation/` 干净重建；独立日志、旧安装凭据失效和目录保护保留。

### 第 1、2 步：固定版本及 PC 同步

| ID | 原审计位置 | 原操作或检查，失败/影响 | 本次决定与当前行为 |
| --- | --- | --- | --- |
| P01 | PC `git fetch`、`git switch --detach` | 获取并选择验证版本；不自动检查 PC 是否有未提交改动 | 保留版本选择。Mutagen 同步实际文件；提交号只作来源标签，不证明当前文件与该提交相同 |
| P02 | [source.py:165](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/source.py#L165) `pack` | 仅 `push.ps1` 路径执行：初始化全部递归子模块，导出 HEAD 原始字节；未提交改动不导出，子模块获取失败则停止 | 保留可选完整 HEAD 导出及递归子模块初始化；导出清单改为路径列表，不生成内容摘要。Mutagen 路径不需要此入口或可选 spdlog |
| P03 | [push.ps1:12](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/push.ps1#L12) | 校验 host/user 格式、Linux 绝对路径并引用 Shell 参数；禁止换行、路径越界等 | 保留参数引用、格式和路径约束；当前仍不支持 user@host、IPv6 等写法 |
| P04 | [push.ps1:31](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/push.ps1#L31) | 对导出、scp、SSH 返回码逐一判失败；生成并传输临时 tar.gz，最终清理临时包 | 保留可选入口的传输和返回码处理；普通 Mutagen 同步不叠加此入口 |
| P05 | [push.ps1:45](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/push.ps1#L45) | 拒绝根目录、路径中的 symlink、Git checkout、无管理标记的非空目录和活动/残留锁 | 按决定保留全部部署目录、symlink、Git checkout 和锁检查，不自动接管其他非空目录 |
| P06 | [push.ps1:61](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/push.ps1#L61) | 校验归档是普通文件、清单与归档文件集合相等、不得覆盖 hostfile 等本地配置；移除旧清单中的退役源码 | 保留普通文件/归档成员集合、配置和路径保护；旧摘要字典仅取路径，刷新时不检查源码内容 |
| P07 | [push.ps1:100](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/push.ps1#L100)、[source.py:119](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/source.py#L119) | 解包后验证每个源码文件 SHA256；首次构建开始时还会再验证一次固定提交清单 | 已取消解包后的源码 SHA256 及构建前固定清单内容验证；不再调用 source verify |

### 第 3 步及后续入口：环境与配置

| ID | 原审计位置 | 原操作或检查，失败/影响 | 本次决定与当前行为 |
| --- | --- | --- | --- |
| E01 | [set_env.sh:14](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/set_env.sh#L14)、[build_all.sh:3](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/build_all.sh#L3)、[sync.sh:3](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/sync.sh#L3)、[run.sh:3](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/run.sh#L3)、[workflow.py:30](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L30) | 显式第 3 步、三个入口和每个 SSH verify/node/cancel 会话重新 source 配置与公共环境 | 已精简为可重新加载的轻量环境；远端 verify/node 仍加载实际环境；cancel 使用最小入口，不依赖 CANN/hostfile 加载 |
| E02 | [set_env.sh:32](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/set_env.sh#L32)、[workflow.py:134](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L134) | Shell 和 Python 都检查 mode/checkpoint，只允许 host/production 和已实现的 ABI/Host checkpoint | 保留已实现 mode/checkpoint 范围约束；失败明确归为 CONFIG_FAILED |
| E03 | [set_env.sh:37](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/set_env.sh#L37)、[common_env.sh:55](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/scripts/common_env.sh#L55) | production 检查 CANN 脚本可读并 source；可读性检查重复。`TILEXR_CANN_VER=9.1.0` 只是赋值，不是安装版本验证 | 保留可用 CANN 环境加载与公共入口独立调用的可读性检查；轻量 host 模式不加载 fallback CANN。变量赋值仍不是安装版本证明 |
| E04 | [set_env.sh:48](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/set_env.sh#L48)、[workflow.py:566](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L566) | 查找 Python 并要求版本至少 3.9 | 保留 Python 3.9+ 要求和自动工具查找；不增加用户手动验证命令 |
| E05 | [workflow.py:102](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L102) | hostfile 必须存在且有 1..64 台主机，检查字段、重复 host、重复 device ID 及 0..255 范围；为空/格式错误时失败 | 保留当前 hostfile 格式、上限、重复项和 device 范围检查；只读取、不创建文件，不把 device 字段当作实际设备验证 |
| E06 | [workflow.py:136](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L136) | JOBS 限制 1..256，TIMEOUT 限制 30..180 秒；SSH_USER 必须匹配限定格式；hostfile 解析后必须在工作目录内 | 保留当前输入范围、SSH_USER 和 hostfile 路径约束；它们不产生新增用户命令 |
| E07 | [common_env.sh:21](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/scripts/common_env.sh#L21)、[common_util.sh:47](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/scripts/common_util.sh#L47)、[common_util.sh:62](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/scripts/common_util.sh#L62)、[common_util.sh:104](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/scripts/common_util.sh#L104) | 探测 SoC、ops 名、设备数；每次 source 在 npu-smi 可用时执行 3 次 `npu-smi info`，解析失败可能调用 lspci；未设置单独超时 | 已取消七步 checkpoint 的 npu-smi/lspci 探测；其他公共环境调用保持原行为 |
| E08 | [common_env.sh:26](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/scripts/common_env.sh#L26)、[common_env.sh:88](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/scripts/common_env.sh#L88) | 创建 env/temp/util、run/plog；设置 MPI/HCCL Test 路径并向 PATH/LD_LIBRARY_PATH 添加工具目录 | 已精简轻量环境的 MPI/HCCL Test 和无消费者工具路径；保留日志/环境目录及必要 driver 库路径，重复 PATH 项去重 |
| E09 | [common_env.sh:78](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/scripts/common_env.sh#L78) | driver/kernel/inc 不可读时，用 CANN driver 头文件建立本地 driver-shim，并修改 ASCEND_DRIVER_PATH；链接仍指向真实 driver 库 | 保留 production 环境所需 driver-shim 兼容路径；重复 source 从原 driver 恢复后重建链接。host 模式不创建 shim |

### 第 4 步：构建与安装检查

| ID | 原审计位置 | 原操作或检查，失败/影响 | 本次决定与当前行为 |
| --- | --- | --- | --- |
| B01 | [workflow.py:161](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L161)、[workflow.py:573](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L573) | build/sync/run 使用目录锁，遇到活动节点锁或既有操作锁即失败；退出时释放 | 保留目录锁、活动任务保护和退出释放；异常残留锁继续需要先确认所属进程 |
| B02 | [workflow.py:254](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L254) | 强制要求 cmake、ctest、g++、make，并执行 cmake/g++ `--version` 保存日志 | 保留当前 cmake/ctest/g++/make 工具约束和版本日志；这些不是新的部署前置检查，版本输出也不是兼容性证明 |
| B03 | [source.py:142](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/source.py#L142)、[source.py:91](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/source.py#L91) | 构建前读取 Git/固定导出清单，或扫描普通文件并计算 SHA256；元数据不完整时使用 commit=null，不要求服务器 Git | 已改为 inventory 路径枚举，不读源码字节、不做 Git dirty/子模块对象校验；commit 明确未校验，旧或损坏来源清单不阻断 |
| B04 | [source.py:43](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/source.py#L43)、[source.py:119](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/source.py#L119) | 拒绝越界/绝对路径/.git 路径、源码 symlink、非法摘要及文件缺失/变化；只检查清单中已有文件 | 按决定保留越界、symlink 和普通文件路径保护；非法摘要、旧清单文件缺失/内容变化检查已取消 |
| B05 | [workflow.py:256](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L256)、[workflow.py:263](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L263) | 每次生成 UUID 新构建目录，生产包与 Host 测试各自 configure/build/install；二者均传 Debug | 已改为固定 build/validation，检查路径后删除旧编译内容再全量编译；生产/Host 仍分子目录并保留 Debug，日志独立 |
| B06 | [src/comm/CMakeLists.txt:30](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/src/comm/CMakeLists.txt#L30) | CMake 编译探测 PTO SDMA 头文件/API，检查 intrinsic 与真实 libnnopbase；不可用时关闭可选 SDMA | 保留可选 SDMA 能力探测及不可用时继续编译；不新增 PTO 或 spdlog 硬依赖 |
| B07 | [src/ep/CMakeLists.txt:5](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/src/ep/CMakeLists.txt#L5)、[CMakeDetermineCCECompiler.cmake:11](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/src/collectives/kernels/cmake/CMakeDetermineCCECompiler.cmake#L11)、[TileXRKernelArtifact.cmake:4](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/src/cmake/TileXRKernelArtifact.cmake#L4) | 生产 EP 查找 ccec/ld.lld、构建并安装四个 A5 kernel，检查 kernel artifact 的 CMake target/参数；CCE 测试文件仅把 WORKS 设为 1 | 保留当前 production 包及四个 A5 kernel 的实际编译安装；host 模式继续用于无 CANN 本地验证，不扩大设备通过声明 |
| B08 | [workflow.py:271](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L271) | 安装后检查 dispatch/combine/dispatch_memory/combine_memory 四个 kernel 文件存在 | 保留四个 kernel 安装文件存在性检查；不表示执行过 kernel |
| B09 | [workflow.py:216](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L216) | 原检查错误地匹配带 `(` 的名称；现用 `nm -D --defined-only --no-demangle --format=posix` 精确核对两个 Dispatch 的 C ABI 动态函数符号 | 已修复误报；真正缺失时仍失败并显示库路径与 symbols.log。每次生产构建完成后一次；sync/run/node 不再调用 nm |
| B10 | [workflow.py:220](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L220) | `readelf -d`，要求恰好一条 RPATH/RUNPATH 且含完整 `[$ORIGIN]` 字段 | 已集中到每次生产构建完成后一次；保留既有 $ORIGIN 加载路径规则，sync/run/node 不再调用 readelf |
| B11 | [workflow.py:224](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L224) | `ldd -r` 拒绝 not found/undefined symbol/devlib，并要求 libascend_hal 解析到配置的真实 driver 路径（兼容 shim） | 按决定每次生产构建完成后仅执行一次 ldd -r；sync/run/node 不再检查运行库，其他节点库环境兼容性不在此证据内 |
| B12 | [workflow.py:281](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L281) | `ctest --show-only=json-v1` 只列出命令；将 Python/C++ 测试映射到分发路径，检查安装文件存在；不接受其他命令形态 | 保留注册清单解析、命令映射与安装文件检查；CTest 清单解析失败归为 LOG_PARSE_FAILED，命令本身不执行测试 |
| B13 | [workflow.py:296](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L296) | 要求恰好 17 项且包含四个 Server Dedup 测试；新增无关测试同样导致构建失败 | 已改为必需测试集合、唯一名称和安装检查，允许新增项；当前全部 17 项仍必需，测试项精简待定 |
| B14 | [workflow.py:299](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L299) | 编译后再次读取所有记录源码并比 SHA256，防止构建过程中 Mutagen 改动源码；计算安装文件摘要，写 manifest | 已取消构建后源码 SHA256 比较；保留安装产物摘要及构建 manifest，不保证源码在构建期间未被同步修改 |
| B15 | [workflow.py:306](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L306) | 拒绝 install/validation 及父目录 symlink，移除旧安装目录后以本次 stage 替换，写源码清单及管理标记 | 保留目录保护及成功后替换安装包；开始新构建立刻作废旧 manifest 和 sync 记录，失败后不能沿用旧包 |

### 第 5 步：节点同步

| ID | 原审计位置 | 原操作或检查，失败/影响 | 本次决定与当前行为 |
| --- | --- | --- | --- |
| S01 | [workflow.py:177](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L177)、[workflow.py:325](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L325) | 确认 hostfile 首项是当前主机；依次尝试本机名称、IP/DNS，必要时 SSH hostname；第 6 步重复 | 保留首节点身份确认，避免把远端误作本机而跳过同步；当前既有探测顺序不变 |
| S02 | [workflow.py:326](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L326) | 多机才要求 ssh/rsync；固定 BatchMode、连接期限与保活 | 保留多机所需 ssh/rsync、免交互连接、连接期限与保活；不安装密钥或启动 NPU 检查 |
| S03 | [workflow.py:240](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L240)、[workflow.py:329](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L329) | 首节点验证 mode/checkpoint/CPU 架构、源码 SHA256、安装文件集合及 SHA256，再执行全部 runtime_check | 保留 mode/checkpoint/CPU 架构、build_id 和安装产物摘要；源码校验与全部 runtime_check 已从 sync 移除 |
| S04 | [workflow.py:330](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L330) | 删除旧 sync.json，只在本次所有节点成功后写新记录；记录 build_id 与配置摘要 | 保留同步前删除旧记录、全部节点通过才记录新 build_id 和有效配置的语义 |
| S05 | [workflow.py:332](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L332) | 仅同步清单中源码、安装包、hostfile/local 配置和管理清单；不复制 build、历史 run 和环境 | 已改用同步时的实际源码路径列表，包含文档而不校验内容；继续排除环境、缓存、历史日志并分发配置和安装包 |
| S06 | [workflow.py:346](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L346) | 远端只接受空目录或有 `.tilexr-validation-root` 的目录，拒绝 Git checkout/路径 symlink/活动锁 | 按决定保留全部目标目录所有权、symlink、Git checkout 和活动锁检查；未管理的 Mutagen 非空目录仍拒绝接管 |
| S07 | [workflow.py:360](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L360) | 同步前删除远端 install/validation；首节点没有 local 配置时删除远端同名配置，否则 rsync 覆盖配置及 hostfile | 保留受管理目标的旧安装目录清理与首节点配置分发；hostfile 不由 source 自动创建，所有权检查不放宽 |
| S08 | [workflow.py:375](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L375) | `rsync --checksum` 以内容判断已有文件是否需要更新；之后还执行 Python SHA256 验证 | 按决定保留 rsync --checksum 的传输选择；后续只做安装产物 SHA256，源码 SHA256 验证已取消 |
| S09 | [workflow.py:379](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L379)、[workflow.py:587](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L587) | 每个远端重新加载环境、运行 verify_build 的全部检查，再比较请求的 build_id；任一失败则整体失败 | 保留远端环境、build_id、CPU 架构及安装产物摘要确认；不再执行源码或 nm/readelf/ldd 检查 |
| S10 | [workflow.py:154](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L154)、[workflow.py:387](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L387) | 配置签名既含解析 host 列表，又包含 hostfile 原始字节 SHA256；local 配置也按原始字节摘要；含 timeout 等参数 | 已移除 hostfile/local 配置原始字节摘要，使用解析后的 hosts/devices 与实际生效配置；仅注释/空白变化不阻断 |

### 第 6、7 步：执行、取消与结果

| ID | 原审计位置 | 原操作或检查，失败/影响 | 本次决定与当前行为 |
| --- | --- | --- | --- |
| R01 | [workflow.py:474](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L474) | 首节点再次检查自身身份与 verify_build，再要求 sync.json 的 build_id/配置完全匹配；单机也必须先 sync | 保留首节点、安装包与 sync 凭据检查；凭据只关联 build_id 和有效配置，源码文字变化不使其失效 |
| R02 | [workflow.py:486](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L486) | 本机用 Python 子进程、其他节点经 SSH 启动一个 node；每主机执行一套 Host 测试 | 保留每主机执行全部 Host 测试；是否精简测试项或其他节点重复范围待定，device 列表仍不启动 NPU rank |
| R03 | [workflow.py:399](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L399) | 每节点保存 PID/starttime，设置 TERM/HUP 处理和期限，检查取消标记，再次 verify_build 并确认 build_id | 保留节点身份、取消标记、期限和安装包身份/摘要检查；移除源码及符号/运行库重复检查 |
| R04 | [workflow.py:421](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L421) | 按 manifest 逐个执行测试；非零退出失败；四个 Server Dedup 测试还要求完整 PASS marker 行 | 保留逐测试执行及 checkpoint marker；非零退出归 EXIT_FAILED，合法 FAIL marker 归 RESULT_FAILED，缺失/非法 marker 归 LOG_PARSE_FAILED |
| R05 | [workflow.py:64](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L64)、[workflow.py:82](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L82) | 各外部命令有独立进程组，退出/超时均执行 TERM，必要时 KILL；保留输出日志，非零显示尾部 | 保留所属进程组 TERM/KILL 和原始日志；区分启动、退出/超时与清理失败，不按 stderr 文本猜测结果 |
| R06 | [workflow.py:30](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L30)、[workflow.py:411](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L411)、[workflow.py:475](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L475) | 环境+远端命令 watchdog、节点最多 120 秒、控制端总期限默认 180 秒；为清理/收集预留时间；构建/同步另计 | 保留节点/控制端期限及清理收集预算；远端环境仍在 watchdog 内，源码内容扫描已经删除 |
| R07 | [workflow.py:445](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L445)、[workflow.py:504](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L504) | 首个启动/测试失败后向所有节点发取消；核对 PID starttime 防 PID 复用误杀，最多等待清理 8 秒；取消失败也计入总失败 | 保留首个失败触发取消、PID/starttime 归属核对和清理确认；cancel 已改最小入口，清理失败另记而不掩盖原错误 |
| R08 | [workflow.py:523](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L523) | 从各节点 rsync 当前 run_id 日志和 result.json；日志收集失败也使总体失败 | 保留日志/result 收集；失败单列 LOG_COLLECT_FAILED，保留已知测试状态，整体仍非零退出，不混作结果校验失败 |
| R09 | [workflow.py:463](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L463)、[workflow.py:543](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L543) | 检查 run_id/build_id、mode/checkpoint、PASS、cleanup、测试集合无缺失/重复且逐项通过，再检查总期限；写 summary | 保留运行身份、必需结果集合/唯一性、状态和清理检查；分类记录首个边界与副错误，日志无法解析不伪造结果 |

### 精简后的检查次数

当前调用链没有 `md5sum` 或 `sha256sum` 命令，也不再计算源码文件或源码清单的 SHA256。Python `hashlib.sha256` 仅保留在安装产物检查；`rsync --checksum` 用于选择需传输的文件，其算法由实际安装的 rsync 决定。

下表按 **Mutagen/普通复制、production、正常成功、N 台主机、一遍第 3～7 步**统计当前调用路径。不计 PC 导出、重试、失败取消、外部 CANN 脚本内部动作、rsync 内部工作和测试内部检查；这是代码调用次数，不是服务器实测耗时。

| 操作 | 首节点 | 每个其他节点 | N=8 总数 | 当前状态 |
| --- | --- | --- | --- | --- |
| 加载 set_env/common_env | 4：显式 source、build、sync、run | 2：同步 verify、运行 node | 18 次轻量加载 | 保留会话环境，去掉无消费者的探测/路径设置 |
| 本流程的 `npu-smi`/`lspci` | 0 | 0 | 0 | 已取消当前 checkpoint 的探测 |
| 源码路径枚举 | 2：build、sync | 0 | 2 次目录遍历 | 只列路径，不读取源码内容 |
| 源码 SHA256/清单总摘要 | 0 | 0 | 0 | 全部取消，原来为 19 次源码扫描 |
| 安装产物 SHA256 | 4：建清单、sync 本机、run 控制端、run 本机 node | 2：sync verify、run node | 18 次扫描 | 保留安装包分发和运行身份检查 |
| `nm` + `readelf` + `ldd -r` | 构建完成后各 1 次 | 0 | 1 组，共 3 个命令 | 每次生产构建仅一次；host 模式不执行 |
| Host/Python 测试 | 当前 17 项 | 当前 17 项 | 当前 136 个测试进程 | 全部保留；测试项及每节点范围精简待定 |

源码内容一致性已经按决定退出验证判据。来源 commit 仅用于记录，build_id 和安装文件摘要标识实际分发包；源码变化本身不使包失效，不自动触发重建。目录遍历用于确定同步范围，不能称为源码完整性校验。

### 当前 17 项测试的范围

| 测试集合 | 数量 | 位置 | 对本 checkpoint 的判定 |
| --- | --- | --- | --- |
| `test_tilexr_ep_server_dedup_{protocol,layout,host_validation,public_abi}` | 4 | [tests/ep/CMakeLists.txt:247](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tests/ep/CMakeLists.txt#L247) | 本 checkpoint 必需，验证协议/布局/Host 参数/公共 ABI，不运行设备去重 |
| `test_tilexr_ep_{layout,expert_ids,host_validation,kernel_launch}` | 4 | [tests/ep/CMakeLists.txt:370](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tests/ep/CMakeLists.txt#L370) | EP 既有功能回归，当前全部保留为必需项；是否精简及每节点重复待定 |
| `test_tilexr_moonep_planner_public_abi`、`test_tilexr_ep_plan_{layout,reference,algorithm,downstream,host}`、`test_tilexr_moonep_planner_compat` | 7 | [tests/ep/CMakeLists.txt:374](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tests/ep/CMakeLists.txt#L374) | Planner 回归，当前全部保留为必需项；是否精简待定 |
| `test_dispatch_memory_trace`、`test_msprof_kernel_stats` | 2 | [tests/ep/CMakeLists.txt:27](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tests/ep/CMakeLists.txt#L27) | Python 日志/统计工具回归，不执行 profiler 或采集 NPU 性能；当前全部保留为必需项，是否精简待定 |

### 已确认取消或没有执行的流程

`04e16e9` 已取消 spdlog 缺失硬阻断；`67168ba` 已取消环境入口自动创建 hostfile，并支持普通复制目录不带 Git 元数据。正常服务器路径不执行 Git clone/submodule、HCCL Test、NPU 空闲检查或依赖安装。当前空算子不执行算法通信，只有 performance 模式调用 msprof。`common_env.sh` 定义的安装函数/权限修复函数不等于已调用，不能仅通过关键词命中把它们列成必执行步骤。

原审计的定点验证在临时目录完成：模拟 npu-smi 确认旧版单次 source 调用三次；hostfile 只加注释导致旧 signature 变化；普通文档修改触发旧 source mismatch；模拟 readelf 输出 `$ORIGIN:$ORIGIN/extra` 被既有 RUNPATH 规则拒绝。这些是 `04e16e9` 的历史证据，前三项行为已在本次精简中移除。

本次精简后验证：Windows/MSYS2 上 87 项脚本行为测试中 84 项通过，3 项因 MSYS 未建立真实 symlink 而跳过，分别涉及构建目录保护、driver shim 和源码路径。覆盖源码清单及实际 PowerShell/Bash 部署、环境重载、单次库检查调用顺序、固定目录构建、必需测试集合、模拟双节点执行和各类故障；退出叠加日志损坏/清理失败、build/sync 清理失败保留锁的回归先复现问题，再通过修复。生产构建调用顺序使用模拟安装产物，不作为 CANN 编译证据。

无 `.git`、无来源清单、空 spdlog 且路径含空格的普通复制目录中，两轮 `source set_env.sh` → `build_all.sh` → `sync.sh` → `run.sh` 完成真实 C++14 Debug 编译，每轮 17/17 Host/Python 测试通过；第二轮清除固定目录旧产物，build_id 改变，Git 拦截器确认服务器流程未调用 Git，hostfile 字节不变。向真实参与构建的 `tests/ep/unit/test_tilexr_ep_server_dedup_layout.cpp` 注入编译错误后，报告 `BUILD_FAILED`，旧 manifest/同步记录失效，sync/run 均拒绝旧包。CANN 生产编译、真实 SSH 集群、Linux 真实 symlink 和 NPU 验证仍未在本地执行。

同日 C ABI 导出检查修复：正确 C 符号的命令输出夹具先复现旧检查误报，修复后完整脚本回归 89 项中 86 项通过、3 项因 MSYS symlink 限制跳过。新用例覆盖两个接口独立缺失、相似名称、C++ mangled 名、未定义/局部/数据符号及正确导出后继续检查 RUNPATH/HAL。另使用现有测试 ACL 头文件夹具，以 C++14 重新编译当前两个 Dispatch Host 源文件；真实 `nm --defined-only --no-demangle --format=posix` 的对象符号均为不带括号的 `T` 函数名。该对象编译证据不代表 CANN 生产共享库的链接、动态符号表或 NPU 执行已在本地验证。

### Kernel 计时与关键结果输出

人工测试与提交验收只采集和统计 kernel 耗时，不采集、校验或输出 Host submit/sync/barrier/batch_total 时间。stream 同步与 Host barrier 仍按整批执行，只移除计时。成功/失败与 kernel 统计前后使用空行和 `==========` 分隔；functional 不输出耗时。当时 performance 的 mean_us 使用各 rank 样本中位数的算术平均；现行模式 1 已改为最后最多 50 轮、按 server 均值汇总，详见前文。此变更仅需验证结果解析和输出，不重复执行已通过的硬件检查点。

关键结果分隔线统一为 32 个 `=`。PASS 或 kernel 性能前先输出两行实际 case 配置：算子/模式/测量与预热轮次，以及 BS/H/TopK/专家数/dtype/卡数/量化模式。信息取自本次 summary，终端与 summary.log 一致；当前明确标注 Dispatch server-dedup 空 kernel，不代表已实现 Combine 或去重数据面。

批次结论只在至少执行一个 case 且全部返回成功时显示 `Batch PASS cases=N`。构建、同步或任一 case 非零退出立即停止并保留非零退出码；空清单不能显示 PASS。核心还检查 summary.status/errors，不能仅凭外层进程 exit=0 把失败结果标成通过。Shell 的 set -e 不能替代显式结果判断。

### 2026-09-15：Dispatch 双窗复用约束

单份通信区的轮末握手已移除，不能只修改 Token 地址而漏掉 cnt/cumsum/prefix、relay 或 counter；否则下一轮写入仍可能与上一轮清理冲突。现在统一使用 `peerMems[rank]+(magic%2)*4 GiB`，保留本地 MTE 完成、全核 SyncAll 和当前窗口 counter 清零，不新增 UB。

连续 Dispatch 要求同 comm 单 stream、各 Rank 一致的调用/shape 次序，且每轮发布全部 count（包含零计数）。快 Rank 完成 N+1 需要收到每个 Rank 的 N+1 count，其同 stream 顺序证明这些 Rank 的 N 已结束，故 N+2 可复用 N 的窗口。后续 Dispatch/Combine 交替会让相邻 Dispatch 使用相同 parity，必须核对实际 Combine 通信、零流量及 relay 清理的依赖；当前未实现 Combine，不据此宣称其生命周期已验证。普通 GM 的 group/assistEx/sourceMask 等输出仍须保存到消费者完成。

本次 8 个相关 Host 目标编译和对应回归、5 项 integration 替身、2 项新旧 trace 分段检查通过，覆盖双窗地址、inactive 窗口保护及最终窗口读取；独立并发/地址 review 无未解决发现。blue SSH 编译前探测返回 `banner exchange: Connection to UNKNOWN port -1: Connection refused`，未进入 CANN 9.1/A5 OFF/ON 编译；记录位于本地 `build/server-dedup-pingpong-verification/`。未运行 NPU，历史 4 GiB 或单窗结果不代表本次 8 GiB 双窗实机通过。

### 2026-09-15：8 GiB 迁移后的 ABI 测试夹具

若执行在 `test_tilexr_ep_server_dedup_public_abi` 报 `ABI consumer propagates submit result and rejects non-A5`，先核对该测试的模拟 CommArgs 容量。双窗实现要求至少 8 GiB，旧夹具仍填写 4 GiB 会在提交替身之前返回参数错误，使原本检查提交返回值的断言失败；随后的 profiling 报告缺失是流程提前中止的结果。容量夹具应引用 `kEpServerDedupPeerBytes`，通信域契约变化时把公开 ABI consumer 纳入相关回归。

此问题已在当前 Host 目标重编译后复现同一断言失败；只修正夹具容量后重新编译，原用例通过。本次未修改 kernel 或生产 Host，不代表 32P 多轮或 NPU 执行已经验证。

## 2026-09-16 Token prefix 编号验证

源码为 `a77ac87` 加本次修改，第一跳 t=row、groupOut 按 capacityRows 分配，删除原子取号和
counter 清零，保留完成屏障；Combine 尚未实现。主规格记录新的字段/容量契约及后续返回区预算。

- blue 上真实 CANN compiler `Version=9.1.0`、bisheng clang 15.0.5（2026-05-18），
  A5 `dav-c310-vec` 的 profiling OFF/ON Server Dedup kernel 及 `tilexr-ep` Host 库均构建成功。
  已核对最终 501 个生产/依赖文件与编译快照摘要一致。本地未安装 cmake/g++，Host C++ 检查也在 blue 执行。
- 12 项 `ctest -R server_dedup` 及 2 项 kernel launch CTest 通过。新增高编号场景验证
  t=32..47 超过旧 8*BS=16 上限、跨接收核关联、同卡终端继承、三轮 magic、group 空洞及污染 counter 保持不变；
  既有量化/非量化、乱序 ready、跨卡转发和 profiling OFF/ON 替身覆盖继续通过。
- Python chain/workflow 最终 104 项通过；本地 chain/trace 最终 27 项通过（含重叠 chain 测试）。
  完整 workflow/chain/trace/build 组合在审查前为 135 项通过；审查后只改动链路编号上界并新增一项边界测试，
  已刷新受影响 chain/workflow。独立审查发现的汇总层旧 `8*BS` 上界已用失败复现后修正为 C，
  覆盖 K<L、K>L、合法高 t、C-1 及 C/-1 拒绝。历史 trace 仍可读取，新 trace 不伪造 Atomic/CounterEnd 样本。
- C++ consumer 集成 21 项中 20 项通过；`test_repeated_calls_reject_wrong_bank_readback` 失败，
  原始 `a77ac87` 上单独复跑得到相同失败：注入读取 API 返回 92，程序失败退出 1，而测试预期返回 0 并发布 INVALID。
  这是已有的 API 失败与数值失败预期差异，未作为本次优化修复；不声称 consumer 套件全通过。

源码快照、摘要、diff、工具链及完整日志保存在本地 `build/prefix-index-verification/`（不提交）。
独立审查及主审已核对 owner、稀疏容量、终端继承和旧 counter/profile 消费者。
这些验证不证明 A5 多卡异步运行或性能收益；用户侧必须重新 build/sync 后在相同 case 比较完整 Dispatch。
既有 HCCL 环境基线无需因本次代码改变重复运行。

## Server Dedup GM tiling 验证边界

当前参数契约为 `r11-independent-gm-tiling`：Kernel 签名和实际 Runtime argsSize
均为 120B。32B tiling 由 communicator 在独立 GM 初始化/缓存；magic 独立传参，
epHandle 尚未引入。必须同时更新 libtile-comm、owning Host 库及 A5 Kernel，执行
`build_all.sh` / `sync.sh` 更新 manifest；旧 r10/160B 提交包不满足当前契约。

Host launch 替身检查 hasTiling=0、无附带内容、指针指向独立 GM，并通过真实缓存类
验证首次同步初始化、重复配置无复制、旧配置内容不被覆盖、不同通信域隔离及失败清理。
保留量化、profiling 槽位、窄化前拒绝和 64 位 magic 检查。缓存保留所有不同配置直到
communicator 销毁，不能用此 Host 证据声称 NPU 搬运或性能已通过。
编译验证需包括 `test_tilexr_ep_kernel_launch` / `_profile` 和公共 API/Host 校验，
以及真实 CANN 9.1/A5 Kernel、owning Host 库和通信库。NPU 验证应保留多轮、不同 shape
和 profiling OFF/ON，用实际结果验证 GM 参数读取；参数字节减少不等于耗时改善。

2026-09-18 验证：基于 `83208c8` 加本次独立 GM 修正，blue 的 CANN 9.1.0 / Bisheng
Clang 15.0.5（2026-05-18）完成 A5 `dav-c310-vec` Kernel、`tilexr-ep` 及
`tile-comm` 的 profiling OFF/ON 编译，最终清理状态调整也已重编。6 项相关 Host CTest
（包含生产缓存类和失败路径）、16 项 build Python 回归、98 项 workflow Python 回归通过。
988 个构建输入的 SHA256 与最终 blue 快照一致，独立审查未发现待修复缺陷。
证据保留在本地 `build/independent-tiling-verification/`。未运行 NPU；多 Rank 释放失败
一致性仅经调用链审查，不声称已完成实机故障注入或性能验证。
