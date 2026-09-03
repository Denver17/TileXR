#!/usr/bin/env bash
#
# Copyright (c) 2026 TileXR Project
#
# One-shot direct CCU bring-up wrapper for the reserved 20:00+ hardware window.
# It still fails closed: submit/barrier/P2P stages run only after prepare logs
# show submitReady=1 for both ranks.

set -euo pipefail

for arg in "$@"; do
    case "${arg}" in
        --dry-run)
            export TILEXR_CCU_DIRECT_EVENING_SMOKE_DRY_RUN=1
            ;;
        *)
            echo "ERROR: unknown argument: ${arg}" >&2
            exit 2
            ;;
    esac
done

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${repo_root}"

set_evening_defaults()
{
    export TILEXR_RUN_CCU_DIRECT_SMOKE_PROBE=1
    export TILEXR_CCU_SMOKE_REQUIRE_NPU_SMI=1
    if [ "${TILEXR_CCU_DIRECT_SMOKE_THREAD_MODE:-}" = "" ]; then
        export TILEXR_CCU_DIRECT_SMOKE_THREAD_MODE=0
    else
        export TILEXR_CCU_DIRECT_SMOKE_THREAD_MODE
    fi
    export TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT="${TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT:-1}"
    export TILEXR_CCU_SMOKE_DEVICES="${TILEXR_CCU_SMOKE_DEVICES:-0,1}"
    export TILEXR_CCU_DIRECT_TRACE="${TILEXR_CCU_DIRECT_TRACE:-1}"
    export TILEXR_LOG_LEVEL="${TILEXR_LOG_LEVEL:-INFO}"
    export TILEXR_CCU_SMOKE_TIMEOUT="${TILEXR_CCU_SMOKE_TIMEOUT:-120}"
    export TILEXR_CCU_PROBE_MISSION_START="${TILEXR_CCU_PROBE_MISSION_START:-6}"
    export TILEXR_CCU_PROBE_INSTRUCTION_START="${TILEXR_CCU_PROBE_INSTRUCTION_START:-475}"
    export TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START="${TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START:-489}"
    export TILEXR_CCU_PROBE_SQE_ARG_COUNT="${TILEXR_CCU_PROBE_SQE_ARG_COUNT:-13}"
    export TILEXR_CCU_PROBE_SYNC_INSTRUCTION_COUNT="${TILEXR_CCU_PROBE_SYNC_INSTRUCTION_COUNT:-143}"
    export TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW="${TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW:-full_repository}"
    export TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE="${TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE:-instruction_bytes}"
    export TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE="${TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE:-acl}"
    export TILEXR_CCU_DIRECT_INSTALL_ORDER="${TILEXR_CCU_DIRECT_INSTALL_ORDER:-lower_layer_first}"
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
    if [ "${TILEXR_CCU_DIRECT_BARRIER_MODE:-}" = "" ]; then
        export TILEXR_CCU_DIRECT_BARRIER_MODE=sync_cke
    else
        export TILEXR_CCU_DIRECT_BARRIER_MODE
    fi
    if [ "${TILEXR_CCU_DIRECT_LOWER_LAYER_WQE_MODE:-}" = "" ]; then
        export TILEXR_CCU_DIRECT_LOWER_LAYER_WQE_MODE=hcomm_cap
    else
        export TILEXR_CCU_DIRECT_LOWER_LAYER_WQE_MODE
    fi
    export TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES="${TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES:-prepare,submit,barrier,p2p}"
    export TILEXR_CCU_DIRECT_EVENING_PREPARE_ALLOC_MODES="${TILEXR_CCU_DIRECT_EVENING_PREPARE_ALLOC_MODES:-acl,acl_module3,rt_hbm}"
    export TILEXR_CCU_DIRECT_EVENING_PREPARE_PROFILES="${TILEXR_CCU_DIRECT_EVENING_PREPARE_PROFILES:-}"
}

prepare_profiles=()
prepare_profile_name=""
prepare_profile_alloc=""
prepare_profile_window=""
prepare_profile_data_len_mode=""
prepare_profile_install_order=""
prepare_profile_pfe_offset_source=""
prepare_profile_pfe_partition=""

