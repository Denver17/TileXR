> 当前版本已删除私有 profiling 构建/运行开关与 record。本文保留的旧 profiling 数值是历史证据，不能视为当前支持的采集接口；当前边界见 [profiling](COMBINE_V2_PROFILING_TRACE.md)。

# MoonEP Combine V2 多机性能测试流程

本文记录部署、编译、空闲检测、测试和结果判定流程。当前设备计时边界见
Chrome Trace JSON 导出见
[COMBINE_V2_PROFILING_TRACE.md](COMBINE_V2_PROFILING_TRACE.md)。

## 0. 0/2 柜固定快速流程

当前 0/2 柜 128P 测试使用固定目录
`/home/h00580772/tilexr_combine_v2_cab0_2_fast` 和 Mutagen 会话
`tilexr-combine-v2-cab0-2-fast`。会话建立后，每次只依次执行以下四条命令：

```bash
mutagen sync flush tilexr-combine-v2-cab0-2-fast
ssh root@141.61.53.150 'bash /home/h00580772/tilexr_combine_v2_cab0_2_fast/tools/moonep/cab0_2_128p/compile.sh'
ssh root@141.61.53.150 'bash /home/h00580772/tilexr_combine_v2_cab0_2_fast/tools/moonep/cab0_2_128p/sync_runtime.sh'
ssh root@141.61.53.150 'bash /home/h00580772/tilexr_combine_v2_cab0_2_fast/tools/moonep/cab0_2_128p/run.sh'
```

三个远端脚本只服务当前工程和 0/2 柜，固定使用 CPU1 `141.61.53.150`、CANN
`/home/pkg/b131/cann-9.1.0`，并执行 128P、BS8192、K16、H3584、Exp256、BF16、
warmup20、80 轮、no-reduce、关闭 profiling 的测试。环境空闲状态仍在执行四条命令前
通过部署环境的外部监控判断，不在 launcher 内重复检测。

### R1 固定 16 核和可重复 barrier

R1 在保留 bidirectional Ring 和 Legacy Grant 的前提下，将 Runtime blockDim 固定为 16，
并要求所有 16 个 block 以相同顺序进入 `AscendC::SyncAll<true>()`。2P-8P 的非工作核不能
在 Kernel 入口提前返回，只跳过 peer、QP、payload、Done 和 Reduce 工作。`SyncAll` 只提供
rendezvous，不提供状态归约；局部失败必须先写入 magic-tagged collective status，再由
16 核同步读取并统一继续或退出。

leading/round global barrier 使用两个 generation slot 循环复用。signal 除 generation 外还
必须携带并校验完整 `boundaryId`，否则 boundary `n` 的陈旧信号可能被 `n+2` 误认。R1
暂不启用 phase barrier，但预留其 boundary 编号，避免后续 R4 改变既有 round 编号。

2026-08-16 在 CANN 9.1、Ascend950、0/2 柜 128P 上完成 target compile 和实机验证：
BS8192、K16、H3584、Exp256、BF16、no-reduce、20 次 warmup + 80 次迭代，128/128 rank
正确性通过；平均 `4.563616 ms`，最大 `4.573830 ms`。日志为
`combine_v2_128p_noprofile_20260816_231819.log`。该结果不覆盖 32P/64P、Reduce 或 R2 之后
的 Server-Pair/64-Grant 行为。

### R2 停用 32P+ Legacy Grant

R2 在 32P/64P/128P 停止发布和等待 Legacy Ring Grant，2P-16P 保留旧路径。每个 round
的数据 CQ drain 后仍执行 global barrier，Done ordering、Ring peer 和公共 ABI 不变。

global barrier 的稳定 `boundaryId` 和两槽 ping-pong `generation` 必须解耦。R1-R3 为后续
phase barrier 预留编号，但尚不执行该 barrier；128P 的 boundary 因而从 4 直接跳到 6。
若使用 `boundaryId % 2`，这两个连续执行的 barrier 会复用同一个槽，较快 rank 可用
boundary 6 覆盖较慢 rank 尚未读取的 boundary 4 signal。停用 Legacy Grant 后该竞态表现为
一柜已等待 boundary 6、另一柜仍等待 boundary 4，最终约 10 秒超时。正确做法是按实际
执行序号选择 generation：leading 为 0，R1-R3 的 round `s` 使用 `s + 1`；signal guard
仍校验完整 `boundaryId`，用于拒绝陈旧或被覆盖的信号。

