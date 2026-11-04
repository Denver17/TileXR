# UniTest 执行与工具接入设计

> 历史设计提案：用于追溯实施决策，其中的目标配置和阶段范围不作为当前用户接口。当前架构与支持范围以 [UniTest 设计](unitest.md) 为准，使用方法见[用户指南](../../tools/unitest/README.md)，验收记录见[验证文档](../validation/unitest.md)。

状态：不带内部 profiling 的版本已交付至 `5e84bdb8`；用户批准继续实现以下固定 GM 槽位方案。

重新实现基线：`1c09422631e1f36ff64e25fd45196cd68c773e40`。
本轮从 `331848016283a55b9d4160b84e32f5a0519d3995` 继续；框架基础已交付，新增固定槽位的通用打点接线。
不合入 `3e2defd33` 的 EP v2 导出方案，也不整提交恢复依赖该方案的后续扩展；
可逐项复用已审查且符合本文职责边界的执行代码，旧版本验证不作为新版本验收。

## 1. 目标与边界

支持六项必需能力：single、correctness、perf、profiling 开关、算子连跑、跨机执行。
其中模式、算子步骤、部署和 profiling 相互独立，不为每个组合建立独立流程。

UniTest 维护执行、输入与参考结果、数值校验、Host 计时、配置、调度和结果汇总。
它只接入已有 profiling 工具，不维护官方 profiler 数据解析、自定义 Trace 生成器、
打点记录格式或重复的分析报告工具。保留生产内核原有打点，忽略其 v2 私有格式；
新增范围仅为下文固定槽位接线；不新增 profiling 公开接口或生产源文件。

保留基线已有的统一构建与针对已安装 TileXR 的独立构建，Host 使用 C++14。
首批数据适配仍限定普通 EP Memory、BF16、无量化；量化、Server Dedup、UDMA
适配和仿真不随本设计自动加入。跨机执行仍使用所选算子的实际通信后端。

## 2. 用户配置

用例描述输入、步骤和数据绑定；运行配置描述模式、轮次、机器和采集选择。
用户修改运行配置不需要重新编译。命令行可覆盖运行配置，优先级逐字段为
显式 CLI > config > 默认值。启动前保存完整解析配置及字段来源。

以下 JSON 表达目标契约，最终字段校验须保持一致，不接受任意函数名调用。

```json
{
  "schema": "tilexr.unitest.run.v1",
  "mode": "correctness",
  "iterations": 100,
  "check_schedule": "powers_of_two",
  "profiling": {
    "enabled": false,
    "tool": "msprof",
    "options_file": null
  }
}
```

步骤配置举例：

```json
{
  "steps": [
    {"id": "dispatch", "operator": "ep_dispatch"},
    {
      "id": "combine",
      "operator": "ep_combine",
      "inputs": {
        "payload": "dispatch.payload",
        "assist": "dispatch.assist",
        "counts": "dispatch.counts"
      }
    }
  ],
  "validate": "combine.output"
}
```

独立 Dispatch/Combine 是只有一个 step 的同类配置。独立 Combine 的输入由
Host stimulus 生成；连跑 Combine 的对应输入绑定到 Dispatch 的真实设备输出，
权重等其他输入仍来自用例。连接类型、形状、容量及依赖顺序在启动前检查。
不支持循环、任意脚本或配置注入生产函数调用。

Dispatch + Barrier 举例：

```json
{
  "steps": [
    {"id": "dispatch", "operator": "ep_dispatch"},
    {"id": "barrier", "operator": "barrier"}
  ],
  "validate": "dispatch.output"
}
```

Barrier 必须注册到适配表，使用真实设备 Barrier API，在约定 stream 上执行。
没有合适的公开实现时明确不支持，不能以 Host 等待、Controller ready gate 或
stream synchronize 冒充。准确的 Barrier 公开入口需要在实现前核实。

## 3. 三种模式

### single

准备一次，执行一轮全部 steps，同步 stream，读取并校验最终输出，然后释放。
single 表示一次完整执行，不表示单卡，也不表示必须只有一个 step。

### correctness

执行 iterations 轮，默认按累计完成轮数 1、2、4、8、16……检查，最后一轮必查。
相同检查点不重复。例如 100 轮检查 1、2、4、8、16、32、64、100。
保留 every_iteration 选项用于定位问题；不用固定 check_interval 表示指数策略。

检查点之间在 Worker 内连续提交，框架不插入逐轮额外 stream 同步。
每到检查点，各 rank 完成该段、同步、读取并校验，收齐结果后再进入下一段。
检查点失败立即终止后续执行，报告上次通过点和本次失败点。

默认固定 seed 和可复现输入；不在本轮引入复杂的逐轮变更输入机制。
报告明确：只验证检查点的最终输出，不证明被覆盖的中间轮次全部正确，
固定输入也不能充分覆盖陈旧结果与当前期望相同的情况。

### perf

性能统计采用 Host batch，不内置 msprof Kernel Duration 解析。
配置 warmup、iterations、batches；建议默认分别为 5、100、3，允许运行时修改。
iterations 表示每批完整执行 steps 的轮数。

