#!/usr/bin/env bash
# 七步验证入口。CANN 路径等在 validation.local.sh 配置，hostfile 一次性填写实际主机。
# source 只读取 hostfile，不创建或修改它，避免干扰文件同步。
# validation.local.sh 示例（只需修改实际环境不同的项，不提交部署配置）：
#   TILEXR_CANN_ENV_SCRIPT=/path/to/cann-9.1.0/set_env.sh
#   ASCEND_DRIVER_PATH=/usr/local/Ascend/driver
#   TILEXR_VALIDATION_SSH_USER=your_user
#   TILEXR_VALIDATION_HOSTFILE=hostfile
#   TILEXR_VALIDATION_JOBS=8
# hostfile：第一行必须是当前服务器；每行 host [devices=0,1,2,3,4,5,6,7]。
# 默认构建生产包。无 CANN 的本地验证可显式配置 TILEXR_VALIDATION_BUILD_MODE=host；
# 该模式的 PASS 仅覆盖 Host 测试，不作为 CANN 生产构建通过证据。

_tilexr_validation_env() {
    local root config tool TILEXR_ENV_LIGHTWEIGHT=1
    root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P) || return 1
    config="$root/validation.local.sh"
    if [[ -f "$config" ]]; then
        source "$config" || { echo "type=CONFIG_FAILED 配置失败：无法加载 $config" >&2; return 1; }
    fi
    export TILEXR_VALIDATION_ROOT="$root"
    export TILEXR_VALIDATION_BUILD_MODE="${TILEXR_VALIDATION_BUILD_MODE:-production}"
    export TILEXR_VALIDATION_CHECKPOINT="${TILEXR_VALIDATION_CHECKPOINT:-ep-sdedup-p1-abi-host}"
    export TILEXR_VALIDATION_HOSTFILE="${TILEXR_VALIDATION_HOSTFILE:-hostfile}"
    export TILEXR_VALIDATION_SSH_USER="${TILEXR_VALIDATION_SSH_USER:-}"
    export TILEXR_VALIDATION_JOBS="${TILEXR_VALIDATION_JOBS:-4}"
    export TILEXR_VALIDATION_TIMEOUT="${TILEXR_VALIDATION_TIMEOUT:-180}"
    export TILEXR_VALIDATION_PYTHON="${TILEXR_VALIDATION_PYTHON:-python3}"
    if [[ "$TILEXR_VALIDATION_HOSTFILE" != /* ]]; then
        export TILEXR_VALIDATION_HOSTFILE="$root/$TILEXR_VALIDATION_HOSTFILE"
    fi
    case "$TILEXR_VALIDATION_CHECKPOINT" in
        ep-sdedup-p1-abi-host|ep-sdedup-p1-empty-kernel|ep-sdedup-p1-dry-run) ;;
        *) printf 'type=CONFIG_FAILED 配置失败：TILEXR_VALIDATION_CHECKPOINT=%q；支持 ep-sdedup-p1-abi-host、ep-sdedup-p1-empty-kernel、ep-sdedup-p1-dry-run。请检查 %s。\n' "$TILEXR_VALIDATION_CHECKPOINT" "$config" >&2; return 1 ;;
    esac
    if [[ "$TILEXR_VALIDATION_CHECKPOINT" == ep-sdedup-p1-empty-kernel && "$TILEXR_VALIDATION_BUILD_MODE" != production ]]; then
        echo "type=CONFIG_FAILED 配置失败：empty-kernel 必须使用 production 模式。" >&2
        return 1
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
    source "$root/scripts/common_env.sh" || { echo 'type=CONFIG_FAILED 配置失败：无法加载公共环境或 CANN 环境。' >&2; return 1; }
    for tool in "$TILEXR_VALIDATION_PYTHON"; do
        command -v "$tool" >/dev/null || { echo "type=CONFIG_FAILED 配置失败：缺少依赖 $tool" >&2; return 1; }
    done
    "$TILEXR_VALIDATION_PYTHON" "$root/tools/validation/workflow.py" env || return 1
}

_tilexr_validation_env
_tilexr_validation_status=$?
unset -f _tilexr_validation_env
return "$_tilexr_validation_status" 2>/dev/null || exit "$_tilexr_validation_status"