2026-08-17 在 CANN 9.1、Ascend950、0/2 柜 128P 上验证 R2：最小 BS128、1 次迭代、
no-reduce 用例由超时恢复为 `correctness=passed`；随后 BS8192、20 次 warmup + 80 次迭代
的 128/128 rank 均通过，平均 `4.466763 ms`，最大 `4.480840 ms`。完整日志为
`combine_v2_128p_noprofile_20260817_002247.log`。该证据覆盖 128P Ring、barrier-only、
no-reduce 路径，不覆盖 32P/64P、Reduce 或 Server-Pair/64-Grant。

### R3 same/cross Server-Pair

R3 仅把 32P/64P/128P 的 peer 顺序切换为 same-half/cross-half Server-Pair，继续以 R2 的
round barrier 作为 admission；不启用 phase barrier，也不发布或等待新 64-Grant。
发送 peer、Done receive round 和 full-sync sender-core 必须从同一组 Server-Pair coordinate
helper 正反推导。只验证发送覆盖不够：receive inverse 或 sender-core 任一处保留旧 Ring
公式，都会分别表现为 Done 或 full-sync 超时。

2026-08-17 在 CANN 9.1、Ascend950、0/2 柜 128P 上完成 R3 target compile、六个 Host/unit/
source-guard 门禁及实机验证。BS8192、K16、H3584、Exp256、BF16、no-reduce、20 次 warmup
和 80 次迭代的 128/128 rank 均通过，平均 `5.773789 ms`，最大 `5.782910 ms`。完整日志为
`combine_v2_128p_noprofile_20260817_011147.log`。全源输出比较覆盖 Self、同 server Fullmesh
和跨 server shared-QP 数据结果；本轮未采集 profile trace，也未覆盖 32P/64P、FP32
route-weight 或 Reduce 路径，因此这些边界仍需在后续阶梯验证中单独证明。

### R4 独立 phase barrier

R4 保持 R3 same/cross Server-Pair、round barrier 和 barrier-only admission 不变，在 phase0
最后一个 round barrier 之后额外执行 boundary `1 + R/32`。phase0 的最后 round barrier 与
phase barrier 故意同时保留，用于单独证明 phase 边界。启用原先预留的 boundary 后，
phase1 round 的 execution ordinal 必须整体加一；generation 只能从实际 ordinal 推导，
不能因为 R4 中 boundary ID 恰好连续就重新把两者合并。

2026-08-17 在 CANN 9.1、Ascend950、0/2 柜 128P 上完成 R4 target compile、六个 Host/unit/
source-guard 门禁及实机验证。Host oracle 验证 boundary/ordinal `0..9`、相邻 generation
交替和同 generation 的完整 boundary guard。BS8192、K16、H3584、Exp256、BF16、
no-reduce、20 次 warmup 和 80 次迭代的 128/128 rank 均通过，平均 `5.842303 ms`，最大
`5.850310 ms`。完整日志为 `combine_v2_128p_noprofile_20260817_012720.log`。该结果覆盖
101 次连续 invocation 的 phase barrier、generation 和 epoch 复用；未覆盖 32P/64P、
FP32 route-weight、Reduce 或 profile trace。

### R9-Q 单接收源 Direct Grant 探针

R8 的每轮 Server-Grant admission 要求每个 core 等待四张源卡，并通过三次
`SyncAll<true>()` 将 16 个 core 的状态归约后再进入下一 round。该结构会把整卡推进速度
绑定到最慢 core。R9-Q 为隔离接收侧规整同步成本，仅把每个目标 core 的等待集合缩为
predecessor server 中相同 local-rank card/core 的一个 signal，并删除每 round 的 collective；
leading/phase barrier、四目标 Grant 发布和 final closure 均保持不变。

可复用的验证方法是先保持发送 WQE 数、数据路径和 phase 边界不变，只缩小接收等待集合
并删除对应的卡内归约，以避免把接收侧同步收益和发送侧发布优化混在一次 A/B 中。控制流
必须让失败 core 继续走完固定 phase 循环骨架并汇入 phase barrier，不能让单 core 提前进入
下一组硬件 collective。

