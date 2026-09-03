#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EP_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TILEXR_ROOT="$(cd "${EP_DIR}/../.." && pwd)"
INSTALL_DIR="${EP_DIR}/install"

: "${ASCEND_HOME_PATH:=}"
: "${LD_LIBRARY_PATH:=}"
source "${TILEXR_ROOT}/scripts/common_env.sh"

soc_name_lower="${TILEXR_SOC_NAME,,}"
case "${soc_name_lower}" in
    ascend950*) default_impl="udma" ;;
    ascend910_9*|ascend910-9*|a5*) default_impl="memory" ;;
    *)
        echo "EP Dispatch/Combine Memory requires an A5-class SoC, got: ${TILEXR_SOC_NAME}" >&2
        exit 2
        ;;
esac

rank_size="${1:-2}"
npu_count="${2:-${rank_size}}"
first_npu="${3:-0}"
loop_count="${4:-${TILEXR_EP_DEMO_LOOP:-150}}"
impl="${5:-${TILEXR_EP_DEMO_IMPL:-${default_impl}}}"
bs="${6:-${TILEXR_EP_DEMO_BS:-4}}"
h="${7:-${TILEXR_EP_DEMO_H:-8}}"
topk="${8:-${TILEXR_EP_DEMO_TOPK:-2}}"
expert_ids="${9:-${TILEXR_EP_DEMO_EXPERT_IDS:-}}"
run_mode="${10:-${TILEXR_EP_DEMO_RUN_MODE:-dispatch_combine}}"
expert_mode="${11:-${TILEXR_EP_DEMO_EXPERT_MODE:-}}"
expert_seed="${12:-${TILEXR_EP_DEMO_EXPERT_SEED:-1}}"
quant_mode="${13:-${TILEXR_EP_DEMO_QUANT_MODE:-0}}"
mxfp8_format="${14:-${TILEXR_EP_DEMO_MXFP8_FORMAT:-e4m3}}"
comm_quant_mode="${15:-${TILEXR_EP_DEMO_COMM_QUANT_MODE:-0}}"

if [[ -z "${expert_mode}" ]]; then
    if [[ -n "${expert_ids}" ]]; then
        expert_mode="explicit"
    else
        expert_mode="uniform"
    fi
fi

for value_name in rank_size npu_count loop_count bs h topk; do
    value="${!value_name}"
    if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
        echo "${value_name} must be a positive integer, got: ${value}" >&2
        exit 2
    fi
done
if [[ ! "${first_npu}" =~ ^[0-9]+$ ]]; then
    echo "first_npu must be a non-negative integer, got: ${first_npu}" >&2
    exit 2
fi
if ((rank_size > npu_count)); then
    echo "rank_size must not exceed npu_count, got rank_size=${rank_size} npu_count=${npu_count}" >&2
    exit 2
fi
if [[ "${impl}" != "udma" && "${impl}" != "memory" ]]; then
    echo "impl must be udma or memory, got: ${impl}" >&2
    exit 2
fi
if [[ "${run_mode}" != "dispatch" && "${run_mode}" != "combine" && "${run_mode}" != "dispatch_combine" ]]; then
    echo "run_mode must be dispatch, combine, or dispatch_combine, got: ${run_mode}" >&2
    exit 2
fi
if [[ "${expert_mode}" != "uniform" && "${expert_mode}" != "random" && "${expert_mode}" != "explicit" ]]; then
    echo "expert_mode must be uniform, random, or explicit, got: ${expert_mode}" >&2
    exit 2
fi
if [[ ! "${expert_seed}" =~ ^[0-9]+$ ]]; then
    echo "expert_seed must be a non-negative integer, got: ${expert_seed}" >&2
    exit 2
fi
if [[ "${quant_mode}" != "0" && "${quant_mode}" != "4" ]]; then
    echo "quant_mode must be 0 or 4 (MXFP8), got: ${quant_mode}" >&2
    exit 2
fi
if [[ "${mxfp8_format}" != "e4m3" && "${mxfp8_format}" != "e5m2" &&
      "${mxfp8_format}" != "fp8_e4m3fn" && "${mxfp8_format}" != "fp8_e5m2" ]]; then
    echo "mxfp8_format must be e4m3 or e5m2, got: ${mxfp8_format}" >&2
    exit 2
