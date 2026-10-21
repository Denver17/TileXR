# TileXR profiling 使用指南

[English](README.md) | [简体中文](README.zh-CN.md)

## 定位与依赖

本工具面向 **CANN 应用独立使用**。TileXR 是使用方之一，不是必需运行环境。
从此仓库安装 SDK/Python 包，不需要构建或安装 TileXR 通信运行时、collectives 或 EP。
`tilexr/` 头文件路径、`TileXR::Profiling` 命名空间和 `TileXR::profiling` CMake target
是 SDK 的公共名称，不表示依赖 `libtile-comm.so`。

| 组件 | 依赖 | SoC 范围 |
|---|---|---|
| 离线分析器 | Python ≥3.9 | 不要求设备或 SoC，不依赖 CANN 或 TileXR 运行时 |
| 采集 SDK | Host C++14 + CANN ACL；设备插桩使用 Ascend C；文件导出使用 Linux | 没有写死 Ascend950 门禁；每个目标仍需编译、运行及 timer 验证 |
| Add4 示例 | 独立 SDK + CANN，不依赖 TileXR 运行时 | 当前示例构建路径仅支持 Ascend950 |
| TileXR 接入 | TileXR 应用 + 同一 SDK | 可选使用方，不是外部算子的前置条件 |

当前真实设备编译证据是 Ascend950 / CANN 9.1，未执行 NPU。这不等于所有 CANN
版本或所有 Ascend SoC 已验证。其他 SoC 应使用其支持的 Ascend C 编译目标，确认 timer
换算，并在对应硬件验证真实记录；不能将 Add4 的 `dav-c310-vec` 当成通用编译设置。

## 1. 按目标选择流程

| 目标 | 所需环境 | 入口 | 成功标准 |
|---|---|---|---|
| 分析已有采集文件 | Python ≥3.9、mapping 和配套 records | 第 2 节 | 三种报告齐全，完整性状态符合预期 |
| 为算子加入采集 | Host C++14、目标 CANN 工具链；运行需要 NPU | 第 3 节 | 每次 kernel 调用独立采集，再离线分析 |
| 运行完整示例 | Linux、CANN 9.1、Ascend950、已确认的 timer 换算 | [Add4 中文](../../examples/profiling/README.zh-CN.md) / [English](../../examples/profiling/README.md) | 数值与 lane 检查成功，报告 complete |
| 无 NPU 验证 | Python、CMake ≥3.16、C++14；真实编译检查另需 CANN | 第 4 节与 [测试指南](TESTING.md) | 对应验证层的实际测试通过 |

除特别说明，以下 shell 命令均在仓库根目录用 Bash 执行。将示例输入路径替换为
自己的文件；无 NPU 时不要运行采集示例。SDK 安装和离线分析不需要设备。

离线分析器读取 `tilexr_profiling_mapping.v1` JSON 与
`tilexr_profiling_records.v1` 二进制文件，运行时不依赖 CANN、NPU、TileXR 动态库
或源码树。[当前设计](../../docs/specs/profiling.md) 说明架构、所有权和不变量；
[测试指南](TESTING.md) 提供分层测试、验收标准及证据模板。

## 2. 安装并离线分析

需要隔离环境时，先执行 `python3 -m venv .venv` 和 `source .venv/bin/activate`。
源码安装要求预先安装 `setuptools>=61` 和 `wheel`，分析器自身没有第三方运行依赖。
以下两种安装方式任选一种：

```bash
# 从源码安装，使用已经准备好的构建工具。
python3 -m pip install --no-build-isolation --no-deps tools/profiling
# 或安装本地 wheel，不访问软件源。
python3 -m pip install --no-index --no-deps tilexr_profiling-0.1.0-py3-none-any.whl
```

准备 mapping 与同一次 capture 的 records 后执行：

```bash
python3 -m tilexr_profiling --mapping operator.mapping.json \
  --records capture.records.bin --output-dir profile-report \
  --attribute kernel_version=my-version
```

多 rank 时重复 `--records`，只传同一个 capture UUID 的文件。输出目录必须不存在或为空。
产物为 `perfetto_trace.json`、`analysis.md`、`report.html`，均可离线使用：浏览器打开
HTML、直接阅读 Markdown，或将 trace 导入兼容的 Perfetto 查看器。

