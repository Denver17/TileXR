#!/usr/bin/env bash
# 为配置中的集群部署 Memory EP demo，执行 correctness 或带 msprof 的性能测量。
# 性能结果沿用预热、rank barrier 和 msprof Task Duration 统计路径。
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
readonly TILEXR_ROOT=$(cd -- "${script_dir}/../../.." && pwd)
readonly REMOTE_ROOT=${TILEXR_EP_CLUSTER_REMOTE_ROOT:-/home/h00580772/combine_test}
readonly MASTER_HOST=${TILEXR_EP_CLUSTER_MASTER_HOST:-141.61.94.18}
readonly SSH_USER=root
readonly LOCAL_NPU_COUNT=8
readonly LAST_SAMPLE_COUNT=50
readonly MSPROF_94=${TILEXR_EP_CLUSTER_MSPROF_94:-${TILEXR_EP_CLUSTER_MSPROF:-/usr/local/Ascend/cann-9.1.T555/tools/profiler/bin/msprof}}
readonly CANN_ENV_SCRIPT_94=${TILEXR_EP_CLUSTER_CANN_ENV_SCRIPT_94:-${TILEXR_EP_CLUSTER_CANN_ENV_SCRIPT:-/usr/local/Ascend/cann-9.1.T555/bin/setenv.bash}}
readonly MSPROF_95=${TILEXR_EP_CLUSTER_MSPROF_95:-${TILEXR_EP_CLUSTER_MSPROF:-/home/pkg/920_b106/cann-9.2.0/tools/profiler/bin/msprof}}
readonly CANN_ENV_SCRIPT_95=${TILEXR_EP_CLUSTER_CANN_ENV_SCRIPT_95:-${TILEXR_EP_CLUSTER_CANN_ENV_SCRIPT:-/home/pkg/920_b106/cann-9.2.0/bin/setenv.bash}}
readonly PYTHON=/usr/bin/python3
readonly HOST_FILE=${TILEXR_EP_CLUSTER_HOST_FILE:-"${script_dir}/hosts_terbo_64p.txt"}
readonly DISPATCH_CONFIG_FILE=${TILEXR_EP_CLUSTER_DISPATCH_CONFIG_FILE:-"${script_dir}/memory_dispatch_64p.env"}
readonly COMBINE_CONFIG_FILE=${TILEXR_EP_CLUSTER_COMBINE_CONFIG_FILE:-"${script_dir}/combine_memory_64p.env"}
readonly DISPATCH_NODE_SCRIPT="${REMOTE_ROOT}/tests/ep/cluster/run_memory_dispatch_node.sh"
readonly COMBINE_NODE_SCRIPT="${REMOTE_ROOT}/tests/ep/cluster/run_memory_combine_node.sh"
readonly STATS_SCRIPT="${REMOTE_ROOT}/tools/ep/msprof_kernel_stats.py"
readonly BUILD_CACHE="${REMOTE_ROOT}/build_ep/CMakeCache.txt"
readonly DEMO_BIN="${REMOTE_ROOT}/tests/ep/install/bin/tilexr_ep_dispatch_demo"
readonly RSYNC_RSH='ssh -o BatchMode=yes -o ConnectTimeout=8 -o ConnectionAttempts=1 -o StrictHostKeyChecking=no -o LogLevel=ERROR -o ServerAliveInterval=5 -o ServerAliveCountMax=12'

OP_SELECTION=dispatch
RUN_MODE=performance
FORCE_BUILD=false
declare -a selected_ops=()
declare -a hosts=()
declare -A perf_min=()
declare -A perf_max=()
declare -A perf_avg=()
declare -A perf_json=()
declare -A functional_result=()

ssh_options=(
    -o BatchMode=yes
    -o ConnectTimeout=8
    -o ConnectionAttempts=1
    -o StrictHostKeyChecking=no
    -o LogLevel=ERROR
    -o ServerAliveInterval=5
    -o ServerAliveCountMax=12
)

usage() {
    cat <<'EOF'
Usage: run_memory_dispatch_msprof_64p.sh [--op dispatch|combine|both]
                                         [--mode functional|performance]
                                         [--build]

Defaults:
  --op dispatch
  --mode performance

Options:
  --build  Force a full rebuild and replace deployed artifacts.

Configuration:
  TILEXR_EP_CLUSTER_HOST_FILE
  TILEXR_EP_CLUSTER_DISPATCH_CONFIG_FILE
  TILEXR_EP_CLUSTER_COMBINE_CONFIG_FILE
EOF
}

