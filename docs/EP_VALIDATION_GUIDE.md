# EP 七步验证

本流程支持 PC 普通文件同步或固定版本导出，并提供四个服务器入口。脚本负责 CMake、安装检查、同步、SSH 启动、超时清理和结果汇总，正常测试无需手动检查符号、执行 ctest 或拼接日志。

当前仅支持 `ep-sdedup-p1-abi-host`：每台主机运行 17 个 EP Host/Python 测试，其中 4 个为 Server Dedup ABI/Host 测试。默认同时编译 CANN 生产库及现有四个 A5 EP kernel。生产 Server Dedup 入口仍返回 `TILEXR_ERROR_NOT_SUPPORT`；本流程不创建 NPU communicator，不执行去重 kernel，不验证多卡通信或数值正确性。未实现的 checkpoint 会直接报错。

## 一次性环境配置

PC 需要 Git；使用 `push.ps1` 导出时另需 Python 3.9+ 和 Windows OpenSSH 的 ssh/scp。服务器需要 Bash、Python 3.9+、CMake 3.20+、C++14 编译器、Make 和 CANN 9.1.0 工具链。多节点还需要 rsync、SSH 免密访问及已确认的主机密钥。服务器目录必须可写，各节点必须具有兼容的 CPU 架构、CANN/driver 运行库。依赖安装和 SSH 认证属于已有服务器环境，不会由验证脚本静默修改。

首次部署后，用编辑器将根目录 `validation.local.sh.example` 的内容保存为 `validation.local.sh`，修改 CANN 环境脚本及其他实际不同的配置，并按 `hostfile.example` 一次性填写实际服务器清单。单机 Host 验证可以填写 `localhost`。`source set_env.sh` 只读取 hostfile 并显示实际主机数；缺失或格式错误时提示配置位置，不会创建、覆盖文件或回退到单机。使用 Mutagen 等同步工具时，由部署者管理配置文件的同步方向，避免错误清单覆盖正确清单。

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

可以继续使用已配置的 Mutagen 等工具同步普通源码文件，不需要同步 `.git`。`3rdparty/spdlog` 是可选的日志后端，允许为空或缺失：`src/comm/tilexr_log.h` 用 `__has_include` 检测头文件，找不到时自动使用 `std::cout`/`std::cerr`；Server Dedup 没有新增 spdlog 依赖。若需要使用仓库固定版本的 spdlog，再在 PC 执行 `git submodule update --init --recursive` 并同步其完整源码。保留 Shell 文件的 LF 换行，不传输 PC 编译缓存、安装产物或 CANN 环境目录；同步完成后再开始构建。服务器不需要安装 Git，也不需要额外执行 Git 命令。

需要在结果中自动记录精确 Git 提交号时，可使用已有导出入口代替普通同步：

```powershell
tools\validation\push.ps1 -Server <first-host> -RemotePath /absolute/path/TileXR -SshUser <user>
```

此入口为完整导出固定 HEAD，仍会自动初始化递归子模块，并附上 commit 和逐文件 SHA256；这是完整导出的规则，不代表普通源码构建必须安装 spdlog。PC 未提交的改动不会混入测试；Shell 文件按 Git 中的 LF 字节交付。远端无需联网拉子模块，也不需要 `.git`。

普通同步目录没有 Git/导出清单时，`build_all.sh` 自动按实际文件生成 `.tilexr-source.json`，包含逐文件 SHA256 和整份清单的 `content_sha256`。提交号在终端中显示 `unknown`，JSON 中为 `null`；内容摘要标识本次构建源码，不能据此声称验证了某个 Git commit。Git 元数据不完整（包括子模块 `.git` 缺失）时也按文件处理。普通同步后直接重建会刷新文件清单，`sync.sh`/`run.sh` 继续检查构建时记录的文件摘要；由导出入口生成的固定提交清单仍严格校验，不会静默接受源码修改。

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

