# EP 七步验证

本流程提供固定的 PC 交付入口和四个服务器入口。脚本负责 CMake、安装检查、同步、SSH 启动、超时清理和结果汇总，正常测试无需手动检查符号、执行 ctest 或拼接日志。

当前仅支持 `ep-sdedup-p1-abi-host`：每台主机运行 17 个 EP Host/Python 测试，其中 4 个为 Server Dedup ABI/Host 测试。默认同时编译 CANN 生产库及现有四个 A5 EP kernel。生产 Server Dedup 入口仍返回 `TILEXR_ERROR_NOT_SUPPORT`；本流程不创建 NPU communicator，不执行去重 kernel，不验证多卡通信或数值正确性。未实现的 checkpoint 会直接报错。

## 一次性环境配置

PC 需要 Git、Python 3.9+、Windows OpenSSH 的 ssh/scp；服务器需要 Bash、Python 3.9+、CMake 3.20+、C++14 编译器、Make 和 CANN 9.1.0 工具链。多节点还需要 rsync、SSH 免密访问及已确认的主机密钥。服务器目录必须可写，各节点必须具有兼容的 CPU 架构、CANN/driver 运行库。依赖安装和 SSH 认证属于已有服务器环境，不会由验证脚本静默修改。

首次部署后，用编辑器将根目录 `validation.local.sh.example` 的内容保存为 `validation.local.sh`，修改 CANN 环境脚本及其他实际不同的配置。单机 Host 验证无需预建 hostfile：首次 `source set_env.sh` 自动创建缺失的默认 `hostfile`，内容为 `localhost`，并显示实际主机数。多机验证时，按 `hostfile.example` 在 `hostfile` 中填写实际服务器。已有文件不会被覆盖；显式指定的其他 hostfile 路径缺失时直接报错，不回退到单机。以后刷新代码保留这两个配置文件。

| 配置 | 说明 |
| --- | --- |
| `TILEXR_CANN_ENV_SCRIPT` | CANN 9.1.0 提供的环境脚本完整路径；可以是 `set_env.sh` 或 `bin/setenv.bash`，不靠 IP 猜路径 |
| `ASCEND_DRIVER_PATH` | driver 安装根目录，默认 `/usr/local/Ascend/driver` |
| `TILEXR_VALIDATION_SSH_USER` | SSH 用户，空值使用当前用户或 SSH config |
| `TILEXR_VALIDATION_HOSTFILE` | 工作目录内的 hostfile，默认 `hostfile` |
| `TILEXR_VALIDATION_JOBS` | 编译并发数，默认 4 |
| `TILEXR_VALIDATION_TIMEOUT` | 单次集群测试总期限，默认 180 秒，允许 30..180 秒；编译、源码同步单独计时 |
| `TILEXR_VALIDATION_BUILD_MODE` | 默认 `production`；显式 `host` 只用于没有 CANN 的 Host/脚本验证，结果中单独标识 |
| `TILEXR_VALIDATION_CHECKPOINT` | 当前固定为 `ep-sdedup-p1-abi-host` |

hostfile 每行格式为 `host [devices=0,1,2,3,4,5,6,7]`，允许注释和空行。第一行必须指向执行脚本的首节点，可以写 `localhost`；后续行可使用主机名、IPv4 或 SSH config 别名。主机清单只保存在此文件，不再内置另一套列表。`devices` 省略时为 0..7，重复主机和重复 device ID 会被拒绝；当前 Host checkpoint 每主机只执行一套测试，device 列表不代表执行了 NPU 测试。

64P 清单通常对应 8 台主机，每台列出 8 个实际物理 device ID。使用主机 IP 时，形式为 `192.0.2.11 devices=0,1,2,3,4,5,6,7`，其余 7 行替换为其他服务器的实际 IP（这里的地址仅作示例）。第一行是当前首节点；需要使用其余节点可访问的真实地址时，不使用 `localhost` 作为跨机通信地址。当前 checkpoint 读取这样的清单后仍只执行每主机 Host 测试，不启动 64 个 NPU rank。

`validation.local.sh` 与 hostfile 是本地部署配置，不提交到仓库。各主机 CANN 路径不同时，可以在同一份 `validation.local.sh` 中按 `hostname` 设置路径；工作目录仍必须相同。编译、同步、运行期间不要在同一路径同时启动其他验证任务。

## 固定七步

### 1. PC 拉取代码并固定版本

在 PC 仓库目录执行，将 `<commit>` 替换为本次交付的完整提交号：

```powershell
git fetch origin codex/SilentEmotion
git switch --detach <commit>
```

### 2. PC 同步到首节点

```powershell
tools\validation\push.ps1 -Server <first-host> -RemotePath /absolute/path/TileXR -SshUser <user>
```

此入口自动初始化递归子模块，导出固定 HEAD 的 Git 原始内容，并附上 commit 和逐文件 SHA256。PC 未提交的改动不会混入测试；Shell 文件按 Git 中的 LF 字节交付。远端无需联网拉子模块，也不需要 `.git`。

目标应为新目录、空目录或本工具此前管理且不含 `.git` 的验证目录。刷新时保留配置，更新源码并删除旧来源清单中已不属于新版本的源码文件；不覆盖 Git checkout 或其他工作目录。可以通过 SSH config 管理端口和密钥，不需要把凭据写入脚本。

### 3. 首节点加载环境

登录首节点并进入上述工作目录，首次使用完成前述文件配置，然后执行：

```bash
source set_env.sh
```

