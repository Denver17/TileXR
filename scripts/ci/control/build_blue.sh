#!/usr/bin/env bash
# 在控制侧配置构建、安装 runtime 与测试程序，并检查交付产物和安装头文件。
# SOURCE_DIR 与 ARTIFACT_DIR 由调用者指定；运行日志写入本轮 artifact 目录。

set -euo pipefail

if [[ "$#" -ne 2 ]]; then
    echo "Usage: $0 SOURCE_DIR ARTIFACT_DIR" >&2
    exit 2
fi

SOURCE_DIR="$1"
ARTIFACT_DIR="$2"

if [[ ! -d "${SOURCE_DIR}" || -L "${SOURCE_DIR}" || ! -f "${SOURCE_DIR}/CMakeLists.txt" ]]; then
    echo "ERROR: SOURCE_DIR must be a real TileXR source directory: ${SOURCE_DIR}" >&2
    exit 2
fi
if [[ -L "${ARTIFACT_DIR}" ]]; then
    echo "ERROR: ARTIFACT_DIR must not be a symbolic link: ${ARTIFACT_DIR}" >&2
    exit 2
fi

SOURCE_DIR="$(cd "${SOURCE_DIR}" && pwd -P)"
mkdir -p "${ARTIFACT_DIR}"
ARTIFACT_DIR="$(cd "${ARTIFACT_DIR}" && pwd -P)"
if [[ "${SOURCE_DIR}" == "/" || "${ARTIFACT_DIR}" == "/" || \
      "${SOURCE_DIR}" == "${ARTIFACT_DIR}" ]]; then
    echo "ERROR: unsafe source or artifact directory" >&2
    exit 2
fi
CASES_FILE="${ARTIFACT_DIR}/cases.tsv"
CASE_LOG_DIR="${ARTIFACT_DIR}/case-logs"
mkdir -p "${CASE_LOG_DIR}"
: > "${CASES_FILE}"

export TILEXR_CANN_HOME=/home/tilexr-ci/toolchains/cann/9.1.0
export ASCEND_HOME_PATH="${TILEXR_CANN_HOME}/cann"
if [[ ! -r /home/tilexr-ci/toolchains/cann/9.1.0/cann/set_env.sh ]]; then
    echo "ERROR: sealed CANN toolchain is missing: ${TILEXR_CANN_HOME}" >&2
    exit 1
fi
set +u
source /home/tilexr-ci/toolchains/cann/9.1.0/cann/set_env.sh
set -u
set -euo pipefail
export TILEXR_CANN_HOME=/home/tilexr-ci/toolchains/cann/9.1.0
export ASCEND_HOME_PATH="${TILEXR_CANN_HOME}/cann"
export ASCEND_TOOLKIT_HOME="${ASCEND_HOME_PATH}"
export ASCEND_OPP_PATH="${ASCEND_HOME_PATH}/opp"
export ARCH="$(uname -m)"
if [[ "${ARCH}" == "arm64" ]]; then
    export ARCH=aarch64
fi
export TILEXR_OS_ARCH="${ARCH}"
export TILEXR_SOC_NAME=Ascend910B3

REAL_ASCEND_DRIVER_PATH=/usr/local/Ascend/driver
TRUSTED_ENV_ROOT="${ARTIFACT_DIR}/trusted-environment"
/bin/rm -rf "${TRUSTED_ENV_ROOT}"
mkdir -p "${TRUSTED_ENV_ROOT}"
export ASCEND_DRIVER_PATH="${REAL_ASCEND_DRIVER_PATH}"
if [[ ! -r "${REAL_ASCEND_DRIVER_PATH}/kernel/inc" ]]; then
    DRIVER_HEADERS="${ASCEND_HOME_PATH}/${ARCH}-linux/include/driver"
    if [[ ! -d "${DRIVER_HEADERS}" ]]; then
        echo "ERROR: Ascend driver headers are unavailable" >&2
        exit 1
    fi
    DRIVER_SHIM="${TRUSTED_ENV_ROOT}/driver-shim"
    mkdir -p "${DRIVER_SHIM}/kernel" "${DRIVER_SHIM}/lib64"
    ln -s "${DRIVER_HEADERS}" "${DRIVER_SHIM}/kernel/inc"
    ln -s "${REAL_ASCEND_DRIVER_PATH}/lib64/driver" "${DRIVER_SHIM}/lib64/driver"
    if [[ -d "${REAL_ASCEND_DRIVER_PATH}/lib64/common" ]]; then
        ln -s "${REAL_ASCEND_DRIVER_PATH}/lib64/common" "${DRIVER_SHIM}/lib64/common"
    fi
    export ASCEND_DRIVER_PATH="${DRIVER_SHIM}"
