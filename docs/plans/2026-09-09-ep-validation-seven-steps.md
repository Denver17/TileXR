# EP 七步验证交付计划

本计划落实已批准的七步流程：PC 固定提交、同步至首节点、source 环境、构建、同步集群、启动、等待结果。用户无需手动执行 CMake、检查符号或收集日志。

## 范围与接口

- 根目录提供 `set_env.sh`、`build_all.sh`、`sync.sh`、`run.sh`；实际编排实现位于 `tools/validation/`。
- `validation.local.sh` 和 `hostfile` 为本地部署配置，不纳入提交。清单首项是当前首节点；每项可声明物理 device IDs。当前仅支持 `ep-sdedup-p1-abi-host`，每主机运行一套 Host 测试，不创建 NPU rank。
- 默认构建 CANN 生产包和完整 EP Host 测试；显式 Host-only 模式仅用于无 CANN 的脚本/Host 验证。结果必须携带构建模式，不能扩大证据范围。
- PC 传输入口自动准备子模块，导出指定提交的 LF 源码与来源清单。服务器无 `.git` 也能核对源码。集群同步包含源码、配置、安装产物，保留相同绝对目录。
- 构建、同步、运行均检查来源和结果；运行失败、缺 marker、超时或日志收集失败必须返回非零。远端使用独立 watchdog 和本次运行进程组清理。

## 实施顺序与分工

1. 主 agent：实现环境入口、Python 构建/同步/运行编排、节点测试执行和结果协议。维护本计划和使用文档。
2. 独立任务：实现 `tools/validation/source.py` 与 `push.ps1`，只拥有这两个文件。提供 `snapshot(root)`、`verify_source(root, manifest)`、`pack(root, archive)` 接口；清单格式为 `{"commit": SHA, "files": {relative_path: sha256}}`，包含子模块实质内容，排除 `.git` 和本地配置。传输完成必须校验来源，使用 SSH stdin 执行远端逻辑。
3. 在接口稳定后补行为测试：配置、hostfile、来源不一致、构建/同步失败、子进程非零/缺 marker、超时及清理。测试通过可控命令或实际子进程验证行为，不扫描生产源码。
4. 简化实现，执行独立审查和主 agent 最终 diff 审查；修复问题后刷新相关验证。
5. 精确暂存本次新增及修改文件，提交并正常推送至 `origin/codex/SilentEmotion`；不创建 PR，不包含会话前未提交文档。

## 验证与边界

- Bash 语法、PowerShell parser、Python 行为测试。
- 本地完整 EP Host/Python 构建和执行，验证测试清单、逐项 marker 与退出码。
- 使用假 SSH/rsync 和可控进程检查编排失败路径；可用时运行本机真实进程端到端闭环。
- 服务器 CANN 9.1 生产构建、真实 SSH 集群同步、NPU 执行由实际环境证据决定；本地测试不得宣称这些边界已通过。
- 在维护文档记录安装测试独立于 CMake 构建树、SSH 环境重载、日志/超时判定与版本来源的经验。

## 实施结果

四个根目录入口、PC 导出入口、独立安装的测试包及行为回归已实现。独立审查发现的路径引用和预检/环境加载超时问题已修复并补回归；操作说明和证据边界集中在 `docs/EP_VALIDATION_GUIDE.md`。本地验证覆盖 Host 闭环和模拟集群；真实服务器 production 模式由用户按交付提交执行。
