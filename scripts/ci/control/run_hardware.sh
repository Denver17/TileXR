#!/usr/bin/env bash

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
touch "${CASES_FILE}"

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

prepare_ci_run_root() {
    CI_RUN_ROOT="${SOURCE_DIR}/.ci-run"
    if [[ -L "${CI_RUN_ROOT}" || ( -e "${CI_RUN_ROOT}" && ! -d "${CI_RUN_ROOT}" ) ]]; then
        echo "ERROR: CI run root must be a real directory: ${CI_RUN_ROOT}" >&2
        return 1
    fi
    if [[ -d "${CI_RUN_ROOT}" ]] && ! /bin/rm -rf "${CI_RUN_ROOT}"; then
        echo "ERROR: could not clean CI run root: ${CI_RUN_ROOT}" >&2
        return 1
    fi
    if ! mkdir -p "${CI_RUN_ROOT}"; then
        echo "ERROR: could not create CI run root: ${CI_RUN_ROOT}" >&2
        return 1
    fi
    if ! CI_RUN_ROOT_REAL="$(cd "${CI_RUN_ROOT}" && pwd -P)"; then
        echo "ERROR: could not resolve CI run root: ${CI_RUN_ROOT}" >&2
        return 1
    fi
    if [[ "${CI_RUN_ROOT_REAL}" != "${CI_RUN_ROOT}" ]]; then
        echo "ERROR: CI run root contains a symbolic-link redirection" >&2
        return 1
    fi
}

run_in_dir() {
    local directory="$1"
    shift
    local directory_real
    case "${directory}" in
        "${CI_RUN_ROOT_REAL}"/*) ;;
        *)
            echo "ERROR: case directory is outside the CI run root: ${directory}" >&2
            return 1
            ;;
    esac
    if [[ -L "${directory}" || ( -e "${directory}" && ! -d "${directory}" ) ]]; then
        echo "ERROR: case path must be a real directory: ${directory}" >&2
        return 1
    fi
    if ! mkdir -p "${directory}"; then
        echo "ERROR: could not create case directory: ${directory}" >&2
        return 1
    fi
    if [[ -L "${directory}" || ! -d "${directory}" ]]; then
        echo "ERROR: case path is not a real directory: ${directory}" >&2
        return 1
    fi
    if ! directory_real="$(cd "${directory}" && pwd -P)"; then
        echo "ERROR: could not resolve case directory: ${directory}" >&2
        return 1
    fi
    case "${directory_real}" in
        "${CI_RUN_ROOT_REAL}"/*) ;;
        *)
            echo "ERROR: resolved case directory escaped the CI run root" >&2
            return 1
            ;;
    esac
    if [[ "${directory_real}" != "${directory}" ]]; then
        echo "ERROR: case directory contains a symbolic-link redirection" >&2
        return 1
    fi
    (
        cd "${directory_real}" || exit 1
        exec "$@"
    )
}

run_required_no_skip() {
    local marker="$1"
    shift
    local output status
    output="$(mktemp "${TRUSTED_ENV_ROOT}/required-case.XXXXXX")" || return 1
    if "$@" > "${output}" 2>&1; then
        status=0
    else
        status=$?
    fi
    if ! /bin/cat "${output}"; then
        status=1
    fi
    if grep -F "${marker}" "${output}" >/dev/null; then
        echo "ERROR: required hardware case reported a skip: ${marker}" >&2
        status=1
    fi
    /bin/rm -f "${output}"
    return "${status}"
}

parse_top_level_health() {
    awk '
        /^[[:space:]]*Health[[:space:]]*:/ {
            value = $0
            sub(/^[[:space:]]*Health[[:space:]]*:[[:space:]]*/, "", value)
            sub(/[[:space:]]*$/, "", value)
            print value
            exit
        }
    '
}

check_device_health() {
    local device="$1"
    local health output status
    set +e
    output="$(/usr/bin/timeout --signal=TERM --kill-after=2 10 \
        npu-smi info -t health -i "${device}" 2>&1)"
    status=$?
    set -e
    printf '%s\n' "${output}"
    if [[ "${status}" -ne 0 ]]; then
        return "${status}"
    fi
    health="$(printf '%s\n' "${output}" | parse_top_level_health)"
    if [[ "${health}" != "OK" ]]; then
        echo "ERROR: device ${device} reported top-level Health: ${health:-missing}" >&2
        return 1
    fi
}

capture_npu_evidence() {
    local destination="$1"
    shift
    local output status
    output="$(mktemp "${TRUSTED_ENV_ROOT}/npu-evidence.XXXXXX")" || return 1
    if /usr/bin/timeout --signal=TERM --kill-after=2 10 "$@" > "${output}" 2>&1; then
        if [[ -L "${destination}" || -f "${destination}" ]]; then
            /bin/rm -f "${destination}"
        elif [[ -e "${destination}" ]]; then
            echo "ERROR: evidence destination is not a regular file: ${destination}" >&2
            /bin/rm -f "${output}"
            return 1
        fi
        if /bin/mv -f "${output}" "${destination}"; then
            return 0
        else
            status=$?
        fi
    else
        status=$?
    fi
    echo "ERROR: failed to capture ${destination##*/}" >&2
    /bin/cat "${output}" >&2 || true
    /bin/rm -f "${output}"
    return "${status}"
}

finalize_hardware() {
    local primary_status=$?
    local evidence_status
    trap - EXIT
    set +e
    capture_npu_evidence "${ARTIFACT_DIR}/npu-state-after.txt" npu-smi info
    evidence_status=$?
    set -e
    if [[ "${primary_status}" -eq 0 && "${evidence_status}" -ne 0 ]]; then
        primary_status="${evidence_status}"
    fi
    exit "${primary_status}"
}

