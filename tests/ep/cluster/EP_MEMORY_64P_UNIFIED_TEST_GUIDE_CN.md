# EP Memory 多规模统一测试脚本使用指南

本文说明如何通过同一个入口执行Memory Dispatch、Memory Combine或两个算子顺序连跑。
脚本默认使用8节点、每节点8张NPU的64P配置，也可以显式选择记录的128P配置或其他规模配置。

## 1. 测试入口

主控脚本：

```text
tests/ep/cluster/run_memory_dispatch_msprof_64p.sh
```

服务器主节点及工程目录：

```text
root@141.61.94.18:/home/h00580772/combine_test
```

脚本必须在`141.61.94.18`节点的工程根目录执行。基本格式：

```bash
bash tests/ep/cluster/run_memory_dispatch_msprof_64p.sh \
  --op dispatch|combine|both \
  --mode functional|performance
```

不带参数时默认为：

```text
--op dispatch --mode performance
```

## 2. 参数说明

### 2.1 算子选择

| 参数 | 行为 |
| --- | --- |
| `--op dispatch` | 只运行Dispatch |
| `--op combine` | 只运行Combine |
| `--op both` | 先运行Dispatch，再运行Combine，不并发执行 |

### 2.2 测试模式

| 参数 | msprof | Kernel打点 | 轮次 | 最终输出 |
| --- | --- | --- | --- | --- |
| `--mode functional` | 关闭 | 关闭 | 1轮 | 功能PASS/FAIL |
| `--mode performance` | 开启 | 关闭 | 50轮热身+150轮测试 | 最后50轮的min/max/avg |

性能模式的统计规则固定为：

1. 每张卡从`op_summary_*.csv`中匹配目标Kernel。
2. 每张卡只保留最后50次执行。
3. 每台server将8张卡的`50 * 8 = 400`个耗时数据取平均值。
4. 对全部server的平均值计算`minimum_us`、`maximum_us`和`average_us`。

性能模式最终打印统计数字并生成汇总JSON。节点之间用于保存单机400个样本均值的
`*_internal.json`属于脚本内部汇总文件。

旧私有 dump 的 `--mode breakdown` 和节点脚本 `profile` 模式已删除，传入时返回参数错误；
没有兼容别名。当前性能路径保留暖机、测量前后 Host barrier 和外部 msprof 采集。

## 3. 测试前准备

### 3.1 修改测试配置

Dispatch配置：

```text
tests/ep/cluster/memory_dispatch_64p.env
```

Combine配置：

```text
tests/ep/cluster/combine_memory_64p.env
```

常用参数包括：

```text
TILEXR_EP_CLUSTER_BS
TILEXR_EP_CLUSTER_H
TILEXR_EP_CLUSTER_TOPK
TILEXR_EP_CLUSTER_MOE_EXPERT_NUM
TILEXR_EP_CLUSTER_DTYPE
TILEXR_EP_CLUSTER_QUANT_MODE
TILEXR_EP_CLUSTER_COMM_QUANT_MODE
TILEXR_EP_CLUSTER_MXFP8_FORMAT
TILEXR_EP_CLUSTER_ACTIVE_MASK_TYPE
TILEXR_EP_CLUSTER_EXPERT_MODE
```

Dispatch和Combine配置文件默认使用50轮热身和150轮测试；performance模式读取
`TILEXR_EP_CLUSTER_WARMUP`和`TILEXR_EP_CLUSTER_PERF_LOOPS`作为轮次配置。

### 3.2 选择128P或其他规模

仓库记录的128P算子配置为：

```text
tests/ep/cluster/memory_dispatch_128p.env
tests/ep/cluster/combine_memory_128p.env
```

128P需要16台8卡节点。主机清单属于执行环境配置，不提交到仓库；通过环境变量同时选择
128P算子配置和外部主机清单：

```bash
TILEXR_EP_CLUSTER_DISPATCH_CONFIG_FILE=tests/ep/cluster/memory_dispatch_128p.env \
TILEXR_EP_CLUSTER_COMBINE_CONFIG_FILE=tests/ep/cluster/combine_memory_128p.env \
TILEXR_EP_CLUSTER_HOST_FILE=/path/to/hosts_128p.txt \
TILEXR_EP_CLUSTER_MASTER_HOST=141.61.94.70 \
bash tests/ep/cluster/run_memory_dispatch_msprof_64p.sh \
  --op both --mode functional
```

测试其他规模时，从最接近的配置复制一份到执行环境，并通过同样的两个`*_CONFIG_FILE`
变量选择，不要覆盖仓库内的64P/128P记录。配置需满足：

1. `TILEXR_EP_CLUSTER_RANK_SIZE`不少于8且能被每节点卡数8整除。
2. 主机清单至少包含`RANK_SIZE / 8`台机器，脚本按顺序取所需数量。
3. `TILEXR_EP_CLUSTER_MOE_EXPERT_NUM`能被`RANK_SIZE`整除。
4. 为本轮测试配置独立的`COMM_ID`和`BARRIER_ADDR`，避免与其他任务端口冲突。
5. `BLOCK_DIM`使用`auto`，或填写目标设备支持的Vector Core数量。

### 3.3 同步到主节点

在Windows Git Bash中执行：

```bash
cd /d/3_codex/tilexr_deepep/TileXR

mutagen sync flush tilexr-deepep-combine-0823-64p
```

该Mutagen会话必须为`one-way-safe`。Mutagen只用于Windows到主节点同步。主节点向
其他七个节点同步脚本和编译产物时由测试脚本并行使用`scp`或`rsync`。

