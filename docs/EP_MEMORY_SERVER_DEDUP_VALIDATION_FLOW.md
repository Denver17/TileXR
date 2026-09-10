# EP Memory Server Dedup 验证流程

## 文档状态

本文记录 EP Memory Server Dedup 的分阶段代码生成、Git checkpoint 和上机验证流程。
当前只详细规划第一阶段：Dispatch Server Dedup 的公共框架和空 kernel。
第一阶段不声称已经实现或验证 Server Dedup 算子功能；本文中的新增文件、接口声明和命令仍是执行规划，不表示仓库已有对应实现。
接口和逻辑以[主设计](specs/ep-memory-server-dedup.md)中的最新已确认结论为准；待确认文档中的推荐或候选不自动成为实现要求。
本文只补齐第一阶段所需的工程约定，不提前设计第二至第四阶段，也不要求后续算法议题全部关闭才能开始空算子工程。

2026-09-09 接口复核：主设计已明确只新增 `assistEx/sourceMask/group` 三类 metadata，不新增 descriptor、group 注册、X 或独立 `tokenNum` 参数。
旧 `forwardPlan` 仅为历史名称；旧三字段 assist 可保留接口占位，但去重逻辑不额外复制同义表。
本文据此撤销原有版本化 metadata descriptor 和 opaque forwardPlan buffer 规划。

适用平台为 Ascend 910A5/Ascend950 A5 环境，目标工具链为 CANN 9.1.0，保持 C++14。
910B Host 或 simulator 只能验证 Host/构建边界，不能作为 A5 kernel 执行证据。

## 总体验证策略

每个功能增量都遵循以下闭环：

1. 在代码仓库中完成一个小功能集，通常包含 2 至 3 个紧密相关的功能点；大型功能点可以单独形成一个 checkpoint。
2. 在本地执行对应的 Host、编译或脚本检查。
3. 仅提交当前会话的改动，将 checkpoint 名称绑定到完整 commit SHA；服务器通过 `git checkout --detach <sha>` 切换到该点。
4. 完成对应 commit 的构建、安装和产物核对后，在目标 A5 服务器执行一个带 180 秒总超时的验证命令。
5. 只输出一个显著结论：`成功` 或 `失败`。失败时保留 commit、配置摘要、首个失败 rank 和日志。
6. 只有当前 checkpoint 成功，才开始下一个增量；失败则在同一 checkpoint 返工，不跨越失败点继续开发。

每个多进程脚本都必须在超时或异常时终止本次运行在各节点上的整个子进程组，先保留失败证据，再清理临时运行状态。
单 rank 执行期限为 120 秒，控制端总期限为 180 秒，包含启动、运行、终止、日志收集和汇总；清理规则见下文。
构建和安装属于准备步骤，使用独立、显式记录的 `build_timeout`，默认 900 秒，不计入这 180 秒，也不伪装为运行成功。

性能测试只验证 profiling、warmup、循环和结果汇总框架是否工作。算子为空时，报告内容必须标注为 launch/framework 开销，
不能称为 Dispatch 算法性能。

首次真正执行通信数据面前，按 `AGENTS.md` 的环境基线复用匹配的 HCCL Test 资格记录；不为每个 checkpoint 重复执行。

## 代码生成阶段

### 第一阶段：Dispatch Server Dedup 框架和空算子

目标是定义可稳定消费的 Dispatch Server Dedup 公共接口，并完成 Host、layout、kernel artifact、注册、launch、测试编排和超时处理。
普通构建的 kernel 入口只保留最小入口防御后直接返回，不读取路由或 hidden，不读写 Dispatch 输出，不验证算子数值。
profiling 构建允许向独立 profiling 区写入入口、出口和本次调用身份，仍不读写 Dispatch 输出或通信协议状态。

