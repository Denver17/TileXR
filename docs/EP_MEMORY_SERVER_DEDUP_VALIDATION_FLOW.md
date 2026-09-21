# EP Memory Server Dedup 验证流程

## 文档状态

本文记录 EP Memory Server Dedup 的分阶段代码生成、Git checkpoint 和上机验证流程。
第一阶段保留公共框架和空 kernel 的历史验收基线；当前细化第二阶段的数据发送、cnt 发送/接收和 cumsum 计算。
实际通过范围和 commit 只登记在[调试进度](EP_MEMORY_SERVER_DEDUP_PROGRESS.md)，计划更新不表示新增功能已实现或测试通过。
接口和逻辑以[主设计](specs/ep-memory-server-dedup.md)中的最新已确认结论为准；待确认文档中的推荐或候选不自动成为实现要求。
Dispatch 阶段历史规划保留如下；当前新增 Combine 第一阶段的七点交付闭环，以本页 Combine 小节为准。

2026-09-09 接口复核：主设计已明确只新增 `assistEx/sourceMask/group` 三类 metadata，不新增 descriptor、group 注册、X 或独立 `tokenNum` 参数。
旧 `forwardPlan` 仅为历史名称；旧三字段 assist 可保留接口占位，但去重逻辑不额外复制同义表。
本文据此撤销原有版本化 metadata descriptor 和 opaque forwardPlan buffer 规划。

2026-09-11 按当前主设计复核：R02-01 两遍分类、R05-04 源端流水、R07-01 单份布局及 R10-01 权重/量化 ABI 已收口。
`expertScales` 改为必填只读输入，新增独立 `expertScalesOut`；非量化和 MXFP8、固定 48/16 核组按下文阶段二契约验收。
第一阶段的旧签名、空参数约定、动态核数和空 kernel 通过记录不能替代上述变更的实现与验证。