执行顺序：

1. 准备输入，执行一轮正确性预检。
2. Worker 执行 warmup 轮，完成 stream 同步。
3. 每批在计时外完成所需准备和 stream 同步，Controller 收齐全局 ready。
4. Worker 使用 steady_clock 取开始时间，连续调用 iterations 轮真实 API。
5. 最终 stream 同步成功后取结束时间，再读取和校验最终输出。
6. 收齐各 rank 的批次结果，开始下一批；所有批次通过后释放。

平均耗时 = elapsed / iterations。连跑表示每轮整个 steps 序列的平均耗时，
不能除以 steps 数量或把总时间平均分配给各算子。
分配、输入生成、参考计算、控制 RPC、JSON 序列化与校验不放入计时窗口。
真实生产 API 内在的 Host 工作和同步保持原样，属于计时范围。

报告使用 Host batch average，不称为纯 Kernel Duration；批均值分布不等于
单次调用延迟分布。带宽仅在已定义通信字节模型时给出，并注明计时和字节口径。

## 4. 有序连跑与算子责任

Worker 持有步骤适配器和整条链路的缓冲区，各轮按 steps 顺序使用约定的 stream
提交。不在 Dispatch 与 Combine 之间读回或重新生成中间结果。缓冲区至少保持到
本轮最后消费者完成，并保持到检查点所需的最终输出完成校验。

连跑默认只检查选定的最终输出。Barrier 无数据输出，因此 Dispatch + Barrier
检查 Dispatch 结果；Dispatch + Combine 检查 Combine 结果。结论是端到端校验，
不宣称每个中间输出独立通过；独立算子用例负责该算子各项输出检查。

框架正确使用公开 API、stream、参数和内存生命周期。magic 等内部状态由生产 API
按原有契约管理，不在测试框架重复推进或修复。框架提供连续调用能力，不因算子
可能不支持而额外插入同步、自动降级或兼容路径。使用者选择合法组合；失败保留
证据，定位前不把所有失败自动归为算子问题。

若 API 明确要求调用者承担某个步骤，则适配器遵守该公开契约，不复制生产内核。
四个公开算子执行接口和注册 Kernel 参数保持原样；新增 CommArgs 字段要求 Host 与设备二进制配套重建。

## 5. profiling 独立开关

三种模式均可设置 profiling.enabled。tool=msprof 仅支持 all，非空自定义选项明确拒绝；
tool=internal 支持 all/selected，复用 SDK 和 tools/profiling。
不引入 CapturePlan、EP 绑定接口或自有格式转换。

Worker 通过官方 ACLprof Init/CreateConfig/Start/Stop/DestroyConfig/Finalize 采集
ACL_PROF_TASK_TIME。single/correctness 在首次真实执行前启动；perf 在正确性预检
与 warmup 后启动，最后一批同步和校验后停止。采集包括批间准备/校验，不能称为
仅 measure 窗口；不额外提交采样调用，不改变 Host batch 的 N 次调用及计时分母。

成功停止并退出 Worker 后，调用官方 msprof --export=on --output=<raw目录>。
每 rank 保留原始目录、API 记录和工具日志，框架只记录命令、退出码及产物文件名，
不解析 CSV/SQLite，不匹配 Kernel、不计算设备延迟、不生成 Trace。
工具失败使整体失败，同时保留独立的 operator_result；导出成功标为 export_executed，
采集完整性标为 unverified，具体数据解释交给官方工具。

### 内部 profiling：初始化固定槽位（2026-11-04 批准）

通信域初始化时，为选中的实际调用创建已有 SDK AclCapture；每槽固定容量，
复用 BuildLayout 计算布局。初始化 descriptor 地址表和实际 lane 数状态区，地址与
容量一次写入 CommArgs 并 H2D。未选中的槽为空，生产公开 EP API 和 Kernel
launch 参数保持不变。不引入 CapturePlan、绑定队列、格式转换或新的 src 文件。

槽位序号 = magic - 初始化时的 magic 起点。启用期间该通信域只用于配置中的 EP 调用，
不混入其它消耗 magic 的调用；总消耗不匹配则销毁失败。magic 是通信域调用标识而非测试
iteration：框架根据固定执行顺序生成完整配置，涵盖 perf 预检、warmup、所有 batch
以及连跑每个阶段；预检/warmup 槽为空。all/selected 只为所选真实调用准备 capture。
通信域使用 TILEXR_PROFILING_SLOTS_FILE 指定的本机配置：第一行是完整槽位数及
设备字节预算；随后每行是 1-based 调用编号、32位十六进制 UUID、双引号路径。
配置在 Worker 初始化通信域之前生成。各 rank 同一次调用 UUID 相同，不同调用不同。

