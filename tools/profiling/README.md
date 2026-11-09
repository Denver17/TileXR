# TileXR profiling

[English](README.md) / [简体中文](README.zh-CN.md)

A standalone profiling tool for Ascend C operators: record named Kernel events
with a C++14 SDK, then analyze them offline with Python. No TileXR communication
runtime, collectives or EP installation is required.

## Documentation

| Purpose | English | 简体中文 |
|---|---|---|
| Concepts, integration and validation commands | [User guide](docs/USER_GUIDE.md) | [工具指南](docs/USER_GUIDE.zh-CN.md) |
| Minimal runnable operator | [Add4](../../examples/profiling/README.md) | [Add4 示例](../../examples/profiling/README.zh-CN.md) |

Detailed references: [API](docs/API_REFERENCE.md), [formats and statistics](docs/FORMAT.md),
and [advanced usage](docs/ADVANCED.md) (English). Advanced usage groups manifests,
comparisons, capture sets, workspace leases, sampling, calibration and collectives.

## Source layout and requirements

`python/` contains the offline package and `docs/` the maintained guides.
The SDK headers remain in `src/include/tilexr/profiling*`, with their standalone
build entry at `src/profiling`. Add4 source, CMake entry and scripts remain in
`examples/profiling`. These paths are relative to the repository root.
The standalone SDK CMake project installs the headers and Add4 source/READMEs. Keep generated
reports, records, and validation evidence outside the source tree.
Public names are `<tilexr/profiling.h>`, `TileXR::profiling` and `tilexr_profiling`.

| Component | Requirements |
|---|---|
| Offline analyzer | Python ≥3.9; no CANN/NPU; source installation needs preinstalled `setuptools>=61` and `wheel` |
| SDK | CMake ≥3.16 for installation; C++14 Host and CANN ACL; Ascend C device compilation; Linux file export |
| Add4 | Installed SDK and CANN 9.1; maintained build targets Ascend950; execution needs compatible hardware and a confirmed timer conversion |

The SDK has no hard-coded Ascend950 gate. Each SoC needs its supported compiler
target and hardware validation; Add4's `dav-c310-vec` is not a universal target.

## Install and analyze offline

Run from the repository root in Bash, using the same Python for installation
and execution. Use a virtual environment if needed.

```bash
python3 -m pip install --no-build-isolation --no-deps tools/profiling
# Or install an existing wheel:
# python3 -m pip install --no-index --no-deps /path/to/tilexr_profiling-0.1.0-py3-none-any.whl
python3 -m tilexr_profiling --mapping operator.mapping.json \
  --records capture.records.bin --output-dir profile-report \
  --attribute kernel_version=my-version
```

Use actual mapping/records and a new or empty output directory. Default outputs
are `analysis.md`, `report.html` and `perfetto_trace.json`. Check completeness
before interpreting timings. Exit 0 means supplied inputs are complete, 1 means
fatal failure, and 2 means incomplete reports or CLI usage error. Independent
expected participants require a manifest; see the [user guide](docs/USER_GUIDE.md#6-outputs-and-how-to-read-them).
The mapping's `cycles_per_us` must be confirmed for the target timer. The
[Add4 template](../../examples/profiling/profiled_add4.mapping.template.json) leaves it `null` deliberately.

## Install the capture SDK

```bash
cmake -S src/profiling -B build/profiling-sdk
cmake --install build/profiling-sdk --prefix "$PWD/install-profiling"
```

Consumers use `find_package(TileXRProfiling 1 CONFIG REQUIRED)`, link
`TileXR::profiling` and configure with `-DCMAKE_PREFIX_PATH=/path/to/install-profiling`.
The application supplies CANN linkage, device compilation and kernel launch.
See the [user guide](docs/USER_GUIDE.md) or [Add4](../../examples/profiling/README.md).

Host/Kernel must deploy DeviceView ABI 2 together. Historical records.v1/mapping.v1
and the original four-argument report API remain supported. Replace the removed
`tools.profiling` entry point with `tilexr_profiling`.
