# Profiling 手动验证

Profiling 不进入专用 CI，也不随根项目的 `BUILD_TESTING` 或
`TILEXR_BUILD_TESTS` 开关启用。普通元数据、报表样式、打包、文档修改不要求
新增测试。按变更风险手动运行相关关键检查，不把测试数量当作验收目标。

当前设计见 [profiling.md](../../docs/specs/profiling.md)，公共接口见
[API_REFERENCE.md](API_REFERENCE.md)，设备示例见 [Add4](../../examples/profiling/README.md)。
历史环境证据保留在[原整改记录](../../docs/plans/PROFILING_CLEANUP.md#9-2026-10-20-实现接续与本地验收)；
历史通过结果不能代替当前源码、二进制和目标硬件的验证。

## 1. SDK 与资源生命周期

使用 C++14 工具链，不需要 CANN、NPU、Python 或 TileXR 子模块：

```bash
cmake -S src/profiling -B build/profiling-host \
  -DTILEXR_PROFILING_BUILD_TESTS=ON -DTILEXR_PROFILING_WITH_ACL=OFF
cmake --build build/profiling-host --parallel 2
ctest --test-dir build/profiling-host --output-on-failure
```

保留五项带 `profiling_host` 标签的关键回归：

| 目标 | 保护的合同 |
|---|---|
| `profiling_sdk_host` | 布局与字节预算、快照完整性、编码；Linux 文件故障、不覆盖和并发单赢家发布 |
| `profiling_workspace_mock` | 借用 lease 的 context、代次、稀疏 lane 与同步失败后的安全归还 |
| `profiling_sdk_acl_mock` | context/stream 归属、部分初始化清理、同步/D2H/free 重试及移动后的所有权 |
| `profiling_capture_queue_mock` | 有界队列、业务路径不插入等待、共同清理阶段和失败重试 |
| `profiling_device_protocol` | CPU stub 下的记录槽、完成状态、地址/容量边界及 ABI |

同步失败时必须保留 workspace，等待成功后才能释放；CPU mock 不能证明设备
可见性或真实 ACL 行为。真实设备写入顺序仍须由目标 CANN 和硬件证据确认。

## 2. 离线输入与输出完整性

仅修改解析、配对、发布或 Perfetto 轨道逻辑时，运行相关手动检查：

```bash
PYTHONPATH=tools/profiling/python \
  python3 -B -m unittest discover -s tests/profiling/python -p 'test_*.py'
```

剩余测试保护损坏输入拒绝、capture UUID 一致、缺失/溢出不伪造完整结果、
大绝对时间戳的小间隔、CLI 状态、文件不覆盖、HTML 转义和轨道身份。
不维护报表标题、统计展示、包导出或文档示例的测试矩阵。

Perfetto 子轨道与主 stream 共用同一进程的 tid 空间，必须先预留全部主
stream ID，再分配分组和实例轨道。轨道元数据统一生成，排序计数按进程递增；
简化这段代码时对照原输出核对事件顺序、tid 和 sort index，不能只检查文件生成。

## 3. CANN 和显式硬件验证

先 source 实际 CANN 9.1 环境，显式选择目标 SoC。以下命令只编译 Add4：

```bash
cmake -S src/profiling -B build/profiling-cann \
  -DTILEXR_PROFILING_WITH_ACL=ON \
  -DTILEXR_PROFILING_BUILD_EXAMPLES=ON -DTILEXR_PROFILING_SOC=Ascend950
cmake --build build/profiling-cann --parallel 2
```

硬件运行按 [Add4 英文](../../examples/profiling/README.md) /
[中文](../../examples/profiling/README.zh-CN.md) 显式操作，确认选定设备和实际
timer 换算依据。没有注册 NPU CTest；测试列表为空不是硬件通过。

保留实际源码/二进制、CANN/driver/firmware、设备、命令、数值和记录结果。
编译和 Host mock 不能证明多 rank 通信、UDMA 数据面、marker 开销或 spill。
通信排错前复用同环境已留存的 HCCL Test 一次性资格记录，按 AGENTS 执行；
纯离线/Host/Add4 不需要通信资格。MoonEP 的 profiler 和阶段 barrier 约束不变。

SDK 构建入口保持 `src/profiling`，公共头文件保持 `src/include/tilexr/profiling`。
这些源码位置不改变 SDK 的独立安装能力；安装后从仓外目录使用
`find_package(TileXRProfiling)`，由消费者自行提供 CANN 依赖。

借用 workspace 时，pool 仅管理 reservation，不拥有 ACL 分配。失败后保持 lease
直到 `Close()` 成功；设备参数已暴露且析构同步仍失败的 slot 必须保持 busy，不能
当作空闲内存复用。代次检查不替代 stream 完成。Host mock 覆盖这些所有权合同，
不证明设备 cache 可见性；ABI 2 Host 与 kernel 必须一起重新编译。

文件发布禁止覆盖已有输出，queue mock 每次运行使用独立临时目录。固定 UUID
配固定目录会让第二次手动检查误报发布失败；验证重试时应复用同一 capture，
验证新一轮检查时则隔离输出目录，不能放宽生产导出的 no-overwrite 合同。

发布 SDK 时同时核对 `src/profiling/VERSION`、公共 ABI 常量与格式说明；版本元数据不会由编译自动更新。ABI 2 的 Host/CPU 检查通过不代表旧 kernel 二进制可继续使用。

升级 DeviceView ABI 时也需检查新接入消费者的手写测试描述符：使用 `DEVICE_VIEW_ABI_VERSION` 并初始化全部字段，不能固定为旧版号。RDMA 的 `rdma_context_profile_cpu` 覆盖 DirectWriter 到 records 导出的 CPU 路径；它不证明 NIC 或 NPU 数据面。
