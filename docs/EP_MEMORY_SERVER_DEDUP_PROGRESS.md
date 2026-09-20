# EP Memory Server Dedup 进度

## 当前状态（2026-09-20）

Dispatch 优化设计阶段收口，当前规格见[主设计](specs/ep-memory-server-dedup.md)，
五表改动前设计提交为 `3b0ba92`。完整 Dispatch 与五表输出已实现，接下来完善 [Combine 设计](specs/ep-memory-server-dedup-combine-design.md)；
Server Dedup Combine 尚未实现。下文阶段 1/2 的表格和日期记录属于历史交付证据，不作为当前实现缺失清单。

已建立 [Dispatch / Combine 联合设计初版](specs/ep-memory-server-dedup-dispatch-combine-design.md)，
已确认五表与 Combine 27 参数设计接口：保留 Memory 原 24 参数，只追加 3 表，
TokenNum 读取 sendCounts 末项。M1–M5 用户细化补充已完成；Combine 独立设计进入整体通读和分级收口，M0/M6 衔接一并纳入。
原 24 项中 CB00-01 已确认：接口含义沿用 Combine Memory，Host 封装、GM TilingData 和 launch
形式参考 Dispatch Server Dedup；公开 27 参数不变，设备所需标量通过独立 tilingData 传递。
[待确定汇总表](specs/ep-memory-server-dedup-combine-design.md#待确定汇总表)剩余 23 项：
高风险 11 项、中风险 9 项、低风险 3 项；无参考 2 项与风险交叉，合计 12 项建议重点核对。
各模块分别记录已确认项、待确定项、推荐和代码依据，推荐尚未自动视为用户确认。
最新约定为固定 64 核，全部先执行 Init、CopyLocalPendingTokens 两个函数；随后分为 Send/Read 核，
Send 核直发类型 1，Read 核拉取类型 2 成员、加权聚合并发出 partial。最终 BS 个 Token 的聚合
独立为 M5，首版由 Read 核负责，任务映射与核类别解耦，预留后续 64 核参与。
M2 由核 56..63 汇总本卡 sum=64.0f 后通知各卡；各 Read 核独占 512B 卡间 Flag 块，
取 8 个槽首 float 求和为 8.0f 后就绪并自行清理。recvCoreNum=8*N（N=1..6），卡间 Flag 两份乒乓共 8..48KiB。
M3 由各 Send 核用 Compare＋GatherMask 密排类型 1 原始行号，再按 sendCoreNum 商余均分；
可整除时任务数相同，否则最多相差 1，发送地址仍取表 2 的原始行。
表 2 仅缓存本核列表行号所覆盖的连续区间，按原始 row 减缓存首行索引，不要求整表搬入 UB。
M4 同样密排类型 2 行号，按 recvCoreNum 商余均分汇聚转发任务；表 2/3 仅缓存本核行号范围，
表 3 提供成员及加权信息，表 2 提供返回目的与成员寻址所需的原始 source，尚未实现。
M5 按 BS 整 Token 分核，预载本核表 4/5 区间，逐 Token 轮询，未到齐即检查下一个。
M5 已改为按 mask 置位的副本逐个提交多块 Flag 拷贝，UB 按 32B 块密排，统一等待后
用一次 ReduceSum（mask=1、repeat=B、stride=1）累加各块首 float，B=N×有效槽数。
用户确认总线协议保证完整性；单次 probe 为 m 次拷贝、32*m*N 字节，实际代价待设备验证。
Combine 新中转区的分配归属和内部细节待收口；Dispatch 五表 ABI 和新包尾布局已实施。

[Dispatch 输出适配方案](specs/ep-memory-server-dedup-dispatch-output-adaptation-design.md)已更新：
32B 包头保留 source rank/token/k/type 四个有效值，type 与 Combine 的 TokenType 共用 1/2/3 定义；
源端生成可原样保存的 8*K 索引段，两个段起点 32B 对齐，索引段避免跨 Flag，Init 缓存两个 wire offset。
上述布局和原样保存方式、源端生成规则、接收任务定位及 DestinationIndex 生成均已确认；
第一跳保留本地 type_2，最终转发前仅改包头 type 为 type_3，其他字段不变。
T01 独立参考与逐输出校验已确认；T02 采用全局路由参考加各 Rank 本地匹配方案，
不新增 Rank 间校验通信。D08 写出同步、D09 Host/caller 清理和 T03 回归已确认，
D01–D09/T01–T03 全部确认并完成实现，执行顺序与验证入口见适配方案第六节。
2026-09-20：真实 CANN 9.1/A5 profiling OFF/ON kernel 与 Host 库编译通过；
14 项相关 Host CTest、25 项 C++ 集成替身、113 项 Python chain/workflow 回归通过；
关闭编译期去重的隔离副本另有 6 项发送/Host/launch 回归通过。
五表真实 NPU 正确性、多轮复用和性能尚待用户设备验证，历史产物的设备结论不自动继承。

| 方向 | 当前实现与验证边界 |
| --- | --- |
| Dispatch 数据面 | 原始 cnt/cumsum、第一跳/DIRECT、接收/relay、metadata、双窗与清 Flag 均已实现 |
| 当前优化 | Count 异步下发/共享 scratch、动态收发 lane、二维 Probe、Token prefix 行与 slot 定位、五表输出、核末无 SyncAll、简化打点、独立 GM tiling、编译期核数/去重开关 |
| 暂缓候选 | 双 Probe、剩余 ≤5 时预探测、ready 认领删除、Vector ready bitmap、metadata 地址缓存 |
| 已收到的完整设备结果 | 64P/E384、BS96、H7168/K6、quant_mode=4/e4m3、breakdown 100+50 轮，64/64 PASS；run_id=b662f5c701e74ec187fdb7f09cb39bc9、kernel=65f0523c47a6 |
| 最新产物验收 | 上一行仅覆盖那次产物；之后源码修改不能自动继承该正确性或性能结论，128P 等组合需各自证据 |
| 未定位故障 | 32P/E384、BS256、performance 100+50 轮报告执行阶段失败、0/32；run_id=d5d6c21f230a496e82ccfa2dc4e0494a，缺原始错误，未确认环境根因 |
| Combine 新方向 | 64 核依次执行 Init 和本地待聚合 Token 拷贝两个函数；之后 Send 核直发，Read 核读取聚合后发送；独立 M5 首版由 Read 核聚合本卡 BS 个结果，预留 64 核参与。尚未实现 |
| Combine 选窗与复用 | 沿用当前选窗，主要隔离 Dispatch/Combine；跨调用 buffer 安全由用户确认的模型调度保证，不新增轮次管理或读完握手 |
| Combine 设计推进 | M1–M5 细化补充完成，已通读并分级；CB00-01 接口组织已确认，剩余 23 项待收口，M0/M6 衔接同步核对 |

以上设备信息依据用户回传，未直接访问生产环境。约 49us 的历史 performance 与约 391us 的
breakdown 口径和产物不同；移除 PacketIssue 未明显缓解的反馈不能证明根因已经闭合。
本次五表迁移不新增 NPU 通过记录，也不将“优化设计告一段落”解释为所有性能目标验收。

## 历史验证记录

完整命令见[人工验证](EP_VALIDATION_GUIDE.md)。下列“当前/下一步/未开始”指对应记录的提交日期。

## 阶段 1：Dispatch 公共框架与空算子

| 检查点 | 已通过范围 / 待验证项 | 通过 commit |
| --- | --- | --- |
| 1 `ep-sdedup-p1-abi-host` | ABI/layout/Host 已通过 | [acb976c](https://github.com/LingquLab/TileXR/commit/acb976cb6c5527e02a0d931e20a954eb1c891d37) |
| 2 `ep-sdedup-p1-empty-kernel` | production 2 主机安装/注册通过，未 launch | [eae264d](https://github.com/LingquLab/TileXR/commit/eae264dc2e3d33169ffd2e78e04c3430d41b7559) |
| 3 `ep-sdedup-p1-dry-run` | production 16P 映射/退出通过 | [67ad16f](https://github.com/LingquLab/TileXR/commit/67ad16f2e917bd1e1fd17a83262c69b3d7b5680c) |
| 4 `ep-sdedup-p1-chain-single` | production 16P 建链/单轮空 launch 通过 | [f5bfa58](https://github.com/LingquLab/TileXR/commit/f5bfa58e93ba7b8d7103c67fc20b9ae08044fb09) |
| 5 `ep-sdedup-p1-chain-multi` | production 16P 空 launch/批末同步/barrier 通过；用户已确认后续多轮批测通过 | [e5a4bdb](https://github.com/LingquLab/TileXR/commit/e5a4bdbcc91052e2aa7a0975e9c9886ff75fe283) |
| 6 profile：performance | 用户确认当前 kernel 计时批测通过；具体 case 参数以运行 summary 为准 | [3b03a4f](https://github.com/LingquLab/TileXR/commit/3b03a4f7e96aa4a1aff8b57f6e95e15a1db95e4f) |
| 6 profile：breakdown | 最后一轮设备打点与耗时拆解待验证 | — |
| 7 `ep-sdedup-p1-memory-boundary` | 每 Rank 4 GiB 分配/建链、本卡末尾 32B DataCopy 写入/读回及 Host 校验通过；免构建/同步重跑通过 | [f79267d](https://github.com/LingquLab/TileXR/commit/f79267d1c0bdeac92100c8272453438f9ab432de) |

2026-09-11：用户确认 `bash run_commit_validation.sh` 和 `bash run.sh run_mode=functional loops=1 warmup=0` 均执行成功。
按最近交付的 `f79267d` 登记，未直接读取本次 summary 核对实际 SHA 和卡数；通过范围为通信域分配/映射与本卡边界访问，不包含跨卡 payload 传输。

2026-09-10：用户确认当前检查点通过，按最近交付的 `3b03a4f` 登记；尚未直接读取此次结果目录，不推断新增规模、dtype 或 breakdown 覆盖。下一步为阶段 1 的 breakdown 设备打点验收，只执行新增 case，不回归已通过检查点。

此前 chain-multi 结果：2/2 主机、17 项测试/主机、16/16 rank PASS；
`/home/h00580772/TileXR-git-chec/run/validation/f97fb895b7e84af28f482a3154ef4ddb/summary.json`。
此前 empty-kernel 结果：`/home/h00580772/TileXR-git-chec/run/validation/f86df093945c42f8985be62e6e934ab9/summary.json`。
以上依据用户回传，未直接读取远端 JSON；`unverified` 是提交来源标签，不表示测试失败。

`chain-multi` 每次只运行 `TILEXR_VALIDATION_RUN_MODE` 指定的一种模式，默认 functional/1 轮/无预热。
`mode=production` 不代表三模式全部执行；实际模式见 `summary.json` 的 `chain_configuration.run_mode`，
performance/breakdown 还需对应的 `measurements` 结果。1 轮 PASS 不证明多轮复用；阶段 1 均不证明去重通信或数值正确。

## 阶段 2：Dispatch 算子逻辑

| 检查方向 | 状态 | 通过 commit |
| --- | --- | --- |
| cnt 发送/接收、cumsum | 用户确认最新提交验收通过 | [0bc6f1b](https://github.com/LingquLab/TileXR/commit/0bc6f1bbe64f25ab21a8fae23e70eed797dc48a5) |
| server grouping / 第一跳 | §2.3 用户确认单轮检查点 PASS；多轮及性能达标未验收 | [0df1db0](https://github.com/LingquLab/TileXR/commit/0df1db02bb139c447456c283afe05586ff9f3c5d) |
| DIRECT / 完整源端发送 | §2.4 用户确认单轮检查点 PASS；后续多轮结果见下一行，性能达标未验收 | [631a1ff](https://github.com/LingquLab/TileXR/commit/631a1ffcc9cc138e583c1b657cd73ef1a6e64792) |
| 源端发送多轮 / 量化 | 用户确认 64P、384 专家（每卡 6 专家）、量化/非量化多轮测试通过；具体输入类型、FP8 格式、轮次及运行模式未回传 | [d9eb0c4](https://github.com/LingquLab/TileXR/commit/d9eb0c45ecc7f13c7c10caedac697be7257ccd8c)（按最近交付登记） |
| DIRECT 循环 server 顺序 | 用户确认本次交付测试通过；本次具体配置、轮次和时延未回传 | [c572a59](https://github.com/LingquLab/TileXR/commit/c572a596b9e5230b489cd1365fb784e7eddcae6d)（DIRECT 改动 b45b20c，按最近交付登记） |
| 接收准备 / UB 监听表 | 用户回传 `EP_SERVER_DEDUP_ABI_HOST_PASS test=counts`，验证 16 核 floor 分区、物理 tokenPos 和 pendingIdx 末项替换；生产入口尚未执行接收准备。 | [daf8173](https://github.com/LingquLab/TileXR/commit/daf8173e2c152ba6317a2a8039960865af099a9f)（按最近交付登记，Host 测试） |
| 接收包 / assistEx / group / 公共转发副本 | 用户回传 `EP_SERVER_DEDUP_ABI_HOST_PASS test=receive_packet`，全包 Flag 探测、完整解包及三类 metadata 的 Host 测试通过；CANN/A5 设备验证仍待完成。 | [06c00ca](https://github.com/LingquLab/TileXR/commit/06c00ca6634106ab8baf080dc29b021cf9d3978f)（按最近交付登记，Host 测试） |
| 接收监听 / 第一跳展开调度 | 当前 source-send 入口不执行 receiver；完整 UB/Event 配置、跨 Token pending/relay 调度及最终 Flag 生命周期尚未闭环，未上机验收。 | — |
| Dispatch correctness / 完整验收 | 未开始 | — |

2026-09-12：依据用户“最新的提交验证通过”回传，按当前最新提交 `0bc6f1b` 登记 cnt/cumsum 检查点通过。
未直接读取本次 summary；具体规模、模式、轮次及性能数据以实际运行结果为准，不扩展为 Token 数据发送或完整 Dispatch 验收通过。

2026-09-12：用户确认“阶段 1 的第一个部分”测试通过并要求实现第二部分；按当前设计验证流程 §2.3 接入第一跳发送。该反馈延续已有 cnt/cumsum 基线，未读取新 summary，不推断额外 dtype、规模或时延。

上一检查点 `ep-sdedup-p2-first-hop` 仅开放单次 functional/performance/breakdown；当前 `ep-sdedup-p2-source-send` 在相同 16P/L6/BS96/H7168/K6/BF16/balance_shift/seed2026、每 rank 4 GiB 基线上启用 DIRECT。数据接收转发和 Combine 未实现。本地验证覆盖非量化 sender、184B ABI、单份布局、consumer 错包诊断和 v3 trace，不能作为 CANN/A5 通过记录。

2026-09-12：修复第一跳 `Duplicate` 掩码限定符编译错误后，用户回传“1轮测试通过，最终结果pass”。按最近交付的 `0df1db0` 登记 `ep-sdedup-p2-first-hop` 单轮检查点通过；未直接读取服务器 summary，实际 SHA、配置和运行模式以该次结果为准。通过范围为当前 cnt/cumsum + 第一跳包及源端输出校验，不扩展为量化组合、breakdown、多轮复用或性能达标已验收。下一增量按验证流程 §2.4 叠加 DIRECT 直发。

2026-09-12：用户确认 `ep-sdedup-p2-source-send` 测试通过。按最近交付的 `631a1ff` 登记单轮完整源端发送检查点通过；未直接读取服务器 summary，实际 SHA、配置、模式和性能数据以该次结果为准。通过范围为 cnt/cumsum、第一跳、DIRECT、完整 route 覆盖及 sourceMask/expertScalesOut 校验，不扩展为多轮复用、接收/relay、Combine 或性能达标已验收。

2026-09-13：用户确认“64P 量化/非量化 384专家 多轮测试通过”。按最近交付的 `d9eb0c4` 登记；未直接读取服务器 summary 或 kernel SHA。通过范围为当前 source-send 的 cnt/cumsum、第一跳/DIRECT、源端输出及同域多轮完成协议，最终数值校验只读取最后一轮。此次反馈未给出输入 dtype、FP8 格式、BS/H/TopK、warmup/loops、run_mode 或时延，不推断两种 FP8 格式均通过，也不标记性能目标、接收/relay 或完整 Dispatch/Combine 已验收。

接收侧实现经验：DataAsFlag 的 Payload 不能按单段连续字节读回，必须对整个包（含 metadata）按每个 480B 数据块剥离 32B Flag；逻辑 tailOffset 只能用于解包后的数据。公共转发副本保留原 k/weight、entry 数量和列表，终端按 expert 匹配并写入独立 assistEx；group 的 FP32 weight 按原位型复制，GM 字节 stride 换为 typed tensor 元素下标。本地 CPU 替身验证格式和串行写出，真实 CANN/A5 的事件顺序、跨 Rank 调度和多轮复用仍需服务器验证。

2026-09-13：用户回传“目前测试通过了，继续完成下一步的功能点”，按最近交付 c572a59 登记 DIRECT 循环 server 顺序通过。未读取此次 summary 或 kernel SHA，不沿用上一记录的规模、量化格式、轮次或性能结论。下一增量先落实已确认的接收准备；完整接收/转发依赖 R05-05/R05-06 的 UB/Event 与跨 Token 调度、R06 清理方案收口。

2026-09-13：用户回传 `EP_SERVER_DEDUP_ABI_HOST_PASS test=counts`，按最近交付 `daf8173` 登记接收准备的 Host 验证通过；未直接读取实际 SHA/构建配置，不推断 NPU 接收已执行。本次继续落实已确认的包处理与 metadata：所有块的 8 个 Flag word、完整 unpack、DIRECT/第一跳/终端 assistEx、dense group 和公共转发副本。本地 5/5 定点 CTest 通过，包含地址空间检查、counts、source-send 打点 OFF/ON 及 receive_packet；构建脚本测试 14 项通过、1 项因 MSYS symlink 限制跳过。生产检查点保持 source-send，暂存 receiver 的逐槽等待及串行清理不作为 R05-05/R05-06/R06 的最终实现。

2026-09-13：用户回传 `EP_SERVER_DEDUP_ABI_HOST_PASS test=receive_packet`，按最近交付 `06c00ca` 登记接收包处理的 Host 测试通过。该测试使用 CPU AscendC 替身，未执行 NPU kernel；未直接读取实际提交和构建日志，不据此登记 CANN/A5 编译或设备通过。下一验证增量应补独立 A5 接收包测试，验证真实 API、UB 搬运及流水依赖；完整多卡接收、异步转发和多轮复用仍待接收调度与清理闭环。

### 2026-09-16：全核 Count 后的 FP8 溢出模式丢失

用户回传 64P/E384、BS96/H7168/K6、BF16→E4M3、performance 0+1 轮失败，
run_id=`0a16163122c2489581e777d54d581b18`、build_id=`ecbc2f381cd647e288ffe8dbd552293a`。
Count 阈值 128 时失败，48 时通过；非量化两者均通过。rank23 的首错是
source0/token16/byte53：期望 `0xfe`，实际 `0x7f`，共 176 字节不符；
接收槽与输出一致，scale 和已打印 assist 字段一致。按夹具重建该行，byte53 的输入为
`-15.625`、scale 为 `1/32`，缩放后为 `-500`；整行恰有 176 个元素的绝对值大于
E4M3 最近偶数舍入的有限值边界 464。这与饱和到 ±448 变为溢出 NaN 的模式吻合，
不应先修改 relay、放宽 golden 或归因于 profiler 的退出提示。

源码边界：`Counts::Init` 设置 `CTRL[60]=0`，但阈值 128 使 BS96 的发送核进入
`SendCnt` 并执行 `TPipe::Reset`；阈值 48 则直接返回。官方 asc-devkit v9.1.0
[A5 Reset 实现](https://gitcode.com/cann/asc-devkit/blob/e5451c7feba11f9e259f4fbc19c4bf82f8c0496c/impl/basic_api/dav_3510/kernel_tpipe_impl_c310.h)
调用 [InitSocStateImpl](https://gitcode.com/cann/asc-devkit/blob/e5451c7feba11f9e259f4fbc19c4bf82f8c0496c/impl/basic_api/kernel_utils_base.h)，
将 CTRL 重设为 `0x1000000000000008 | (old & (1ULL << 48))`，因此 bit60 重新变为 1。
修复在 Count 的 Reset 后重新设置 bit60 为 0，维持后续阶段契约；`Finish` 仍恢复入口值。
维护时不要把 Reset 当作只回收 UB/Event：阶段依赖的控制寄存器状态也须重建。

验证边界：Host 替身补齐上述 Reset 副作用后，未修复版本在状态断言失败；修复后
阈值 48/128 的 C++14 counts 测试通过，包含 64P/E384/BS96；阈值 128 下的发送、接收
Host 替身测试也分别在 profiling OFF/ON 通过，共六项检查。这些测试只证明阶段状态
契约和 Host 模型，不证明真实 FP8 指令或多卡异步行为。当前环境无 CANN，
`ssh -o BatchMode=yes -o ConnectTimeout=10 blue true` 返回 255（无法解析 blue），
未进入 A5 编译。回传日志的 commit 为 null，尚未取得实机工具链和 binary 哈希；
需重新构建、同步后以相同阈值 128 和原命令复测，再补阈值 48、非量化及多轮回归。

## 阶段 3：Combine 空算子工程

| 检查方向 | 状态 | 通过 commit |
| --- | --- | --- |
| 公共 API / Host / layout | 未开始 | — |
| 空 kernel / 注册 / launch | 未开始 | — |
| 单轮 / 多轮框架 | 未开始 | — |

## 阶段 4：Combine 算子逻辑

| 检查方向 | 状态 | 通过 commit |
| --- | --- | --- |
| expert 返回 | 未开始 | — |
| group 归约 / partial 返回 | 未开始 | — |
| source 归约 | 未开始 | — |
| correctness / 性能验收 | 未开始 | — |

历史 source-send 阶段说明（已由完整 Dispatch 取代）：当时交付 `ep-sdedup-p2-source-send`，支持第一跳 + DIRECT 的同域连续多轮。默认仍为 16P/L6/BS96/H7168/K6/BF16/balance_shift/seed2026、每 rank 4 GiB、loops=1/warmup=0；64P/E384 的新增通过范围见上文。完整 Dispatch 多轮复用仍待接收清理契约。
