#!/usr/bin/env bash
#
# Copyright (c) 2026 TileXR Project
#
# Two-rank runner for the private TileXR direct CCU smoke probe.
# Default execution is safe and does not touch ACL/NPU runtime.

set -euo pipefail

if [ "${TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE:-0}" != "1" ]; then
    echo "tilexr_ccu_direct_smoke_runner skipped: set TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE=1 to run hardware smoke"
    exit 0
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

ASCEND_HOME_PATH="${ASCEND_HOME_PATH:-/usr/local/Ascend/ascend-toolkit/latest}"
ARCH="${ARCH:-$(uname -m)}"
if [ "${ARCH}" = "arm64" ]; then
    ARCH="aarch64"
fi
ASCEND_DRIVER_PATH="${ASCEND_DRIVER_PATH:-/usr/local/Ascend/driver}"
export ASCEND_HOME_PATH ARCH ASCEND_DRIVER_PATH
export TILEXR_CCU_DIRECT_INSTALL_ORDER="${TILEXR_CCU_DIRECT_INSTALL_ORDER:-lower_layer_first}"

work_dir="${TILEXR_CCU_SMOKE_WORK_DIR:-${repo_root}/build/ccu_direct_smoke}"
mkdir -p "${work_dir}"

endpoint_fields=(
    EID
    TPN
    DOORBELL_VA
    DOORBELL_TOKEN_ID
    DOORBELL_TOKEN_VALUE
    SQ_DEPTH
)

resource_window_token_fields=(
    TOKEN_ID
    RAW_TOKEN_ID
    TOKEN_VALUE
)

parse_int()
{
    local value="$1"
    local fallback="$2"
    if [ -z "${value}" ]; then
        echo "${fallback}"
        return
    fi
    printf "%d" "${value}" 2>/dev/null || printf "%d" "${fallback}"
}

default_sync_instruction_count()
{
    local sync_resource_count="$1"
    local barrier_mode="${TILEXR_CCU_DIRECT_BARRIER_MODE:-}"
    local hcomm_style_task1_prelude_count=5
    case "${barrier_mode}" in
        sync_cke|sync_cke_set_wait)
            echo $((sync_resource_count * 2 + 1))
            ;;
        sync_cke_post_only)
            echo $((sync_resource_count + 1))
            ;;
        local_cke_post_only)
            echo "${sync_resource_count}"
            ;;
        sync_xn_post_only)
            echo $((hcomm_style_task1_prelude_count + sync_resource_count))
            ;;
        sync_xn_load_post_only)
            echo $((hcomm_style_task1_prelude_count + sync_resource_count * 2))
            ;;
        *)
            echo $((hcomm_style_task1_prelude_count + sync_resource_count * 2))
            ;;
    esac
}

apply_p2p_ccu_copy_defaults()
{
    if [ "${TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY:-0}" != "1" ]; then
        return
    fi

    export TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT="${TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT:-1}"
    export TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY_ACTIVE_RANK="${TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY_ACTIVE_RANK:-0}"
    export TILEXR_CCU_PROBE_MISSION_START="${TILEXR_CCU_PROBE_MISSION_START:-6}"
    export TILEXR_CCU_PROBE_INSTRUCTION_START="${TILEXR_CCU_PROBE_INSTRUCTION_START:-475}"
    export TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START="${TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START:-489}"
    export TILEXR_CCU_PROBE_SQE_ARG_COUNT="${TILEXR_CCU_PROBE_SQE_ARG_COUNT:-13}"
    export TILEXR_CCU_PROBE_SYNC_INSTRUCTION_COUNT="${TILEXR_CCU_PROBE_SYNC_INSTRUCTION_COUNT:-143}"
    export TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW="${TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW:-full_repository}"
    export TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE="${TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE:-instruction_bytes}"
    export TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE="${TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE:-acl}"
    export TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE="${TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE:-ra_ctx}"
    export TILEXR_CCU_PROBE_RANK0_XN_START="${TILEXR_CCU_PROBE_RANK0_XN_START:-1961}"
    export TILEXR_CCU_PROBE_RANK1_XN_START="${TILEXR_CCU_PROBE_RANK1_XN_START:-1961}"
    export TILEXR_CCU_PROBE_GSA_START="${TILEXR_CCU_PROBE_GSA_START:-510}"
    export TILEXR_CCU_PROBE_RANK0_REMOTE_XN_START="${TILEXR_CCU_PROBE_RANK0_REMOTE_XN_START:-2361}"
    export TILEXR_CCU_PROBE_RANK1_REMOTE_XN_START="${TILEXR_CCU_PROBE_RANK1_REMOTE_XN_START:-2361}"
    export TILEXR_CCU_PROBE_REMOTE_XN_COUNT="${TILEXR_CCU_PROBE_REMOTE_XN_COUNT:-8}"
    export TILEXR_CCU_PROBE_RANK0_LOCAL_WAIT_CKE_START="${TILEXR_CCU_PROBE_RANK0_LOCAL_WAIT_CKE_START:-332}"
    export TILEXR_CCU_PROBE_RANK1_LOCAL_WAIT_CKE_START="${TILEXR_CCU_PROBE_RANK1_LOCAL_WAIT_CKE_START:-332}"
    export TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_COUNT="${TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_COUNT:-8}"
    export TILEXR_CCU_PROBE_RANK0_REMOTE_NOTIFY_CKE_START="${TILEXR_CCU_PROBE_RANK0_REMOTE_NOTIFY_CKE_START:-364}"
    export TILEXR_CCU_PROBE_RANK1_REMOTE_NOTIFY_CKE_START="${TILEXR_CCU_PROBE_RANK1_REMOTE_NOTIFY_CKE_START:-364}"
    export TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_COUNT="${TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_COUNT:-8}"
    export TILEXR_CCU_PROBE_CHANNEL_START="${TILEXR_CCU_PROBE_CHANNEL_START:-2}"
    export TILEXR_CCU_DIRECT_BARRIER_MODE="${TILEXR_CCU_DIRECT_BARRIER_MODE:-sync_cke}"
    export TILEXR_CCU_DIRECT_LOWER_LAYER_WQE_MODE="${TILEXR_CCU_DIRECT_LOWER_LAYER_WQE_MODE:-hcomm_cap}"
}

