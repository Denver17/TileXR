# EP Server Dedup 使用与验证

本指南覆盖当前完整 Server Dedup Dispatch、Combine 及其联合运行。接口、五表、内存布局
和算法约束见 [当前设计](specs/ep-memory-server-dedup-dispatch-combine-design.md)。
普通 Memory 与 UDMA 测试使用各自入口，见 [EP 测试说明](../tests/ep/README.md)。

## 环境与部署

设备运行要求至少两台每台 8 卡的 A5 服务器，CANN 9.1.0、driver 25.1.rc1 或更新兼容版本。
运行 CLI 要求 Python 3.9+；CTest consumer 导入的共享测量/报告模块保持 Python 3.8 兼容。
Host 回归无需 NPU，不能替代设备通信验证。

在首节点准备同一工作目录，初始化子模块并配置本地环境：

```bash
git submodule update --init --recursive
cp validation.local.sh.example validation.local.sh
# 编辑 validation.local.sh 中的 CANN 环境、Python、SSH 用户等实际配置。
# 创建 hostfile：每行一个服务器 IPv4，首项必须是当前首节点。
bash set_env.sh
```

`validation.local.sh` 和 hostfile 是本地部署配置，不提交真实主机清单或凭据。
服务器数、shape、算子模式和轮数由 run.sh 决定，不写入环境配置。
sync 使用完整 hostfile，run 使用前 N 台；每台 8 卡、连续 rank 编号，
`globalRank=serverIndex*8+localRank`。支持 2..32 台，即 16..256 ranks。
总专家数不超过 1024，且须被总 rank 数整除。

按顺序执行，每步成功后再继续：

```bash
bash build_all.sh dedup=auto breakdown=0
bash sync.sh
bash run.sh -n 2 -e 96 -m 0 -l 3 0 operator_mode=both
```

构建固定覆盖 `install/server-dedup`，同步当前唯一安装包；重新打开 shell 不需要向 sync
重复传递构建参数。构建失败会失效旧 manifest，不能把旧包作为新产物运行。新增服务器或
改变 Host/kernel/consumer 源码、核数、去重策略、打点模式后须重新构建并同步。
各节点检查来源摘要、build_id、安装文件和同步回执，防止源码与二进制或不同 rank 不一致。
256-rank 的 CommArgs 影响 ABI，通信库、EP Host、kernel、consumer 必须配套重建。

## 构建与运行参数

| 参数 | 含义 |
| --- | --- |
| build: dedup=auto/0/1 | 默认 auto：BS≤32 直发，BS>32 去重；0/1 强制模式 |
| build: breakdown=0/1 | 设备内部打点开关，默认关闭 |
| build: dispatch_send_cores=N | 16..48，步长 4；默认 32 |
| build: combine_send_cores=N | 16..56，步长 4；默认 32 |
| run: -n N -e E | hostfile 前 N 台、总专家数 E，必填 |
| run: -m 0/1/2 | functional / performance / breakdown |
| run: -l LOOPS WARMUP | 正式轮数与预热轮数，默认 1 0 |
| operator_mode | dispatch / combine / both，默认 dispatch |
| bs、h、topk、dtype | 默认 96、7168、6、bf16；dtype 可为 fp16 |
| dispatch_quant_mode | 0 或 4；4 时 fp8_format=e4m3/e5m2 |
| combine_quant_mode | 当前仅 0，与 Dispatch 量化独立 |
| route_mode、seed | random / balance / balance_shift；默认 balance_shift、2026 |
| host_validation=1 | 设备运行前在首节点执行独立 Host 回归，默认关闭 |
| dry_run=1 | 用已安装 Host consumer 检查参数与 rank 映射，不 SSH、不加载 CANN、不访问 NPU |

完整参数以 `bash run.sh --help` 和 `bash build_all.sh --help` 为准。
functional/performance 要求打点关闭的包，breakdown 要求打点开启的包；不匹配时启动前拒绝。
策略和核数是构建参数；shape、类型、路由 seed、轮数是运行参数。

```bash
# 独立 Combine 性能；与 Dispatch 量化开关无关。
bash run.sh -n 2 -e 96 -m 1 -l 100 20 operator_mode=combine

# FP16 输入的 MXFP8 Dispatch，同时验证 Combine 接线。
bash run.sh -n 2 -e 96 -m 0 -l 3 0 operator_mode=both \
  dtype=fp16 dispatch_quant_mode=4 fp8_format=e5m2 route_mode=random seed=42

# 内部流水观察，必须重建和同步打点包。
bash build_all.sh dedup=auto breakdown=1
bash sync.sh
bash run.sh -n 2 -e 96 -m 2 -l 3 2 operator_mode=both
```

both 连接真实 Dispatch 输出元数据与 Combine，并使用测试定义的 expertOut 夹具；它不运行
真实 MoE expert 网络。独立 Combine 同样使用确定性的 expert 结果与独立参考，不能把通过
此测试解释为上层模型已经验收。