case "${PR_NUMBER:-}" in
    ''|*[!0-9]*)
        echo "ERROR: PR_NUMBER must be a positive integer" >&2
        exit 2
        ;;
esac
if [[ "${PR_NUMBER}" -le 0 ]]; then
    echo "ERROR: PR_NUMBER must be a positive integer" >&2
    exit 2
fi

if [[ ! -x /usr/bin/timeout ]]; then
    echo "ERROR: /usr/bin/timeout is required" >&2
    exit 1
fi
if ! command -v mpirun >/dev/null 2>&1; then
    echo "ERROR: mpirun is required" >&2
    exit 1
fi
if ! command -v npu-smi >/dev/null 2>&1; then
    echo "ERROR: npu-smi is required" >&2
    exit 1
fi

export TILEXR_TEST_DEVICES=0,1,2,3,4,5,6,7
export TILEXR_AVAILABLE_NPUS=8
export TILEXR_SKIP_IF_INSUFFICIENT_NPUS=0
export TILEXR_COLLECTIVES_RUN_TIMEOUT_SEC=600
export TILEXR_COMM_ID="127.0.0.1:$((20000 + PR_NUMBER % 20000))"

trap finalize_hardware EXIT
capture_npu_evidence "${ARTIFACT_DIR}/npu-state-before.txt" npu-smi info
capture_npu_evidence "${ARTIFACT_DIR}/npu-topology.txt" npu-smi info -t topo
prepare_ci_run_root

for device in 0 1 2 3 4 5 6 7; do
    run_case "npu-health-${device}" check_device_health "${device}"
done

run_case udma-single-rank-fallback \
    /usr/bin/timeout --signal=TERM --kill-after=10 600 \
    env RANK=0 RANK_SIZE=1 \
        "${SOURCE_DIR}/tests/udma/install/bin/test_tilexr_udma"
run_case udma-eight-rank-fallback \
    /usr/bin/timeout --signal=TERM --kill-after=10 600 \
    mpirun -n 8 "${SOURCE_DIR}/tests/udma/install/bin/test_tilexr_udma"

run_case sdma-disabled-comm run_required_no_skip \
    "Skip test_tilexr_sdma_disabled_comm:" \
    /usr/bin/timeout --signal=TERM --kill-after=10 600 \
    env -u TILEXR_ENABLE_SDMA ASCEND_RT_VISIBLE_DEVICES=0 \
        "${SOURCE_DIR}/tests/sdma/install/bin/test_tilexr_sdma_disabled_comm"

run_case memory-eight-rank \
    /usr/bin/timeout --signal=TERM --kill-after=10 600 \
    bash "${SOURCE_DIR}/tests/memory/demo/run_tilexr_memory_demo.sh" 8 1024 8 0

for op in allgather allreduce reducescatter; do
    run_case "collectives-correctness-${op}" run_in_dir \
        "${SOURCE_DIR}/.ci-run/collectives/correctness-${op}" \
        /usr/bin/timeout --signal=TERM --kill-after=10 600 \
        bash "${SOURCE_DIR}/tests/collectives/run_collectives_correctness.sh" \
            8 1024 0 "${SOURCE_DIR}/build-ci/tests/collectives" "${op}"
done
run_case collectives-correctness-broadcast-root-0 run_in_dir \
    "${SOURCE_DIR}/.ci-run/collectives/correctness-broadcast-root-0" \
    /usr/bin/timeout --signal=TERM --kill-after=10 600 \
    bash "${SOURCE_DIR}/tests/collectives/run_collectives_correctness.sh" \
        8 1024 0 "${SOURCE_DIR}/build-ci/tests/collectives" broadcast --root 0
run_case collectives-correctness-broadcast-root-7 run_in_dir \
    "${SOURCE_DIR}/.ci-run/collectives/correctness-broadcast-root-7" \
    /usr/bin/timeout --signal=TERM --kill-after=10 600 \
    bash "${SOURCE_DIR}/tests/collectives/run_collectives_correctness.sh" \
        8 1024 0 "${SOURCE_DIR}/build-ci/tests/collectives" broadcast --root 7

for op in allgather allreduce reducescatter broadcast; do
    run_case "collectives-perf-${op}" run_in_dir \
        "${SOURCE_DIR}/.ci-run/collectives/perf-${op}" \
        /usr/bin/timeout --signal=TERM --kill-after=10 600 \
        bash "${SOURCE_DIR}/tools/collectives/run_collective_perf.sh" \
            8 0 "${SOURCE_DIR}/build-ci/tests/collectives" \
            --op "${op}" --min-bytes 4 --max-bytes 1048576 --step-factor 8 \
            --iters 3 --warmup-iters 1 --datatype int32 --check 1
done

for device in 0 1 2 3 4 5 6 7; do
    run_case "sdma-device-${device}" \
        /usr/bin/timeout --signal=TERM --kill-after=10 600 \
        bash "${SOURCE_DIR}/tests/sdma/demo/run_tilexr_sdma_demo.sh" \
            "${ASCEND_HOME_PATH}" "${device}" 64 4096 1048576
done

echo "UDMA data plane: out of scope on 910B3"
echo "EP Dispatch/Combine Memory: out of scope on 910B3"
echo "Multi-host validation: out of scope for this gate"
echo "vLLM model inference: out of scope for this gate"
echo "Performance regression thresholds: out of scope for this gate"