build_prepare_profiles()
{
    prepare_profiles=()
    if [ "${TILEXR_CCU_DIRECT_EVENING_PREPARE_PROFILES:-}" != "" ]; then
        IFS=',' read -r -a prepare_profiles <<< "${TILEXR_CCU_DIRECT_EVENING_PREPARE_PROFILES}"
        return
    fi

    local prepare_alloc_modes=()
    local prepare_mode
    IFS=',' read -r -a prepare_alloc_modes <<< "${TILEXR_CCU_DIRECT_EVENING_PREPARE_ALLOC_MODES}"
    for prepare_mode in "${prepare_alloc_modes[@]}"; do
        prepare_mode="${prepare_mode//[[:space:]]/}"
        if [ "${prepare_mode}" = "" ]; then
            continue
        fi
        prepare_profiles+=(
            "${prepare_mode}:${prepare_mode}:${TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW}:${TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE}:${TILEXR_CCU_DIRECT_INSTALL_ORDER}"
        )
    done
}

parse_prepare_profile()
{
    local profile="$1"
    local extra1=""
    local extra2=""
    local extra3=""
    IFS=':' read -r \
        prepare_profile_name \
        prepare_profile_alloc \
        prepare_profile_window \
        prepare_profile_data_len_mode \
        prepare_profile_install_order \
        extra1 \
        extra2 \
        extra3 <<< "${profile}"
    prepare_profile_name="${prepare_profile_name//[[:space:]]/}"
    prepare_profile_alloc="${prepare_profile_alloc//[[:space:]]/}"
    prepare_profile_window="${prepare_profile_window//[[:space:]]/}"
    prepare_profile_data_len_mode="${prepare_profile_data_len_mode//[[:space:]]/}"
    prepare_profile_install_order="${prepare_profile_install_order//[[:space:]]/}"
    prepare_profile_pfe_offset_source="${extra1//[[:space:]]/}"
    prepare_profile_pfe_partition="${extra2//[[:space:]]/}"
    if [ "${prepare_profile_name}" = "" ] ||
        [ "${prepare_profile_alloc}" = "" ] ||
        [ "${prepare_profile_window}" = "" ] ||
        [ "${prepare_profile_data_len_mode}" = "" ] ||
        [ "${prepare_profile_install_order}" = "" ] ||
        [ "${extra3}" != "" ]; then
        echo "ERROR: invalid prepare profile '${profile}', expected name:alloc:window:dataLenMode:installOrder[:pfeOffsetSource:pfePartition]" >&2
        exit 21
    fi
    if { [ "${prepare_profile_pfe_offset_source}" != "" ] && [ "${prepare_profile_pfe_partition}" = "" ]; } ||
        { [ "${prepare_profile_pfe_offset_source}" = "" ] && [ "${prepare_profile_pfe_partition}" != "" ]; }; then
        echo "ERROR: invalid prepare profile '${profile}', pfeOffsetSource and pfePartition must be provided together" >&2
        exit 21
    fi
}

print_prepare_profile_dry_run()
{
    build_prepare_profiles
    local index=0
    local profile
    for profile in "${prepare_profiles[@]}"; do
        parse_prepare_profile "${profile}"
        echo "dryRun prepareProfile[${index}] name=${prepare_profile_name} alloc=${prepare_profile_alloc} window=${prepare_profile_window} dataLenMode=${prepare_profile_data_len_mode} installOrder=${prepare_profile_install_order} pfeOffsetSource=${prepare_profile_pfe_offset_source:-default} pfePartition=${prepare_profile_pfe_partition:-default}"
        index=$((index + 1))
    done
}

print_resource_window_token_dry_run()
{
    local token_field
    for token_field in TOKEN_ID RAW_TOKEN_ID TOKEN_VALUE; do
        local token_var="TILEXR_CCU_DIRECT_RESOURCE_WINDOW_${token_field}"
        local token_value="${!token_var:-}"
        local rank0_token_var="TILEXR_CCU_DIRECT_RESOURCE_WINDOW_${token_field}_RANK0"
        local rank0_token_value="${!rank0_token_var:-}"
        local rank1_token_var="TILEXR_CCU_DIRECT_RESOURCE_WINDOW_${token_field}_RANK1"
        local rank1_token_value="${!rank1_token_var:-}"
        if [ "${token_value}" != "" ]; then
            echo "${token_var}=${token_value}"
        fi
        if [ "${rank0_token_value}" != "" ]; then
            echo "${rank0_token_var}=${rank0_token_value}"
        fi
        if [ "${rank1_token_value}" != "" ]; then
            echo "${rank1_token_var}=${rank1_token_value}"
        fi
    done
}

