# UniTest 验证记录

本文保留各阶段的构建与实机证据，供维护者追溯。每条结论只适用于所列源码、安装产物和环境，不能代替当前版本的验收。日常使用见 [UniTest 用户指南](../../tools/unitest/README.md)，当前架构见 [UniTest 设计](../specs/unitest.md)。

当前保留的最新记录覆盖 CANN 9.2 / Ascend950DT 两卡和内部 profiling 数据通路。真实多节点、CANN 9.1 实机、官方 profiler 样本完整性、异常/中断实机清理及校准后的时钟测量仍须分别验收。未执行的项不能由 Host fixture 或同机 NodeAgent 结果代替。

## 早期阶段记录

### 运行配置（逐项交付）：阶段记录

本项验证：config/CLI 字段优先级与整数边界 Host 检查通过；官方 CANN9.2
独立配置、C++14 编译、安装、安装后 help 与配置文件交付通过。
产物 `/home/h30059441/unitest-pr199-clean/logs/A-build.log`。
此阶段链接保留的已安装生产库，不宣称新生产打点或硬件矩阵已验收。

### correctness 检查点：阶段记录

本项验证：Host 调度 fixture 覆盖 100/64/逐轮检查点、首个 mismatch 和段提交失败后停止，
保留上次通过及失败检查点；官方 CANN9.2 C++14 编译安装及新 Worker 的 A5 八 rank
Dispatch correctness100 通过，检查点为 1/2/4/8/16/32/64/100。
证据 `unitest-pr199-clean/B-dispatch100/result.json`；不复用旧框架硬件结论。

### Host batch perf：阶段记录

本项验证：调度 fixture 验证预检/warmup/ready/batch/校验顺序，缺失 rank、无效耗时
由汇总拒绝；官方 CANN9.2 新 Worker 构建安装及 A5 八 rank Combine perf W5/N100/B3
通过，每 rank/batch 耗时为正且最终数值校验通过。证据 `C-combine-perf/result.json`。

### 有序步骤与真实连跑：阶段记录

本项验证：C++14 Host fixture 覆盖命名步骤、绑定/顺序/最终输出拒绝和 Barrier unsupported；
官方 CANN9.2 构建安装及 A5 八 rank chain perf W1/N5/B2 通过，真实最终输出校验成功。
证据 `D-chain-perf/result.json`。Host 解析 fixture 不是跨机或设备 Barrier 证明。

## 不带内部 profiling 版本的验证记录（2026-11-04）

基线 `33184801`；新增内容仅在 tools/unitest、tests/unitest 与文档，src 无改动。
WSL CANN 9.1.0/9.2.0 已分别针对已有生产安装完成框架 C++14 编译、链接、安装；
构建日志位于 `D:/workspace/TileXR-build-results/unitest-nointernal-20261104/` 下
`9.1.0/build.log` 和 `9.2.0/build.log`；各自的 install 目录保留安装结果。未重新编译生产库。
`python3 -B -m unittest discover -s tests/unitest -v` 的 9 项 Host 检查通过，
含真实 Agent/fixture Worker 进程的正常退出和 EOF 回收。fixture 不加载 CANN。
### A5 两卡短验收（2026-11-04）

本次实际构建源码为 `5e84bdb861aaed827834f703eaa4618797e8bbc8`，在 a5 的
devices 0、1（Ascend950DT，驱动 26.2.0.b020）运行。CANN 9.2.0、aarch64
GCC 12.3.1 下独立构建、链接、安装通过，安装入口 help、internal/selected/custom
配置拒绝及 9 项 Host 协议检查通过。Worker 无 RPATH/RUNPATH，运行依赖解析到真实
驱动 HAL，无 devlib。Worker SHA256：
`fe6a66127c8af56a58f059f2a2e1929f1c3d2da16f1403de3ac7a10a8405a9ec`。

复用 `/home/h30059441/unitest-pr199/tilexr` 的已安装生产库及配套设备对象，未重新
编译生产代码。该安装保留的源码版本为 `f5fd8d075a4d5c880b2bdd34f89a30dd9482bd07`，
与本次版本的 `src/` 无差异；安装库和设备对象 SHA256 与保留的构建记录一致。
这不是新一轮生产库或统一构建验收。