### 3.4 NPU空闲门禁

统一run脚本只负责执行，不检查NPU是否空闲。调用方必须在启动run脚本前完成全部目标节点的
空闲门禁：任意NPU进程都视为占用，每隔5秒重新检查；120秒内未全部空闲时停止本次任务，
不得调用统一run脚本。直接调用节点脚本时也遵循同一前置条件。

## 4. 常用命令

以下命令均从Windows Git Bash发起，在主节点执行统一脚本。

### 4.1 功能验证

只验证Dispatch：

```bash
ssh root@141.61.94.18 \
  'cd /home/h00580772/combine_test && \
   bash tests/ep/cluster/run_memory_dispatch_msprof_64p.sh \
     --op dispatch --mode functional'
```

只验证Combine：

```bash
ssh root@141.61.94.18 \
  'cd /home/h00580772/combine_test && \
   bash tests/ep/cluster/run_memory_dispatch_msprof_64p.sh \
     --op combine --mode functional'
```

顺序验证两个算子：

```bash
ssh root@141.61.94.18 \
  'cd /home/h00580772/combine_test && \
   bash tests/ep/cluster/run_memory_dispatch_msprof_64p.sh \
     --op both --mode functional'
```

### 4.2 msprof性能测试

只测试Dispatch：

```bash
ssh root@141.61.94.18 \
  'cd /home/h00580772/combine_test && \
   bash tests/ep/cluster/run_memory_dispatch_msprof_64p.sh \
     --op dispatch --mode performance'
```

只测试Combine：

```bash
ssh root@141.61.94.18 \
  'cd /home/h00580772/combine_test && \
   bash tests/ep/cluster/run_memory_dispatch_msprof_64p.sh \
     --op combine --mode performance'
```

顺序测试两个算子并分别汇总：

```bash
ssh root@141.61.94.18 \
  'cd /home/h00580772/combine_test && \
   bash tests/ep/cluster/run_memory_dispatch_msprof_64p.sh \
     --op both --mode performance'
```

最终输出示例：

```text
最终性能结果
op=dispatch
minimum_us=54.613
maximum_us=57.900
average_us=56.208

op=combine
minimum_us=74.464
maximum_us=77.812
average_us=75.977
```

### 4.3 SDK 诊断入口

本 cluster runner 不导出设备阶段记录。需要 SDK 诊断时，使用
[算子 profiling 指南](../../../tools/profiling/OPERATOR_PROFILING_GUIDE.md) 中的独立调用方或
managed invocation plan 流程，并另建 `TILEXR_EP_ENABLE_PROFILING=ON` 产物。
普通 Memory 每 lane 只有 body begin/end 两个 SDK marker；ServerDedup 使用对应的
`DirectWriter` 业务事件目录，不能用旧 dump parser 或普通两点 mapping 解释。

在 communicator 初始化前提供 `TILEXR_PROFILING_SLOTS_FILE`；各 rank 保持相同调用顺序和
采样 UUID。`TILEXR_PROFILING_RECORDS_PER_STREAM` 默认 8，严格接受 8…65536 内的
十进制 8 的倍数。详细 ServerDedup 可从 512 或更大容量开始，实际仍须验证所有参与 lane
完整且 dropped=0。SDK 采样计划包含 warmup 的调用序号，但不选择 warmup 为采样槽。

## 5. 编译配置

脚本检查 `build_ep/CMakeCache.txt` 中 `TILEXR_EP_ENABLE_PROFILING=OFF` 和 demo 是否存在。
不满足时执行 `tests/ep/build.sh full`，然后分发 `install/` 和 `tests/ep/install/`。
该构建脚本为普通功能/性能基线关闭 SDK 插桩；旧 Dispatch/Combine 私有 profiling 开关已删除。

源码或共享 ABI 变化后使用 `--build` 强制重新构建并部署。缓存中的 OFF 标记不能证明已有
二进制与源码一致。`CommArgs`/tiling 布局变更要求 communicator、EP Host、外置 kernel 和
所有 consumer 配套重编译；不得只替换 Host 或一个 kernel。记录构建选项及源码/二进制
provenance，再进行对应环境验证。

## 6. 日志与产物位置

每次总流程的主日志：

```text
/home/h00580772/combine_test/tests/ep/logs/<session_id>/orchestrator.log
```

每个算子的rank日志：

```text
/home/h00580772/combine_test/tests/ep/logs/<run_id>/rank_<rank>.log
```

性能模式的msprof原始数据位于各节点：

```text
/home/h00580772/test_dir_<run_id>
```

`both`模式为Dispatch和Combine生成不同的`run_id`、msprof原始数据目录和rank日志目录，
不会互相覆盖。SDK诊断由对应调用方按 invocation UUID 保存，与此runner的msprof产物分开。

## 7. 注意事项

- 不要并行启动两套64P测试；通信地址和barrier地址是固定配置。
- `both`是顺序连跑，不代表Demo内部一次调用同时执行两个算子。
- 性能基线使用SDK插桩OFF构建；源码迁移后先用`--build`，并核对实际产物provenance。
- SDK阶段数据包含打点开销；父区间和子区间不能累加成总耗时，不能代替msprof性能结果。
- 测试失败时先查看总流程日志，再查看对应节点日志和`rank_<rank>.log`。
- 如果提示`Run this script on 141.61.94.18`，说明脚本执行节点错误。
- 如果提示找不到msprof，仅影响`performance`模式；`functional`不使用msprof。
