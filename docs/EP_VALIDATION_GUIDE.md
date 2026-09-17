# EP 验证与人工测试

## 当前提交：完整 Dispatch（设计验证流程 §2.5）

当前检查点为 `ep-sdedup-p2-dispatch`：48 核执行第一跳及循环 DIRECT，16 核完成 cnt/cumsum 后接收、转发并写最终 expert 行、assistEx/group；核末完成本核写出及当前窗口的数据 Flag 清理后独立返回，不再执行 SyncAll；第一跳 forwardTokenIdx 使用 Token 级 prefix 输出 row，不再执行 AtomicAdd 或 counter 清零；不再发布/等待跨 Rank 结束握手。Combine 尚未实现。完整 Dispatch 的设备验收使用下面的多卡运行流程，`EP_SERVER_DEDUP_ABI_HOST_PASS test=receive_packet` 只表示 Host 替身测试通过。

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

接收以完整前缀确定连续输出行，在 16 核按 floor 分区；tokenPos/pendingIdx 均只存 UB。每批独立探测最多 16 个 Token 的全部块、全部 8 个 Flag word，记录稳定索引，按原 pending 位置逆序消费就绪项并末项覆盖删除。整批未就绪时继续扫描；有删除则重查批起点的换入项。

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
bash run.sh -n 2 -e 96 -m 0 -l 1 0
```

构建应输出 `BUILD_PASS`，同步输出 `SYNC_PASS`，运行输出 `VALIDATION_PASS`。三个脚本各自负责一个步骤；`run_commit_validation.sh` 只提示迁移命令，不再一键执行。

`build_all.sh` 默认关闭设备打点，production 默认选择完整 Dispatch 检查点。同步和运行读取已安装的 manifest。若终端显式导出旧 `TILEXR_VALIDATION_CHECKPOINT=ep-sdedup-p1-chain-*` 、`ep-sdedup-p2-first-hop` 或 `ep-sdedup-p2-source-send`，先 `unset TILEXR_VALIDATION_CHECKPOINT` 再构建；当前代码拒绝沿用旧检查点。内部 kernel launch ABI 现为 192B（追加 profiling launch slot），R07 单份布局不变；kernel、Host 库、consumer 和 manifest 需一起重新编译和同步。

单轮通过后，用同一产物检查完整 Dispatch 多轮复用：

```bash
bash run.sh -n 2 -e 96 -m 0 -l 3 0
```

再手动测量完整 kernel：

```bash
bash run.sh -n 2 -e 96 -m 1 -l 100 10
```

每 rank 检查 msprof 恰好采到 warmup+loops 个目标 kernel 样本；剔除预热后保留全部 loops 个原始样本，模式 1 的最终统计只取每卡最后 `N=min(loops,50)` 个测量样本。每台 server 固定 8 卡，server 代表耗时是这 `8×N` 个样本的算术平均；最终输出各 server 代表耗时的 max/min/mean（单位 us）。例如 `-l 100 50` 采集 150 次，仅最后 50 个测量轮次参与统计，每台 server 共 400 个样本；`-l 20 50` 每台只统计 8×20 个测量样本，不用 warmup 补足 50 轮。

测量范围为完整 Dispatch，包含 cnt/cumsum、源端发送、接收转发、输出、清理和轮末完成等待。有效带宽使用每次调用的第一跳/DIRECT/relay 完整 T（含本卡拷贝）及远端 cnt 记录量，排除 polling 和完成代次元数据，按 server 汇总 8 卡每次调用的字节数，再除以上述 server 平均 kernel 耗时，得到各 server 的有效 GB/s，最终也输出跨 server max/min/mean。该值是 server 聚合吞吐，不能和旧逐卡带宽直接比较；不把流量乘 loops 后除以单次延迟。原始样本和 kernel SHA256 随结果保留，此值不是纯网络带宽。

缩小统计窗口不等于放宽采集完整性：仍验证所有 warmup+loops 样本的数量、合法耗时和唯一开始时间；缺卡、缺轮或损坏的早期样本不能借“只看最后 50 轮”绕过验证。此统计转换由 Host Python 完成，替身回归只证明解析与汇总，不证明 NPU 正确性或性能。

sendData 细打点仍表示相邻 MTE3 下发间隔；recv_data 包括接收准备、等待、输出、转发与本核 Flag 清理，finish 包括全核及跨 rank 收尾。

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

`bs=... h=... topk=... dtype=... quant_mode=... fp8_format=... block_dim=...`
继续作为可选覆盖项，默认 BS96/H7168/TopK6/BF16/非量化/auto。当前 route_mode
固定 balance_shift，seed=2026，每 rank 通信域固定 4 GiB。
`comm_id=... barrier_addr=... msprof=... hostfile=...` 可覆盖本次部署配置；
相对 hostfile 路径相对于仓库根目录。`-h`/`--help` 不加载 CANN。
`run_case.sh` 仅兼容转交到 `run.sh`，`run_batch.sh` 统一调用同一入口。
旧 `run_mode=... loops=... warmup=... moe_expert_num=...` 改用上述短参数。

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
- 测量：模式 1 取最后最多 50 个测量样本，先算每服务器 8 卡全部选中样本平均，再汇总服务器均值的 max/min/mean。与旧集群工具比较时仍需统一 warmup、样本选择、kernel SHA、通信字节和带宽定义。
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

`chrome/` 集中放置每 rank 一份 Chrome Trace JSON；`index.json` 记录文件路径，并按最后一轮 kernel 时长选择 fastest/p50/slowest rank。每核时间归零不构成跨核或跨 NPU 时钟同步。`measurements.breakdown.kernel_us` 取每 rank 最后一轮的核最大值，再汇总跨 rank median/min/max；保留各 rank 值，不先合并服务器中位数。操作统计使用该轮实际配对区间；多次操作的中位数不能称为多轮执行中位数。performance 仍使用关闭设备打点的 msprof 多轮样本，统计口径独立。

需要从原始记录重新解析时，显式取回各服务器的 `rank-N.records.v1` 到同一本地目录，再用 `--records-dir` 生成每 rank 一份 JSON；输出目录须为空或尚不存在。默认执行仅归集已完成的 JSON：

```bash
python3 tools/validation/measurements.py /path/to/run/summary.json --records-dir /path/to/copied-records --output-dir /path/to/new-report
```

维护要点：保持事件位置与记录机制分离；新增点位只扩展数字 ID 和 mapping，不重新引入 UB 暂存或算子专属 cache flush。复用槽位必须同时处理有效范围和丢点状态，不能只把 slot 改成 0。跨 Host/kernel/reader 改动需同时校验 launch ABI、固定分配预算、长轮后短轮/空轮、截断状态及安装产物身份。多机 profiling 应在数据所在节点完成解析，再收集成品，避免每卡本地解析后主节点又解析两遍。

2026-09-17：为区分 SendCnt 的统计和通信开销，追加两个区间（旧事件 ID 保持不变）：

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

此类输出错误不取消其他 rank/节点：所有节点先各自解析，主节点再归集每 rank JSON，最后打印 `VALIDATION_FAIL` 并返回结果校验失败退出码。`traces/index.json` 的每 rank `validation_status`/`validation_error` 与采集完整性 `status` 分开；全部 rank 的计时完整时，即使输出校验失败，也保留 `measurements.performance` 的跨 server max/min/mean 或 `measurements.breakdown` 的跨 rank median/min/max；错误结果或因日志收集失败等原因未完成跨 rank 校验时，通信量为 null、带宽为 N/A，不使用未通过完整校验的流量。缺卡、样本不完整或损坏时不生成部分 rank 的性能汇总。所有模式的算例最终状态仍为 FAIL、退出码非零。设备/API 错误、超时、释放失败以及旧运行身份或损坏 capture 仍属于执行/采集失败，不能通过输出错误标记绕过。

维护此流程时，须分别验证“真实 consumer 输出错误后完成导出”和“节点/主节点不因该错误取消同伴”；只验证原始 records 文件存在会漏掉上层提前退出。若把输出不一致直接作为 consumer 执行异常退出，msprof 或调度器可能提前退出/取消同伴，模式 1、2 的完整耗时因此丢失。维护时应保持 INVALID 与执行失败分离，并验证最终 case 仍失败。Host 替身可覆盖上述失败处理和 JSON 归集，不证明 NPU 上固定 `t=0` 实验本身能正常结束或其性能收益。

每次运行在仓库 `run/` 下生成独立目录。`run.sh` 终端只输出一行最终汇总，并保存为 `result.txt`：PASS 携带可用的性能数据或 trace 路径；FAIL 分为“执行阶段失败”“执行卡死（超时）”“结果校验失败”，有完整计时/trace 时一并保留。执行超时不等价于已证明 kernel 死锁。环境详情在 `environment.log`，可选 Host 回归在 `host-validation.log`，调度输出在 `workflow.log`，入口重定向等错误在 `launcher.log`，逐卡日志和结构化结果在汇总指定的 `<run_id>/summary.json` 同级目录；配置错误可能指向 `failure.json`。不再向终端展开各卡错误栈。只统计 kernel 耗时，不采集或汇总 Host submit/sync/barrier/batch_total 时间。模式 1 的统计口径写入 `measurements.performance.selection/reduction`，`samples_per_rank`/`samples_per_server` 记录有效样本数，`server_mean_us` 按 hostfile 的 server 顺序记录代表耗时；`mean_us` 是这些 server 均值的算术平均。

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