| 用例 | 参数 | 本次结果 |
| --- | --- | --- |
| Dispatch single | 默认小输入，一次调用 | 两 rank 数值校验通过 |
| Combine single | 默认小输入，一次调用 | 两 rank 数值校验通过 |
| Dispatch→Combine single | 真实 Dispatch 设备输出绑定 | 两 rank 最终输出校验通过 |
| 连跑 correctness | iterations=3，累计检查点 1、2、3 | 所有检查点、两 rank 通过 |
| 连跑 Host batch perf | warmup=1 / iterations=2 / batches=1，无采集 | 批后校验通过；rank 0/1 为 61.740/58.990 us/chain |
| Dispatch 官方 profiling | single，selection=all | 两 rank 完成六步 ACLprof 生命周期，官方 msprof 导出均退出 0 |
| NodeAgent Dispatch single | 同机两 rank，真实 Worker | start/init/prepare/execute/validate/release/wait/close 通过，Agent 退出 0 |

输入均为 BS16/H128/topk2/experts8/seed1，普通 Memory、BF16、quant0。perf 数字仅为
一次极小批的框架计时通路证据，不是性能基准、纯 Kernel Duration 或全局 makespan。
上述链路未新增内部打点或更改算子。

初始化限时 60 秒，其余阶段及官方导出限时 20 秒；未触发超时。每项前后均确认两卡
空闲，最终无本次 Worker/Agent 残留。运行使用只允许 `/home/h30059441` 写入的私有
mount namespace；未重置设备或清理其他任务，未重跑已有 HCCL 资格记录。

官方导出各产生 71 个文件，包含原始数据及官方 CSV/SQLite/Trace 产物，但同时出现
只读 `/dev/shm` 导致的信号量创建失败和 `Cluster Tuning did not complete` 警告。
应区分导出退出 0 与完整分析成功，产物样本完整性仍未验证；UniTest 不解析这些文件。

随后经用户授权，仅离线重导出：私有 mount namespace 内在 `/dev/shm` 挂载独立
64 MiB tmpfs，其余写入仍限于 `/home/h30059441`；不接触宿主机现有共享内存文件，
不占用 NPU。在原始数据副本上调用同一框架导出入口，两个 rank 总导出限时 20 秒，
均退出 0，各目录变为 79 个文件，原始采集目录的逐文件 SHA256 保持不变。
信号量的只读异常消失，但 Cluster Tuning 警告仍在，故不能将该警告完全归因于只读
限制；完整分析仍为待定位，不能据此认定采集完整。此次日志还表明已解析数据被复用，
不宣称从原始 bin 重新解析通过。证据位于同一根目录下
`profiling-shm-reexport-20261104T083502Z/`（隔离信息、前后哈希、官方日志与产物）。
导出结束后确认宿主机仍在原 mount namespace，`/dev/shm` 挂载保持原状。
NodeAgent 临时验收脚本首次漏传 run_options，prepare 无法读取 run.json，未调用
算子；日志保留，修正脚本后通过，不归因于算子或产品实现。

证据保留在 a5 `/home/h30059441/unitest-two-5e84/`：`logs/build.log`、
`logs/host-tests.log`、`logs/reject-*.log`、`logs/artifacts.sha256`、
`logs/validation-summary.json`、`logs/node-agent-protocol.json`、逐项空闲检查，以及
各用例 `result.json`、rank 日志和 `dispatch-msprof/profiling/rank-*.log`。
真实跨机数据面、其他规模/输入、CANN 9.1 实机及 profiling 完整性仍未覆盖；同机
NodeAgent 通过不代表跨机通过。设备 Barrier 仍明确不支持，PR 保持 Draft。

## 固定槽位版本的验证边界（2026-11-04）

WSL CANN 9.1.0/9.2.0 各自重建并安装生产通信库、EP Host 库、四个注册设备二进制
及 UniTest；通用 EP 打点开关为 ON。Host 使用 C++14，A5 Kernel 使用已有设备编译配置。
WSL 使用 CANN devlib HAL 仅作链接，不写入 RPATH/RUNPATH；不能作为可运行 NPU 环境。
保留构建与安装日志于 `D:/workspace/TileXR-build-results/unitest-fixedslots-20261104/`。

14 项框架 Host 检查、SDK 的 4 项 Host/设备协议替身检查，以及 EP launcher 打点
启用/关闭两种 Host 检查通过。双 rank records fixture 经原有工具生成报告，验证接入
参数与产物关联；fixture 不证明实际设备采集。SDK 64字节隔离避免相邻 lane 在同一
标量缓存行写入；上述 Host/fixture 检查本身不证明实际 GM 可见性或采集完整性。

### 固定槽位版本 A5 两卡验收（2026-11-04）