默认 production 模式检查新增/原有公共符号、ELF `$ORIGIN`、真实 driver HAL 依赖以及四个 A5 kernel 文件。源码来源及内容摘要、安装文件摘要、测试命令、构建模式和工具链日志随构建保留。成功输出 `BUILD_PASS`；编译失败会显示错误摘要和日志路径。脚本不要求存在 `3rdparty/spdlog`，日志后端沿用代码中的自动检测。

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
VALIDATION_PASS checkpoint=ep-sdedup-p1-abi-host mode=production hosts=8/8 tests_per_host=17 commit=... source_sha256=...
```

只有全部预期主机测试成功、退出码为 0、四个 checkpoint marker 完整、结果属于本次运行且日志汇总完成时才返回 0。失败时返回非零，并显示失败节点、阶段、错误摘要和结果路径。结果保存在 `run/validation/<run_id>/summary.json`，原始日志按节点/测试保留。

设备能力尚未实现不是成功结果。`mode=host` 的 PASS 仅证明 Host 契约；`mode=production` 增加目标环境生产编译/链接和加载依赖证据，也不证明 NPU launch 或去重通信。

## 维护与验证边界

- hostfile 属于一次性部署配置，环境入口只能读取和提示错误。曾自动生成 localhost 默认文件，这会在 Mutagen 等同步流程中引入错误清单传播的风险，因此已撤销自动创建；回归覆盖缺失文件不落盘、已有多机清单不变、自定义路径缺失及空文件拒绝。
- `git -C <子模块目录>` 会向父目录查找仓库。普通同步遗漏子模块 Git 元数据时，以子模块 commit 查询父仓库会报 `fatal: not a tree object`。源码快照先检查仓库本地元数据，普通文件目录直接计算内容摘要，不要求服务器补 `.git`。回归须同时覆盖普通复制、缺失子模块元数据和固定提交导出，不能只测试带导出清单的部署目录。
- 不能由 `.gitmodules` 或 CMake include 路径推断依赖是必需的。此前源码复制预检错误地要求 spdlog 头文件，阻断了已有的无 spdlog 日志路径；应核对实际 include 条件、链接目标及替代实现。spdlog 子模块和可选日志封装分别来自 `b063b61`、`b6c88d8`（2026-05-29），均早于本次 Server Dedup。回归需覆盖空子模块仍进入生产配置阶段，并实际编译、运行日志替代实现；这不代替 CANN/NPU 验证。
- 普通文件清单排除 `.git`、部署配置、build/install/run、Python 缓存、env、reference 等环境或生成物；包含实际子模块源码和测试脚本。构建后的同步及运行校验已记录文件的存在和字节摘要，构建缓存、日志及本地 hostfile 另行管理；内容摘要不证明 CANN 编译或 NPU 正确性。
- CTest 注册不等于安装。测试构建成功但未安装时，同步后的节点不能运行；新增测试必须有安装规则，构建脚本核对全部注册命令的可分发路径。本次补齐五个已有测试的安装规则。
- CMake 测试清单可能引用构建机绝对路径。分发时将 C++ 程序映射到安装目录、Python 测试映射到源码目录，用实际子进程执行验证，不能复制构建树后假定远端可用。
- 父进程退出或 SSH 退出不代表测试子进程退出。节点设置自身期限，各测试使用独立进程组，TERM 后必要时 KILL；只清理本次拥有的进程。节点身份无法确认或清理/日志收集不完整时不得输出 PASS。
- 构建、同步和运行使用本目录锁，避免相互覆盖。异常断电可能留下锁；这种异常恢复必须先确认没有活动进程，不自动删除未知任务的锁。
- `TILEXR_CANN_ENV_SCRIPT` 是显式环境选择，`common_env.sh` 不再用仓库内另一套 CANN 环境覆盖它。没有设置此变量的既有调用保持原来的环境发现规则。
- 传输支持带空格的目录，因此环境入口及公共工具中的 `dirname`、`realpath`、`source`、目录创建也必须引用路径；只测 tar/SSH 引号不能证明真正的环境加载可用。远端 watchdog 覆盖环境加载和生产依赖预检，不能仅对测试程序设置超时。
- 本 checkpoint 不触达设备通信，不需要执行 HCCL Test。后续设备阶段必须按 AGENTS.md 复用匹配环境的一次性资格记录，不能把 Host PASS 当作通信基线。

脚本维护测试：`python3 -m unittest discover -s tools/validation -p 'test_*.py' -v`。这些是开发者测试，不是用户七步流程之外的必执行命令。测试使用临时 Git 仓库、模拟传输及实际受控子进程；真实 CANN 构建和真实 SSH 集群通过情况必须以服务器运行日志为准。

2026-09-09 开发验证：Windows/MSYS2 上 41 项脚本行为测试中 40 项通过，1 项因文件系统未创建真实 symlink 而跳过；PowerShell 5.1 同步入口通过模拟传输执行了实际导出、Bash 解包、刷新和拒绝覆盖检查。全新且路径含空格的部署目录中，四个服务器入口以 `mode=host` 完成实际 C++14 Debug 构建、安装和 17/17 Host/Python 测试。CANN 生产构建、真实 SSH 集群同步及 NPU 执行未在本地验证。

同日普通文件同步修复：临时子模块移除 `.git` 后复现 `fatal: not a tree object`。修复后 52 项脚本回归中 51 项通过，1 项因 MSYS 未创建真实 symlink 而跳过；包含普通同步清单跨模拟节点分发及结果摘要检查。无 `.git`、无导出清单且路径含空格的普通复制目录中，首次构建和修改源码后的重建均通过四个服务器入口，每轮 17/17 Host/Python 测试成功；用 Git 拦截程序确认两轮服务器流程均未调用 Git，hostfile 字节不变。hostfile 保持手动配置，不再自动生成。本地没有 CANN/NPU，生产编译及真实 SSH 集群验证仍需以服务器日志为准。

同日可选 spdlog 修正：新增回归先复现 production 模式被空 spdlog 目录阻断，移除预检后确认能进入生产 CMake 配置调用（测试在调用边界停止，不代表生产构建成功）。52 项脚本回归中 51 项通过，1 项 symlink 跳过。在临时目录仅复制真实 `tilexr_log.h`，不提供 spdlog 头文件、不定义 `TILEXR_LOG_DISABLE_SPDLOG`，按项目默认 GNU C++14 模式编译并运行现有日志测试通过，编译断言确认使用自动替代路径；现有 fake-spdlog API 测试亦通过。MSYS 严格 `-std=c++14` 下现有测试的 POSIX `setenv`/`unsetenv` 声明不可见，故使用与 CMake 一致的 `-std=gnu++14`；未修改生产日志代码或测试源码。CANN 生产编译与 NPU 执行未在本地验证。

## 七步流程检查审计（2026-09-09）

审计源码版本：`04e16e98cf2c3502d765436c87be43616a172192`。以下为检查清单及精简建议，不表示已经删除这些检查。范围覆盖四个根目录入口、它们调用的环境/源码/工作流工具、可选 PC 导出入口，以及当前 CMake 配置。没有访问实际服务器、用户的 Mutagen 配置或已安装的 CANN 环境脚本，因此不能判断这些外部工具内部是否还有检查；也没有测量真实集群上的检查耗时。

判定含义：**保留**表示检查支撑当前已承诺行为；**建议精简**表示当前调用路径存在明确重复或无消费者的操作；**待定**表示需要明确部署/验证范围后才能删除。表内位置指向上述源码版本的实现，不引用未来规划中的命令。

### 可以优先精简的部分

- 将同一安装包的 `nm`/`readelf` 检查集中到构建完成后；其他节点在产物 SHA256 一致后无需再次检查相同二进制的符号和 RUNPATH。`ldd -r` 检查的是各节点实际运行库，另行保留每节点运行前检查。
- 当前 checkpoint 不使用 NPU，生产 EP kernel 也固定编译 A5；环境脚本探测出的 SoC、ops 名和卡数没有被当前调用路径消费。可跳过这些硬件探测，避免每次加载环境都执行三次 `npu-smi info`。未来 NPU checkpoint 另行定义设备检查。
- 用解析后的 hostfile 内容判断是否需要重新同步。注释/空白变化不改变主机与 device 配置，不应单独阻断 `run.sh`。`validation.local.sh` 是可执行 Shell，不能用简单去注释的方式等价处理，应记录实际生效的配置。
- 构建时保留完整来源记录，但运行前只校验参与构建/执行的源文件与产物；普通说明文档、规划文档不应因文字修改阻断运行。具体纳入哪些目录仍需制定，不能只按扩展名粗略裁剪。
- 将 `len(tests) == 17` 改为必需测试集合/CTest label 检查，保留测试未安装、必需项缺失和结果不完整时的失败。新增一个无关测试不应使流程报“必须 17 项”。

### 第 1、2 步：固定版本及 PC 同步

| ID | 位置 | 当前操作或检查，失败/影响 | 判定与建议 |
| --- | --- | --- | --- |
| P01 | PC `git fetch`、`git switch --detach` | 获取并选择验证版本；不自动检查 PC 是否有未提交改动 | 保留版本选择。Mutagen 会同步实际工作文件，不能据此保证全是该提交的内容 |
| P02 | [source.py:165](../tools/validation/source.py#L165) `pack` | 仅 `push.ps1` 路径执行：初始化全部递归子模块，导出 HEAD 原始字节；未提交改动不导出，子模块获取失败则停止 | 待定：完整提交导出的既有规则。用户使用 Mutagen 时完全不需要此入口，也不需要为当前算子获取可选 spdlog |
| P03 | [push.ps1:12](../tools/validation/push.ps1#L12) | 校验 host/user 格式、Linux 绝对路径并引用 Shell 参数；禁止换行、路径越界等 | 保留参数引用和路径约束。当前不支持 `user@host`、IPv6 等写法，扩展支持需独立明确 |
| P04 | [push.ps1:31](../tools/validation/push.ps1#L31) | 对导出、scp、SSH 返回码逐一判失败；生成并传输临时 tar.gz，最终清理临时包 | 使用该入口时保留；不应叠加到 Mutagen 流程 |
| P05 | [push.ps1:45](../tools/validation/push.ps1#L45) | 拒绝根目录、路径中的 symlink、Git checkout、无管理标记的非空目录和活动/残留锁 | 路径越界与活动任务保护保留；非空目录必须有标记、全面禁 symlink 属于部署政策，待定 |
| P06 | [push.ps1:61](../tools/validation/push.ps1#L61) | 校验归档是普通文件、清单与归档文件集合相等、不得覆盖 hostfile 等本地配置；移除旧清单中的退役源码 | 保留包完整性和配置保护。旧源码删除用于避免残留文件参与运行 |
| P07 | [push.ps1:100](../tools/validation/push.ps1#L100)、[source.py:119](../tools/validation/source.py#L119) | 解包后验证每个源码文件 SHA256；首次构建开始时还会再验证一次固定提交清单 | 待定：各自覆盖“传输完成”和“准备构建”时点，不能仅因使用相同算法就认为等价 |

### 第 3 步及后续入口：环境与配置

| ID | 位置 | 当前操作或检查，失败/影响 | 判定与建议 |
| --- | --- | --- | --- |
| E01 | [set_env.sh:14](../set_env.sh#L14)、[build_all.sh:3](../build_all.sh#L3)、[sync.sh:3](../sync.sh#L3)、[run.sh:3](../run.sh#L3)、[workflow.py:30](../tools/validation/workflow.py#L30) | 显式第 3 步、三个入口和每个 SSH verify/node/cancel 会话重新 source 配置与公共环境 | 远端会话加载环境保留；本机可精简为可重复执行的轻量加载，避免重复硬件探测、PATH 增长。不能只删掉远端 source |
| E02 | [set_env.sh:32](../set_env.sh#L32)、[workflow.py:134](../tools/validation/workflow.py#L134) | Shell 和 Python 都检查 mode/checkpoint，只允许 host/production 和已实现的 ABI/Host checkpoint | 范围检查保留，避免误报设备测试；重复实现可合并，但独立调用 Python 时仍须检查 |
| E03 | [set_env.sh:37](../set_env.sh#L37)、[common_env.sh:55](../scripts/common_env.sh#L55) | production 检查 CANN 脚本可读并 source；可读性检查重复。`TILEXR_CANN_VER=9.1.0` 只是赋值，不是安装版本验证 | 保留环境加载；重复可读检查可合并。实际 CANN 脚本内部工作未知，不能把这里描述为已验证版本兼容 |
| E04 | [set_env.sh:48](../set_env.sh#L48)、[workflow.py:566](../tools/validation/workflow.py#L566) | 查找 Python 并要求版本至少 3.9 | 保留，代码实际使用 Python 3.9 API；不应增加另一套手工安装/验证命令 |
| E05 | [workflow.py:102](../tools/validation/workflow.py#L102) | hostfile 必须存在且有 1..64 台主机，检查字段、重复 host、重复 device ID 及 0..255 范围；为空/格式错误时失败 | 保留文件读取、格式及重复主机检查。64 主机上限、255 device 上限及 Host checkpoint 的 device 检查待定；这不是实际设备存在性验证 |
| E06 | [workflow.py:136](../tools/validation/workflow.py#L136) | JOBS 限制 1..256，TIMEOUT 限制 30..180 秒；SSH_USER 必须匹配限定格式；hostfile 解析后必须在工作目录内 | 正数及输入合法性保留。数值上限和 hostfile 必须位于源码目录是工具政策，是否必要待定；后者便于自动分发配置 |
| E07 | [common_env.sh:21](../scripts/common_env.sh#L21)、[common_util.sh:47](../scripts/common_util.sh#L47)、[common_util.sh:62](../scripts/common_util.sh#L62)、[common_util.sh:104](../scripts/common_util.sh#L104) | 探测 SoC、ops 名、设备数；每次 source 在 npu-smi 可用时执行 3 次 `npu-smi info`，解析失败可能调用 lspci；未设置单独超时 | 建议在当前 checkpoint 跳过。当前生产 EP 固定 A5 编译，这些值未被当前构建/Host 测试路径使用；设备探测不等于健康/空闲检查 |
| E08 | [common_env.sh:26](../scripts/common_env.sh#L26)、[common_env.sh:88](../scripts/common_env.sh#L88) | 创建 env/temp/util、run/plog；设置 MPI/HCCL Test 路径并向 PATH/LD_LIBRARY_PATH 添加工具目录 | 保留实际所需日志和库路径。MPI、HCCL Test、sshpass 等路径没有当前流程消费者，建议轻量环境不再设置；没有实际执行依赖安装或 HCCL Test |
| E09 | [common_env.sh:78](../scripts/common_env.sh#L78) | driver/kernel/inc 不可读时，用 CANN driver 头文件建立本地 driver-shim，并修改 ASCEND_DRIVER_PATH；链接仍指向真实 driver 库 | 待定：兼容已安装环境的功能，不应因当前机器不需要就删；需要服务器实际路径才能判断是否启用 |

### 第 4 步：构建与安装检查

| ID | 位置 | 当前操作或检查，失败/影响 | 判定与建议 |
| --- | --- | --- | --- |
| B01 | [workflow.py:161](../tools/validation/workflow.py#L161)、[workflow.py:573](../tools/validation/workflow.py#L573) | build/sync/run 使用目录锁，遇到活动节点锁或既有操作锁即失败；退出时释放 | 保留并发保护。断电残留锁的恢复是异常处理，当前会需要人工确认，正常路径不增加命令 |
| B02 | [workflow.py:254](../tools/validation/workflow.py#L254) | 强制要求 cmake、ctest、g++、make，并执行 cmake/g++ `--version` 保存日志 | cmake/ctest 保留；固定 g++/make 待定，CMake 本可选其他 Host 编译器或 Ninja。版本记录不是版本兼容性测试；文档要求 CMake 3.20+，当前根 CMakeLists 声明 3.16，是否确有更高版本依赖尚未确认 |
| B03 | [source.py:142](../tools/validation/source.py#L142)、[source.py:91](../tools/validation/source.py#L91) | 构建前读取 Git/固定导出清单，或扫描普通文件并计算 SHA256；元数据不完整时使用 commit=null，不要求服务器 Git | 保留实际源码标识。扫描普通目录还包含 docs 等不参与编译内容，建议缩小阻断检查范围 |
| B04 | [source.py:43](../tools/validation/source.py#L43)、[source.py:119](../tools/validation/source.py#L119) | 拒绝越界/绝对路径/.git 路径、源码 symlink、非法摘要及文件缺失/变化；只检查清单中已有文件 | 越界与内容不符检查保留；全面禁 symlink 待定。该清单不检测所有额外文件，不能称为整个目录完全相等 |
| B05 | [workflow.py:256](../tools/validation/workflow.py#L256)、[workflow.py:263](../tools/validation/workflow.py#L263) | 每次生成 UUID 新构建目录，生产包与 Host 测试各自 configure/build/install；二者均传 Debug | 待定：新目录避免旧缓存但导致每次全量编译。Host Debug 要保留断言；生产包是否也用 Debug，应按调试/性能目标决定 |
| B06 | [src/comm/CMakeLists.txt:30](../src/comm/CMakeLists.txt#L30) | CMake 编译探测 PTO SDMA 头文件/API，检查 intrinsic 与真实 libnnopbase；不可用时关闭可选 SDMA | 保留能力探测及不可用时继续编译的行为；它没有要求必须安装 PTO。不要重演把可选 spdlog 升为硬依赖的问题 |
| B07 | [src/ep/CMakeLists.txt:5](../src/ep/CMakeLists.txt#L5)、[CMakeDetermineCCECompiler.cmake:11](../src/collectives/kernels/cmake/CMakeDetermineCCECompiler.cmake#L11)、[TileXRKernelArtifact.cmake:4](../src/cmake/TileXRKernelArtifact.cmake#L4) | 生产 EP 查找 ccec/ld.lld、构建并安装四个 A5 kernel，检查 kernel artifact 的 CMake target/参数；CCE 测试文件仅把 WORKS 设为 1 | 生产包编译依赖保留，实际编译返回码才是证据。是否为纯 ABI/Host 阶段编译全部 kernel 待定；不能删除后仍声称验证完整生产包 |
| B08 | [workflow.py:271](../tools/validation/workflow.py#L271) | 安装后检查 dispatch/combine/dispatch_memory/combine_memory 四个 kernel 文件存在 | 保留当前完整包检查；它不表示执行过 kernel，不是 spdlog 那样的可选依赖 |
| B09 | [workflow.py:216](../tools/validation/workflow.py#L216) | `nm -D --defined-only -C libtilexr-ep.so`，检查两个 Dispatch 公共符号文本 | 构建后保留；相同产物在多节点重复做建议精简。不能替代完整 ABI 或实际调用测试 |
| B10 | [workflow.py:220](../tools/validation/workflow.py#L220) | `readelf -d`，要求恰好一条 RPATH/RUNPATH 且含完整 `[$ORIGIN]` 字段 | 构建后校验外置 kernel 包的加载路径有用；重复调用建议精简。精确字符串政策待定：`$ORIGIN:$ORIGIN/extra` 也被拒绝，虽然不是 devlib |
| B11 | [workflow.py:224](../tools/validation/workflow.py#L224) | `ldd -r` 拒绝 not found/undefined symbol/devlib，并要求 libascend_hal 解析到配置的真实 driver 路径（兼容 shim） | 每节点运行前保留；本机四次可减。真实 HAL 约束来自 AGENTS.md；不能只因链接成功就删除运行环境检查 |
| B12 | [workflow.py:281](../tools/validation/workflow.py#L281) | `ctest --show-only=json-v1` 只列出命令；将 Python/C++ 测试映射到分发路径，检查安装文件存在；不接受其他命令形态 | 保留清单与安装检查。该命令没有执行测试；命令形态限制是当前导出器能力边界，待扩展用例出现再调整 |
| B13 | [workflow.py:296](../tools/validation/workflow.py#L296) | 要求恰好 17 项且包含四个 Server Dedup 测试；新增无关测试同样导致构建失败 | 建议精简为必需集合/label。四项必需测试不能少，是否纳入另外 13 项是验证范围决策 |
| B14 | [workflow.py:299](../tools/validation/workflow.py#L299) | 编译后再次读取所有记录源码并比 SHA256，防止构建过程中 Mutagen 改动源码；计算安装文件摘要，写 manifest | 保留构建前后的一致性。普通文档变化也失败属于范围过大；缩小范围前须覆盖 CMake、头文件和运行时 Python 导入 |
| B15 | [workflow.py:306](../tools/validation/workflow.py#L306) | 拒绝 install/validation 及父目录 symlink，移除旧安装目录后以本次 stage 替换，写源码清单及管理标记 | 保留避免旧产物混入的目的；是否改为按 build_id 保留安装并切换，待定。路径保护应与删除动作一起评估 |

### 第 5 步：节点同步

| ID | 位置 | 当前操作或检查，失败/影响 | 判定与建议 |
| --- | --- | --- | --- |
| S01 | [workflow.py:177](../tools/validation/workflow.py#L177)、[workflow.py:325](../tools/validation/workflow.py#L325) | 确认 hostfile 首项是当前主机；依次尝试本机名称、IP/DNS，必要时 SSH hostname；第 6 步重复 | 保留首节点身份契约，避免把另一主机当本机跳过同步；重复远端探测可按清单缓存，必要性取决于实际 hostfile |
| S02 | [workflow.py:326](../tools/validation/workflow.py#L326) | 多机才要求 ssh/rsync；固定 BatchMode、连接期限与保活 | 保留自动执行所需连接行为；不安装密钥、不接受未知主机密钥、不执行 NPU 空闲探测 |
| S03 | [workflow.py:240](../tools/validation/workflow.py#L240)、[workflow.py:329](../tools/validation/workflow.py#L329) | 首节点验证 mode/checkpoint/CPU 架构、源码 SHA256、安装文件集合及 SHA256，再执行全部 runtime_check | 身份和产物变化检查保留；重复 nm/readelf 建议精简；这一时点是否还需 ldd 待定 |
| S04 | [workflow.py:330](../tools/validation/workflow.py#L330) | 删除旧 sync.json，只在本次所有节点成功后写新记录；记录 build_id 与配置摘要 | 保留失败不能沿用旧同步成功的语义 |
| S05 | [workflow.py:332](../tools/validation/workflow.py#L332) | 仅同步清单中源码、安装包、hostfile/local 配置和管理清单；不复制 build、历史 run 和环境 | 保留避免分发缓存；是否同步全部文档待定。这不是逐字复制整个工作目录 |
| S06 | [workflow.py:346](../tools/validation/workflow.py#L346) | 远端只接受空目录或有 `.tilexr-validation-root` 的目录，拒绝 Git checkout/路径 symlink/活动锁 | 活动任务和越界保护保留；非空目录必须已有标记待定。其他节点若已经由 Mutagen 预置源码，会在此失败，不符合“任意已有同名工作目录都可同步”的理解 |
| S07 | [workflow.py:360](../tools/validation/workflow.py#L360) | 同步前删除远端 install/validation；首节点没有 local 配置时删除远端同名配置，否则 rsync 覆盖配置及 hostfile | 待定：能清掉退役产物，但会强制重传整个安装包；配置必须统一由首节点管理。不能在未明确所有权时接管现有目录 |
| S08 | [workflow.py:375](../tools/validation/workflow.py#L375) | `rsync --checksum` 以内容判断已有文件是否需要更新；之后还执行 Python SHA256 验证 | 待定，不等同于完全重复：rsync 决定“传哪些文件”，SHA256 判断“传完是否正确”。去掉 checksum 可能漏传同大小/同时间戳但内容变化的源码；已删除的安装目录则没有旧文件可比较 |
| S09 | [workflow.py:379](../tools/validation/workflow.py#L379)、[workflow.py:587](../tools/validation/workflow.py#L587) | 每个远端重新加载环境、运行 verify_build 的全部检查，再比较请求的 build_id；任一失败则整体失败 | 传输后核验有用；相同二进制的 nm/readelf 再次重复。SHA256 与运行前检查是否两次都保留待定 |
| S10 | [workflow.py:154](../tools/validation/workflow.py#L154)、[workflow.py:387](../tools/validation/workflow.py#L387) | 配置签名既含解析 host 列表，又包含 hostfile 原始字节 SHA256；local 配置也按原始字节摘要；含 timeout 等参数 | hostfile 双重记录建议精简，注释不应失效；local Shell 配置需按实际生效值判断。不能用简单文本删除注释处理任意 Shell |

### 第 6、7 步：执行、取消与结果

| ID | 位置 | 当前操作或检查，失败/影响 | 判定与建议 |
| --- | --- | --- | --- |
| R01 | [workflow.py:474](../tools/validation/workflow.py#L474) | 首节点再次检查自身身份与 verify_build，再要求 sync.json 的 build_id/配置完全匹配；单机也必须先 sync | 多机同步身份检查保留。单机的同步凭据只满足固定七步接口，没有网络复制；是否内部自动通过待定 |
| R02 | [workflow.py:486](../tools/validation/workflow.py#L486) | 本机用 Python 子进程、其他节点经 SSH 启动一个 node；每主机执行一套 Host 测试 | 这是当前实现边界。device 列表不会转换成 NPU rank；是否每节点都跑整套相同 Host 测试待定，可考虑首节点全量、其他节点部署 smoke |
| R03 | [workflow.py:399](../tools/validation/workflow.py#L399) | 每节点保存 PID/starttime，设置 TERM/HUP 处理和期限，检查取消标记，再次 verify_build 并确认 build_id | 身份、取消与节点自身期限保留；本机紧接控制端再次校验相同文件与 nm/readelf 可合并，但须保留独立 node 调用的检查能力 |
| R04 | [workflow.py:421](../tools/validation/workflow.py#L421) | 按 manifest 逐个执行测试；非零退出失败；四个 Server Dedup 测试还要求完整 PASS marker 行 | 保留必需测试和完整结果语义；marker 是额外执行完成证据，是否只用返回码待定，不能直接假定任意程序 exit=0 就测过 checkpoint |
| R05 | [workflow.py:64](../tools/validation/workflow.py#L64)、[workflow.py:82](../tools/validation/workflow.py#L82) | 各外部命令有独立进程组，退出/超时均执行 TERM，必要时 KILL；保留输出日志，非零显示尾部 | 保留，解决 SSH/父进程退出而子进程遗留；不要改成按程序名全局杀进程 |
| R06 | [workflow.py:30](../tools/validation/workflow.py#L30)、[workflow.py:411](../tools/validation/workflow.py#L411)、[workflow.py:475](../tools/validation/workflow.py#L475) | 环境+远端命令 watchdog、节点最多 120 秒、控制端总期限默认 180 秒；为清理/收集预留时间；构建/同步另计 | 保留有界执行目标。具体 30..180 限制、各层预算待定；本机 source、逐文件摘要、DNS 等同步调用没有统一可中断期限，不能仅靠最终超时检查保证严格墙钟上限 |
| R07 | [workflow.py:445](../tools/validation/workflow.py#L445)、[workflow.py:504](../tools/validation/workflow.py#L504) | 首个启动/测试失败后向所有节点发取消；核对 PID starttime 防 PID 复用误杀，最多等待清理 8 秒；取消失败也计入总失败 | 保留进程归属与清理确认。取消 SSH 仍加载完整 CANN/hostfile 环境，建议改为最小清理入口，避免环境错误阻断取消 |
| R08 | [workflow.py:523](../tools/validation/workflow.py#L523) | 从各节点 rsync 当前 run_id 日志和 result.json；日志收集失败也使总体失败 | result.json 收集保留；完整原始日志收集是否作为 PASS 的硬条件待定，可区分“测试通过但日志不完整”，但这是结果语义变更 |
| R09 | [workflow.py:463](../tools/validation/workflow.py#L463)、[workflow.py:543](../tools/validation/workflow.py#L543) | 检查 run_id/build_id、mode/checkpoint、PASS、cleanup、测试集合无缺失/重复且逐项通过，再检查总期限；写 summary | 保留，防止旧结果、漏主机/漏测试被误判为本次成功；这些与文件摘要解决不同问题 |

### SHA256 和重复调用的具体次数

当前调用链没有 `md5sum` 或 `sha256sum` 命令。源码与产物校验使用 Python `hashlib.sha256`；`rsync --checksum` 是 rsync 自己的传输选择机制，其具体算法由实际安装的 rsync 决定。`content_sha256` 只对已有“路径→文件摘要”映射做一次摘要，不再读取源码文件。

下表按 **Mutagen/普通复制、production、正常成功、N 台主机、一遍第 3～7 步**统计源码调用路径。计入构建时首次计算文件摘要，不计 PC 导出、重试、失败取消、rsync 内部校验和测试程序内部工作；次数不是实测耗时。

| 操作 | 首节点 | 每个其他节点 | N=8 总数 | 建议 |
| --- | --- | --- | --- | --- |
| 加载 set_env/common_env | 4：显式 source、build、sync、run | 2：同步 verify、运行 node | 18 次 | 保留会话环境，去掉无消费者的探测/路径设置 |
| `npu-smi info`（命令可用时） | 12 | 6 | 54 次 | 当前 checkpoint 可跳过；临时模拟确认每次 source 为 3 次 |
| 逐文件源码 SHA256 | 5：构建前、构建后、sync 本机、run 控制端、run 本机 node | 2：sync verify、run node | 19 次扫描 | 校验范围与时点待定，优先排除无关文档 |
| 逐文件安装产物 SHA256 | 4：建清单、sync 本机、run 控制端、run 本机 node | 2：sync verify、run node | 18 次扫描 | 相邻控制端/本机 node 检查可合并；传输后与运行前是否都要待定 |
| `nm` + `readelf` + `ldd -r` | 各 4 次：构建后、sync、run 控制端、run 本机 node | 各 2 次 | 18 组，即 54 个命令 | nm/readelf 建议全包只做一次；ldd 保留每节点运行前检查 |
| Host/Python 测试 | 17 项 | 17 项 | 136 个测试进程 | 每节点重复范围待定，不是 64P NPU 测试 |

源码校验的不同目的：构建前确定输入、构建后发现构建期间被同步修改、同步前防止分发过期构建、同步后确认目标内容、启动前防止沿用旧产物。这些目的应保留，但不代表每个目的都必须重读全仓库。当前只有“重读并比较”一种实现，没有版本缓存或不可变发布目录机制。

### 当前 17 项测试的范围

| 测试集合 | 数量 | 位置 | 对本 checkpoint 的判定 |
| --- | --- | --- | --- |
| `test_tilexr_ep_server_dedup_{protocol,layout,host_validation,public_abi}` | 4 | [tests/ep/CMakeLists.txt:247](../tests/ep/CMakeLists.txt#L247) | 本 checkpoint 必需，验证协议/布局/Host 参数/公共 ABI，不运行设备去重 |
| `test_tilexr_ep_{layout,expert_ids,host_validation,kernel_launch}` | 4 | [tests/ep/CMakeLists.txt:370](../tests/ep/CMakeLists.txt#L370) | 有关 EP 既有功能回归，建议至少构建后验证一次；是否每节点重复待定 |
| `test_tilexr_moonep_planner_public_abi`、`test_tilexr_ep_plan_{layout,reference,algorithm,downstream,host}`、`test_tilexr_moonep_planner_compat` | 7 | [tests/ep/CMakeLists.txt:374](../tests/ep/CMakeLists.txt#L374) | Planner 回归，不是当前 Server Dedup 最小必需集合；是否纳入每次交付待定 |
| `test_dispatch_memory_trace`、`test_msprof_kernel_stats` | 2 | [tests/ep/CMakeLists.txt:27](../tests/ep/CMakeLists.txt#L27) | Python 日志/统计工具回归，不执行 profiler 或采集 NPU 性能；是否纳入每次节点验证待定 |

### 已确认取消或没有执行的流程

`04e16e9` 已取消 spdlog 缺失硬阻断；`67168ba` 已取消环境入口自动创建 hostfile，并支持普通复制目录不带 Git 元数据。当前正常服务器路径不执行 Git clone/submodule、HCCL Test、NPU 空闲检查、设备通信、正式 NPU profiler 或依赖安装。`common_env.sh` 定义的安装函数/权限修复函数不等于已调用，不能仅通过关键词命中把它们列成必执行步骤。

本次审计的定点验证在临时目录完成：模拟 npu-smi 确认单次 source 调用三次；hostfile 只加注释导致 signature 变化；普通文档修改触发 source mismatch；模拟 readelf 输出 `$ORIGIN:$ORIGIN/extra` 被精确 RUNPATH 规则拒绝。没有执行实际 NPU 查询或远程命令，也没有删除上述待定检查。