apply_p2p_ccu_copy_defaults

if [ "${TILEXR_CCU_DIRECT_SMOKE_DRY_RUN:-0}" = "1" ]; then
    echo "tilexr_ccu_direct_smoke_runner dryRun=1 workDir=${work_dir}"
    for diagnostic_var in \
        TILEXR_CCU_DIRECT_BARRIER_MODE \
        TILEXR_CCU_DIRECT_LOWER_LAYER_WQE_MODE \
        TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW \
        TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE \
        TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE \
        TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE \
        TILEXR_CCU_DIRECT_INSTALL_ORDER \
        TILEXR_CCU_PROBE_SQE_ARG_COUNT \
        TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START; do
        diagnostic_value="${!diagnostic_var:-}"
        if [ "${diagnostic_value}" != "" ]; then
            echo "dryRun ${diagnostic_var}=${diagnostic_value}"
        fi
    done
    sqe_arg_count="$(parse_int "${TILEXR_CCU_PROBE_SQE_ARG_COUNT:-13}" 13)"
    sync_resource_count="$(parse_int "${TILEXR_CCU_PROBE_SYNC_RESOURCE_COUNT:-1}" 1)"
    default_sync_instruction_count_value="$(default_sync_instruction_count "${sync_resource_count}")"
    sync_instruction_count="$(parse_int \
        "${TILEXR_CCU_PROBE_SYNC_INSTRUCTION_COUNT:-${default_sync_instruction_count_value}}" \
        "${default_sync_instruction_count_value}")"
    repository_start="$(parse_int "${TILEXR_CCU_PROBE_INSTRUCTION_START:-1}" 1)"
    mission_instruction_start="$(parse_int "${TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START:-0}" 0)"
    if [ "${mission_instruction_start}" -eq 0 ]; then
        mission_instruction_start="${repository_start}"
    fi
    repository_prefix_count=$((mission_instruction_start - repository_start))
    if [ "${repository_prefix_count}" -lt 0 ]; then
        repository_prefix_count=0
    fi
    mission_instruction_count=$((sqe_arg_count + sync_instruction_count))
    repository_count=$((repository_prefix_count + mission_instruction_count))
    task0_start="${mission_instruction_start}"
    task0_count="${sqe_arg_count}"
    task1_start=$((mission_instruction_start + sqe_arg_count))
    task1_count="${sync_instruction_count}"
    if [ "${TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW:-}" = "full_repository" ] ||
        [ "${TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW:-}" = "full" ] ||
        [ "${TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW:-}" = "1" ]; then
        install_start="${repository_start}"
        install_count="${repository_count}"
    else
        install_start="${mission_instruction_start}"
        install_count="${mission_instruction_count}"
    fi
    instruction_data_len=$((install_count * 32))
    if [ "${TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE:-}" = "descriptor_bytes" ] ||
        [ "${TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE:-}" = "descriptor" ] ||
        [ "${TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE:-}" = "1" ]; then
        set_instruction_data_len=24
    else
        set_instruction_data_len="${instruction_data_len}"
    fi
    echo "dryRun derived repositoryStartId=${repository_start} repositoryCount=${repository_count} missionInstructionStartId=${mission_instruction_start} missionInstructionCount=${mission_instruction_count}"
    echo "dryRun derived task0.instStartId=${task0_start} task0.instCnt=${task0_count}"
    echo "dryRun derived task1.instStartId=${task1_start} task1.instCnt=${task1_count}"
    echo "dryRun derived SET_INSTRUCTION offsetStartIdx=${install_start} dataLen=${set_instruction_data_len} instructionBytes=${instruction_data_len}"
    for endpoint_field in "${endpoint_fields[@]}"; do
        endpoint_var="TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_${endpoint_field}"
        common_endpoint_value="${!endpoint_var:-}"
        rank0_endpoint_var="TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_${endpoint_field}_RANK0"
        rank0_endpoint_value="${!rank0_endpoint_var:-${common_endpoint_value}}"
        rank1_endpoint_var="TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_${endpoint_field}_RANK1"
        rank1_endpoint_value="${!rank1_endpoint_var:-${common_endpoint_value}}"
        if [ "${rank0_endpoint_value}" != "" ]; then
            echo "dryRun rank0 TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_${endpoint_field}=${rank0_endpoint_value}"
        fi
        if [ "${rank1_endpoint_value}" != "" ]; then
            echo "dryRun rank1 TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_${endpoint_field}=${rank1_endpoint_value}"
        fi
    done
    for token_field in "${resource_window_token_fields[@]}"; do
        token_var="TILEXR_CCU_DIRECT_RESOURCE_WINDOW_${token_field}"
        common_token_value="${!token_var:-}"
        rank0_token_var="TILEXR_CCU_DIRECT_RESOURCE_WINDOW_${token_field}_RANK0"
        rank0_token_value="${!rank0_token_var:-${common_token_value}}"
        rank1_token_var="TILEXR_CCU_DIRECT_RESOURCE_WINDOW_${token_field}_RANK1"
        rank1_token_value="${!rank1_token_var:-${common_token_value}}"
        if [ "${rank0_token_value}" != "" ]; then
            echo "dryRun rank0 TILEXR_CCU_DIRECT_RESOURCE_WINDOW_${token_field}=${rank0_token_value}"
        fi
        if [ "${rank1_token_value}" != "" ]; then
            echo "dryRun rank1 TILEXR_CCU_DIRECT_RESOURCE_WINDOW_${token_field}=${rank1_token_value}"
        fi
    done
    exit 0
