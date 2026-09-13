# EP 验证与人工测试

## 当前提交：完整源端发送（设计验证流程 §2.4）

当前检查点为 `ep-sdedup-p2-source-send`：保留 cnt 发送/接收、cumsum 和第一跳发送，新增 DIRECT 执行。每个发送核先逐 Token 分类并发送全部第一跳，再按缓存的 tokenOffset/originalK/expertId/q 顺序执行直发表；两阶段共用发送池和包序号，之间无新增屏障。sourceMask/expertScalesOut 随本模块完成；接收转发、最终 expert 行整理、assistEx/group 生成及 Combine 留待后续模块。

固定基线为 **16P、两台各 8 卡 A5、单卡 6 专家（E=96）、BS=96、H=7168、TopK=6、BF16、balance_shift、seed=2026**。每 rank 始终申请 **4 GiB 通信域**：4092 MiB 数据区 + 4 MiB Flag 区，`peerMemBytes=4294967296`。直到完整 Dispatch/Combine 功能和性能验收结束，不随增量缩小通信域。底层另有 1 MiB 地址偏移余量，实际分配 4097 MiB/rank。

本检查点支持 **loops>=1、warmup>=0**，默认仍为 1/0，functional/performance/breakdown 均可配置。一批复用同一 communicator、stream、输入/输出和通信域，固定同一组输入及路由，连续提交 warmup+loops 次；只在整批末尾做一次 stream 同步和 Host barrier。每轮源端覆盖本轮目标槽位，设备轮末完成协议保证所有 rank 的 cnt 消费/清理和发送完成后才进入下一轮。整批结束先在 `round-read` 校验每个 rank 的四份完成代次均等于 warmup+loops，再检查最后一轮数值/包；不能解释为每轮输出快照。

当前入口明确停在源端发送检查点，未启用尚未闭环的 receiver/relay。旧入口曾调用 receiver，但其 prefix 基址遗漏 cntOffset，在固定 E96 场景未实际消费数据；不能将这段调用当作已验收接收功能。后续启用接收时，必须一起验证 packed 元数据、转发、pending 调度和 Flag 清理，再切换检查点。

实际执行需要 CANN 9.1、匹配 driver（25.1.rc1 或更新）及每卡至少 64 AIV 的 Ascend910A5/Ascend950。复用相同环境已保留的 HCCL Test 资格记录，不因本次构建重复运行；环境规则见 `AGENTS.md`。

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

## 手动编译、同步、运行

首次部署时准备环境文件与主机清单；已有文件只检查，不覆盖：

```bash
git submodule update --init --recursive
test -f validation.local.sh || cp validation.local.sh.example validation.local.sh
test -f hostfile || cp hostfile.example hostfile
```

`validation.local.sh` 只保存 CANN/driver、Python/msprof、SSH 用户、hostfile 和首节点可跨机访问的 `TILEXR_COMM_ID`。`hostfile` 每行只填写一个 IPv4，第一项为当前首节点；每台固定使用 device 0..7，允许空行和注释。脚本向其余节点同一绝对路径分发，目标必须为空或有 `.tilexr-validation-root` 的受管理目录。

按顺序逐条执行，每步成功后再执行下一步：

```bash
bash build_all.sh
```

```bash
bash sync.sh
```

```bash
bash run.sh -n 2 -e 96 -m 0 -l 3 0
```

构建应输出 `BUILD_PASS`，同步输出 `SYNC_PASS`，运行输出 `VALIDATION_PASS`。三个脚本各自负责一个步骤；`run_commit_validation.sh` 只提示迁移命令，不再一键执行。

`build_all.sh` 默认关闭设备打点，production 默认选择新的 source-send 检查点。同步和运行读取已安装的 manifest。若终端显式导出旧 `TILEXR_VALIDATION_CHECKPOINT=ep-sdedup-p1-chain-*` 或 `ep-sdedup-p2-first-hop`，先 `unset TILEXR_VALIDATION_CHECKPOINT` 再构建；当前代码拒绝沿用旧检查点。184B ABI 和 R07 单份布局沿用上一增量；kernel、consumer 和检查点身份需一起重新编译和同步。

