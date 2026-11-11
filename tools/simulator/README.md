# Memory Dispatch 仿真

`memory_dispatch_case.py` 为普通 `tilexr_ep_dispatch_memory_kernel` 生成全新的
SST testcase，并逐 rank 比对 FP16 payload、expert token counts、send counts 和
combine assist triples。当前支持无 shared expert、无 active mask、非量化的 MEM
路径。它不生成 UDMA WQE，也不修改生产 kernel。

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

维护时注意：CommArgs 的 rank 容量变化会改变后续所有字段偏移；旧 testcase 即使
使用相同的 22 个启动参数，也可能传入错误结构。重新运行 Host 布局探针，并重新
生成全部输入。仅从模板复制框架配置，禁止覆盖新生成的 workload；旧绝对路径可能
指向其他量化模式或历史输入。`bin_param.addr` 保持 rank-local `0x40000000`，参数区
保持 `0x80000000`，两者再按 GM size 逐 rank 平移。

双 die sparse 输出可能包含另一 die 尚未更新的初始化块。模板提取器按文件顺序覆盖
同地址块，会让初始化值覆盖有效写入。本工具只合并与对应输入初始化字节不同的块，
并拒绝同地址的冲突写入；未写入区域保留初始化值，交给数值校验判错。合并过程不使用
`expected/` 的内容选择或生成输出。
