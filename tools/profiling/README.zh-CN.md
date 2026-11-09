# TileXR profiling

[简体中文](README.zh-CN.md) / [English](README.md)

独立的 Ascend C 算子 profiling 工具：使用 C++14 SDK 按事件名在 Kernel 中打点，再用 Python
离线分析。无需安装 TileXR 通信运行时、collectives 或 EP。

## 文档导航

| 用途 | 简体中文 | English |
|---|---|---|
| 概念、接入与验证命令 | [工具指南](docs/USER_GUIDE.zh-CN.md) | [User guide](docs/USER_GUIDE.md) |
| 最简可运行算子 | [Add4 示例](../../examples/profiling/README.zh-CN.md) | [Add4](../../examples/profiling/README.md) |

详细参考（英文）：[API](docs/API_REFERENCE.md)、[格式与统计](docs/FORMAT.md)、
[高级用法](docs/ADVANCED.md)。高级用法集中说明 manifest、比较、capture set、workspace lease、
采样、校准与 collectives 接入。

## 目录与依赖

`python/` 为离线包，`docs/` 为维护指南。SDK 头文件保留在仓库根目录下的
`src/include/tilexr/profiling*`，独立构建入口为 `src/profiling`；Add4 源码、CMake
入口和脚本保留在 `examples/profiling`。SDK 独立 CMake 工程安装头文件和 Add4
源码/README；生成报告、二进制记录和
验证证据保存在源码树外。公开名称为 `<tilexr/profiling.h>`、`TileXR::profiling` 和 `tilexr_profiling`。

| 组件 | 要求 |
|---|---|
| 离线分析器 | Python ≥3.9；无需 CANN/NPU；源码安装需预装 `setuptools>=61` 和 `wheel` |
| SDK | 安装需 CMake ≥3.16；C++14 Host 与 CANN ACL；Ascend C 设备编译；Linux 文件导出 |
| Add4 | 已安装 SDK 与 CANN 9.1；维护构建面向 Ascend950；执行需兼容硬件及已确认的 timer 换算 |

SDK 未硬编码 Ascend950 限制。每种 SoC 都需要其支持的编译目标和硬件验证；Add4 的
`dav-c310-vec` 不是通用目标。

## 安装并离线分析

以下 Bash 命令从仓库根目录执行，安装与运行使用同一 Python；可按需先创建虚拟环境。

```bash
python3 -m pip install --no-build-isolation --no-deps tools/profiling
# 或安装已有 wheel：
# python3 -m pip install --no-index --no-deps /path/to/tilexr_profiling-0.1.0-py3-none-any.whl
python3 -m tilexr_profiling --mapping operator.mapping.json \
  --records capture.records.bin --output-dir profile-report \
  --attribute kernel_version=my-version
```

使用实际 mapping/records 和新的或空的输出目录。默认输出为 `analysis.md`、`report.html`、
`perfetto_trace.json`。先检查完整性，再解释耗时。退出码 0 表示已提供输入完整，1 表示致命
失败，2 表示报告不完整或命令行用法错误。独立的预期参与者检查需要 manifest，详见
[工具指南](docs/USER_GUIDE.zh-CN.md#6-生成了什么应该怎样阅读)。mapping 的 `cycles_per_us`
须按目标 timer 确认；[Add4 模板](../../examples/profiling/profiled_add4.mapping.template.json) 故意留为 `null`。

## 安装采集 SDK

```bash
cmake -S src/profiling -B build/profiling-sdk
cmake --install build/profiling-sdk --prefix "$PWD/install-profiling"
```

使用方调用 `find_package(TileXRProfiling 1 CONFIG REQUIRED)`、链接 `TileXR::profiling`，
配置时传入 `-DCMAKE_PREFIX_PATH=/path/to/install-profiling`。CANN 链接、设备编译和 Kernel
启动由应用提供。完整接入见[工具指南](docs/USER_GUIDE.zh-CN.md)或 [Add4](../../examples/profiling/README.zh-CN.md)。

Host/Kernel 须成对部署 DeviceView ABI 2。历史 records.v1/mapping.v1 与原四参数报告 API
保持兼容。已移除的 `tools.profiling` 入口改用 `tilexr_profiling`。