脚本输出 `ENV_READY` 及本次 mode/checkpoint。CANN 路径错误、配置错误或缺少 Python 时直接给出配置位置。后续步骤均在此目录执行。

### 4. 一键编译

```bash
bash build_all.sh
```

自动配置独立构建目录、构建生产包和 Host 测试、安装到 `install/validation/`。Host 测试使用 Debug，保留现有断言；要求注册和安装完整的 17 个测试，避免缺少 Python 时只运行 15 个却误报完整通过。

默认 production 模式检查新增/原有公共符号、ELF `$ORIGIN`、真实 driver HAL 依赖以及四个 A5 kernel 文件。源码版本、安装文件摘要、测试命令、构建模式和工具链日志随构建保留。成功输出 `BUILD_PASS`；编译失败会显示错误摘要和日志路径。

### 5. 同步所有节点

```bash
bash sync.sh
```

首节点不重复复制自身。其他节点自动创建相同绝对目录，同步来源清单中的源码、测试脚本、配置和 `install/validation/`，核对源码与产物摘要和运行依赖；任一节点失败，总体失败。只重建本工具的安装目录，不同步 `.git`、本地 CANN 安装、编译缓存和历史日志。

全部节点通过后输出 `SYNC_PASS`，保存本次构建和配置的同步记录。源码、测试配置或产物变化后，`run.sh` 会拒绝沿用旧同步记录。

### 6. 启动测试

```bash
bash run.sh
```

首节点执行本机测试，其余节点通过 SSH 并行启动。每个远端会话重新加载 `set_env.sh`；不会假设 SSH 继承了首节点的环境。测试从安装包直接运行，不依赖首节点的 CMake 构建树。

### 7. 等待最终结果

`run.sh` 阻塞到测试结束并自动收集结果。成功输出类似：

```text
VALIDATION_PASS checkpoint=ep-sdedup-p1-abi-host mode=production hosts=8/8 tests_per_host=17 commit=...
```

只有全部预期主机测试成功、退出码为 0、四个 checkpoint marker 完整、结果属于本次运行且日志汇总完成时才返回 0。失败时返回非零，并显示失败节点、阶段、错误摘要和结果路径。结果保存在 `run/validation/<run_id>/summary.json`，原始日志按节点/测试保留。

设备能力尚未实现不是成功结果。`mode=host` 的 PASS 仅证明 Host 契约；`mode=production` 增加目标环境生产编译/链接和加载依赖证据，也不证明 NPU launch 或去重通信。

## 维护与验证边界

- 首次部署回归必须覆盖没有 hostfile 的目录。原先测试夹具预建了 hostfile，掩盖了 `source set_env.sh` 直接报文件不存在的问题；默认路径现在以独占创建方式初始化为 localhost，已有配置及错误的自定义路径均不会被单机默认值覆盖。
- CTest 注册不等于安装。测试构建成功但未安装时，同步后的节点不能运行；新增测试必须有安装规则，构建脚本核对全部注册命令的可分发路径。本次补齐五个已有测试的安装规则。
- CMake 测试清单可能引用构建机绝对路径。分发时将 C++ 程序映射到安装目录、Python 测试映射到源码目录，用实际子进程执行验证，不能复制构建树后假定远端可用。
- 父进程退出或 SSH 退出不代表测试子进程退出。节点设置自身期限，各测试使用独立进程组，TERM 后必要时 KILL；只清理本次拥有的进程。节点身份无法确认或清理/日志收集不完整时不得输出 PASS。
- 构建、同步和运行使用本目录锁，避免相互覆盖。异常断电可能留下锁；这种异常恢复必须先确认没有活动进程，不自动删除未知任务的锁。
- `TILEXR_CANN_ENV_SCRIPT` 是显式环境选择，`common_env.sh` 不再用仓库内另一套 CANN 环境覆盖它。没有设置此变量的既有调用保持原来的环境发现规则。
- 传输支持带空格的目录，因此环境入口及公共工具中的 `dirname`、`realpath`、`source`、目录创建也必须引用路径；只测 tar/SSH 引号不能证明真正的环境加载可用。远端 watchdog 覆盖环境加载和生产依赖预检，不能仅对测试程序设置超时。
- 本 checkpoint 不触达设备通信，不需要执行 HCCL Test。后续设备阶段必须按 AGENTS.md 复用匹配环境的一次性资格记录，不能把 Host PASS 当作通信基线。

脚本维护测试：`python3 -m unittest discover -s tools/validation -p 'test_*.py' -v`。这些是开发者测试，不是用户七步流程之外的必执行命令。测试使用临时 Git 仓库、模拟传输及实际受控子进程；真实 CANN 构建和真实 SSH 集群通过情况必须以服务器运行日志为准。

2026-09-09 开发验证：Windows/MSYS2 上 41 项脚本行为测试中 40 项通过，1 项因文件系统未创建真实 symlink 而跳过；PowerShell 5.1 同步入口通过模拟传输执行了实际导出、Bash 解包、刷新和拒绝覆盖检查。全新且路径含空格的部署目录中，四个服务器入口以 `mode=host` 完成实际 C++14 Debug 构建、安装和 17/17 Host/Python 测试。CANN 生产构建、真实 SSH 集群同步及 NPU 执行未在本地验证。

同日 hostfile 初始化修复：先用真实 `source set_env.sh` 在缺少 hostfile 时复现失败；修复后 38 项工作流回归全部通过，包含默认文件创建、保留多机清单、自定义路径缺失和空文件拒绝。此次修复不涉及算子代码，未重跑 CANN 构建或 NPU 测试。