run_dry_run()
{
    export TILEXR_CCU_DIRECT_SMOKE_DRY_RUN=1
    echo "tilexr_ccu_direct_evening_smoke dryRun=1"
    echo "TILEXR_CCU_SMOKE_DEVICES=${TILEXR_CCU_SMOKE_DEVICES}"
    echo "TILEXR_CCU_DIRECT_SMOKE_THREAD_MODE=${TILEXR_CCU_DIRECT_SMOKE_THREAD_MODE}"
    echo "TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT=${TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT}"
    echo "TILEXR_CCU_DIRECT_SMOKE_SUBMIT=${TILEXR_CCU_DIRECT_SMOKE_SUBMIT:-0}"
    echo "TILEXR_CCU_DIRECT_SMOKE_DRY_RUN=${TILEXR_CCU_DIRECT_SMOKE_DRY_RUN}"
    echo "TILEXR_CCU_SMOKE_DRY_RUN=${TILEXR_CCU_SMOKE_DRY_RUN:-0}"
    echo "TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES=${TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES}"
    echo "TILEXR_CCU_DIRECT_EVENING_PREPARE_ALLOC_MODES=${TILEXR_CCU_DIRECT_EVENING_PREPARE_ALLOC_MODES}"
    echo "TILEXR_CCU_DIRECT_EVENING_PREPARE_PROFILES=${TILEXR_CCU_DIRECT_EVENING_PREPARE_PROFILES}"
    echo "TILEXR_CCU_DIRECT_BARRIER_MODE=${TILEXR_CCU_DIRECT_BARRIER_MODE}"
    echo "TILEXR_CCU_DIRECT_LOWER_LAYER_WQE_MODE=${TILEXR_CCU_DIRECT_LOWER_LAYER_WQE_MODE}"
    echo "TILEXR_CCU_PROBE_MISSION_START=${TILEXR_CCU_PROBE_MISSION_START}"
    echo "TILEXR_CCU_PROBE_INSTRUCTION_START=${TILEXR_CCU_PROBE_INSTRUCTION_START}"
    echo "TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START=${TILEXR_CCU_PROBE_MISSION_INSTRUCTION_START}"
    echo "TILEXR_CCU_PROBE_SQE_ARG_COUNT=${TILEXR_CCU_PROBE_SQE_ARG_COUNT}"
    echo "TILEXR_CCU_PROBE_SYNC_INSTRUCTION_COUNT=${TILEXR_CCU_PROBE_SYNC_INSTRUCTION_COUNT}"
    echo "TILEXR_CCU_PROBE_GSA_START=${TILEXR_CCU_PROBE_GSA_START}"
    echo "TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE=${TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE}"
    echo "TILEXR_CCU_DIRECT_INSTALL_ORDER=${TILEXR_CCU_DIRECT_INSTALL_ORDER}"
    print_resource_window_token_dry_run
    print_prepare_profile_dry_run
    bash tests/ccu/run_tilexr_ccu_direct_smoke.sh
}

if [ "${TILEXR_CCU_DIRECT_EVENING_SMOKE_DRY_RUN:-0}" = "1" ] ||
    [ "${TILEXR_CCU_SMOKE_DRY_RUN:-0}" = "1" ]; then
    set_evening_defaults
    run_dry_run
    exit 0
fi

if [ -f scripts/common_env.sh ]; then
    # shellcheck source=/dev/null
    source scripts/common_env.sh >/tmp/tilexr_env_evening_smoke.log 2>&1
fi

set_evening_defaults
evening_work_root="${TILEXR_CCU_EVENING_WORK_ROOT:-${repo_root}/build/ccu_direct_evening_smoke/$(date +%Y%m%d_%H%M%S)}"

stage_enabled()
{
    case ",${TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES}," in
        *",$1,"*) return 0 ;;
        *) return 1 ;;
    esac
}

append_profile_pfe_env()
{
    if [ "${1:-}" = "" ] && [ "${2:-}" = "" ]; then
        return
    fi
    printf '%s\n' \
        "TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_OFFSET_SOURCE=$1" \
        "TILEXR_CCU_DIRECT_LOWER_LAYER_PFE_PARTITION=$2"
}