| CLI 退出码 | 含义 | 应检查什么 |
|---|---|---|
| 0 | 提供的输入 complete | 三种报告、诊断，以及所需 rank 是否全部提供 |
| 1 | 致命输入或输出失败 | 错误信息，不应出现一组成功发布的报告 |
| 2 | capture incomplete，仍生成报告；也可能是 argparse 用法错误 | 同时检查诊断及产物，用法错误不产生报告 |

退出 0 仅针对已提供的输入。当前格式没有参与者 manifest，调用方必须额外核对 required
ranks；不同 kernel 调用的记录不能混在一起，即使事件 ID 相同。

mapping 使用非零事件 ID，每个 lane 恰好一个 critical interval。
`cycles_per_us` 必须来自目标 timer 的可信换算；fixture 或其他 SoC 的数值不是校准证据。
完整 mapping 示例与统计定义见 [FORMAT.md](FORMAT.md)。也可参考
[Add4 模板](../../examples/profiling/profiled_add4.mapping.template.json)，其中
`cycles_per_us: null` 故意不可直接分析，必须先填写已确认的值。

对应 Python API：

```python
from pathlib import Path
from tilexr_profiling import generate_reports

result = generate_reports(Path("operator.mapping.json"),
                          [Path("capture.records.bin")], Path("profile-report"),
                          attributes={"kernel_version": "my-version"})
print(result.status)
```

## 3. 安装并接入采集 SDK

SDK 是独立的 header-only C++ 接口，安装不需要构建 TileXR 或初始化其子模块：

```bash
cmake -S src/profiling -B build/profiling-sdk
cmake --install build/profiling-sdk --prefix "$PWD/install-profiling"
```

引入 `<tilexr/profiling.h>`，使用 `TileXR::Profiling::AclCapture`。
下面是**接入伪代码，不是可独立编译程序**：调用方创建 ACL context/stream，
实现 `check` 和 `LaunchMyKernel`，并按每种 API 自己的返回类型检查错误。

```cpp
TileXR::Profiling::AclCapture capture;
check(capture.Initialize(stream, logicalStreamCount));
check(LaunchMyKernel(stream, capture.DeviceArgs())); // 调用方自己的 launcher
check(capture.Save("capture.records.bin"));
check(capture.Close());
```

CMake 消费方使用 `find_package(TileXRProfiling 1 CONFIG REQUIRED)`，链接
`TileXR::profiling`，配置时提供 `-DCMAKE_PREFIX_PATH=/path/to/install-profiling`。
该接口 target 仅提供头文件和 C++14 要求，不会隐式链接 ACL；应用需自行查找并链接
CANN。完整可构建示例见 [Add4](../../examples/profiling/README.zh-CN.md)。

外部 CANN 应用可采用以下最小 CMake 接线，仅使用已安装 SDK prefix 和 CANN。
`my_operator.cpp` 是调用方自己的源码，负责 ACL 初始化与 kernel 启动；以下仅说明
构建依赖，不是完整算子实现。配置前先 source 实际 CANN 环境。

```cmake
cmake_minimum_required(VERSION 3.16)
project(MyProfiledOperator LANGUAGES CXX)
find_package(TileXRProfiling 1 CONFIG REQUIRED)
find_path(CANN_ACL_INCLUDE_DIR acl/acl.h HINTS "$ENV{ASCEND_HOME_PATH}/include")
find_library(CANN_ACL_LIBRARY ascendcl HINTS "$ENV{ASCEND_HOME_PATH}/lib64")
add_executable(my_operator my_operator.cpp)
target_compile_features(my_operator PRIVATE cxx_std_14)
target_include_directories(my_operator PRIVATE "${CANN_ACL_INCLUDE_DIR}")
target_link_libraries(my_operator PRIVATE TileXR::profiling "${CANN_ACL_LIBRARY}")
```

在仓外项目执行 `cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/install-profiling`。
业务 kernel/launcher 所需的其他 CANN 编译和链接设置由应用自行提供。SDK 安装后，
该消费方不需要 TileXR 源码树或动态库。采集文件可用单独安装的 Python wheel 在本机
或另一台没有 CANN 的机器上分析。

