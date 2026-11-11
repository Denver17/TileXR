# RDMA UB context 与 Debug Profiling 实施计划

## 状态与权威输入

2026-11-06：书面设计已确认；源码与 CPU 验证完成，目标 CANN 构建和设备验证待执行。
本文是随实现保留的长期计划，不单独提交文档。

范围以 `docs/specs/2026-11-06-rdma-ub-context-profiling-design.md`、用户已确认的
简要方案和根目录 `AGENTS.md` 为准。工作分支 `codex/rdma-explicit-wait`；
不更改完整批次调度、不增加自动刷盘或新 trace 协议。

各步骤修改同一条状态/调用链，由主 agent 顺序执行；不启动额外子 agent。
计划不额外授权 push、PR、历史改写或设备任务派发。

## 1. Debug 边界与构建

- 目标：Release 编译掉调用方参数检查，必要的容量和错误处理不变。
- 修改范围：`src/include/tilexr_rdma_write.h`、`tilexr_rdma_device.h`、
  `tilexr_rdma_pipeline.h` 及直接依赖的公共选项头，`tools/rdma_compare/CMakeLists.txt`。
- 前置：书面设计确认；先记录原有合法行为和批次/CQE 计数预期。
- 验收：Debug 拒绝非法参数，Release 合法输入等价，两个宏值的实际设备编译命令
  明确；测试直接编译对应配置，不能仅凭 `NDEBUG` 或源文本匹配作结论。
- 输出：稳定的宏定义规范，供 context 与插桩使用。

## 2. UB 状态 I/O 与调用入口

- 目标：四个 WRITE 和 Wait 共用调用方持有的 UB context，热路径软件元数据不落 GM。
- 修改范围：上述 RDMA 头文件、必要的 context 头、`tests/rdma` 中的现有及新增测试。
- 前置：宏定义规范；复用当前状态/FIFO 编码及发布顺序。
- 约束：一 AIV 独占 QP；不同 MR 共享 QP 状态；V3 view 和硬件 ABI 不变；
  老入口用临时 context 保持 API 边界持久化，不增加逐批 GM 写回。
- 验收：有效 pending 保存/恢复，错误状态持久化，可观测元数据 I/O 计数为预期；
  模拟跨 kernel、多 MR、窗口边界、多 SGE，原有 CQE 数量和背压策略不变。
- 输出：供 benchmark 使用的 context 初始化、WRITE/Wait 和显式保存接口。

## 3. 现有 DirectWriter 接线

- 目标：使用已有 DirectWriter 为 WRITE 调用链记录 Debug 事件，不扩展公共 SDK。
- 修改范围：RDMA trace 接线点、benchmark kernel 和 `tests/rdma`；
  公共 profiling 实现、文档和测试保持与 `main` 一致。
- 前置：context 接口；阅读 `docs/specs/profiling.md` 和 `tools/profiling/FORMAT.md`
  的当前规范后接入，沿用追加、dropped 和 Publish 行为。
- 验收：宏关闭时无实际 RDMA 标记；四种 WRITE 路径记录事件并提交 lane；
  原有分析器可读取，无新文件协议或额外计时屏障。文档明确逐点 GM 写入的开销。
- 输出：现有格式的记录及 WRITE/Wait 阶段 mapping。

## 4. benchmark、文档与最终验证

- 目标：现有 `tools/rdma_compare` 使用显式 context，采集后调用现有分析工具；
  WRITE 提交、Wait、保存/恢复和 trace 导出分别归因。
- 修改范围：benchmark kernel/Host/ABI、必要的可复用 mapping、`docs/RDMA_API.md`。
- 前置：前述接口稳定；保持每个 AIV/QP 独占上下文和独立 profiling lane。
- 验收命令按最终测试目标调整，基础入口为：

```sh
cmake -S tests/rdma -B build/rdma-ub-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/rdma-ub-debug -j2
ctest --test-dir build/rdma-ub-debug --output-on-failure
cmake -S tests/rdma -B build/rdma-ub-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/rdma-ub-release -j2
ctest --test-dir build/rdma-ub-release --output-on-failure
c++ -std=c++14 -Wall -Wextra -pedantic -D__CCE__ -D__CCE_IS_AICORE__ \
  -DTILEXR_ENABLE_PROFILING=1 -Isrc/include \
  -Itests/profiling/sdk_device/stubs tests/profiling/sdk_device/test_device.cpp \
  -o /tmp/tilexr-rdma-profile-device-protocol
/tmp/tilexr-rdma-profile-device-protocol
PYTHONPATH=tools/profiling/python python3 -m unittest discover -s tests/profiling/python
```

- CPU 替身不证明真实 UB 分配、指令时序、CANN 编译兼容或设备性能。目标构建和
  绿区 EXEC 设备验证另列证据；无硬件证据时不得宣称设备调通或性能提升。
- 最终先做代码简化和风险相称的审查，再查看 diff、运行受最终改动影响的检查。
  新增/修改代码遵守中英双语文件头要求，经验写入维护中的 API 文档。

## 当前证据与交付边界

- RDMA CPU：Debug、Release 两个构建各 18/18 通过；参数负例目标显式使用 Debug 宏，
  context 行为分别以宏 0/1 编译，另有启用 profiling 的真实 adapter CPU 替身目标。
- Profiling：公共 SDK、文档和测试与 `main` 无 diff；原有 device 协议测试和
  Python 分析测试 23/23 通过，覆盖现有 writer 和分析器行为约定。
- 显式 UB context 测试验证连续调用无 GM 元数据 I/O、跨 MR/Save/Restore、pending wrap、
  原批次/CQE 计数和 timeout 状态。替身检查 UB 端点 32B 对齐。
- `test_rdma_context_profile <新输出文件>` 可选导出 DirectWriter 的 SDK records；
  该测试验证四种 WRITE 的阶段事件、Publish 前后的 lane 状态和记录数量。
  使用原有 mapping 模板及合成 tick 换算，现有分析器读取该输出并报告 complete。
  此结果只验证协议兼容，CPU tick 不是硬件 timer 校准或 WRITE 性能数据。
- 宏 0 且 `TILEXR_ENABLE_PROFILING=1` 的 adapter CPU 构建/执行通过；
  真实 AICore 指令裁剪仍需目标产物检查。
- 本机无 CANN/bisheng：未验证 benchmark Host、AICore 编译、真实 UB 预算、MTE/NIC
  顺序或性能。本轮没有派发绿区任务，也没有重跑 HCCL 环境基线。
- 最终执行主 agent diff 审查；未声称已做独立审查或设备验收。保留设计/计划为维护文档。