fi
SOURCE_LIBRARY_PATH="${SOURCE_DIR}/install/lib64:${SOURCE_DIR}/install/lib"
for test_install in comm udma sdma ep memory; do
    SOURCE_LIBRARY_PATH="${SOURCE_LIBRARY_PATH}:${SOURCE_DIR}/tests/${test_install}/install/lib64:${SOURCE_DIR}/tests/${test_install}/install/lib"
done
export LD_LIBRARY_PATH="${SOURCE_LIBRARY_PATH}:${ASCEND_DRIVER_PATH}/lib64/driver:${ASCEND_DRIVER_PATH}/lib64/common:${ASCEND_DRIVER_PATH}/lib64:${ASCEND_HOME_PATH}/${ARCH}-linux/lib64:${LD_LIBRARY_PATH:-}"

# Define trusted helpers only after the sealed environment has been loaded.
run_case() {
    local name="$1"
    shift
    local started finished elapsed status result log_file
    local -a pipeline_status
    started="$(date +%s)"
    log_file="${CASE_LOG_DIR}/${name}.log"
    set +e
    "$@" 2>&1 | /usr/bin/tee "${log_file}"
    pipeline_status=("${PIPESTATUS[@]}")
    status="${pipeline_status[0]}"
    if [[ "${status}" -eq 0 && "${pipeline_status[1]}" -ne 0 ]]; then
        status="${pipeline_status[1]}"
    fi
    set -e
    finished="$(date +%s)"
    elapsed=$((finished - started))
    result="pass"
    if [[ "${status}" -ne 0 ]]; then
        result="fail"
    fi
    printf '%s\t%s\t%s\t%s\n' "${name}" "${result}" "${status}" "${elapsed}" >> "${CASES_FILE}"
    return "${status}"
}

run_logged_step() {
    local name="$1"
    shift
    local status
    local -a pipeline_status
    set +e
    "$@" 2>&1 | /usr/bin/tee "${ARTIFACT_DIR}/build-${name}.log"
    pipeline_status=("${PIPESTATUS[@]}")
    status="${pipeline_status[0]}"
    if [[ "${status}" -eq 0 && "${pipeline_status[1]}" -ne 0 ]]; then
        status="${pipeline_status[1]}"
    fi
    set -e
    return "${status}"
}

run_ctest() {
    (cd "$1" && ctest --output-on-failure)
}

require_executable() {
    local path="$1"
    if ! test -x "${path}"; then
        echo "ERROR: required executable is missing: ${path}" >&2
        return 1
    fi
}

require_regular_file() {
    local path="$1"
    if [[ -L "${path}" || ! -f "${path}" ]]; then
        echo "ERROR: required regular file is missing: ${path}" >&2
        return 1
    fi
}

installed_library() {
    local name="$1"
    local path
    for directory in lib64 lib; do
        path="${SOURCE_DIR}/install/${directory}/${name}"
        if [[ -f "${path}" && ! -L "${path}" ]]; then
            printf '%s\n' "${path}"
            return 0
        fi
    done
    echo "ERROR: required installed library is missing: ${name}" >&2
    return 1
}

validate_dynamic_output() {
    local label="$1"
    local path="$2"
    local expect_hal="$3"
    local ldd_report="${ARTIFACT_DIR}/ldd-${label}.txt"
    local readelf_report="${ARTIFACT_DIR}/readelf-${label}.txt"

    if ! /usr/bin/ldd "${path}" > "${ldd_report}" 2>&1; then
        echo "ERROR: ldd failed for ${path}" >&2
        return 1
    fi
    if grep -E '=>[[:space:]]*not found|[^[:space:]]+[[:space:]]+not found' \
        "${ldd_report}" >/dev/null; then
        echo "ERROR: unresolved dynamic dependency for ${path}" >&2
        return 1
    fi
    if grep -E '=>[^[:cntrl:]]*devlib' "${ldd_report}" >/dev/null; then
        echo "ERROR: runtime dependency resolved through CANN devlib for ${path}" >&2
        return 1
    fi
    if [[ "${expect_hal}" == "yes" ]]; then
        if ! grep -F 'libascend_hal.so' "${ldd_report}" >/dev/null; then
            echo "ERROR: libascend_hal.so is absent for ${path}" >&2
            return 1
        fi
        if grep -E 'libascend_hal\.so[^[:cntrl:]]*devlib' "${ldd_report}" >/dev/null; then
            echo "ERROR: libascend_hal.so resolved through CANN devlib for ${path}" >&2
            return 1
        fi
    fi

    /usr/bin/readelf -d "${path}" > "${readelf_report}"
    if grep -E '\((RPATH|RUNPATH)\)[^[:cntrl:]]*devlib' \
        "${readelf_report}" >/dev/null; then
        echo "ERROR: RPATH or RUNPATH contains CANN devlib for ${path}" >&2
        return 1
    fi
}