同一普通产物可继续手动测量多轮完整 kernel：

```bash
bash run.sh -n 2 -e 96 -m 1 -l 100 10
```

每 rank 检查 msprof 恰好采到 warmup+loops 个目标 kernel 样本，剔除预热后统计 loops 个样本，并汇总各 rank 样本中位数的 median/min/max。测量范围为 cnt/cumsum、完整源端发送和设备轮末完成等待。有效带宽仍使用每次调用的第一跳/DIRECT 完整 T（含本卡拷贝）及远端 cnt 记录量，排除 polling 和完成代次元数据，除以完整 kernel 延迟；不把流量乘 loops 后除以单次延迟。原始样本和 kernel SHA256 随结果保留；此值不是纯网络带宽或完整 Dispatch 性能。sendData 内部打点保持相邻 MTE3 下发间隔，跨 rank 轮末等待计入 finish/完整 kernel。

## 整服务器调度与参数归属

同一 A5 产物同步到 8 台服务器后，可以连续运行 16P/32P/48P/64P 和不同专家数。
`hostfile` 是分发清单；`sync.sh` 使用完整清单，`run.sh -n N` 只选择前 N 台。
每台固定使用 0..7 卡，按清单顺序分配连续 rank。未选中节点不执行本次用例。

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

`bs=... h=... topk=... dtype=... quant_mode=... fp8_format=... block_dim=...`
继续作为可选覆盖项，默认 BS96/H7168/TopK6/BF16/非量化/auto。当前 route_mode
固定 balance_shift，seed=2026，每 rank 通信域固定 4 GiB。
`comm_id=... barrier_addr=... msprof=... hostfile=...` 可覆盖本次部署配置；
相对 hostfile 路径相对于仓库根目录。`-h`/`--help` 不加载 CANN。
`run_case.sh` 仅兼容转交到 `run.sh`，`run_batch.sh` 统一调用同一入口。
旧 `run_mode=... loops=... warmup=... moe_expert_num=...` 改用上述短参数。

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
- 测量：旧集群工具取最后 50 次样本，先算每服务器各卡全部样本平均，再汇总服务器平均；当前取最后 loops 次、逐 rank 样本中位数后汇总。应统一样本选择、跨 rank median/min/max、kernel SHA、通信字节和带宽定义。
- 打点：普通 Memory 使用独立的 `TILEXR_EP_DISPATCH_MEMORY_ENABLE_PROFILING` 和 v2 reader；当前 `build_all.sh breakdown=1` 只打开 Server Dedup 打点。性能对照两侧关闭设备诊断打点、保留 msprof；阶段对照使用各自 reader，不能仅替换 kernel 名。

普通 Memory 的完整 kernel 包含接收及最终 expert 行写出，而当前 Server Dedup 只到 source-send。当前可准备 Memory 完整基线或分析对应阶段；在 Server Dedup 接收闭环前，不把二者整核耗时比值报告为完整 Dispatch 加速比。接入时复用公共编译/同步/启动/采样流程，通过算法适配层分别选择 API、输出校验、profile reader 和流量计算；尚无可直接使用的 `run.sh impl=memory` 参数。

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
- 所有预期第一跳及 DIRECT 目标槽位、完整 hidden、量化 scale、32B 头部、有效 16B entries、原始 k/q/真实权重及每个 512B 块的 8 个 Flag。Flag 固定为 FP32 1.0 的位型；填充及未用 entries 不比较。
- DIRECT 两个 forward 计数为 0、返回字段为 -1，头部保留原始 k/真实权重。第一跳头部、有效 entries 和 DIRECT 展开后覆盖所有原始 route；尚未转发的 entry 槽位 Flag 保持零。
- sourceMask 保留各 group 的首 k 和全部 DIRECT k；expertScalesOut 对应为 1.0 和真实权重。原始 expertScales 逐字节不变，空洞不消费。
- 正常资源清理完成后才输出 `EP_SERVER_DEDUP_SOURCE_SEND_PASS`，其 `submitted=completed=loops`、`warmup_submitted=warmup_completed=warmup`、`cleanup=complete`。提交失败保留预热/测量各自已提交数；只有整批 stream 同步和 Host barrier 均成功才填写 completed。仅 `EP_SERVER_DEDUP_CHAIN_PASS` 不算成功。