fi

tile_comm_lib="${TILEXR_TILE_COMM_LIB:-}"
if [ -z "${tile_comm_lib}" ]; then
    for candidate in \
        "${repo_root}/build/src/comm/libtile-comm.so" \
        "${repo_root}/install/lib64/libtile-comm.so" \
        "${repo_root}/install/lib/libtile-comm.so" \
        "${repo_root}/install_direct_ccu_guard/lib64/libtile-comm.so"; do
        if [ -f "${candidate}" ]; then
            tile_comm_lib="${candidate}"
            break
        fi
    done
fi
if [ -z "${tile_comm_lib}" ] || [ ! -f "${tile_comm_lib}" ]; then
    echo "ERROR: libtile-comm.so not found; build tile-comm first or set TILEXR_TILE_COMM_LIB" >&2
    exit 2
fi
tile_comm_dir="$(cd "$(dirname "${tile_comm_lib}")" && pwd)"

cann_root="${ASCEND_HOME_PATH}/${ARCH}-linux"
cann_lib_dir="${cann_root}/lib64"
driver_lib_dir="${ASCEND_DRIVER_PATH}/lib64/driver"
probe_bin="${work_dir}/ccu_tilexr_direct_smoke_probe"

c++ -std=c++14 \
    -DTILEXR_CCU_TESTING=1 \
    -I "${repo_root}/src/include" \
    -I "${repo_root}/src/comm" \
    -I "${cann_root}/pkg_inc" \
    -I "${cann_root}/pkg_inc/runtime" \
    -I "${cann_root}/include" \
    "${repo_root}/tests/ccu/ccu_tilexr_direct_smoke_probe.cpp" \
    -L "${tile_comm_dir}" \
    -L "${cann_lib_dir}" \
    -L "${driver_lib_dir}" \
    -Wl,-rpath-link,"${tile_comm_dir}" \
    -Wl,-rpath-link,"${cann_lib_dir}" \
    -Wl,-rpath-link,"${driver_lib_dir}" \
    -ltile-comm -lascendcl -lruntime -ldl -pthread \
    -o "${probe_bin}"

devices="${TILEXR_CCU_SMOKE_DEVICES:-${TILEXR_TEST_DEVICES:-0,1}}"

if command -v npu-smi >/dev/null 2>&1; then
    npu_smi_rc=0
    timeout "${TILEXR_CCU_SMOKE_NPU_SMI_TIMEOUT:-20}s" npu-smi info > "${work_dir}/npu-smi.log" 2>&1 || npu_smi_rc=$?
    if [ "${npu_smi_rc}" -ne 0 ]; then
        echo "ERROR: npu-smi info did not complete; refusing to run ACL/CCU smoke" >&2
        echo "npu-smi rc=${npu_smi_rc}" >&2
        echo "npu-smi log: ${work_dir}/npu-smi.log" >&2
        exit 3
    fi
    if [ "${TILEXR_CCU_SMOKE_ALLOW_BUSY_NPU:-0}" != "1" ]; then
        busy_rc=0
        npu_guard_args=(
            --log "${work_dir}/npu-smi.log"
            --devices "${devices}"
        )
        if [ "${TILEXR_CCU_SMOKE_ALLOW_UNHEALTHY_NPU:-0}" = "1" ]; then
            npu_guard_args+=(--allow-unhealthy)
        fi
        python3 "${repo_root}/tests/ccu/ccu_npu_smi_busy_guard.py" \
            "${npu_guard_args[@]}" > "${work_dir}/npu-smi-busy.log" 2>&1 || busy_rc=$?
        if [ "${busy_rc}" -ne 0 ]; then
            echo "ERROR: selected NPU device is busy or unhealthy; refusing to run ACL/CCU smoke" >&2
            cat "${work_dir}/npu-smi-busy.log" >&2
            echo "npu-smi log: ${work_dir}/npu-smi.log" >&2
            echo "set TILEXR_CCU_SMOKE_ALLOW_UNHEALTHY_NPU=1 to allow Alarm health while still rejecting busy devices" >&2
            echo "set TILEXR_CCU_SMOKE_ALLOW_BUSY_NPU=1 only for an explicitly approved short test that may use busy devices" >&2
            exit 3
        fi
    fi