Kernel 从 CommArgs 和 magic 取得 descriptor，使用 Recorder<2> 的事件 1/2、Flush。
每槽按设备 vector core 容量预分配，实际 core 数由 Kernel GetBlockNum 记录到独立
64字节状态槽；导出前检查实际 core 范围，并保留这些实际 lane 的 SDK snapshot。
不在框架复制 blockDim 选择逻辑。launcher 仅在取得 magic 后检查槽位容量及
实际 blockDim 容量，编译未启用打点但请求采集时明确失败。

同一通信域仅支持串行 Host 提交、同一执行 stream；没有每轮 H2D、同步或导出。
总预算包含地址表、lane 状态及所有 capture；初始化不足即失败，不动态扩容。
整个运行的 capture 保留到通信域结束，因此大规模 all 可能超过预算，应减小规模
或显式 selected，不暗中拆批或减少采集。打点开销计入开启采集的 Host 测量。

销毁入口在 stream/context 释放前执行 device completion，再复用 SDK Collect、
ExportSnapshot、Close。验证完整 magic 消耗、选中槽实际执行、每个实际 lane 两条
完整记录且无丢失；导出失败通过销毁入口传播，并参与全 rank 状态约定。
关闭/未启用不创建资源。异常清理不能在设备完成未知时释放采集缓冲区。
原始记录和 mapping 直接交给已安装 tools/profiling，不维护自有二进制格式或解析器。
只记录两个通用事件，覆盖入口调用耗时，不解释 v2 阶段；cycles_per_us 和 clock_basis
必须由使用者确认。跨机按节点分析，不跨设备/主机对齐时钟。

## 6. 单机与跨机共用执行协议

Controller 使用相同的 init、prepare、run-segment/batch、validate、release 语义。
单机直接启动 Worker；跨机通过小型远程代理启动已安装的 Worker 并转发命令和
结果，不在代理重新实现参考计算、测量或执行状态机。

复用 maintained inventory，不复制机器名单；运行 placement 描述每机设备、已安装
框架/库路径和 rank-zero 通信地址。框架不自动构建、上传源码、安装依赖或更新远端。
全局 rank 显式且唯一，按 host-major 排列；用例规模受算子能力限制，不以本地
2..8 卡参数校验代替跨机全局规模契约。

每个正确性检查段及性能批次开始前收齐所有 rank 就绪，存在超时和失联时终止。
Host 时间使用各 rank 本机时钟，仅比较 elapsed；不跨主机相减时间戳。
保存各 rank/batch 数值，汇总 median/min/max，并展示每批最大 rank 平均耗时。
该最大值不称为精确同步的全局 makespan。

核对实际加载库与配套内核身份，记录 CANN/设备/拓扑来源；采用已有元数据能力，
不为框架发明通用产物管理系统。中断、EOF、超时、rank 失败只清理本次拥有的
进程，保留部分报告，不自动重试、不清理其他任务。清理无法确认时明确记录。

## 7. 最小代码组织与变更约束

- controller：参数解析与配置、单一执行状态机、Host 数值汇总和报告。
- worker：公开 API 调用、检查段/批次循环、同步、计时和资源管理。
- adapters：算子注册、输入输出绑定、独立参考计算及最终输出校验。
- 远程代理/传输：启动、转发、超时及所属进程清理。
- 工具接入：官方命令调用和产物关联；分析工具不搬入框架。

仅按实际职责新增文件，不提前建立通用插件系统、动态 DAG 调度器、自动部署系统。
不承诺行数指标，以删除重复职责和保持接口清晰为目标。生产 API/打点格式变动
单独说明必要性与兼容性；不恢复旧 UniTest 专用 profiling 公开 ABI。

## 8. 验收与交付顺序

1. 基线构建安装；single 独立 Dispatch/Combine 数值校验。
2. config、correctness 检查点和 Host batch；验证修改配置无需重编译。
3. 步骤绑定、Dispatch + Combine、Dispatch + Barrier；无有效 Barrier 接口不得
   用 Host 同步替代，也不得声称该组合已交付。
4. 外部 msprof 官方启停与导出，验证预检/warmup 在采集之外、工具错误传播；
   内部打点使用固定槽位和现有 SDK，覆盖 all/selected，逐调用与 rank 检查完整性。
5. 跨机复用同一执行流程，验证全 rank 正确性、批次报告及失败清理。

各阶段用小提交交付，每个提交含必要文档；本次设计修订先留在工作区供审阅，
后续交付按用户指定范围提交，不把旧扩展整提交恢复。
官方 CANN 9.1 为兼容基线，9.2 分别记录；Host 编译不证明设备正确性。
硬件验收覆盖 single/correctness/perf、单算子/连跑、单机/跨机和 profiling 开关。
检查点覆盖非二次幂末轮和二次幂末轮；性能验证正数计时、完整批次及校验成功。
跨机测试检查 rank 失败、失联和中断，不用本地协议 fixture 冒充实机结果。
复用相同环境已有 HCCL 资格记录，不为本次改动重复运行官方 HCCL Test。

报告必须区分：框架构建、算子正确性、Host 性能、工具采集/分析、真实跨机验收。
能力未实现、工具未接通或硬件未运行时单列缺口，不表述六项全部完成。