固定 H/K 下非量化 T=15872B，MXFP8 T=8192B。`first_hop_packets/received/bytes`、`direct_packets/received/bytes` 和 `token_stride` 记录已校验物理包的流量。`local_data_bytes` 为本卡拷贝，`same_server_data_bytes` 为同 Server 跨卡，`cross_server_data_bytes` 为跨 Server；三者互斥并合计为全部包字节。JSON 同时要求 DIRECT、完整 route 覆盖及源端输出校验通过。全 Rank 第一跳和 DIRECT 的发送/读回包数分别一致；同址同值重复发送由本地发送观察测试检查，Host 最终读回不能单独识别。每源 rank 的远端 cnt 为 `(E-E/R)*32=2880B`，不包含轮询与本卡前缀访问。

调用方为真实权重输入和新增的 `expertScalesOut[BS,K]` 分别分配内存，两者不可重叠。公共签名在 `groupOut` 后新增权重输出；内部 launch 为 13 地址 + 10 个 int64，共 184B。最终 expandX/dynamicScales/assistEx/group 仍按完整容量分配，当前 checkpoint 不校验尚未产生的最终结果。

运行会在本次 `<run_id>/inputs/` 保存 `rank-N-x.bin`、`rank-N-ids.bin`、`rank-N-weights.bin`（小端原始位型）。远端输入随日志收集到 `nodes/<节点序号>/inputs/`；shape、dtype、量化、seed、commit/build_id 在同次 summary 中。直接运行 C++ 可用 `TILEXR_VALIDATION_INPUT_DIR` 指定已存在目录。

在收集结果的首节点执行以下命令，可将实际专家列表汇总为同目录的 `expert-ids-<卡数>ranks.json`，无需编译或运行 NPU：

```bash
python3 tools/validation/export_expert_ids.py /path/to/run/summary.json
```

卡数、BS、TopK 和专家总数取自该 summary，默认输出紧凑 JSON，保留 `expert_ids[rank][token][original_k]` 顺序及运行身份；需要人工阅读时追加 `--pretty`。
分析 relay 选择时应使用本次保存的列表并保留原始 k 顺序；跨环境仅凭 seed 重建可能受 `std::shuffle` 实现影响。
脚本仅扫描同次运行的 `inputs/` 和 `nodes/*/inputs/`，缺 rank、重复 rank、字节数错误或专家编号越界均报错，不导出部分结果；失败运行在输入齐全时也可导出用于诊断。汇总只证明输入可解析，不证明设备路由或中转负载均衡。

## Breakdown JSON

在首节点逐条执行：

```bash
bash build_all.sh breakdown=1
```

```bash
bash sync.sh
```

```bash
bash run.sh -n 2 -e 96 -m 2 -l 3 2
```

构建参数设置 `TILEXR_EP_SERVER_DEDUP_ENABLE_PROFILING=ON`；manifest 为 `server_dedup_profiling=true`、`server_dedup_profile_version=4`，并标记 184B ABI、`r07-single` 布局及 `source_send` 范围。`bash build_all.sh` 等同 `breakdown=0`，不受遗留 RUN_MODE 影响。同步只校验安装包，运行还校验模式与包的匹配；不手工编辑 manifest。

成功结果和 trace 路径：

```text
run/case-<时间>-<PID>/<run_id>/summary.json
run/case-<时间>-<PID>/<run_id>/traces/all-ranks.trace.json
run/case-<时间>-<PID>/<run_id>/traces/rank-N.trace.json
run/case-<时间>-<PID>/<run_id>/traces/fastest.trace.json
run/case-<时间>-<PID>/<run_id>/traces/p50.trace.json
run/case-<时间>-<PID>/<run_id>/traces/slowest.trace.json
run/case-<时间>-<PID>/<run_id>/traces/index.json
```

