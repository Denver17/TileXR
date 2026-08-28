# EP Memory 64P 统一测试脚本使用指南

本文说明如何在当前8节点、每节点8张NPU的64P环境中，通过同一个入口执行
Memory Dispatch、Memory Combine或两个算子顺序连跑。

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
  --mode functional|performance|breakdown
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
| `--mode breakdown` | 关闭 | 开启 | 50轮热身+150轮测试 | 最快/P50/最慢卡的耗时和Trace JSON |

性能模式的统计规则固定为：

1. 每张卡从`op_summary_*.csv`中匹配目标Kernel。
2. 每张卡只保留最后50次执行。
3. 每个轮次取64张卡中的最大耗时，作为该轮耗时。
4. 对这50个轮次计算`minimum_us`、`maximum_us`和`average_us`。

性能模式最终只打印统计数字，不生成最终性能结果JSON。节点之间用于保留50轮数据的
`*_internal.json`属于脚本内部汇总文件。

耗时拆解模式不启动msprof。固定DFX区域只保留最后一次Kernel记录，因此Trace对应
第149个测试轮次。每个算子生成最快、P50和最慢rank三个Chrome Trace JSON。

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

脚本固定使用50轮热身和150轮测试，不读取环境文件中的`WARMUP`和`PERF_LOOPS`作为
本流程的轮次配置。

### 3.2 同步到主节点

在Windows Git Bash中执行：

```bash
cd /d/3_codex/tilexr_deepep/TileXR

mutagen sync flush tilexr-deepep-combine-0823-64p
```

该Mutagen会话必须为`one-way-safe`。Mutagen只用于Windows到主节点同步。主节点向
其他七个节点同步脚本和编译产物时由测试脚本并行使用`scp`或`rsync`。

### 3.3 NPU空闲门禁

统一脚本在每个算子启动前并行检查8个节点上的全部NPU。任意NPU进程都视为占用，
每隔5秒重新检查；120秒内未全部空闲时脚本以状态75停止，不启动本轮测试。直接调用
节点脚本时，调用方也必须先执行同样的空闲门禁。

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

### 4.3 Kernel耗时拆解

只拆解Dispatch：

```bash
ssh root@141.61.94.18 \
  'cd /home/h00580772/combine_test && \
   bash tests/ep/cluster/run_memory_dispatch_msprof_64p.sh \
     --op dispatch --mode breakdown'
```

只拆解Combine：

```bash
ssh root@141.61.94.18 \
  'cd /home/h00580772/combine_test && \
   bash tests/ep/cluster/run_memory_dispatch_msprof_64p.sh \
     --op combine --mode breakdown'
```

顺序拆解两个算子：

```bash
ssh root@141.61.94.18 \
  'cd /home/h00580772/combine_test && \
   bash tests/ep/cluster/run_memory_dispatch_msprof_64p.sh \
     --op both --mode breakdown'
```

终端会分别打印三张代表卡的信息：

```text
role=fastest rank=<rank> elapsed_us=<time> output=<trace.json>
role=p50     rank=<rank> elapsed_us=<time> output=<trace.json>
role=slowest rank=<rank> elapsed_us=<time> output=<trace.json>
```

JSON不包含`displayTimeUnit`，可以直接导入`chrome://tracing`。

新版Trace只展示相邻timepoint之间的阶段事件，以及每个AIV核的累计metric即时事件；
不再生成覆盖所有阶段的整段Kernel包络事件。算子总耗时仍由终端输出和JSON中的
`selection_elapsed_us`给出。

## 5. 编译与profiling切换

脚本会读取`build_ep/CMakeCache.txt`并检查两个Kernel的编译期开关：

| 运行方式 | Dispatch profiling | Combine profiling |
| --- | --- | --- |
| 功能或性能 | OFF | OFF |
| Dispatch拆解 | ON | OFF |
| Combine拆解 | OFF | ON |
| 两个算子拆解 | ON | ON |

当前编译状态不符合目标模式时，脚本才执行`tests/ep/build.sh full`，随后并行分发
`install/`和`tests/ep/install/`。因此从拆解模式切回功能或性能模式时，首次执行会
触发一次profiling OFF重编译；后续相同模式不会重复编译。

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

耗时拆解JSON位于主节点：

```text
/home/h00580772/combine_test/tests/ep/logs/<run_id>/traces/
```

`both`模式为Dispatch和Combine生成不同的`run_id`、原始数据目录、日志目录和Trace
目录，不会互相覆盖。

## 7. 注意事项

- 不要并行启动两套64P测试；通信地址和barrier地址是固定配置。
- `both`是顺序连跑，不代表Demo内部一次调用同时执行两个算子。
- 性能数据只能使用profiling OFF构建；脚本会自动保证这一点。
- 耗时拆解数据包含打点开销，只用于阶段归因，不能代替msprof性能结果。
- 测试失败时先查看总流程日志，再查看对应节点日志和`rank_<rank>.log`。
- 如果提示`Run this script on 141.61.94.18`，说明脚本执行节点错误。
- 如果提示找不到msprof，仅影响`performance`模式；`functional`和`breakdown`不使用msprof。