parse_args() {
    while (( $# > 0 )); do
        case "$1" in
            --op)
                (( $# >= 2 )) || {
                    echo "--op requires a value" >&2
                    return 2
                }
                OP_SELECTION=$2
                shift 2
                ;;
            --mode)
                (( $# >= 2 )) || {
                    echo "--mode requires a value" >&2
                    return 2
                }
                RUN_MODE=$2
                shift 2
                ;;
            --build)
                FORCE_BUILD=true
                shift
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                echo "Unknown argument: $1" >&2
                usage >&2
                return 2
                ;;
        esac
    done

    case "${OP_SELECTION}" in
        dispatch) selected_ops=(dispatch) ;;
        combine) selected_ops=(combine) ;;
        both) selected_ops=(dispatch combine) ;;
        *)
            echo "--op must be dispatch, combine, or both: ${OP_SELECTION}" >&2
            return 2
            ;;
    esac
    case "${RUN_MODE}" in
        functional|performance) ;;
        *)
            echo "--mode must be functional or performance: ${RUN_MODE}" >&2
            return 2
            ;;
    esac
}

print_separator() {
    printf '%s\n' '========================================================================'
}

print_stage() {
    printf '\n阶段：%s\n' "$1"
    printf 'timestamp=%s\n\n' "$(date '+%Y-%m-%d_%H:%M:%S')"
}

run_on_host() {
    local host=$1
    shift
    if [[ "${host}" == "${MASTER_HOST}" ]]; then
        "$@"
    else
        ssh "${ssh_options[@]}" "${SSH_USER}@${host}" "$@"
    fi
}

select_profiler_for_host() {
    local host=$1
    case "${host}" in
        141.61.95.*)
            HOST_MSPROF=${MSPROF_95}
            HOST_CANN_ENV_SCRIPT=${CANN_ENV_SCRIPT_95}
            ;;
        *)
            HOST_MSPROF=${MSPROF_94}
            HOST_CANN_ENV_SCRIPT=${CANN_ENV_SCRIPT_94}
            ;;
    esac
}

wait_for_jobs() {
    local label=$1
    local -n pids_ref=$2
    local -n logs_ref=$3
    local index status ret=0
    for index in "${!pids_ref[@]}"; do
        set +e
        wait "${pids_ref[index]}"
        status=$?
        set -e
        if ((status != 0)); then
            printf '%s_FAILED\nindex=%s\nstatus=%d\n\n' "${label}" "${index}" "${status}" >&2
            [[ -r "${logs_ref[index]}" ]] && cat "${logs_ref[index]}" >&2
            ret=1
        fi
    done
    return "${ret}"
}

load_case_config() {
    CASE_OP=$1
    unset TILEXR_EP_CLUSTER_BLOCK_DIM TILEXR_EP_CLUSTER_COMM_QUANT_MODE \
        TILEXR_EP_CLUSTER_MXFP8_FORMAT
    case "${CASE_OP}" in
        dispatch)
            CASE_CONFIG_FILE=${DISPATCH_CONFIG_FILE}
            CASE_REMOTE_CONFIG_FILE="${REMOTE_ROOT}/tests/ep/cluster/$(basename -- "${DISPATCH_CONFIG_FILE}")"
            CASE_NODE_SCRIPT=${DISPATCH_NODE_SCRIPT}
            CASE_KERNEL_NAME=tilexr_ep_dispatch_memory_kernel
            ;;
        combine)
            CASE_CONFIG_FILE=${COMBINE_CONFIG_FILE}
            CASE_REMOTE_CONFIG_FILE="${REMOTE_ROOT}/tests/ep/cluster/$(basename -- "${COMBINE_CONFIG_FILE}")"
            CASE_NODE_SCRIPT=${COMBINE_NODE_SCRIPT}
            CASE_KERNEL_NAME=tilexr_ep_combine_memory_kernel
            ;;
    esac
    [[ -r "${CASE_CONFIG_FILE}" ]] || {
        echo "Missing cluster config: ${CASE_CONFIG_FILE}" >&2
        return 2
    }
    source <(sed 's/\r$//' "${CASE_CONFIG_FILE}")

    CASE_RANK_SIZE=${TILEXR_EP_CLUSTER_RANK_SIZE}
    CASE_BLOCK_DIM=${TILEXR_EP_CLUSTER_BLOCK_DIM:-auto}
    CASE_BS=${TILEXR_EP_CLUSTER_BS}
    CASE_H=${TILEXR_EP_CLUSTER_H}
    CASE_TOPK=${TILEXR_EP_CLUSTER_TOPK}
    CASE_EXPERTS=${TILEXR_EP_CLUSTER_MOE_EXPERT_NUM}
    CASE_DTYPE=${TILEXR_EP_CLUSTER_DTYPE}
    CASE_QUANT_MODE=${TILEXR_EP_CLUSTER_QUANT_MODE}
    CASE_COMM_QUANT_MODE=${TILEXR_EP_CLUSTER_COMM_QUANT_MODE:-0}
    CASE_MXFP8_FORMAT=${TILEXR_EP_CLUSTER_MXFP8_FORMAT:-none}
    CASE_ACTIVE_MASK_TYPE=${TILEXR_EP_CLUSTER_ACTIVE_MASK_TYPE}
    CASE_EXPERT_MODE=${TILEXR_EP_CLUSTER_EXPERT_MODE}
    CASE_EXPERT_SEED=${TILEXR_EP_CLUSTER_EXPERT_SEED}
    CASE_FUNCTIONAL_LOOP_COUNT=${TILEXR_EP_CLUSTER_CORRECTNESS_LOOPS}
    CASE_WARMUP_COUNT=${TILEXR_EP_CLUSTER_WARMUP}
    CASE_ITERATION_COUNT=${TILEXR_EP_CLUSTER_PERF_LOOPS}
    CASE_TIMEOUT_SECONDS=${TILEXR_EP_CLUSTER_TIMEOUT_SECONDS}
}

