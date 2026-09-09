# EP 七步验证

本流程支持 PC 普通文件同步或固定版本导出，并提供四个服务器入口。脚本负责 CMake、安装检查、同步、SSH 启动、超时清理和结果汇总，正常测试无需手动检查符号、执行 ctest 或拼接日志。

当前仅支持 `ep-sdedup-p1-abi-host`：每台主机保留完整的 17 个 EP Host/Python 测试，其中 4 个为 Server Dedup ABI/Host 测试；检查必需测试集合，允许以后新增测试，不再限定总数必须等于 17。测试项是否精简仍待定。默认同时编译 CANN 生产库及现有四个 A5 EP kernel。生产 Server Dedup 入口仍返回 `TILEXR_ERROR_NOT_SUPPORT`；本流程不创建 NPU communicator，不执行去重 kernel，不验证多卡通信或数值正确性。未实现的 checkpoint 会直接报错。

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

需要从固定 HEAD 导出并记录来源提交号时，可使用已有导出入口代替普通同步：

```powershell
tools\validation\push.ps1 -Server <first-host> -RemotePath /absolute/path/TileXR -SshUser <user>
```

此入口完整导出固定 HEAD，仍会自动初始化递归子模块，并附上 commit、来源说明和相对路径列表；不再生成或检查源码 SHA256。这是完整导出的规则，不代表普通源码构建必须安装 spdlog。导出不包含 PC 未提交改动，Shell 文件按 Git 中的 LF 字节交付。远端无需联网拉子模块，也不需要 `.git`。

构建和同步只枚举当前源码路径，不读取源码内容计算摘要，也不与旧清单比较内容。`.tilexr-source.json` 中 `files` 是相对路径列表；没有可用来源时，commit 在终端显示 `unknown`、JSON 中为 `null`，`provenance=file-copy`。本地 Git HEAD 标为 `git-head-unverified`，新导出清单记录的提交号标为 `manifest-unverified`：这些是来源标签，不证明当前文件仍等于该提交。无 Git、缺失子模块元数据、旧版摘要清单或损坏的可选来源信息都不会阻断普通复制目录；路径和 symlink 保护仍保留。

源码或文档变化不再触发一致性失败，也不会自动重建已安装程序。需要验证更新后的源码时，仍按第 4～7 步构建、同步和运行；脚本只确认安装包身份及产物摘要，不保证运行时源码与二进制来自同一时刻。请在 PC 同步完成后开始构建。

目标应为新目录、空目录或本工具此前管理且不含 `.git` 的验证目录。刷新时保留配置，更新源码并删除旧来源清单中已不属于新版本的源码文件；不覆盖 Git checkout 或其他工作目录。可以通过 SSH config 管理端口和密钥，不需要把凭据写入脚本。

### 3. 首节点加载环境

登录首节点并进入上述工作目录，首次使用完成前述文件配置，然后执行：

```bash
source set_env.sh
```

脚本输出 `ENV_READY` 及本次 mode/checkpoint、主机数和 hostfile 路径。CANN 路径错误、配置错误或缺少 Python 时输出 `CONFIG_FAILED` 并给出配置位置。七步入口使用轻量环境：不执行 `npu-smi`/`lspci` 探测，不添加当前流程不用的 MPI/HCCL Test 等工具路径；保留构建实际使用的 `env/util/cmake/bin`，重复加载不会不断增长 PATH/LD_LIBRARY_PATH。实际配置和 production 的 CANN 环境仍会重新加载。直接使用其他公共环境入口的既有行为保持不变。后续步骤均在此目录执行。

### 4. 一键编译

```bash
bash build_all.sh
```

每次先检查路径归属，删除固定 `build/validation/` 内的旧编译内容，再配置和全量编译；不再按 UUID 新建构建目录。生产包和 Host 测试仍使用各自子目录，安装到 `install/validation/`。build_id 和日志目录继续独立记录每次尝试。开始新构建时立即作废旧安装 manifest 和同步记录，即使本次编译失败，也不能继续运行上次安装结果。

Host 测试使用 Debug，保留现有断言；要求当前 17 项必需测试全部注册并安装，允许测试总数增长，新增测试也随清单执行。缺少 Python 测试或缺少安装文件仍失败；本次没有减少测试范围。

production 模式在本次构建完成后执行一次 `nm`、`readelf`、`ldd -r`，检查公共符号、ELF `$ORIGIN` 和真实 driver HAL 依赖，并检查四个 A5 kernel 文件。sync、run 及远端 node 不再重复检查符号或运行库；其他节点的运行库兼容性不会由这一检查覆盖。源码路径/来源标签、安装文件摘要、测试命令、构建模式和工具链日志随构建保留。成功输出 `BUILD_PASS`；编译命令无法启动、非零退出或安装失败归为 `BUILD_FAILED`，附原始日志路径。脚本不要求存在 `3rdparty/spdlog`，日志后端沿用代码中的自动检测。

### 5. 同步所有节点

```bash
bash sync.sh
```

首节点不重复复制自身。其他节点使用相同绝对目录，同步当前枚举的源码、测试脚本、配置和 `install/validation/`，检查安装包身份、CPU 架构及产物摘要；任一节点失败，总体失败。源码没有内容校验，运行库不再重复检查。`rsync --checksum` 保留，用于决定已有文件是否需要传输，避免同大小/同时间戳的内容变化被漏传；它不生成源码验证报告。只重建本工具的安装目录，不同步 `.git`、本地 CANN 安装、编译缓存和历史日志。