2026-08-17 在 CANN 9.1、Ascend950、0/2 柜 128P 上完成 target compile、六个 Host/unit/
source-guard 门禁和两轮实机验证。BS8192、K16、H3584、Exp256、BF16、no-reduce、20 次
warmup 和 80 次迭代的两轮结果分别为 `max_ms=4.977630` 和 `4.981410`，两轮均为
128/128 rank 正确性通过，且各有 128 条 rank 性能记录。两轮 `max_ms` 均值
`4.979520 ms`，相对 R8 的 `5.231500 ms` 提升 `4.82%`。日志分别为
`combine_v2_128p_noprofile_20260817_025341.log` 和
`combine_v2_128p_noprofile_20260817_025607.log`。

该结果证明卡内 Grant 规整同步有可测成本，但不足以解释全部差距。由于 R9-Q 仍让每个
core 发布四个 Grant WQE，不能据此估算完整点对点 Grant 的最终上限；后续应把发送端
单目标化作为独立变量，再验证 Grant 与 payload final batch 合并。当前证据不覆盖 32P/
64P、Reduce、FP32 route-weight 或发送端单目标 Grant。

### R10-Q 单目标 Grant Publisher 探针

R10-Q 保持 R9-Q 的单 signal direct wait、每 round CQ drain、phase barrier 和 final closure，
只把发送端从四目标广播缩为 direct wait 映射的唯一逆向目标。64P/128P 每个 core 每 round
只提交一个 ordered-completion Grant WQE；32P 的 phase step 数为 1，direct target 是本
rank，因此发布一个本地 receive signal，不提交远端 WQE。schedule oracle 必须单独覆盖
这个 32P self-target 边界，不能统一假设 direct target 都是远端 rank。

2026-08-17 在 CANN 9.1、Ascend950、0/2 柜 128P 上完成 target compile、六个 Host/unit/
source-guard 门禁和两轮实机验证。BS8192、K16、H3584、Exp256、BF16、no-reduce、20 次
warmup 和 80 次迭代的结果分别为 `max_ms=4.918600`、`4.921610`，两轮均为 128/128
rank 正确性通过，且各有 128 条 rank 性能记录。两轮均值 `4.920105 ms`，相对 R9-Q
`4.979520 ms` 提升 `1.19%`，相对 R8 `5.231500 ms` 提升 `5.95%`。日志分别为
`combine_v2_128p_noprofile_20260817_031358.log` 和
`combine_v2_128p_noprofile_20260817_031528.log`。

单目标发布带来稳定但较小的约 `0.059 ms` 收益，说明发布端四个 WQE/CQ 有成本，但不是
主要剩余瓶颈。后续应把 Grant 与 payload final batch 合并作为独立 A/B，避免同时删除
closure 或改变 phase barrier。另一个验证陷阱是固定 `compile.sh` 只重建 perf target；
直接运行 build 目录中的历史测试二进制可能得到与当前源码无关的结果。当前验证必须
重新构建并运行五个 Host 测试 target，再以其退出码作为门禁证据。当前证据不覆盖 32P/
64P 实机、Reduce 或 FP32 route-weight。

同日追加的第三轮关闭 profiling 复测为 `max_ms=4.925600`、128/128 正确性通过，日志为
`combine_v2_128p_noprofile_20260817_032121.log`。隔离 profiling 构建的
`max_ms=5.374750`，日志为
`combine_v2_128p_profile_r10_single_target_20260817_0325.log`；该耗时包含埋点扰动，
不能与关闭 profiling 的宏观性能直接比较。profile 完整性为 128 条 sample、128 条 rank
perf 和 2048 条 core profile。

最后一轮选出的 fastest rank8/core10、P50 rank87/core5、slowest rank59/core9 的 kernel
时间分别为 `4.632520/4.783730/4.881838 ms`。关键 core 的 wait 加 inbound 占比分别为
`89.36%/91.94%/90.29%`，8 个 send 合计均约 `292-295 us`。最大 wait 分别落在 step0、
step0/step4 和 step3，热点不是固定 round。现有 trace 的 `Step N wait` 同时覆盖 data CQ、
单目标 Grant 发布/CQ、direct Grant wait 和 phase barrier；未增加更细 profile point 前，
不得把整个 wait 区间归因于 Grant。

## 1. 基准口径

常用 128P case：