validate_case_config() {
    local value_name value
    for value_name in CASE_RANK_SIZE CASE_BS CASE_H CASE_TOPK CASE_EXPERTS \
        CASE_FUNCTIONAL_LOOP_COUNT CASE_ITERATION_COUNT CASE_TIMEOUT_SECONDS; do
        value=${!value_name}
        [[ "${value}" =~ ^[1-9][0-9]*$ ]] || {
            echo "${value_name} must be a positive integer: ${value}" >&2
            return 2
        }
    done
    [[ "${CASE_EXPERT_SEED}" =~ ^[0-9]+$ ]] || {
        echo "expert seed must be non-negative: ${CASE_EXPERT_SEED}" >&2
        return 2
    }
    [[ "${CASE_WARMUP_COUNT}" =~ ^[0-9]+$ ]] || {
        echo "warmup count must be a non-negative integer: ${CASE_WARMUP_COUNT}" >&2
        return 2
    }
    if [[ "${RUN_MODE}" == performance ]] &&
        ((CASE_ITERATION_COUNT < LAST_SAMPLE_COUNT)); then
        echo "performance iterations must be at least ${LAST_SAMPLE_COUNT}: ${CASE_ITERATION_COUNT}" >&2
        return 2
    fi
    ((CASE_RANK_SIZE >= LOCAL_NPU_COUNT &&
        CASE_RANK_SIZE % LOCAL_NPU_COUNT == 0 &&
        CASE_TOPK <= CASE_EXPERTS &&
        CASE_EXPERTS % CASE_RANK_SIZE == 0)) || {
        echo "Require rankSize divisible by local NPU count, topK <= experts, and experts divisible by rankSize" >&2
        return 2
    }
    [[ "${CASE_DTYPE}" == fp16 || "${CASE_DTYPE}" == bf16 ]] || {
        echo "dtype must be fp16 or bf16: ${CASE_DTYPE}" >&2
        return 2
    }
    [[ "${CASE_ACTIVE_MASK_TYPE}" == none || "${CASE_ACTIVE_MASK_TYPE}" == token ]] || {
        echo "active mask type must be none or token: ${CASE_ACTIVE_MASK_TYPE}" >&2
        return 2
    }
    [[ "${CASE_EXPERT_MODE}" == uniform || "${CASE_EXPERT_MODE}" == random ]] || {
        echo "expert mode must be uniform or random: ${CASE_EXPERT_MODE}" >&2
        return 2
    }
    if [[ "${CASE_BLOCK_DIM}" != auto ]] && [[ ! "${CASE_BLOCK_DIM}" =~ ^[1-9][0-9]*$ ]]; then
        echo "${CASE_OP} block dim must be auto or a positive integer: ${CASE_BLOCK_DIM}" >&2
        return 2
    fi
    if [[ "${CASE_OP}" == dispatch ]]; then
        if [[ "${CASE_BLOCK_DIM}" != auto ]] &&
            ((CASE_BLOCK_DIM < 2)); then
            echo "dispatch block dim must be auto or an integer greater than one: ${CASE_BLOCK_DIM}" >&2
            return 2
        fi
        [[ "${CASE_QUANT_MODE}" == 0 || "${CASE_QUANT_MODE}" == 4 ]] || {
            echo "dispatch quant mode must be 0 or 4: ${CASE_QUANT_MODE}" >&2
            return 2
        }
        [[ "${CASE_MXFP8_FORMAT}" == e4m3 || "${CASE_MXFP8_FORMAT}" == e5m2 ]] || {
            echo "MXFP8 format must be e4m3 or e5m2: ${CASE_MXFP8_FORMAT}" >&2
            return 2
        }
    else
        [[ "${CASE_QUANT_MODE}" == 0 &&
            ("${CASE_COMM_QUANT_MODE}" == 0 || "${CASE_COMM_QUANT_MODE}" == 3 ||
                "${CASE_COMM_QUANT_MODE}" == 4) ]] || {
            echo "combine requires quantMode=0 and commQuantMode=0, 3, or 4" >&2
            return 2
        }
        [[ "${CASE_MXFP8_FORMAT}" == e4m3 || "${CASE_MXFP8_FORMAT}" == e5m2 ]] || {
            echo "MXFP8 format must be e4m3 or e5m2: ${CASE_MXFP8_FORMAT}" >&2
            return 2
        }
        if [[ "${CASE_COMM_QUANT_MODE}" == 3 && "${CASE_MXFP8_FORMAT}" != e5m2 ]] ||
            [[ "${CASE_COMM_QUANT_MODE}" == 4 && "${CASE_MXFP8_FORMAT}" != e4m3 ]]; then
            echo "commQuantMode and MXFP8 format mismatch: ${CASE_COMM_QUANT_MODE}/${CASE_MXFP8_FORMAT}" >&2
            return 2
        fi
    fi
}

