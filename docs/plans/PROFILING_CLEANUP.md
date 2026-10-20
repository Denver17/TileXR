# Profiling 代码、分析与文档整改计划

日期：2026-10-20。状态：权威执行计划；I00 已复核，首期代码已实现并完成本地分层验证，尚未关闭的目标平台验收见第 9 节。

实际仓库根目录为 `C:\Git_data\tileXR`，位于用户指定的 `C:\Git_data`；本文所有相对路径和命令均以该仓库为准。第 1–8 节保留原始风险、固定基线证据与验收合同；其中旧路径属于历史基线。当前实现及证据统一维护于第 9 节，其状态优先于原始任务的“待实施”标签。

本文与 [Profiling 独立化设计及实施计划](profiling.md) 共同构成后续实施的两份维护入口。主设计定义 I00–I06、唯一公共 API、提交布局、生命周期和兼容边界；本文定义整改任务、风险触发条件、验收及关闭证据。接口以主设计为准，本文不另设签名或专用调用方式。

## 1. 基线与实施前置

用户确认主基线为 [`codex/TileXR_Profiling_Upgrade`](https://github.com/LingquLab/TileXR/tree/codex/TileXR_Profiling_Upgrade)，[`main`](https://github.com/LingquLab/TileXR/tree/main) 为对照。两条可移动分支链接用于导航；本计划的固定代码依据为 [efa5d80d87aea2eb60e4df770625a31236ee9a93](https://github.com/LingquLab/TileXR/commit/efa5d80d87aea2eb60e4df770625a31236ee9a93)。

| I00 取证项 | 本次结果 |
|---|---|
| 时间 | 2026-10-20 11:58（UTC+8） |
| 认证仓库 | `C:\Git_data\tileXR`；origin 为 `https://github.com/LingquLab/TileXR.git` |
| 远端验证 | 认证 HTTPS `git ls-remote --heads origin refs/heads/main refs/heads/codex/TileXR_Profiling_Upgrade` 返回两条相同完整 SHA，均为上述固定提交 |
| 本地 Git 状态 | 工作分支 main，`git status --short` 为空；HEAD 及两条 origin 跟踪引用同为上述 SHA |
| 主/对照差异 | 本次观察的 Upgrade tip 与 main tip 相同，提交差异为空 |
| 固定对象归档 | 会话比较目录中的非交付缓存，由固定 Git 对象 archive 导出；SHA256 为 `5352f96305f0402f7e1edaf02b59c99ff49c18d5da79f99d1d15c1cec640e95e`。真实仓库无需存在该缓存，不作为未来文档/构建/执行输入 |
| 本地源码匹配 | 归档同名解压目录中的 315 个源码/工具/测试/scripts-ci 及关键 AGENTS/CMake/README/BUILD/CI 文件，忽略 CRLF 后与当前工作目录一致，differences=[] |
| 操作边界 | 基线阶段只读 refs/对象，未 checkout、merge 或 fetch；计划交付仅新增两份 Markdown，实现代码/分支/提交不变，比较缓存不交付 |

**I00 是所有实现任务的硬前置，本次已关闭该前置。**下面的相对源码链接指向已与固定对象匹配的工作目录，不单凭旧文档的历史 fetch 记录建立可信基线。后续开工时核对工作区与固定 SHA；若远端或实际实施提交变化，先更新取证和对应风险，避免重复修复或沿用失效行号。I00 完成不代表 I01–I06 或任何代码整改已完成。

本次在固定对象比较快照运行现有全量测试：63 项/2.478 s 通过；随后又在实际 `C:\Git_data\tileXR`，用 Python 3.12.14 运行 `python -B -m unittest discover -s tests/profiling/python -p 'test_*.py'`：**63 项，2.279 s，全部通过**。覆盖的是现有 Python 行为，不关闭任何尚未实现的整改项。本次没有新增实现、CANN 编译或硬件运行；离线测试不证明设备提交、开销或独立化完成。

### 1.1 任务状态与取证规则

| 状态 | 使用条件 |
|---|---|
| 待实施 | 尚无实现，或等待上游合同；本文 C/A/T/D 一期任务当前均处于此状态 |
| 实施中 | I00 已确认且开始修改拥有文件；附负责人和当前依赖 |
| 待验证 | 修改已完成，但对应级别证据未齐；不能写“整改完成” |
| 已验证 | 本项修改、行为验证及维护文档全部具备当前证据，注明实际覆盖平台 |
| 延期 | E 系列增强，不阻塞本期；不表示已经支持或已有 bug 被修复 |

每项记录：基线 SHA/源码位置、触发条件、修改文件、依赖、验证命令与 cwd、工具版本、结果/日志位置、未覆盖边界、关闭日期。没有硬件时可以完成 Host 部分，设备部分继续标待验证；不得用文字更新或合成测试替代设备结论。不要把主机、设备清单、临时调度脚本或凭据提交进仓库。

### 1.2 分类与统一合同

- **条件性风险**：只在指定复用、分支或竞争条件下出现，不据此声称生产已经发生故障。
- **当前边界/不一致**：已匹配固定对象的代码、测试或文档能够直接核对出的事实；切换实施基线后需重新确认。
- **新增 API 验收**：统一 SDK 引入的新能力及资源合同，不能误写为现有代码缺陷。
- **增强**：超出一期已选范围，登记 E 编号并明确延期。

一期统一使用 `<tilexr/profiling.h>`、`TileXR::Profiling::AclCapture` 和 `DeviceArgs()` 返回的 GM descriptor。`Save` 是 `Collect` 与 `Export` 的便捷组合；所有使用者可用同样的分步 API。一个 capture 对应一次业务 Kernel 调用，重复测量 `Close` 后重新初始化并生成新 UUID；不提供 Reset，不保留解释 `CommArgs` 的适配 API。

所有活跃 TileXR 调用点及测试整体迁移。磁盘 `records.v1/mapping.v1` 保持兼容；旧内存布局、旧 profiling 类/方法签名、旧 `tools.profiling` 成功运行的 CLI/API 不属于兼容承诺。新的 Python 唯一实现位于 `tools/profiling/python/tilexr_profiling`，发行配置位于 `tools/profiling/pyproject.toml`，教程只使用 `python -m tilexr_profiling`。

## 2. 一期任务总表与依赖

表中依赖均隐含已完成的 I00；I 编号的完整定义见主设计。其他 I 项依赖表示它的合同或所需实现产物已可用，不是要求该 I 项全部验收先关闭。C/A/T/D 是必须关闭的一期任务，E 是明确延期的增强。任务 ID 保持稳定，后续拆分使用后缀，不能因重命名实现而删除问题记录。

| ID | 类型/期次 | 主要依赖 | 对应独立化范围 | 可分配修改范围 |
|---|---|---|---|---|
| C01 | 条件性风险，一期 | I01、C08 | I01/I04 | Recorder 有效槽及编译/设备用例 |
| C02 | 条件性风险，一期 | I01、C08 | I01/I04 | DirectWriter 初始化与状态 |
| C03 | 条件性风险，一期 | C02、C08 | I01/I04 | dropped、幂等提交、重复写入拒绝 |
| C04 | 契约规范化，一期 | C08 | I01 | 零时间戳与非法事件 ID |
| C05 | 契约规范化，一期 | I01/I02 | I01/I02/I04 | 编译/运行时启停 |
| C06 | 契约规范化，一期 | I01/I02、C08 | I01/I02 | rank/lane、容量及布局校验 |
| C07 | 条件性风险，一期 | I02 | I02/I03 | Host 文件编码和无覆盖发布 |
| C08 | 整体布局迁移，一期 | I01/I02 合同冻结 | I01/I02/I04 | producer、Collect、exporter、协议回归 |
| C09 | 新增 API 验收，一期 | I02、C07/C08 | I02 | AclCapture 资源、状态、context、失败路径 |
| C10 | 生产集成合同，一期 | C05/C08/C09 | I04 | 异步持有者、采集池及生产 launch 编排 |
| A01 | 时钟/文档一致性，一期 | I05、目标时间源证据 | I05/I06 | 模型、归零说明及现有测试 |
| A02 | 测量单位一致性，一期 | C08/C10 | I04/I05/I06 | 单 critical、warmup 和重复测量 |
| A03 | 完整性说明，一期 | C08、I05 | I02/I05 | 已知 lane 状态与输入证据范围 |
| A04 | 统计整改，一期 | A01、A02 测量合同 | I05 | union/exclusive 与 wait/sync pressure |
| A05 | 溯源一致性，一期 | I05、A01 | I05/I06 | 三产物共同 provenance |
| T01 | 测试整改，一期 | I01–I06 已有可测合同 | I01–I06 | 替换生产源码文本守卫 |
| T02 | 行为/故障验证，一期 | C01–C10、A01–A05 | I01/I02/I04/I05 | 单元、故障注入、协议与硬件测试 |
| T03 | 交付/CI，一期 | I03/I05/I06 | I03/I05/I06 | 独立构建、安装消费、Python 包和 CI |
| D01 | 协议参考，一期 | C08、A01–A05 | I01/I05 | mapping/records/提交协议参考 |
| D02 | 统一教程，一期 | I01–I06 | I01–I06 | 唯一接入流程及历史入口迁移 |
| D03 | 示例/平台边界，一期 | T02/T03、A01/A05 | I03/I06 | Add4、支持矩阵、开销与 provenance |

建议顺序：I00 → 冻结 I01/I02 的布局、状态、A02 单测量及异步调用合同 → C01–C09 与 Python A01/A04/A05 并行 → C10 迁移生产调用 → A02/A03 集成验收、T01–T03 → D01–D03 与最终验收。布局头、主 CMake、协议模型及教程入口各指定一个集成人；并行任务不得各自修改一套状态定义。

## 3. 采集与 Host 代码整改

### C01 固定槽 Recorder 表达未执行槽

**状态/期次：待实施，一期。依赖：I01、C08。类型：条件性风险。**

- 本地证据：[局部数组](../../src/include/tilexr_profiling.h#L119) 未初始化，[Flush](../../src/include/tilexr_profiling.h#L107) 读取全部模板容量；Add4 当前执行全部 20 槽，不能把它称为分支故障复现。
- 触发与影响：部分 marker 未执行但仍 Flush 时，旧路径可能读取未初始化的 ID/timestamp。整个 Kernel 提前返回、根本没有 Flush，则属于 C08 的未提交状态。
- 修改：构造时将有效 ID 初始化为 0；Flush 先判断 ID，仅有效槽读取 timestamp。按 Slot 递增将有效事件压紧到新布局 `[0,count)`，保留相同 timestamp 的 Slot 顺序；不自动给 marker 增加 barrier/GM 写入。槽顺序由调用者按实际事件顺序规划，重复覆盖同槽不是动态追加。
- 验收/关闭：覆盖全槽、条件跳槽、零有效槽、缺失 begin/end、重复同槽、边界 Slot；无未初始化读取且缺失边仍由分析器标 incomplete。设备编译检查构造开销与 spill；硬件核对压紧输出，不凭数组写法宣称全部在寄存器。

### C02 DirectWriter 重新初始化不保留旧对象状态

**状态/期次：待实施，一期。依赖：I01、C08。类型：条件性风险。**

- 本地证据：[Initialize](../../src/include/tilexr_profiling.h#L125) 没有重置游标/丢弃数/启用状态，[无效参数路径](../../src/include/tilexr_profiling.h#L129) 直接返回。当前 A5 生产创建新局部对象，未据此证明已经触发复用错误。
- 修改：每次初始化先恢复完整 disabled 状态；通过 descriptor、lane、state、地址和容量校验后再启用。新 capture 的写入使用新 writer；同对象 Publish 后不能靠再次 Initialize 绕过同 lane 的单提交合同。
- 验收/关闭：成功→成功、成功→失败、失败→成功分别核对目标、游标、dropped、状态；失败后不得写旧缓冲。非法 lane、空 descriptor、已提交 lane 均有明确定义；无需调用方手工重置内部字段。

### C03 dropped 及提交幂等与采集范围一致

**状态/期次：待实施，一期。依赖：C02、C08。类型：条件性风险和新合同迁移。**

- 本地证据：[Publish](../../src/include/tilexr_profiling.h#L204) 将本对象 dropped 赋给共享计数；旧多个串行 launch 在同一 capture 各自丢弃时，后一次可能覆盖先一次，而非累计总量。
- 修改：新合同每 lane 一次 writer 提交，dropped 按实际无法保留的有效事件饱和计数；同对象重复 Publish 幂等，不重复累加、不重新发布。新 writer 再次提交同一 lane 属协议错误，不覆盖已提交记录，按 C08 标记/拒绝。不得用原子或锁偷偷扩张共享 lane 并发范围。
- 验收/关闭：前缀保留、数次 RecordAt 超限、无丢弃、饱和上限、重复 Publish、新对象重复提交；报告 dropped 不少计、不二次累计、不伪装 complete。旧跨 launch 计数用例转换为新单 capture 用例，不能继续承诺旧多轮累积。

### C04 timestamp 0 合法，事件 ID 0 保留

**状态/期次：待实施，一期。依赖：C08。类型：契约规范化。**

- 本地证据：[RecordAt](../../src/include/tilexr_profiling.h#L174) 丢弃 timestamp 0，而 [records parser](../../tools/profiling/records.py) 允许零起点。
- 修改：DirectWriter 的时间戳值与事件有效性分离；timestamp 0 合法，ID 0 为非法事件并明确拒绝。不得静默把 ID 0 计为正常有效记录；具体设备错误状态遵循 C08，Host 得到可处理结果。
- 验收/关闭：零起点、同时间戳、非递增回填后稳定排序、最大 timestamp、ID 0 的 API→提交→导出→分析结果；不因新内存布局改变历史磁盘解析规则。

### C05 编译开关与运行时 disabled 分别定义

**状态/期次：待实施，一期。依赖：I01/I02。类型：契约规范化。**

- 本地证据：[宏判断](../../src/include/tilexr_profiling.h#L34) 使用 `defined`，值为 0 仍启用；这不是旧指南已承诺 0 可关闭而实现违约。
- 修改：统一 `TILEXR_ENABLE_PROFILING` 未定义/0 为编译关闭，1 为开启，非法值有诊断。编译开关不改变 Host 类型布局、业务 Kernel 参数或磁盘协议。
- 运行时 `enabled=false`：Initialize 不分配 workspace；DeviceArgs 返回 nullptr；Flush/DirectWriter 安全无操作；Collect/Export/Save/Close 成功无操作且不创建文件。已经编译开启的 marker 仍可能读时间戳，不能宣称 runtime disabled 等于零开销。
- 验收/关闭：未定义/0/1 设备编译、Host 多 TU 链接、空 descriptor、runtime disabled 的分配次数/文件结果；关闭版无设备记录数组/采集 GM 写入。生产开关同步覆盖 Host 与设备编译命令。

### C06 身份、容量和地址布局统一校验

**状态/期次：待实施，一期。依赖：I01/I02、C08。类型：契约规范化。**

- 本地证据：[stream ID 编码](../../src/include/tilexr_profiling.h#L29) 直接按位组合，[导出](../../src/comm/tilexr_profiling_runtime.cpp#L191) 将带符号 rank 转无符号；本地实际 TileXR 合法范围未必触发越界。
- 修改：保留磁盘 rank/lane 的高低 16 位编码。配置/导出入口拒绝负 rank 和不能编码的值；区分 lane 索引最大 65535 与 lane 数量上界，不能 off-by-one。layout 检查乘加溢出、地址/stride 对齐、ABI version/struct bytes、缓冲尺寸与内存预算。
- 修改边界：新 `count` 是有效记录数，可以不是 8 的倍数；只有 stride/布局对齐，不能沿用旧 occupied 对齐校验。AIC/AIV/subblock lane 映射必须由实际执行模式确定，不能普遍把 `GetBlockIdx()` 当物理核心或唯一身份。
- 验收/关闭：负 rank、0/65535/65536、无符号上界、lane 数量/索引边界、容量 0/极值、算术溢出、ABI/地址不合法；合法身份唯一，错误不产生越界访问。硬件指针转换和 Host/Device offset 另有 CANN 编译证据。

### C07 records 文件发布无竞争覆盖

**状态/期次：待实施，一期。依赖：I02。类型：条件性风险。**

- 本地证据：[检查目标](../../src/comm/tilexr_profiling_runtime.cpp#L278)、[固定 `.tmp`](../../src/comm/tilexr_profiling_runtime.cpp#L283) 和普通 rename 之间存在竞争窗口。不能把单进程“不覆盖已有文件”测试通过当作并发保证。
- 修改：纯 Host 编码与路径发布分开；同目录以 O_CREAT|O_EXCL 创建唯一临时文件，完整写入/同步/关闭后以 hard link 无覆盖创建目标，再清理本次临时名。文件系统不支持时明确失败，不退化为 check-then-rename；不假定 C++14 有 `std::filesystem`，不承诺断电后的目录项持久性。
- 写入、close 或发布失败只清理本次拥有的临时资源，已有目标不变，Snapshot 保留以便换路径重试。若目标已经发布、仅临时清理失败，返回“已发布、清理警告”，不能把它当未发布错误诱导重试覆盖。
- 验收/关闭：并发导出同一路径、目标已存在、临时名冲突、写入/同步/close/发布失败、文件系统不支持、不可写目录及已发布后的临时清理失败；最多一个完整目标成功，其余不破坏先到文件。保存故障注入与真实文件行为证据；Python report 原子写测试不能替代 C++ records 发布测试。

### C08 producer、Collect 与 exporter 整体迁移有效提交布局

**状态/期次：待实施，一期。依赖：I01/I02 合同冻结。类型：已有边界与明确的新布局迁移。**

- 本地证据：[旧 exporter](../../src/comm/tilexr_profiling_runtime.cpp#L195) 全容量扫描并 stable sort；[旧单测](../../tests/profiling/unit/test_profiling_runtime.cpp#L56) 以 occupied=0、槽 6/7 有效证明稀疏行为。该行为不是“扫描越界 bug”，不能只缩短 exporter 扫描范围。
- 新合同：每 lane 独占，状态为 EMPTY/WRITING/COMMITTED/INVALID；有效记录压紧到 `[0,count)`，count 是有效记录数，不要求 8 对齐；物理 stride 独立对齐。Recorder 按 Slot 压紧，DirectWriter 按 RecordAt 调用顺序提交。新 producer 不使用旧 launch-slot 稀疏区或 padding 占用数。
- 提交：完成记录、count、dropped 后发布 COMMITTED；同对象重复 Publish 幂等，新 writer 复用已提交 lane 产生协议错误。同 lane 并发不支持，状态字段不是锁。具体 CANN 发布/Host 可见性按主设计和目标工具链核对，不能凭标量代码顺序宣称硬件保证。
- Collect：绑定 stream 同步后，EMPTY/WRITING 未完成提交的 lane 保留零记录 stream 并清除 complete，不读取其 count/payload；其他已提交 lane 仍可分析。COMMITTED 且 count=0 也保留，用于区分“提交了空测量”与“未提交”。INVALID/未知 state、COMMITTED 的非法 count 或其他协议错误时 Collect 失败，不制造完整成功；不能把所有非 COMMITTED 状态都判作协议错误。合法 overflow 保留可容纳前缀和饱和 dropped，导出为 incomplete。
- 兼容范围：改变的是内存布局和全部 producer，不改变 `records.v1` 的小端、48/24/16 字节结构、magic、rank/lane 身份和历史 reader。未提交 stream 的 incomplete 标志使用 v1 已有字段；它不证明少传的 rank 文件能被发现。
- 修改范围：[公共设备头](../../src/include/tilexr_profiling.h)、[runtime](../../src/comm/tilexr_profiling_runtime.cpp)、[coarse Recorder](../../src/collectives/kernels/lccl_op.h)、[A5 direct producer](../../src/collectives/kernels/tilexr_alltoall_a5_kernel.cpp)、[A5 Host 编排](../../src/collectives/host/collective_launcher.cpp) 及对应 unit/integration tests 同步迁移。
- 验收/关闭：列出每个旧 producer 的等价事件序列，通过新公共 API→Collect→v1 writer→现有 reader 比较事件、时间戳、lane 和 dropped。保留旧稀疏 fixture 的历史文件读取测试，增加新紧凑 producer 的等价路径；不得只删除 occupied=0 用例。覆盖非 8 对齐 count、空提交/未提交、所有状态、同 timestamp、timestamp 0、overflow 前缀、非法 count、重复提交和跨 producer 一致性；EMPTY/WRITING 中无效 count 不得触发 payload 读取或阻断其他有效 lane；新文件 golden 与历史 v1 都可读。

### C09 AclCapture 资源和生命周期的新增 API 验收

**状态/期次：待实施，一期。依赖：I02、C07/C08。类型：新增 API 合同，不是现有已发生 bug。**

- 本地提取起点：[Init/Finalize/ResetProfilingState](../../src/comm/tilexr_comm.cpp) 以及 [CommDestroy](../../src/comm/comm_wrap.cpp)；新 SDK 不继承业务错误码、日志、跨 rank 状态协调或 communicator 适配接口。
- 修改：AclCapture 只拥有自己的普通设备 workspace/descriptor 和 Host Snapshot；借用 context/stream。应用拥有 ACL 初始化与业务资源；SDK 不调用 aclFinalize/resetDevice，不创建通信器。头文件实现无可变全局单例和跨 TU 定义冲突。
- Initialize 部分失败逆序清理；明确第 N 次分配、memset、descriptor 上传失败时的状态。Host 操作校验所属 context，不匹配返回错误并保留状态；已收集 Snapshot 可纯 Host 导出。不可复制，只允许移动构造，删除移动赋值，避免覆盖仍有 pending writer 的目标。
- Collect 等待绑定 stream 后 D2H，成功幂等；首个 Collect/Close 开始即封闭后续提交。Save=Collect+Export，发布失败保留 Snapshot，换路径不重复执行 Kernel。Close 幂等，先确认在途写入完成；同步失败不提前释放 workspace，保留对象供重试。析构不导出、不抛异常，无法确认安全完成时保守保留 GM 并给清理诊断，不承诺任意失败下零泄漏。
- Snapshot() 给出只读已收集快照，未收集/disabled 返回空；Close 保留 Host 数据以便纯 Host Export，重新 Initialize 替换旧快照。重复测量 Close 后 Initialize 新 capture，生成/设置新 UUID 和新输出路径；旧 DeviceArgs 从封闭提交开始不可再 launch。没有 Reset，也不宣称 SDK 自动知道任意 launcher 调用了多少次。
- 验收/关闭：内部 ACL 调用替身覆盖分配/memset/H2D/D2H/sync/free 错误、context 不匹配、重复 Collect/Save/Close、导出重试、移动构造/禁止赋值、旧 view 与 Snapshot 寿命、Initialize/Close 非法状态。故障时无重复释放、不覆盖业务错误、不伪装完整文件，保守保留的资源及诊断有明确证据；真实 ACL 链接、context 和可见性另列 CANN/NPU 证据。

### C10 异步生产调用的持久所有权与采集预算

**状态/期次：待实施，一期。依赖：C05/C08/C09。类型：生产集成合同；资源安全验收。**

- 本地证据：[A5 launcher](../../src/collectives/host/collective_launcher.cpp#L433) 按 communicator 累计 slot，容量耗尽可在业务 launch 前返回错误；[成功 launch 后](../../src/collectives/host/collective_launcher.cpp#L487) 再递增 slot。它与“一次 capture 一次 Kernel”需要整体改造。
- 修改：所有活跃调用点使用相同 AclCapture。业务层持久 owner 保存 capture/pending 队列；异步 launch 返回时不 Save/Collect、不销毁局部 capture，不引入采集专用等待。提前准备采集池和内存预算，池耗尽可以跳过本次采样，业务继续按原异步接口执行。
- 在已有安全完成点或现有业务资源收尾处逐 capture Collect→Export→Close；一个 communicator 不是一个 capture。复用业务资源销毁机制持有普通 capture 是调用方编排，SDK 不增加业务 callback、专用 adapter 或根据使用者选择不同布局。
- 未采样传 nullptr 并走 C05 无操作路径；已采样但事件容量耗尽保留前缀+dropped/incomplete。跳过整次采样须在调用方记录原因，不能伪造一个 complete capture。采集 I/O 错误与业务结果分别报告，不以隐藏同步改变公开业务异步合同。
- 修改范围：collectives Host/runtime、内部 Kernel 参数/纯 AICore binary 及业务资源 owner；旧 profilingLaunchSlot/CommArgs 采集字段不再作为 SDK 协议。业务结构 ABI 如需暂留占位须明确，旧 profiling API 不承诺源码兼容。MoonEP 公共消费者合同不得改变。
- 本地 A5 是 32 个 send lane + 16 个 receive lane，共 48，而物理 blockDim 为 33；一个 receive block 管理多个 writer。I00 重核后按实际逻辑集合配置，不能把 33 当采集 lane 数。若扩为 CommonCollectiveRuntime，所有资源 attach/get/destroy 与原始指针强转一起迁移，不保留类型错配。
- 资源 destroy 在内部锁中执行，不能重入 Lock/Detach；各 rank pending 长度可不同，本地逐项 Save 不夹带每 entry collective，在固定共同阶段汇总错误。每条已发布/关闭状态持久保存，失败重试不能从队头重写成功文件。跨 rank UUID/采样轮次由应用一致分发，不能靠单进程默认 UUID 生成代替。
- 验收/关闭：launch 行为替身确认异步返回路径不调用采集 sync/Save/free；延迟完成时 owner 仍持有 workspace，安全点逐项收集；并发 stream 独立 capture、池耗尽不阻断业务、48 lane 映射、资源强转/销毁不死锁、rank 队列长度不同也不发生协商错位、重试跳过已发布/关闭条目。硬件比较业务数值及启停后的异步语义，Host/Kernel 同步重编译安装，避免新 Host 配旧二进制。

## 4. 分析、统计与结果语义

### A01 时间源、时钟域和展示 origin 分开定义

**状态/期次：待实施，一期。依赖：I05、目标时间源证据。类型：当前文档/模型不一致。**

- 本地证据：[pairing](../../tools/profiling/pairing.py) 实际按 `(launch,rank,device)` 共用 origin，模型同时声明 `per_core`；[renderer 测试](../../tests/profiling/python/test_renderers.py#L173) 已验证同 rank 两 core 保留 `0.0/0.5 us`，不能再列为缺失测试。
- 修改：保持当前软件归零行为并统一字段/文档含义，区分 raw timer、duration 与显示 origin。不同 rank 分别归零；device unknown 不是识别了真实设备。若要改变策略，先提供目标 CANN/SoC 时间源与可比性证据，不能从合成测试推导硬件同步。
- 验收/关闭：复用现有同 rank 偏移、跨 rank 分别归零、大绝对时间戳短区间精度；显示、metadata、说明一致。记录 cycles_per_us 来源，示例 1000 不能当通用默认值；跨设备校准继续延期。

### A02 一次 capture 的 critical 与重复子阶段区分

**状态/期次：待实施，一期。依赖：C08/C10。类型：当前使用说明与分析范围不一致。**

- 本地证据：[A5 Host 多 slot](../../src/collectives/host/collective_launcher.cpp#L477) 与 [pairing](../../tools/profiling/pairing.py) 要求每 stream 恰好一个完整 critical、固定 synthetic launch 的模型并存。顺序重复子阶段可配对，不等于多轮总延迟可统计。
- 修改：warmup 不写本次 capture；每次业务 Kernel 独立 capture/UUID/报告。一次总区间内可重复普通子阶段；不按事件顺序猜 launch 边界，不保留新旧两种长期编排。
- 验收/关闭：单 critical+重复子阶段有效；零/多 critical 清楚诊断；两个不同 capture 分别正确分析，混合 UUID 拒绝；生产 A5 不再向单 stream 塞入多次 kernel_total。SDK 不接管 launcher，调用次数靠集成合同保证。

### A03 完整性说明与已知 lane 状态一致

**状态/期次：待实施，一期说明必需，预期参与者清单延期到 E02。依赖：C08、I05。类型：当前证明边界。**

- 本地证据：[pairing](../../tools/profiling/pairing.py) 令 expected=present、missing=0；[旧 exporter](../../src/comm/tilexr_profiling_runtime.cpp#L207) 省略空 stream。少传整份 rank 文件时，剩余输入仍可能 complete。
- 修改：报告明确“已提供输入内部完整性”；新 capture 导出其已配置 lane 的未提交/空提交状态，但不据此宣称能够发现未提供文件。新 lane 完整性由 C08 发布状态决定，paired critical 的可用性仍单独判断。
- 验收/关闭：未提交、已提交零记录、丢弃、缺失 critical、少传 rank 文件的诊断与说明准确；避免 expected=present 被解释成世界所有参与者已验证。外部 expected 清单不塞进严格 mapping.v1。

### A04 一期增加 union/exclusive，并修正 wait/sync 重叠压力

**状态/期次：待实施，一期。依赖：A01、A02 测量合同。类型：已选统计整改。**

- 本地证据：[analysis](../../tools/profiling/analysis.py#L143) 累加样本 duration，[wait/sync](../../tools/profiling/analysis.py#L227) 再累加 share；交叠实例与嵌套阶段可重复计时。这是现有累计口径边界，不能称解析失败。
- 修改：保留可解释的 inclusive/sample sum。同 lane 存在唯一有效 critical 区间 C 时，令 U_s 为 stage s 全部 interval 样本与 C 相交后的区间并集，U_children 为 mapping 明确声明的直接子 stage 的 U_t 并集；union(s)=长度(U_s)，exclusive(s)=长度(U_s 减去 (U_s 与 U_children 的交集))。先用整数 timestamp 计算再换算，不跨 rank 拼并集，不相减子阶段 duration，不简单截 share 到 100%。
- 多个交叠父 instance 按 stage 覆盖并集处理，不根据时间猜测不存在的唯一父 instance 绑定；同级 stage 仍可能重叠，不承诺所有 exclusive share 加和为 100%。instant 不计区间长度；无唯一有效 critical 时保留原始配对/诊断，新的 critical 相对统计显示 unknown，不制造分母。
- wait/sync pressure 使用关键 lane、critical 区间内 wait/sync 区间的并集，嵌套和交叠只计一次。inclusive、union、exclusive 各自名称、分母、父子/instance 范围必须进入模型、Markdown/HTML 和 provenance/trace 的统计说明。
- 验收/关闭：critical=2 μs、wait=1.8 μs、其内部 sync=1.6 μs 得到 90% 压力，原 inclusive 可保留 170% 累计值但不能继续称覆盖压力。另测相离、相邻、部分交叠、重复区间、嵌套、多个父样本、零时长、区间外样本与大 timestamp；子并集只扣一次，统计非负、有限且不改变 raw trace。

### A05 三个产物保留一致 provenance，不改变 v1

**状态/期次：待实施，一期。依赖：I05、A01。类型：结果溯源一致性。**

- 本地证据：[CLI/API](../../tools/profiling/cli.py#L158) 支持 Host attributes 并合并到 run；[Perfetto metadata](../../tools/profiling/perfetto.py#L274) 没有完整带入同一 run attributes。记录内已有的事实与用户提供的 metadata 需区分。
- 修改：同一份经过校验的 provenance 进入 Perfetto、analysis.md、report.html；保留 capture、源文件/事件、SDK/Kernel 版本、SoC/CANN/driver、backend/native status（适用时）、计时换算及统计口径。未知值显示 unknown；身份以 records 推导结果为准，换算以 mapping.cycles_per_us 为准，调用方 attributes 不能覆盖这些协议事实，不声称换算来自 records。
- 使用现有 API/CLI attributes 和新增统一报告模型传递，不向严格 mapping.v1 加字段，不声称 v1 自动携带了硬件身份。业务层负责提供真实数据；机器可读独立 summary、字节指标等另列 E 项。
- 验收/关闭：同一 metadata 在三个产物取值一致；缺省/unknown、来源冲突、非有限值、深层结构、恶意 HTML/Markdown 字符和大属性有边界处理；保持离线/self-contained，报告不能因 provenance 触发外部资源请求。

## 5. 测试、构建与交付整改

### T01 用可执行合同替换生产源码文本守卫

**状态/期次：待实施，一期。依赖：I01–I06 的对应可测合同。类型：当前测试与项目规则不一致。**

- 本地证据：[demo source checks](../../tests/profiling/unit/test_profiled_add4_demo_sources.cpp#L93) 匹配成员、token、调用次数/顺序；[Python dependencies](../../tests/profiling/python/test_dependencies.py) 搜生产 token/路径，其检查不能证明完整的标准库依赖。
- 修改：移除要求生产实现必须包含或禁止特定源码片段的守卫；以编译/链接、安装消费、API 行为、导入运行、导出记录和真实硬件结果覆盖合同。依赖分析可辅助定位，不能成为强制生产实现写法的 regex/AST 守卫。
- 验收/关闭：等价重构不必更新字符串期望值；破坏协议、有效范围、状态、依赖或生命周期会使相关行为测试失败。明确原每条守卫由哪个测试替代，不能只删测试。

### T02 风险对应的行为与故障验证

**状态/期次：待实施，一期。依赖：C01–C10、A01–A05。类型：覆盖边界与新增 API 验收。**

本地 [runtime 单测](../../tests/profiling/unit/test_profiling_runtime.cpp#L43) 主要覆盖一组稀疏记录、排序与拒绝已有文件；Python 的布局、配对、escaping、report 发布等已有证据继续复用。不能从某个单测缺少用例推导全仓缺证据；I00 后先核对全部相关测试。

| 验证组 | 必须覆盖 | 对应 ID |
|---|---|---|
| 纯 Host/协议 | ID/rank/lane/尺寸边界、各提交状态、count 非对齐、排序、零 timestamp、空提交/未提交、overflow、历史文件读取 | C04/C06/C08、A02/A03 |
| 设备逻辑 | 有效槽压紧、初始化复用、重复发布、重复 writer、dropped 饱和、编译关闭/runtime disabled | C01–C05/C08 |
| ACL 故障注入 | 第 N 次分配/memset/上传/同步/D2H/释放失败，context mismatch，移动、重复关闭、重试和 Snapshot 寿命 | C09 |
| 文件系统 | 并发发布、不覆盖、写入/close/发布失败、残留只清理本次资源 | C07 |
| 生产编排 | pending owner 寿命、无新增 launch 等待、预算耗尽跳过采样、已有安全点收集、Host/Kernel 配套 | C10 |
| 离线分析 | 原协议/诊断、A01 现有归零测试、union/exclusive、三产物 provenance、escaping 与有限数 | A01–A05 |
| 目标工具链/NPU | 地址/布局、GM 发布可见性、真实 marker/数值、独立 capture、开关开销与 spill | C01/C05/C06/C08–C10、D03 |

**关闭条件：**每项具有触发输入和失败断言；替身/模拟、CANN 编译、硬件三类结果分开记录。硬件不可用时保留未验证项，不把 Host 通过写成设备可运行。

### T03 独立安装、Python 包与持续集成

**状态/期次：待实施，一期。依赖：I03/I05/I06。类型：交付验收。**

- 本地证据：[根 CMake](../../CMakeLists.txt#L72) 无条件加入 `src/comm`；[profiling CMake](../../tests/profiling/CMakeLists.txt) 仍编译 runtime `.cpp`、demo 链接 tile-comm，且没有注册 Python unittest；[现有头安装](../../src/comm/CMakeLists.txt#L235) 是显式文件列表。上述不是独立 SDK 的消费验证。
- 独立 `src/profiling` 自带 project、GNUInstallDirs、版本和 install/export；纯 include 安装不查 CANN、不编译 kernel、不查通信依赖、不初始化 TileXR 子模块。CMake INTERFACE target 传播 include/C++14，调用方分别配置必要 CANN Host 链接和设备编译，不将 Host flags 强套 AICore。
- 统一选项：`TILEXR_PROFILING_BUILD_TESTS=OFF`、`TILEXR_PROFILING_WITH_ACL=OFF`、`TILEXR_PROFILING_BUILD_EXAMPLES=OFF` 为默认。只有构建 ACL 验证/例子才开启 WITH_ACL；例子要求显式设置实际 `TILEXR_PROFILING_SOC`。这些新增开关须由 I03 实现后才能执行。
- 覆盖“纯 Host 无 CANN”“实际 CANN 编译”“显式硬件运行”三组；多个 TU/共享库与 executable/include 顺序验证 ODR。SDK 自身语法和 Host 为 C++14；本地 demo [设备命令](../../tests/profiling/CMakeLists.txt#L92) 实际 gnu++17，[Host](../../tests/profiling/CMakeLists.txt#L132) 为 C++14，设备支持标准按目标 CANN 探针记录，不能直接降标准或冒称已验证设备 C++14。
- 安装完整 include 子树、license、版本和 CMake config/export；在源码不可见的安装 prefix 编译外部 consumer，检查无源码树绝对依赖。源码样例和安装样例都不创建 communicator。
- 唯一 Python 包迁入 `tools/profiling/python/tilexr_profiling`，`pyproject.toml` 明确 package-dir、发现范围、wheel/sdist 资源；目标 Python >=3.9，最低版本和 3.12 均经 CI 后才发布支持声明，requires-python 随证据填写，不把本地 3.12 结果当 3.9 验证。全部仓内调用与测试改 canonical import；移除旧根实现/CLI，不做第二份实现或长期 shim。
- 分别验证源码环境安装、editable 安装、干净环境 wheel 安装；wheel 测试清空仓库 PYTHONPATH，从仓外用 `-I` 调用模块/API。除 help 外生成真实 fixture 报告，并覆盖正常 0、fatal 1、incomplete 2；argparse 参数错误的 2 不保证有报告。
- Python 只对包/相关测试必需，CTest 可选查找解释器并明确跳过相关组；包构建工具在 CI 预置，不在离线运行中临时下载。增加 `profiling_host/profiling_python/profiling_cann/profiling_npu` 标签；未给显式设备配置时不注册/启用 NPU 测试，不能只加 label 就认为默认 CTest 不会运行它。验收需证明无设备配置的默认 CTest 不触发硬件。
- 将 Host/协议/安装包验证接入 [host checks](../../scripts/ci/host_checks.sh)，新增维护 runner 加入 shell 语法检查；更新 [CI 说明](../CI.md)、[脚本目录](../../scripts/README.md) 及相应 workflow。部署主机/凭据不能进入一次性脚本。
- 关闭条件：安装消费、canonical package、仓内集成及 CI 可重复通过；Header-only 接入不强制用户装 Python，不依赖根项目全局 include、spdlog、reference 或 HCCL/MPI。CLI 移除与旧教程归档在同一迁移完成，不遗留成功运行旧入口的承诺。

## 6. 文档与示例整改

### D01 完整的格式、提交与统计参考

**状态/期次：待实施，一期。依赖：C08、A01–A05。类型：文档遗漏及新合同维护。**

- 固定仓库证据：[mapping](../../tools/profiling/mapping.py) 已有 track/instance_range；[旧接入指南](../../tools/profiling/OPERATOR_PROFILING_GUIDE.md) 与 [Add4 字段表](../../tests/profiling/demo/README.md) 未完整同步。目标 `tools/profiling/README.md`、`API_REFERENCE.md`、`FORMAT.md` 在此基线需新增；不能把会话解压目录额外存在的说明文件当作 Git 基线已有材料。
- 修改：维护唯一 FORMAT/API_REFERENCE，完整列出 mapping 四种事件形式、track、实例 ID、count/begin/end/stride、非零 ID/uint32 界限、唯一 critical、层级约束；说明磁盘 v1 与新内存 ABI 分别版本化。记录 count/stride、状态、空提交/未提交、排序、时间源、inclusive/union/exclusive 与完整性边界。
- 验收/关闭：每个公开字段有类型、必填/默认、约束和最小例子；例子经真实 parser/行为验证，不靠生产 token 搜索。更丰富 manifest 不偷加到严格 mapping.v1；未来扩展使用 E 系列任务。

### D02 统一入口、迁移说明及历史文档归档

**状态/期次：待实施，一期。依赖：I01–I06。类型：已选范围替换。**

- 新旧分歧已定：不保留 CommArgs adapter、旧 Flush 签名、Session/Reset 或旧 tools.profiling 成功入口；统一 AclCapture/DeviceArgs、Close 后新 capture、tilexr_profiling 包。不能将旧日期方案的兼容承诺继续当现行要求。
- 主设计与本文是仅有的两份权威计划；此前会话中的日期草案已被取代，实际仓库不存在时不复制、不创建，也不要求未来 agent 阅读。仓库已有旧接入指南按本项统一导航。旧字段/类的源代码迁移与业务 API 不变分别解释；公开 MoonEP 消费者合同不变，内部 Host/Kernel ABI 必须配套重编译。
- 实施时形成唯一 README（安装/最短流程）、API_REFERENCE（类型/状态/错误/并发）、FORMAT（协议/统计/时钟/证据）；TileXR 和外部用户同教程。已有 guide 转导航或合并，不保留内部专用接入版本。
- 最短教程说明应用负责 ACL/context/stream/业务 launcher，GM descriptor 是设备地址；Header-only 不等于无需 CANN 编译链接。默认示例可立即 Save；异步生产另教持久 owner+已有完成点，不教局部 capture 退出时等待。
- 同步实际基线已有的 [根 README](../../README.md)、[设计索引](../specs/README.md)、[原设计](../specs/2026-08-28-generic-profiling-tool-design.md)、[接入指南](../../tools/profiling/OPERATOR_PROFILING_GUIDE.md)、[构建验证](../BUILD_VERIFICATION.md) 和脚本/CI 文档；新增统一 README/API_REFERENCE/FORMAT 后，旧指南转导航或并入主参考。该 Git 基线没有会话解压目录中的中文接入指南和已有算子清单，不把它们列为既有待改文件。旧命令不得继续作为现行可执行教程。
- 验收/关闭：新用户只沿一条入口完成 include→Initialize→marker→Save→报告；异步集成能找到 owner/预算/收尾合同；链接、示例参数、路径和包名与实际发行一致。可复用经验记录到维护文档并随实现交付，不创建散落调查日志。

### D03 最小示例、生产 binary 与平台/开销边界

**状态/期次：待实施，一期；跨平台扩展和专项采样优化延期。依赖：T02/T03、A01/A05。类型：已有边界。**

- 本地证据：[demo CMake](../../tests/profiling/CMakeLists.txt#L33) 限 Ascend950，[runner](../../tests/profiling/demo/run_demo.sh#L23) 和 [mapping](../../tests/profiling/demo/profiled_add4.mapping.json) 选择 950/1000 cycles per μs；文档已有平台声明，本轮未证明其换算错误。
- 当前测试可执行文件内嵌 kernel，不据此指控生产 Host 库违反 binary 外置要求。统一分发示例生成纯 AICore 文件，安装到 `${CMAKE_INSTALL_LIBDIR}/tilexr/kernels`，相对所属 Host 产物加载，经 rtDevBinaryRegister/rtFunctionRegister/rtKernelLaunchWithFlagV2 启动，不新增 kernel<<<...>>> wrapper 或生产内嵌对象。
- 源码/安装树 Add4 验证数值、记录、状态、三报告；保存 SoC、执行模式、CANN/driver、编译标准/宏、Kernel/SDK 版本、换算依据。未知 SoC 提前明确不支持，不能从 950 运行推导 910B/910A5 全覆盖。
- 一期基本开销验收需比较编译关闭、仅本地打点、打点加 flush，并检查容量相关 spill；这是低扰动声明的证据，不是默认采样优化承诺。流水线同步含义由算子明确，SDK 不自动为每个 marker 插全流水 barrier。
- 关闭条件：实际已支持平台有编译及数值/记录证据，未测平台保留未覆盖；性能与真实硬件绑定。通用 Add4 不为形式建立通信依赖或重复运行 HCCL Test；通信故障调试遵循 AGENTS 的匹配环境一次资格记录规则。

## 7. 明确延期的增强清单

以下条目完整承接旧方案的后续能力，均为 **延期**。不以其缺失否定已明确范围的一期交付；实施时先更新主设计、协议/接口及验收，再进入代码。A04 和 A05 已纳入一期，不能再次把它们整体推到增强阶段。

| ID | 增强与边界 | 未来修改/依赖 | 未来验收 |
|---|---|---|---|
| E01 | 多 launch/iteration 容器、warmup 窗口和重复运行统计 | 显式身份/版本化协议、Capture 与 pairing/analysis 键；不按事件顺序猜分界 | 多轮单/多 rank、并发身份隔离、统计不串轮；旧 v1 仍可读 |
| E02 | 外部 expected rank/lane 清单及 manifest | I02/I05 后设计可选 sidecar/API，不扩充严格 mapping.v1 | 少文件、少 lane、额外参与者、合法未参与与静默未执行可区分 |
| E03 | bytes、数据量语义与带宽入口 | manifest/API、StageMetric、按 rank/stage 的 scope；数据来源由业务提供 | per-interval/total、交叠分母、十进制单位、跨 rank min/median/max；不凭名字猜字节数 |
| E04 | analysis.json/CSV、捕获对比与阈值 | 稳定机器 schema、CLI、统计版本和 provenance | 版本兼容、基线/目标可比性、失败阈值与 missing/unknown 清晰 |
| E05 | 大捕获和峰值内存控制 | 总 bytes/records/streams 限额、按 stream 解析、流式 renderer、可选产物 | 边界和拒绝行为、峰值内存、部分失败不发布伪完整结果 |
| E06 | HTML 筛选、排序、搜索与层级展开 | 复用统一分析模型，不复制第二套统计 | 离线可用、escaping、较大数据交互与可访问性 |
| E07 | 可选采样、block/stage/iteration 过滤和专项开销优化 | 预准备池/预算沿用 C10；记录选择策略与未采样来源 | 同等正确性与测量口径 A/B，保留 on/off/spill 证据，不禁用框架 profiler/barrier |
| E08 | 时间源自动发现、跨设备校准及更丰富自动 metadata | 目标 CANN/SoC API 与校准证据；A01/A05 的一期手工来源仍保留 | 时钟漂移、校准误差、不可比平台拒绝错误全局时间线；不能仅靠共享 origin |
| E09 | 通用借用 workspace/池化与稀疏 lane map | 新公共布局/所有权合同，对所有调用者同样开放；一期仅预备独立 AclCapture | caller owner/stream 寿命、稀疏集合身份与完整性、池复用隔离，不引入内部专用模式 |

E02 不是 A03 一期说明的替代；E03 不是 MoonEP 现有性能报告必需字段的豁免；E07 不改变一期异步资源安全和基本开销验收。新任务不能在实现中顺手引入这些扩展后不更新范围。

## 8. 可执行验收与关闭标准

下列验收入口已实现，命令模板本身不表示该平台已运行；当前结果与缺口见第 9 节。README 维护测试名与标签，无 NPU 配置的默认 CTest 不触发硬件工作。

### 8.1 I00 通过后保存基线

在已获访问权的真实 checkout 记录 `git status --short`、`git rev-parse HEAD`、主基线和对照分支完整 SHA、远端 URL 和实际比对结果；无须在文档暴露认证数据。记录实际测试目录和 Python 导入路径，防止用另一个已安装版本的测试结果关闭本 checkout 的任务。

### 8.2 纯 Host、安装及包

```bash
# 从已核实源码根目录执行；纯 Host 组不要求 CANN。
cmake -S src/profiling -B build-profiling-host \
  -DTILEXR_PROFILING_BUILD_TESTS=ON -DTILEXR_PROFILING_WITH_ACL=OFF
cmake --build build-profiling-host
ctest --test-dir build-profiling-host --output-on-failure -L profiling_host
cmake --install build-profiling-host --prefix "$PWD/install-profiling"

# T03 安装 consumer 从另一个目录构建，只提供 install-profiling。
# Python 源码环境/打包工具已预备，不能假定包已发布到公网仓库。
python3 -m pip install --no-build-isolation --no-deps -e tools/profiling
python3 -B -m unittest discover -s tests/profiling/python -p 'test_*.py'
python3 -m build --no-isolation tools/profiling --outdir build-profiling-dist
```

另建干净环境以 `--no-index --no-deps` 安装生成的本地 wheel，从仓外、空 PYTHONPATH 运行 `python -I -m tilexr_profiling` 与 API fixture 报告，核对导入来自安装目录；sdist 构建 wheel 后同样验证。CMake config consumer、多个 TU、wheel 内容及实际报告生成均属关闭证据，help 成功不够。Python 不存在时 C++ 测试明确跳过 Python 组；旧 `tools.profiling` 命令不作为新计划的成功验收入口。

### 8.3 目标 CANN 与设备

```bash
# TARGET_SOC 由使用方设置为实际、已支持的型号；先加载目标 CANN 环境。
cmake -S src/profiling -B build-profiling-cann \
  -DTILEXR_PROFILING_BUILD_TESTS=ON -DTILEXR_PROFILING_WITH_ACL=ON \
  -DTILEXR_PROFILING_BUILD_EXAMPLES=ON \
  -DTILEXR_PROFILING_SOC="${TARGET_SOC:?set actual supported SoC}"
cmake --build build-profiling-cann
ctest --test-dir build-profiling-cann --output-on-failure -L profiling_cann
```

只有明确设备和可用环境才运行 `profiling_npu`。保存编译器/标准、设备模式、实际加载的外置 binary 和驱动身份。参数及设备选择由环境/使用方提供，不在计划硬编码主机或卡号。

设备提交可见性、spill、低扰动、异步保持、数值正确性必须各有对应证据。MoonEP 若纳入回归，保留 Buffer/MoonEPCommPlan/返回值/事件/零拷贝公共合同，使用 group_credit（只有任务明确要求才用 group），保持框架 profiler 与 Dispatch/Combine barrier；报告包含 backend、native status、Kernel 版本、数据量、可读延迟、适用带宽和跨 rank min/median/max。Host/模拟/910B fallback/一般 Add4 不证明 A5 UDMA 数据面。

### 8.4 I 与整改任务的关闭映射

| 主任务 | 关闭所需一期任务/证据 |
|---|---|
| I00 基线（已完成） | 用户确认 Upgrade 主/main 对照；11:58 认证远端 SHA 相同、固定对象归档及 315 文件匹配证据见第 1 节 |
| I01 Device 解耦 | C01–C06/C08，T01/T02 的 Host/Device 编译及提交证据，D01 |
| I02 Capture/export | C05–C09，A03，故障/文件/状态证据；资源安全不靠析构隐藏 |
| I03 独立交付 | C07、T03、D03；完整安装 prefix 外部 consumer，无通信库依赖 |
| I04 TileXR 整体迁移 | C08/C10、A02、T01/T02；无 CommArgs 采集 adapter，生产异步与业务合同保持 |
| I05 Python 分析 | A01–A05、T03、D01/D02；canonical 包、三产物和历史磁盘协议 |
| I06 统一示例 | T02/T03、D02/D03；相同 API、真实数值/记录、明确平台与开销边界 |

实施完成时在每项追加：`变更位置 / 验证命令与结果 / 证据路径 / 未覆盖边界 / 关闭日期`。仅完成计划合并不关闭任何代码整改。所有一期项和 I00–I06 均达到实际交付层的标准后，才能报告独立化完成；E 项保留延期，不用删掉它们来制造“全部完成”。

## 9. 2026-10-20 实现接续与本地验收

本轮在 `C:\Git_data\tileXR` 接续已有未提交改造，保留并复核 SDK、Python 包、collectives 和 Add4 的同一套实现。开始时 HEAD/main 为 `efa5d80d87aea2eb60e4df770625a31236ee9a93`；认证 `git ls-remote --heads origin refs/heads/main refs/heads/codex/TileXR_Profiling_Upgrade` 再次返回两条相同 SHA，本地 origin 引用也一致，无需 fetch、checkout 或合并。用户明确选择本次只做本地验证与提交，未启动远端硬件工作。

### 9.1 代码交付状态

下表按实际层级记录，不用“代码已存在”关闭硬件门槛。日期均为 2026-10-20；“本地完成”仅指表中证据范围，所有注明未验证的上层任务保持开放。

| ID | 变更位置与结果 | 本地证据 / 尚未关闭边界 |
|---|---|---|
| I01、C01–C06、C08 | `src/include/tilexr/profiling/{types,device,direct,detail/device}.h`：72-byte ABI、有效槽压紧、零时间戳、dropped 饱和、单 writer 状态、0/1 编译开关 | `profiling_device_protocol`、switch 编译/可执行测试及 round-trip；CANN 地址空间、目标指令/可见性、spill 与开销未验证 |
| I02、C07、C09 | `host.h/file.h/acl.h`：布局预算、Snapshot、v1 编码、独立 AclCapture、context/同步/释放重试、Linux 无覆盖发布 | `profiling_sdk_host`、`profiling_sdk_acl_mock`；本机未运行 Linux 专有文件故障/并发分支与真实 ACL。`WorkspaceBytes()` 已按仍拥有的分配返回，关闭/移动后归零 |
| I03 | `src/profiling`：独立 CMake、INTERFACE target、include/license/version/examples 安装 | `profiling_sdk_installed_consumer` 从删除后的 staging 源码隔离构建 C++14 executable/shared library/多 TU；无需通信库或 CANN。Linux 消费待 CI |
| I04、C10 | `collective_launcher/kernel`、`profiling_capture_queue.h`、coarse/A5 kernel 参数一起迁移；comm 旧采集/第二 exporter 移除；最多 4 capture/64 MiB，48 A5 lane，独立 UUID | `profiling_capture_queue_mock` 覆盖耗尽跳过、后续 acquire 无 sync/D2H/free、失败重试和固定共同清理阶段；A5 布局与 mapping 回归。真实 launcher/跨 rank 异步/数值及新 Host/Kernel 配套编译仍待目标环境 |
| I05、A01–A05 | canonical `tilexr_profiling` 包、union/exclusive/pressure、统一 provenance、输入完整性边界、旧成功入口退役 | 81 项 Python 3.12.14 测试、wheel/sdist 仓外隔离 CLI 0/1/2 与 API 三报告；Python 3.9 CI 只配置、未运行，不把它标已验 |
| I06、D01–D03 | `examples/profiling` 唯一外置 binary Add4；`README/API_REFERENCE/FORMAT`、根索引、collectives/CI/scripts 说明同步；旧设计标历史 | 文档 JSON 示例和 Add4 模板通过真实 parser，纯 Host 安装消费通过；Ascend950 Add4 的真实编译、数值/记录、独立目录搬迁和性能证据待验证 |
| T01 | 删除 demo 生产 token 守卫与依赖字符串守卫，改为实际编译、导入、安装和行为 | 编译开关/ABI、SDK 安装消费者、独立发行消费者、实际 writer→reader 往返；文档测试只提取公开 JSON 示例，不约束生产实现文本 |
| T02、T03 | 增加 Host/设备 CPU 替身/ACL 故障/队列、跨语言 round-trip、发行 runner、3.9/3.12 workflow 和 CTest 标签 | 本地可执行组通过；一个 Host switch 二进制被 Windows 应用控制策略禁止启动；Linux/CANN/NPU 与 GitHub CI 尚未执行，T02/T03 不全量关闭 |

本轮检查修复了 canonical 包迁移后 A5 mapping CTest 未设置导入路径、API 以真假值判断导致空列表 attributes 被静默接受、`WorkspaceBytes()` 在已关闭/移走所有权后仍报告旧布局大小及 Add4 mapping 文档死链。补充的 writer→ACL 替身→v1→Python 往返覆盖原稀疏槽的等价事件序列、稳定相同时间戳、丢弃前缀、空提交和未提交，避免只靠两端独立合成 fixture 证明协议一致。

### 9.2 可重复命令与结果

在本仓库执行，下列 `cmake/ctest/c++/python` 使用本机预置工具。C++14 编译器为已有 Zig 工具链；设备测试使用 CPU stub，不能称为 Ascend C 编译。

```sh
PYTHONPATH=tools/profiling/python python -B -m unittest discover -s tests/profiling/python -p 'test_*.py'
# Python 3.12.14：81 tests，OK。
PYTHONPATH=tools/profiling/python python -B tests/collectives/unit/test_alltoall_a5_profiling_mapping.py
# 3 tests，OK。
cmake --build build/profiling-sdk-escalated
ctest --test-dir build/profiling-sdk-escalated --output-on-failure -E '^profiling_switch_host_1$'
# 12/12 通过，含 Python suite、11 个 Host 标签测试与跨语言 round-trip。
python scripts/ci/check_profiling_python.py --work-dir /external/evidence
# wheel + sdist 重建 wheel：各自新 venv、仓外 cwd、清空 PYTHONPATH、-I；CLI 0/1/2 和 API 均生成预期报告。
```

未排除测试时的完整 Host CTest 为 11/12 通过，`profiling_switch_host_1` 是 `BAD_COMMAND/Not Run`；直接执行确认 Windows 报“应用程序控制策略已阻止此文件”，发生在进入 main 前。未通过更名或关闭策略绕过，不能把该项记为运行通过；它的编译已完成。最终排除该项只是区分平台不可用与其余行为，不能宣称完整 CTest 全绿。

运行日志属于忽略的本地构建证据，不提交二进制/venv/机器目录：`build/profiling-package-review.log`、`build/profiling-sdk-escalated/Testing/Temporary/LastTest.log`；发行实际产物位于本次会话外部 evidence 目录，路径见 package 日志。原有较早日志仅作历史，不替代本轮命令结果。

### 9.3 保留的验收与复用教训

未关闭：Linux 文件系统的真实 hard-link/O_EXCL 并发及故障分支、Python 3.9 CI、真实 CANN 9.1 开/关设备编译、GM 提交可见性、Add4 数值与三报告、生产异步多 rank/48 lane、marker/flush 开销与 spill。E01–E09 继续延期；没有扩协议或添加 TileXR 专用 adapter。下次从这些未验证边界继续，不需要重做已核实的架构设计。

补充检查：A5 C++14 layout/192-byte KernelArgs ABI 回归通过；`TILEXR_ENABLE_PROFILING=2` 被编译器按预期拒绝；默认 CTest 枚举 13 项且无 NPU 项；两个 shell 入口分别经 Bash `-n` 通过。CI 中间失败继续执行的行为 fixture 已同步到 8 个 case，但依赖 POSIX `/bin/bash` 的整套 `tests/ci` 未在本 Windows 环境运行，保留给 Linux CI。

新增示例接入 shell 扫描时还修正了原检查器的多路径调用：`bash -n file1 file2` 只解析第一个脚本，不能据此证明新增 runner 的语法。现在逐文件调用，避免剩余路径被当作位置参数；维护说明见 `scripts/README.md`。

可复用教训已同步到维护文档：包迁移应逐个检查 CTest 的实际进程导入环境；跨语言协议以真实 producer→exporter→reader 合同测试补齐 fixture；API 必须区分 `None` 与空的非法参数；资源字节数应跟所有权而非旧布局；可选本地资源是否存在不能决定 rank 是否进入共同清理阶段。Host 替身和本机策略失败都须与真实 Linux/CANN/NPU 证据分开记录。
