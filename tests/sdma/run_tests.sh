#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
INSTALL_DIR="${SCRIPT_DIR}/install"
TILEXR_ROOT=$(cd "${SCRIPT_DIR}/../.." && pwd)
CANN_HOME="${1:-${ASCEND_HOME_PATH:-}}"

if [ -z "${CANN_HOME}" ]; then
    set +u
    source "${TILEXR_ROOT}/scripts/common_env.sh"
    set -u
else
    export ASCEND_HOME_PATH="${CANN_HOME}"
    export ASCEND_TOOLKIT_HOME="${CANN_HOME}"
    export ASCEND_OPP_PATH="${CANN_HOME}/opp"
    if [ -f "${CANN_HOME}/set_env.sh" ]; then
        set +u
        source "${CANN_HOME}/set_env.sh"
        set -u
    fi
fi

if [ -z "${ASCEND_HOME_PATH:-}" ]; then
    echo "ERROR: ASCEND_HOME_PATH is not set; pass CANN_HOME or source scripts/common_env.sh" >&2
    exit 1
fi

export ARCH="${ARCH:-${TILEXR_OS_ARCH:-$(uname -m)}}"
if [ "${ARCH}" = "arm64" ]; then
    export ARCH="aarch64"
fi
export ASCEND_DRIVER_PATH="${ASCEND_DRIVER_PATH:-/usr/local/Ascend/driver}"
TILEXR_INSTALL_LIB_DIR="${TILEXR_ROOT}/install/lib"
if [ -f "${TILEXR_ROOT}/install/lib64/libtile-comm.so" ]; then
    TILEXR_INSTALL_LIB_DIR="${TILEXR_ROOT}/install/lib64"
fi
TILEXR_COMM_LIB="${TILEXR_INSTALL_LIB_DIR}/libtile-comm.so"
SANITIZED_LD_LIBRARY_PATH=""
IFS=':' read -r -a LD_LIBRARY_PATH_PARTS <<< "${LD_LIBRARY_PATH:-}"
for path in "${LD_LIBRARY_PATH_PARTS[@]}"; do
    if [[ -z "${path}" || "${path}" == *devlib* ]]; then
        continue
    fi
    if [ -z "${SANITIZED_LD_LIBRARY_PATH}" ]; then
        SANITIZED_LD_LIBRARY_PATH="${path}"
    else
        SANITIZED_LD_LIBRARY_PATH="${SANITIZED_LD_LIBRARY_PATH}:${path}"
    fi
done
export LD_LIBRARY_PATH="${INSTALL_DIR}/lib:${TILEXR_INSTALL_LIB_DIR}:${ASCEND_DRIVER_PATH}/lib64/driver:${ASCEND_HOME_PATH}/${ARCH}-linux/lib64"
if [ -n "${SANITIZED_LD_LIBRARY_PATH}" ]; then
    export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:${SANITIZED_LD_LIBRARY_PATH}"
fi

bash "${SCRIPT_DIR}/check_runtime_deps.sh" "${TILEXR_COMM_LIB}"

TILE_COMM_DEPS=$(ldd "${TILEXR_COMM_LIB}" || true)
HAL_AVAILABLE=1
if echo "${TILE_COMM_DEPS}" | grep -E 'libascend_hal.so => not found' >/dev/null; then
    HAL_AVAILABLE=0
fi

"${INSTALL_DIR}/bin/test_tilexr_sdma_metadata"
"${INSTALL_DIR}/bin/test_tilexr_sdma_transport_disabled"
"${INSTALL_DIR}/bin/test_tilexr_sdma_header_compile"

if [ "${HAL_AVAILABLE}" -eq 1 ]; then
    "${INSTALL_DIR}/bin/test_tilexr_sdma_api_invalid"
else
    echo "Skip test_tilexr_sdma_api_invalid: libascend_hal.so not found"
fi

if [ "${HAL_AVAILABLE}" -eq 1 ] && command -v npu-smi >/dev/null 2>&1; then
    "${INSTALL_DIR}/bin/test_tilexr_sdma_disabled_comm"
elif [ "${HAL_AVAILABLE}" -eq 0 ]; then
    echo "Skip test_tilexr_sdma_disabled_comm: libascend_hal.so not found"
else
    echo "Skip test_tilexr_sdma_disabled_comm: npu-smi not found"
fi

echo "TileXR SDMA unit tests passed"