case_label() {
    local quant_label=none core_label=""
    if [[ "${CASE_OP}" == dispatch && "${CASE_QUANT_MODE}" == 4 ]] ||
        [[ "${CASE_OP}" == combine && "${CASE_COMM_QUANT_MODE}" != 0 ]]; then
        quant_label="mxfp8_${CASE_MXFP8_FORMAT}"
    fi
    core_label="_c${CASE_BLOCK_DIM}"
    printf 'memory_%s_%dp_%s_%s%s_bs%d_h%d_k%d_e%d' \
        "${CASE_OP}" "${CASE_RANK_SIZE}" "${CASE_DTYPE}" "${quant_label}" \
        "${core_label}" \
        "${CASE_BS}" "${CASE_H}" "${CASE_TOPK}" "${CASE_EXPERTS}"
}

print_case() {
    print_separator
    printf '%s\n' '当前测试用例'
    printf 'op=%s\n' "${CASE_OP}"
    printf 'mode=%s\n' "${RUN_MODE}"
    printf 'impl=memory\n'
    printf 'ranks=%d\n' "${CASE_RANK_SIZE}"
    printf 'block_dim=%s\n' "${CASE_BLOCK_DIM}"
    printf 'BS=%d\n' "${CASE_BS}"
    printf 'H=%d\n' "${CASE_H}"
    printf 'TopK=%d\n' "${CASE_TOPK}"
    printf 'experts=%d\n' "${CASE_EXPERTS}"
    printf 'dtype=%s\n' "${CASE_DTYPE}"
    printf 'quant_mode=%d\n' "${CASE_QUANT_MODE}"
    if [[ "${CASE_OP}" == dispatch ]]; then
        printf 'mxfp8_format=%s\n' "${CASE_MXFP8_FORMAT}"
    else
        printf 'comm_quant_mode=%d\n' "${CASE_COMM_QUANT_MODE}"
        if [[ "${CASE_COMM_QUANT_MODE}" != 0 ]]; then
            printf 'mxfp8_format=%s\n' "${CASE_MXFP8_FORMAT}"
        fi
    fi
    printf 'active_mask=%s\n' "${CASE_ACTIVE_MASK_TYPE}"
    printf 'expert_mode=%s\n' "${CASE_EXPERT_MODE}"
    if [[ "${RUN_MODE}" == functional ]]; then
        printf 'warmup=0\n'
        printf 'iterations=%d\n' "${CASE_FUNCTIONAL_LOOP_COUNT}"
    else
        printf 'warmup=%d\n' "${CASE_WARMUP_COUNT}"
        printf 'iterations=%d\n' "${CASE_ITERATION_COUNT}"
    fi
    if [[ "${RUN_MODE}" == performance ]]; then
        printf 'result_rounds=%d\n' "${LAST_SAMPLE_COUNT}"
        printf 'server_reduction=mean_of_50_rounds_x_8_cards\n'
        printf 'cluster_reduction=min_max_mean_of_%d_server_averages\n' "${#hosts[@]}"
    fi
    printf 'run_id=%s\n' "${CASE_RUN_ID}"
    print_separator
}

baseline_build_ready() {
    [[ -r "${BUILD_CACHE}" && -x "${DEMO_BIN}" ]] &&
        grep -q '^TILEXR_EP_ENABLE_PROFILING:BOOL=OFF$' "${BUILD_CACHE}"
}