elif [ "${TILEXR_CCU_SMOKE_REQUIRE_NPU_SMI:-0}" = "1" ]; then
    echo "ERROR: npu-smi not found; refusing to run ACL/CCU smoke" >&2
    exit 3
fi

comm_port="${TILEXR_CCU_SMOKE_PORT:-$((30000 + (RANDOM % 20000)))}"
comm_id="${TILEXR_COMM_ID:-127.0.0.1:${comm_port}}"
comm_domain="${TILEXR_CCU_PROBE_COMM_DOMAIN:-0}"
timeout_s="${TILEXR_CCU_SMOKE_TIMEOUT:-180}"
ready_dir="${work_dir}/submit_ready_${comm_port}"
done_dir="${work_dir}/submit_done_${comm_port}"
rank0_log="${work_dir}/ccu_rank0.log"
rank1_log="${work_dir}/ccu_rank1.log"
rm -rf "${ready_dir}" "${done_dir}"
mkdir -p "${ready_dir}" "${done_dir}"
rm -f "${rank0_log}" "${rank1_log}"

common_env=(
    "LD_LIBRARY_PATH=${tile_comm_dir}:${cann_lib_dir}:${driver_lib_dir}:${LD_LIBRARY_PATH:-}"
    "TILEXR_COMM_ID=${comm_id}"
    "TILEXR_TEST_DEVICES=${devices}"
    "TILEXR_CCU_DIRECT_SMOKE_ENABLE=1"
    "TILEXR_CCU_DIRECT_SMOKE_READY_DIR=${ready_dir}"
    "TILEXR_CCU_DIRECT_SMOKE_DONE_DIR=${done_dir}"
    "TILEXR_CCU_DIRECT_SMOKE_FAST_EXIT_ON_PREPARE_FAILURE=${TILEXR_CCU_DIRECT_SMOKE_FAST_EXIT_ON_PREPARE_FAILURE:-1}"
    "TILEXR_CCU_PROBE_RANK_SIZE=2"
    "TILEXR_CCU_PROBE_COMM_DOMAIN=${comm_domain}"
)
if [ "${TILEXR_CCU_DIRECT_SMOKE_SUBMIT:-0}" = "1" ]; then
    common_env+=("TILEXR_CCU_DIRECT_SMOKE_SUBMIT=1")
fi
if [ "${TILEXR_CCU_DIRECT_SMOKE_THREAD_MODE:-0}" = "1" ]; then
    common_env+=("TILEXR_CCU_DIRECT_SMOKE_THREAD_MODE=1")
fi
if [ "${TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT:-0}" = "1" ]; then
    common_env+=("TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT=1")
fi
if [ "${TILEXR_CCU_DIRECT_BARRIER_MODE:-}" != "" ]; then
    common_env+=("TILEXR_CCU_DIRECT_BARRIER_MODE=${TILEXR_CCU_DIRECT_BARRIER_MODE}")
fi
if [ "${TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW:-}" != "" ]; then
    common_env+=("TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW=${TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW}")
fi
if [ "${TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE:-}" != "" ]; then
    common_env+=("TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE=${TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE}")
fi
if [ "${TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE:-}" != "" ]; then
    common_env+=("TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE=${TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE}")
fi
if [ "${TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE:-}" != "" ]; then
    common_env+=("TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE=${TILEXR_CCU_DIRECT_RESOURCE_WINDOW_REGISTRATION_MODE}")
fi
if [ "${TILEXR_CCU_DIRECT_INSTALL_ORDER:-}" != "" ]; then
    common_env+=("TILEXR_CCU_DIRECT_INSTALL_ORDER=${TILEXR_CCU_DIRECT_INSTALL_ORDER}")
fi
if [ "${TILEXR_CCU_DIRECT_INSTALL_DIE_ID:-}" != "" ]; then
    common_env+=("TILEXR_CCU_DIRECT_INSTALL_DIE_ID=${TILEXR_CCU_DIRECT_INSTALL_DIE_ID}")
fi
if [ "${TILEXR_CCU_PROBE_MISSION_START:-}" != "" ]; then
    common_env+=("TILEXR_CCU_PROBE_MISSION_START=${TILEXR_CCU_PROBE_MISSION_START}")
fi
if [ "${TILEXR_CCU_PROBE_INSTRUCTION_START:-}" != "" ]; then
    common_env+=("TILEXR_CCU_PROBE_INSTRUCTION_START=${TILEXR_CCU_PROBE_INSTRUCTION_START}")
fi
if [ "${TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START:-}" != "" ]; then
    common_env+=("TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START=${TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START}")
