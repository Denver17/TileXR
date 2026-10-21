# 独立 Add4 profiling 示例使用说明

[English](README.md) | [简体中文](README.zh-CN.md)

## 1. 环境要求与示例行为

Ascend950 是当前示例 kernel 构建路径的限制，不是 profiling SDK 或离线分析器的限制。
本示例不要求安装 TileXR 运行时。其他 CANN 应用/SoC 的独立接入与验证边界见
[主指南](../../tools/profiling/README.zh-CN.md)。

需要 Linux、CMake ≥3.16、支持 C++14 的 Host 编译器、实际 CANN 9.1 工具链、
已安装的 profiling SDK 和 Python 包（Python ≥3.9）。构建需要目标编译器；
运行还需要明确选定的 Ascend950 卡、真实驱动，以及已确认的 `GetSystemCycle()` 换算。
没有 NPU 时仅构建和安装，不启动可执行程序或 runner。

示例只使用公共 profiling 头文件和 CANN，不使用 communicator 或 TileXR 动态库。
应用创建 ACL device/context/stream，调用 `AclCapture::Initialize(stream,4)`，
将 `DeviceArgs()` 传给 kernel，随后调用 `Save(path)` 和 `Close()`。
ACL 初始化、业务内存、数值校验及 kernel 启动均由应用负责。

4 个 AIV block 分别对四路、每路 2,048 个 float32 输入求和。每个 block 独占一个
逻辑 lane，通过 `Recorder<20>` 写入 20 条 marker。输入复制区间顺序重复，
`accumulate_window` 内三个显式 partial-sum 实例存在重叠。事件 ID 1/2 定义唯一
critical 区间 `kernel_total`。业务输出复制依赖完成后才记录最后的 marker，再 Flush；
marker 不会自动插入业务流水屏障。

## 2. 使用已安装 SDK 构建和安装

先按 [主使用指南](../../tools/profiling/README.zh-CN.md) 安装独立 SDK 和 Python 包，
再 source 实际 CANN 9.1 环境。SDK 会将完整示例安装到
`share/tilexr-profiling/examples/add4`。将该目录复制到干净的仓外项目，验证安装消费。
下面的 `/path/to/add4` 和 `/path/to/sdk-prefix` 需要替换为实际路径：

```bash
cmake -S /path/to/add4 -B /tmp/add4-build \
  -DCMAKE_PREFIX_PATH=/path/to/sdk-prefix \
  -DTILEXR_PROFILING_SOC=Ascend950
cmake --build /tmp/add4-build -j2
cmake --install /tmp/add4-build --prefix /tmp/add4-install
# 仅硬件环境执行；/tmp/new.records.bin 必须尚不存在。
TILEXR_ADD4_DEVICE_ID=0 /tmp/add4-install/bin/tilexr_profiled_add4_demo /tmp/new.records.bin
```

构建生成纯 AICore ELF，独立安装到 `${CMAKE_INSTALL_LIBDIR}/tilexr/kernels`。
Host 根据 `/proc/self/exe` 相对定位 kernel，使用 `rtDevBinaryRegister`、
`rtFunctionRegister` 注册，再通过 `rtKernelLaunchWithFlagV2` 启动。
可执行程序不嵌入 kernel 对象，也不依赖 TileXR 私有 loader。bin/lib 使用相对安装
目录，使整个 prefix 搬迁后仍可相对定位；构建树中也提供相同的相对 kernel 布局。
正常运行时必须解析到真实 runtime/driver 库，不能使用 devlib stub 代替驱动。

## 3. 在硬件上采集并生成报告

在复制后的 Add4 源码目录中运行 runner 前，先在同一 shell 中 export 下列变量，
值必须来自当前实际环境：