ensure_build() {
    if [[ "${FORCE_BUILD}" != true ]] && baseline_build_ready; then
        print_stage "编译配置检查完成"
        printf 'sdk_profiling=OFF\n'
        printf 'rebuild=no\n'
        return
    fi

    print_stage "代码编译开始"
    printf 'force_build=%s\n' "${FORCE_BUILD}"
    (
        cd "${TILEXR_ROOT}"
        bash tests/ep/build.sh full
    )
    baseline_build_ready || {
        echo "Build completed without the SDK-disabled baseline" >&2
        return 1
    }
    print_stage "代码编译完成"
}

deploy_runtime_scripts() {
    local host index
    declare -a deploy_pids=()
    declare -a deploy_logs=()
    for index in "${!hosts[@]}"; do
        host=${hosts[index]}
        [[ "${host}" == "${MASTER_HOST}" ]] && continue
        deploy_logs[index]="${SESSION_LOG_DIR}/deploy_runtime_${index}.log"
        (
            ssh "${ssh_options[@]}" "${SSH_USER}@${host}" \
                mkdir -p -- "${REMOTE_ROOT}/tests/ep/cluster" "${REMOTE_ROOT}/tools/ep" \
                    "${REMOTE_ROOT}/scripts"
            scp "${ssh_options[@]}" \
                "${script_dir}/run_memory_dispatch_node.sh" \
                "${script_dir}/run_memory_combine_node.sh" \
                "${DISPATCH_CONFIG_FILE}" \
                "${COMBINE_CONFIG_FILE}" \
                "${SSH_USER}@${host}:${REMOTE_ROOT}/tests/ep/cluster/"
            scp "${ssh_options[@]}" \
                "${TILEXR_ROOT}/tools/ep/msprof_kernel_stats.py" \
                "${SSH_USER}@${host}:${REMOTE_ROOT}/tools/ep/"
            scp "${ssh_options[@]}" \
                "${TILEXR_ROOT}/scripts/common_env.sh" \
                "${TILEXR_ROOT}/scripts/common_util.sh" \
                "${SSH_USER}@${host}:${REMOTE_ROOT}/scripts/"
        ) >"${deploy_logs[index]}" 2>&1 &
        deploy_pids[index]=$!
    done
    wait_for_jobs SCRIPT_DEPLOY deploy_pids deploy_logs
}

deploy_artifacts() {
    local host index
    declare -a artifact_pids=()
    declare -a artifact_logs=()
    for index in "${!hosts[@]}"; do
        host=${hosts[index]}
        [[ "${host}" == "${MASTER_HOST}" ]] && continue
        artifact_logs[index]="${SESSION_LOG_DIR}/deploy_artifacts_${index}.log"
        (
            rsync -a -e "${RSYNC_RSH}" "${TILEXR_ROOT}/install/" \
                "${SSH_USER}@${host}:${REMOTE_ROOT}/install/"
            rsync -a -e "${RSYNC_RSH}" "${TILEXR_ROOT}/tests/ep/install/" \
                "${SSH_USER}@${host}:${REMOTE_ROOT}/tests/ep/install/"
        ) >"${artifact_logs[index]}" 2>&1 &
        artifact_pids[index]=$!
    done
    wait_for_jobs ARTIFACT_DEPLOY artifact_pids artifact_logs
}

prepare_case_hosts() {
    local host index node_config
    declare -a prepare_pids=()
    declare -a prepare_logs=()
    for index in "${!hosts[@]}"; do
        host=${hosts[index]}
        node_config=${CASE_REMOTE_CONFIG_FILE}
        [[ "${host}" == "${MASTER_HOST}" ]] && node_config=${CASE_CONFIG_FILE}
        prepare_logs[index]="${CASE_LOG_DIR}/prepare_${index}.log"
        (
            run_on_host "${host}" test -x "${DEMO_BIN}" -a -r "${CASE_NODE_SCRIPT}" \
                -a -r "${node_config}"
            select_profiler_for_host "${host}"
            if [[ -n "${HOST_CANN_ENV_SCRIPT}" ]]; then
                run_on_host "${host}" test -r "${HOST_CANN_ENV_SCRIPT}"
            fi
            if [[ "${RUN_MODE}" == performance ]]; then
                run_on_host "${host}" test -x "${HOST_MSPROF}" -a -x "${PYTHON}" \
                    -a -r "${STATS_SCRIPT}"
                run_on_host "${host}" test ! -e "${CASE_RAW_ROOT}"
                run_on_host "${host}" mkdir -p -- "${CASE_RAW_ROOT}"
            fi
        ) >"${prepare_logs[index]}" 2>&1 &
        prepare_pids[index]=$!
    done
    wait_for_jobs HOST_PREPARE prepare_pids prepare_logs
}