prepare_has_submit_ready()
{
    local prepare_dir="$1"
    local thread_log="${prepare_dir}/ccu_thread.log"
    local rank0_log="${prepare_dir}/ccu_rank0.log"
    local rank1_log="${prepare_dir}/ccu_rank1.log"
    if [ -f "${thread_log}" ]; then
        [ "$(grep -c "submitReady=1" "${thread_log}")" -ge 2 ]
        return
    fi
    [ -f "${rank0_log}" ] && [ -f "${rank1_log}" ] &&
        grep -q "submitReady=1" "${rank0_log}" &&
        grep -q "submitReady=1" "${rank1_log}"
}

summarize_stage_logs()
{
    local stage="$1"
    local stage_dir="$2"
    local status="$3"
    local mode="${4:-}"
    local found=0
    for log in \
        "${stage_dir}/ccu_thread.log" \
        "${stage_dir}/ccu_rank0.log" \
        "${stage_dir}/ccu_rank1.log"; do
        if [ ! -f "${log}" ]; then
            continue
        fi
        found=1
        if [ "${stage}" = "prepare" ]; then
            echo "tilexr_ccu_direct_evening_smoke prepareLogSummary mode=${mode} status=${status} log=${log}"
        else
            echo "tilexr_ccu_direct_evening_smoke stageLogSummary stage=${stage} status=${status} log=${log}"
        fi
        grep -E 'direct CCU submit failed|rtRet=|args\[' "${log}" |
            head -n "${TILEXR_CCU_EVENING_SUBMIT_FAILURE_SUMMARY_LINES:-8}" || true
        grep -E \
            'tilexr_ccu_direct_smoke config|tilexr_ccu_direct_smoke prepare|tilexr_ccu_direct_smoke preparedTasks|tilexr_ccu_direct_smoke submit|tilexr_ccu_direct_smoke submitTiming|tilexr_ccu_direct_smoke p2pCcuCopy|tilexr_ccu_direct_smoke aclrtSynchronizeStream|direct CCU submit failed|rtRet=|args\[|CCU custom channel call failed|op=[0-9]+|driverRet=|opRet=|SET_INSTRUCTION|SET_MSID_TOKEN|submitReady=|TileXRDirectCcuTrace .*decoded=|TileXRDirectCcuTrace remoteXnBinding|TileXRDirectCcuTrace task\[|TileXRDirectCcuTrace finalRuntimeTask|TileXRDirectCcuTrace customChannel.return|TileXRDirectCcuTrace program.sync' \
            "${log}" | tail -n "${TILEXR_CCU_EVENING_LOG_SUMMARY_LINES:-24}" || true
    done
    if [ "${found}" -eq 0 ]; then
        if [ "${stage}" = "prepare" ]; then
            echo "tilexr_ccu_direct_evening_smoke prepareLogSummary mode=${mode} status=${status} log=missing workDir=${stage_dir}"
        else
            echo "tilexr_ccu_direct_evening_smoke stageLogSummary stage=${stage} status=${status} log=missing workDir=${stage_dir}"
        fi
    fi
}

extract_last_log_field()
{
    local key="$1"
    shift
    awk -v key="${key}" '
        {
            for (i = 1; i <= NF; ++i) {
                if ($i ~ ("^" key "=")) {
                    split($i, parts, "=");
                    value = parts[2];
                    gsub(/[^0-9A-Za-z_.:-].*$/, "", value);
                    last = value;
                }
            }
        }
        END {
            if (last != "") {
                print last;
            }
        }
    ' "$@" 2>/dev/null || true
}