fi
if [ "${TILEXR_CCU_PROBE_SQE_ARG_COUNT:-}" != "" ]; then
    common_env+=("TILEXR_CCU_PROBE_SQE_ARG_COUNT=${TILEXR_CCU_PROBE_SQE_ARG_COUNT}")
fi
if [ "${TILEXR_CCU_PROBE_SYNC_INSTRUCTION_COUNT:-}" != "" ]; then
    common_env+=("TILEXR_CCU_PROBE_SYNC_INSTRUCTION_COUNT=${TILEXR_CCU_PROBE_SYNC_INSTRUCTION_COUNT}")
fi
if [ "${TILEXR_CCU_PROBE_SYNC_RESOURCE_COUNT:-}" != "" ]; then
    common_env+=("TILEXR_CCU_PROBE_SYNC_RESOURCE_COUNT=${TILEXR_CCU_PROBE_SYNC_RESOURCE_COUNT}")
fi
if [ "${TILEXR_CCU_PROBE_BINDINGS_PER_RESOURCE:-}" != "" ]; then
    common_env+=("TILEXR_CCU_PROBE_BINDINGS_PER_RESOURCE=${TILEXR_CCU_PROBE_BINDINGS_PER_RESOURCE}")
fi
if [ "${TILEXR_CCU_PROBE_CKE_START:-}" != "" ]; then
    common_env+=("TILEXR_CCU_PROBE_CKE_START=${TILEXR_CCU_PROBE_CKE_START}")
fi
if [ "${TILEXR_CCU_PROBE_GSA_START:-}" != "" ]; then
    common_env+=("TILEXR_CCU_PROBE_GSA_START=${TILEXR_CCU_PROBE_GSA_START}")
fi
if [ "${TILEXR_CCU_PROBE_CHANNEL_START:-}" != "" ]; then
    common_env+=("TILEXR_CCU_PROBE_CHANNEL_START=${TILEXR_CCU_PROBE_CHANNEL_START}")
fi
if [ "${TILEXR_CCU_DIRECT_SMOKE_DELAY_RANK:-}" != "" ]; then
    common_env+=("TILEXR_CCU_DIRECT_SMOKE_DELAY_RANK=${TILEXR_CCU_DIRECT_SMOKE_DELAY_RANK}")
fi
if [ "${TILEXR_CCU_DIRECT_SMOKE_PRE_SUBMIT_DELAY_MS:-}" != "" ]; then
    common_env+=("TILEXR_CCU_DIRECT_SMOKE_PRE_SUBMIT_DELAY_MS=${TILEXR_CCU_DIRECT_SMOKE_PRE_SUBMIT_DELAY_MS}")
fi
if [ "${TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY:-}" != "" ]; then
    common_env+=("TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY=${TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY}")
fi
if [ "${TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY_BYTES:-}" != "" ]; then
    common_env+=("TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY_BYTES=${TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY_BYTES}")
fi
if [ "${TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_START:-}" != "" ]; then
    common_env+=("TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_START=${TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_START}")
fi
if [ "${TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_COUNT:-}" != "" ]; then
    common_env+=("TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_COUNT=${TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_COUNT}")
fi
if [ "${TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_START:-}" != "" ]; then
    common_env+=("TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_START=${TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_START}")
fi
if [ "${TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_COUNT:-}" != "" ]; then
    common_env+=("TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_COUNT=${TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_COUNT}")
fi
for endpoint_field in "${endpoint_fields[@]}"; do
    endpoint_var="TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_${endpoint_field}"
    endpoint_value="${!endpoint_var:-}"
    if [ "${endpoint_value}" != "" ]; then
        common_env+=("${endpoint_var}=${endpoint_value}")
    fi
done
for token_field in "${resource_window_token_fields[@]}"; do
    token_var="TILEXR_CCU_DIRECT_RESOURCE_WINDOW_${token_field}"
    token_value="${!token_var:-}"
    if [ "${token_value}" != "" ]; then
        common_env+=("${token_var}=${token_value}")
    fi
    rank0_token_var="TILEXR_CCU_DIRECT_RESOURCE_WINDOW_${token_field}_RANK0"
    rank0_token_value="${!rank0_token_var:-}"
    if [ "${rank0_token_value}" != "" ]; then
        common_env+=("${rank0_token_var}=${rank0_token_value}")
    fi
    rank1_token_var="TILEXR_CCU_DIRECT_RESOURCE_WINDOW_${token_field}_RANK1"
    rank1_token_value="${!rank1_token_var:-}"
    if [ "${rank1_token_value}" != "" ]; then
        common_env+=("${rank1_token_var}=${rank1_token_value}")
    fi
done

rank0_env=()
rank1_env=()
if [ "${TILEXR_CCU_PROBE_RANK0_XN_START:-}" != "" ]; then
    rank0_env+=("TILEXR_CCU_PROBE_XN_START=${TILEXR_CCU_PROBE_RANK0_XN_START}")
fi
if [ "${TILEXR_CCU_PROBE_RANK1_XN_START:-}" != "" ]; then
    rank1_env+=("TILEXR_CCU_PROBE_XN_START=${TILEXR_CCU_PROBE_RANK1_XN_START}")