将 `.trace.json` 导入 Perfetto/Chrome tracing。`measurements.breakdown.stages_us` 给出各阶段核最大值的跨 rank median/min/max；`nodes[].reported_result.ranks[].breakdown.records[]` 保留 64 核的原始时间戳。

阶段为 `init`、`send_cnt`、`send_data`、`recv_cnt`、`cumsum`、`prefix_consume`、`finish` 和 `source_send_total`。源端核未执行的计数接收阶段为零时长，计数接收核的 send_data 为零；finish 包含末尾全核 SyncAll、core63 counter 清零及跨 rank 完成等待。时间戳从 Init 前到上述收尾后，排除对象构造和最终 trace 写出。breakdown 仅保留最后一次测量的粗粒度和逐包记录，generation=warmup+loops；不生成每轮 trace。各卡独立归零，不能推断跨卡绝对时序。

v4 在每个发送核的 `send_data` 下细分三个连续区间，`stages_us` 同时汇总这三项：

| trace 名称 | 起止时间点 |
| --- | --- |
| `send_prepare` | `send_cnt_end` → 第一个物理包的 MTE3 下发点，包含首包准备 |
| `send_first_hop` | 第一个待中转包的 MTE3 下发点 → 第一个 DIRECT 包下发点；没有 DIRECT 时终止于 `send_data_end` |
| `send_direct` | 第一个 DIRECT 包下发点 → `send_data_end` |

缺少某类包时对应发送阶段为零；本核无 Token 时只有准备区间。每个物理包在 `DataCopy` 下发后立即调用 `GetSystemCycle`，没有新增 `PIPE_ALL`、SetFlag/WaitFlag 或完成等待。已有粗粒度时间戳仍保留原来的 PIPE_ALL；读时钟和 UB 标量记录本身仍有诊断开销。普通性能基线关闭设备打点，performance 的 msprof 仍保留。恢复普通产物需再次手动执行 `bash build_all.sh`、`bash sync.sh`。

`nodes[].reported_result.ranks[].breakdown.records[]` 的发送核增加 `first_hop_count` 和 `send_packets: [{issue, token, k}, ...]`。数组按实际发送顺序排列，前 `first_hop_count` 项为待中转包，其余为 DIRECT；同一输入 Token 可产生多个物理包，`token/k` 标识该包的原始路由头。trace 中每包都有 `MTE3 issue` 瞬时点，`send_issue_interval` 段表示 **本包下发 → 下一包下发**，跨 first-hop/DIRECT 切换的间隔归前一个包。它包含下一包准备、已有等待与下发开销，不表示本包 DMA 搬运完成耗时。最后一包没有下一下发点，只保留瞬时点，并将其后至 `send_data_end` 单列为 `send_drain_and_outputs`；该收尾包含已有 lane 排空、mask/weight 输出及粗边界同步，不冒充 Token 间隔。各段核最大值不可直接相加。

R07 单份布局从 offset=0 预留 12800B profile，cnt 从 12800B 开始；v3/v4 粗粒度记录每核两个 64B 行，共 8192B。v4 逐包记录暂存在独立 UB，最终 `end` 取时后才写到通信域 payload 尾部，必须避开 `peerMemBytes` 中最后 4MiB 的 communicator flags。每核最多 `ceil(BS/48)*TopK` 个 16B 记录，加 32B 头后按 64B 对齐；48 核总区按 512B 对齐。固定 BS96/TopK6 时占用 12288B GM，每发送核增加 192B UB，计入 lane 容量计算，仍保留 8 个 lane。Host 拒绝数据区与诊断区重叠，现有 cnt/数据偏移和 4GiB 通信域大小不变。reader 校验版本、核/rank、generation、包数和时间范围，汇总再核对 first-hop/DIRECT 包数，禁止静默截断。

已有 v3/v4 成功 summary 可离线导出，无需再次启动设备；v3 只能显示原有粗粒度阶段，无法补出逐包时间点：

```bash
python3 tools/validation/measurements.py run/case-实际目录/实际run_id/summary.json
```