同步目录检查按决定保留：目标必须为空或已有本流程的管理标记，不得为 Git checkout、经过 symlink 或存在活动锁。其他节点若仅由 Mutagen 放入源码而没有管理标记，会被拒绝接管；脚本不会自动把任意非空目录认作本流程目录。

全部节点通过后输出 `SYNC_PASS`，保存 build_id 与实际生效配置。配置签名使用解析后的主机/device 列表和有效环境参数；hostfile 或 `validation.local.sh` 只改注释、空白且不改变实际配置时，不要求重新同步。构建身份或实际配置改变后须重新同步；安装产物变化仍会被检查。源码和文档文字变化本身不使同步记录失效。

### 6. 启动测试

```bash
bash run.sh
```

首节点执行本机测试，其余节点通过 SSH 并行启动。每个远端会话重新加载 `set_env.sh`；不会假设 SSH 继承了首节点的环境。测试从安装包直接运行，不依赖首节点的 CMake 构建树。

### 7. 等待最终结果

`run.sh` 阻塞到测试结束并自动收集结果。成功输出类似：

```text
VALIDATION_PASS checkpoint=ep-sdedup-p1-abi-host mode=production hosts=8/8 tests_per_host=17 commit=unknown
```

只有全部预期主机测试成功、退出码为 0、四个 checkpoint marker 完整、结果属于本次运行且日志汇总完成时才返回 0。失败时返回非零，分别记录错误类别、节点、阶段、原始退出码/超时状态及日志路径，保留首个失败边界和后续收集/清理错误。结果保存在 `run/validation/<run_id>/summary.json`，原始日志按节点/测试保留；不再输出源码 SHA256。

设备能力尚未实现不是成功结果。`mode=host` 的 PASS 仅证明 Host 契约；`mode=production` 增加目标环境生产编译/链接和加载依赖证据，也不证明 NPU launch 或去重通信。

## 失败分类

错误按实际失败边界分类；“推出失败”按进程退出与清理处理。进程失败、结果不通过和日志无法解析分别记录，不根据 stderr 关键词猜测测试结果。

| 类别 | 含义与示例 |
| --- | --- |
| `CONFIG_FAILED` | CANN/hostfile/工具配置错误，或安装包身份、架构、产物检查失败，尚未进入有效测试 |
| `BUILD_FAILED` | 配置、编译或安装失败；编译命令无法启动、非零退出或超时均属于此类 |
| `SYNC_FAILED` | 节点准备、文件传输或同步确认失败 |
| `LAUNCH_FAILED` | 测试或节点进程未能拉起，例如可执行文件不存在、SSH 启动失败 |
| `EXIT_FAILED` | 已启动进程非零退出、被信号终止或超过期限；保留原始退出码，不混作结果校验失败 |
| `RESULT_FAILED` | 执行结束且结果可解析，但报告合法 FAIL marker，或运行身份、测试集合、测试状态不符合预期 |
| `LOG_PARSE_FAILED` | 日志编码、JSON 或结果协议无法解析；必需 PASS marker 缺失或格式非法也属于此类 |
| `LOG_COLLECT_FAILED` | 原始日志或结果文件传输/收集失败；不能据此声称测试逻辑失败 |
| `CLEANUP_FAILED` | 无法确认进程身份、终止失败或未确认所属进程清理完成；已有执行错误另行保留 |

例如，测试已报告 PASS 但日志回传失败，摘要的 `reported_result` 保留节点已报告的结果，同时报告 `LOG_COLLECT_FAILED`，整体流程仍非零退出。`primary_error` 记录最先观察到的失败，`errors` 保留后续错误；编译失败后发生清理错误，应同时保留编译失败和清理失败。解析不了日志时不虚构 PASS/FAIL 结论。

## 维护与验证边界