fi
if [[ "${quant_mode}" == "4" && "${run_mode}" != "dispatch" ]]; then
    echo "MXFP8 golden generation currently supports dispatch-only mode" >&2
    exit 2
fi
if [[ "${quant_mode}" == "4" && "${impl}" != "memory" ]]; then
    echo "MXFP8 dispatch currently requires the memory backend" >&2
    exit 2
fi
if [[ "${comm_quant_mode}" != "0" && "${comm_quant_mode}" != "3" && "${comm_quant_mode}" != "4" ]]; then
    echo "comm_quant_mode must be 0, 3 (E5M2), or 4 (E4M3), got: ${comm_quant_mode}" >&2
    exit 2
fi
if [[ "${comm_quant_mode}" != "0" && "${run_mode}" == "dispatch" ]]; then
    echo "comm_quant_mode requires combine or dispatch_combine mode" >&2
    exit 2
fi
if [[ "${comm_quant_mode}" != "0" && "${impl}" != "memory" ]]; then
    echo "nonzero comm_quant_mode currently requires the memory backend" >&2
    exit 2
fi
if [[ "${expert_mode}" == "explicit" && -z "${expert_ids}" ]]; then
    echo "expert_ids is required when expert_mode=explicit" >&2
    exit 2
fi
if [[ "${expert_mode}" != "explicit" && -n "${expert_ids}" ]]; then
    echo "expert_ids must be empty unless expert_mode=explicit" >&2
    exit 2
fi

export TILEXR_COMM_ID="${TILEXR_COMM_ID:-127.0.0.1:10077}"
export TILEXR_DEMO_NPUS="${npu_count}"
export TILEXR_DEMO_FIRST_NPU="${first_npu}"
export TILEXR_EP_DEMO_LOOP="${loop_count}"
export TILEXR_EP_DEMO_IMPL="${impl}"
export TILEXR_EP_DEMO_BS="${bs}"
export TILEXR_EP_DEMO_H="${h}"
export TILEXR_EP_DEMO_TOPK="${topk}"
export TILEXR_EP_DEMO_EXPERT_IDS="${expert_ids}"
export TILEXR_EP_DEMO_RUN_MODE="${run_mode}"
export TILEXR_EP_DEMO_EXPERT_MODE="${expert_mode}"
export TILEXR_EP_DEMO_EXPERT_SEED="${expert_seed}"
export TILEXR_EP_DEMO_QUANT_MODE="${quant_mode}"
export TILEXR_EP_DEMO_MXFP8_FORMAT="${mxfp8_format}"
export TILEXR_EP_DEMO_COMM_QUANT_MODE="${comm_quant_mode}"
export LD_LIBRARY_PATH="${TILEXR_ROOT}/install/lib64:${TILEXR_ROOT}/install/lib:"\
"${INSTALL_DIR}/lib64:${INSTALL_DIR}/lib:${LD_LIBRARY_PATH:-}"

bin="${INSTALL_DIR}/bin/tilexr_ep_dispatch_demo"
if [[ ! -x "${bin}" ]]; then
    echo "Missing demo binary: ${bin}" >&2
    echo "Build it with: cd ${EP_DIR} && bash build.sh full" >&2
    exit 1
fi

log_dir="${EP_DIR}/logs/tilexr_ep_dispatch_demo_$(date +%Y%m%d_%H%M%S)"
mkdir -p "${log_dir}"

pids=()
logs=()
for ((rank = 0; rank < rank_size; ++rank)); do
    log="${log_dir}/rank_${rank}.log"
    logs+=("${log}")
    (
        export RANK="${rank}"
        export RANK_SIZE="${rank_size}"
        exec "${bin}" "${rank_size}" "${rank}" "${npu_count}" "${first_npu}"
    ) >"${log}" 2>&1 &
    pids+=("$!")
done

ret=0
for pid in "${pids[@]}"; do
    set +e
    wait "${pid}"
    status=$?
    set -e
    if [[ "${status}" -ne 0 && "${ret}" -eq 0 ]]; then
        ret="${status}"
    fi
done

for log in "${logs[@]}"; do
    echo "===== ${log} (last 120 lines) ====="
    tail -n 120 "${log}" || true
done

exit "${ret}"
