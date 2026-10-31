#!/usr/bin/env bash
set -euo pipefail

if (( $# != 9 )); then
  echo "Usage: $0 ROOT INSTALL_PREFIX OUTPUT_DIR RANK_SIZE NODE_INDEX RANKS_PER_HOST ROUNDS COMM_ID REQUIRED_IDLE_DEVICES" >&2
  exit 2
fi

root_dir="$1"
install_prefix="$2"
output_dir="$3"
rank_size="$4"
node_index="$5"
ranks_per_host="$6"
rounds="$7"
comm_id="$8"
required_idle_devices="$9"
binary="${install_prefix}/bin/test_tilexr_clock_sync_e2e"

for value in "${rank_size}" "${node_index}" "${ranks_per_host}" "${rounds}" \
             "${required_idle_devices}"; do
  if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
    echo "ERROR: rank and round arguments must be non-negative integers" >&2
    exit 2
  fi
done
if (( rank_size == 0 || ranks_per_host == 0 || rounds == 0 )); then
  echo "ERROR: rank size, ranks per host, and rounds must be positive" >&2
  exit 2
fi
if [[ ! -f "${binary}" ]]; then
  echo "ERROR: ${binary} is missing" >&2
  exit 1
fi
chmod +x "${binary}"
if [[ ! -f "${root_dir}/scripts/common_env.sh" ]]; then
  echo "ERROR: ${root_dir}/scripts/common_env.sh is missing" >&2
  exit 1
fi

cd "${root_dir}"
source scripts/common_env.sh
runtime_home=
for candidate in "${ASCEND_HOME_PATH:-}" /home/pkg/b020/cann \
                 /usr/local/Ascend/ascend-toolkit/latest; do
  if [[ -n "${candidate}" && -d "${candidate}/${TILEXR_OS_ARCH}-linux/lib64" ]]; then
    runtime_home="${candidate}"
    break
  fi
done
if [[ -z "${runtime_home}" ]]; then
  echo "ERROR: no usable CANN runtime lib64 directory was found" >&2
  exit 1
fi
export ASCEND_HOME_PATH="${runtime_home}"
export TILEXR_COMM_ID="${comm_id}"
export LD_LIBRARY_PATH="${install_prefix}/lib64:${install_prefix}/lib:${runtime_home}/${TILEXR_OS_ARCH}-linux/lib64:${LD_LIBRARY_PATH:-}"
mkdir -p "${output_dir}"
if (( required_idle_devices > 0 )); then
  idle_probe="${root_dir}/tools/moonep/mindspeed/probe_idle.sh"
  if [[ ! -f "${idle_probe}" ]]; then
    echo "ERROR: idle probe ${idle_probe} is missing" >&2
    exit 1
  fi
  idle=$(bash "${idle_probe}" --devices "${required_idle_devices}" \
    --log "${output_dir}/node${node_index}_idle.txt")
  if [[ "${idle}" != "${required_idle_devices}" ]]; then
    echo "ERROR: node ${node_index} is not confirmed ${required_idle_devices}/${required_idle_devices} idle" >&2
    exit 1
  fi
fi
ldd "${binary}" >"${output_dir}/node${node_index}_ldd.txt"
if grep -q "not found" "${output_dir}/node${node_index}_ldd.txt"; then
  cat "${output_dir}/node${node_index}_ldd.txt" >&2
  exit 1
fi

{
  echo "host=$(hostname)"
  echo "node_index=${node_index}"
  echo "rank_size=${rank_size}"
  echo "ranks_per_host=${ranks_per_host}"
  echo "rounds=${rounds}"
  echo "required_idle_devices=${required_idle_devices}"
  echo "comm_id=${comm_id}"
  echo "soc_name=${TILEXR_SOC_NAME:-unknown}"
  echo "ascend_home_path=$(readlink -f "${ASCEND_HOME_PATH}")"
  sha256sum "${binary}" "${install_prefix}/lib64/libtilexr-diagnostics.so" \
    "${install_prefix}/lib64/libtile-comm.so" \
    "${install_prefix}/lib64/tilexr/kernels/tilexr_diagnostics_op.o"
} >"${output_dir}/node${node_index}_provenance.txt"

pids=()
ranks=()
cleanup() {
  local pid
  for pid in "${pids[@]:-}"; do
    kill "${pid}" 2>/dev/null || true
  done
  for pid in "${pids[@]:-}"; do
    wait "${pid}" 2>/dev/null || true
  done
}
trap cleanup HUP INT TERM EXIT

first_rank=$((node_index * ranks_per_host))
for ((local_rank = 0; local_rank < ranks_per_host; ++local_rank)); do
  rank=$((first_rank + local_rank))
  if (( rank >= rank_size )); then
    break
  fi
  "${binary}" --rank-size "${rank_size}" --rank "${rank}" \
    --device-id "${local_rank}" --rounds "${rounds}" \
    --output-dir "${output_dir}" --comm-mode socket \
    >"${output_dir}/rank${rank}.log" 2>&1 &
  pids+=("$!")
  ranks+=("${rank}")
done

status=0
for pid in "${pids[@]}"; do
  if ! wait "${pid}"; then
    status=1
  fi
done
trap - HUP INT TERM EXIT

for rank in "${ranks[@]}"; do
  if ! grep -q "PASS clock sync e2e" "${output_dir}/rank${rank}.log" ||
      [[ ! -s "${output_dir}/clock_rank${rank}.json" ]]; then
    status=1
  fi
done
if (( status != 0 )); then
  for rank in "${ranks[@]}"; do
    echo "===== rank ${rank} =====" >&2
    tail -n 120 "${output_dir}/rank${rank}.log" >&2 || true
  done
  exit 1
fi

echo "PASS clock sync worker: node=${node_index} ranks=${ranks[*]}"
