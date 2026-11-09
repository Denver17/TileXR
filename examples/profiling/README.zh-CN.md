# 最简 Add4 打点示例

[简体中文](README.zh-CN.md) | [English](README.md)

四个 AIV block 分别计算四路 FP32 输入之和，仅依赖独立 profiling SDK 和 CANN。

```text
add4/
├── *.cpp、CMakeLists.txt、*.json、*.py、run_demo.sh  示例与报告辅助代码
├── README.md
└── README.zh-CN.md
```

Host 只展示 `AclCapture`、`CaptureConfig`、`Initialize`、`DeviceArgs`、`Save` 和
`Close` 的直接调用。最简 Host 不做返回值、SoC 和数值校验，不回读输出，也没有校验分支。
Python 准备报告的 manifest；可选验收、校准代码
仍独立于 Host/kernel。

Kernel 直接以 `kEventAccumulateBegin`、`kEventPartialSum0End` 等事件名常量调用
`Recorder<17>::RecordMarker`，最后 `Flush`，由 SDK 内部
`TILEXR_PROFILE_ACTIVE` 控制设备记录。CMake 将同一个数值开关
`TILEXR_ENABLE_PROFILING` 传给 Host/kernel；Host 将其赋给 `CaptureConfig.enabled`。
打点前没有谓词，也没有两套 ON/OFF 启动分支。OFF 仍执行同一个 Add4 kernel，不保存 records。

[唯一 mapping](profiled_add4.mapping.template.json) 使用 **9U 槽位、事件 20**
作为共享起点，三个 partial_sum 分别以 **201/202/203** 结束，都是从同一原始记录
开始的累计窗口。每 lane 实际 17 条记录，槽位连续编号为 0–16，Recorder 容量为 17。
输入拷贝依次复用 10/11。业务同步屏障保留；打点本身不增加屏障。

## 构建与运行

需要 Linux、CMake ≥3.16、Host C++14、CANN 9.1，以及已安装的
[SDK 和 Python 包](../../README.zh-CN.md)。本示例的编译目标为 Ascend950，
不代表 SDK 仅支持该 SoC。

```bash
# 仓库根目录执行；已安装副本则使用 add4 作为源码目录。
cmake -S examples/profiling -B build/add4 \
  -DCMAKE_PREFIX_PATH=/path/to/profiling-prefix \
  -DTILEXR_PROFILING_SOC=Ascend950
cmake --build build/add4 -j2
cmake --install build/add4 --prefix /tmp/add4-install
# 仅在硬件环境执行，选择空闲设备和新的输出文件。
TILEXR_ADD4_DEVICE_ID=0 /tmp/add4-install/bin/tilexr_profiled_add4_demo /tmp/new.records.bin
```

`TILEXR_ENABLE_PROFILING` 默认 ON；设为 OFF 则关闭采集。
AICore binary 独立安装到 `${CMAKE_INSTALL_LIBDIR}/tilexr/kernels`，相对可执行文件加载，支持整体搬迁。
使用 `rtDevBinaryRegister`、`rtFunctionRegister` 注册及 `rtKernelLaunchWithFlagV2`
启动，不嵌入 kernel 对象。

若需构建、运行并生成报告，先加载 CANN 环境，设置 `TILEXR_PROFILING_PREFIX`、
`TILEXR_PROFILING_CYCLES_PER_US`、`TILEXR_PROFILING_CLOCK_BASIS`，再运行：

```bash
bash examples/profiling/run_demo.sh /tmp/new-add4-results
```

必须提供已确认的 timer 换算及来源，模板中的 `cycles_per_us: null` 故意不可直接分析。
脚本输出 records、填好换算的 mapping、`manifest.json`、运行日志和三种报告。
`manifest.py` 在运行前声明四个预期 lane 和字节数，采集后绑定实际 capture UUID。
默认脚本和 `npu_check.py` 均携带 manifest 分析。records 只包含时间戳和事件 ID，
没有工作量字节数；漏传 manifest 就无法计算 bytes 和带宽。

每 lane 处理 2048 个 FP32 元素，每个数组为 8192 字节。`kernel_total` 计四次 GM 读和
一次 GM 写，`copy_input` 计四次 GM 读，`copy_out` 计一次 GM 写。每次 Add 计两次 UB 读和
一次 UB 写，因此 `accumulate_window` 计九个数组。三个累计 `partial_sum` 窗口共覆盖
1+2+3 次 Add，计 18 个数组，与三个窗口的 inclusive 耗时之和对应。所有指标使用
`per_lane_total`，不能将父子行相加。此处是逻辑 lane-time 带宽，不是物理 GM/UB 实测吞吐；
完整语义保留在报告的折叠数据中。

最简 Host 不导出 health 观测，其状态保持 unverified。可选诊断能力见
[完整 API](../../tools/profiling/docs/API_REFERENCE.md)。

## 结果与维护

采集记录、mapping、manifest 和报告保留在该次运行指定的输出目录，不进入功能 PR。SDK 安装只提供示例源码和说明，排除生成结果。

capture 清理结束前，ACL device/stream 和 kernel 注册必须保持有效。`Save` 和清理前
保留显式 stream 同步：OFF 关闭采集同步，但仍会启动 kernel。容量、丢弃和事件序列
检查放在 SDK 协议测试或可选验收脚本中。验收和 timer probe 检查采集，不验证 Add4 数值结果。

使用事件名常量时保持数值 ID 与 mapping 一致；配对依赖 ID，不依赖 C++ 名称。
修改打点后检查实际导出的事件序列。
