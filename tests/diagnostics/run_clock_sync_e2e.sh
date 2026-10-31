#!/usr/bin/env bash
set -euo pipefail

bin_dir="${1:-./install/bin}"
output_dir="${2:-clock_sync_e2e_$(date +%Y%m%d_%H%M%S)}"
first_npu="${3:-0}"
rank_size="${TILEXR_CLOCK_SYNC_RANK_SIZE:-4}"
rounds="${TILEXR_CLOCK_SYNC_ROUNDS:-20}"
binary="${bin_dir}/test_tilexr_clock_sync_e2e"

mkdir -p "${output_dir}"
pids=()
cleanup() {
    for pid in "${pids[@]:-}"; do kill "${pid}" 2>/dev/null || true; done
}
trap cleanup INT TERM EXIT
for ((rank = 0; rank < rank_size; ++rank)); do
    "${binary}" --rank-size "${rank_size}" --rank "${rank}" \
        --device-id "$((first_npu + rank))" --rounds "${rounds}" \
        --output-dir "${output_dir}" --comm-mode local \
        >"${output_dir}/rank${rank}.log" 2>&1 &
    pids+=("$!")
done
for pid in "${pids[@]}"; do wait "${pid}"; done
trap - INT TERM EXIT
for ((rank = 0; rank < rank_size; ++rank)); do
    grep -q "PASS clock sync e2e" "${output_dir}/rank${rank}.log"
    test -s "${output_dir}/clock_rank${rank}.json"
done
echo "PASS clock sync e2e: ${output_dir}"
