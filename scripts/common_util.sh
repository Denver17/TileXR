#!/usr/bin/env bash

script_path=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)

GREEN=$'\033[0;32m'
YELLOW=$'\033[0;33m'
RED=$'\033[0;31m'
PURPLE=$'\033[38;5;171m'
END=$'\033[0m'

colorful_time() {
    _desc="time cost"
    if command -v ${script_path}/env/util/time/bin/time >/dev/null 2>&1; then
        ${script_path}/env/util/time/bin/time -f "[OK] ${GREEN}✅${END} ${_desc}: ${PURPLE}%e${END}s" "$@"
    elif command -v /usr/bin/time >/dev/null 2>&1; then
        /usr/bin/time -f "[OK] ${GREEN}✅${END} ${_desc}: ${PURPLE}%e${END}s" "$@"
    else
        TIMEFORMAT="[OK] ${_desc}: %Rs" && time $@
    fi
}

colorful_time_with_desc() {
    _desc=$1
    shift 1
    if command -v ${script_path}/env/util/time/bin/time >/dev/null 2>&1; then
        ${script_path}/env/util/time/bin/time -f "[OK] ${GREEN}✅${END} ${_desc}: ${PURPLE}%e${END}s" "$@"
    elif command -v /usr/bin/time >/dev/null 2>&1; then
        /usr/bin/time -f "[OK] ${GREEN}✅${END} ${_desc}: ${PURPLE}%e${END}s" "$@"
    else
        TIMEFORMAT="[OK] ${_desc}: %Rs" && time $@
    fi
}

success() { echo -e "[OK] \033[0;32m✅\033[0m $@"; }
error()   { echo -e "[ERROR] \033[0;31m❌\033[0m $@"; }
warn()    { echo -e "[WARN] \033[0;33m🔶\033[0m $@"; }

line()    { echo "##########################################################"; }

davinci_detect() {
    if ! command -v lspci >/dev/null 2>&1; then
        return
    fi
    lspci -n -D 2>/dev/null | grep -o '19e5:d[0-9a-f]\{3\}' | head -n1 | cut -d: -f2
}

npu_smi_chip_detect() {
    if ! command -v npu-smi >/dev/null 2>&1; then
        return
    fi

    npu-smi info 2>/dev/null | awk -F'|' '
        /^[|][[:space:]]*[0-9]+[[:space:]]+[|]/ {
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", $3)
            if ($3 ~ /^Ascend/) {
                print $3
                exit
            }
        }'
}

ascend_dev_num() {
    local count=0
    if command -v npu-smi >/dev/null 2>&1; then
        count=$(npu-smi info 2>/dev/null | awk -F'|' '
            /^[|][[:space:]]*[0-9]+[[:space:]]+[|]/ {
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", $3)
                if ($3 ~ /^Ascend/) {
                    count++
                }
            }
            END { print count + 0 }')
    fi

    if [ "${count}" -eq 0 ]; then
        count=$(lspci -n -D 2>/dev/null | grep -o '19e5:d[0-9a-f]\{3\}' | wc -l)
    fi

    echo "${count}"
}

soc_name() {
    local npu_name
    npu_name=`npu_smi_chip_detect`
    case "${npu_name}" in
        Ascend950*) echo "ascend950"; return ;;
        Ascend910B*) echo "ascend910b"; return ;;
        Ascend910A*) echo "ascend910_93"; return ;;
        Ascend310P*) echo "ascend310p3"; return ;;
    esac

    davinci_type=`davinci_detect`
    if [ "${davinci_type}" = "" ]; then
        echo "ascend910b"
        return
    fi
    case "${davinci_type}" in
        d802) echo "ascend910b" ;;
        d803) echo "ascend910_93" ;;
        *) echo "ascend910b" ;;
    esac
}

ops_name() {
    name=`soc_name`
    case "${name}" in
        ascend950) echo "950" ;;
        ascend910b) echo "910b" ;;
        ascend910_93) echo "A3" ;;
        ascend310p3) echo "310p" ;;
        *) echo "910b" ;;
    esac
}

# 修复指定路径及其所有祖先目录的权限为 755（直至 /、/home、/root 或 /tmp）
fix_permissions() {
    local fix_path=$1
    while [ "${fix_path}" != "/" ] && [ "${fix_path}" != "/home" ] && [ "${fix_path}" != "/root" ] && [ "${fix_path}" != "/tmp" ]; do
        local perm=`stat -c "%a" ${fix_path}`
        if [ "${perm}" != "755" ]; then
            warn "fix permission to 755 for ${fix_path}"
            if ! chmod 755 ${fix_path}; then
                warn "skip permission fix for ${fix_path}"
                return
            fi
        fi
        fix_path=$(dirname "${fix_path}")
    done
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    success "test success" 1 2 3
    error "test error"
    warn "test warn"

    success `davinci_detect`
    success `soc_name`
fi
