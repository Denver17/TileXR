# EP Memory Server Dedup 进度

## 当前状态（2026-09-21）

Dispatch 优化设计阶段收口，当前规格见[主设计](specs/ep-memory-server-dedup.md)，
五表改动前设计提交为 `3b0ba92`。完整 Dispatch 与五表输出已实现，接下来完善 [Combine 设计](specs/ep-memory-server-dedup-combine-design.md)；
Server Dedup Combine 的 C1/C2 已收到用户 `COMMIT_VALIDATION_PASS` 确认；C3 也已收到用户通过确认；C4 单轮接线已收到用户通过确认；C5 已交付、待设备验证；按用户授权提前交付 C6/C7，分别提交。
下文阶段 1/2 的表格和日期记录属于历史交付证据，不作为当前实现缺失清单。

Combine 第一阶段采用[七点逐次提交/外部验收流程](EP_MEMORY_SERVER_DEDUP_VALIDATION_FLOW.md#七点交付与外部验收门槛2026-09-20)。
C1 交付 commit 为 `01c9b1d9f2fa49ea15e57df01c1cfd1905b6cd1f`；2026-09-20 用户明确回传通过。
未附外部 summary/环境信息，实际运行 SHA、工具链与测试日志未独立核对；按用户确认登记 C1 通过并推进 C2，
不把这一回传当作 NPU 数据面证据。
C2 交付 commit 为 `3b2706281e450df7c6263dc112a753fbece21493`；2026-09-20 用户回传：
`COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p1-empty-kernel`。
报告路径：`/home/h00580772/TileXR-git-check/run/validation/commit-4d2b65cfcb1b4328bf2cf4058df4d364/summary.json`。
该目录名是验收 run 标识，不能据此推断实际 Git SHA。未取得 summary 内容、设备/工具链及产物哈希，
按用户回传登记通过；范围为 C1 回归、生产 launch 替身和本机 A5 空 kernel 安装/注册/64 核提交/同步，
不包含通信或业务数据验证。

C3 交付 commit 为 `b68b56e02e1f49975549515f2d7f805ee4e1e0c8`；用户回传：
`COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p1-dry-run`。
报告路径：`/home/h00580772/TileXR-git-check/run/validation/commit-7fdca463600a4770aca68a6ef620ad67/summary.json`。
按用户回传登记通过，未取得 summary 内容，实际运行 SHA/工具链未独立核对；报告目录不是 Git SHA。
范围为 C1 回归、同一 consumer 三种算子模式、独立量化开关和 rank 映射；
没有 CANN/SSH/设备分配/communicator/launch 证据。

C4 交付 commit 为 `c86e1efe8fd598119112074df420fc1cce7ee4aa`；用户回传：
`COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p1-chain-single`。
报告路径：`/home/h00580772/TileXR-git-check/run/validation/commit-a710cc4e536641cda61bd27e23f73487/summary.json`。
按用户回传登记通过，未独立读取 summary 或核对实际运行 SHA/工具链；报告目录不是 Git SHA。
范围为 C1/C3 回归、Host 生命周期、50μs 空 kernel，以及前两台固定 8P 的六个串行单轮
Combine 独跑/Dispatch→空 Combine 用例；验证同步、输入/yOut 保持、五表/count 连接及各自 magic 选窗，
不包含 Combine 通信或 yOut 业务数值验证。

C5 交付点 `ep-sdedup-combine-p1-chain-multi`；入口仍为 `bash run_commit_validation.sh`。
C5 连续提交 warmup/正式轮次，批末同步，分别检查各算子计数和 magic/窗口；保留空 Combine 50μs 等待。
**C5 待验证**：交付 `4ef0439640b016727287bebec84ef5cc0a521445`。2026-09-21 用户通知验证环境暂不可用，
授权继续 C6/C7 并要求分批提交，后续切换目标 commit 执行同一入口。本授权仅调整开发顺序，不代表 C5 通过。
当前开发 C6 `ep-sdedup-combine-p1-profile`，C7 将单独提交；两点设备验收同样等待服务器恢复。


已建立 [Dispatch / Combine 联合设计初版](specs/ep-memory-server-dedup-dispatch-combine-design.md)，
已确认五表与 Combine 27 参数设计接口：保留 Memory 原 24 参数，只追加 3 表，
TokenNum 读取 sendCounts 末项。M1–M5 用户细化补充已完成；Combine 独立设计进入整体通读和分级收口，M0/M6 衔接一并纳入。
原 24 项中 CB00-01 已确认：接口含义沿用 Combine Memory，Host 封装、GM TilingData 和 launch
形式参考 Dispatch Server Dedup；公开 27 参数不变，设备所需标量通过独立 tilingData 传递。
CB00-03 已确认：首版仅支持有权重、非量化输入/通信，保留 quantMode 参数但仅接受 0；未实现特性只接受关闭配置。
量化相关子问题已移至 [Combine 附录 A 遗留项](specs/ep-memory-server-dedup-combine-design.md#deferred-quantization)，后续启动量化设计时再讨论，不进入首版 UB 预算或验收。
表 3/5 的路由 weight 保持必需；expertScales 是表 5 接口名，不是量化 scale。
CB1A-01 跨模块 UB 复用暂缓至各模块 UB 用量明确后讨论。CB1A-02 确认 Host 首次清零、kernel 消费后当场清零；
prepareDone/serverReady 的清理 owner、空任务和分核变化覆盖已梳理，M5 数据区 Flag 初始状态及清理完成依赖仍需收口。
[CB00-02](specs/ep-memory-server-dedup-combine-design.md#cb00-02) 布局已确认：Dispatch 控制区、Combine 控制区、共用数据区依次 512B 对齐，前后半区各保留一份，第二份起点为 peerMemBytes/2；共用数据区按 max(Ddata,Cdata) 核算。
控制区以公共宏解耦：Dispatch 764KiB（当前固定表区占用 574KiB），Combine 52KiB（已设计上限 26KiB 的两倍），
合计 816KiB；其后留空 208KiB，每份 buffer 的共用数据起点固定为 +1MiB（1048576B）。
公共宏和 Dispatch 布局已落实到源码，Combine C1 已接入固定区与参数化地址助手，真实数据面尚未实现；双方仅校验各自使用量，核算见[联合设计 §6.4](specs/ep-memory-server-dedup-dispatch-combine-design.md#64-固定控制区宏及上限核算)。
Combine 控制小表独立 offset 宏已确认：prepareDone=0、serverReady=2048、扩展预留=26624，均相对 Combine 控制区并按 512B 对齐。
两表固定预留 2KiB/24KiB，BS 或分核变化只改变有效范围，不移动表边界；Dispatch 内部固定 offset 的源码改造由独立专题维护。
Combine 接收区为 BS*K 个 DataAsFlag 槽，中转区紧接其后，为 E*BS 行原始 BF16/FP16，行 stride=alignUp(H*2,512)。
Sresult 为选定通信格式后推导的完整槽大小；ceilDiv(2H,480)*512 仅为无附加字段的 BF16/FP16 非量化返回示例，非量化 partial 返回格式及舍入仍归 CB03-02/CB04-03，量化格式移至附录。
按单份完整需求分别检查两个半区，统一容量上限为 peerMemBytes/2 减去 runtime 保留区；当前 run.sh chain 每卡显式分配 8GiB，Combine 的新布局消费逻辑尚未实现。
[待确定汇总表](specs/ep-memory-server-dedup-combine-design.md#待确定汇总表)仍有 20 项包含待确认内容（含暂缓的 CB1A-01）：
高风险 9 项、中风险 8 项、低风险 3 项；无参考 1 项与风险交叉，合计 10 项建议重点核对。
各模块分别记录已确认项、待确定项、推荐和代码依据，推荐尚未自动视为用户确认。
量化子问题迁出后，原条目仍各有非量化工作，待定总数保持 20；附录 Q01–Q06 不计入首版汇总。
Combine 最近一次通读已同步 M0–M5 固定宏/半区地址，区分控制区固定预留与有效 Flag 范围，
明确 stage 原始行与 result DataAsFlag 槽的不同 stride；已确认项不重复放回待选方案，仍按现有 20 个待定 CB 编号继续补充。
最新约定为固定 64 核，全部先执行 Init、CopyLocalPendingTokens 两个函数；随后分为 Send/Read 核，
Send 核直发类型 1，Read 核拉取类型 2 成员、加权聚合并发出 partial。最终 BS 个 Token 的聚合
独立为 M5，首版由 Read 核负责，任务映射与核类别解耦，预留后续 64 核参与。
M2 由核 56..63 汇总本卡 sum=64.0f 后通知各卡；各 Read 核独占 512B 卡间 Flag 块，
取 8 个槽首 float 求和为 8.0f 后就绪并自行清理。recvCoreNum=8*N（N=1..6）。
CB02-02 已确认：每个整体 buffer 的 Combine 控制区只放一份 M2 完成 Flag，随本次 Combine 整体选窗使用，
不设独立双 bank/翻转状态。两份 serverReady 固定预留 48KiB（有效 8..48KiB），两份 prepareDone 共 4KiB，已知 Flag 有效范围合计 12..52KiB。
M3 由各 Send 核用 Compare＋GatherMask 密排类型 1 原始行号，再按 sendCoreNum 商余均分；
可整除时任务数相同，否则最多相差 1，发送地址仍取表 2 的原始行。
表 2 仅缓存本核列表行号所覆盖的连续区间，按原始 row 减缓存首行索引，不要求整表搬入 UB。
M4 同样密排类型 2 行号，按 recvCoreNum 商余均分汇聚转发任务；表 2/3 仅缓存本核行号范围，
表 3 提供成员及加权信息，表 2 提供返回目的与成员寻址所需的原始 source，尚未实现。
M5 按 BS 整 Token 分核，预载本核表 4/5 区间，逐 Token 轮询，未到齐即检查下一个。
M5 DataAsFlag 清理已确认移至本核全部聚合任务结束后，单独清理本核连续 Token 段的全部 K 槽、全部 Flag 区，不筛选 mask。
每 Token 一次 DataCopyPad 清理 K*N 个 Flag，for 循环中只推进目的 GM 地址，Token 间不插同步；所有调用共用 K*N*32B 零值 UB，循环前完成置零依赖，末尾统一确认 MTE3 写回完成。
具体 UB 复用区间仍随 CB1A-01 汇总，目标块数限制按单 Token 的 K*N 核对。
M5 已改为按 mask 置位的副本逐个提交多块 Flag 拷贝，UB 按 32B 块密排，统一等待后
用一次 ReduceSum（mask=1、repeat=B、stride=1）累加各块首 float，B=N×有效槽数。
用户确认总线协议保证完整性；单次 probe 为 m 次拷贝、32*m*N 字节，实际代价待设备验证。
Combine 新中转区的分配归属、区域顺序、行对齐及整体双 buffer 已确认；内部实现细节继续收口，Dispatch 五表 ABI 和新包尾布局已实施。

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
| Combine 选窗与复用 | 每份 buffer 数据起点为 +1MiB（控制区及留空），整体双 buffer 步长为 peerMemBytes/2；公共布局已落地，Combine 待实现；跨调用安全仍按模型调度保证，M2 每窗一份 Flag 随整体选窗使用，不新增公共轮次参数或读完握手 |
| Combine 设计推进 | M1–M5 细化补充完成，已通读并分级；CB00-01 接口组织、CB00-02 布局、CB02-02 M2 单份配置已确认，CB00-03 加权与关闭配置策略已确认，剩余 20 项含待收口内容，M0/M6 衔接同步核对 |

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
| 公共 API / Host / layout | C1 用户回传 COMMIT_VALIDATION_PASS；外部详细环境未提供 | 交付 `01c9b1d` |
| 空 kernel / 注册 / launch | C2 用户回传 COMMIT_VALIDATION_PASS；报告路径见上文 | 交付 `3b27062` |
| 三模式配置 / dry-run | C3 用户回传 COMMIT_VALIDATION_PASS；报告路径见上文 | 交付 `b68b56e` |
| 单轮接线 | C4 已实现，用户外部验收待回传 | 待验收后登记 |
| 单轮 / 多轮框架 | 未开始 | — |

## 阶段 4：Combine 算子逻辑

| 检查方向 | 状态 | 通过 commit |
| --- | --- | --- |
| expert 返回 | 未开始 | — |
| group 归约 / partial 返回 | 未开始 | — |
| source 归约 | 未开始 | — |
| correctness / 性能验收 | 未开始 | — |

历史 source-send 阶段说明（已由完整 Dispatch 取代）：当时交付 `ep-sdedup-p2-source-send`，支持第一跳 + DIRECT 的同域连续多轮。默认仍为 16P/L6/BS96/H7168/K6/BF16/balance_shift/seed2026、每 rank 4 GiB、loops=1/warmup=0；64P/E384 的新增通过范围见上文。完整 Dispatch 多轮复用仍待接收清理契约。
