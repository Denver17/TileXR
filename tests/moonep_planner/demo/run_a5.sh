#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -x "${SCRIPT_DIR}/tilexr_moonep_planner_demo" ]]; then
    TILEXR_INSTALL_PREFIX="$(cd "${SCRIPT_DIR}/.." && pwd)"
else
    TILEXR_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
    TILEXR_INSTALL_PREFIX="${TILEXR_ROOT}/install"
fi
rank_size="${1:-8}"
s="${2:-8192}"
k="${3:-8}"
experts="${4:-896}"
pattern="${5:-biased}"
warmup="${6:-20}"
rounds="${7:-100}"
physical_device_count="${8:-$((rank_size < 8 ? rank_size : 8))}"

if ((physical_device_count <= 0 || rank_size <= 0)); then
    echo "rank_size and physical_device_count must be positive" >&2
    exit 2
fi
ranks_per_device=$(((rank_size + physical_device_count - 1) / physical_device_count))
oversubscribed=false
if ((rank_size > physical_device_count)); then
    oversubscribed=true
fi
if [[ -z "${TILEXR_MOONEP_PLANNER_BLOCK_DIM:-}" ]]; then
    block_dim=$((64 / ranks_per_device))
    if ((block_dim < rank_size)); then
        echo "oversubscription requires a blockDim >= rank_size; set TILEXR_MOONEP_PLANNER_BLOCK_DIM" >&2
        exit 2
    fi
    export TILEXR_MOONEP_PLANNER_BLOCK_DIM="${block_dim}"
fi
export TILEXR_PHYSICAL_DEVICE_COUNT="${physical_device_count}"

export TILEXR_COMM_ID="${TILEXR_COMM_ID:-127.0.0.1:10191}"
export LD_LIBRARY_PATH="${TILEXR_INSTALL_PREFIX}/lib64:${TILEXR_INSTALL_PREFIX}/lib:${LD_LIBRARY_PATH:-}"
binary="${TILEXR_MOONEP_PLANNER_DEMO_BIN:-${TILEXR_INSTALL_PREFIX}/bin/tilexr_moonep_planner_demo}"
log_dir="${SCRIPT_DIR}/../logs/run_$(date +%Y%m%d_%H%M%S)"
mkdir -p "${log_dir}"
export TILEXR_MOONEP_SAMPLE_DIR="${log_dir}"
echo "logical_ranks=${rank_size} physical_devices=${physical_device_count} ranks_per_device=${ranks_per_device} oversubscribed=${oversubscribed} block_dim=${TILEXR_MOONEP_PLANNER_BLOCK_DIM}" |
    tee "${log_dir}/run_metadata.txt"

pids=()
for rank in $(seq 0 $((rank_size - 1))); do
    device=$((rank % physical_device_count))
    "${binary}" "${rank_size}" "${rank}" "${device}" "${s}" "${k}" "${experts}" \
        "${pattern}" "${warmup}" "${rounds}" \
        >"${log_dir}/rank_${rank}.log" 2>&1 &
    pids+=("$!")
done

status=0
for rank in $(seq 0 $((rank_size - 1))); do
    if ! wait "${pids[$rank]}"; then
        status=1
    fi
done

for rank in $(seq 0 $((rank_size - 1))); do
    echo "----- rank ${rank} -----"
    tail -n 80 "${log_dir}/rank_${rank}.log"
done

sample_files=("${log_dir}"/rank_*.samples)
if [[ "${status}" -eq 0 && "${#sample_files[@]}" -eq "${rank_size}" && -e "${sample_files[0]}" ]]; then
    sorted_max="${log_dir}/cross_rank_max_us.sorted"
    awk '
        {
            if (!(FNR in maxima) || $1 > maxima[FNR]) maxima[FNR] = $1;
            if (FNR > count) count = FNR;
        }
        END { for (i = 1; i <= count; ++i) print maxima[i]; }
    ' "${sample_files[@]}" | sort -n >"${sorted_max}"
    sample_count="$(wc -l <"${sorted_max}")"
    p50_line=$(((50 * sample_count + 99) / 100))
    p90_line=$(((90 * sample_count + 99) / 100))
    p99_line=$(((99 * sample_count + 99) / 100))
    p50="$(sed -n "${p50_line}p" "${sorted_max}")"
    p90="$(sed -n "${p90_line}p" "${sorted_max}")"
    p99="$(sed -n "${p99_line}p" "${sorted_max}")"
    average="$(awk '{ sum += $1 } END { if (NR > 0) print sum / NR; else print 0 }' "${sorted_max}")"
    performance_valid=true
    if [[ "${oversubscribed}" == "true" ]]; then
        performance_valid=false
    fi
    echo "cross_rank_max_us samples=${sample_count} avg=${average} p50=${p50} p90=${p90} p99=${p99} logical_ranks=${rank_size} physical_devices=${physical_device_count} oversubscribed=${oversubscribed} performance_valid=${performance_valid}"
fi
echo "logs=${log_dir}"
exit "${status}"
