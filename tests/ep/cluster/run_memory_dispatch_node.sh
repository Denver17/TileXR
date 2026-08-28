#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
readonly CONFIG_FILE="${script_dir}/memory_dispatch_64p.env"
[[ -r "${CONFIG_FILE}" ]] || {
    echo "Missing cluster config: ${CONFIG_FILE}" >&2
    exit 2
}
source "${CONFIG_FILE}"

if (( $# < 4 || $# > 5 )); then
    echo "Usage: $0 rank_base run_id loop_count correctness|perf|profile [warmup_count]" >&2
    exit 2
fi

readonly RANK_BASE=$1
readonly RUN_ID=$2
readonly LOOP_COUNT=$3
readonly TEST_MODE=$4
readonly RANK_SIZE=${TILEXR_EP_CLUSTER_RANK_SIZE}
readonly LOCAL_NPU_COUNT=8
readonly COMM_ID=${TILEXR_EP_CLUSTER_COMM_ID}
readonly BARRIER_ADDR=${TILEXR_EP_CLUSTER_BARRIER_ADDR}
readonly BS=${TILEXR_EP_CLUSTER_BS}
readonly H=${TILEXR_EP_CLUSTER_H}
readonly TOPK=${TILEXR_EP_CLUSTER_TOPK}
readonly MOE_EXPERT_NUM=${TILEXR_EP_CLUSTER_MOE_EXPERT_NUM}
readonly DTYPE=${TILEXR_EP_CLUSTER_DTYPE}
readonly QUANT_MODE=${TILEXR_EP_CLUSTER_QUANT_MODE}
readonly MXFP8_FORMAT=${TILEXR_EP_CLUSTER_MXFP8_FORMAT}
readonly ACTIVE_MASK_TYPE=${TILEXR_EP_CLUSTER_ACTIVE_MASK_TYPE}
readonly EXPERT_MODE=${TILEXR_EP_CLUSTER_EXPERT_MODE}
readonly EXPERT_SEED=${TILEXR_EP_CLUSTER_EXPERT_SEED}
readonly WARMUP_COUNT=${5:-${TILEXR_EP_CLUSTER_WARMUP}}
readonly RANK_TIMEOUT_SECONDS=${TILEXR_EP_CLUSTER_TIMEOUT_SECONDS}

[[ "${RUN_ID}" =~ ^[A-Za-z0-9_.-]{1,128}$ ]] || {
    echo "run_id must contain 1-128 letters, digits, dot, underscore, or hyphen: ${RUN_ID}" >&2
    exit 2
}
[[ "${RANK_BASE}" =~ ^[0-9]+$ ]] && ((RANK_BASE % LOCAL_NPU_COUNT == 0)) || {
    echo "rank_base must be a multiple of ${LOCAL_NPU_COUNT}: ${RANK_BASE}" >&2
    exit 2
}
((RANK_SIZE >= LOCAL_NPU_COUNT && RANK_BASE + LOCAL_NPU_COUNT <= RANK_SIZE)) || {
    echo "rank range exceeds rank_size: rank_base=${RANK_BASE} rank_size=${RANK_SIZE}" >&2
    exit 2
}
[[ "${LOOP_COUNT}" =~ ^[1-9][0-9]*$ ]] || {
    echo "loop_count must be a positive integer: ${LOOP_COUNT}" >&2
    exit 2
}
[[ "${TEST_MODE}" == correctness || "${TEST_MODE}" == perf || "${TEST_MODE}" == profile ]] || {
    echo "test_mode must be correctness, perf, or profile: ${TEST_MODE}" >&2
    exit 2
}
for value_name in RANK_SIZE BS H TOPK MOE_EXPERT_NUM RANK_TIMEOUT_SECONDS; do
    value=${!value_name}
    [[ "${value}" =~ ^[1-9][0-9]*$ ]] || {
        echo "${value_name} must be a positive integer: ${value}" >&2
        exit 2
    }
done
[[ "${WARMUP_COUNT}" =~ ^[0-9]+$ ]] || {
    echo "warmup count must be a non-negative integer: ${WARMUP_COUNT}" >&2
    exit 2
}
((TOPK <= MOE_EXPERT_NUM && MOE_EXPERT_NUM % RANK_SIZE == 0)) || {
    echo "Require topK <= moeExpertNum and moeExpertNum divisible by rankSize" >&2
    exit 2
}
[[ "${DTYPE}" == fp16 || "${DTYPE}" == bf16 ]] || {
    echo "dtype must be fp16 or bf16: ${DTYPE}" >&2
    exit 2
}
[[ "${QUANT_MODE}" == 0 || "${QUANT_MODE}" == 4 ]] || {
    echo "dispatch quant mode must be 0 or 4: ${QUANT_MODE}" >&2
    exit 2
}
[[ "${MXFP8_FORMAT}" == e4m3 || "${MXFP8_FORMAT}" == e5m2 ]] || {
    echo "MXFP8 format must be e4m3 or e5m2: ${MXFP8_FORMAT}" >&2
    exit 2
}
[[ "${ACTIVE_MASK_TYPE}" == none || "${ACTIVE_MASK_TYPE}" == token ]] || {
    echo "active mask type must be none or token: ${ACTIVE_MASK_TYPE}" >&2
    exit 2
}
[[ "${EXPERT_MODE}" == uniform || "${EXPERT_MODE}" == random ]] || {
    echo "cluster expert mode must be uniform or random: ${EXPERT_MODE}" >&2
    exit 2
}
[[ "${EXPERT_SEED}" =~ ^[0-9]+$ ]] || {
    echo "expert seed must be a non-negative integer: ${EXPERT_SEED}" >&2
    exit 2
}
readonly TILEXR_ROOT=$(cd -- "${script_dir}/../../.." && pwd)
readonly BIN="${TILEXR_ROOT}/tests/ep/install/bin/tilexr_ep_dispatch_demo"
readonly LOG_DIR="${TILEXR_ROOT}/tests/ep/logs/${RUN_ID}"

: "${ASCEND_HOME_PATH:=}"
: "${LD_LIBRARY_PATH:=}"
source "${TILEXR_ROOT}/scripts/common_env.sh"

[[ -x "${BIN}" ]] || {
    echo "Missing demo binary: ${BIN}" >&2
    exit 1
}
mkdir -p -- "${LOG_DIR}"

export TILEXR_COMM_ID="${COMM_ID}"
export TILEXR_DEMO_BARRIER_ADDR="${BARRIER_ADDR}"
export TILEXR_EP_DEMO_IMPL=memory
export TILEXR_EP_DEMO_RUN_MODE=dispatch
export TILEXR_EP_DEMO_LOOP="${LOOP_COUNT}"
if [[ "${TEST_MODE}" == perf || "${TEST_MODE}" == profile ]]; then
    export TILEXR_EP_DEMO_PERF=1
    export TILEXR_EP_DEMO_WARMUP="${WARMUP_COUNT}"
else
    unset TILEXR_EP_DEMO_PERF TILEXR_EP_DEMO_WARMUP
fi
if [[ "${TEST_MODE}" == profile ]]; then
    export TILEXR_EP_DEMO_DISPATCH_MEMORY_PROFILE=1
    export TILEXR_EP_DISPATCH_MEMORY_PROFILE_RUN_ID="${RUN_ID}"
else
    unset TILEXR_EP_DEMO_DISPATCH_MEMORY_PROFILE TILEXR_EP_DISPATCH_MEMORY_PROFILE_RUN_ID
fi
export TILEXR_EP_DEMO_BS="${BS}"
export TILEXR_EP_DEMO_H="${H}"
export TILEXR_EP_DEMO_TOPK="${TOPK}"
export TILEXR_EP_DEMO_MOE_EXPERT_NUM="${MOE_EXPERT_NUM}"
export TILEXR_EP_DEMO_ACTIVE_MASK_TYPE="${ACTIVE_MASK_TYPE}"
export TILEXR_EP_DEMO_EXPERT_MODE="${EXPERT_MODE}"
export TILEXR_EP_DEMO_EXPERT_SEED="${EXPERT_SEED}"
export TILEXR_EP_DEMO_QUANT_MODE="${QUANT_MODE}"
export TILEXR_EP_DEMO_MXFP8_FORMAT="${MXFP8_FORMAT}"
export TILEXR_EP_DEMO_DTYPE="${DTYPE}"
export LD_LIBRARY_PATH="${TILEXR_ROOT}/install/lib64:${TILEXR_ROOT}/install/lib:"\
"${TILEXR_ROOT}/tests/ep/install/lib64:${TILEXR_ROOT}/tests/ep/install/lib:${LD_LIBRARY_PATH:-}"

declare -a pids=()
declare -a ranks=()
for ((local_rank = 0; local_rank < LOCAL_NPU_COUNT; ++local_rank)); do
    rank=$((RANK_BASE + local_rank))
    log="${LOG_DIR}/rank_${rank}.log"
    (
        exec timeout --signal=TERM --kill-after=10s "${RANK_TIMEOUT_SECONDS}" \
            "${BIN}" "${RANK_SIZE}" "${rank}" "${LOCAL_NPU_COUNT}" 0
    ) >"${log}" 2>&1 &
    pids+=("$!")
    ranks+=("${rank}")
done

ret=0
for index in "${!pids[@]}"; do
    set +e
    wait "${pids[index]}"
    status=$?
    set -e
    rank=${ranks[index]}
    log="${LOG_DIR}/rank_${rank}.log"
    if ((status != 0)); then
        printf 'RANK_FAILED rank=%d status=%d log=%s\n' "${rank}" "${status}" "${log}" >&2
        tail -n 80 "${log}" >&2 || true
        ret=1
    elif ! grep -q "rank ${rank} dispatch validation PASS" "${log}"; then
        printf 'RANK_MISSING_PASS rank=%d log=%s\n' "${rank}" "${log}" >&2
        tail -n 80 "${log}" >&2 || true
        ret=1
    elif [[ "${TEST_MODE}" == profile ]] &&
        ! grep -q "^EP_DISPATCH_MEMORY_PROFILE run_id=${RUN_ID} .* rank=${rank} " "${log}"; then
        printf 'RANK_MISSING_PROFILE rank=%d run_id=%s log=%s\n' \
            "${rank}" "${RUN_ID}" "${log}" >&2
        tail -n 80 "${log}" >&2 || true
        ret=1
    fi
done

if ((ret == 0)); then
    printf 'NODE_VALIDATION_PASS host=%s rank_base=%d ranks=%d run_id=%s\n' \
        "$(hostname)" "${RANK_BASE}" "${LOCAL_NPU_COUNT}" "${RUN_ID}"
fi
exit "${ret}"