### 专家输入

输入生成器位于 [tilexr_ep_expert_ids.h](../tests/ep/demo/tilexr_ep_expert_ids.h)。
`BuildRandomExpertIds` 对每个 Token 打乱全部专家后取前 K 个，使用 seed 和全局 Token
编号派生随机种子，保证同一 Token 专家不重复。
`balance` 为均衡分配，`balance_shift` 每个均衡周期洗牌后分配；后者要求全局 Token 数
能被 `E/gcd(E,K)` 整除。研究 BS=31/32/33 等边界时优先使用 random，避免均衡约束干扰。

同一 seed 的 `std::shuffle` 跨标准库实现不保证逐位一致。精确复现须保留该次输入文件和
构建身份，不能仅凭 seed 推断跨环境输入相同。consumer 保存的 x、ids、weights 原始位型
与 summary 的 shape/dtype 一起构成复现输入；生成器与数值参考均应独立于生产算法。

## 本机回归

无需 CANN/NPU 的源码回归：

```bash
cmake -S tests/ep -B build/server-dedup-host
cmake --build build/server-dedup-host -j2
ctest --test-dir build/server-dedup-host --output-on-failure
python3 -m unittest discover -s tools/validation -p 'test_*.py'
```

对现有安装包执行登记的 Host C++ 与 Python 测试：

```bash
bash run_host_validation.sh
```

该入口核对 manifest、CPU 架构和产物摘要，排除必须使用设备的 registration 测试。
成功需有有效且非空的测试集合、正确 marker 和退出码；超时、缺失或重复 marker 均失败。
结果位于 `run/validation/host-<id>/summary.json`，同目录保留逐项日志。
纯 Host 包可通过 `TILEXR_VALIDATION_BUILD_MODE=host bash build_all.sh` 构建；执行安装包
回归时保持同一 build mode。纯 Host 包只能做 Host 回归和 dry-run，不能用于 NPU 运行。

回归按职责覆盖公共 ABI、Host 参数和容量、Count/布局、发送接收、五表、Combine 数值、
跨轮 Flag 清理、profiling 生命周期及工作流失败传播。同步 CPU API 替身不模拟真实 MTE
异步、缓存可见性和跨机调度；这些行为需要匹配 CANN 编译和实际设备测试补充。

## 设备回归矩阵

使用当前完整入口覆盖以下集合，不需重放开发 checkpoint：

| 维度 | 必需用例与判据 |
| --- | --- |
| 算子 | dispatch、combine、both；各自最终输出与 cleanup |
| 类型 | FP16/BF16；Dispatch 非量化、E4M3、E5M2，Combine 非量化 |
| 策略 | auto 小 BS 对照强制 0，auto 大 BS 对照强制 1；31/32/33 边界 |
| 路由 | random 与均衡路由；同 Server、多远端 Server、DIRECT 与 relay 混合 |
| 生命周期 | 单轮、双轮及重复调用；分别按实际 magic 检查窗口与 Flag 清理 |
| 形状 | K=1、K=32，零接收任务、容量/UB 准入与关键非法参数 |
| 打点 | OFF/ON，both 的独立记录域，最后一轮完整性与溢出判定 |
| 规模 | 先验证选定环境的小规模，再扩大到实际交付规模；256P 需要独立实机证据 |

例如在 profiling OFF 包上检查 auto 阈值：

```bash
for bs in 31 32 33; do
  bash run.sh -n 2 -e 96 -m 0 -l 3 0 bs="$bs" route_mode=random \
    seed=42 operator_mode=both || break
done
```

这是不同进程/communicator 的三次运行；不能证明同一 communicator 的动态 BS 切换。
同一 communicator 32→33→32 的资源复用需要专门的多调用测试和设备证据。
性能或规模矩阵中的 shape 仍须通过内存/UB 检查，不能把上界参数任意组合。

在排查通信、peer-memory、MTE 或设备错误前，按仓库规则复用同环境已保留的官方 HCCL
Test 基线；只有不存在匹配记录时才做一次环境资格验证。记录主机/容器、设备集合、拓扑、
加速器模式、CANN、驱动、固件和测试构建，不能用不同环境的通过记录替代当前数据路径。

### 正确性通过条件

独立参考依据原始 expertIds 和权重重建路由，不能由被测生产函数生成预期结果。
每 rank 在 stream 完成后校验：

- communicator 映射与本次配置容量；普通 consumer 的逻辑通信域是 8 GiB，生产 API
  依据实际 CommArgs 容量检查，不能把 consumer 默认大小当作固定 ABI。
- 全路由 Count、inclusive prefix、expert 分段数与实际有效行数。
- Dispatch 的紧密 hidden、量化 scale、五表有效区、原始权重未被修改。
- DIRECT、第一跳与终端的关联；所有原始路由均有且仅有对应贡献，发送/接收数量守恒。
- Combine 的独立数值参考，包含 partial 的 FP32 累加和中间 FP16/BF16 舍入，再做 source
  加权归约；零权重仍完成接收。不得只检查返回码或与 yOut 共用计算路径。