```text
world_size=128, BS=8192, K=16, H=3584, ExpNum=256, BF16
warmup=0, iterations=80, reduce=disabled
```

`K=16` 和 `BF16` 由 benchmark 固定。省略 `--reduce-hidden` 即测试 no-reduce。

性能口径是在一次 rank 对齐后连续提交 80 轮，用整批 ACL Event 耗时除以 80 得到
每个 rank 的 `avg_ms`，再取 128 个 rank 平均耗时中的最大值作为整组延迟
`max_ms`。计时轮次之间没有 barrier、同步、D2H、profiling 解析或日志输出。单 rank
等效算法数据量和带宽为：

```text
bytes = BS * K * H * sizeof(BF16)
      = 8192 * 16 * 3584 * 2
      = 939,524,096 bytes

alg_bw_GBps = 939.524096 / max_ms
```

## 2. 目录与 Hostfile

每次测试使用独立目录，避免复用其他任务的源码、产物、日志和 PID 文件：

```text
REMOTE_ROOT/
|-- source/
|-- build/
|-- install/
|-- logs/
`-- hostfile
```

hostfile 每行是 `IP:slots`。128P 使用 16 台服务器，每台 8 个 rank；第一行是
bootstrap 和任务控制节点：

```text
PRIMARY_IP:8
WORKER_IP:8
...
```

hostfile 顺序决定 global rank；每台服务器的 `device` 等于 local rank `0-7`。
编译和运行必须显式使用目标环境实际安装的同一套 CANN。

## 3. 部署源码

在本地 Git Bash 中创建任务专属 Mutagen 会话，只把源码单向同步到主节点：

```bash
SESSION=tilexr-combine-v2-<task>
PRIMARY_HOST=<primary-ip>
REMOTE_ROOT=/home/h00580772/tilexr_combine_v2_<task>

mutagen sync create \
  --name "${SESSION}" \
  --mode one-way-safe \
  --ignore-vcs \
  --ignore '/build*' \
  --ignore '/install*' \
  --ignore '/artifacts*' \
  --compression zstandard \
  'D:/3_codex/tileXR' \
  "root@${PRIMARY_HOST}:${REMOTE_ROOT}/source"

mutagen sync flush "${SESSION}"
```

忽略规则必须锚定仓库根目录；未锚定的 `build_*` 会误排除
`tools/moonep/build_combine_v2_perf.sh`。

不要改动或终止其他 Mutagen 会话。任务完成后只关闭本会话：

```bash
mutagen sync terminate "${SESSION}"
```

## 4. 编译与运行时同步

将命令写入任务专属 Bash 脚本，再通过 SSH 在主节点执行。先编译，不启动 NPU
进程：

```bash
bash "${REMOTE_ROOT}/source/tools/moonep/build_combine_v2_perf.sh" \
  --source-dir "${REMOTE_ROOT}/source" \
  --build-dir "${REMOTE_ROOT}/build" \
  --install-dir "${REMOTE_ROOT}/install" \
  --cann-path "${CANN_PATH}" \
  --jobs 16
```

确定本轮 hostfile 后，从主节点向所有计算节点平铺同步运行产物：

```bash
bash "${REMOTE_ROOT}/source/tools/moonep/sync_combine_v2_perf_runtime.sh" \
  --hostfile "${REMOTE_ROOT}/hostfile" \
  --install-dir "${REMOTE_ROOT}/install" \
  --ssh-user root
