# Memory Dispatch 仿真

`memory_dispatch_case.py` 为普通 `tilexr_ep_dispatch_memory_kernel` 生成全新的
SST testcase，并逐 rank 比对 payload、expert token counts、send counts 和
combine assist triples。支持 FP16 非量化与已有 MXFP8 E4M3 量化分支，后者额外
比对 E8M0 scales。当前限定无 shared expert、无 active mask 的 MEM 路径。
它不生成 UDMA WQE，也不修改生产 kernel。

需要 Python 3、PyYAML、C++14 Host 编译器、已验证的 SST testcase 模板，以及用
CANN 9.1 构建的 `tilexr_ep_dispatch_memory_kernel.o`。先从同一源码生成 CommArgs
布局；不要复用其他版本的结构大小和偏移：

```bash
c++ -std=c++14 -I src/include -I src/ep/common \
  tools/simulator/memory_dispatch_layout.cpp -o /tmp/memory_dispatch_layout
/tmp/memory_dispatch_layout > /tmp/memory_dispatch_layout.json
python3 tools/simulator/test_memory_dispatch_case.py
```

在 simulator 的 `Testcases/` 下选择一个尚不存在的目录，让模板 driver 能从其
父目录找到 `UBusSim_deployment/` 和 `v310_deployment/`：

```bash
python3 tools/simulator/memory_dispatch_case.py generate \
  --template "$SIM_ROOT/Testcases/<validated-case>" \
  --output "$SIM_ROOT/Testcases/memory_dispatch_2p_new" \
  --kernel /absolute/path/tilexr_ep_dispatch_memory_kernel.o \
  --layout /tmp/memory_dispatch_layout.json \
  --ranks 2 --cores 4 --bs 8 --h 256 --top-k 2 --experts 8
python3 tools/simulator/memory_dispatch_case.py preflight \
  "$SIM_ROOT/Testcases/memory_dispatch_2p_new"
```

用已验证的 SST 环境运行 testcase，保存 SST 退出码和完整日志。不要使用会覆盖
共享插件安装的 wrapper。仿真成功后，从 per-die sparse GM 文件提取输出，再验收
结果：

```bash
cd "$SIM_ROOT/Testcases/memory_dispatch_2p_new"
ulimit -n 524288
sst -n 2 ./test_UBusSim.py > sim.log 2>&1
sim_exit=$?
python3 /absolute/path/memory_dispatch_case.py extract "$PWD"
python3 /absolute/path/memory_dispatch_case.py validate "$PWD" \
  --log sim.log --exit-code "$sim_exit"
```

只有 SST 退出成功、存在非零模拟完成时间、全部输出尺寸及字节比对通过，并且输入
和部署对象的 SHA-256 均保持一致，才报告通过。生成器自检通过不代表仿真通过。
先验证小规模，再使用 `--ranks 32 --cores 64 --bs 32 --h 7168 --top-k 6 --experts 384`
复现参考规模。模拟时间不代表真实 NPU 延迟。

使用 `--output-format mxfp8-e4m3` 选择 `quantMode=4`、FP16 输入和 E4M3 输出；
生成器为 `dynamicScalesOut` 分配独立区域，并按每 32 元素一组、每行 scale 数向上
补齐为偶数的布局生成参考字节。FP16 与 MXFP8 必须分别通过数值校验。Python 量化
参考通过已有 `tests/ep/demo/mxfp8_golden.h` 的独立 C++ 实现验证，不读取仿真输出。

在生成时增加 `--instruction-dump` 会启用模板已有的 `enable_dump`，保留关闭的
详细 dump、UBus trace 和带宽统计开关。它不保证模板覆盖全部 ranks；报告需从实际
dump 文件核对 chip/core 范围。配置属于 manifest，生成后不要手工改写。

`instruction_trace.py` 可从原始指令 dump 生成带 PC、InstrID、源文件/行号的 Chrome
Trace，并输出按指令/PC 的驻留分位数、各流水线区间并集、VEC/MTE 重叠及核结束
分布。例如，**已确认 dump 时间是 1.65GHz 公共 cycle** 时：

```bash
python3 tools/simulator/instruction_trace.py /path/to/case/dumps \
  --clock-ghz 1.65 --output /path/to/new-analysis
python3 tools/simulator/test_instruction_trace.py
```

分析器对未配对、重复、身份不一致、负时长或无法解析的指令记录输出诊断，并以
`PARTIAL` 和非零退出码标识不完整结果。不要丢弃异常记录来获得完整性通过。
`Popped→Retire` 包含依赖等待；区间并集是驻留覆盖，不是执行单元吞吐利用率。
FlowCtrl 与 MTE 的相近长区间可能描述同一依赖，RVEC 子流水与 VEC 也不能直接相加。

比较硬件能力参数前，先证明配置被当前 plugin 消费、参数作用在实际 MEM 数据通路、
单位和方向正确。链路单向 latency 不能直接称为 RTT，整体降频不能冒充独立量化延迟。
变更时钟域后须重新证明公共时间基准，不能机械沿用上述 1.65GHz 换算。H 变化还会
改变 480B payload/512B wire padding、量化 scale 大小及 UB 中可容纳的发送 buffer 数；
应记录这些变化，再判断流水瓶颈是否转移。

维护时注意：CommArgs 的 rank 容量变化会改变后续所有字段偏移；旧 testcase 即使
使用相同的 22 个启动参数，也可能传入错误结构。重新运行 Host 布局探针，并重新
生成全部输入。仅从模板复制框架配置，禁止覆盖新生成的 workload；旧绝对路径可能
指向其他量化模式或历史输入。`bin_param.addr` 保持 rank-local `0x40000000`，参数区
保持 `0x80000000`，两者再按 GM size 逐 rank 平移。

双 die sparse 输出可能包含另一 die 尚未更新的初始化块。模板提取器按文件顺序覆盖
同地址块，会让初始化值覆盖有效写入。本工具只合并与对应输入初始化字节不同的块，
并拒绝同地址的冲突写入；未写入区域保留初始化值，交给数值校验判错。合并过程不使用
`expected/` 的内容选择或生成输出。
Sparse record 中的地址是 chip 内地址；与 workload 中的输出指针比较前，必须从
后者减去对应的 `sys_va_base_addr`。仅测试 rank 0 无法发现这类地址转换错误。

模板曾对 2304B 的 assist 输出仅导出四个 512B 块，缺少最后 256B。生成器将输出
初始化文件和 `output_param.size` 一起向上补齐到 512B，使用 `semantic_output_sizes`
单独记录有效长度；数值校验仍比对全部有效字节，不会截掉缺失的尾部来获得通过。