## 失败定位与本地检查

| 步骤 | 日志 | 首先查看 |
| --- | --- | --- |
| 编译 | `run/validation/build-*/production-*.log`、`host-*.log` | 终端指出的编译/链接命令及错误 |
| 同步 | `run/validation/sync-*/<节点>/prepare.log`、`transfer.log`、`verify.log` | SSH、分发路径和安装包身份 |
| 启动前产物检查 | `run/case-*/<run_id>/verify-<IP>.log` | 对应节点安装包及 build_id |
| 运行环境 | `run/case-*/environment.log` | CANN、Python、配置加载 |
| 运行 | `run/case-*/workflow.log` 和终端指定的 `<run_id>/summary.json` | primary_error、errors、失败 rank 的日志与 stage |

`stage=count-read` 或 `stage=source-send-read` 表示 stream 与 Host barrier 已完成，随后数值/包校验失败，不代表等待超时。错误保留首个不匹配字段、source/token/k/q 或 count 邻域。count 输出预填 -1，mask/权重输出预填哨兵；保留实际原始输入供复现。提交、同步或 barrier 失败而完成状态未知时，不释放可能仍在使用的设备资源，交由进程退出和调度器收尾。

不带 CANN/NPU 的聚焦检查：

```bash
cmake -S tests/ep -B build/server-dedup-host
cmake --build build/server-dedup-host --target \
  test_tilexr_ep_server_dedup_protocol test_tilexr_ep_server_dedup_layout \
  test_tilexr_ep_server_dedup_host_validation test_tilexr_ep_server_dedup_public_abi \
  test_tilexr_ep_kernel_launch test_tilexr_ep_server_dedup_counts \
  test_tilexr_ep_server_dedup_profile test_tilexr_ep_server_dedup_source_send \
  test_tilexr_ep_server_dedup_source_send_profile test_tilexr_ep_server_dedup_host_profile_validation -j8
ctest --test-dir build/server-dedup-host --output-on-failure \
  -R 'test_tilexr_ep_server_dedup_|test_tilexr_ep_kernel_launch$'
python3 tests/ep/integration/test_server_dedup_chain.py
```

本地 sender 测试执行真实非量化生产逻辑，覆盖原始 q、全部隐藏数据/Flag/尾表、UB/输出边界和环形池复用；在 GM 搬运替身记录实际发送，核对第一跳优先、DIRECT 的 token/k 顺序、每个槽位恰好一包及跨阶段 lane 序号连续。入口六种类型分支做替身编译，Host consumer 六种类型做夹具及错包注入；这些不证明真实 MXFP8 计算、异步 MTE、SyncAll 或 CANN/A5 通过。服务器结果按实际 shape/模式登记到 [调试进度](EP_MEMORY_SERVER_DEDUP_PROGRESS.md)。

逐包打点的本地回归还核对每条记录与实际发送对应、空核头、容量/同步标志区边界、Host 容量拒绝、损坏记录拒绝、三个子阶段及最后一包的 trace 语义。纯 CPU 替身只验证记录和导出契约，真实 CANN 9.1 编译、A5 可见性及诊断开销仍需服务器运行确认。维护时不要在热路径逐包写 GM/DCCI，也不要为获取完成时间新增同步；相邻下发点才能保持这里约定的测量含义。

维护经验：通信域由双份计数布局迁移到单份数据布局时，必须同时更新 Host 容量、设备地址、profile reader、manifest 和当前检查点，禁止复用旧计数通过标记。源端发送批次在所有 rank 完成本轮后覆盖相同路由槽位，数据 Flag 保持 1；这只覆盖当前无数据消费者的检查点，不能证明未来接收/Combine 的清理复用。量化输入尾部必须按 Memory 显式补零，并在复用输入 UB 前完成初始化写入；非量化传输填充无需清零。