run_performance_host() {
    local host=$1
    local rank_base=$2
    local application profile_command node_config=${CASE_REMOTE_CONFIG_FILE}
    [[ "${host}" == "${MASTER_HOST}" ]] && node_config=${CASE_CONFIG_FILE}
    select_profiler_for_host "${host}"
    application="bash ${CASE_NODE_SCRIPT} ${rank_base} ${CASE_RUN_ID} ${CASE_ITERATION_COUNT} perf ${CASE_WARMUP_COUNT} ${node_config}"
    printf -v profile_command '%q --application=%q --output=%q' \
        "${HOST_MSPROF}" "${application}" "${CASE_RAW_ROOT}"
    if [[ -n "${HOST_CANN_ENV_SCRIPT}" ]]; then
        printf -v profile_command 'source %q >/dev/null 2>&1 && %s' \
            "${HOST_CANN_ENV_SCRIPT}" "${profile_command}"
    fi
    if [[ "${host}" == "${MASTER_HOST}" ]]; then
        bash -lc "${profile_command}"
    else
        ssh "${ssh_options[@]}" "${SSH_USER}@${host}" "${profile_command}"
    fi
}

run_direct_host() {
    local host=$1
    local rank_base=$2
    local loop_count=$3
    local node_mode=$4
    local warmup_count=$5
    local direct_command node_config=${CASE_REMOTE_CONFIG_FILE}
    [[ "${host}" == "${MASTER_HOST}" ]] && node_config=${CASE_CONFIG_FILE}
    select_profiler_for_host "${host}"
    printf -v direct_command '%q %q %q %q %q %q %q %q' \
        bash "${CASE_NODE_SCRIPT}" "${rank_base}" "${CASE_RUN_ID}" \
        "${loop_count}" "${node_mode}" "${warmup_count}" "${node_config}"
    if [[ -n "${HOST_CANN_ENV_SCRIPT}" ]]; then
        printf -v direct_command 'source %q >/dev/null 2>&1 && %s' \
            "${HOST_CANN_ENV_SCRIPT}" "${direct_command}"
    fi
    if [[ "${host}" == "${MASTER_HOST}" ]]; then
        bash -lc "${direct_command}"
    else
        ssh "${ssh_options[@]}" "${SSH_USER}@${host}" "${direct_command}"
    fi
}

run_case_host() {
    local host=$1
    local rank_base=$2
    case "${RUN_MODE}" in
        functional)
            run_direct_host "${host}" "${rank_base}" \
                "${CASE_FUNCTIONAL_LOOP_COUNT}" correctness 0
            ;;
        performance)
            run_performance_host "${host}" "${rank_base}"
            ;;
    esac
}

run_case_all_hosts() {
    local host index rank_base status ret=0
    declare -a run_pids=()
    for index in "${!hosts[@]}"; do
        host=${hosts[index]}
        rank_base=$((index * LOCAL_NPU_COUNT))
        run_case_host "${host}" "${rank_base}" >"${CASE_LOG_DIR}/node_${index}.log" 2>&1 &
        run_pids[index]=$!
    done

    for index in "${!run_pids[@]}"; do
        set +e
        wait "${run_pids[index]}"
        status=$?
        set -e
        if ((status != 0)); then
            printf 'CASE_NODE_FAILED\n' >&2
            printf 'op=%s\n' "${CASE_OP}" >&2
            printf 'host=%s\n' "${hosts[index]}" >&2
            printf 'status=%d\n\n' "${status}" >&2
            tail -n 100 "${CASE_LOG_DIR}/node_${index}.log" >&2 || true
            ret=1
        else
            printf 'CASE_NODE_PASS\n'
            printf 'op=%s\n' "${CASE_OP}"
            printf 'host=%s\n\n' "${hosts[index]}"
        fi
    done
    return "${ret}"
}

summarize_performance_host() {
    local host=$1
    local index=$2
    local rank_base=$((index * LOCAL_NPU_COUNT))
    local output="${CASE_RAW_ROOT}/node_${index}_last50_internal.json"
    run_on_host "${host}" "${PYTHON}" "${STATS_SCRIPT}" "${CASE_RAW_ROOT}" \
        --kernel-name "${CASE_KERNEL_NAME}" \
        --aggregation server-samples \
        --expected-inputs "${LOCAL_NPU_COUNT}" \
        --label "${host}_ranks_${rank_base}_$((rank_base + LOCAL_NPU_COUNT - 1))" \
        --output "${output}"
}

