#!/bin/bash

if [ -z "${BASH_SOURCE[0]}" ]; then
    echo "请在 bash 中执行脚本"
    return
fi

script_path=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
source "${script_path}/common_util.sh"

_tilexr_os_arch() {
    case "`uname -m`" in
        arm64) echo "aarch64" ;;
        *) uname -m ;;
    esac
}

export TILEXR_OS_ARCH=`_tilexr_os_arch`
export TILEXR_CANN_VER="9.1.0"

# 七步 Host checkpoint 不使用 SoC/卡数；普通公共入口保留硬件探测。
if [ "${TILEXR_ENV_LIGHTWEIGHT:-0}" != 1 ]; then
    export TILEXR_SOC_NAME=`soc_name`
    export TILEXR_OPS_NAME=`ops_name`
    export TILEXR_ASCEND_DEV_NUM=`ascend_dev_num`
fi
export TILEXR_HOME=$(realpath "${script_path}/..")
export TILEXR_3RD_HOME=${TILEXR_HOME}/3rdparty
export TILEXR_3RD_OPEN_HOME=${TILEXR_3RD_HOME}/open_source
export TILEXR_ENV_HOME=${TILEXR_HOME}/env
export TILEXR_CANN_HOME="${TILEXR_CANN_HOME:-${TILEXR_ENV_HOME}/cann}"
export TILEXR_TEMP_HOME=${TILEXR_ENV_HOME}/temp
export TILEXR_UTIL_HOME=${TILEXR_ENV_HOME}/util
export TILEXR_DRIVER_SHIM_HOME=${TILEXR_ENV_HOME}/driver-shim

mkdir -p "${TILEXR_ENV_HOME}"
mkdir -p "${TILEXR_TEMP_HOME}"
mkdir -p "${TILEXR_UTIL_HOME}"

if [ "${TILEXR_ENV_LIGHTWEIGHT:-0}" != 1 ]; then
    export TILEXR_HCCL_TEST_HOME=${TILEXR_CANN_HOME}/cann/tools/hccl_test
    export ASCEND_DIR=${TILEXR_CANN_HOME}/cann
    export MPI_HOME=${TILEXR_UTIL_HOME}/mpich
fi

# 运行日志相关目录
export TILEXR_RUN_HOME=${TILEXR_HOME}/run
export TILEXR_PLOG_HOME=${TILEXR_RUN_HOME}/plog
export TILEXR_PROF_HOME=${TILEXR_RUN_HOME}/prof
export TILEXR_PLOG_FILE_PATH=${TILEXR_TEMP_HOME}/logfile

date_str=`date '+%y%m%d%H%M'`
export ASCEND_PROCESS_LOG_PATH=${TILEXR_PLOG_HOME}/$date_str
export ASCEND_GLOBAL_LOG_LEVEL=3
mkdir -p "${TILEXR_PLOG_HOME}"

# 显式 Host 模式不加载任何 CANN fallback，也不设置运行库或 driver shim。
if [ "${TILEXR_ENV_LIGHTWEIGHT:-0}" != 1 ] || [ "${TILEXR_VALIDATION_BUILD_MODE:-production}" != host ]; then
    if [ -n "${TILEXR_CANN_ENV_SCRIPT:-}" ]; then
        if [ ! -r "${TILEXR_CANN_ENV_SCRIPT}" ]; then
            echo "CANN environment script is not readable: ${TILEXR_CANN_ENV_SCRIPT}" >&2
            return 1
        fi
        source "${TILEXR_CANN_ENV_SCRIPT}" || return 1
    elif [ -f "${TILEXR_CANN_HOME}/cann/set_env.sh" ]; then
        source "${TILEXR_CANN_HOME}/cann/set_env.sh"
    fi

    if [ -z "${TILEXR_CANN_ENV_SCRIPT:-}" ] && [ -f "${TILEXR_CANN_HOME}/cann/vendors/custom_transformer/bin/set_env.bash" ]; then
        source "${TILEXR_CANN_HOME}/cann/vendors/custom_transformer/bin/set_env.bash"
    fi

    if [ -z "${ASCEND_HOME_PATH:-}" ]; then
        if [ -f "${TILEXR_CANN_HOME}/cann/set_env.sh" ]; then
            export ASCEND_HOME_PATH=${TILEXR_CANN_HOME}/cann
        else
            export ASCEND_HOME_PATH=/usr/local/Ascend/ascend-toolkit/latest
        fi
    fi

    export ASCEND_DRIVER_PATH=${ASCEND_DRIVER_PATH:-/usr/local/Ascend/driver}
    # 重复 source（包括子 Bash）时，从 shim 恢复原 driver 后再按当前 CANN 建链。
    if [ "${ASCEND_DRIVER_PATH}" = "${TILEXR_DRIVER_SHIM_HOME}" ] && [ -n "${_TILEXR_DRIVER_SOURCE_PATH:-}" ]; then
        export ASCEND_DRIVER_PATH=${_TILEXR_DRIVER_SOURCE_PATH}
    fi
    if [ ! -r "${ASCEND_DRIVER_PATH}/kernel/inc" ] && [ -d "${ASCEND_HOME_PATH}/${TILEXR_OS_ARCH}-linux/include/driver" ]; then
        export _TILEXR_DRIVER_SOURCE_PATH=${ASCEND_DRIVER_PATH}
        mkdir -p "${TILEXR_DRIVER_SHIM_HOME}/kernel" "${TILEXR_DRIVER_SHIM_HOME}/lib64"
        ln -sfn "${ASCEND_HOME_PATH}/${TILEXR_OS_ARCH}-linux/include/driver" "${TILEXR_DRIVER_SHIM_HOME}/kernel/inc"
        ln -sfn "${ASCEND_DRIVER_PATH}/lib64/driver" "${TILEXR_DRIVER_SHIM_HOME}/lib64/driver"
        if [ -d "${ASCEND_DRIVER_PATH}/lib64/common" ]; then
            ln -sfn "${ASCEND_DRIVER_PATH}/lib64/common" "${TILEXR_DRIVER_SHIM_HOME}/lib64/common"
        fi
        export ASCEND_DRIVER_PATH=${TILEXR_DRIVER_SHIM_HOME}
    fi

    export LD_LIBRARY_PATH=${ASCEND_DRIVER_PATH}/lib64/driver:${ASCEND_DRIVER_PATH}/lib64/common:${ASCEND_DRIVER_PATH}/lib64:${ASCEND_HOME_PATH}/${TILEXR_OS_ARCH}-linux/lib64:${LD_LIBRARY_PATH:-}