设备侧在测量区间之前构造 `TileXR::Profiling::Recorder<2>`，在业务前后分别调用
`RecordMarker<0,1>()`、`RecordMarker<1,2>()`，最后执行
`Flush(profileGM, logicalStream)`。设备编译时设置 `TILEXR_ENABLE_PROFILING=1`；
未定义或为 0 时禁用插桩，其他值编译失败。Host 生命周期类型与磁盘格式不随宏变化。
每个 lane 只有一个 writer，每次 capture 只测量一次 kernel 调用，warmup 不采集。
marker 不会替业务插入流水屏障，mapping 的 timer 换算没有通用默认值。

`DeviceArgs()` 是设备内存中的描述符地址，不是 Host 栈对象。异步接入需要将 capture
保留到原有安全完成点；`Save`、`Collect` 以及析构可能等待绑定 stream。原 context/stream
必须保持到 `Close()` 成功，不能因 launch 或 Save 失败就销毁借用资源。可恢复错误的
重试合同见 [API_REFERENCE.md](API_REFERENCE.md)。

多 rank 调用方自行分发同一个 fresh、非零 capture UUID，设置不同 rank 和输出路径，
分析时核对所有必需文件。SDK 不协调 ranks。协议、时钟域与统计定义见
[FORMAT.md](FORMAT.md)。

## 4. 按环境能力验证

先准备构建工具；以下为源码行为与 Host 验证入口：

```bash
python3 -m pip install --no-build-isolation --no-deps -e tools/profiling
python3 -B -m unittest discover -s tests/profiling/python -p 'test_*.py'
python3 scripts/ci/check_profiling_python.py
cmake -S src/profiling -B build/profiling-host -DTILEXR_PROFILING_BUILD_TESTS=ON
cmake --build build/profiling-host
ctest --test-dir build/profiling-host --output-on-failure -L profiling_host
```

发行检查额外要求预装 `build`、`setuptools>=61`、`wheel` 和可用的 venv/ensurepip。
默认在临时仓外目录验证 wheel/sdist；需要保留证据时，用 `--work-dir` 指定**仓外目录**。
CI 配置覆盖 Python 3.9/3.12 不代表对应版本已运行通过，必须检查实际结果。

CTest 标签为 `profiling_host`、`profiling_python`，配置真实 CANN 后另有
`profiling_cann`。目前即使启用 Add4 也不会自动注册 NPU 测试；
`profiling_npu` 选择结果为空不能算通过。Host/mock 测试不能证明设备可见性、
真实设备编译兼容性、marker 开销、寄存器 spill 或 UDMA 数据传输。

有 CANN 9.1、无 NPU 时按 [测试指南第 5 节](TESTING.md) 构建和验证，
不要运行 Add4 的 `run_demo.sh`。Windows 挂载到 WSL 的脚本应采用 LF 换行；
CRLF 语法错误不属于 kernel 或编译器失败。

## 5. 常见问题与人类 / AI 交接

| 现象 | 检查或下一步 |
|---|---|
| `No module named tilexr_profiling` | 确认安装与执行使用同一解释器；执行 `python3 -c 'import tilexr_profiling; print(tilexr_profiling.__file__)'` 检查来源 |
| 输出目录被拒绝 | 使用新目录或空目录，不覆盖已有报告 |
| 时钟换算缺失或非法 | 填写已确认的正数，并保留来源，不猜频率 |
| UUID 混合或 stream 重复 | 只分析一次调用，核对 rank/lane 身份与输入选择 |
| 退出 2 | 结合诊断与产物区分 incomplete 和命令用法错误 |
| 找不到 CANN runtime 头 | source 目标 CANN 9.1 环境，检查真实安装布局，使用干净构建目录配置 |

交接时记录：源码 commit 与 dirty diff、环境、准确命令和 cwd、输入文件与 required ranks、
capture UUID、时钟换算与依据、退出码、诊断及产物路径。明确写出未运行的验证层，保留
既有修改，使用新的输出路径。验收以 [TESTING.md](TESTING.md) 为准，合同以
[当前设计](../../docs/specs/profiling.md) 为准，不能用 mock 结果替代硬件证据。
修改命令、前提或成功标准时同步维护中英文，避免 AI 沿用另一语言的旧步骤。

迁移旧项目时，安装本包，将 `tools.profiling` 导入和命令替换为 `tilexr_profiling`。
旧 Python 入口及 communicator 持有的多 launch exporter 已移除；历史 v1 文件仍可读取。