print_prepare_matrix_summary()
{
    local prepare_dir="$1"
    local status="$2"
    local profile="$3"
    local alloc="$4"
    local window="$5"
    local data_len_mode="$6"
    local install_order="$7"
    local logs=()
    local log
    for log in \
        "${prepare_dir}/ccu_thread.log" \
        "${prepare_dir}/ccu_rank0.log" \
        "${prepare_dir}/ccu_rank1.log"; do
        if [ -f "${log}" ]; then
            logs+=("${log}")
        fi
    done

    local submit_ready="NA"
    local op="NA"
    local driver_ret="NA"
    local op_ret="NA"
    local lower_layer_preconditions=0
    local summary_log="missing"
    if [ "${#logs[@]}" -gt 0 ]; then
        summary_log="${logs[0]}"
        submit_ready="$(extract_last_log_field submitReady "${logs[@]}")"
        op="$(extract_last_log_field op "${logs[@]}")"
        driver_ret="$(extract_last_log_field driverRet "${logs[@]}")"
        op_ret="$(extract_last_log_field opRet "${logs[@]}")"
        if grep -q 'lowerLayerPreconditions{' "${logs[@]}"; then
            lower_layer_preconditions=1
        fi
    fi
    submit_ready="${submit_ready:-NA}"
    op="${op:-NA}"
    driver_ret="${driver_ret:-NA}"
    op_ret="${op_ret:-NA}"

    echo "tilexr_ccu_direct_evening_smoke prepareMatrix profile=${profile} status=${status} submitReady=${submit_ready} op=${op} driverRet=${driver_ret} opRet=${op_ret} lowerLayerPreconditions=${lower_layer_preconditions} alloc=${alloc} window=${window} dataLenMode=${data_len_mode} installOrder=${install_order} log=${summary_log}"
}

run_smoke_stage()
{
    local stage="$1"
    shift
    local stage_dir="${evening_work_root}/${stage}"
    mkdir -p "${stage_dir}"
    echo "tilexr_ccu_direct_evening_smoke stage=${stage} workDir=${stage_dir}"
    timeout "${TILEXR_CCU_EVENING_TOTAL_TIMEOUT:-160}s" \
        env TILEXR_CCU_SMOKE_WORK_DIR="${stage_dir}" "$@" bash tests/ccu/run_tilexr_ccu_direct_smoke.sh
}

prepare_stage_exit_is_environmental()
{
    local status="$1"
    [ "${status}" -eq 3 ] || [ "${status}" -eq 124 ]
}

cmake --build build --target tile-comm -j"${TILEXR_CCU_EVENING_BUILD_JOBS:-2}"

if ! stage_enabled prepare; then
    echo "ERROR: TILEXR_CCU_DIRECT_EVENING_SMOKE_STAGES must include prepare" >&2
    exit 20
fi

build_prepare_profiles
selected_prepare_alloc_mode=""
selected_prepare_window=""
selected_prepare_data_len_mode=""
selected_prepare_install_order=""
selected_prepare_pfe_offset_source=""
selected_prepare_pfe_partition=""
selected_prepare_dir=""
selected_prepare_profile_name=""
prepare_status_summary=""

print_prepare_failure_final_status()
{
    local final_status_line="tilexr_ccu_direct_evening_smoke finalStatus prepare=fail submit=skipped barrier=skipped p2p=skipped completionCandidate=0 failedStage=prepare selectedProfile=none selectedAlloc=none selectedWindow=none selectedDataLenMode=none selectedInstallOrder=none pfeOffsetSource=default pfePartition=default prepareStatusSummary=${prepare_status_summary}"
    echo "${final_status_line}"
    mkdir -p "${evening_work_root}"
    printf '%s\n' "${final_status_line}" > "${evening_work_root}/final_status.log"
}

