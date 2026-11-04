# UniTest 不带内部 profiling 的实施计划

> 历史实施计划：后续固定槽位方案已改变部分阶段限制。当前行为见 [UniTest 设计](../specs/unitest.md)，实机证据见[验证记录](../validation/unitest.md)。

2026-11-04 用户批准以 `33184801` 为基础；旧本地/远端提交已有备份。
本轮不修改 src，不恢复 EP 采集绑定、CapturePlan 或通信域 profiling 改造。
实现按功能拆分提交：

1. 官方 ACLprof 采集边界及 msprof 导出；删除内部 profiling 配置入口。
2. NodeAgent 有界协议和全局 rank Worker 启动，EOF 只清理本轮进程。
3. 读取 maintained inventory 和显式 placement，跨机推进同一执行状态机，
   每节点官方导出，保留全局结果与节点产物路径。
4. 同步设计、使用方法和验证边界。

保留 single 数值比较、correctness 累计检查点、Host batch perf 和真实
Dispatch→Combine 绑定。外部采集只支持 all，不解析官方 profiler 数据。

本轮验证：WSL CANN 9.1/9.2，分别针对已有 TileXR 安装独立构建并安装框架；
Worker C++14 编译/链接通过，不代表重新编译生产库或 NPU 正确性通过。
Host unittest 覆盖采集顺序、工具失败、配置拒绝、placement、缺失 rank，
以及真实 NodeAgent/fixture Worker 子进程的正常退出和 EOF 回收。
这些 fixture 不加载 CANN，不证明实际跨机传输或官方采集产物完整。
2026-11-04 已对 `5e84bdb8` 完成 CANN 9.2 独立构建安装及 A5 两卡短验收：
三个 single、连跑 correctness 3 轮、Host batch perf W1/N2/B1、同机真实 NodeAgent
均通过。Dispatch 官方采集启停及导出退出 0；初次导出有只读 /dev/shm 异常。
后续在私有 tmpfs 中离线重导出，异常消失，但 Cluster Tuning 未完成警告仍在，
完整分析待定位，不能宣称样本完整性通过。未修改 src，未重编译生产库。
初始化限时 60 秒，其余阶段 20 秒，未超时；逐项确认设备恢复空闲后停止实机验证。
详细身份、产物路径和临时验收脚本失败记录见 docs/validation/unitest.md 的两卡验收节。
真实多节点、CANN 9.1 实机、官方采集完整性、设备 Barrier 仍须另行验收。
不复用旧版本实机结果作为本轮结论，不重复运行已有匹配 HCCL 资格记录。

## 后续已批准：固定 GM 内部 profiling

1. 单独恢复 SDK 64字节缓存行隔离修复，运行 SDK Host 测试。
2. 在现有 comm 文件初始化固定 capture/descriptor 表并通过销毁入口导出；
   同步新增 CommArgs 字段、文档和预算/失败边界。
3. EP 两个入口复用 Recorder<2>，launcher 只检查容量，不改执行 API 和 launch 参数。
4. 框架在通信域初始化之前生成 magic 槽配置，结束后直接调用现有分析工具；
   同步跨机与 all/selected 支持，并补行为验证、9.1/9.2 构建证据。

本节取代上文“本轮不修改 src”限制；允许范围以已批准的八个生产接线文件为准。
每项独立小提交，生产 Host/设备二进制必须配套构建；实机与跨机证据单独记录。

验证记录：固定槽位版本在 WSL CANN 9.1/9.2 完成生产库、EP Kernel 和框架的构建安装；
14 项框架 Host 检查、4 项 SDK 检查及 launcher 开关两种 Host 检查通过。
工具接入通过两 rank records fixture 验证，不代表 NPU 实测；详见 docs/validation/unitest.md。

2026-11-04 追加固定槽位实机验收：源码 e0618746，CANN 9.2 统一重建配套生产库、
设备对象与框架，a5 两卡完成 5 项关闭采集数值用例和 6 项内部采集用例。
涵盖三个 single、连跑 correctness 3 轮、Host batch perf、两批 all 和非连续 selected。
20 个 capture/40 个 rank records 通过原有工具完整性复核与报告生成；各 rank 每槽
64 lane、每 lane 两条记录，无丢失。预检/warmup 不采集，所有请求集合与 UUID 对应。
14 项框架/4 项 SDK Host 检查通过，越界/时钟/预算在不占卡路径拒绝。未发现需要修复的问题。
实机阶段限时遵守 60/20 秒约定，逐项确认两卡恢复空闲后停止占卡。
用户已将跨机及其它能力本轮标记为未验收、暂不执行；时钟未校准，不宣称准确阶段延迟。
细节及证据路径见 docs/validation/unitest.md 的“固定槽位版本 A5 两卡验收”，PR 保持 Draft。