单份 cnt/发送槽位的连续调用必须有跨 rank 完成边界；本卡 SyncAll 不能阻止快 rank 覆盖慢 rank 的上一轮记录。当前核63在已有全核收敛、counter 写零完成后，将本次 NextMagic 的四个 int64 副本写入各 rank 的专属 32B 行，再等待本地所有 rank 行均 >= 本代次。记录位于 4MiB communicator Flag 区的最后 4KiB（最多128行），不移动 payload、cnt 或 profile；这些行不清零，允许更晚代次满足已完成的旧代次。仅支持同一 communicator 上所有 rank 同序、同 stream 的 Dispatch 批次；不得与其他未完成操作并发复用此区域。本地回归覆盖迟到/部分/旧代次、后续代次越过等待、四轮复用和边界，设备可见性及实际等待开销仍须 A5 验证。

新增 Host CTest 必须安装并输出与测试名对应的 marker（如 `EP_SERVER_DEDUP_ABI_HOST_PASS test=source_send`）。EP 自定义链接命令必须依赖 `$<TARGET_OBJECTS:...>`，保证对象重编后最终外部 kernel 重链；生产 Host 库继续相对加载、注册独立 AICore 二进制。CPU 编译/链接和故障注入只证明这些边界，不替代设备验证。

## 输出和验收范围

以下保留历史检查点说明，原文中的“当前提交/当前阶段”指记录形成时的代码。当前源端发送操作步骤及通过条件以上文为准；不运行下面的旧空 kernel 或 breakdown 验收。

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

breakdown 成功后自动生成 `run/case-<时间>-<PID>/<run_id>/traces/`：

- `all-ranks.trace.json`：全部卡，每 rank 一个 process，每 AIV core 一个 thread。
- `rank-N.trace.json`：每张卡的独立 trace，所有卡均保留。
- `fastest.trace.json`、`p50.trace.json`、`slowest.trace.json`：按 Memory 版本相同规则筛选的三张代表卡。
- `index.json`：全卡排序、筛选结果和文件路径，也写入 summary.json 的 traces 字段。

将 `.trace.json` 拖入 Chrome tracing；summary.json/index.json 是汇总索引，不能当作 trace 导入。核心结果区打印全卡和三份筛选 trace 的绝对路径，人工批测和提交验收复用同一导出流程。

筛选依据为每 rank 的 `max_core(end-start)`，按耗时升序、同耗时按 rank 排序；p50 选择 `ceil(N/2)-1` 的实际卡，不平均两张卡。各 rank 时间戳先以本卡最早 start 做整数减法，再按 1000 ticks/us 转换；不同卡不具备同步时钟，合并图不能用来判断跨卡先后关系。每核输出 entry_checks、record_prepare 两段，完整区间为二者之和。当前仅保存最后一轮打点，空 kernel 也必须有有效事件；不声称输出所有轮次或算子算法阶段。

已有成功 breakdown summary 可以离线导出，不重新编译或启动 NPU：

```bash
python3 tools/validation/measurements.py run/case-实际目录/summary.json
```

可用 `--output-dir run/trace-review` 指定输出目录。缺卡、缺核、错误 generation 或非法时间戳使导出失败；自动导出失败不能将该 breakdown case 标为 PASS。当前验证重点是解析、Chrome Trace 序列化、全卡覆盖和筛选，不重复旧硬件检查点；后续算子内容增加时再扩展设备打点。

breakdown 的关键结果区和 summary.log 额外打印 `JSON: <绝对路径>`，指向 `run/case-<时间>-<PID>/summary.json`。其中 `measurements.breakdown.stages_us` 为微秒汇总，`stages` 为原始 ticks 汇总，`rank_stages` 为每 rank 各段的跨核最大值；各核原始时间戳与 generation 位于 `nodes[].reported_result.ranks[].breakdown`。若未完成汇总，measurements 可能缺失，应查看 errors 和节点记录。

每次运行在仓库 `run/` 下生成独立目录，保留节点/rank 日志、summary.json 与 summary.log。关键输出使用空行和 32 个 `=` 分隔，在 PASS 或 kernel 性能之前展示两行实际 case 参数。只统计 kernel 耗时，不采集或汇总 Host submit/sync/barrier/batch_total 时间。现有 performance average 是各 rank 样本中位数的算术平均，不能误称为全部样本平均。