实际源码 `e0618746cbaf0f92fcab2e95738e890e852bc943`（生产功能为 `9241de7b`，
其后仅验收文档），a5 devices 0、1，Ascend950DT、驱动 26.2.0.b020、CANN 9.2.0。
本次重新统一构建并安装通信库、EP Host/四个配套注册设备对象与 UniTest，
`TILEXR_EP_ENABLE_PROFILING=ON`、Host C++14；没有复用旧版生产库作为新接线证据。
Worker 无 RPATH/RUNPATH，依赖解析到本次安装与真实驱动 HAL，无 devlib。
Worker SHA256：`9d6f5ca7ed28fba92f4df4137ae4477e2f29cbd58dae87a60d4860a6171181af`。
输入均为 BS16/H128/topk2/experts8/seed1，普通 EP Memory、BF16、无量化。

安装入口及工具 help、14 项框架 Host 检查、4 项 SDK 检查通过；指定轮次越界和
缺少时钟依据在启动 Worker 前拒绝。1 byte 预算通过实际安装 CLI 拒绝，未启动
Worker；没有额外在设备上耗尽内存。a5 缺少 wheel，源码 pip 安装报 bdist_wheel
缺失；改用同版源码构建的通用 wheel，离线安装到本次专用目录，不改系统 Python。

关闭采集：Dispatch/Combine/连跑各一次 single、连跑 correctness 3 轮和 perf
W1/N2/B1 均在两 rank 通过数值校验；关闭时无内部 records 文件。

| 内部采集用例 | 选择 | capture 数（每个 capture 含两个 rank） |
| --- | --- | --- |
| Dispatch single | all | 1 |
| Combine single | all | 1 |
| Dispatch→Combine single | all | 2 |
| 连跑 correctness，iterations=3 | selected：第 1、3 轮 | 4 |
| 连跑 perf，W1/N2/B2 | all，两批 measure 轮次 | 8 |
| 连跑 perf，W1/N3/B1 | selected：batch 1 的第 1、3 轮 | 4 |

六项均通过数值校验、全 rank release 和既有分析器调用。共 20 个独立 capture、
40 个 records 文件；每文件实际参与的 64 个 lane 均有 begin/end 两条完整记录，
无 dropped/overflow。用已安装 tools/profiling 的原有读取/配对入口复核，20 组均为
complete，并生成各自的 HTML、Markdown、Perfetto 产物；未在 UniTest 添加解析逻辑。
核对两个 rank 的同次 UUID 一致，不同调用 UUID 独立、连跑两阶段独立，文件数与
请求集合一致。perf 全采集槽为 5..12，指定采集为 5/6/9/10；预检、warmup 和未选
轮次留空，没有额外采样调用。Host batch 记录为正数且批后校验成功，不用于性能对比。

时钟换算取保留的同环境官方 device_1 元数据 `hwts_frequency=999.987 MHz`，同时
记录 CANN dav_3510 的 GetSystemCycleImpl 读取 SYS_CNT 的来源；两者对应关系及跨 rank
时钟未校准。因此仅验收采集/记录/工具数据通路，不宣称报告中的准确阶段延迟或对齐。

初始化限时 60 秒，其余阶段与分析限时 20 秒，未触发超时；11 项实机用例均在前后
确认两卡空闲，最终无本轮 Worker/Agent 残留。使用私有 mount namespace，写入仅限
`/home/h30059441` 及私有 `/dev/shm` tmpfs；未修改算子实现、重置设备、清理其他任务
或重跑已有 HCCL 资格记录。没有发现需要修复的框架问题或需标记的算子阻塞。

证据：a5 `/home/h30059441/unitest-fixedslots-e061/`，包括 `logs/build.log`、
`logs/artifacts.sha256`、`logs/host-tests.log`、`logs/sdk-tests.log`、
`logs/validation-summary.json`、逐项空闲检查、各用例 result/rank 日志、capture manifest、
records 及工具报告。预算拒绝、时钟依据、初次工具安装失败日志也保留。
按用户收敛，本轮未执行真实跨机、新版 NodeAgent 实机矩阵或官方 msprof 重采集；
这些结果不由本轮两卡内部采集代替。CANN 9.1 实机、设备预算耗尽、异常/中断实机清理、
精确时钟和设备 Barrier 仍未覆盖，PR 保持 Draft。

## main Server Dedup integration (2026-11-04)

