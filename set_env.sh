#!/usr/bin/env bash
# 七步验证入口。CANN 路径等在 validation.local.sh 配置，hostfile 一次性填写实际主机。
# source 只加载环境；hostfile 内容由运行入口读取，不创建或修改配置文件。
# validation.local.sh 示例（只需修改实际环境不同的项，不提交部署配置）：
#   TILEXR_CANN_ENV_SCRIPT=/path/to/cann-9.1.0/set_env.sh
#   ASCEND_DRIVER_PATH=/usr/local/Ascend/driver
#   TILEXR_VALIDATION_SSH_USER=your_user
#   TILEXR_VALIDATION_HOSTFILE=hostfile
#   TILEXR_VALIDATION_JOBS=8
# hostfile：第一行必须是当前服务器；每行一个 IPv4，整机使用 0..7 卡。
# 本入口只加载环境；测试参数属于调用脚本。
# 局部作用域隔离旧配置文件遗留的测试参数，保留调用方传入值。
_tilexr_load_deployment_config() {
    local TILEXR_VALIDATION_CHECKPOINT TILEXR_VALIDATION_BUILD_MODE
    local TILEXR_VALIDATION_RUN_MODE TILEXR_VALIDATION_LOOPS TILEXR_VALIDATION_WARMUP
    local TILEXR_VALIDATION_BS TILEXR_VALIDATION_H TILEXR_VALIDATION_TOPK
    local TILEXR_VALIDATION_MOE_EXPERT_NUM TILEXR_VALIDATION_DTYPE TILEXR_VALIDATION_BLOCK_DIM
    local TILEXR_VALIDATION_QUANT_MODE TILEXR_VALIDATION_FP8_FORMAT
    local TILEXR_VALIDATION_SERVERS TILEXR_VALIDATION_BREAKDOWN TILEXR_VALIDATION_ACTION
    local TILEXR_VALIDATION_TIMEOUT
    local TILEXR_VALIDATION_CASE_ANALYSIS
    local TILEXR_VALIDATION_ROUTE_MODE TILEXR_VALIDATION_SEED
    # Endpoints belong to this invocation, never to a copied machine config.
    local TILEXR_COMM_ID TILEXR_DEMO_BARRIER_ADDR
    source "$1"
}

_tilexr_validation_env() {
    local root config tool TILEXR_ENV_LIGHTWEIGHT=1
    root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P) || return 1
    config="$root/validation.local.sh"
    if [[ -f "$config" ]]; then
        _tilexr_load_deployment_config "$config" || { echo "type=CONFIG_FAILED 配置失败：无法加载 $config" >&2; return 1; }
    fi
    export TILEXR_VALIDATION_ROOT="$root"
    export TILEXR_VALIDATION_BUILD_MODE="${TILEXR_VALIDATION_BUILD_MODE:-production}"
    export TILEXR_VALIDATION_HOSTFILE="${TILEXR_VALIDATION_HOSTFILE:-hostfile}"
    export TILEXR_VALIDATION_SSH_USER="${TILEXR_VALIDATION_SSH_USER:-}"
    export TILEXR_VALIDATION_JOBS="${TILEXR_VALIDATION_JOBS:-4}"
    export TILEXR_VALIDATION_TIMEOUT="${TILEXR_VALIDATION_TIMEOUT:-180}"
    export TILEXR_VALIDATION_PYTHON="${TILEXR_VALIDATION_PYTHON:-python3}"
    if [[ "$TILEXR_VALIDATION_HOSTFILE" != /* ]]; then
        export TILEXR_VALIDATION_HOSTFILE="$root/$TILEXR_VALIDATION_HOSTFILE"
    fi
    # Distribution, artifact verification and local Host tests do not load CANN
    # or resolve runtime endpoints.
    if [[ ${TILEXR_VALIDATION_ACTION:-} == sync || ${TILEXR_VALIDATION_ACTION:-} == verify ||
          ${TILEXR_VALIDATION_ACTION:-} == host-validate ]]; then
        command -v "$TILEXR_VALIDATION_PYTHON" >/dev/null || return 1
        printf 'ENV_READY root=%s\n' "$root"
        return 0
    fi
    case "$TILEXR_VALIDATION_BUILD_MODE" in
        production)
            export TILEXR_CANN_ENV_SCRIPT="${TILEXR_CANN_ENV_SCRIPT:-/usr/local/Ascend/ascend-toolkit/latest/set_env.sh}"
            if [[ ! -r "$TILEXR_CANN_ENV_SCRIPT" ]]; then
                echo "type=CONFIG_FAILED 配置失败：请在 validation.local.sh 配置 CANN 9.1.0 的 TILEXR_CANN_ENV_SCRIPT：$TILEXR_CANN_ENV_SCRIPT" >&2
                return 1
            fi
            ;;
        host) export TILEXR_CANN_ENV_SCRIPT= ;;
        *) echo 'type=CONFIG_FAILED 配置失败：TILEXR_VALIDATION_BUILD_MODE 必须为 production 或 host。' >&2; return 1 ;;
    esac
    export TILEXR_COMM_ID TILEXR_DEMO_BARRIER_ADDR TILEXR_VALIDATION_MSPROF
    source "$root/scripts/common_env.sh" || { echo 'type=CONFIG_FAILED 配置失败：无法加载公共环境或 CANN 环境。' >&2; return 1; }
    for tool in "$TILEXR_VALIDATION_PYTHON"; do
        command -v "$tool" >/dev/null || { echo "type=CONFIG_FAILED 配置失败：缺少依赖 $tool" >&2; return 1; }
    done
    printf "ENV_READY root=%s\n" "$root"
}

_tilexr_validation_env
_tilexr_validation_status=$?
unset -f _tilexr_validation_env _tilexr_load_deployment_config
return "$_tilexr_validation_status" 2>/dev/null || exit "$_tilexr_validation_status"