- hostfile 属于一次性部署配置，环境入口只能读取和提示错误。曾自动生成 localhost 默认文件，这会在 Mutagen 等同步流程中引入错误清单传播的风险，因此已撤销自动创建；回归覆盖缺失文件不落盘、已有多机清单不变、自定义路径缺失及空文件拒绝。
- `git -C <子模块目录>` 会向父目录查找仓库。普通同步遗漏子模块 Git 元数据时，以子模块 commit 查询父仓库会报 `fatal: not a tree object`。源码清单直接枚举实际文件；只有当前目录有 Git 元数据时才尝试读取顶层 HEAD 标签，不遍历子模块对象库。回归须覆盖普通复制、缺失子模块元数据、空 spdlog 和固定提交导出，不要求服务器补 `.git`。
- 不能由 `.gitmodules` 或 CMake include 路径推断依赖是必需的。此前源码复制预检错误地要求 spdlog 头文件，阻断了已有的无 spdlog 日志路径；应核对实际 include 条件、链接目标及替代实现。spdlog 子模块和可选日志封装分别来自 `b063b61`、`b6c88d8`（2026-05-29），均早于本次 Server Dedup。回归需覆盖空子模块仍进入生产配置阶段，并实际编译、运行日志替代实现；这不代替 CANN/NPU 验证。
- 导出检查必须遵循公共接口的链接约定。`tilexr_ep.h` 中两个 Dispatch 入口声明为 `extern "C"`，`nm -C` 也不会给它们加参数括号；原先匹配 `TileXRMoeEpDispatchMemoryServerDedup(` 会在正常导出时误报缺失。现在读取 POSIX 格式的原始动态符号，精确匹配已定义的全局函数名；相似名称、C++ mangled 名、未定义引用和数据符号不能替代接口。真正缺失时显示本次库路径和 `symbols.log`，便于确认产物来源。
- 普通文件清单排除 `.git`、部署配置、build/install/run、Python 缓存、env 等环境或生成物，以及根级 `reference/`、`third_party/` 比较目录；包含实际子模块源码、文档和测试脚本。源码只用于列出同步路径，不校验内容，也不通过文档改动阻断运行；安装包的 build_id 和产物摘要仍用于拒绝旧包或不完整分发。旧源码清单的摘要字段在迁移时忽略，不把它升级为构建依赖。
- 比较目录排除规则必须限定目录层级。无 Git 四入口验证曾因全层排除 `reference` 漏掉 `src/moonep/planner_v2/reference/ep_plan_reference.cpp`，导致 CMake 找不到必需源文件。现在只排除根级 `reference/`、`third_party/`，保留 src/tests 内同名目录；回归复制真实 Planner 源并检查其可分发路径，不能用带导出清单的测试代替普通目录枚举。
- 同一份二进制的符号/RUNPATH 不需要在每个节点重复检查，本流程把它们和运行库检查统一放在生产构建完成后执行一次。各节点安装文件摘要相等不证明它们的 CANN/driver 环境相等；本次取消了远端运行库预检，实际节点执行仍按退出、结果和日志边界报告。
- 固定构建目录清理前必须确认它及安装目录没有越界或 symlink；开始新构建立即作废旧 manifest/同步凭据。否则编译失败后继续 sync/run 可能把上次成功包当作本次结果。日志目录保留独立 build_id，用失败重建行为测试验证此边界。
- 解析后的 hostfile 和有效配置决定同步记录是否失效；不要对配置文件原始字节做签名，也不要用去注释正则解析可执行 Shell。环境加载允许重新配置，同时去重 PATH/LD_LIBRARY_PATH，取消操作使用不依赖完整 CANN 环境的最小入口。
- CTest 注册不等于安装。测试构建成功但未安装时，同步后的节点不能运行；新增测试必须有安装规则，构建脚本核对全部注册命令的可分发路径。本次补齐五个已有测试的安装规则。
- CMake 测试清单可能引用构建机绝对路径。分发时将 C++ 程序映射到安装目录、Python 测试映射到源码目录，用实际子进程执行验证，不能复制构建树后假定远端可用。
- 父进程退出或 SSH 退出不代表测试子进程退出。节点设置自身期限，各测试使用独立进程组，TERM 后必要时 KILL；只清理本次拥有的进程。身份无法确认、清理或日志收集不完整时，整体不得输出 PASS；同时保留已完成测试状态和首个失败，不能用清理/收集错误覆盖它。
- 退出失败与日志损坏可能同时发生：解析非 UTF-8 或损坏 JSON 时，必须附加解析错误，保留已有退出码及清理失败。构建、同步、运行任何阶段遇到清理失败都保留操作锁，避免下一轮删除仍被进程使用的固定构建目录；普通编译失败且进程已退出则释放锁。组合故障回归验证这两个边界。
- 构建、同步和运行使用本目录锁，避免相互覆盖。异常断电可能留下锁；这种异常恢复必须先确认没有活动进程，不自动删除未知任务的锁。
- `TILEXR_CANN_ENV_SCRIPT` 是显式环境选择，`common_env.sh` 不再用仓库内另一套 CANN 环境覆盖它。没有设置此变量的既有调用保持原来的环境发现规则。
- 传输支持带空格的目录，因此环境入口及公共工具中的 `dirname`、`realpath`、`source`、目录创建也必须引用路径；只测 tar/SSH 引号不能证明真正的环境加载可用。远端 watchdog 覆盖环境加载和节点入口，不能仅对测试程序设置超时。
- 本 checkpoint 不触达设备通信，不需要执行 HCCL Test。后续设备阶段必须按 AGENTS.md 复用匹配环境的一次性资格记录，不能把 Host PASS 当作通信基线。

脚本维护测试：`python3 -m unittest discover -s tools/validation -p 'test_*.py' -v`。这些是开发者测试，不是用户七步流程之外的必执行命令。测试使用临时 Git 仓库、模拟传输及实际受控子进程；真实 CANN 构建和真实 SSH 集群通过情况必须以服务器运行日志为准。

以下三段保留历史版本的开发证据，不代替本次精简后的回归。2026-09-09 初始实现验证：Windows/MSYS2 上 41 项脚本行为测试中 40 项通过，1 项因文件系统未创建真实 symlink 而跳过；PowerShell 5.1 同步入口通过模拟传输执行了实际导出、Bash 解包、刷新和拒绝覆盖检查。全新且路径含空格的部署目录中，四个服务器入口以 `mode=host` 完成实际 C++14 Debug 构建、安装和 17/17 Host/Python 测试。CANN 生产构建、真实 SSH 集群同步及 NPU 执行未在本地验证。

同日普通文件同步修复：临时子模块移除 `.git` 后复现 `fatal: not a tree object`。修复后 52 项脚本回归中 51 项通过，1 项因 MSYS 未创建真实 symlink 而跳过；包含普通同步清单跨模拟节点分发及结果摘要检查。无 `.git`、无导出清单且路径含空格的普通复制目录中，首次构建和修改源码后的重建均通过四个服务器入口，每轮 17/17 Host/Python 测试成功；用 Git 拦截程序确认两轮服务器流程均未调用 Git，hostfile 字节不变。hostfile 保持手动配置，不再自动生成。本地没有 CANN/NPU，生产编译及真实 SSH 集群验证仍需以服务器日志为准。