validate_cann_metadata_file() {
    local metadata="$1"
    local expected_package="$2"
    local package_line version_line
    if [[ -L "${metadata}" || ! -r "${metadata}" || ! -f "${metadata}" ]]; then
        echo "ERROR: required sealed CANN metadata is missing: ${metadata}" >&2
        return 1
    fi
    package_line="$(grep '^package_name=' "${metadata}" || true)"
    version_line="$(grep '^version=' "${metadata}" || true)"
    if [[ "${package_line}" != "package_name=${expected_package}" ]]; then
        echo "ERROR: unexpected package_name in ${metadata}" >&2
        return 1
    fi
    if [[ "${version_line}" != 'version=9.1.0' ]]; then
        echo "ERROR: expected exact CANN version=9.1.0 in ${metadata}" >&2
        return 1
    fi
}

capture_cann_metadata() {
    local output="${ARTIFACT_DIR}/version-cann.txt"
    local temporary
    local toolkit_metadata="${ASCEND_HOME_PATH}/${ARCH}-linux/ascend_toolkit_install.info"
    local ops_metadata="${ASCEND_HOME_PATH}/${ARCH}-linux/ascend_ops_install.info"
    validate_cann_metadata_file "${toolkit_metadata}" "Ascend-cann-toolkit"
    validate_cann_metadata_file "${ops_metadata}" "Ascend-cann-910b-ops"
    temporary="$(mktemp "${TRUSTED_ENV_ROOT}/cann-metadata.XXXXXX")" || return 1
    {
        printf '===== %s =====\n' "${toolkit_metadata}"
        /bin/cat "${toolkit_metadata}"
        printf '\n===== %s =====\n' "${ops_metadata}"
        /bin/cat "${ops_metadata}"
        printf '\n'
    } > "${temporary}"
    /bin/mv -f "${temporary}" "${output}"
}

capture_cann_metadata

BUILD_JOBS="$(nproc)"
rm -rf \
    "${SOURCE_DIR}/build-ci" \
    "${SOURCE_DIR}/install" \
    "${SOURCE_DIR}/build-sdma-tests" \
    "${SOURCE_DIR}/build_ep" \
    "${SOURCE_DIR}/tests/comm/build" \
    "${SOURCE_DIR}/tests/comm/install" \
    "${SOURCE_DIR}/tests/udma/build" \
    "${SOURCE_DIR}/tests/udma/install" \
    "${SOURCE_DIR}/tests/sdma/build" \
    "${SOURCE_DIR}/tests/sdma/install" \
    "${SOURCE_DIR}/tests/ep/build" \
    "${SOURCE_DIR}/tests/ep/install" \
    "${SOURCE_DIR}/tests/memory/build" \
    "${SOURCE_DIR}/tests/memory/install"

run_logged_step "top-level-configure" cmake \
    -S "${SOURCE_DIR}" -B "${SOURCE_DIR}/build-ci" \
    -DCMAKE_INSTALL_PREFIX="${SOURCE_DIR}/install" \
    -DTILEXR_BUILD_COLLECTIVES=ON \
    -DTILEXR_BUILD_EP=ON \
    -DTILEXR_BUILD_CHECKER=ON \
    -DTILEXR_BUILD_TESTS=ON \
    -DBUILD_TESTING=ON
run_logged_step "top-level-build" cmake \
    --build "${SOURCE_DIR}/build-ci" --target install -j"${BUILD_JOBS}"
run_case top-level-ctest run_ctest "${SOURCE_DIR}/build-ci"

run_logged_step "comm-build" bash "${SOURCE_DIR}/tests/comm/build.sh"
run_case comm-log "${SOURCE_DIR}/tests/comm/install/bin/test_tilexr_log"
run_case comm-spdlog-compile "${SOURCE_DIR}/tests/comm/install/bin/test_tilexr_log_spdlog_compile"