```

每台节点都应输出 `SHA256 verified`。源码不需要同步到 worker。

节点缺少 `/etc/hccl_rootinfo.json` 时，在该节点生成并安装：

```bash
tmp=$(mktemp /tmp/hccl_rootinfo.XXXXXX)
mindcluster-tools rootinfo --output "${tmp}"
install -m 0644 "${tmp}" /etc/hccl_rootinfo.json
rm -f "${tmp}"
```

启动前用 `test -f /etc/hccl_rootinfo.json && test -s /etc/hccl_rootinfo.json`
确认它是非空普通文件。若该路径被误建为空目录，C++ 文件读取会抛出
`basic_filebuf::underflow ... Is a directory` 并使 rank abort；确认目录为空后删除并
重新生成文件。

工具缺失时安装 `unofficial-ascend-tools==0.0.7rc2`。覆盖已有 rootinfo 前先保存
原文件，避免破坏共享环境配置。

## 5. 识别共享环境空闲状态

使用部署环境批准的外部监控工具选择候选设备，再以目标主机上的
`npu-smi info` 进程表作最终判定。TileXR 仓库不维护或分发机房主机清单与
占用监控脚本。`Alarm` 状态本身不表示卡不可用。

- 无 NPU 进程：可测试。
- 只有 `tilexr_*` 进程：允许共用，可直接测试。
- `python` 或其他通信测试进程：不可共用，每 15 秒复查。
- `?`、SSH 失败或 `npu-smi` 失败：不可判定为空闲。
- 等待 120 秒仍未满足条件：停止本轮，不启动 rank，等待下次唤起。

launcher 不重复执行 NPU 进程预检，也不负责等待环境空闲。调用方必须在启动前通过
部署环境的外部检查确认环境可用。

共享环境禁止 `pkill`，也不要清理其他任务的 PID、端口、日志或 Mutagen 会话。

## 6. 启动 128P 测试

环境可用后，在主节点执行 launcher。以下命令关闭 profiling、关闭 reduce，且不
预热：

```bash
LOG_FILE="${REMOTE_ROOT}/logs/combine_v2_128p_$(date +%Y%m%d_%H%M%S).log"

bash "${REMOTE_ROOT}/source/tools/moonep/run_combine_v2_perf_multihost.sh" \
  --hostfile "${REMOTE_ROOT}/hostfile" \
  --install-dir "${REMOTE_ROOT}/install" \
  --cann-path "${CANN_PATH}" \
  --ssh-user root \
  --bs 8192 \
  --warmup 0 \
  --iterations 80 \
  --experts 256 \
  --hidden-size 3584 \
  --comm-domain 141 \
  --comm-id "${PRIMARY_HOST}:10067" \
  --timeout 600 \
  --log-file "${LOG_FILE}"
```

并发任务必须使用不同的 bootstrap 端口；launcher 还会使用由该端口推导出的
barrier 端口。

128P launcher 会为每台节点预建一条 SSH ControlMaster，并让本机 8 个 rank 复用；
不要移除此逻辑，否则并发建连可能触发共享节点的 `MaxStartups`，表现为固定 rank
以 SSH `status 255` 退出。

## 7. 结果判定

正常聚合行如下：

```text
COMBINE_V2_PERF ... max_ms=<slowest-rank-mean> max_alg_bw_GBps=<bandwidth> ...
```

128P、80 轮、单 BS 的完整数据必须包含：

```text
COMBINE_V2_RANK_PERF: 128 条
```

旧私有 profiling 已删除。以下旧版采样说明仅用于解释历史日志：每个 rank 只为最后
一轮输出一条 `timing_source=kernel_profile` 的 sample，供最快/P50/最慢卡选择使用。

快速计数：

```bash
awk '
  /^COMBINE_V2_RANK_PERF / { ranks++ }
  END { print "rank_perf=" ranks + 0 }
' "${LOG_FILE}.ranks"/rank_*.log
```

正确性状态与性能采集相互独立。临时优化版本即使输出 `self_only_failed` 或
`failed`，只要 rank 性能记录完整，性能数据仍有效并应输出；报告中保留实际
`correctness` 状态即可。

旧测试产物如果出现 `barrier failed after all benchmark cases`，但已有完整的逐轮
样本和 `128/128` rank 汇总，则本轮性能数据完整。原因是复用同一
端口时，旧 listener 在逐个 release 之后才关闭，快 rank 可能误连上一轮；当前
实现会先关闭本轮 listener，再 release 全部客户端。需要从旧 rank 日志重新聚合
时使用：

```bash
awk -v expected_ranks=128 -v expected_iterations=80 -v bs=8192 \
  -f "${REMOTE_ROOT}/source/tools/moonep/configs/aggregate_combine_v2_rank_samples.awk" \
  "${LOG_FILE}.ranks"/rank_*.log
```

新格式中零条逐轮 sample 是正常现象，必须以 128 条 `COMBINE_V2_RANK_PERF` 判断
数据是否完整。SSH `status 255` 表示 rank 启动失败；`TsdProcessOpen failed: 31`
或 `UDMA init failed: -4` 表示资源尚未释放。两种情况都等待环境恢复后再试，不终止
共享任务。