fi

if [ "${TILEXR_ENV_LIGHTWEIGHT:-0}" != 1 ]; then
    export PATH=${MPI_HOME}/bin:${PATH}
    export PATH=${TILEXR_UTIL_HOME}/cmake/bin:${PATH}
    export PATH=${TILEXR_UTIL_HOME}/ccache:${TILEXR_UTIL_HOME}/ripgrep:${TILEXR_UTIL_HOME}/sshpass/bin:${PATH}
    export PATH=${TILEXR_UTIL_HOME}/time/bin:${TILEXR_UTIL_HOME}/patch/bin:${TILEXR_UTIL_HOME}/pigz:${PATH}

    export LD_LIBRARY_PATH=${MPI_HOME}/lib:${LD_LIBRARY_PATH:-}
else
    # build_all.sh 使用仓库环境已安装的 cmake/ctest。
    export PATH=${TILEXR_UTIL_HOME}/cmake/bin:${PATH}
    # CANN 脚本仍可重新加载，让本机配置变更生效；保留路径顺序并去掉重复项。
    _tilexr_unique_path() {
        local name=$1 remaining part value= separator= more=1 first=1
        remaining=${!name-}
        while [ "$more" = 1 ]; do
            if [[ "$remaining" == *:* ]]; then
                part=${remaining%%:*}
                remaining=${remaining#*:}
            else
                part=$remaining
                more=0
            fi
            if [ "$first" = 1 ] || [[ ":$value:" != *":$part:"* ]]; then
                value+="$separator$part"
                separator=:
                first=0
            fi
        done
        export "$name=$value"
    }
    _tilexr_unique_path PATH
    _tilexr_unique_path LD_LIBRARY_PATH
    unset -f _tilexr_unique_path
fi

env_print() {
    line
    success "TILEXR_OS_ARCH = ${TILEXR_OS_ARCH}"
    success "TILEXR_SOC_NAME = ${TILEXR_SOC_NAME}"
    success "TILEXR_OPS_NAME = ${TILEXR_OPS_NAME}"
    success "TILEXR_CANN_VER = ${TILEXR_CANN_VER}"
    success "TILEXR_HOME = ${TILEXR_HOME}"
    success "TILEXR_CANN_HOME = ${TILEXR_CANN_HOME}"
    line
    env | grep ASCEND
    line
}

# 解压 tarball 并通过 autoconf configure/make/make install 安装工具包
# 用法: _install_autoconf_pkg <pkg_name> <tarball> [configure_extra_args...]
# 安装到 ${TILEXR_UTIL_HOME}/<pkg_name>/，日志追加到 ${TILEXR_TEMP_HOME}/3rd.log
_install_autoconf_pkg() {
    local pkg_name=$1
    local tarball=$2
    shift 2
    warn "install ${pkg_name} begin"
    mkdir -p ${TILEXR_UTIL_HOME}/${pkg_name}/
    mkdir -p ${TILEXR_TEMP_HOME}/${pkg_name}/
    colorful_time tar -xzf ${TILEXR_3RD_OPEN_HOME}/${tarball} --overwrite --strip-components=1 -C ${TILEXR_TEMP_HOME}/${pkg_name}/
    cd ${TILEXR_TEMP_HOME}/${pkg_name}/
    colorful_time ./configure --prefix=${TILEXR_UTIL_HOME}/${pkg_name}/ "$@" >> ${TILEXR_TEMP_HOME}/3rd.log
    colorful_time make -j`nproc` >> ${TILEXR_TEMP_HOME}/3rd.log
    make install >> ${TILEXR_TEMP_HOME}/3rd.log
    if [ $? -eq 0 ]; then
        success "install ${pkg_name} success."
    else
        error "install ${pkg_name} failed."
        return 1
    fi
    cd ${TILEXR_HOME}
}

if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    env_print
fi