run_logged_step "udma-build" env BUILD_TILEXR_UDMA_DEMO=OFF \
    bash "${SOURCE_DIR}/tests/udma/build.sh"
run_case udma-transport-layout "${SOURCE_DIR}/tests/udma/install/bin/test_tilexr_udma_transport_layout"
run_case udma-registry "${SOURCE_DIR}/tests/udma/install/bin/test_tilexr_udma_registry"

run_logged_step "sdma-build" bash \
    "${SOURCE_DIR}/tests/sdma/build.sh" "${ASCEND_HOME_PATH}"
run_case sdma-metadata "${SOURCE_DIR}/tests/sdma/install/bin/test_tilexr_sdma_metadata"
run_case sdma-api-invalid "${SOURCE_DIR}/tests/sdma/install/bin/test_tilexr_sdma_api_invalid"
run_case sdma-transport-disabled "${SOURCE_DIR}/tests/sdma/install/bin/test_tilexr_sdma_transport_disabled"
run_case sdma-header-compile "${SOURCE_DIR}/tests/sdma/install/bin/test_tilexr_sdma_header_compile"

run_logged_step "ep-build" bash "${SOURCE_DIR}/tests/ep/build.sh" full
run_case ep-layout "${SOURCE_DIR}/tests/ep/install/bin/test_tilexr_ep_layout"
run_case ep-host-validation "${SOURCE_DIR}/tests/ep/install/bin/test_tilexr_ep_host_validation"

run_logged_step "memory-configure" cmake \
    -S "${SOURCE_DIR}/tests/memory" -B "${SOURCE_DIR}/tests/memory/build" \
    -DCMAKE_INSTALL_PREFIX="${SOURCE_DIR}/tests/memory/install" \
    -DBUILD_TILEXR_MEMORY_DEMO=ON \
    -DTILEXR_MEMORY_DEMO_SOC_TYPE=Ascend910B
run_logged_step "memory-build" cmake \
    --build "${SOURCE_DIR}/tests/memory/build" --target install -j"${BUILD_JOBS}"
# Dedicated suite builds reuse the root install prefix with optional components
# disabled. Restore the complete top-level install before validating outputs.
run_logged_step "top-level-reinstall" cmake --install "${SOURCE_DIR}/build-ci"

for required_binary in \
    "${SOURCE_DIR}/tests/udma/install/bin/test_tilexr_udma" \
    "${SOURCE_DIR}/tests/memory/install/bin/tilexr_memory_demo" \
    "${SOURCE_DIR}/tests/ep/install/bin/tilexr_ep_dispatch_demo" \
    "${SOURCE_DIR}/tests/sdma/install/bin/tilexr_sdma_demo" \
    "${SOURCE_DIR}/tests/sdma/install/bin/test_tilexr_sdma_disabled_comm" \
    "${SOURCE_DIR}/build-ci/tests/collectives/test_tilexr_collectives_correctness" \
    "${SOURCE_DIR}/build-ci/tests/collectives/tilexr_collective_perf"
do
    require_executable "${required_binary}"
done

for required_header in \
    tilexr_api.h tilexr_types.h comm_args.h tilexr_sync.h \
    tilexr_data_as_flag.h \
    tilexr_udma.h tilexr_udma_reg.h tilexr_udma_types.h \
    tilexr_sdma_config.h tilexr_sdma_types.h tilexr_sdma.h tilexr_sdma_compat.h \
    tilexr_ep.h tilexr_collectives.h
do
    require_regular_file "${SOURCE_DIR}/install/include/${required_header}"
done

TILEXR_LIBRARY="$(installed_library libtile-comm.so)"
EP_LIBRARY="$(installed_library libtilexr-ep.so)"
COLLECTIVES_LIBRARY="$(installed_library libtilexr-collectives.so)"
CHECKER_EXECUTABLE="${SOURCE_DIR}/install/bin/tilexr_checker"
CHECKER_CORE="$(installed_library libtilexr-checker-core.a)"
require_executable "${CHECKER_EXECUTABLE}"
require_regular_file "${CHECKER_CORE}"

validate_dynamic_output tile-comm "${TILEXR_LIBRARY}" yes
validate_dynamic_output tilexr-ep "${EP_LIBRARY}" yes
validate_dynamic_output tilexr-collectives "${COLLECTIVES_LIBRARY}" yes
validate_dynamic_output tilexr-checker "${CHECKER_EXECUTABLE}" no