Rebased the complete UniTest branch, including documentation update `c224ac14`,
onto main `f622e4fa` (Server Dedup PR #185); the replayed tip is `8cb1f268`.
The main-line Server Dedup implementation, EP compatibility capture, tiling cache,
and their cleanup order remain intact. Generic fixed-slot fields follow main's
fields in `CommArgs`; their export retains a separate all-rank cleanup gate.
The compatibility latest-launch region cannot replace UniTest's all/selected
per-invocation captures. Both paths reuse the existing records analysis tool.

Fresh local checks: 14 framework Host tests, four standalone SDK Host/device-mock
tests, EP Host validation, and launcher tests with generic profiling OFF and ON
passed. GCC 15.2 Release compilation warned about signed arithmetic overflow in
existing fake-address expressions in `test_tilexr_ep_host_validation.cpp`; that
fixture was not changed here. These checks do not establish real-device behavior.
The local host has no configured CANN compiler; official production build/install
and NPU regression were not rerun in this rebase step, and no NPU was occupied.

Main raises the maximum rank count to 256, changing array sizes and offsets in
`CommArgs`. Rebuild and install the complete paired Host/kernel set before runtime
validation; do not reuse pre-rebase objects or treat the earlier two-card result
as evidence for the rebased ABI. The framework's supported rank scope is unchanged.

## Review scope: framework self-tests (2026-11-04)

Removed the four `tests/unitest/` framework self-test files from the deliverable,
following review and the agreed framework-only scope. Earlier Host test results
above remain historical evidence for their recorded revisions, not commands to
rerun against the current tree. Production EP and profiling SDK tests remain.
Subsequent acceptance uses build/install checks and bounded installed-framework
runs; removing self-tests does not remove numerical validation from UniTest.

## Post-rebase A5 acceptance (2026-11-04)

Source `30ef3e71` includes main `f622e4fa`, the documentation reorganization, and
removal of framework self-tests. On the same authorized a5 devices 0/1,
Ascend950DT, driver 26.2.0.b020 and CANN 9.2.0, rebuilt and installed the complete
core, diagnostics, EP Host library, all six EP kernel objects (including main's
Server Dedup objects), and UniTest. Generic profiling was ON and Host code C++14.
This replaces pre-rebase binary evidence for the changed `CommArgs` ABI; it does
not qualify Server Dedup execution. Source archive SHA256:
`96346f03c83281c985453d47190744d80c8df73c6e8854a583043ae697539b7f`.

Installed help, NodeAgent EOF/exit, invalid iteration/clock/selected-round rejection
passed without device execution. Worker dependencies resolve to this installation,
CANN and the real driver HAL, without devlib or Worker RPATH/RUNPATH.

The five profiling-OFF numerical cases passed on both ranks: Dispatch, Combine
and chain single; chain correctness 3 rounds; chain perf W1/N2/B1. Six internal
profiling cases also passed: those three single cases, chain correctness selecting
rounds 1/3, chain perf all W1/N2/B2, and chain perf W1/N3/B1 selecting iterations
1/3. Their 20 captures contain 40 rank files. The existing profiling package
confirmed 64 actual lanes/file, two begin/end events/lane, complete records and
no drops/overflow, with matching per-call UUIDs across ranks. All analyzer calls
succeeded. The retained clock basis is unchanged and uncalibrated; no accurate
stage timing or cross-rank alignment claim is made.

Successful case wall times were 7.7–9.9 seconds, with initialization limited to
60 seconds and each execution/tool phase to 20 seconds. Every pre/post idle gate
passed; no owned Worker remained. No device reset, other-process cleanup, extra
HCCL qualification, official msprof rerun or cross-host workload was performed.

Initial setup failures (missing noninteractive CMake path and diagnostics SoC
cache setting) are retained. Two initial Dispatch attempts failed before operator
execution with aclInit=500000 / driver query=87 under a temporary runner that
nested the read-only mount wrapper. A single-wrapper ACL probe succeeded;
removing the runner's redundant outer namespace made the unchanged installed
Worker pass. These are validation-runner failures, not operator failures or
product fixes. Writes remained under `/home/h30059441`, except private tmpfs
`/dev/shm`; no host mount namespace or shared-memory files were changed.

Evidence: `/home/h30059441/unitest-review-30ef3e71/`, especially `logs/build-final.log`,
`logs/installed-checks.log`, `logs/artifacts.sha256`, `logs/validation-summary.json`,
per-case rank logs, capture manifests and reports. CANN 9.1 compilation of this
rebased revision, real cross-host execution, Server Dedup runtime, abnormal NPU
cleanup, official profiler completeness and calibrated clocks remain unverified.