同日可选 spdlog 修正：新增回归先复现 production 模式被空 spdlog 目录阻断，移除预检后确认能进入生产 CMake 配置调用（测试在调用边界停止，不代表生产构建成功）。52 项脚本回归中 51 项通过，1 项 symlink 跳过。在临时目录仅复制真实 `tilexr_log.h`，不提供 spdlog 头文件、不定义 `TILEXR_LOG_DISABLE_SPDLOG`，按项目默认 GNU C++14 模式编译并运行现有日志测试通过，编译断言确认使用自动替代路径；现有 fake-spdlog API 测试亦通过。MSYS 严格 `-std=c++14` 下现有测试的 POSIX `setenv`/`unsetenv` 声明不可见，故使用与 CMake 一致的 `-std=gnu++14`；未修改生产日志代码或测试源码。CANN 生产编译与 NPU 执行未在本地验证。

## 七步流程检查审计与落实（2026-09-09）

原审计源码版本为 `04e16e98cf2c3502d765436c87be43616a172192`。保留以下 50 项 ID 便于逐项核对；“原审计位置”和“原操作”描述该历史提交，代码链接固定到该提交，不随当前行号变化。“本次决定与当前行为”描述本次精简后的实现。范围覆盖根目录入口、环境/源码/工作流工具、可选 PC 导出和 CMake；没有访问实际服务器、用户 Mutagen 配置或已安装 CANN 环境脚本，也没有测量真实集群耗时。

本次已执行的决定包括：取消全部源码内容校验；符号/RUNPATH/运行库集中到每次生产构建末尾一次；固定目录清理重建；保留目录检查；按配置语义判断同步；保留当前全部测试并改为必需集合检查；失败按边界分类。测试项和每节点测试范围是否精简继续标为待定。未涉及的工具约束和兼容行为保留现状。

### 已落实的精简

- `nm`、`readelf`、`ldd -r` 在每次生产构建完成后各执行一次，后续只校验安装包身份与产物摘要。
- 七步入口不调用 `npu-smi`/`lspci`；保留真正的 CANN 加载，去掉无消费者的路径设置和重复 PATH 项。
- hostfile 注释/空白及不改变实际配置的 Shell 注释不使同步失效。
- 源码 SHA256、源码清单内容比较、构建前后源码一致性检查和源码总摘要全部取消；文档继续可以同步，但其变化不阻断验证。
- 保留当前全部 17 项测试，检查必需集合及安装状态，允许总数增加；测试精简待定。
- 固定 `build/validation/` 干净重建；独立日志、旧安装凭据失效和目录保护保留。

### 第 1、2 步：固定版本及 PC 同步