| 变量 | 要求 |
|---|---|
| `ASCEND_HOME_PATH` | 当前目标 CANN 安装路径，由其环境脚本设置 |
| `TILEXR_PROFILING_PREFIX` | 已安装的独立 SDK prefix |
| `TILEXR_PROFILING_SOC` | `Ascend950`，当前唯一支持的示例目标 |
| `TILEXR_PROFILING_CYCLES_PER_US` | 已确认的 `GetSystemCycle()` 正数换算 |
| `TILEXR_PROFILING_CLOCK_BASIS` | 支持该换算的匹配版本资料或校准依据 |
| `TILEXR_ADD4_DEVICE_ID` | 选定卡索引，默认 `0` |

填写全部必需变量后执行，输出目录必须不存在：

```bash
cd /path/to/copied-add4
bash run_demo.sh /path/to/new-add4-evidence
```

runner 会构建、运行示例并分析记录，**不是仅编译检查**。
可选 `TILEXR_ADD4_BUILD_DIR` 指定构建目录（默认 `$PWD/build-profiled-add4`），
`TILEXR_ADD4_BUILD_JOBS` 指定并行度（默认 `2`）。

可额外设置 `TILEXR_PROFILING_CANN_VERSION`、`TILEXR_PROFILING_DRIVER_VERSION`、
`TILEXR_PROFILING_FIRMWARE_VERSION`。未知值保留 unknown，不自动补猜。
`environment.txt` 保存平台/编译器、标准、宏、kernel/SDK 版本及换算依据；
`operator.log` 记录 SoC、外置 kernel 路径和数值/记录检查。runner 不预留设备，
需按正常环境策略自行选卡；该独立示例不执行通信环境资格测试。

mapping 模板中的 `cycles_per_us: null` 故意不能直接分析，runner 会填入调用方确认的值。
不要复制其他 SoC 的 timer 频率。报告使用已安装的标准入口
`python3 -m tilexr_profiling`，并包含调用方提供的 provenance。
事件形式、层级和统计定义见 [FORMAT.md](../../tools/profiling/FORMAT.md)，
SDK 所有权及错误处理见 [API_REFERENCE.md](../../tools/profiling/API_REFERENCE.md)。

## 4. 成功标准与验证边界

硬件运行必须同时满足：

- `operator.log` 数值校验成功，4 个 COMMITTED lane，每 lane 20 records，dropped=0。
- `capture.mapping.json` 与 `capture.records.bin` 事件匹配，时钟换算已确认；保留
  `environment.txt` 和实际 Host/kernel 二进制来源。
- `report/` 下同时有 `perfetto_trace.json`、`analysis.md`、`report.html`，分析结果
  complete；打开 HTML/Markdown 检查诊断，不只看文件是否生成。

只有显式硬件运行成功才能证明上述数值与记录结果。默认 CTest 不注册 NPU 测试。
2026-10-21 的 WSL / CANN 9.1 验证已完成源码树与仓外已安装 SDK 消费的构建和安装，
见 [验证记录](../../tools/profiling/TESTING.md)。实际设备编译覆盖 Ascend950
`dav-c310-vec`、`gnu++17`，以及 profiling 宏未定义/0/1；非法 2 被拒绝。
Host 使用 C++14；该次没有运行 NPU。

设备 C++14、其他 SoC、指令可见性、spill 和开销仍未验证。低开销结论还需在同一目标、
输入和同步边界比较“编译禁用”“仅本地 marker”“marker+flush”，保留原始样本与资源/
spill 证据，并覆盖足以暴露 spill 的 recorder 容量。

## 5. 排错与交接

输出路径存在就换新路径；timer 换算缺失就补充真实目标依据，不猜频率；runtime 头文件
缺失时检查目标 CANN 环境和安装布局，并使用干净构建目录；Bash 脚本应采用 LF 换行。
其他失败保留首条失败命令、退出码/日志、源码 commit/diff、toolkit/compiler 身份、
选定设备及实际 Host/kernel 路径，明确区分编译证据与运行证据。行为变更时同步两种语言。

修改示例时，kernel 数据必须存活到注册完成；借用的 stream/context 必须保持到 `Close()`
及 capture 析构结束。launch 或 Save 错误不意味着没有在途工作；失败清理应先同步，
同步失败时保守保留资源，不能提前释放业务内存。