fi
if [ "${TILEXR_CCU_PROBE_RANK0_REMOTE_XN_START:-}" != "" ]; then
    rank0_env+=("TILEXR_CCU_PROBE_REMOTE_XN_START=${TILEXR_CCU_PROBE_RANK0_REMOTE_XN_START}")
fi
if [ "${TILEXR_CCU_PROBE_RANK1_REMOTE_XN_START:-}" != "" ]; then
    rank1_env+=("TILEXR_CCU_PROBE_REMOTE_XN_START=${TILEXR_CCU_PROBE_RANK1_REMOTE_XN_START}")
fi
if [ "${TILEXR_CCU_PROBE_RANK0_REMOTE_XN_COUNT:-}" != "" ]; then
    rank0_env+=("TILEXR_CCU_PROBE_REMOTE_XN_COUNT=${TILEXR_CCU_PROBE_RANK0_REMOTE_XN_COUNT}")
elif [ "${TILEXR_CCU_PROBE_REMOTE_XN_COUNT:-}" != "" ]; then
    rank0_env+=("TILEXR_CCU_PROBE_REMOTE_XN_COUNT=${TILEXR_CCU_PROBE_REMOTE_XN_COUNT}")
fi
if [ "${TILEXR_CCU_PROBE_RANK1_REMOTE_XN_COUNT:-}" != "" ]; then
    rank1_env+=("TILEXR_CCU_PROBE_REMOTE_XN_COUNT=${TILEXR_CCU_PROBE_RANK1_REMOTE_XN_COUNT}")
elif [ "${TILEXR_CCU_PROBE_REMOTE_XN_COUNT:-}" != "" ]; then
    rank1_env+=("TILEXR_CCU_PROBE_REMOTE_XN_COUNT=${TILEXR_CCU_PROBE_REMOTE_XN_COUNT}")
fi
if [ "${TILEXR_CCU_PROBE_RANK0_LOCAL_WAIT_CKE_START:-}" != "" ]; then
    rank0_env+=("TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_START=${TILEXR_CCU_PROBE_RANK0_LOCAL_WAIT_CKE_START}")
fi
if [ "${TILEXR_CCU_PROBE_RANK1_LOCAL_WAIT_CKE_START:-}" != "" ]; then
    rank1_env+=("TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_START=${TILEXR_CCU_PROBE_RANK1_LOCAL_WAIT_CKE_START}")
fi
if [ "${TILEXR_CCU_PROBE_RANK0_LOCAL_WAIT_CKE_COUNT:-}" != "" ]; then
    rank0_env+=("TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_COUNT=${TILEXR_CCU_PROBE_RANK0_LOCAL_WAIT_CKE_COUNT}")
fi
if [ "${TILEXR_CCU_PROBE_RANK1_LOCAL_WAIT_CKE_COUNT:-}" != "" ]; then
    rank1_env+=("TILEXR_CCU_PROBE_LOCAL_WAIT_CKE_COUNT=${TILEXR_CCU_PROBE_RANK1_LOCAL_WAIT_CKE_COUNT}")
fi
if [ "${TILEXR_CCU_PROBE_RANK0_REMOTE_NOTIFY_CKE_START:-}" != "" ]; then
    rank0_env+=("TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_START=${TILEXR_CCU_PROBE_RANK0_REMOTE_NOTIFY_CKE_START}")
fi
if [ "${TILEXR_CCU_PROBE_RANK1_REMOTE_NOTIFY_CKE_START:-}" != "" ]; then
    rank1_env+=("TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_START=${TILEXR_CCU_PROBE_RANK1_REMOTE_NOTIFY_CKE_START}")
fi
if [ "${TILEXR_CCU_PROBE_RANK0_REMOTE_NOTIFY_CKE_COUNT:-}" != "" ]; then
    rank0_env+=("TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_COUNT=${TILEXR_CCU_PROBE_RANK0_REMOTE_NOTIFY_CKE_COUNT}")
fi
if [ "${TILEXR_CCU_PROBE_RANK1_REMOTE_NOTIFY_CKE_COUNT:-}" != "" ]; then
    rank1_env+=("TILEXR_CCU_PROBE_REMOTE_NOTIFY_CKE_COUNT=${TILEXR_CCU_PROBE_RANK1_REMOTE_NOTIFY_CKE_COUNT}")
fi
for endpoint_field in "${endpoint_fields[@]}"; do
    rank0_endpoint_var="TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_${endpoint_field}_RANK0"
    rank0_endpoint_value="${!rank0_endpoint_var:-}"
    if [ "${rank0_endpoint_value}" != "" ]; then
        rank0_env+=("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_${endpoint_field}=${rank0_endpoint_value}")
    fi
    rank1_endpoint_var="TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_${endpoint_field}_RANK1"
    rank1_endpoint_value="${!rank1_endpoint_var:-}"
    if [ "${rank1_endpoint_value}" != "" ]; then
        rank1_env+=("TILEXR_CCU_DIRECT_LOCAL_ENDPOINT_${endpoint_field}=${rank1_endpoint_value}")
    fi
