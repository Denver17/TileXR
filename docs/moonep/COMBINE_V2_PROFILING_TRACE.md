# MoonEP profiling 边界

MoonEP 私有 Dispatch/Combine timestamp record、writer、workspace 区、构建开关、
运行参数和 trace 解析器已删除；不保留兼容层。旧记录的复现方式只能从 Git 历史查阅，
不适用于当前 binary。

当前 MoonEP 保留框架 NPU profiler、Host Dispatch/Combine stage barrier、业务 timeout、
DFX 错误槽和 kernel status。DFX/status 是错误诊断，不是阶段计时。
UDMA registration Profile API 描述 QP 注册布局，亦不属于被删除的计时模块。

MoonEP 当前不提供上述私有记录对应的设备阶段采集替代接口；不能把 msprof 的整 kernel
时间称为阶段归因。新增设备阶段测量应直接使用公共 `<tilexr/profiling.h>` SDK，
按 [SDK 接入合同](../specs/profiling.md) 绑定真实 stream 并提供 mapping、timer 换算及
完整 lane 证据；不能恢复私有 record 或旧环境变量。

常规多机功能与性能流程见 [COMBINE_V2_MULTIHOST_PERF.md](COMBINE_V2_MULTIHOST_PERF.md)。
移除参数后 native ABI 已改变，Host、kernel、安装头文件和 Python ctypes 必须配套重建。