gather_performance_summaries() {
    local host index source target
    declare -a gather_pids=()
    declare -a gather_logs=()
    mkdir -p -- "${CASE_STATS_DIR}"
    for index in "${!hosts[@]}"; do
        host=${hosts[index]}
        source="${CASE_RAW_ROOT}/node_${index}_last50_internal.json"
        target="${CASE_STATS_DIR}/node_${index}_last50_internal.json"
        if [[ "${host}" == "${MASTER_HOST}" ]]; then
            cp -- "${source}" "${target}"
            continue
        fi
        gather_logs[index]="${CASE_LOG_DIR}/gather_stats_${index}.log"
        scp "${ssh_options[@]}" "${SSH_USER}@${host}:${source}" "${target}" \
            >"${gather_logs[index]}" 2>&1 &
        gather_pids[index]=$!
    done
    wait_for_jobs STATS_GATHER gather_pids gather_logs
}

summarize_performance() {
    local index status ret=0 stats_output summary_output
    declare -a stats_pids=()
    declare -a stats_logs=()
    for index in "${!hosts[@]}"; do
        stats_logs[index]="${CASE_LOG_DIR}/node_${index}_stats.log"
        summarize_performance_host "${hosts[index]}" "${index}" \
            >"${stats_logs[index]}" 2>&1 &
        stats_pids[index]=$!
    done
    wait_for_jobs NODE_STATS stats_pids stats_logs || ret=1
    ((ret == 0)) || return "${ret}"

    gather_performance_summaries
    local -a summaries=("${CASE_STATS_DIR}"/node_*_last50_internal.json)
    summary_output="${CASE_STATS_DIR}/${CASE_OP}_${CASE_RANK_SIZE}p_last50.json"
    stats_output=$("${PYTHON}" "${STATS_SCRIPT}" "${summaries[@]}" \
        --kernel-name "${CASE_KERNEL_NAME}" \
        --aggregation server-averages \
        --expected-inputs "${#hosts[@]}" \
        --label "${CASE_RUN_ID}_${#hosts[@]}_servers" \
        --output "${summary_output}")

    perf_min[${CASE_OP}]=$(awk -F= '$1 == "minimum_us" {print $2}' <<<"${stats_output}")
    perf_max[${CASE_OP}]=$(awk -F= '$1 == "maximum_us" {print $2}' <<<"${stats_output}")
    perf_avg[${CASE_OP}]=$(awk -F= '$1 == "average_us" {print $2}' <<<"${stats_output}")
    perf_json[${CASE_OP}]=${summary_output}
    [[ -n "${perf_min[${CASE_OP}]}" && -n "${perf_max[${CASE_OP}]}" &&
        -n "${perf_avg[${CASE_OP}]}" ]] || {
        echo "Failed to parse performance statistics for ${CASE_OP}" >&2
        return 1
    }

    print_separator
    printf '%s\n' '当前算子性能结果'
    printf 'op=%s\n' "${CASE_OP}"
    printf 'minimum_us=%s\n' "${perf_min[${CASE_OP}]}"
    printf 'maximum_us=%s\n' "${perf_max[${CASE_OP}]}"
    printf 'average_us=%s\n' "${perf_avg[${CASE_OP}]}"
    printf 'summary_json=%s\n' "${perf_json[${CASE_OP}]}"
    print_separator
}

run_operator() {
    local op=$1
    load_case_config "${op}"
    validate_case_config
    CASE_RUN_ID="$(case_label)_${RUN_MODE}_w${CASE_WARMUP_COUNT}_i${CASE_ITERATION_COUNT}_${SESSION_TIMESTAMP}"
    if [[ "${RUN_MODE}" == functional ]]; then
        CASE_RUN_ID="$(case_label)_functional_i${CASE_FUNCTIONAL_LOOP_COUNT}_${SESSION_TIMESTAMP}"
    fi
    [[ "${CASE_RUN_ID}" =~ ^[A-Za-z0-9_.-]+$ && ${#CASE_RUN_ID} -le 128 ]] || {
        echo "Generated run_id is invalid or exceeds 128 characters: ${CASE_RUN_ID}" >&2
        return 2
    }
    CASE_LOG_DIR="${SESSION_LOG_DIR}/${CASE_OP}"
    CASE_STATS_DIR="${CASE_LOG_DIR}/msprof_stats"
    CASE_RAW_ROOT="/home/h00580772/test_dir_${CASE_RUN_ID}"
    CASE_RANK_LOG_DIR="${TILEXR_ROOT}/tests/ep/logs/${CASE_RUN_ID}"
    mkdir -p -- "${CASE_LOG_DIR}"

    print_case
    print_stage "服务器准备开始"
    prepare_case_hosts
    print_stage "服务器准备完成"
    print_stage "算子测试启动"
    run_case_all_hosts
    print_stage "算子测试结束"

    case "${RUN_MODE}" in
        functional)
            functional_result[${CASE_OP}]=PASS
            print_separator
            printf '%s\n' '当前算子功能结果'
            printf 'op=%s\n' "${CASE_OP}"
            printf 'status=PASS\n'
            print_separator
            ;;
        performance)
            print_stage "数据汇总分析开始"
            summarize_performance
            print_stage "数据汇总分析完成"
            ;;
    esac
}