for prepare_profile in "${prepare_profiles[@]}"; do
    parse_prepare_profile "${prepare_profile}"
    if [ "${prepare_profile_name}" = "" ]; then
        continue
    fi
    safe_prepare_mode="$(printf '%s' "${prepare_profile_name}" | sed 's/[^A-Za-z0-9_]/_/g')"
    prepare_status=0
    run_smoke_stage "prepare_${safe_prepare_mode}" \
        TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE="${prepare_profile_alloc}" \
        TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW="${prepare_profile_window}" \
        TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE="${prepare_profile_data_len_mode}" \
        TILEXR_CCU_DIRECT_INSTALL_ORDER="${prepare_profile_install_order}" \
        $(append_profile_pfe_env "${prepare_profile_pfe_offset_source}" "${prepare_profile_pfe_partition}") ||
        prepare_status=$?
    prepare_dir="${evening_work_root}/prepare_${safe_prepare_mode}"
    echo "tilexr_ccu_direct_evening_smoke prepareStatus=${prepare_status} profile=${prepare_profile_name} alloc=${prepare_profile_alloc} window=${prepare_profile_window} dataLenMode=${prepare_profile_data_len_mode} installOrder=${prepare_profile_install_order} pfeOffsetSource=${prepare_profile_pfe_offset_source:-default} pfePartition=${prepare_profile_pfe_partition:-default} workDir=${prepare_dir}"
    print_prepare_matrix_summary \
        "${prepare_dir}" \
        "${prepare_status}" \
        "${prepare_profile_name}" \
        "${prepare_profile_alloc}" \
        "${prepare_profile_window}" \
        "${prepare_profile_data_len_mode}" \
        "${prepare_profile_install_order}"
    summarize_stage_logs prepare "${prepare_dir}" "${prepare_status}" "${prepare_profile_name}"
    prepare_status_summary="${prepare_status_summary}${prepare_status_summary:+,}${prepare_profile_name}:${prepare_status}:${prepare_profile_alloc}:${prepare_profile_window}:${prepare_profile_data_len_mode}:${prepare_profile_install_order}:${prepare_profile_pfe_offset_source:-default}:${prepare_profile_pfe_partition:-default}:${prepare_dir}"
    if prepare_stage_exit_is_environmental "${prepare_status}"; then
        echo "ERROR: direct CCU prepare stopped on environmental gate status=${prepare_status} profile=${prepare_profile_name} alloc=${prepare_profile_alloc} window=${prepare_profile_window} dataLenMode=${prepare_profile_data_len_mode} installOrder=${prepare_profile_install_order} pfeOffsetSource=${prepare_profile_pfe_offset_source:-default} pfePartition=${prepare_profile_pfe_partition:-default} workDir=${prepare_dir}" >&2
        exit "${prepare_status}"
    fi
    if [ "${prepare_status}" -eq 0 ] && prepare_has_submit_ready "${prepare_dir}"; then
        selected_prepare_alloc_mode="${prepare_profile_alloc}"
        selected_prepare_window="${prepare_profile_window}"
        selected_prepare_data_len_mode="${prepare_profile_data_len_mode}"
        selected_prepare_install_order="${prepare_profile_install_order}"
        selected_prepare_pfe_offset_source="${prepare_profile_pfe_offset_source}"
        selected_prepare_pfe_partition="${prepare_profile_pfe_partition}"
        selected_prepare_dir="${prepare_dir}"
        selected_prepare_profile_name="${prepare_profile_name}"
        break
    fi
done

if [ "${selected_prepare_alloc_mode}" = "" ]; then
    print_prepare_failure_final_status
    echo 'tilexr_ccu_direct_evening_smoke stopAfter=prepare reason="submitReady=1 missing for every prepare profile" workRoot='"${evening_work_root} prepareStatusSummary=${prepare_status_summary}"
    exit 0
fi
echo "tilexr_ccu_direct_evening_smoke selectedPrepare alloc=${selected_prepare_alloc_mode} window=${selected_prepare_window} dataLenMode=${selected_prepare_data_len_mode} installOrder=${selected_prepare_install_order} pfeOffsetSource=${selected_prepare_pfe_offset_source:-default} pfePartition=${selected_prepare_pfe_partition:-default} workDir=${selected_prepare_dir}"

submit_final_status="skipped"
barrier_final_status="skipped"
p2p_final_status="skipped"

print_final_status()
{
    local failed_stage="${1:-none}"
    local completion_candidate=0
    if [ "${failed_stage}" = "none" ] &&
        [ "${submit_final_status}" = "pass" ] &&
        [ "${barrier_final_status}" = "pass" ] &&
        [ "${p2p_final_status}" = "pass" ]; then
        completion_candidate=1
    fi

    local final_status_line="tilexr_ccu_direct_evening_smoke finalStatus prepare=pass submit=${submit_final_status} barrier=${barrier_final_status} p2p=${p2p_final_status} completionCandidate=${completion_candidate} failedStage=${failed_stage} selectedProfile=${selected_prepare_profile_name} selectedAlloc=${selected_prepare_alloc_mode} selectedWindow=${selected_prepare_window} selectedDataLenMode=${selected_prepare_data_len_mode} selectedInstallOrder=${selected_prepare_install_order} pfeOffsetSource=${selected_prepare_pfe_offset_source:-default} pfePartition=${selected_prepare_pfe_partition:-default}"
    echo "${final_status_line}"
    mkdir -p "${evening_work_root}"
    printf '%s\n' "${final_status_line}" > "${evening_work_root}/final_status.log"
}

