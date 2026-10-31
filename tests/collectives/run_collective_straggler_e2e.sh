#!/usr/bin/env bash
set -u

bin_dir="${1:-./install/bin}"
output_dir="${2:-collective_straggler_e2e_$(date +%Y%m%d_%H%M%S)}"
first_npu="${3:-0}"
binary="${bin_dir}/test_collective_straggler_e2e"
timeout_sec="${TILEXR_STRAGGLER_E2E_TIMEOUT_SEC:-600}"

if [[ ! -x "${binary}" ]]; then
  echo "ERROR: ${binary} is not executable" >&2
  exit 1
fi
if [[ ! "${timeout_sec}" =~ ^[0-9]+$ || "${timeout_sec}" -le 0 ]]; then
  echo "ERROR: TILEXR_STRAGGLER_E2E_TIMEOUT_SEC must be a positive integer" >&2
  exit 1
fi
if [[ ! "${first_npu}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: first_npu must be a non-negative integer" >&2
  exit 1
fi

mkdir -p "${output_dir}"
pids=()
cleanup() {
  local pid
  for pid in "${pids[@]}"; do
    kill "${pid}" 2>/dev/null || true
  done
  for pid in "${pids[@]}"; do
    wait "${pid}" 2>/dev/null || true
  done
}
show_logs() {
  local rank
  for rank in 0 1 2 3; do
    echo "===== ${output_dir}/rank${rank}.log =====" >&2
    tail -n 120 "${output_dir}/rank${rank}.log" 2>/dev/null >&2 || true
  done
}
trap 'cleanup; show_logs; exit 130' INT TERM

for rank in 0 1 2 3; do
  "${binary}" --rank-size 4 --rank "${rank}" --first-npu "${first_npu}" \
    --clock-rounds 20 --measured-rounds 3 \
    --small-count 262144 --large-count 4194304 --delay-cycles 5000000 \
    --output-dir "${output_dir}" >"${output_dir}/rank${rank}.log" 2>&1 &
  pids+=("$!")
done

sleep "${timeout_sec}" &
watchdog="$!"
completed=0
while (( completed < 4 )); do
  if wait -n; then
    if ! kill -0 "${watchdog}" 2>/dev/null; then
      echo "ERROR: straggler e2e timed out after ${timeout_sec}s" >&2
      cleanup
      show_logs
      exit 124
    fi
    completed=$((completed + 1))
  else
    status="$?"
    echo "ERROR: straggler rank failed with status ${status}" >&2
    kill "${watchdog}" 2>/dev/null || true
    wait "${watchdog}" 2>/dev/null || true
    cleanup
    show_logs
    exit 1
  fi
done

kill "${watchdog}" 2>/dev/null || true
wait "${watchdog}" 2>/dev/null || true
trap - INT TERM

for rank in 0 1 2 3; do
  grep -q "PASS straggler e2e" "${output_dir}/rank${rank}.log" || {
    show_logs
    exit 1
  }
done
test -s "${output_dir}/summary.json"
echo "PASS straggler e2e: ${output_dir}/summary.json"