print_final_summary() {
    local op
    print_separator
    case "${RUN_MODE}" in
        functional)
            printf '%s\n' '最终功能结果'
            for op in "${selected_ops[@]}"; do
                printf 'op=%s\n' "${op}"
                printf 'status=%s\n\n' "${functional_result[${op}]}"
            done
            ;;
        performance)
            printf '%s\n' '最终性能结果'
            for op in "${selected_ops[@]}"; do
                printf 'op=%s\n' "${op}"
                printf 'minimum_us=%s\n' "${perf_min[${op}]}"
                printf 'maximum_us=%s\n' "${perf_max[${op}]}"
                printf 'average_us=%s\n' "${perf_avg[${op}]}"
                printf 'summary_json=%s\n\n' "${perf_json[${op}]}"
            done
            ;;
    esac
    printf 'session_id=%s\n' "${SESSION_ID}"
    printf 'session_log=%s\n' "${SESSION_LOG_DIR}"
    print_separator
}

main() {
    parse_args "$@"
    for required in "${HOST_FILE}" "${DISPATCH_CONFIG_FILE}" "${COMBINE_CONFIG_FILE}" \
        "${TILEXR_ROOT}/tools/ep/msprof_kernel_stats.py"; do
        [[ -r "${required}" ]] || {
            echo "Missing required file: ${required}" >&2
            exit 2
        }
    done
    local op cluster_rank_size=0 required_host_count
    local -a available_hosts=()
    mapfile -t available_hosts < <(sed -n -E '/^[[:space:]]*($|#)/d; s/[[:space:]]+$//; p' "${HOST_FILE}")
    for op in "${selected_ops[@]}"; do
        load_case_config "${op}"
        if ((cluster_rank_size == 0)); then
            cluster_rank_size=${CASE_RANK_SIZE}
        elif ((cluster_rank_size != CASE_RANK_SIZE)); then
            echo "Selected operators must use the same rank size" >&2
            exit 2
        fi
    done
    ((cluster_rank_size >= LOCAL_NPU_COUNT &&
        cluster_rank_size % LOCAL_NPU_COUNT == 0)) || {
        echo "Rank size must be divisible by local NPU count: ${cluster_rank_size}" >&2
        exit 2
    }
    required_host_count=$((cluster_rank_size / LOCAL_NPU_COUNT))
    (( ${#available_hosts[@]} >= required_host_count )) || {
        echo "Host inventory has ${#available_hosts[@]} nodes, need ${required_host_count}" >&2
        exit 2
    }
    hosts=("${available_hosts[@]:0:required_host_count}")
    [[ " $(hostname -I 2>/dev/null) " == *" ${MASTER_HOST} "* ]] || {
        echo "Run this script on ${MASTER_HOST}" >&2
        exit 2
    }
    [[ "${TILEXR_ROOT}" == "${REMOTE_ROOT}" ]] || {
        echo "Expected repository at ${REMOTE_ROOT}, got ${TILEXR_ROOT}" >&2
        exit 2
    }

    SESSION_TIMESTAMP=$(date +%Y%m%d_%H%M%S)
    SESSION_ID="memory_${OP_SELECTION}_${RUN_MODE}_${SESSION_TIMESTAMP}"
    SESSION_LOG_DIR="${TILEXR_ROOT}/tests/ep/logs/${SESSION_ID}"
    mkdir -p -- "${SESSION_LOG_DIR}"
    exec > >(tee "${SESSION_LOG_DIR}/orchestrator.log") 2>&1

    print_stage "测试流程开始"
    printf 'op_selection=%s\n' "${OP_SELECTION}"
    printf 'mode=%s\n' "${RUN_MODE}"
    print_stage "脚本同步开始"
    deploy_runtime_scripts
    print_stage "脚本同步完成"
    ensure_build
    print_stage "编译产物同步开始"
    deploy_artifacts
    print_stage "编译产物同步完成"

    local op
    for op in "${selected_ops[@]}"; do
        run_operator "${op}"
    done
    print_final_summary
    print_stage "测试流程完成"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