- 当前窗口全部应清理的 Flag、跨轮复用和资源释放；容量尾部、无效表行和 padding 不比较。

汇总要求全部选定 rank 的身份、构建、提交/完成轮数、预热数、输出与 cleanup 一致。
孤立的 `CHAIN_PASS`、某个 Host marker、进程退出 0 或单 rank 通过均不足以证明整个用例成功。
profiling 是否完整和通信结果是否正确分别记录，不能把截断 trace 记为完整性能证据。

## 性能与 profiling

performance 使用 profiling OFF 内核，由 msprof 获取实际 kernel 样本；保留标准采集和
调用间 barrier，比较时使用相同设置。不能通过省略正确性检查、关闭外层 profiler 或移除
barrier 掩盖准备开销。设备内部打点用于 breakdown，会扰动执行，不作为无打点性能基线。

比较结果须带源码/产物身份、平台/CANN/驱动、rank 数、shape、dtype/量化、route/seed、
去重策略与实际模式、核数、预热/正式轮数、profiler 状态。报告每 rank 数据及跨 rank
min/median/max，注明数据量/带宽口径，不能把各核或嵌套区间简单相加成关键路径。

breakdown 使用标准 `tilexr_profiling_records.v1`，Dispatch 与 Combine 各自独立记录。
每核复用 latest launch 槽，只保留最后一次提交，`captured_launch=warmup+loops-1`。
它不是多轮平均，不能按 warmup 数再次剔除唯一 capture。短轮/空轮只读取本轮发布的有效
范围；最后一轮丢点标记 incomplete，先前轮次已覆盖，不推断其完整性。

默认每核容量有 65,536 条下限，较大非去重 Combine 按 BS/K 增长。Host reservation、kernel
writer、consumer 使用同一预算函数；自定义调用方须在 communicator 初始化前设置足够的
`TILEXR_PROFILING_RECORDS_PER_CORE`。同一 capture 固定核集合与槽宽，按 stream 串行运行，
不混用 latest 与普通 reserved 模式。

节点在本地解析 records 并生成报告，主节点收集成品和摘要；原始 records 保留在所属节点。
终端 `TRACE dispatch/combine` 输出实际 trace 目录及 complete/incomplete 状态，每 rank
一份 Chrome Trace JSON。每核归零不等于跨核或跨设备时钟同步。
Scalar 事件表示到达该代码位置；issue 结束并不表示 DMA 完成。M4/M5、metadata/atomic
等父子区间不能重复计入总耗时。映射及解析规则见
[算子 profiling 指南](../tools/profiling/OPERATOR_PROFILING_GUIDE.md)。

## 结果与故障定位

每次运行在 `run/case-<时间>-<PID>/` 保存入口日志、环境日志、workflow 日志和结果；
实际运行子目录含 summary.json、逐节点/逐 rank 日志及 trace 索引。
`TILEXR_VALIDATION_RESULT_ROOT` 可覆盖结果根目录。失败记录保留首错、退出码和已有结果，
强制杀进程可能没有完整终态；不能把缺失记录填成零或通过。

| 统计 | 解释 |
| --- | --- |
| RUN_WALL / launcher_timing.json | shell setup、可选 Host 回归、环境加载与 workflow，秒级 |
| WORKFLOW_WALL / summary.wall_time | 本机检查、节点检查、运行、收集与汇总 |
| RANK_WALL_MAX | 各阶段独立的最慢 rank 耗时；各项可能来自不同 rank，不能相加 |
| CONSUMER_WALL_MAX | communicator、分配、输入/参考、barrier、执行、校验与清理等 Host 耗时 |
| kernel_us | 设备 kernel 时长，与上述 Host 墙钟统计不同 |

优先查看第一条异常及其阶段。全 rank 都未提交时先查初始化、输入/参考构造和 barrier；
SIGTERM 只说明进程被终止，不证明 kernel 卡死。`execution` 包含 Host 提交和 stream 等待，
不能直接等同 kernel 延迟；超时和后续清理错误不应覆盖原始原因。

大规模 fixture 应按完整 shape/route/seed 缓存不可变输入，不能重复为每个 producer 构造
全局路由，更不能缓存被测设备结果。共享报告代码保持 Python 3.8 兼容，先处理首个异常，
避免未清理资源引发后续假失败。consumer 的参考计算允许编译优化，但保持严格浮点顺序，
不能启用改变数值参考的 fast-math。

提交或交付时分别记录实际执行的 Python/Host 回归、CANN 编译、设备功能、性能和规模
验证。未执行的范围明确留空；Host 模型或某版本编译不证明真实 A5 异步通信、其他 CANN
版本、256P 或动态 BS 已通过。历史阶段、实验数据与旧故障日志通过对应 Git 提交追溯。