第一阶段保持普通 `TileXRMoeEpDispatchMemory` ABI 和行为不变，新增独立的 Dispatch Server Dedup 入口。
按主设计的[公共接口与 metadata shape](specs/ep-memory-server-dedup.md#dispatchcombine-公共接口与-metadata-shape)，
新增三个 caller-owned 设备输出指针 `assistExOut/sourceMaskOut/groupOut`，不增加 descriptor 或第四类 metadata。

第一阶段只验证已有布局的字节计算、指针传参和资源契约，不生成或解析输出内容。
`assistEx` 的 32B 行、`sourceMask` 的 4B stride 和 `group` 的固定行 stride 已确认，不再列为待设计项。
真实 `recvSlot` 编码、有效 group 识别、weight 消费和通信区域预算等，等对应阶段设计确定后再执行。

### 第二阶段：逐步实现 Dispatch 算子逻辑

在第一阶段接口和框架通过后，按小步加入 server grouping、第一跳发送、relay 展开、metadata 写入、接收布局和真实 Dispatch correctness。
每个逻辑块继续单独提交、单独上机；不把多个未验证的通信阶段合并成一个不可定位的大提交。

### 第三阶段：Combine Server Dedup 空算子工程

在 Dispatch metadata ABI 稳定后，增加独立的 Combine Server Dedup API、空 kernel、Host layout、注册、launch 和测试框架。
该阶段仍不实现 Combine 数值逻辑，只验证输入 metadata 的生命周期和空算子调用。

### 第四阶段：逐步实现 Combine 算子逻辑

按 expert 返回、group 监听和归约、partial 返回、source 归约、数值正确性和性能报告顺序逐步实现。
Combine 的真实验证必须使用可区分的 expert 输出，不能使用会互相抵消错误的联合测试作为唯一证据。

## 第一阶段接口和资源契约

### 空入口声明

下列声明是第一阶段的工程实现约定，基础参数沿用当前 `TileXRMoeEpDispatchMemory` 的类型和相对顺序，
仅在末尾 dtype/stream 参数前加入三个 metadata 输出。它不表示 R10 的后续算法支持矩阵或 Combine 签名已关闭。

```cpp
int TileXRMoeEpDispatchMemoryServerDedup(void *x, int32_t *expertIds, void *scales, bool *xActiveMask,
    int64_t activeMaskType, void *expertScales, TileXRCommPtr comm, int64_t bs, int64_t h, int64_t topK,
    int64_t moeExpertNum, int64_t epWorldSize, int64_t epRankId, int64_t tpWorldSize, int64_t tpRankId,
    int64_t expertShardType, int64_t sharedExpertNum, int64_t sharedExpertRankNum, int64_t quantMode,
    int64_t globalBs, int64_t expertTokenNumsType, void *expandXOut, void *dynamicScalesOut,
    int32_t *assistInfoForCombineOut, int64_t *expertTokenNumsOut, int32_t *sendCountsOut,
    int32_t *tpRecvCountsOut, void *expandScalesOut, uint32_t *assistExOut, uint32_t *sourceMaskOut,
    uint32_t *groupOut, TileXR::TileXRDataType dtype, TileXR::TileXRDataType expandXOutDtype,
    aclrtStream stream);
```

沿用公共头文件的 C++14 类型和 `extern "C"` 导出方式。`assistExOut` 的整数和 FP32 字段均使用 32 位原始位视图，
不能将 weight 数值转换为整数。旧 `assistInfoForCombineOut` 在新入口中仅为占位，允许为空，不读写或复制旧三字段表；
旧入口继续遵守原有契约。不新增 `tokenNum`、X、groupCount、plan handle 或 metadata 版本参数。
内部 launch 参数和 profiling record 可以有内部格式版本，但不将其扩成新的公共 metadata descriptor。

### 本阶段 Host 检查

以下是空工程的调用范围及错误处理，不是对尚未确认的真实算法能力作出承诺。

| 项目 | 第一阶段约定 |
| --- | --- |
| 平台 | A5 才能执行；非 A5 返回 `TILEXR_ERROR_NOT_SUPPORT`，不注册 kernel 或消耗 magic |
| Rank/expert | 每 Server 8P，R 为 16 至 128 且为 8 的倍数；rank 在 `[0,R)`；`moeExpertNum=R*L`，`2<=L<=32`；`1<=topK<=32` |
| Shape | `bs>0`、`h>0`；`globalBs=0` 时推导为 `R*bs`，非零时必须相等；EP world/rank 的默认值处理沿用当前 Memory Host 规则 |
| 格式 | 空工程先接通 FP16/BF16、`quantMode=0`，输出 dtype 为输入 dtype 或 `RESERVED` 表示继承；不实现量化或转换 |
| 核数 | 默认使用设备报告的 Vector 核数；显式 `block_dim` 必须为正且不超过该设备核数，不沿用旧算法双 Group 的限制 |
| 不支持功能 | mask 必须为 NONE/null，共享专家参数均为 0；TP world 为 0/1、rank 为 0，shardType 为 0；`scales/dynamicScalesOut/tpRecvCountsOut/expandScalesOut` 为空 |
| Weight 占位 | `expertScales` 允许为空或传入 caller-owned FP32 `[bs,topK]` buffer，本阶段只传参、不读取；空值不在本文定义为算法 weight=1 |
| 必需资源 | `x/expertIds/expandXOut/expertTokenNumsOut/sendCountsOut` 和三个 metadata 指针非空，使用同设备有效 stream；count 输出类型沿用 Memory 支持的 prefix/count 枚举 |
| 错误分类 | 无效 shape、空必需指针、对齐或算术溢出为 `TILEXR_ERROR_PARA_CHECK_FAIL`；不支持配置为 `TILEXR_ERROR_NOT_SUPPORT`；communicator 和 runtime 错误沿用现有错误码 |

可判断的 Host 参数错误在申请本次 magic 和调用 runtime launch 前拒绝；设备入口的直接返回不能充当参数错误报告。
Host 不扫描 expertIds、输入 mask、重复 top-k 或中转容量，不检查未生成的 sendCounts/metadata 内容。
本阶段 demo 使用各 Rank 一致的 shape、dtype 和调用次数；不新增跨 Rank 参数握手协议。

### 分配、容量和生命周期

令 `capacityRows=R*bs*min(topK,L)`，按主设计已给出的 `tokenNum` 上界预分配；它是 Host/layout 内部计算结果，
不增加 public scalar，也不等于空 kernel 生成了有效行数。`align32(n)` 表示向上取整到 32B。

| Buffer | 第一阶段最小分配量 | 基址对齐 |
| --- | --- | --- |
| `assistExOut` | `capacityRows*32` 字节，行 stride 32B | 32B |
| `sourceMaskOut` | `bs*4` 字节，元素 stride 4B | 4B |
| `groupOut` | `bs*align32(4*(1+2*(topK-1)))` 字节 | 32B |

K=1 时 group 行仍有 firstHopRow 的预留空间，不能将其容量算成零。布局计算使用带溢出检查的 64 位字节运算，
分配前再检查平台 `size_t` 可表示范围；其余输入输出按当前非量化 Memory shape 预分配。
不照搬旧 Dispatch 算法的 UB 拒绝条件，不为尚未实现的通信数据区预先设计布局。

公共入口只有裸指针，无法据此证明实际 allocation 的容量和完整存活期：Host 检查可见的非空、对齐和布局算术；
caller/demo 根据内部 layout helper 返回的所需字节数检查分配记录、设备归属和区间不重叠。
容量不足及区间重叠的负例针对该分配校验 helper，不对裸指针入口虚构可检测能力，也不为此增加 descriptor 或注册接口。
内部 helper 不成为新增公共容量查询 ABI；布局版本或容量查询的后续公共接口仍随正式设计处理。

本阶段同一 comm 使用单 stream 顺序调用，无并发未完成调用；这是测试调度约束，不预先决定后续并发能力。
所有 buffer、comm 和 stream 保持有效至本次 stream 同步成功及轮末 barrier 完成后再复用或释放。
peer window 使用 communicator 已有分配，检查映射和框架实际使用区域的边界；空 kernel 不访问 Dispatch 通信槽位。
profiling 区必须与旧协议已占用区域、旧 assist 和同步状态不重叠，启动前检查真实分配边界。

### ABI 和错误路径证据

`abi-host` 使用独立 consumer 验证公共声明、函数指针类型和 Host 测试链接；fake runtime 记录新增指针和 shape，
检查被拒绝调用没有进入 launch 或消耗 magic。旧 Memory ABI/Host 测试作为回归检查继续执行。
`empty-kernel` 补充已安装头文件与真实 Host 库的编译/链接、导出符号、ELF、相对加载路径和注册失败检查。
Host-only 测试中的替身不进入生产库；缺失 artifact、注册失败、launch 失败均必须返回错误，不能打印 PASS。

## 第一阶段文件范围

### 建议新增

- `src/ep/common/ep_memory_server_dedup_protocol.h`
- `src/ep/host/ep_memory_server_dedup_layout.{h,cpp}`
- `src/ep/host/ep_memory_server_dedup_host.{h,cpp}`
- `src/ep/host/ep_memory_server_dedup_launch.{h,cpp}`
- `src/ep/host/tilexr_ep_memory_server_dedup_dispatch.cpp`
- `src/ep/kernels/tilexr_ep_dispatch_memory_server_dedup_kernel.{h,cpp}`
- `src/ep/common/ep_dispatch_memory_server_dedup_profile.h`
- `tests/ep/unit/test_tilexr_ep_server_dedup_protocol.cpp`
- `tests/ep/unit/test_tilexr_ep_server_dedup_layout.cpp`
- `tests/ep/unit/test_tilexr_ep_server_dedup_host_validation.cpp`
- `tests/ep/unit/test_tilexr_ep_server_dedup_public_abi.cpp`
- `tests/ep/demo/tilexr_ep_memory_server_dedup_demo.cpp`
- `tests/ep/cluster/run_memory_server_dedup_cluster.sh`
- `tests/ep/cluster/run_memory_server_dedup_node.sh`
- `tests/ep/cluster/memory_server_dedup_64p.env`
- `tests/ep/cluster/memory_server_dedup_128p.env`
- `tests/ep/python/test_dispatch_server_dedup_trace.py`
- `tests/ep/python/test_memory_server_dedup_runner.py`：使用可控子进程验证超时、异常和日志失败处理。
- `tools/ep/dispatch_server_dedup_trace.py`：解析本阶段框架记录，不解析未实现的 Dispatch 协议。

### 需要修改

- `src/include/tilexr_ep.h`：增加独立公共入口和三个 metadata 输出指针，不能修改旧入口签名或新增 descriptor。
- `src/ep/CMakeLists.txt`：增加 A5 CCE target、Host source、artifact 安装和 profiling 依赖。
- `src/ep/common/ep_memory_window.h`：只增加 Dedup 专用常量，不改变旧 window 和旧 assist layout。
- `tests/ep/CMakeLists.txt`：注册新增 Host、ABI、layout、Python 和 demo 测试。
- `tests/ep/README.md`：记录参数化运行方式、A5 限制、无输出校验边界和 checkpoint 命令。
- `docs/BUILD_VERIFICATION.md`：增加本文链接和第一阶段验证入口。

### 明确不修改

- 普通 Memory Dispatch/Combine 的公共函数和旧 kernel ABI。
- 旧 `kEpMemoryAssistInfoWidth` 及普通 Memory 的通信记录布局。
- `src/moonep` 的 `DedupPlan`、URMA Dispatch 或 MoonEP E2E 契约；它们只能作为测试思路参考。
- 不增加 `legacy` fallback，不把不支持配置静默降级为普通 Memory。

## 第一阶段上机 checkpoint

第一阶段建议分成 6 次上机验证。每个 checkpoint 都应是可 checkout、可复现、可给出单一结论的 Git 点。

| checkpoint | 覆盖内容 | 成功结论 |
|---|---|---|
| `ep-sdedup-p1-abi-host` | 公共 API、三类 metadata 的 layout、Host 校验、错误码、Host-only 构建 | 接口、ABI、layout 和 Host 测试全部通过 |
| `ep-sdedup-p1-empty-kernel` | A5 CCE 空 kernel、ELF、安装路径、注册和 launch 参数 | 空 kernel 编译、安装和注册测试通过 |
| `ep-sdedup-p1-dry-run` | 读取 hostfile，批量启动多节点/多卡进程，不创建 communicator | 所有 rank 映射正确并在 180 秒内退出 |
| `ep-sdedup-p1-chain-single` | communicator、peer window、一次空 Dispatch launch、stream 同步和 barrier | 所有 A5 rank 的提交、设备完成和轮末 barrier 均成功 |
| `ep-sdedup-p1-chain-multi` | 多轮调用、magic/generation 传参、顺序完成和框架资源复用 | 所有 rank 完成指定循环，复用符合本阶段契约；不证明协议旧 ready 隔离 |
| `ep-sdedup-p1-profile` | warmup、profile marker、性能脚本和跨 rank 汇总 | 所有 rank 产生完整框架记录并在 180 秒内汇总 |

每个 checkpoint 的脚本应支持配置：`hostfile`、`rank_size`、`local_npu_count`、`bs`、`h`、`topk`、`moe_expert_num`、
`dtype`、`quant_mode`、`block_dim`、`comm_id`、`barrier_addr`、`warmup`、`loops`、`timeout` 和 `run_id`。
hostfile 不得硬编码到脚本；`rank_size` 可以由主机数乘每主机卡数推导，也可以由配置显式指定并校验一致性。
本阶段 `local_npu_count=8`，连续 rank 映射为 `hostIndex*8+localRank`；hostfile 行序固定 Server 编号，
逐 Rank 记录物理 device ID，拒绝同一 host/device 重复映射。hostfile 由运行环境提供，不提交新的部署主机清单。
两个 env 文件仅保存通用 shape/运行参数；远端仓库、安装目录、CANN 环境脚本、设备集合和日志目录均从运行配置传入。

### 必测规模与完成条件

| checkpoint | 必需执行范围 |
| --- | --- |
| `abi-host` | Host 测试覆盖 R=16/64/128、L=2/3/6/32、K=1/6/32 的代表性组合，以及非法 shape、空指针、对齐、布局溢出和 helper 容量负例；不要求全部笛卡尔积 |
| `empty-kernel` | 目标 CANN 9.1/A5 artifact；每个实际使用的不同构建/安装环境验证生产 Host 链接与设备侧注册，不创建多 Rank communicator |
| `dry-run` | 64P 和 128P 都检查全量 rank/device 映射及无 communicator 的进程退出 |
| `chain-single` | 64P 和 128P 均全 Rank 完成一次空 launch；single 表示一轮，不表示单卡 |
| `chain-multi` | 64P 和 128P 均全 Rank 完成至少 `PING_PONG_SIZE+1` 轮，复用同一批分配；默认 loops=5，运行前校验轮数满足下限 |
| `profile` | 64P 和 128P 均全 Rank 执行 warmup=2、loops=5；FP16/BF16 各一次，记录实际参数 |

默认形状采用 `bs=256/h=7168/topK=6/moe_expert_num=384`，非量化；其他 checkpoint 的 dtype 默认 BF16。
较小规模可用于排查，但不能替代上述规模的验收。每条命令只判定本次配置，180 秒预算也按单次命令计算；
checkpoint 完成判定汇总同一 commit 的全部必测配置记录。资源不足或规模未运行时，checkpoint 保持未完成，
不得用某一次配置的 PASS 代替整个 checkpoint 通过。

单轮顺序为：初始化并取得 peer window，执行测试进程间的 Host barrier，调用空入口，检查 Host 返回值，
执行 stream 同步并检查异步错误，最后通过 Host barrier 确认所有 Rank 完成本轮。
barrier 复用现有测试的控制面机制，`barrier_addr` 是其 rendezvous 地址，不是新增设备 metadata-ready 协议。
Host barrier 不证明 peer-memory 数据传输；若建链或现有 barrier 实际触达设备通信路径，先按 AGENTS.md 核对匹配环境资格记录。

每次实际 launch 内部调用一次 `TileXRCommNextMagic`，不要为了测试在外部再取一次 magic。
Host 测试捕获传入 kernel 的值并核对顺序，多轮 demo 记录实际提交次数和完成次数；barrier 的轮次编号与 kernel magic 分开。
未启用 profiling 时不声称已从设备读回 generation；profile checkpoint 再用设备记录核对本次传入值。
空 kernel 不产生 count、metadata 或 ready，测试不得读取其未写入值作为下一轮输入。
可预填输出并检查未被修改，作为“不写输出”的资源检查，但不得称为算子数值 correctness 或旧协议状态隔离验证。

建议命令形式如下：

```bash
bash tests/ep/cluster/run_memory_server_dedup_cluster.sh \
  --hostfile tests/ep/cluster/hosts_terbo_64p.txt \
  --config tests/ep/cluster/memory_server_dedup_64p.env \
  --checkpoint ep-sdedup-p1-dry-run
```

### Marker 和汇总

每个 marker 所在记录必须携带 `run_id/checkpoint/commit/rank/status`；Host 检查使用独立 host 标识，控制端总结果使用 rank=all。
同一 checkpoint 在单次运行中只要求自己的 marker，不能在 dry-run 要求尚未执行的 chain/perf marker。

| checkpoint | 每个执行单元必须提供的 marker |
| --- | --- |
| `abi-host` | `EP_SERVER_DEDUP_ABI_HOST_PASS` |
| `empty-kernel` | `EP_SERVER_DEDUP_EMPTY_KERNEL_PASS` |
| `dry-run` | `EP_SERVER_DEDUP_DRYRUN_PASS` |
| `chain-single` | `EP_SERVER_DEDUP_CHAIN_PASS` 和 `EP_SERVER_DEDUP_SINGLE_ROUND_PASS` |
| `chain-multi` | `EP_SERVER_DEDUP_CHAIN_PASS` 和 `EP_SERVER_DEDUP_MULTI_ROUND_PASS`，包含实际完成轮数 |
| `profile` | `EP_SERVER_DEDUP_CHAIN_PASS` 和 `EP_SERVER_DEDUP_PERF_PASS`，包含 warmup/测量轮数 |

控制端按实际 checkpoint 名称匹配上表后缀，对每一种 marker 分别检查预期 Rank 或 Host 检查单元完整且唯一；
以 `(run_id,marker,rank/host)` 去重，不把同一 Rank 的 CHAIN 和轮次 marker 当成重复。拒绝旧 run_id、错误 commit、
缺失日志、非零退出以及异步错误。`CHAIN_PASS` 表示 communicator 初始化和 peer 映射检查通过，不表示传输正确。
仅在本次配置的全部检查和清理通过后输出 `EP_SERVER_DEDUP_VALIDATION_PASS` 及显著的 `成功`；否则输出 `EP_SERVER_DEDUP_VALIDATION_FAIL`、
显著的 `失败` 和非零退出码。详细 rank 日志与唯一总结果分开保存。

控制端必须在任一 rank 失败、缺少 marker、进程超时或日志无法汇总时返回非零状态。
Windows 控制端通过 `tools/tssh/tssh.cmd` 执行复杂远端 Bash 时，使用 stdin 传递脚本，不拼接复杂的单行 SSH 命令。

### 超时、故障注入和证据保留

1. 控制端从本次验证开始计算 180 秒总期限；SSH 建连、barrier、日志传输均有不超过剩余预算的期限。
   最晚第 120 秒停止执行阶段并开始收敛，不让后启动的 Rank 重新获得完整 120 秒。
2. 节点 runner 为本次 run_id 建立独立进程组和本地 watchdog，覆盖 demo、profiler 及其子进程；控制端失联或 SSH 退出不能取消 watchdog。
   任一 Rank 失败时通知全部节点中止本次运行，先 TERM，最多等待 5 秒后 KILL，并 wait/reap 子进程。
3. 只清理已记录且核对属于本 run_id 的进程组、pid/lock 和临时目录；不按进程名称批量杀其他任务。
   控制端确认各节点清理结果；节点不可达或无法证明清理完成时结果为失败，保留清理未确认状态。
4. 每个节点持续写入本次运行专属日志，失败时先保留原日志、退出码、首个观察到的失败 Rank/阶段及清理状态，
   在总期限内汇总。不能可靠确定首个失败 Rank 时记录 unknown，不凭日志时间戳猜测跨节点先后。
5. 临时日志只有在持久归档确认后才删除；汇总失败时保留远端日志路径。180 秒到期仍未完成日志收集或清理确认，一律失败。
   日志只保留本次有界运行的证据；成功记录也保存配置和逐 Rank 结果，不依赖终端滚屏。

runner 测试用可控子进程覆盖正常退出、单 Rank 非零、缺 marker、旧 run_id、重复 Rank、卡住、TERM 不退出、
控制连接断开和日志收集失败；验证非零结果、watchdog 生效和无测试进程残留，不需要 NPU。
dry-run 在目标节点补做一次进程卡住/控制端断连的清理演练，仅对本次测试进程组操作。

### Profiling 记录和性能边界

普通构建不写 profiling 区；profiling 构建只记录空入口开始/结束及身份，写入完成后才结束 kernel。
区地址通过内部 launch 参数传递或从已核对的 comm 区域取得，不新增公共 metadata 参数，不改变旧区域布局。
每轮在 stream 完成后提取记录再复用缓冲，warmup 同样同步但从测量统计中排除。

设备记录至少包含 marker、内部格式版本、recordBytes、rank、core、generation、开始/结束计数值；
Host 将其绑定到 run_id、commit、kernel artifact 和迭代号。核对所有预期 AIV 的记录、版本、身份及实际提交的 generation，
拒绝残留记录或缺失核心；计数器单位及换算依据须来自目标工具链，不能直接套用未经核对的 cycles/us 常量。
普通与 profiling 构建均通过各自空 launch 检查；profiling 的额外写出开销在报告中单列说明。

报告包含 backend=memory、native 空 kernel 状态、kernel/artifact 版本、shape/dtype、block_dim、warmup/loops、
profiling 开关、Host barrier 状态和每 Rank 完成数。分别报告 Host 提交、设备空入口、barrier 与端到端框架耗时，
不能用任意两项相减推导另一项。每 Rank 对测量轮次取 median，再汇总所有 Rank 的 median/min/max，使用 ns/us/ms 等可读单位。
空 Dispatch 的算法通信量记为 0B，带宽记为 N/A；profiling 写入和控制面流量不算去重收益。
trace parser 使用记录样本做正反例测试，不通过扫描实现源码的文本来证明功能。

## 第一阶段提交和验证顺序

推荐按以下顺序生成和提交代码：

1. 提交 ABI、protocol 和 Host-only 测试，验证 `ep-sdedup-p1-abi-host`。
2. 提交空 AICore kernel、CMake artifact 和 launch，验证 `ep-sdedup-p1-empty-kernel`。
3. 提交 hostfile dry-run 编排，验证 `ep-sdedup-p1-dry-run`。
4. 提交 communicator 建链和单轮空算子，验证 `ep-sdedup-p1-chain-single`。
5. 提交多轮 magic/generation 和清理逻辑，验证 `ep-sdedup-p1-chain-multi`。
6. 提交 profiling、warmup、性能汇总和文档，验证 `ep-sdedup-p1-profile`。

每次提交前执行对应的本地检查；每次提交后记录 commit、服务器 checkout 命令、配置文件和唯一测试结论。
第一阶段所有 checkpoint 成功后，才能进入 Dispatch 算子逻辑实现。

每次提交前以本会话开始时的工作区为基线，逐文件或逐片段暂存并检查 `git diff --cached`；不使用全量 add 把已有改动带入。
已有未跟踪文件也属于会话前基线，不能因它未跟踪就整文件提交为本会话新增内容。
对这类文档的补充保留在工作区，待文件归属明确后再纳入提交；测试、必要文档和代码遵循相同归属规则。
每个实现增量产生的可复用经验更新到模块 README 或本文，并与产生该经验的本会话改动一起提交。

checkpoint 是验证阶段名称，具体尝试用完整 commit SHA 标识；失败返工增加修复 commit，并记录新旧 SHA，不移动已有成功记录。
服务器运行前核对 checkout SHA、干净源码、配置摘要，以及实际加载的 Host 库/kernel 文件路径和 SHA256；
记录 CANN、driver、设备/拓扑、构建选项和安装目录。不能用旧 install 产物给新 commit 出具通过结论。
成功记录必须覆盖该 checkpoint 的全部必测配置；目前六个 checkpoint 均未在本文登记硬件通过记录。

维护经验：验证流程可能滞后于并行演进的设计。每个 checkpoint 开工前重新核对公共接口清单，按“已确认/候选/本阶段工程约定”区分内容；
不把历史 forwardPlan、descriptor 或计数名词重新扩成接口，不把空调用通过扩大为真实协议验证。

## 第一阶段验证边界

必须验证：

- C++14/CANN 9.1 兼容的公共头文件和 Host 编译。
- A5 空 kernel 的 CCE 生成、安装、注册和 `rtKernelLaunchWithFlagV2` 调用。
- API 参数、metadata 布局计算及 caller/helper 分配容量检查、对齐、rank、dtype、A5 能力和错误码。
- hostfile 到 rank/device 的唯一映射和多进程退出处理。
- communicator、peer window 映射、magic/generation 传参、stream 完成、单轮和多轮框架资源复用。
- 180 秒超时、子进程清理、日志汇总和固定 PASS marker。

不验证：

- `expandXOut`、`dynamicScalesOut`、`sendCountsOut`、`expertTokenNumsOut`、`assistExOut/sourceMaskOut/groupOut` 的算子数值；不生成旧三字段 assist。
- Server grouping、第一跳、relay forwarding、真实 metadata 写入和通信字节收益。
- Combine、partial、source reduce 或最终 `yOut`。
- X 超限处理和尚未确认的物理 metadata layout。
- 真实通信旧 ready 隔离、后续完整 workspace 预算、Combine 生命周期或未确认的并发/量化支持。

本文只记录流程规划。实际通过情况必须以对应 Git checkpoint 的服务器日志为准，并在后续实现提交中更新验证记录。