done
for token_field in "${resource_window_token_fields[@]}"; do
    rank0_token_var="TILEXR_CCU_DIRECT_RESOURCE_WINDOW_${token_field}_RANK0"
    rank0_token_value="${!rank0_token_var:-}"
    if [ "${rank0_token_value}" != "" ]; then
        rank0_env+=("TILEXR_CCU_DIRECT_RESOURCE_WINDOW_${token_field}=${rank0_token_value}")
    fi
    rank1_token_var="TILEXR_CCU_DIRECT_RESOURCE_WINDOW_${token_field}_RANK1"
    rank1_token_value="${!rank1_token_var:-}"
    if [ "${rank1_token_value}" != "" ]; then
        rank1_env+=("TILEXR_CCU_DIRECT_RESOURCE_WINDOW_${token_field}=${rank1_token_value}")
    fi
done

echo "tilexr_ccu_direct_smoke_runner begin workDir=${work_dir} devices=${devices} commId=${comm_id} threadMode=${TILEXR_CCU_DIRECT_SMOKE_THREAD_MODE:-0} submit=${TILEXR_CCU_DIRECT_SMOKE_SUBMIT:-0} barrierMode=${TILEXR_CCU_DIRECT_BARRIER_MODE:-} p2pCcuCopy=${TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY:-0} timeout=${timeout_s} npuSmiTimeout=${TILEXR_CCU_SMOKE_NPU_SMI_TIMEOUT:-20}"

if [ "${TILEXR_CCU_DIRECT_SMOKE_THREAD_MODE:-0}" = "1" ]; then
    thread_log="${work_dir}/ccu_thread.log"
    rm -f "${thread_log}"
    thread_status=0
    timeout "${timeout_s}s" env "${common_env[@]}" TILEXR_CCU_PROBE_RANK=0 "${probe_bin}" > "${thread_log}" 2>&1 ||
        thread_status=$?
    cat "${thread_log}"
    echo "tilexr_ccu_direct_smoke_runner threadMode summary status=${thread_status} log=${thread_log}"
    if [ "${thread_status}" -ne 0 ]; then
        echo "ERROR: direct CCU thread-mode smoke failed status=${thread_status}" >&2
        echo "thread log: ${thread_log}" >&2
        exit 4
    fi
    if [ "$(grep -c "tilexr_ccu_direct_smoke prepare ret=0" "${thread_log}")" -lt 2 ]; then
        echo "ERROR: direct CCU thread-mode prepare did not return success for both ranks" >&2
        exit 5
    fi
    if [ "$(grep -c "installSucceeded=1" "${thread_log}")" -lt 2 ]; then
        echo "ERROR: direct CCU thread-mode prepare did not complete install attempt for both ranks" >&2
        exit 6
    fi
    if [ "${TILEXR_CCU_DIRECT_SMOKE_SUBMIT:-0}" = "1" ]; then
        if [ "$(grep -c "submitReady=1" "${thread_log}")" -lt 2 ]; then
            echo "ERROR: direct CCU thread-mode submit requested but prepare did not reach submitReady=1" >&2
            exit 6
        fi
        if [ "$(grep -c "tilexr_ccu_direct_smoke submit ret=0" "${thread_log}")" -lt 2 ]; then
            echo "ERROR: direct CCU thread-mode submit did not return success for both ranks" >&2
            exit 7
        fi
        if [ "$(grep -c "tilexr_ccu_direct_smoke submitTiming" "${thread_log}")" -lt 2 ]; then
            echo "ERROR: direct CCU thread-mode submit timing was not reported for both ranks" >&2
            exit 8
        fi
    fi
    if [ "${TILEXR_CCU_DIRECT_SMOKE_EXPECT_P2P_CCU_COPY:-0}" = "1" ]; then
        if [ "${TILEXR_CCU_DIRECT_SMOKE_SUBMIT:-0}" != "1" ]; then
            echo "ERROR: direct CCU thread-mode P2P CCU-copy check requires TILEXR_CCU_DIRECT_SMOKE_SUBMIT=1" >&2
            exit 11
        fi
        if [ "$(grep -c "tilexr_ccu_direct_smoke p2pCcuCopy" "${thread_log}")" -lt 2 ]; then
            echo "ERROR: direct CCU thread-mode P2P CCU-copy result missing" >&2
            exit 12
        fi
        if [ "$(grep -c "tilexr_ccu_direct_smoke p2pCcuCopy .*passed=1" "${thread_log}")" -lt 2 ]; then
            echo "ERROR: direct CCU thread-mode P2P CCU-copy check failed" >&2
            exit 13
        fi
    fi
    echo "tilexr_ccu_direct_smoke_runner success workDir=${work_dir}"
    exit 0
fi

timeout "${timeout_s}s" env "${common_env[@]}" "${rank0_env[@]}" TILEXR_CCU_PROBE_RANK=0 "${probe_bin}" > "${rank0_log}" 2>&1 &
rank0_pid=$!
sleep "${TILEXR_CCU_SMOKE_RANK1_DELAY:-1}"
timeout "${timeout_s}s" env "${common_env[@]}" "${rank1_env[@]}" TILEXR_CCU_PROBE_RANK=1 "${probe_bin}" > "${rank1_log}" 2>&1 &
rank1_pid=$!

rank0_status=0
rank1_status=0
wait "${rank0_pid}" || rank0_status=$?
wait "${rank1_pid}" || rank1_status=$?