| ID | 原审计位置 | 原操作或检查，失败/影响 | 本次决定与当前行为 |
| --- | --- | --- | --- |
| P01 | PC `git fetch`、`git switch --detach` | 获取并选择验证版本；不自动检查 PC 是否有未提交改动 | 保留版本选择。Mutagen 同步实际文件；提交号只作来源标签，不证明当前文件与该提交相同 |
| P02 | [source.py:165](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/source.py#L165) `pack` | 仅 `push.ps1` 路径执行：初始化全部递归子模块，导出 HEAD 原始字节；未提交改动不导出，子模块获取失败则停止 | 保留可选完整 HEAD 导出及递归子模块初始化；导出清单改为路径列表，不生成内容摘要。Mutagen 路径不需要此入口或可选 spdlog |
| P03 | [push.ps1:12](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/push.ps1#L12) | 校验 host/user 格式、Linux 绝对路径并引用 Shell 参数；禁止换行、路径越界等 | 保留参数引用、格式和路径约束；当前仍不支持 user@host、IPv6 等写法 |
| P04 | [push.ps1:31](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/push.ps1#L31) | 对导出、scp、SSH 返回码逐一判失败；生成并传输临时 tar.gz，最终清理临时包 | 保留可选入口的传输和返回码处理；普通 Mutagen 同步不叠加此入口 |
| P05 | [push.ps1:45](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/push.ps1#L45) | 拒绝根目录、路径中的 symlink、Git checkout、无管理标记的非空目录和活动/残留锁 | 按决定保留全部部署目录、symlink、Git checkout 和锁检查，不自动接管其他非空目录 |
| P06 | [push.ps1:61](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/push.ps1#L61) | 校验归档是普通文件、清单与归档文件集合相等、不得覆盖 hostfile 等本地配置；移除旧清单中的退役源码 | 保留普通文件/归档成员集合、配置和路径保护；旧摘要字典仅取路径，刷新时不检查源码内容 |
| P07 | [push.ps1:100](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/push.ps1#L100)、[source.py:119](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/source.py#L119) | 解包后验证每个源码文件 SHA256；首次构建开始时还会再验证一次固定提交清单 | 已取消解包后的源码 SHA256 及构建前固定清单内容验证；不再调用 source verify |

### 第 3 步及后续入口：环境与配置

| ID | 原审计位置 | 原操作或检查，失败/影响 | 本次决定与当前行为 |
| --- | --- | --- | --- |
| E01 | [set_env.sh:14](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/set_env.sh#L14)、[build_all.sh:3](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/build_all.sh#L3)、[sync.sh:3](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/sync.sh#L3)、[run.sh:3](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/run.sh#L3)、[workflow.py:30](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L30) | 显式第 3 步、三个入口和每个 SSH verify/node/cancel 会话重新 source 配置与公共环境 | 已精简为可重新加载的轻量环境；远端 verify/node 仍加载实际环境；cancel 使用最小入口，不依赖 CANN/hostfile 加载 |
| E02 | [set_env.sh:32](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/set_env.sh#L32)、[workflow.py:134](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L134) | Shell 和 Python 都检查 mode/checkpoint，只允许 host/production 和已实现的 ABI/Host checkpoint | 保留已实现 mode/checkpoint 范围约束；失败明确归为 CONFIG_FAILED |
| E03 | [set_env.sh:37](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/set_env.sh#L37)、[common_env.sh:55](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/scripts/common_env.sh#L55) | production 检查 CANN 脚本可读并 source；可读性检查重复。`TILEXR_CANN_VER=9.1.0` 只是赋值，不是安装版本验证 | 保留可用 CANN 环境加载与公共入口独立调用的可读性检查；轻量 host 模式不加载 fallback CANN。变量赋值仍不是安装版本证明 |
| E04 | [set_env.sh:48](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/set_env.sh#L48)、[workflow.py:566](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L566) | 查找 Python 并要求版本至少 3.9 | 保留 Python 3.9+ 要求和自动工具查找；不增加用户手动验证命令 |
| E05 | [workflow.py:102](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L102) | hostfile 必须存在且有 1..64 台主机，检查字段、重复 host、重复 device ID 及 0..255 范围；为空/格式错误时失败 | 保留当前 hostfile 格式、上限、重复项和 device 范围检查；只读取、不创建文件，不把 device 字段当作实际设备验证 |
| E06 | [workflow.py:136](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L136) | JOBS 限制 1..256，TIMEOUT 限制 30..180 秒；SSH_USER 必须匹配限定格式；hostfile 解析后必须在工作目录内 | 保留当前输入范围、SSH_USER 和 hostfile 路径约束；它们不产生新增用户命令 |
| E07 | [common_env.sh:21](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/scripts/common_env.sh#L21)、[common_util.sh:47](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/scripts/common_util.sh#L47)、[common_util.sh:62](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/scripts/common_util.sh#L62)、[common_util.sh:104](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/scripts/common_util.sh#L104) | 探测 SoC、ops 名、设备数；每次 source 在 npu-smi 可用时执行 3 次 `npu-smi info`，解析失败可能调用 lspci；未设置单独超时 | 已取消七步 checkpoint 的 npu-smi/lspci 探测；其他公共环境调用保持原行为 |
| E08 | [common_env.sh:26](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/scripts/common_env.sh#L26)、[common_env.sh:88](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/scripts/common_env.sh#L88) | 创建 env/temp/util、run/plog；设置 MPI/HCCL Test 路径并向 PATH/LD_LIBRARY_PATH 添加工具目录 | 已精简轻量环境的 MPI/HCCL Test 和无消费者工具路径；保留日志/环境目录及必要 driver 库路径，重复 PATH 项去重 |
| E09 | [common_env.sh:78](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/scripts/common_env.sh#L78) | driver/kernel/inc 不可读时，用 CANN driver 头文件建立本地 driver-shim，并修改 ASCEND_DRIVER_PATH；链接仍指向真实 driver 库 | 保留 production 环境所需 driver-shim 兼容路径；重复 source 从原 driver 恢复后重建链接。host 模式不创建 shim |

### 第 4 步：构建与安装检查

| ID | 原审计位置 | 原操作或检查，失败/影响 | 本次决定与当前行为 |
| --- | --- | --- | --- |
| B01 | [workflow.py:161](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L161)、[workflow.py:573](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L573) | build/sync/run 使用目录锁，遇到活动节点锁或既有操作锁即失败；退出时释放 | 保留目录锁、活动任务保护和退出释放；异常残留锁继续需要先确认所属进程 |
| B02 | [workflow.py:254](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L254) | 强制要求 cmake、ctest、g++、make，并执行 cmake/g++ `--version` 保存日志 | 保留当前 cmake/ctest/g++/make 工具约束和版本日志；这些不是新的部署前置检查，版本输出也不是兼容性证明 |
| B03 | [source.py:142](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/source.py#L142)、[source.py:91](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/source.py#L91) | 构建前读取 Git/固定导出清单，或扫描普通文件并计算 SHA256；元数据不完整时使用 commit=null，不要求服务器 Git | 已改为 inventory 路径枚举，不读源码字节、不做 Git dirty/子模块对象校验；commit 明确未校验，旧或损坏来源清单不阻断 |
| B04 | [source.py:43](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/source.py#L43)、[source.py:119](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/source.py#L119) | 拒绝越界/绝对路径/.git 路径、源码 symlink、非法摘要及文件缺失/变化；只检查清单中已有文件 | 按决定保留越界、symlink 和普通文件路径保护；非法摘要、旧清单文件缺失/内容变化检查已取消 |
| B05 | [workflow.py:256](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L256)、[workflow.py:263](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L263) | 每次生成 UUID 新构建目录，生产包与 Host 测试各自 configure/build/install；二者均传 Debug | 已改为固定 build/validation，检查路径后删除旧编译内容再全量编译；生产/Host 仍分子目录并保留 Debug，日志独立 |
| B06 | [src/comm/CMakeLists.txt:30](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/src/comm/CMakeLists.txt#L30) | CMake 编译探测 PTO SDMA 头文件/API，检查 intrinsic 与真实 libnnopbase；不可用时关闭可选 SDMA | 保留可选 SDMA 能力探测及不可用时继续编译；不新增 PTO 或 spdlog 硬依赖 |
| B07 | [src/ep/CMakeLists.txt:5](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/src/ep/CMakeLists.txt#L5)、[CMakeDetermineCCECompiler.cmake:11](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/src/collectives/kernels/cmake/CMakeDetermineCCECompiler.cmake#L11)、[TileXRKernelArtifact.cmake:4](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/src/cmake/TileXRKernelArtifact.cmake#L4) | 生产 EP 查找 ccec/ld.lld、构建并安装四个 A5 kernel，检查 kernel artifact 的 CMake target/参数；CCE 测试文件仅把 WORKS 设为 1 | 保留当前 production 包及四个 A5 kernel 的实际编译安装；host 模式继续用于无 CANN 本地验证，不扩大设备通过声明 |
| B08 | [workflow.py:271](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L271) | 安装后检查 dispatch/combine/dispatch_memory/combine_memory 四个 kernel 文件存在 | 保留四个 kernel 安装文件存在性检查；不表示执行过 kernel |
| B09 | [workflow.py:216](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L216) | 原检查错误地匹配带 `(` 的名称；现用 `nm -D --defined-only --no-demangle --format=posix` 精确核对两个 Dispatch 的 C ABI 动态函数符号 | 已修复误报；真正缺失时仍失败并显示库路径与 symbols.log。每次生产构建完成后一次；sync/run/node 不再调用 nm |
| B10 | [workflow.py:220](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L220) | `readelf -d`，要求恰好一条 RPATH/RUNPATH 且含完整 `[$ORIGIN]` 字段 | 已集中到每次生产构建完成后一次；保留既有 $ORIGIN 加载路径规则，sync/run/node 不再调用 readelf |
| B11 | [workflow.py:224](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L224) | `ldd -r` 拒绝 not found/undefined symbol/devlib，并要求 libascend_hal 解析到配置的真实 driver 路径（兼容 shim） | 按决定每次生产构建完成后仅执行一次 ldd -r；sync/run/node 不再检查运行库，其他节点库环境兼容性不在此证据内 |
| B12 | [workflow.py:281](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L281) | `ctest --show-only=json-v1` 只列出命令；将 Python/C++ 测试映射到分发路径，检查安装文件存在；不接受其他命令形态 | 保留注册清单解析、命令映射与安装文件检查；CTest 清单解析失败归为 LOG_PARSE_FAILED，命令本身不执行测试 |
| B13 | [workflow.py:296](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L296) | 要求恰好 17 项且包含四个 Server Dedup 测试；新增无关测试同样导致构建失败 | 已改为必需测试集合、唯一名称和安装检查，允许新增项；当前全部 17 项仍必需，测试项精简待定 |
| B14 | [workflow.py:299](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L299) | 编译后再次读取所有记录源码并比 SHA256，防止构建过程中 Mutagen 改动源码；计算安装文件摘要，写 manifest | 已取消构建后源码 SHA256 比较；保留安装产物摘要及构建 manifest，不保证源码在构建期间未被同步修改 |
| B15 | [workflow.py:306](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L306) | 拒绝 install/validation 及父目录 symlink，移除旧安装目录后以本次 stage 替换，写源码清单及管理标记 | 保留目录保护及成功后替换安装包；开始新构建立刻作废旧 manifest 和 sync 记录，失败后不能沿用旧包 |

### 第 5 步：节点同步

| ID | 原审计位置 | 原操作或检查，失败/影响 | 本次决定与当前行为 |
| --- | --- | --- | --- |
| S01 | [workflow.py:177](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L177)、[workflow.py:325](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L325) | 确认 hostfile 首项是当前主机；依次尝试本机名称、IP/DNS，必要时 SSH hostname；第 6 步重复 | 保留首节点身份确认，避免把远端误作本机而跳过同步；当前既有探测顺序不变 |
| S02 | [workflow.py:326](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L326) | 多机才要求 ssh/rsync；固定 BatchMode、连接期限与保活 | 保留多机所需 ssh/rsync、免交互连接、连接期限与保活；不安装密钥或启动 NPU 检查 |
| S03 | [workflow.py:240](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L240)、[workflow.py:329](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L329) | 首节点验证 mode/checkpoint/CPU 架构、源码 SHA256、安装文件集合及 SHA256，再执行全部 runtime_check | 保留 mode/checkpoint/CPU 架构、build_id 和安装产物摘要；源码校验与全部 runtime_check 已从 sync 移除 |
| S04 | [workflow.py:330](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L330) | 删除旧 sync.json，只在本次所有节点成功后写新记录；记录 build_id 与配置摘要 | 保留同步前删除旧记录、全部节点通过才记录新 build_id 和有效配置的语义 |
| S05 | [workflow.py:332](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L332) | 仅同步清单中源码、安装包、hostfile/local 配置和管理清单；不复制 build、历史 run 和环境 | 已改用同步时的实际源码路径列表，包含文档而不校验内容；继续排除环境、缓存、历史日志并分发配置和安装包 |
| S06 | [workflow.py:346](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L346) | 远端只接受空目录或有 `.tilexr-validation-root` 的目录，拒绝 Git checkout/路径 symlink/活动锁 | 按决定保留全部目标目录所有权、symlink、Git checkout 和活动锁检查；未管理的 Mutagen 非空目录仍拒绝接管 |
| S07 | [workflow.py:360](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L360) | 同步前删除远端 install/validation；首节点没有 local 配置时删除远端同名配置，否则 rsync 覆盖配置及 hostfile | 保留受管理目标的旧安装目录清理与首节点配置分发；hostfile 不由 source 自动创建，所有权检查不放宽 |
| S08 | [workflow.py:375](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L375) | `rsync --checksum` 以内容判断已有文件是否需要更新；之后还执行 Python SHA256 验证 | 按决定保留 rsync --checksum 的传输选择；后续只做安装产物 SHA256，源码 SHA256 验证已取消 |
| S09 | [workflow.py:379](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L379)、[workflow.py:587](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L587) | 每个远端重新加载环境、运行 verify_build 的全部检查，再比较请求的 build_id；任一失败则整体失败 | 保留远端环境、build_id、CPU 架构及安装产物摘要确认；不再执行源码或 nm/readelf/ldd 检查 |
| S10 | [workflow.py:154](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L154)、[workflow.py:387](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L387) | 配置签名既含解析 host 列表，又包含 hostfile 原始字节 SHA256；local 配置也按原始字节摘要；含 timeout 等参数 | 已移除 hostfile/local 配置原始字节摘要，使用解析后的 hosts/devices 与实际生效配置；仅注释/空白变化不阻断 |

### 第 6、7 步：执行、取消与结果

| ID | 原审计位置 | 原操作或检查，失败/影响 | 本次决定与当前行为 |
| --- | --- | --- | --- |
| R01 | [workflow.py:474](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L474) | 首节点再次检查自身身份与 verify_build，再要求 sync.json 的 build_id/配置完全匹配；单机也必须先 sync | 保留首节点、安装包与 sync 凭据检查；凭据只关联 build_id 和有效配置，源码文字变化不使其失效 |
| R02 | [workflow.py:486](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L486) | 本机用 Python 子进程、其他节点经 SSH 启动一个 node；每主机执行一套 Host 测试 | 保留每主机执行全部 Host 测试；是否精简测试项或其他节点重复范围待定，device 列表仍不启动 NPU rank |
| R03 | [workflow.py:399](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L399) | 每节点保存 PID/starttime，设置 TERM/HUP 处理和期限，检查取消标记，再次 verify_build 并确认 build_id | 保留节点身份、取消标记、期限和安装包身份/摘要检查；移除源码及符号/运行库重复检查 |
| R04 | [workflow.py:421](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L421) | 按 manifest 逐个执行测试；非零退出失败；四个 Server Dedup 测试还要求完整 PASS marker 行 | 保留逐测试执行及 checkpoint marker；非零退出归 EXIT_FAILED，合法 FAIL marker 归 RESULT_FAILED，缺失/非法 marker 归 LOG_PARSE_FAILED |
| R05 | [workflow.py:64](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L64)、[workflow.py:82](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L82) | 各外部命令有独立进程组，退出/超时均执行 TERM，必要时 KILL；保留输出日志，非零显示尾部 | 保留所属进程组 TERM/KILL 和原始日志；区分启动、退出/超时与清理失败，不按 stderr 文本猜测结果 |
| R06 | [workflow.py:30](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L30)、[workflow.py:411](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L411)、[workflow.py:475](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L475) | 环境+远端命令 watchdog、节点最多 120 秒、控制端总期限默认 180 秒；为清理/收集预留时间；构建/同步另计 | 保留节点/控制端期限及清理收集预算；远端环境仍在 watchdog 内，源码内容扫描已经删除 |
| R07 | [workflow.py:445](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L445)、[workflow.py:504](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L504) | 首个启动/测试失败后向所有节点发取消；核对 PID starttime 防 PID 复用误杀，最多等待清理 8 秒；取消失败也计入总失败 | 保留首个失败触发取消、PID/starttime 归属核对和清理确认；cancel 已改最小入口，清理失败另记而不掩盖原错误 |
| R08 | [workflow.py:523](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L523) | 从各节点 rsync 当前 run_id 日志和 result.json；日志收集失败也使总体失败 | 保留日志/result 收集；失败单列 LOG_COLLECT_FAILED，保留已知测试状态，整体仍非零退出，不混作结果校验失败 |
| R09 | [workflow.py:463](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L463)、[workflow.py:543](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tools/validation/workflow.py#L543) | 检查 run_id/build_id、mode/checkpoint、PASS、cleanup、测试集合无缺失/重复且逐项通过，再检查总期限；写 summary | 保留运行身份、必需结果集合/唯一性、状态和清理检查；分类记录首个边界与副错误，日志无法解析不伪造结果 |

### 精简后的检查次数

当前调用链没有 `md5sum` 或 `sha256sum` 命令，也不再计算源码文件或源码清单的 SHA256。Python `hashlib.sha256` 仅保留在安装产物检查；`rsync --checksum` 用于选择需传输的文件，其算法由实际安装的 rsync 决定。

下表按 **Mutagen/普通复制、production、正常成功、N 台主机、一遍第 3～7 步**统计当前调用路径。不计 PC 导出、重试、失败取消、外部 CANN 脚本内部动作、rsync 内部工作和测试内部检查；这是代码调用次数，不是服务器实测耗时。

| 操作 | 首节点 | 每个其他节点 | N=8 总数 | 当前状态 |
| --- | --- | --- | --- | --- |
| 加载 set_env/common_env | 4：显式 source、build、sync、run | 2：同步 verify、运行 node | 18 次轻量加载 | 保留会话环境，去掉无消费者的探测/路径设置 |
| 本流程的 `npu-smi`/`lspci` | 0 | 0 | 0 | 已取消当前 checkpoint 的探测 |
| 源码路径枚举 | 2：build、sync | 0 | 2 次目录遍历 | 只列路径，不读取源码内容 |
| 源码 SHA256/清单总摘要 | 0 | 0 | 0 | 全部取消，原来为 19 次源码扫描 |
| 安装产物 SHA256 | 4：建清单、sync 本机、run 控制端、run 本机 node | 2：sync verify、run node | 18 次扫描 | 保留安装包分发和运行身份检查 |
| `nm` + `readelf` + `ldd -r` | 构建完成后各 1 次 | 0 | 1 组，共 3 个命令 | 每次生产构建仅一次；host 模式不执行 |
| Host/Python 测试 | 当前 17 项 | 当前 17 项 | 当前 136 个测试进程 | 全部保留；测试项及每节点范围精简待定 |

源码内容一致性已经按决定退出验证判据。来源 commit 仅用于记录，build_id 和安装文件摘要标识实际分发包；源码变化本身不使包失效，不自动触发重建。目录遍历用于确定同步范围，不能称为源码完整性校验。

### 当前 17 项测试的范围

| 测试集合 | 数量 | 位置 | 对本 checkpoint 的判定 |
| --- | --- | --- | --- |
| `test_tilexr_ep_server_dedup_{protocol,layout,host_validation,public_abi}` | 4 | [tests/ep/CMakeLists.txt:247](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tests/ep/CMakeLists.txt#L247) | 本 checkpoint 必需，验证协议/布局/Host 参数/公共 ABI，不运行设备去重 |
| `test_tilexr_ep_{layout,expert_ids,host_validation,kernel_launch}` | 4 | [tests/ep/CMakeLists.txt:370](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tests/ep/CMakeLists.txt#L370) | EP 既有功能回归，当前全部保留为必需项；是否精简及每节点重复待定 |
| `test_tilexr_moonep_planner_public_abi`、`test_tilexr_ep_plan_{layout,reference,algorithm,downstream,host}`、`test_tilexr_moonep_planner_compat` | 7 | [tests/ep/CMakeLists.txt:374](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tests/ep/CMakeLists.txt#L374) | Planner 回归，当前全部保留为必需项；是否精简待定 |
| `test_dispatch_memory_trace`、`test_msprof_kernel_stats` | 2 | [tests/ep/CMakeLists.txt:27](https://github.com/LingquLab/TileXR/blob/04e16e98cf2c3502d765436c87be43616a172192/tests/ep/CMakeLists.txt#L27) | Python 日志/统计工具回归，不执行 profiler 或采集 NPU 性能；当前全部保留为必需项，是否精简待定 |

### 已确认取消或没有执行的流程

`04e16e9` 已取消 spdlog 缺失硬阻断；`67168ba` 已取消环境入口自动创建 hostfile，并支持普通复制目录不带 Git 元数据。当前正常服务器路径不执行 Git clone/submodule、HCCL Test、NPU 空闲检查、设备通信、正式 NPU profiler 或依赖安装。`common_env.sh` 定义的安装函数/权限修复函数不等于已调用，不能仅通过关键词命中把它们列成必执行步骤。

原审计的定点验证在临时目录完成：模拟 npu-smi 确认旧版单次 source 调用三次；hostfile 只加注释导致旧 signature 变化；普通文档修改触发旧 source mismatch；模拟 readelf 输出 `$ORIGIN:$ORIGIN/extra` 被既有 RUNPATH 规则拒绝。这些是 `04e16e9` 的历史证据，前三项行为已在本次精简中移除。

本次精简后验证：Windows/MSYS2 上 87 项脚本行为测试中 84 项通过，3 项因 MSYS 未建立真实 symlink 而跳过，分别涉及构建目录保护、driver shim 和源码路径。覆盖源码清单及实际 PowerShell/Bash 部署、环境重载、单次库检查调用顺序、固定目录构建、必需测试集合、模拟双节点执行和各类故障；退出叠加日志损坏/清理失败、build/sync 清理失败保留锁的回归先复现问题，再通过修复。生产构建调用顺序使用模拟安装产物，不作为 CANN 编译证据。

无 `.git`、无来源清单、空 spdlog 且路径含空格的普通复制目录中，两轮 `source set_env.sh` → `build_all.sh` → `sync.sh` → `run.sh` 完成真实 C++14 Debug 编译，每轮 17/17 Host/Python 测试通过；第二轮清除固定目录旧产物，build_id 改变，Git 拦截器确认服务器流程未调用 Git，hostfile 字节不变。向真实参与构建的 `tests/ep/unit/test_tilexr_ep_server_dedup_layout.cpp` 注入编译错误后，报告 `BUILD_FAILED`，旧 manifest/同步记录失效，sync/run 均拒绝旧包。CANN 生产编译、真实 SSH 集群、Linux 真实 symlink 和 NPU 验证仍未在本地执行。

同日 C ABI 导出检查修复：正确 C 符号的命令输出夹具先复现旧检查误报，修复后完整脚本回归 89 项中 86 项通过、3 项因 MSYS symlink 限制跳过。新用例覆盖两个接口独立缺失、相似名称、C++ mangled 名、未定义/局部/数据符号及正确导出后继续检查 RUNPATH/HAL。另使用现有测试 ACL 头文件夹具，以 C++14 重新编译当前两个 Dispatch Host 源文件；真实 `nm --defined-only --no-demangle --format=posix` 的对象符号均为不带括号的 `T` 函数名。该对象编译证据不代表 CANN 生产共享库的链接、动态符号表或 NPU 执行已在本地验证。
