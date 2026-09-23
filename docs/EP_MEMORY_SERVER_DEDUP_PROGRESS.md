# EP Memory Server Dedup 进度

## 当前状态（2026-09-21）

Dispatch 优化设计阶段收口，当前规格见[主设计](specs/ep-memory-server-dedup.md)，
五表改动前设计提交为 `3b0ba92`。完整 Dispatch 与五表输出已实现，接下来完善 [Combine 设计](specs/ep-memory-server-dedup-combine-design.md)；
Server Dedup Combine 的 C1/C2 已收到用户 `COMMIT_VALIDATION_PASS` 确认；C3 也已收到用户通过确认；C4 单轮接线已收到用户通过确认；C5 连续多轮已收到用户通过确认；C6 独立 profiling/trace 已收到用户通过确认；C7 双窗边界及 Dispatch 回归已收到用户通过确认。Combine 第一阶段 C1–C7 已全部按用户回传登记通过。
当前[Combine 第二阶段模块验收](EP_MEMORY_SERVER_DEDUP_VALIDATION_FLOW.md#combine-phase2-modular-acceptance)：P2.1 已按用户回传通过；P2.2（M3）已按用户回传通过 16/16；P2.3（M4）已按用户回传通过 16/16；P2.4（M5）已按用户回传通过 24/24，覆盖最终 yOut、生产结果 Flag 清理及完整多轮。第二阶段 P2.1–P2.4 已按既定范围全部验收通过。
P2.1 开发验证：blue CANN 9.1 A5 OFF/ON 实际构建成功，安装包 Host 各 14/14；Python 验收工具 329 项中 328 通过、1 项 Windows 专属检查跳过。P2.1 的 23 个串行 16P 用例已按用户回传登记通过；当前一键入口切换为 P2.4 的 24 例单轮/两轮/多轮验收，详见[验收记录](EP_MEMORY_SERVER_DEDUP_VALIDATION_FLOW.md#combine-phase2-modular-acceptance)。
下文 Dispatch 阶段 1/2 的表格和日期记录属于历史交付证据，不作为当前实现缺失清单。

**P2.1 已验收（用户回传，续跑汇总）**：用户回传
`COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p2-m12`。
单轮 F1、多轮 F2、性能 P、耗时拆解 B 各 4/4，Dispatch 回归 2/2、量化接线 2/2、边界 3/3，合计 23/23。
总报告：`/home/h00580772/TileXR-git-check/run/validation/commit-346c04fba7a244a8adec8cddc1b516ab/summary.json`。
末例 B-both-fp16：`/home/h00580772/TileXR-git-check/run/validation/commit-346c04fba7a244a8adec8cddc1b516ab/cases/B-both-fp16/11d5d5a9b0cb4d8bb4407c88ff5145d9/summary.json`。
本轮按续跑交付 `4ffc700` 关联登记：复用原 OFF 19 项通过证据，补齐原 ON 包的 Host 检查和 4 项 breakdown；
原报告为 `/home/h00580772/TileXR-git-check/run/validation/commit-a222916af636494c87f86100f24c6004/summary.json`。
算子实现交付为 `acb826b`；`15274db` 排除编辑器临时文件，`4ffc700` 增加显式续跑，后二者未修改 Host/kernel。
本次未独立读取外部 summary、trace、实际执行 SHA、环境身份或产物哈希；报告目录为 run 标识，不是 Git SHA。
沿用续跑记录的历史 OFF/ON 源码摘要差异，不将本结果表述为两包摘要一致或当前 checkout 重新构建后的全量运行。
通过范围：M0/M1/M2 的 stage 原始数据、控制通知/消费清理、保护区和各自 magic/选窗，以及保留的 50μs 等待；
不包含 M3–M5 返回/归约、yOut 业务数值，不据四项性能模式通过推断具体性能指标达标。
P2.4（M5）已验收通过：功能覆盖 1/2/普通多轮，性能与 breakdown 恢复多轮，结果 Flag 由生产 kernel 清理；详见[当前入口及开发证据](EP_VALIDATION_GUIDE.md#当前入口combine-p24m5)。

### P2.4 设备验收已通过（用户回传）

用户回传 `COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p2-m5`，共 24/24：
F1、F2、Fmulti、P、B 各 4/4，Dispatch regression 与量化 connection 各 2/2。
结果：`/home/h00580772/TileXR-git-check/run/validation/commit-88e6f60b915444958a04f6149a354671/summary.json`。
对应本轮交付提交 `cc0477d9934caa3123ab30718f432eaa746de629`；目录后缀为验证运行标识，非 Git SHA。
按用户回传登记，未在开发环境重新运行设备测试或读取远端完整报告。
通过范围：完整 M1–M5、单轮/双轮/普通多轮、最终 yOut、生产结果 Flag 清理、性能采集和模块 breakdown；
不保留固定等待、不含性能阈值，不据采集通过推断性能达标或任意 shape/轮数均已验证。
P2.1–P2.4 已全部按各模块既定范围验收通过，Combine 第二阶段收口；量化和跨 shape 复用等专题遗留不因此关闭。

### P2.3 设备验收已通过（2026-09-21，用户回传）

用户回传 `COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p2-m4`：
F/P/B 各 4/4，Dispatch regression 2/2，量化 connection 2/2，共 16/16。
结果：`/home/h00580772/TileXR-git-check/run/validation/commit-c44caafd510d495e9c2eb67e5cef2604/summary.json`。
目录后缀为验收运行标识；M4 独立交付提交为 `f770c82`。
通过范围为 M1–M4 无预热单轮、DIRECT/weighted partial payload/Flag、功能/性能/模块 trace，
无 50μs 等待；不包含 M5、最终 yOut 或完整多轮复用。M5 单独按 P2.4 验收。


### P2.2 设备验收已通过（2026-09-21，用户回传）

用户回传 `COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p2-m3`：
F/P/B 各 4/4，Dispatch regression 2/2，量化 connection 2/2，共 16/16。
结果：`/home/h00580772/TileXR-git-check/run/validation/commit-f59416872bdb49aa9387c398c779c522/summary.json`。
该目录后缀是验收运行标识，不据此推断 Git commit；M3 交付提交为 `fe87c3c`。
通过范围为 M1/M2/M3 无预热单轮、DIRECT payload/Flag、模块性能/trace，无 50μs 等待；
不包含 M4/M5、yOut 算术或完整多轮复用。M4 按独立 P2.3 提交和验收。



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
**C5 已验收（用户回传）**：交付 `4ef0439640b016727287bebec84ef5cc0a521445`。
用户回传 `COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p1-chain-multi`，末例
`[8/8] PASS both-fp16-mxfp8-e5m2`，总报告：`/home/h00580772/TileXR-git-check/run/validation/commit-22f77fd2236149a09405863e25bc9543/summary.json`。
末例报告：`/home/h00580772/TileXR-git-check/run/validation/commit-22f77fd2236149a09405863e25bc9543/cases/both-fp16-mxfp8-e5m2/3e555c194a314f998419f03601195426/summary.json`。
按用户回传登记通过，未独立读取 summary 或核对实际运行 SHA、工具链及产物哈希；报告目录为 run 标识，不是 Git SHA。
范围：C1/C3/C4/C5 Host 回归及 50μs 空 kernel；前两台固定 8P，三种算子模式共 8 个串行用例，
每例 2 轮预热 + 5 轮正式调用，验证批末同步、各自 magic/选窗和最终 Dispatch 校验；
不包含 Combine 通信或业务数值验证，也不替代 C6 profiling 或 C7 边界验收。
此前用户因环境暂不可用授权提前开发 C6/C7，现按独立验收结果更新 C5 状态。
**C6 已验收（用户回传）**：交付 `abee43c70502de88c18ddd0700b0afde528ecc93`。
用户回传 `COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p1-profile`，末例
`[6/6] PASS breakdown-both`，总报告：`/home/h00580772/TileXR-git-check/run/validation/commit-06f978880a8340f78a48a51176ad30a6/summary.json`。
末例报告：`/home/h00580772/TileXR-git-check/run/validation/commit-06f978880a8340f78a48a51176ad30a6/cases/breakdown-both/8e43ca849ded4d3d85d940acfd2356e1/summary.json`。
按用户回传登记通过，未独立读取 summary、trace 或核对实际运行 SHA、工具链及产物哈希；报告目录为 run 标识，不是 Git SHA。
范围：独立 profiling OFF/ON 构建，各跑 dispatch/combine/both，共 6 组 16P，
每组 2 轮预热 + 5 轮正式调用，验证各算子独立测量/采集及 trace。
空 Combine 保留 50μs 等待，算法字节 0、算法带宽 N/A；不包含 Combine 通信或业务数值，也不替代 C7 边界验收。

**C7 已验收（用户回传）**：交付 `5ce1a927802a05ab5455a55ec30fe449d99e3d0d`。
用户回传 `COMMIT_VALIDATION_PASS checkpoint=ep-sdedup-combine-p1-memory-boundary`，末例
`[3/3] PASS dispatch-fp16`，总报告：`/home/h00580772/TileXR-git-check/run/validation/commit-93bd127f77944513acd404a57c244c76/summary.json`。
末例报告：`/home/h00580772/TileXR-git-check/run/validation/commit-93bd127f77944513acd404a57c244c76/cases/dispatch-fp16/82411664c32b42b1ba2c53a3e43b51c3/summary.json`。
按用户回传登记通过，未独立读取 summary 或核对实际运行 SHA、工具链及产物哈希；报告目录为 run 标识，不是 Git SHA。
范围：C1–C6 Host 回归及边界探针；前两台固定 8P，共 16P，独立 8GiB 双窗八点探针、
BF16/FP16 真实 Dispatch 多轮回归共 3 例，保留真实 Dispatch 输出/五表/Flag 校验，避开 runtime 保留区。
不包含 Combine 跨卡通信、ready/Flag 协议、yOut 业务数值或超出当前 shape 的业务容量验证。

**第一阶段收口**：C1–C7 均已获用户通过确认，公共接口、空 kernel、三种测试模式、
单轮/多轮接线、独立 profiling/trace 和双窗边界已完成各自范围的验收。
第一阶段交付时 Combine 为保留 50μs 等待的空算子；真实非量化业务逻辑属于第二阶段，Flag 复用继续作为重点遗留单独处理。
第一阶段 C7 提交的 `run_commit_validation.sh` 为 C7 入口；复验需切换各自交付提交，C7 的 Host 回归不替代此前各点的独立设备验收。

**结构对齐后测试通过（用户确认）**：最近交付 `53f33139a227e2603e0ba15cae1f5bf69cb07780`，
用户回传“测试通过”。该提交将 Launch 声明归入 launch.h、将空 Kernel 拆为同名头文件内的 Init/Process，
保留 50μs 与独立 profiling；本次回传未附 summary、实际运行 SHA、模式/环境或日志，按最近交付关联登记，
不推断新增业务数据面、具体矩阵或布局升级已通过。

**第二阶段计划**：四个独立模块交付点 `ep-sdedup-combine-p2-m12/m3/m4/m5`，按最新 CB06-04 的模块适用范围验收后推进下一点。
M1/M2 全核参与前置逻辑，阶段末尾保留 50μs；M3 起所有核移除该等待。M0 最新 96KiB 控制区、完整 result/stage
容量检查纳入首点，M6 依赖/排空随模块实施，不延后到最后。原固定矩阵不再作为后续各点统一门槛，已有 P2.1 开发记录保留。
首轮按最新 CB06-03 已确认的 32/32 独立编译期配置；M3/M4 只验无预热单轮数据/Flag 到达，不清理结果 Flag、不引入测试侧清理助手。
上述为初始拆解记录；当前 P2.1 已实现并切换一键入口，后续模块尚未开发。


已建立 [Dispatch / Combine 联合设计初版](specs/ep-memory-server-dedup-dispatch-combine-design.md)，
已确认五表与 Combine 27 参数设计接口：保留 Memory 原 24 参数，只追加 3 表，
TokenNum 读取 sendCounts 末项。M1–M5 用户细化补充已完成；Combine 独立设计进入整体通读和分级收口，M0/M6 衔接一并纳入。
原 24 项中 CB00-01 已确认：接口含义沿用 Combine Memory，Host 封装、GM TilingData 和 launch
形式参考 Dispatch Server Dedup；公开 27 参数不变，设备所需标量通过独立 tilingData 传递。
CB00-03 已确认：首版仅支持有权重、非量化输入/通信，保留 quantMode 参数但仅接受 0；未实现特性只接受关闭配置。
量化相关子问题已移至 [Combine 附录 A 遗留项](specs/ep-memory-server-dedup-combine-design.md#deferred-quantization)，后续启动量化设计时再讨论，不进入首版 UB 预算或验收。
表 3/5 的路由 weight 保持必需；expertScales 是表 5 接口名，不是量化 scale。
CB1B-01 已移至 [附录 C 大行数专题遗留](specs/ep-memory-server-dedup-combine-design.md#deferred-large-token-num)：第一阶段每卡实际 TokenNum 不超过 10000，不设计超限两遍筛选/密排方案；MaxTokenNum 和中转区容量公式不变。
CB1B-02 已确认并关闭：表 2 采用单份有界缓存，按本核 type3 原始行号范围分批读取完整 12B 行，以 row-cacheStartRow 查询 offset；不因任务少就假定行号跨度小。payload 仅拷贝 H*2B 有效数据，512B 对齐产生的 stage 行尾既不写入也不读取；MTE2/MTE3 共用环形 lane，按扣除固定资源后的剩余 UB 及 Event 上限配置，轮转跨表缓存批次连续推进。仅在再次搬入对应 lane 前等待其上次 MTE3 释放，搬入完成后再写出；本核全部 payload 写出排空后发布完成 Flag。具体 UB/Event 预算归 CB1A-01，实际重叠和多轮正确性待设备验证。
空任务同步责任明确：全部 64 核无 type3 拷贝任务也发布本核八份完成 Flag；所有 Read 核无接收任务也必须消费自身 serverReady 的 8 个 Flag 并清理，通知核仍完成汇总和通知。任务为空只跳过数据循环，不能免除控制区消费清理，否则残留 Flag 会影响后续复用。此为设计约束，尚未验证实际 kernel。
CB02-03 已确认采用推荐方案：完整 sum 后保存状态，由通知核清自身 2048B prepareDone 行、Read 核清自身 512B serverReady 块；通知/清理/DMA 分别建依赖，M6 排空后复用，保留设备验证。
CB1B-03 已确认并关闭：两级控制 Flag 统一占 32B、首 4B FP32=1.0f 有效，发布完整 32B；不同监听核区域互不重叠，起点按 512B 对齐且间距至少 512B。
卡内 prepareDone 为 8×2048B，生产核 c 向每行 c*32B 写入，单核 8 个目的起点间距 2048B，UB 连续 256B，DataCopyPad 块间空隙为 2016B。
核 56+j 独占第 j 个 2048B 区，只累加 64 个槽首 float，sum=64 后保存状态并清自身完整区域。卡间按 64 个 512B 块预留，当前实际 R≤48；每个 Recv 核按组内 LocalID 监听自身块，每块前 256B 为 8 个 32B 来源槽、后 256B 空闲。
卡间通知已确认：来源卡 s 的核 56+j 向目标卡 j（包括本卡）的 R 个块写 s*32B 槽；UB 连续 R*32B，一次 DataCopyPad 使用 R 块/32B/480B 间隙，源 UB 写完才复用。
每个消费者只累加 8 个槽首 float，sum=8 后保存 ready 并清自身 512B 区。64 块只是 GM 预留，不扩大分核范围；CB02-01 的统一检测流程与首版独立 UB 已确认。
CB02-01 已关闭设计待定：卡内一次读 2048B、卡间一次读前 256B，统一 MTE2→Vector、ReduceSum(mask=1,repeat=B,stride=1)、Vector→Scalar，B 分别为 64/8。
未到齐重新读取，完整到齐后保存状态再清理；检测输入、work、sumOut、清理零值区和通知源首版独立分配，两级之间也不复用。work 初步预算为 256B/32B，目标尾部余量及对齐进入实现验证。
此独立 UB 决定仅针对 M2，M5 仍按 CB05-03 复用检测 UB。总预算归 CB1A-01，目标编译、并发邻槽保护和多轮清理验证归 CB06-04。
CB1A-01 已确认首版 M4/M5 保留 PIPE_ALL，UB 使用生命周期、EventID 和流水状态按阶段独立管理；屏障位于 M5 初始化、加载表或覆盖复用 UB 之前。UB 按常驻区、公共聚合区、阶段专用区规划物理复用，具体容量、offset、lane 配额及 Event 分配仍待核算，未关闭本项。屏障不代替 Event 配对收尾，M2 独立缓冲及 M5 probe 转清理零值源的既定规则不变。
后续移除 PIPE_ALL、延续公共 lane/Event 或更深度融合仅记入 [Combine 附录 D 后续优化方案](specs/ep-memory-server-dedup-combine-design.md#deferred-m4-m5-fusion)：全部功能完成后，只有性能专项确认此处存在性能问题才评估，首版不要求实现或增加融合开关。
CB1A-03 已关闭设计待定，按 Dispatch 参考落实阶段内 Event/Reset 依赖，不要求 M1a 统一预置未使用的 lane 或寄存器状态；首次先 Wait 的环形 lane 仍需对应初始事件。
CB1A-02 当前初始化方案已关闭设计待定：Host 首次清零、kernel 按模块职责用后清理，接线保留实现验证；
结果 Flag 跨布局/已用域复用仍为附录 B 重点遗留，M2 按 CB02-03 已定依赖实现，整体排空按 CB06-02 已确认方案执行。M1a 仅剩 CB1A-01，现已进入讨论，未标记全部资源设计完成。
[CB00-02](specs/ep-memory-server-dedup-combine-design.md#cb00-02) 布局已确认：Dispatch 控制区、Combine 控制区、共用数据区依次 512B 对齐，前后半区各保留一份，第二份起点为 peerMemBytes/2；共用数据区按 max(Ddata,Cdata) 核算。
控制区以公共宏解耦：Dispatch 764KiB（当前固定表区占用 574KiB），Combine 96KiB（固定预留需求 48KiB 的两倍），
合计 860KiB；其后留空 164KiB，每份 buffer 的共用数据起点固定为 +1MiB（1048576B）。
Dispatch 布局及原 Combine 公共宏已落实到源码，C1 已接入原固定区与参数化地址助手；
本次 32B Flag 升级后的 Combine 96KiB/16KiB/16384B 宏及地址助手尚未实现，当前源码仍为 52KiB/2KiB/2048B，既有 C1–C7 验收不覆盖新布局。双方仅校验各自使用量，核算见[联合设计 §6.4](specs/ep-memory-server-dedup-dispatch-combine-design.md#64-固定控制区宏及上限核算)。
Combine 控制小表独立 offset 宏已确认：prepareDone=0、serverReady=16384、扩展预留=49152，均相对 Combine 控制区并按 512B 对齐。
两表固定预留 16KiB/32KiB，BS 或分核变化只改变有效范围，不移动表边界；Dispatch 内部固定 offset 的源码改造由独立专题维护。
Combine 接收区为 BS*K 个 DataAsFlag 槽，中转区紧接其后，为 E*BS 行原始 BF16/FP16，行 stride=alignUp(H*2,512)。
CB03-02 已确认沿用 Dispatch 非量化打包/发送流水，去掉包尾，DIRECT/partial 均以 BF16/FP16 返回；Sresult=ceilDiv(2H,480)*512，槽间无额外间隔。
CB04-03 已确认 M4/M5 均先转 FP32 加权累加；M4 全部贡献累加完成后转回 BF16/FP16 转发，M5 最终转为目标 dtype 输出。partial 在表 5 的权重为 1，精度 oracle 复现中间转换。
CB04-02 已确认并关闭：M4 聚合参考 Memory，输入 lane 按扣除固定工作区及独立输出缓冲后的剩余 UB 扩展，不限于 1/2；MTE2/Vector 按 lane 生产消费，Vector 读完才允许输入覆盖。
输出采用独立环形 lane，仅在再次写该 lane 前等待其上次 MTE3 完成；FP32 累加区在最终转换读取结束后即可开始下一 Token，允许上一 Token 发送与下一 Token 聚合重叠。
不照搬 Memory ProcessExpert 的逐 Token 入口 MTE3→Vector 等待；阶段末仍按 CB06-02 排空后进入 M5。lane 配额及 Event 资源归 CB1A-01，回绕正确性按 CB06-04 的开发边界验收，重叠效果留到性能专项；本节设计确认不代表实现或设备验证通过。
按单份完整需求分别检查两个半区，统一容量上限为 peerMemBytes/2 减去 runtime 保留区；当前 run.sh chain 每卡显式分配 8GiB，Combine 的新布局消费逻辑尚未实现。
CB05-02 已确认并关闭：M5 每核一套 probe 缓冲逐 Token 复用，Flag 按完整 K*N*32B 加目标访问余量预留，work 按 alignUp(K*N*4,32)、sumOut 按独立 32B 预算。每个有效 k 一次多块搬运，统一 MTE2→Vector 后，一次 ReduceSum 只累加全部有效副本各 32B 块的首 float，预期为 N*popcount(mask)，再由 Scalar 判断。最后 probe 结束且本核全部有效副本已到齐后，将完整 K*N*32B 置零用于清理，不能只清零最后一次 probe 的有效子段；清理源保持至 MTE3 完成，480B payload 不受影响。具体总预算归 CB1A-01，设备验证仍待完成。
CB06-04 已确认并关闭：沿用独立模块正确性判据，M4/M5 oracle 包含中间低精度舍入。开发阶段确认 1/2/普通多轮功能；M3/M4 例外只验无预热单轮数据/Flag 到达，不清结果 Flag，不加测试清理助手，其 performance/breakdown 也为单轮。性能验收只确认数据可获取和模块级耗时/trace 解析正常，不设基线或达标线、不加内部细打点；输出、统计及校验失败仍导出产物沿用 Dispatch。压力测试在所有功能、性能达标后启动，例如单轮/双轮各重复 1 万次；当前不新增该类测试或脚本。既有 P2.1 开发证据保留，不作为扩张后续验收矩阵的模板。
CB03-01 已确认并关闭：M3 沿用单份有界表段缓存，按原始行号跨度切批。DataCopyPad 从 GM 的实际首行地址整段搬入，GM 不必 32B 对齐、UB 起点对齐；[22,66] 从 264B 偏移搬 540B，UB 预留 544B，内部行距保持 12B，以 (row-22)*3 索引 32 位三字段，仅段尾补齐。不能逐行按普通 padding 搬运后仍假定 12B 行距。换缓存批次不排空发送、不重置 lane 序号，模块结束按 M6 排空；UB 总预算归 CB1A-01，M3 开发仍仅验单轮。
CB04-01 已确认并关闭：M4 表 2/3 各一份独立 UB 缓存，共用批次边界及 baseRow，按两表可容纳行数的较小值切批；行距分别为 12B/K*8B，按 row-baseRow 索引。表 3 header.N 为 uint32，成员为 uint16 expert/q 和 FP32 weight；self 读 expertOut，其余成员含同卡均按联合 source/expert/q→slot 公式读 stage，仅读有效 H*2B，零权重仍正常处理。旧批次路由、索引和权重消费完才覆盖表缓存；换批不排空独立 partial 发送、不重置 lane，模块末尾仍排空。UB 总预算归 CB1A-01，本次未修改 kernel 或增加设备测试。
CB05-01 已确认并关闭：M5 用 UB uint32 pendingTokenIds 保存本核原始 TokenId 连续段，pendingCount 初始为任务数；未到齐检查下一项，到齐后 Scalar 完成相关参数处理和指令提交，即以末项覆盖删除、长度减一；删除不等待 Vector/MTE 执行完成，也不新增 Scalar 与流水同步，继续检查换入项，不保留完成状态表。表 4/5 仍按 p-begin 索引，不随 pending 交换重排。所有实际 Token 的 mask 至少一个有效 bit，取消空 mask 补零分支；空核仍履行 M2，TokenNum=0 不免除 BS 接收。最后剩一项且 probe 成功即可保存 ready、按原始 begin/end 启动已定清理，Scalar 提交流程结束即删除该项；pendingCount=0 不替代 Vector/MTE 的资源复用依赖和最终排空。本次仅设计文档确认。
[待确定汇总表](specs/ep-memory-server-dedup-combine-design.md#待确定汇总表)仍有 1 项包含待确认内容（CB1A-01，正在讨论）：
高风险 1 项、中风险 0 项、低风险 0 项；无参考 0 项，CB1A-01 的阶段隔离及资源分类已确认，具体预算仍在讨论。
各模块分别记录已确认项、待确定项、推荐和代码依据，推荐尚未自动视为用户确认。
量化子问题迁出后，本次再关闭 CB1A-02/CB1A-03 的设计待定，CB1B-01 超限场景迁出、CB1B-02/CB1B-03/CB02-01/CB02-03/CB03-01/CB03-02/CB04-01/CB04-02/CB04-03/CB05-01/CB05-02/CB05-03/CB06-01/CB06-02/CB06-03/CB06-04 确认后当前共 1 项；附录 Q01–Q06 不计入首版汇总。
Combine 最近一次通读已同步 M0–M5 固定宏/半区地址，区分控制区固定预留与有效 Flag 范围，
明确 stage 原始行与 result DataAsFlag 槽的不同 stride；已确认项不重复放回待选方案，按当前汇总中的 1 个待定 CB 编号继续补充。
最新约定为固定 64 核，全部先执行 Init、CopyLocalPendingTokens 两个函数；随后分为 Send/Read 核，
Send 核直发类型 1，Read 核拉取类型 2 成员、加权聚合并发出 partial。最终 BS 个 Token 的聚合
独立为 M5，首版由 Read 核负责，任务映射与核类别解耦，预留后续 64 核参与。
CB06-01 已确认：Send 在自身 M1b 发布后进入 M3；Read 完成 M2 后处理全部本核 M4，首版以 Event 收尾及 PIPE_ALL 排空后进入 M5，不等其他 Read 切阶段，首版不交替 M4/M5。
CB06-03 已确认并关闭：默认 Send/Recv=32/32，入口参考 Dispatch 编译期宏，设计新增独立 TILEXR_EP_SERVER_DEDUP_COMBINE_SEND_CORE_NUM，默认 32；合法发送核为 16/24/32/40/48/56，接收核为 64 减发送核。
同一 communicator 生命周期内所有卡保持相同且固定的 Combine 配置；更改后同步重编译部署 Host/kernel，不新增运行时公开分核参数或 tiling 字段，不做单卡自适应。该宏尚未落到源码；64 个 serverReady 块仍仅为预留。
核 56..63 先完成通知再监听 serverReady；M3/M4 写预留结果槽，不依赖目的卡先进入 M5 或消费。空任务核仍履行发布及 M2 责任。具体排空按 CB06-02 已确认方案执行，实际进度验证归 CB06-04。
CB06-02 已确认：M1b 中转写入完成后发布 Flag；M2 的通知/清理 UB 复用前等待对应写出，独立缓冲清理可在途；M4 全部读取、计算和发送排空后进入 M5。
M5 保留提前清理，退出前确认全部聚合、yOut 和 Flag 清理完成；Send 在自身发送及控制写出完成后独立退出，不新增全核结束屏障或 Server 读完握手。
Send 退出后 GM 中转数据保留给其他卡读取，跨轮复用仍由既定模型调度保证；Event/UB 具体接线归 CB1A-01 及模块实现，设备验证归 CB06-04。
M2 由核 56..63 汇总本卡 sum=64.0f 后通知各卡；各 Read 核独占 512B 卡间 Flag 块，
取 8 个槽首 float 求和为 8.0f 后就绪并自行清理。recvCoreNum=8*N（N=1..6）。
CB02-02 已确认：每个整体 buffer 的 Combine 控制区只放一份 M2 完成 Flag，随本次 Combine 整体选窗使用，
不设独立双 bank/翻转状态。两份 serverReady 固定预留 64KiB（当前有效 8..48KiB），两份 prepareDone 共 32KiB，已知 Flag 有效范围合计 40..80KiB。
M3 由各 Send 核用 Compare＋GatherMask 密排类型 1 原始行号，再按 sendCoreNum 商余均分；
可整除时任务数相同，否则最多相差 1，发送地址仍取表 2 的原始行。
表 2 仅缓存本核列表行号所覆盖的连续区间，按原始 row 减缓存首行索引，不要求整表搬入 UB。
M4 同样密排类型 2 行号，按 recvCoreNum 商余均分汇聚转发任务；表 2/3 仅缓存本核行号范围，
表 3 提供成员及加权信息，表 2 提供返回目的与成员寻址所需的原始 source，尚未实现。
M5 按 BS 整 Token 分核，预载本核表 4/5 区间，逐 Token 轮询，未到齐即检查下一个。
CB05-03 已确认：本核全部有效副本到齐、最后一次 probe 完成并保存 ready 后，直接复用 Flag 检测 UB 置零，提前清理本核全部 K 槽的 Flag，与剩余 payload 读取、聚合和输出处理重叠；不筛选 mask。
每个 512B 块只写末尾 32B，严格保留前 480B payload；后续不再 probe，即使整块搬入也不再消费 Flag。最后一个 Token 按实际到达进度定义，不按最大行号。
每 Token 一次 DataCopyPad 清理 K*N 个 Flag，for 循环中只推进目的 GM 地址，Token 间不插同步；所有调用共用 K*N*32B 零值 UB，循环前完成置零依赖，末尾统一确认 MTE3 写回完成。
检测 UB 按 K*N*32B 加 probe 所需余量预算，最后一次 probe 完全结束后切换用途，与 payload/累加/输出区独立，清理完成前不可覆盖或释放。
CB05-03 设计待定关闭；具体 UB offset/Event 随 CB1A-01 汇总，M6 排空按 CB06-02 已确认方案执行，目标 API 范围和重叠清理不破坏 payload 的设备验证归 CB06-04。
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
| Combine 设计推进 | M1–M5 细化补充完成，已通读并分级；CB00-01 接口组织、CB00-02 布局、CB02-02 M2 单份配置已确认，CB00-03 加权与关闭配置策略已确认，CB1A-02/CB1A-03 当前方案已收口，CB1B-01 超限场景已迁至附录 C，CB02-03 已确认，CB1B-02/CB1B-03/CB02-01/CB03-01/CB03-02/CB04-01/CB04-02/CB04-03/CB05-01/CB05-02/CB05-03/CB06-01/CB06-02/CB06-03/CB06-04 已确认，剩余 1 项含待收口内容，M0/M6 衔接同步核对 |

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