cat "${rank0_log}"
cat "${rank1_log}"

echo "tilexr_ccu_direct_smoke_runner summary rank0Status=${rank0_status} rank1Status=${rank1_status} rank0Log=${rank0_log} rank1Log=${rank1_log}"

if [ "${rank0_status}" -ne 0 ] || [ "${rank1_status}" -ne 0 ]; then
    echo "ERROR: direct CCU smoke rank process failed rank0=${rank0_status} rank1=${rank1_status}" >&2
    echo "rank0 log: ${rank0_log}" >&2
    echo "rank1 log: ${rank1_log}" >&2
    exit 4
fi

for log in "${rank0_log}" "${rank1_log}"; do
    if ! grep -q "tilexr_ccu_direct_smoke prepare ret=0" "${log}"; then
        echo "ERROR: direct CCU prepare did not return success in ${log}" >&2
        exit 5
    fi
    if ! grep -q "installSucceeded=1" "${log}"; then
        echo "ERROR: direct CCU prepare did not complete install attempt in ${log}" >&2
        exit 6
    fi
done

if [ "${TILEXR_CCU_DIRECT_SMOKE_SUBMIT:-0}" = "1" ]; then
    for log in "${rank0_log}" "${rank1_log}"; do
        if ! grep -q "submitReady=1" "${log}"; then
            echo "ERROR: direct CCU submit requested but prepare did not reach submitReady=1 in ${log}" >&2
            exit 6
        fi
    done
    for log in "${rank0_log}" "${rank1_log}"; do
        if ! grep -q "tilexr_ccu_direct_smoke submit ret=0" "${log}"; then
            echo "ERROR: direct CCU submit did not return success in ${log}" >&2
            exit 7
        fi
        if ! grep -q "tilexr_ccu_direct_smoke submitTiming" "${log}"; then
            echo "ERROR: direct CCU submit timing was not reported in ${log}" >&2
            exit 8
        fi
    done
fi

if [ "${TILEXR_CCU_DIRECT_SMOKE_EXPECT_BARRIER_WAIT:-0}" = "1" ]; then
    if [ "${TILEXR_CCU_DIRECT_SMOKE_SUBMIT:-0}" != "1" ]; then
        echo "ERROR: direct CCU barrier wait check requires TILEXR_CCU_DIRECT_SMOKE_SUBMIT=1" >&2
        exit 9
    fi
    delay_rank="${TILEXR_CCU_DIRECT_SMOKE_DELAY_RANK:-0}"
    min_sync_ms="${TILEXR_CCU_DIRECT_SMOKE_MIN_SYNC_MS:-100}"
    if [ "${delay_rank}" = "0" ]; then
        wait_log="${rank1_log}"
    else
        wait_log="${rank0_log}"
    fi
    wait_sync_ms="$(
        awk '
            /tilexr_ccu_direct_smoke submitTiming/ {
                for (i = 1; i <= NF; ++i) {
                    if ($i ~ /^syncMs=/) {
                        split($i, parts, "=");
                        print parts[2];
                    }
                }
            }
        ' "${wait_log}" | tail -n 1
    )"
    if [ -z "${wait_sync_ms}" ]; then
        echo "ERROR: barrier wait timing missing from ${wait_log}" >&2
        exit 9
    fi
    if [ "${wait_sync_ms}" -lt "${min_sync_ms}" ]; then
        echo "ERROR: direct CCU barrier wait was too short syncMs=${wait_sync_ms} minSyncMs=${min_sync_ms} log=${wait_log}" >&2
        exit 10
    fi
fi

if [ "${TILEXR_CCU_DIRECT_SMOKE_EXPECT_P2P_CCU_COPY:-0}" = "1" ]; then
    if [ "${TILEXR_CCU_DIRECT_SMOKE_SUBMIT:-0}" != "1" ]; then
        echo "ERROR: direct CCU P2P CCU-copy check requires TILEXR_CCU_DIRECT_SMOKE_SUBMIT=1" >&2
        exit 11
    fi
    p2p_passed_count=0
    for log in "${rank0_log}" "${rank1_log}"; do
        if ! grep -q "tilexr_ccu_direct_smoke p2pCcuCopy" "${log}"; then
            echo "ERROR: direct CCU P2P CCU-copy result missing in ${log}" >&2
            exit 12
        fi
        if grep -q "tilexr_ccu_direct_smoke p2pCcuCopy .*passed=1" "${log}"; then
            p2p_passed_count=$((p2p_passed_count + 1))
            continue
        fi
        if grep -q "tilexr_ccu_direct_smoke p2pCcuCopy skipped" "${log}"; then
            continue
        fi
        if ! grep -q "tilexr_ccu_direct_smoke p2pCcuCopy .*passed=1" "${log}"; then
            echo "ERROR: direct CCU P2P CCU-copy check failed in ${log}" >&2
            exit 13
        fi
    done
    if [ "${p2p_passed_count}" -lt 1 ]; then
        echo "ERROR: direct CCU P2P CCU-copy produced no passing receiver result" >&2
        exit 13
    fi
fi

echo "tilexr_ccu_direct_smoke_runner success workDir=${work_dir}"