同一 stream 连续提交整批，批末才 stream 同步与 Host barrier；删除 Host 计时不改变同步策略。functional 不采集 profiling；performance 使用 msprof；breakdown 读取最后一轮各核的设备记录。

当前阶段 1 是空 Dispatch 框架，PASS 不证明去重通信或输出数值。64P/128P 仍待环境具备后验收，16P 通过不扩大到未测规模。提交验收仅保留本次受影响 case；测试脚本重构不要求重新执行已经通过的硬件检查点。

## 后续维护约束

接收解包曾在 A5 `dav_3510` 编译时触发 `SupportBytes<unsigned char, 2, 4>()`：用户日志中的 CANN 9.2.0 `CopyImpl` 只支持 b16/b32，`recv.h:127` 是实例化提示，真正调用错误在 `Copy<uint8_t>`。用 b32 视图按 mask 64/56 分两次复制每包的 480B 数据，源/目标 repeat stride 分别为 16/15 个 32B block；repeat 是包数，不是 `包数*15`（H=7168 时后者为 450，转 uint8 会截断）。量化路径解包范围包含 padding 和 scales，现有两处同步分别对应 MTE2→V 与 V→MTE3。CPU 替身同步限制元素位宽，完整入口 profiling=0/1 编译和包布局回归验证类型、内容与 UB 边界；不能替代 CANN 9.1/A5 实编译或异步设备验证。判断工具链版本应依据真实 include 路径和版本信息，不能仅凭项目默认版本。

分步开启 DIRECT 时必须一起更新检查点、Host 包集合、sourceMask/权重预期和流量统计，否则已发送的 DIRECT 会被旧校验误报为非目标槽位，或性能 JSON 仍把其流量记为零。最终槽位内容无法识别同址同值的重复发送；本地在搬运替身观察每包地址、身份和发送池顺序，补充次数与核内顺序证据，不在生产两阶段之间加同步。该证据不证明真实异步 MTE 顺序或 A5 性能。

breakdown 编译曾在 profile record 转换处报 `casts away qualifiers`：CANN 将 `GM_ADDR` 定义为指针宏，`const GM_ADDR` 实际限定指向的数据为只读；CPU 替身的 `using` 定义却限定指针本身，掩盖了问题。可写 profile 区应显式使用 `__gm__ uint8_t *const`，替身同步采用指针宏语义。

接收侧 `PayloadReady(slot, flags)` / `ClearFlags(slot, flags)` 又暴露了地址空间遗漏：`slot` 来自 GM，但形参曾使用普通 `const uint8_t *`，`const_cast<uint8_t *>` 还试图去掉 GM 地址空间。沿槽位地址、Flag 读取/清零和转发目标始终保留 `__gm__`，只固定指针时写 `__gm__ uint8_t *const`；`CommArgs` 自身保留 const，读取其中的可写 peer 指针无需去除 const。普通 CPU 替身将 `__gm__` 置空会漏报此类错误；`test_server_dedup_address_space` 用 Host Clang 独立地址空间对完整生产入口做 profiling=0/1 语法编译，未修复源码可复现这两处调用错误，修复后通过。可运行 `python3 tests/ep/python/test_server_dedup_address_space.py`；缺少 Host clang++ 时显式跳过。此检查验证 C++ 地址空间/限定符，不证明 CANN API、设备行为或性能。接收头文件同时列入外部 kernel 构建依赖，防止只改头文件却复用旧产物。