专家数约束按最新主设计同步：总专家数不超过 1024 且能被 R 整除，不设独立单卡上限，允许 L=1。
当前 cnt/cumsum 增量已适配 Host/layout/调度器限制；可执行步骤与数值判据见[当前提交验收](EP_VALIDATION_GUIDE.md#当前提交cntcumsum-验收)。
Dispatch 当前每 rank 使用 8 GiB 通信域（8188 MiB 数据区 + 4 MiB runtime Flag）；以 `magic % 2` 选择两个固定 4 GiB 窗口，后续 Combine 共用该域但尚未实现。历史 Dispatch 布局曾在每窗前留 2 MiB；当前联合布局为 764KiB Dispatch 控制区、52KiB Combine 控制区及 208KiB 间隙，共享数据从 1MiB 开始。Token 区不得越过 4092 MiB 相对偏移；第一窗尾部留空以保持两窗容量一致。
开发阶段 1～4 统一先用 16P、每卡 6 专家（总计 96）、BS=96、TopK=6、balance_shift 主场景验证流程及性能；边界条件检查留到完整流程的最终验收，不在各增量默认扫描。
下文保留阶段规划，空 kernel 辅助打点及完整数据路径的布局/ABI 不代表当前计数实现；既有通过记录不作为新范围已经验证的证据。

适用平台为 Ascend 910A5/Ascend950 A5 环境，目标工具链为 CANN 9.1.0，保持 C++14。
910B Host 或 simulator 只能验证 Host/构建边界，不能作为 A5 kernel 执行证据。

<a id="combine-phase1-confirmed"></a>
## Combine 第一阶段：已确认的窗口检查与 breakdown 补充

本节记录 Combine 空算子第一阶段的最新确认，供七个验收点落实；后文 Dispatch 阶段记录
保留历史上下文，不能用旧固定 4GiB 步长、2MiB 数据起点或旧接口替代最新联合设计。
当前公共布局以[联合设计 §6](specs/ep-memory-server-dedup-dispatch-combine-design.md#6-共用通信域的布局cb00-02-已确认)为准。

### 七点交付与外部验收门槛（2026-09-20）

每次只开发当前一个检查点：开发/对应检查 → commit 并推送 `codex/SilentEmotion` →
用户在验证环境拉取该 commit，执行 `bash run_commit_validation.sh` → 回传结果 →
记录 SHA、环境、命令、结果及验证边界 → 默认通过后才开始下一点。
2026-09-21 用户授权例外：验证服务器暂不可用，C5 标记待验证，允许提前完成 C6/C7，
但两点必须分批提交；用户恢复环境后按对应 commit 逐点执行同一入口，未回传不得标记设备通过。
开发环境通过、提交成功或脚本返回 0 均不能替代用户回传的检查点验收；失败只修当前点。
回传至少包含终端结论和 `summary.json`；失败同时提供 summary 指向的构建/测试日志。
不需要为每个检查点建立新的测试脚本，入口随当前交付点更新；不会自动推进下一点。

所有检查点名称前缀为 `ep-sdedup-combine-p1-`：

| 点 | 后缀 | 交付范围与验收边界 |
| --- | --- | --- |
| C1 | `abi-host` | 27 参数 ABI、独立 Host 参数/配置校验、五表/tensor 容量及固定控制区/双窗边界；错误不提交、不消耗 magic。用户回传通过，交付 `01c9b1d` |
| C2 | `empty-kernel` | 独立 A5 artifact、安装/注册、独立 16B GM tiling/88B launch ABI、固定 64 核空入口；不读写业务数据。用户回传通过，交付 `3b27062` |
| C3 | `dry-run` | 同一脚本/consumer 接入 dispatch、combine、both；dtype/量化格式共用，quant 开关独立；仅验证配置/rank 映射，不分配设备通信域。用户回传通过，交付 `b68b56e` |
| C4 | `chain-single` | 单轮 Combine 独立及真实 Dispatch→空 Combine；五表/count 原指针连接、量化 Dispatch 的独立 BF16/FP16 expertOut、输入输出生命周期；不验 yOut 数值 |
| C5 | `chain-multi` | warmup/多轮、各自 magic/选窗、同步与生命周期；联合模式 D/C 固定使用不同窗口符合预期。用户回传 8/8 通过，交付 `4ef0439` |
| C6 | `profile` | functional/performance/breakdown 共用 consumer；分离记录 base/counter/latest，两算子独立 trace；空算子算法字节 0、算法带宽 N/A。用户回传 6/6 通过，交付 `abee43c` |
| C7 | `memory-boundary` | 两半区分别执行合法区边界与保护区探针，避开 runtime；保留真实 Dispatch 回归，不冒充 Combine 数据面验证 |

C1 历史入口一次完成构建和三项 Host 测试，不要求 CANN/hostfile/SSH/NPU；
使用独立 `build/validation-combine-c1` / `install/validation-combine-c1`，不覆盖 Dispatch 安装包。
准备期限 900 秒、运行总期限 180 秒、单项最多 120 秒；成功结论为
`成功 COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p1-abi-host`。
执行需要 Python 3.9+、CMake、Make、g++。现有 Dispatch 的 `build_all.sh` / `sync.sh` / `run.sh`
仍按原用途使用，C3 后才扩充三种算子模式；C1 不提前改变这些调用参数。

**C1 历史范围约束：** 该提交的生产入口完成 Host 链路后返回 `NOT_SUPPORT`；C2 改为真实提交空 kernel。C1 当时
用 launch 替身验证传参/错误传播，同时 ABI 测试链接真正的 no-launch 实现；当前 ABI 测试使用替身，真实提交由 C2 测试负责。
Host 不读设备 sendCounts 或五表，裸指针无法证明 allocation 容量、设备归属或业务有效行，
这些仍由 caller 记录及后续 consumer 负责。与 Dispatch 保持一致，EP rank/world/globalBs 的 0 是推导哨兵。

固定控制区及 1MiB 数据起点已经确定；空 kernel 无 result/stage 访问，C1 的 Host 容量门槛为
每半区至少 `1MiB + runtime 保留区`，并检查整个 peer 地址跨度及 512B 对齐。
结果 wire 的非量化 dtype/尾块仍属 CB03-02/CB04-03，因此独立地址助手接收显式 `resultSlotBytes`
验证 `BS*K*Sresult` 和 `E*BS*alignUp(H*2,512)`，不默认采用示例 `ceilDiv(2H,480)*512`。
第二阶段启用 result/stage 前必须据最终 wire 格式把完整 `Cdata` 纳入生产 Host 容量校验；
C1 通过不证明真实通信数据区容量充足。

维护经验：提交验收只能消费本次重新构建的当前 checkpoint 安装包；构建开始先使旧 manifest 失效，
缺工具/构建失败不得继续运行旧测试。每项必须同时满足退出码 0 和唯一匹配 PASS marker，
summary 记录 commit、源码 provenance/工作区状态，避免把文件副本或未提交修改误认成该 SHA 的原样产物。

### C2 历史交付：空 kernel 的安装与提交

C1/C2 均已获用户 `COMMIT_VALIDATION_PASS` 确认。C2 交付时入口默认 `ep-sdedup-combine-p1-empty-kernel`。
`bash run_commit_validation.sh` 加载现有 `validation.local.sh` 的 CANN/driver 设置，独立构建
`build/validation-combine-c2` 并安装到 `install/validation-combine-c2`，检查新 kernel ELF、公共导出和真实 driver 依赖。
同一命令执行 C1 三项 Host 回归、生产 launch 替身和安装包真实注册/提交，共五项；缺少任一项不得成功。
本点需要 CANN 9.1.0 和本机 A5（至少 64 AIV），不需要 hostfile/SSH/多卡 rendezvous。
`TILEXR_EP_SERVER_DEDUP_TEST_DEVICE` 指定本机逻辑设备，默认 0；以实际 report 中的设备为准。
准备/运行/单项上限继续是 900/180/120 秒。

| 契约 | C2 判据 |
| --- | --- |
| artifact | 独立 `tilexr_ep_combine_memory_server_dedup_kernel.o`，位于安装目录 `lib/tilexr/kernels`；Host 相对所属库加载，不嵌入 binary |
| GM tiling | BS/H 为 uint32、E 为 uint16、K/dtype/quantMode 为 uint8，3 个保留字节置零，总计 16B；comm 缓存拥有不可变副本 |
| launch | CommArgs、expertOut、assistInfo、sendCounts、expertScales、tokenType、relayReadIndex、sourceMask、yOut、tilingData 十个地址，加独立 int64 magic，总计 88B；固定 64 核 |
| 失败路径 | SoC/core 查询、binary/function 注册、GM tiling 获取失败均不提交；tiling 获取成功后才取 magic，runtime launch 失败会报告错误但已消耗该 magic |
| 空行为 | 每核只读取六个 tiling 标量，不读写五表、计数、expertOut/yOut、CommArgs 或 peer-memory；不申请/覆盖 Dispatch profiling 记录 |
| Host 证据 | 注册失败回滚、字节布局/保留零值、原指针、固定核数、错误传播、同 shape 缓存及异步 tiling 不被后续调用覆盖 |
| A5 证据 | consumer 链接已安装生产库，真实注册后提交 FP16/BF16 两次，stream 同步成功，输入/输出/CommArgs/tiling 哨兵保持原样 |

C2 设备 consumer 使用内部 Register/Submit 入口及独立 GM 哨兵；不创建真实多卡 communicator，
因此不证明公开 API 的多卡生命周期、选窗或数值行为（属于 C4/C5）。magic=1/2 是此隔离测试的输入，
生产路径仍每次通过 TileXRCommNextMagic 获取。C2 未访问通信数据面，不运行 HCCL Test。
空 kernel 的算法带宽无意义，不做性能结论；profiling 分区和两份 trace 仍归 C6。
任何 A5 不可用、注册/提交/同步失败或缺少唯一 marker 都使本点失败，不以 Host 回归替代。

维护经验：安装包真实测试应加载本次安装目录的 Host 库和相对路径 kernel，不能用源码编译的
Host 替身冒充生产注册成功。GM tiling 必须在异步任务结束前保持不可变；不同算子/shape 使用
不同缓存键，不能仅复用一个“最新 tiling”槽。失败或超时保留本次 commit、产物哈希和首个失败阶段。

### C3 历史交付：共享 consumer 的三模式 dry-run

C2 已按用户回传登记，报告路径及证据边界见[进度](EP_MEMORY_SERVER_DEDUP_PROGRESS.md)。
C3 交付时 `bash run_commit_validation.sh` 默认 `ep-sdedup-combine-p1-dry-run`，
独立构建 `build/validation-combine-c3` 并安装到 `install/validation-combine-c3`，不覆盖 C2/Dispatch 安装包。
需要 Python 3.9+、CMake、Make、g++；无需 CANN、NPU、hostfile 或 SSH。
准备/总运行/单项期限为 900/180/120 秒；必须通过 C1 三项和配置 integration driver 共四项。

实现顺序与判据：

1. 同一 `run.sh` / workflow / C++ consumer 接入 `operator_mode=dispatch|combine|both`，默认 dispatch。
2. `dispatch_quant_mode=0|4` 和 `combine_quant_mode=0` 独立；旧 `quant_mode` 仅为 Dispatch 别名，
   与显式 Dispatch 值冲突时报错。`dtype=bf16|fp16`、`fp8_format=e4m3|e5m2` 共用；非量化时格式不生效。
3. C++ `--dry-run` 使用生产 Host layout 做参数验证，检查固定 8P/server 映射，
   在 ACL、profiling、socket、comm 和设备分配之前返回；combine 单跑不继承 Dispatch 的路由 epoch/Count UB 限制。
4. 以同一 C++ 源码构建可观测 runtime 替身 consumer，测试三种算子模式、两个 dtype、两种 D quant/format、
   三种运行模式、16/64/128 rank 映射、非法配置及旧别名；要求替身调用记录为空，且缺任何 marker 都失败。
5. `run.sh ... dry_run=1` 经现有 hostfile/rank mapping 在本机逐 rank 执行已安装 C3 consumer；
   不连接 hostfile 中的主机。真实 combine/both 执行在 C4 前明确拒绝，不静默执行 Dispatch。

C3 的 JSON 只记录计划的输入来源：both+D0 为 `dispatch_expand_x`，both+D4 为
`separate_expert_out`（共享 dtype 的 BF16/FP16）；五表来源标记为 `dispatch_outputs`。
这不证明真实 tensor 原指针相连，实际分配/连接/生命周期归 C4；不检查通信窗口、yOut 或带宽。
breakdown 此处只验证配置，双记录区及双 trace 文件仍归 C6。

维护经验：增加联合模式时，必须在同一个 consumer 的所有设备副作用之前设置 dry-run 边界，
并用可观测 runtime 调用记录证明未进入设备路径；仅输出“未分配”字段不能替代此检查。
停用算子的配置不应触发其业务路由约束，但保留的量化枚举仍须合法，避免把未支持的 Combine 量化带入后续执行。

### C4 历史交付：单轮接线和输入生命周期

C3/C4 已按用户回传登记通过。生产接口仍是既有 27 参数 API、16B tiling 和 88B launch ABI。
复用 `run.sh`、workflow、同一 C++ consumer；C4 要求 functional、loops=1、warmup=0，
不把多轮、performance 或 breakdown 提前纳入本点。

实现及验收顺序：

1. 构建独立 C4 生产安装包；运行 C1 三项、C3 dry-run、C4 Host 生命周期与 kernel 等待行为测试。
2. Combine 单跑自行分配合法参考五表及完整 inclusive prefix，使用支持不完整 epoch 的独立 balance fixture，
   不运行 Dispatch route_mode 对应的生成器；专家输出固定为所选 dtype 的有限值，
   不调用 Dispatch；所有 tensor 按生产 Combine layout 容量分配并保留到同步完成。
3. 联合模式直接传 Dispatch 输出的 DestinationIndex、TokenType、RelayReadIndex、sourceMask、
   expertScalesOut 以及完整 sendCountsOut 原指针；D0 的 expertOut 直接取 expandXOut，
   D4 则新分配 BF16/FP16 expertOut，不传 FP8 数据或 dynamicScales 给非量化 Combine。
4. C4 在 Dispatch 同步/barrier 和完整输出校验后，保存 Combine 七个输入及 yOut 的全容量快照；
   空 Combine 完成同步/barrier 后逐字节比较。此同步是 C4 可观测生命周期检查，
   不作为后续连续多轮或性能路径的调度方案。Dispatch 不通过时不能把空 Combine 成功当作联合通过。
5. 每个进程只创建一个 comm/stream，记录各算子提交/完成以及各自 magic/window；
   新 comm 单 C 为 magic=1，D→C 为 1/2，不额外取 magic 观察。未确认完成时不提前释放输入或销毁 comm。
6. 一键入口沿已有 hostfile/sync/run 路径执行 16P 六组：独立 Combine BF16/FP16，
   both+D0 BF16/FP16，both+D4 BF16/E4M3 与 FP16/E5M2。缺任一 rank、唯一 marker、输入不变或真实 Dispatch 判据均失败。

用户追加的 50μs 等待在空 Combine 每个 AIV 内统一执行，单跑也生效，不增加 ABI 或测试模式参数。
`uint64_t(GetSystemCycle()) - start >= 50000` 结束等待；A5/Ascend950 SYS_CNT 为 1GHz，
不能套用 A2/A3 的 50MHz。核对依据为安装 CANN 9.1.0 的
`asc/impl/basic_api/dav_3510/kernel_operator_sys_var_impl.h`（设备读取 SYS_CNT，CPU 仿真实现为纳秒），
以及官方 [GetSystemCycle 文档](https://gitcode.com/cann/asc-devkit/blob/master/docs/zh/api/SIMD-API/%E5%9F%BA%E7%A1%80API/%E5%B7%A5%E5%85%B7%E6%8E%A5%E5%8F%A3/%E7%B3%BB%E7%BB%9F%E8%B5%84%E6%BA%90%E4%B8%8E%E5%8F%98%E9%87%8F/GetSystemCycle%28ISASI%29.md)；
在线版本为当前 master，9.1.0 过滤检索无结果，实际兼容性以已安装头文件和 A5 编译为证。
Host 受控时钟测试覆盖 49999/50000 阈值和溢出；不能把它当作实测 50μs。
等待不替代跨 rank 同步，也不证明任意进度偏差下的多轮复用安全，后续 C5 仍需单独验收。

维护经验：空算子不产生可比数值，必须验证真实调用、原指针关联、完整输入及输出保持不变，
并注入首/末字节破坏及提交/同步失败确认判据有效。量化 Dispatch 的输入分离发生在调用方，
不能为接线方便改变 Combine 的固定 BF16/FP16 契约。
单算子 fixture 也必须隔离停用算子的约束：BS=1/E32/K3 的 Combine 单跑不能因 Dispatch balance_shift epoch 不完整而失败。
每组用例采用独立结果目录时，远端取消操作必须收到相同 RESULT_ROOT，才能定位当前 PID 并保留清理失败状态。

### C5 当前交付：连续批次、各算子 magic 和生命周期

C5 已收到用户 `COMMIT_VALIDATION_PASS` 和 8/8 通过回传；总报告及边界见[进度](EP_MEMORY_SERVER_DEDUP_PROGRESS.md)。
C6/C7 已提前分批交付；C6 已获用户通过回传，C7 仍待设备验收。下文保留 C5 交付契约。

C4 已获用户 `COMMIT_VALIDATION_PASS` 回传，交付 commit `c86e1ef`，报告见进度文档。
本次只实现 `ep-sdedup-combine-p1-chain-multi`，保留 C4 单轮快照回归；C6/C7 不提前实现。

1. 同一 consumer 保留三种算子模式；C5 只接受 functional。同一 comm/stream/tensor
   连续提交 warmup 和正式轮次，both 每轮为 D→C；仅批末一次 stream 同步和 Host barrier，
   不在轮间清零、读回、重新分配或额外取 magic。空 Combine 的 50μs 等待保持。
2. 每个成功提交按公开入口消费一次 magic 的契约累计轮次，分别记录 D/C 的正式及预热提交/完成数、
   first/last magic、实际计算的 window mask 和最后窗口 offset；此为调用方推导，非设备侧 magic 采样。
   单算子 magic=1..N；both D=1,3..2N-1、C=2,4..2N，D/C 各固定一窗符合预期。
3. C-only 在批前保存全部七输入和 yOut，批后全容量比较；both 由 Dispatch 合法重写共享输入，
   批前只保存独立 expertOut（D4）及 yOut，批后验证它们保持，并按最后 D magic 检查真实
   Dispatch 全部输出/五表/count/通信 Flag。明确标注 `combine_input_check=dispatch-final-reference`，
   不宣称每轮共享输入均有全容量不变快照；该空行为由 C4 独立回归补充。
4. 同步和 barrier 成功后才读回及释放；中途提交/同步失败不得提前 free/destroy。
   统计正式轮次与 warmup 分开，缺任何 rank/唯一 marker/最终 Dispatch 正确性均失败。
5. 一键入口构建独立 C5 安装包，先执行 C1/C3/C4/C5 Host 回归与 50μs 行为检查，再按前两台
   固定 8P 执行 8 个串行设备用例：D-only BF16/FP16 D0；C-only BF16/FP16；
   both BF16/FP16 D0；both BF16 D4/E4M3 和 FP16 D4/E5M2。每例 loops=5、warmup=2。
   控制端显式显示 `[i/8]`、算子/dtype/量化/轮次和结果路径；首个失败立即停止并保留已完成结果。

实现分工：共享 consumer/快照与本文由主任务负责；workflow、run.sh、run_commit_validation.sh
及 workflow 行为测试可独立实现；Host runtime 替身、C5 集成 driver 和 CMake 接线独立实现。
契约由同一 checkpoint、marker 和上述计数语义连接。验证先检查配置和故障注入，再执行共享
consumer Host 回归、既有 Dispatch 回归、CANN 9.1 A5 生产库与真实 consumer 编译；
开发侧编译/替身通过不作为 C5 A5 多轮通过；本次已按用户外部回传单独登记 C5 验收通过。

维护经验：连续连跑中的 Dispatch magic 必须计入穿插的 Combine 调用，不能直接使用循环数；
以偶数总轮次的 both 替身回归检查最后 D 窗口，避免奇数轮次碰巧选对掩盖错误。
将 C4 的逐轮快照读回移入连续批次会引入额外同步并改变调度，必须把不变 tensor 快照放在批前/批后，
共享输出采用最终参考检查，并在报告中写明该边界。固定时长等待不等价于跨 rank barrier，
Host 替身通过不证明设备进度偏差下的复用安全，需用户执行 C5 矩阵验收。

### consumer 的职责

现有 consumer 为 `tests/ep/integration/test_tilexr_ep_server_dedup_chain.cpp` 编译出的
`test_tilexr_ep_server_dedup_chain`，由 `run.sh` 经 `tools/validation/workflow.py` 在每个 rank 启动。
它负责设置设备、创建 comm/stream、分配并构造输入输出、调用公开算子、同步和 barrier、
读取结果与独立参考比较、输出逐 rank marker/统计及清理资源。
`server_dedup_dispatch_consumer.h` 是其 Dispatch 输出及通信槽校验部分。
它是测试侧调用方，不是生产通信中的接收核；后续在同一 consumer 中接入三种算子模式。

### 选窗与检查时机

联合模式 D→C 各 launch 一次并各递增 magic，D/C 各自持续使用一个不同窗口是已确认预期，
不是设计漏洞；C5 不要求两算子在联合模式中各自轮换两窗。以新 comm、首次 magic=1 为例，
D 为 1/3/5…、C 为 2/4/6…；实际 bufferIndex 始终按各自 magic%2 取得，不由自然语言窗口
编号推断。不要为检查取窗额外调用 TileXRCommNextMagic，也不要把联合循环数作为 D 的 magic。

| 场景 / 验收点 | 需要检查的对象和时机 |
| --- | --- |
| C1 Host/layout | 提交前检查两个半区的地址、容量、对齐和 runtime 保留边界；属于地址计算检查，不扫描设备窗口内容 |
| C4/C5 中的真实 Dispatch | 完成同步及测试 barrier 后，按最后一次 Dispatch magic 读取其已消费槽，验证 DataAsFlag 已清零；输出不符时可读对应槽辅助诊断 |
| C4/C5 中的空 Combine | 检查正确传参、选窗、完成及输入/yOut 不被修改；没有结果包或 ready 协议可校验，不要求窗口产生非零数据 |
| C7 独立边界探针 | 分别检查两个 buffer 内合法区域边界及邻接保护区，探针结束后读回；不侵入 runtime 保留区，不替代算法通信验证 |
| 后续真实 Combine | 在对应功能点检查自身控制/结果/stage 区的协议不变量，不复用 Dispatch 的包格式解析器 |

五表、计数、expertOut/yOut 是 caller 独立分配的 tensor，正常输出校验直接读这些 tensor；
通信窗口不是这些输出的存储位置。当前 Dispatch 校验按实际使用槽读回，不要求扫描整个
8GiB 通信域；读取在设备批次完成后进行，不进入 kernel 计时区间或逐轮热路径。
联合模式按两算子的角色分别检查各自窗口，不把 Combine 窗口交给 Dispatch 校验器。

### C6：两算子记录区及 trace 隔离（用户回传验收通过）

C6 已收到用户 `COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p1-profile` 及 6/6 通过回传，
总报告及验收边界见[进度](EP_MEMORY_SERVER_DEDUP_PROGRESS.md)。C7 仍需独立验收。

breakdown 扩充标准 profiling 记录区，以不同 base 分隔 D/C 的记录、有效计数、丢弃计数和
latest-launch 状态；记录区不占通信窗口。Dispatch 单测只解析 D 区，Combine 单测只解析 C 区，
联合模式分别解析并分别保存两份算子 trace；未启用区域不解析、不要求有效数据。
沿用现有跨 rank trace 组织，两套输出明确标注 operator，并绑定各自 kernel/launch 身份。
不能用同一个 latest 记录槽先后覆盖两算子的记录，也不以在 D/C 之间导出同一槽代替分区。
完整要求见[Combine breakdown 设计](specs/ep-memory-server-dedup-combine-design.md#第一阶段-breakdown-记录隔离已确认)。

C6 执行约定（2026-09-21）：

- checkpoint=`ep-sdedup-combine-p1-profile`，独立 C6 安装包；functional/performance 使用
  profiling OFF，breakdown 使用 profiling ON。沿用 C5 连续批次/计数/选窗判据。
- 公共 27 参数及 88B launch 不变；16B Combine tiling 消费原保留字节 13 作为 profilingEnabled，
  其余保留字节仍置零。Combine 独立 latest reservation 固定 slot=0，不增加业务 tensor。
- CommArgs 保留旧 Dispatch profiling 字段和 API，末尾扩充 Combine 独立四个 GM base 与容量；
  每区有独立 Host config/reservation/latest/overflow 状态。原 RECORDS_PATH 为 Dispatch，
  新 TILEXR_PROFILING_COMBINE_RECORDS_PATH 为 Combine；未启用区不分配、不导出、不解析。
  combine capture 与原标准 records.v1 同格式，按各自路径独立导出，不在两算子间排空导出。
- 空 Combine 在 50μs 等待前后记录 start/end，latest 只保留最后一轮，各核记录独立；
  标准 parser 使用独立 Combine mapping。两份 trace 明确 operator、run_id、rank、kernel 哈希和最终 launch。
- performance 从同一 msprof 采集分别匹配启用算子，严格要求各自 warmup+loops 数；
  Combine 报告 scope=empty/framework、algorithm_bytes=0、algorithm_bandwidth=N/A，保留延迟。
- 一键验收分 OFF/ON 两次构建：各执行三个 operator 模式（BF16、D0、C0、loops=5/warmup=2），
  OFF 跑 performance，ON 跑 breakdown，共六组 16P；functional 由 C5 Host/consumer 回归覆盖，
  C5 硬件验收在 C6 交付时待验证，现已获用户通过回传。每组显示批次并首错停止，产物与 report 绑定各自 build_id。
- 分工边界：runtime/CommArgs/API/writer 及 runtime 单测；标准测量解析/报告及 mapping 与 Python 单测；
  workflow/入口/安装和控制脚本测试；主任务负责 Combine kernel/Host tiling、consumer、端到端替身及文档。
  CANN9.1 A5 profiling OFF/ON 均需编译；所有开发侧通过均不替代用户设备验收。

维护经验：联合 profiling 共用 run/capture UUID 时仍要分别绑定 operator、kernel SHA、rank、
最终 launch 和各自原始记录/summary/trace。逐区捕获解析错误后再汇总失败，避免 Dispatch 失败
提前中断而丢失有效 Combine 报告。OFF/ON 分目录构建并比较源码内容摘要，禁止把两个版本合并验收。
writer 的 occupied count 必须按 8 条记录对齐，只有两个有效事件也需发布 8 槽；尾部零事件不作为有效数据。
CPU 时钟/导出测试仅验证记录契约，设备缓存可见性和 profile 时序由用户 A5 验证。

维护经验（C6）：CANN 的 `GM_ADDR` 可能是指针宏，`const GM_ADDR` 会限定所指数据，
与 Host typedef 替身的语义不同；记录区可写地址使用明确的 `__gm__ uint8_t * const`。
Host 编译通过不能替代 profiling ON 的真实 A5 编译，OFF/ON 必须分别构建。

### C7：双窗合法边界探针与 Dispatch 回归（当前交付）

C6 交付为 `abee43c70502de88c18ddd0700b0afde528ecc93`，现已按用户回传登记通过；此前按 2026-09-21
用户授权单独开发并提交 C7，不把 C5/C6 的开发检查或 Git 交付当作上机通过。

- checkpoint=`ep-sdedup-combine-p1-memory-boundary`，同一 consumer、独立 C7 安装包，
  profiling OFF、functional。测试内部 `TILEXR_VALIDATION_BOUNDARY_PROBE=1` 选择探针，
  默认 0 保留三种算子模式及 C5 连续批次语义；不新增业务算子参数。
- 一键入口先构建和执行 Host 回归，然后前两台固定 8P，串行三组：独立双窗探针
  （loops=1/warmup=0）、真实 Dispatch BF16、真实 Dispatch FP16（均 loops=5/warmup=2，
  BS96/H7168/K6/E96，D quant=0）。各组新进程、新通信域，首错停止，不让探针写入污染业务回归。
- 探针使用 8GiB 通信域，两窗步长均为 4GiB；每窗统一排除末 4MiB，合法上界为
  相对偏移 4092MiB。第二窗被排除的末端是真实 runtime 区，第一窗相同位置保留不用。
- 每窗四个 32B 写入点：Combine 控制区首 `764KiB`、控制区尾 `816KiB-32`、
  共享数据首 `1MiB`、合法区尾 `4092MiB-32`。各点/各窗使用不同非零模式，
  检查初始零值、设备写后读回、Host 读回及合法范围内相邻 guard 保持零；最后重读全部目标以发现窗别名。
  guard 不进入 runtime 保留范围；208KiB 间隙仅作邻接保护字节读回，不写入。
- 独立测试 kernel 只由一个 AIV 访问本 rank 窗口，通过 UB/DataCopy 写入并清空 UB 后重读；
  64 位偏移贯穿 Host/launch/Kernel。独立 artifact 经注册式 launch 提交，业务 D/C ABI 不变。
  不调用 NextMagic，不伪造算子轮次；probe 提交和完成单独计数，未确认完成前不释放通信域。
- Host 替身注入错误窗口、丢失写入/读回、guard 改写及同步/清理失败，要求无总 PASS；
  脚本严格检查两窗完整点位、独立模式、身份与产物，真实 Dispatch 使用既有完整输出和 Flag 判据。

本点仅证明合法窗口边界的本卡地址访问、邻接保护及真实 Dispatch 回归；不证明跨卡 Combine
结果传输、ready/Flag 协议、yOut 业务数值或超出当前 shape 的业务容量。

维护经验：旧 Dispatch 的 4GiB 单域尾部探针不能直接用于当前 8GiB 双窗；必须从实际
`peerMemBytes/2` 计算窗口，并先扣除统一 runtime 保留量。末端 guard 只能位于合法一侧。
探针与业务回归使用不同通信域，可保留初始零值 oracle，同时避免测试写入影响后续算法。
GM 写后先清空 UB 再读回，Host 零值检查缓冲预填非零哨兵，避免漏读仍命中初始值；
全部目标与 guard 在最后再次复核，可捕获后续探针的别名或越界改写。
内部测试 selector 必须在普通 `run.sh` 入口清零，并与部署配置隔离；否则 shell 遗留
`BOUNDARY_PROBE=1` 会让请求的 both 调用被替换为探针，产生错误的通过结论。
selector 只由 C7 验收 workflow 的专用用例启用，Host 参数回归须覆盖此污染场景。

### Flag 遗留范围

每次测试新建通信 buffer 并在执行前清零，当前第一阶段不受首次 Flag 初值问题阻塞。
未重新清零的已用域、shape/数据布局变化后的结果 Flag 复用为[重点遗留](specs/ep-memory-server-dedup-combine-design.md#deferred-result-flags)，
后续单独讨论；不新增每轮 Host 清零，不取消真实 kernel 已确认的用后清理职责。
本节只记录设计和验收要求，不表示 Combine 或 profiling 分区已经实现或通过设备验证。

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

第一阶段性能测试只验证 profiling、warmup、循环和结果汇总框架是否工作。算子为空时，报告内容必须标注为 launch/framework 开销，
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

在第一阶段框架通过后，按“cnt/cumsum → 待中转第一跳 → 叠加 DIRECT”推进；每个功能模块可包含多笔提交，分别登记功能和性能通过范围。
输入/reference、ABI 和布局适配随消费它们的模块提交，不额外拆出一套上机流程。尚未实现的数据接收路径由测试脚手架隔离。

#### 实现前的设计边界

每个模块开工前重新核对[主设计](specs/ep-memory-server-dedup.md)和[待确认项](specs/ep-memory-server-dedup-open-topics.md)：只确认当前模块的接口、布局、资源存活期和完成条件。发现当前模块的遗漏先补齐；其他模块未收敛不扩大本次实现范围。

| 当前可实现范围 | 已确认的实现约束 | 本次不包含 |
| --- | --- | --- |
| cnt 发送/接收、cumsum | 整体复用 Memory 统计、记录、等待、前缀就绪及清理；适配固定核组和新区域基址 | 新统计算法、额外 q 表、旧 LocalWindowCopy 监听调度 |
| source 数据发送 | R02-01 分类、R05-04 单包流水和 UB/Event 复用、源端输出写出 | R05-05/R05-06 数据接收/转发流水 |
| 公共参数与布局 | R08-03 非量化/MXFP8、R10-01 独立权重输出、R07-01 单份布局 | R10 其余未确认参数范围、R06 数据 Flag 清理及 R11 跨调用复用 |

生产入口保持一个 `init` 和一个 `process`，后者按核角色并行执行 `sendData`、`recvData`，不拆两个生产算子。
48 个源端发送核为物理核 0..47，16 个接收/cumsum 核为 48..63；接收组内索引为 0..15。
`BS<48` 时全部 64 核按目标 expert 区间分担 cnt，每核先完成自己的 cnt 职责；`BS>=48` 时仅 16 个接收核发送 cnt，发送核直接处理数据。
cnt 任务与 Token 分配独立，无 Token 的核仍发送所分配的 cnt；不增加全核 cnt 完成屏障，不让发送核等待 cumsum。
全 64 核仅在算子末尾执行一次 `SyncAll()`，之后核 63 清唯一 counter 的首 4B，并完成该写出；无任务核也参与收敛。

#### 2.1 共用输入、接口与 Host reference

复用已有真实输入构造：BF16/FP16 `x[BS,H]`、保持原顺序且每 Token 不重复的 `expertIds[BS,K]`、非空只读 FP32 `expertScales[BS,K]`。
权重采用可复现的随机归一化值，行和允许 FP32 `1e-6` 误差；K=1 时为 1。hidden 应能区分 source/token，路由和尾部进一步区分 expert/k。
`balance`、`random`、`balance_shift` 沿用已有生成逻辑，保存 seed 和完整原始输入；必要的定向小例只在测试中指定路由。
所有 Rank 使用一致的正 BS/H 和配置，总专家数 N=R*L 不超过 1024，K<=min(32,N)，Dispatch group 表按 C=R*BS*min(K,L) 行预留，第一跳 t 为 Token 级 prefix row，不使用旧 8*BS 编号上界。
共享专家等未支持功能维持现有 0/null 占位，不为 BS=0、非法/重复路由、mask 或中转超限添加新行为。

| 接口适配 | 当前验收要求 |
| --- | --- |
| 权重输入/输出 | 公共签名在 `groupOut` 后、`dtype` 前新增 `float *expertScalesOut`；输入/输出各 `BS*K*4B`，非空、独立且不重叠，输入不得原地改写 |
| 内部 launch/Device | `expandXOut` 后增加 `dynamicScalesOut`，`groupOut` 后、`bs` 前增加 `expertScalesOut`，`expertTokenNumsType` 后、`dtype` 前增加 `quantMode`；13 个 GM_ADDR + 10 个 int64，共 184B；对应 offset 为 dynamicScalesOut=40、expertScalesOut=96、bs=104、quantMode=152、magic=176 |
| 非量化 | `quantMode=0`，输出 dtype 同输入或 RESERVED，`dynamicScalesOut=nullptr` |
| MXFP8 | `quantMode=4`，`expandXOutDtype` 显式选 FP8E4M3 或 FP8E5M2，`dynamicScalesOut` 必填；不是用 quantMode=3 区分 E5M2，也不以 RESERVED 默认选择 FP8 |
| caller 分配 | 按最新内部 layout 分配 hidden、scale、权重及 metadata；容量、设备归属和区间不重叠由 caller/helper 核对，不对裸指针虚构长度检查或新增 public descriptor |

上述变更同步公共声明、Host 参数、launch 结构、Device 签名和 typed 分支，以对应编译/ABI/layout 用例验证传参及必填/类型组合；只检查本次改变的契约，不重跑第一阶段整套验收。

Host/reference 从独立保存的原始路由遍历生成下列预期，不调用设备分组/q 实现来生成自己的答案：

- `cnt[d,e,s]`：source s 的全部原始 route 中 expertId=`d*L+e` 的条数，包含以后经 relay 到达的 route；cnt 由原始 source 直接发送，relay 不补发或转发 cnt。
- `q(s,token,k)`：在原始扁平序号 `token*K+k` 之前命中同一 expert 的次数。第一跳、entry 和 DIRECT 各取自己的原始 q，不能用筛选后序号、包数或 tokenId 替代。
- 分组：按 `(s,token,destinationServer)` 聚合；源 Server 内每条 route 和远端单 route 为 DIRECT，远端多 route 的最小原始 k 为第一跳，其余按原始 k 升序为 entries。
- 源端输出：最终 sourceMask 保留全部 DIRECT k 和各 group 首 k；这些有效位的 expertScalesOut 分别为真实权重和 1.0。空洞不比较，包内权重始终取真实输入。

量化只在设备处理 hidden，Host 不把输入预先换成 FP8；校验沿用 Memory 的 `QuantMxfp8`/`TileXRMxfp8Quant` 语义及[MXFP8 golden](../tests/ep/demo/mxfp8_golden.h)和原有判据。
记 `Q=align256(H)`、`S=align2(ceil(H/32))` 字节；非量化数据跨度 `D=2H`，MXFP8 为 `D=Q+S`，scale 从 Q 开始。
`Payload=align32(D)+align32(32+16*(K-1))`，`T=ceil(Payload/480)*512`；例如 H=7168/K=6 时非量化 T=15872，MXFP8 T=8192。
完整输出最终按 `capacityRows=R*BS*min(K,L)` 预分配，hidden 每行非量化 2H、量化 H 字节，量化 scale 单独每行 S 字节；发送检查点先比包内 hidden/scale，不宣称已经生成最终 `expandXOut/dynamicScalesOut`。

#### 共用通信域与地址预期

按主设计 [单窗布局](specs/ep-memory-server-dedup.md#单窗布局)计算完整偏移，不沿用空 kernel 或旧 Memory 的固定常量。所有字节运算先转 uint64 并检查溢出。
令 C=16、N=R*L，A32/A512 为对应字节对齐；布局如下：

```text
profileOffset = 0; profileBytes = 12800             # align512(200*64)，禁用 profiling 也保留
cntOffset = profileBytes; cntBytes = N*32           # [L,R,32B]
sumOffset = A32(cntOffset+cntBytes); sumBytes = C*C*32
prefixOffset = A32(sumOffset+sumBytes); prefixStride = A32(N*4)
readyOffset = A32(prefixOffset+C*prefixStride); readyBytes = C*C*32
counterOffset = align1024(readyOffset+readyBytes)   # 1024B，仅首 4B 使用
stateBytes = A512(counterOffset+1024)
sourceOffset = 2*1024*1024; sourceBytes = BS*K*U_s
dispatchOffset = A512(sourceOffset+sourceBytes); dispatchBytes = N*BS*T
relayOffset = A512(dispatchOffset+dispatchBytes); relayBytes = 8*BS*K*U_m
requiredBytes = relayOffset+relayBytes
B_d = peerMems[d] + uint64(magic%2)*4*1024*1024*1024
cntAddr(d,s,e) = B_d+cntOffset+(uint64(e)*R+s)*32
dispatchAddr(d,s,e,q) = B_d+dispatchOffset+(((uint64(s)+d)%R*L+e)*BS+q)*T
```

当前预留返回跨度 `U_s=U_m=ceil(2H/480)*512`，不能替换成含 Dispatch 尾部的 T；保留返回区域不等于实现 Combine。
完整前缀和 ready 仅有 16 个消费者，按接收组内索引发布/读取/清理，不向发送核复制前缀；cnt/sum/ready 沿用 Memory 清理，prefix 本身不要求清零。
窗口内布局使用两份固定跨度副本；count、prefix、counter、Token 和 relay 均跟随相同窗口。Host 检查 `stateBytes<=2 MiB`、`requiredBytes<=4 GiB-4 MiB` 和总分配至少 8 GiB。历史 4 GiB 独立 memory-boundary 检查保持原样，它不证明新的 8 GiB 双窗已在设备通过。

#### 2.2 cnt 发送、接收与最终 cumsum 表

这是一个整体功能检查点，不把成熟 cnt/cumsum 内部步骤拆开验收。复用 `CalAndSendCntByRankAndExpert/SendExpertCntRange`、`WaitDispatch`（含清理和汇总）、`GetCumSum/CalRecvAndSetFlag`，适配上面的核组和地址。
`SetValidExpertInfo` 中 expert 计数生成移到完整前缀就绪后的接收准备路径，由物理核 48 写出，不接入旧 LocalWindowCopy 监听调度。
此时只执行 cnt/cumsum，跳过 payload 监听；发送核完成自身 cnt 职责后仍参与算子末收敛。

Host 的主判据是最终 `sendCountsOut` 全表逐项相等，不增加 cnt 包、分段和或核间 ready 的逐层硬件检查。对目标 Rank d，令 `index=e*R+s`：

```text
c[index] = cnt[d,e,s]
expectedSendCounts[index] = sum(c[0:index+1])       # int32[L*R]，包含当前项
expertEnd[e] = expectedSendCounts[(e+1)*R-1]
expectedExpertTokenNums[e] = expertEnd[e]          # type=0，int64[L]
                          或 expertEnd[e]-(e==0 ? 0 : expertEnd[e-1])  # type=1
tokenNum = expectedSendCounts[L*R-1]
```

`expertTokenNumsOut` 作为同一前缀结果的关联输出一起比较，不另立检查点。覆盖零 cnt/整卡 tokenNum=0，仍要求所有通知、等待、输出和清理正常完成。
N<=1024、16 核每核至多 64 条 cnt 记录，不能把 64 当作 Token 监听表长上限。
功能通过后保留本模块并叠加数据发送；cumsum 是后续接收布局的依据，不是源端数据发送的前置等待。
性能记录仅含 cnt/cumsum 的当前 kernel 基线，不能当作完整 Dispatch 性能。

#### 2.3 待中转数据（第一跳）发送及尾表

在 2.2 基础上启用远端多 route group 的第一跳，暂不执行 DIRECT 或 relay 展开。
源端连续分核沿用主设计：`base=floor(BS/48)`、`remainder=BS%48`，第 i 核 `count_i=base+(i<remainder)`、`start_i=i*base+min(i,remainder)`；同一 Token 全部 K 路归同一核。
第一遍使用每核 `serverRouteMask[16]`（64B）分类，将 DIRECT k 留在本核 directMask；每个远端多 route Server 发一包，第一跳本身不重复写入 entries。

测试脚手架放在独立测试文件，以内部测试开关/参数限制执行范围，不新增 public debug ABI。
各 Rank 完成发送、整批 stream 同步和 Host barrier 后，Host 只读本卡预期 GM 槽位，按每 480B Payload+32B Flag 剥离并解析；这是完成后读回，不模拟正式监听/转发流水。
检查覆盖完整 T 的全部块 Flag，尾表 offset 相对剥离后的 Payload，不能直接作为物理通信地址。

| 第一跳 Host 预期 | 校验重点 |
| --- | --- |
| 物理槽位 | 用第一跳 expert 得到 d/e，用原始 s/q 计算 dispatchAddr；每个预期第一跳槽位恰有对应包，定向小例检查非目标槽位未收到本轮包 |
| 数据段 | 非量化 hidden 与原输入逐字节一致；量化 hidden/scale 按 Memory golden 比较，数据填充不当作有效 hidden |
| 固定头 32B | offset 0/4/8/12 为 int32 srcRank/srcTokenId/k 与 FP32 真实 weight；16/20 为 forwardCount/forwardEntryCount，均等于 n=m-1>0；24/28 的 relayRank/forwardTokenIdx 均为 -1 |
| 有效 entries | 从 offset 32 起，每条 16B 为 destinationExpertId/destinationQ/originalK/FP32 weight；只比较前 n 条，按原始 k 升序且排除第一跳，覆盖 relay 本卡与跨卡 expert |
| 容量和填充 | 尾部按 K-1 条容量计算 T；n 才是有效数，未用 entries、传输/对齐 padding 不要求清零，不参与解析 |
| 源端输出 | 本检查点 sourceMask 仅置已启用的各 group 首 k，这些 expertScalesOut 为 1.0；验证只读输入仍保留所有真实权重，包中未替换为 1.0。DIRECT 输出在 2.4 补齐 |

cnt 仍统计全部原始 route，不能据 `tokenNum` 等待所有 payload 到齐；本检查点只期望第一跳物理包。
例如 R=24/L=3、R0/T0 原路由 `[E4,E25,E28,E50,E26,E5]`，本步只向 R8/E25 发一包，entries 为 E28(k2)、E26(k4)，mask=`0b000010`；混合 route 不因关闭 DIRECT 而改写原始 q/cnt。
功能重点是接收槽位中随数据携带的尾表正确，性能记录“cnt/cumsum+第一跳发送”的 kernel 时间及第一跳实际包数/T 字节。

#### 2.4 叠加 DIRECT，完成源端发送

在同一 kernel 的每个发送核内部，对自己的连续 Token 区间扫描两遍：先下发本核全部第一跳，再根据 directMask 下发全部 DIRECT。
不是在同 stream 分别提交两个 kernel，也不是每个 Token 的第一跳后立即跟上 DIRECT；不同核可以处在不同扫描阶段，不以跨 Rank 到达时间断言全局先后。
两遍之间不加 PIPE_ALL、SyncAll 或阶段 MTE3 等待；没有第一跳任务的核可直接发 DIRECT。第二遍不能用包含 group 首 k 的 sourceMask 代替 directMask。

DIRECT 覆盖源 Server 内本卡/跨卡的每条 route，以及远端单 route；`forwardCount=forwardEntryCount=0`、返回字段均 -1，头部 k/weight 为当前原始 route。
沿用 2.3 的完成后 Host 读回，比较目标槽位、q、hidden/scale 和尾部，同时保持第一跳尾表正确。
Host 将“第一跳头部 route+有效 entries+全部 DIRECT”展开后，与全部原始 route 一一对应；物理包集合只包含第一跳和 DIRECT，不要求 entry 的终端 payload 已经到达。
槽位最终内容不能识别同址同值的重复发送；上述判据验证最终内容和逻辑 route 覆盖，实际发送次数需要时通过测试打点补充证据。
上例此步补发 E4(k0)、E50(k3)、E5(k5)，最终 mask=`0b101011`；group 首 k 权重输出 1.0，DIRECT 为原权重，空洞不比较。

源端流水直接复用 Memory 的 `MTE2 CopyIn → 可选量化 → 原 Tensor 尾部填字段 → Vector DataAsFlag 打包 → MTE3`。
非量化在 xInTensor 尾部填信息；量化在 quantTensor_ 的 hidden/scale 后填信息，不另建拼尾 Tensor 或增加整包中间拷贝。
输入队列双 buffer，源端所需 UB 先预算，剩余 outBuf 取 1..8 路；每路按新 T、输入/量化结果按新 Payload 容量分配，安全输入至少容纳一路。
lane=`累计实际发送包数%sendTokenBufNum`，两遍共用包序号和池；固定 lane 的 MTE3_V/V_MTE3 事件初始化、等待/发布和核末回收沿用 Memory。
权重输入/输出 UB 各 `align32(count_i*K*4)`，sourceMask/directMask 各 `align32(count_i*4)`，另有 64B serverRouteMask，均在计算可用发送路数前计入。
核末先正常回收发送池，再按 S_MTE3 → 两次 DataCopyPad → MTE3_S 写出 mask 和权重；只写有效 `count_i*4`、`count_i*K*4` 字节，不能用对齐填充覆盖邻核输出。

功能用例覆盖发送池环绕、第一跳/DIRECT 跨遍复用，以及每核只有 1/5/6 个 Token 的输出边界；可用小规模 trace 核对每核顺序和任务数，不为顺序检查加入同步。
性能记录当前“cnt/cumsum+完整源端发送”的 kernel，与同 shape、路由和测量条件的 2.3 基线比较；不能将相减结果标为 DIRECT 的独立精确耗时，因为两个核组并行且 MTE3 可跨遍在途。
`sourceMask/expertScalesOut` 随本模块完成验收；`assistEx/group` 及最终 expert 行数据由未来接收 owner 生成，此时不校验其未写入内容。

#### 用例、性能及脚手架边界

| 范围 | 当前模块的最小覆盖方向 |
| --- | --- |
| cnt/cumsum | BS=1/47/48/49 覆盖无 Token 核和 cnt 分工切换；零 cnt/整卡 tokenNum=0；两种 expertTokenNumsType；不同路由分布 |
| expert/K | L=1、K=1/32（不超过 N）、总 expert 上限 1024；(R,L)=(16,64)/(64,16)/(128,8) 及 24P/L=42 按可用硬件和容量取安全 shape，Host/layout 与实卡证据分开 |
| 第一跳/DIRECT | 同 Token 混合两类、同 Server 多 expert、交错 k、不同 source 的同 tokenId；较晚 Token 先发而 q 仍用原始前缀；K=1 无第一跳正常结束。单 Token 多远端 group 需至少 3 Server，16P 记录未覆盖 |
| Payload/类型 | BF16/FP16 × 非量化/E4M3/E5M2 六种组合分批覆盖；选共同支持的 H 尾部及 metadata 跨 480B 边界例，检查所有块；不定义尚未确认的任意 H 范围 |
| UB/输出 | BS=256 的每核 5/6 Token，非 32B 输出长度和邻核边界；实际包数超过发送池路数、跨两遍的池复用；mask 的 k31 使用 uint32 位操作 |

这是本次模块的验收方向，R12 的最终完整 Dispatch 矩阵和性能目标仍待确认。每个实现提交只选择受影响的 case，不跑笛卡尔积，不重做第一阶段已通过的检查点；没有 64P/128P 时在现有规模推进并注明未覆盖项。
依次手动执行三个入口：`build_all.sh` 编译安装、`sync.sh` 同步产物、`run.sh` 运行单 case（内部调用 `run_case.sh`）；每一步通过后再执行下一步。
`run_batch.sh` 仅保留可选人工扫描用途；`run_commit_validation.sh` 已停用自动编排，仅提示迁移命令。
`validation.local.sh` 只保存环境，模式、loops、warmup、shape 等通过 case 参数传入；当前 production checkpoint 在编译入口选择并写入 manifest，同步/运行继承安装检查点。

functional 默认 1 轮，不采 profiling 或性能；performance 沿用 msprof，warmup 排除，统计有效 kernel 记录；breakdown 沿用全 Rank 解析、筛选和 trace 输出链路。
多轮保持同一 stream 连续提交整批，整批末才 stream 同步和 Host barrier，Host 读回/解析在测量外。
数据发送检查点尚无正式接收清理；在 R06/R11 或当前测试脚手架尚未明确防覆盖和旧 Flag 隔离前，先做单次功能与单次 kernel 测量，不把空 kernel 多轮通过当作数据多轮证据。
需要多轮测量时先明确本检查点的测试隔离/回收和跨 Rank 完成边界；测试专用动作放独立文件并排除出被测 kernel，不通过逐轮 Host 同步/barrier 规避，也不据此确定生产窗口复用方案。

msprof 给出当前已启用路径的整体 kernel 时间；模块内耗时仅在已有对应打点时用 breakdown 报告，核组并行时不将各段时间相加当作总时间。
日志保留 commit/artifact、backend/native 状态、启用模块、shape/路由/量化、loops/warmup、逐 Rank 样本及所有 Rank 的 median/min/max，沿用现有聚合口径；流量分清原始 route 数、第一跳/DIRECT 包数、完整 T（含尾部/Flag/填充）及实际本卡/跨卡/跨 Server 字节，适用时报告带宽。
终端显示 case 参数、有效 kernel 性能报告和最终结论，失败时直接显示错误类型、host/rank/stage（如已获取）和日志绝对路径，保留原始非零退出码。
运行输出同时保存在 `run/case-*/workflow.log`；实际汇总为 `run/case-*/<run_id>/summary.json`，预检查失败可能尚未生成汇总。避免再次折叠成单独的 `Failed`；不统计或打印 Host submit/sync/barrier 时间，不追加无条件 Pass。
性能基线与诊断插桩分开，先登记测得数据，不臆定尚未确认的性能阈值或宣称完整算法收益。新增通信测试复用匹配的 HCCL 环境资格记录。

#### 2.5 以后：待数据接收/转发设计收敛后补充

本次不预拆监听、relay 本卡/跨卡展开、公共转发 UB 回收、assistEx/group 写出和完整 Dispatch 组合检查点；后续按最新已收敛设计逐项补计划。
每次记录仅保留“模块/检查点、实际通过范围、通过 commit SHA、结论”，不记录每笔提交改了什么；失败在当前模块返工后再叠加。

维护经验：发送与接收分步实现时，原始 cnt 表示最终 route 总数，当前物理包集合只表示已启用路径。用 cnt 等待未实现的 payload 会造成假超时；用后者代替原始 cnt 又会改变 q/行布局。Host 保留这两种预期分别核对，不把发送后读回宣称为正式接收/转发通过。

### 第三阶段：Combine Server Dedup 空算子工程

在 Dispatch metadata ABI 稳定后，增加独立的 Combine Server Dedup API、空 kernel、Host layout、注册、launch 和测试框架。
该阶段仍不实现 Combine 数值逻辑，只验证输入 metadata 的生命周期和空算子调用。

### 第四阶段：逐步实现 Combine 算子逻辑

按 expert 返回、group 监听和归约、partial 返回、source 归约、数值正确性和性能报告顺序逐步实现。
Combine 的真实验证必须使用可区分的 expert 输出，不能使用会互相抵消错误的联合测试作为唯一证据。

## 第一阶段接口和资源契约

以下至文末保留第一阶段空工程的历史规划；旧接口占位、默认轮次/核数、脚本示例和输出口径不作为阶段二要求。
后续实现使用上文阶段二当前契约及现有三脚本流程，实际完成情况以[调试进度](EP_MEMORY_SERVER_DEDUP_PROGRESS.md)为准，不重新回归已通过的空工程检查点。

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
| Rank/expert | 每 Server 8P，R 为 16 至 128 且为 8 的倍数；rank 在 `[0,R)`；`0<moeExpertNum<=1024` 且 `moeExpertNum=R*L`，L 由均匀分配推导并允许为 1；`1<=topK<=min(32,moeExpertNum)`；128P 下 L 至多为 8 |
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
| `abi-host` | Host 测试覆盖 L=1、384 总专家的典型配置、1024 总专家上限 (R,L)=(16,64)/(64,16)/(128,8)，以及 24P/L=42（总数 1008）；K=1/6/32 只组合不超过总专家数的值。负例含总专家数超过 1024、不能均匀分配、其他非法 shape、空指针、对齐、布局溢出和 helper 容量；不要求全部笛卡尔积 |
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