if stage_enabled submit; then
    submit_status=0
    run_smoke_stage submit \
        TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE="${selected_prepare_alloc_mode}" \
        TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW="${selected_prepare_window}" \
        TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE="${selected_prepare_data_len_mode}" \
        TILEXR_CCU_DIRECT_INSTALL_ORDER="${selected_prepare_install_order}" \
        $(append_profile_pfe_env "${selected_prepare_pfe_offset_source}" "${selected_prepare_pfe_partition}") \
        TILEXR_CCU_DIRECT_SMOKE_SUBMIT=1 ||
        submit_status=$?
    summarize_stage_logs submit "${evening_work_root}/submit" "${submit_status}"
    if [ "${submit_status}" -ne 0 ]; then
        submit_final_status="fail"
        print_final_status submit
        exit "${submit_status}"
    fi
    submit_final_status="pass"
fi

if stage_enabled barrier; then
    barrier_status=0
    run_smoke_stage barrier \
        TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE="${selected_prepare_alloc_mode}" \
        TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW="${selected_prepare_window}" \
        TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE="${selected_prepare_data_len_mode}" \
        TILEXR_CCU_DIRECT_INSTALL_ORDER="${selected_prepare_install_order}" \
        $(append_profile_pfe_env "${selected_prepare_pfe_offset_source}" "${selected_prepare_pfe_partition}") \
        TILEXR_CCU_DIRECT_BARRIER_MODE="${TILEXR_CCU_DIRECT_BARRIER_MODE}" \
        TILEXR_CCU_DIRECT_SMOKE_SUBMIT=1 \
        TILEXR_CCU_DIRECT_SMOKE_DELAY_RANK="${TILEXR_CCU_DIRECT_SMOKE_DELAY_RANK:-0}" \
        TILEXR_CCU_DIRECT_SMOKE_PRE_SUBMIT_DELAY_MS="${TILEXR_CCU_DIRECT_SMOKE_PRE_SUBMIT_DELAY_MS:-300}" \
        TILEXR_CCU_DIRECT_SMOKE_EXPECT_BARRIER_WAIT=1 \
        TILEXR_CCU_DIRECT_SMOKE_MIN_SYNC_MS="${TILEXR_CCU_DIRECT_SMOKE_MIN_SYNC_MS:-100}" ||
        barrier_status=$?
    summarize_stage_logs barrier "${evening_work_root}/barrier" "${barrier_status}"
    if [ "${barrier_status}" -ne 0 ]; then
        barrier_final_status="fail"
        print_final_status barrier
        exit "${barrier_status}"
    fi
    barrier_final_status="pass"
fi

if stage_enabled p2p; then
    p2p_status=0
    run_smoke_stage p2p \
        TILEXR_CCU_DIRECT_REPOSITORY_MEMORY_ALLOC_MODE="${selected_prepare_alloc_mode}" \
        TILEXR_CCU_DIRECT_REPOSITORY_INSTALL_WINDOW="${selected_prepare_window}" \
        TILEXR_CCU_DIRECT_REPOSITORY_DATA_LEN_MODE="${selected_prepare_data_len_mode}" \
        TILEXR_CCU_DIRECT_INSTALL_ORDER="${selected_prepare_install_order}" \
        $(append_profile_pfe_env "${selected_prepare_pfe_offset_source}" "${selected_prepare_pfe_partition}") \
        TILEXR_CCU_DIRECT_SMOKE_DIRECT_CCU_ONLY_INIT=1 \
        TILEXR_CCU_DIRECT_SMOKE_SUBMIT=1 \
        TILEXR_CCU_DIRECT_SMOKE_P2P_CCU_COPY=1 \
        TILEXR_CCU_DIRECT_SMOKE_EXPECT_P2P_CCU_COPY=1 ||
        p2p_status=$?
    summarize_stage_logs p2p "${evening_work_root}/p2p" "${p2p_status}"
    if [ "${p2p_status}" -ne 0 ]; then
        p2p_final_status="fail"
        print_final_status p2p
        exit "${p2p_status}"
    fi
    p2p_final_status="pass"
fi

print_final_status none
echo "tilexr_ccu_direct_evening_smoke success workRoot=${evening_work_root}"