第一跳 Flag 初始化曾在 `Duplicate<float>` 处报 `no matching function`：[官方 asc-devkit v9.1.0 接口](https://gitcode.com/cann/asc-devkit/blob/e5451c7feba11f9e259f4fbc19c4bf82f8c0496c/include/basic_api/kernel_operator_vec_duplicate_intf.h) 的位掩码参数为 `uint64_t mask[]`，不能传 `const uint64_t[]`。应使用可写局部掩码，仍仅初始化每个 512B 块中的 32B Flag；CPU 替身也必须保持该重载的参数限定符和整数宽度。收紧替身后旧调用可复现同一编译错误，修复后用现有 sender/count/profile 测试检查包内容、边界和计数；该检查不替代真实 CANN 编译或 A5 运行。

- 环境配置只读，不用追加参数/备份恢复方式传递 case；那会造成双份配置、覆盖用户修改和多机不一致。
- 核心参数由调用方提供，批测只组合 case，不隐式编译或同步。提交验收按实际需要决定部署步骤。
- checkpoint 只在提交验收入口选择；人工运行继承安装产物身份，结果仍记录 checkpoint。
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

以下三段保留历史版本的开发证据，不代替本次精简后的回归。2026-09-09 初始实现验证：Windows/MSYS2 上 41 项脚本行为测试中 40 项通过，1 项因文件系统未创建真实 symlink 而跳过；PowerShell 5.1 同步入口通过模拟传输执行了实际导出、Bash 解包、刷新和拒绝覆盖检查。全新且路径含空格的部署目录中，四个服务器入口以 `mode=host` 完成实际 C++14 Debug 构建、安装和 17/17 Host/Python 测试。CANN 生产构建、真实 SSH 集群同步及 NPU 执行未在本地验证。

同日普通文件同步修复：临时子模块移除 `.git` 后复现 `fatal: not a tree object`。修复后 52 项脚本回归中 51 项通过，1 项因 MSYS 未创建真实 symlink 而跳过；包含普通同步清单跨模拟节点分发及结果摘要检查。无 `.git`、无导出清单且路径含空格的普通复制目录中，首次构建和修改源码后的重建均通过四个服务器入口，每轮 17/17 Host/Python 测试成功；用 Git 拦截程序确认两轮服务器流程均未调用 Git，hostfile 字节不变。hostfile 保持手动配置，不再自动生成。本地没有 CANN/NPU，生产编译及真实 SSH 集群验证仍需以服务器日志为准。

同日可选 spdlog 修正：新增回归先复现 production 模式被空 spdlog 目录阻断，移除预检后确认能进入生产 CMake 配置调用（测试在调用边界停止，不代表生产构建成功）。52 项脚本回归中 51 项通过，1 项 symlink 跳过。在临时目录仅复制真实 `tilexr_log.h`，不提供 spdlog 头文件、不定义 `TILEXR_LOG_DISABLE_SPDLOG`，按项目默认 GNU C++14 模式编译并运行现有日志测试通过，编译断言确认使用自动替代路径；现有 fake-spdlog API 测试亦通过。MSYS 严格 `-std=c++14` 下现有测试的 POSIX `setenv`/`unsetenv` 声明不可见，故使用与 CMake 一致的 `-std=gnu++14`；未修改生产日志代码或测试源码。CANN 生产编译与 NPU 执行未在本地验证。

## 七步流程检查审计与落实（2026-09-09）

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

人工测试与提交验收只采集和统计 kernel 耗时，不采集、校验或输出 Host submit/sync/barrier/batch_total 时间。stream 同步与 Host barrier 仍按整批执行，只移除计时。成功/失败与 kernel 统计前后使用空行和 `==========` 分隔；functional 不输出耗时。performance 的现有 mean_us 是各 rank 样本中位数的算术平均，统计口径保持不变。此变更仅需验证结果解析和输出，不重复执行已通过的硬件检查点。

关键结果分隔线统一为 32 个 `=`。PASS 或 kernel 性能前先输出两行实际 case 配置：算子/模式/测量与预热轮次，以及 BS/H/TopK/专家数/dtype/卡数/量化模式。信息取自本次 summary，终端与 summary.log 一致；当前明确标注 Dispatch server-dedup 空 kernel，不代表已实现 Combine 或去重数据面。

批次结论只在至少执行一个 case 且全部返回成功时显示 `Batch PASS cases=N`。构建、同步或任一 case 非零退出立即停止并保留非零退出码；空清单不能显示 PASS。核心还检查 summary.status/errors，不能仅凭外层进程 exit=0 把失败结果标成通过。Shell 的 set -e 不能替代显式结果判断。
