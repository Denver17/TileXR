#!/usr/bin/env bash
# 构建或启动 Combine V2 性能探针，向 tools/moonep 和 tests/moonep_combine_v2 传递运行参数。
set -euo pipefail

HOSTFILE=""
INSTALL_DIR=""
CANN_PATH="${ASCEND_HOME_PATH:-/usr/local/Ascend/ascend-toolkit/latest}"
SSH_USER="$(id -un)"
BS=""
BS_LIST=""
WARMUP=20
ITERATIONS=80
EXPERTS=64
HIDDEN_SIZE=3584
TOPK=16
COMM_DOMAIN=141
COMM_ID=""
TIMEOUT_SECONDS=600
LOG_FILE=""
REDUCE_HIDDEN=0
FUSED_WEIGHT=0

usage() {
    cat <<'EOF'
Usage: bash tools/moonep/run_combine_v2_perf_multihost.sh --hostfile PATH --install-dir PATH [options]

Options:
  --bs N                 Run one batch size (default: benchmark default 128)
  --bs-list N[,N...]     Run multiple BS points after one TileXR initialization
  --warmup N             Warmup launches per BS (default: 20)
  --iterations N         Timed launches per BS (default: 80)
  --experts N            Total expert count (default: 64)
  --hidden-size N        Hidden size H (default: 3584)
  --topk N               Router TopK K (default: 16)
  --comm-domain N        Shared-QP domain (default: 141)
  --comm-id IP:PORT      Bootstrap address (default: first host:10067)
  --cann-path PATH       CANN root
  --ssh-user USER        SSH user for rank launch (default: current user)
  --timeout N            Per-rank timeout (default: 600)
  --log-file PATH        Controller log path on the primary host
  --skip-iteration-barriers
                         Deprecated no-op; launches are always continuous
  --reduce-hidden        Include BF16 TopK hidden reduction in the kernel
  --fused-weight         Transfer and validate FP32 route weights in the same launch
  --help                 Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --hostfile) HOSTFILE="$2"; shift 2 ;;
        --install-dir) INSTALL_DIR="$2"; shift 2 ;;
        --bs) BS="$2"; shift 2 ;;
        --bs-list) BS_LIST="$2"; shift 2 ;;
        --warmup) WARMUP="$2"; shift 2 ;;
        --iterations) ITERATIONS="$2"; shift 2 ;;
        --experts) EXPERTS="$2"; shift 2 ;;
        --hidden-size) HIDDEN_SIZE="$2"; shift 2 ;;
        --topk) TOPK="$2"; shift 2 ;;
        --comm-domain) COMM_DOMAIN="$2"; shift 2 ;;
        --comm-id) COMM_ID="$2"; shift 2 ;;
        --cann-path) CANN_PATH="$2"; shift 2 ;;
        --ssh-user) SSH_USER="$2"; shift 2 ;;
        --timeout) TIMEOUT_SECONDS="$2"; shift 2 ;;
        --log-file) LOG_FILE="$2"; shift 2 ;;
        --skip-iteration-barriers) shift ;;
        --reduce-hidden) REDUCE_HIDDEN=1; shift ;;
        --fused-weight) FUSED_WEIGHT=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "${HOSTFILE}" || -z "${INSTALL_DIR}" ]]; then
    usage >&2
    exit 2
fi
if [[ -n "${BS}" && -n "${BS_LIST}" ]]; then
    echo "--bs and --bs-list are mutually exclusive" >&2
    exit 2
fi
if [[ ! "${WARMUP}" =~ ^[0-9]+$ ]]; then
    echo "--warmup must be a non-negative integer" >&2
    exit 2
fi
for value in "${ITERATIONS}" "${EXPERTS}" "${HIDDEN_SIZE}" "${TOPK}" "${COMM_DOMAIN}" \
    "${TIMEOUT_SECONDS}"; do
    if [[ ! "${value}" =~ ^[1-9][0-9]*$ ]]; then
        echo "iterations, domains, and timeouts must be positive integers" >&2
        exit 2
    fi
done
if [[ ! -f "${HOSTFILE}" ||
      ! -x "${INSTALL_DIR}/bin/tilexr_moonep_combine_v2_perf" ]]; then
    echo "hostfile or staged benchmark is missing" >&2
    exit 1
fi
if [[ "${INSTALL_DIR}" != /* || "${CANN_PATH}" != /* ||
      "${INSTALL_DIR}" == *"'"* || "${CANN_PATH}" == *"'"* ]]; then
    echo "install and CANN paths must be absolute and cannot contain single quotes" >&2
    exit 2
fi
if ! help wait 2>&1 | grep -q -- '-n'; then
    echo "the launcher requires Bash 4.3 or newer for wait -n" >&2
    exit 1
fi

mapfile -t host_entries < <(awk '
    /^[[:space:]]*($|#)/ { next }
    { gsub(/[[:space:]]/, "", $0); print $0 }
' "${HOSTFILE}")
if [[ ${#host_entries[@]} -eq 0 ]]; then
    echo "hostfile has no hosts: ${HOSTFILE}" >&2
    exit 1
fi

ranks=0
hosts=()
slots_by_host=()
declare -A seen_hosts=()
for entry in "${host_entries[@]}"; do
    host="${entry%%:*}"
    slots="${entry#*:}"
    if [[ -z "${host}" || "${host}" == "${slots}" ||
          ! "${slots}" =~ ^[1-8]$ || -n "${seen_hosts[${host}]:-}" ]]; then
        echo "invalid or duplicate hostfile entry: ${entry}" >&2
        exit 2
    fi
    seen_hosts["${host}"]=1
    hosts+=("${host}")
    slots_by_host+=("${slots}")
    ranks=$((ranks + slots))
done
case "${ranks}" in
    2|3|4|5|6|7|8|16|32|64|128) ;;
    *)
        echo "unsupported Combine V2 world size ${ranks}; expected 2-8, 16, 32, 64, or 128" >&2
        exit 2
        ;;
esac
if (( EXPERTS % ranks != 0 )); then
    echo "expert count ${EXPERTS} must be divisible by world size ${ranks}" >&2
    exit 2
fi

requested_bs="${BS_LIST:-${BS:-128}}"
if [[ ! "${requested_bs}" =~ ^[1-9][0-9]*(,[1-9][0-9]*)*$ ]]; then
    echo "batch sizes must be a comma-separated list of positive integers" >&2
    exit 2
fi
IFS=',' read -r -a requested_batch_sizes <<<"${requested_bs}"
for batch_size in "${requested_batch_sizes[@]}"; do
    if (( batch_size % ranks != 0 )); then
        echo "batch size ${batch_size} must be divisible by world size ${ranks}" >&2
        exit 2
    fi
done

if [[ -z "${COMM_ID}" ]]; then
    COMM_ID="${hosts[0]}:10067"
fi
if [[ ! "${COMM_ID}" =~ ^([^:]+):([1-9][0-9]*)$ ]]; then
    echo "--comm-id must use the first host and a valid TCP port" >&2
    exit 2
fi
comm_host="${BASH_REMATCH[1]}"
comm_port="${BASH_REMATCH[2]}"
if [[ "${comm_host}" != "${hosts[0]}" || ${comm_port} -gt 65535 ]]; then
    echo "--comm-id must use the first host and a valid TCP port" >&2
    exit 2
fi
barrier_port=$((comm_port + 97))
if (( barrier_port > 65535 )); then
    barrier_port=$((comm_port - 97))
fi
if (( barrier_port <= 0 )); then
    echo "cannot derive a valid barrier port from ${comm_port}" >&2
    exit 2
fi
BARRIER_ID="${hosts[0]}:${barrier_port}"

if [[ -z "${LOG_FILE}" ]]; then
    run_root="$(cd "${INSTALL_DIR}/.." && pwd)"
    LOG_FILE="${run_root}/logs/combine_v2_${ranks}p_$(date +%Y%m%d_%H%M%S).log"
fi
if [[ "${LOG_FILE}" != /* || "${LOG_FILE}" == *"'"* ]]; then
    echo "--log-file must be an absolute path without single quotes" >&2
    exit 2
fi
mkdir -p "$(dirname "${LOG_FILE}")"
rank_log_dir="${LOG_FILE}.ranks"
mkdir -p "${rank_log_dir}"

ssh_options=(-o BatchMode=yes -o ConnectTimeout=10)
for host in "${hosts[@]}"; do
    if ! ssh "${ssh_options[@]}" "${SSH_USER}@${host}" \
        "test -x '${INSTALL_DIR}/bin/tilexr_moonep_combine_v2_perf' && test -d '${CANN_PATH}/aarch64-linux' && command -v timeout >/dev/null && command -v ss >/dev/null"; then
        echo "remote runtime validation failed on ${host}" >&2
        exit 1
    fi
done
if ssh "${ssh_options[@]}" "${SSH_USER}@${hosts[0]}" \
    "ss -ltnH | awk '{print \$4}' | grep -Eq ':(${comm_port}|${barrier_port})\$'"; then
    echo "bootstrap or barrier port is already listening on ${hosts[0]} (${comm_port}, ${barrier_port})" >&2
    exit 1
fi

benchmark_args=(
    --warmup "${WARMUP}"
    --iterations "${ITERATIONS}"
    --experts "${EXPERTS}"
    --hidden-size "${HIDDEN_SIZE}"
    --topk "${TOPK}"
    --comm-domain "${COMM_DOMAIN}"
)
if [[ -n "${BS}" ]]; then
    benchmark_args+=(--bs "${BS}")
elif [[ -n "${BS_LIST}" ]]; then
    benchmark_args+=(--bs-list "${BS_LIST}")
fi
if (( REDUCE_HIDDEN )); then
    benchmark_args+=(--reduce-hidden)
fi
if (( FUSED_WEIGHT )); then
    benchmark_args+=(--fused-weight)
fi
job_id="combine_v2_${ranks}p_$(date +%Y%m%d_%H%M%S)_$$"
remote_job_dir="$(cd "${INSTALL_DIR}/.." && pwd)/logs/.combine_v2_jobs/${job_id}"
ssh_control_dir=$(mktemp -d "${TMPDIR:-/tmp}/tilexr-combine-v2-ssh.XXXXXX")
rank_ssh_options=(
    "${ssh_options[@]}"
    -o ControlMaster=auto
    -o ControlPersist=60
    -o "ControlPath=${ssh_control_dir}/%C"
)
ssh_masters_active=0

close_rank_ssh_masters() {
    local host
    if (( ssh_masters_active )); then
        for host in "${hosts[@]}"; do
            ssh "${rank_ssh_options[@]}" -O exit "${SSH_USER}@${host}" \
                >/dev/null 2>&1 || true
        done
    fi
    ssh_masters_active=0
    rmdir "${ssh_control_dir}" 2>/dev/null || true
}

ssh_masters_active=1
for host in "${hosts[@]}"; do
    if ! ssh "${rank_ssh_options[@]}" -MNf "${SSH_USER}@${host}"; then
        echo "failed to establish rank SSH control connection to ${host}" >&2
        close_rank_ssh_masters
        exit 1
    fi
done
rank_hosts=()
rank_devices=()
rank_pidfiles=()
global_rank=0
for host_index in "${!hosts[@]}"; do
    host="${hosts[${host_index}]}"
    slots="${slots_by_host[${host_index}]}"
    for ((local_rank = 0; local_rank < slots; ++local_rank)); do
        rank_hosts[${global_rank}]="${host}"
        rank_devices[${global_rank}]="${local_rank}"
        rank_pidfiles[${global_rank}]="${remote_job_dir}/rank_${global_rank}.pid"
        echo "RANK_MAP rank=${global_rank} host=${host} local_rank=${local_rank} device=${local_rank}" | \
            tee -a "${LOG_FILE}"
        global_rank=$((global_rank + 1))
    done
done

remote_rank_script=$(cat <<'REMOTE_SCRIPT'
set -euo pipefail
job_id=$1
pidfile=$2
rank_timeout=$3
install_dir=$4
cann_path=$5
comm_id=$6
barrier_id=$7
rank=$8
world=$9
device=${10}
shift 10
mkdir -p "$(dirname "${pidfile}")"
printf '%s %s\n' "$$" "${job_id}" >"${pidfile}"
child_pid=""
cleanup_rank() {
    status=$?
    trap - EXIT HUP INT TERM
    if [[ -n "${child_pid}" ]] && kill -0 "${child_pid}" 2>/dev/null; then
        kill -TERM "${child_pid}" 2>/dev/null || true
        wait "${child_pid}" 2>/dev/null || true
    fi
    rm -f "${pidfile}"
    exit "${status}"
}
trap cleanup_rank EXIT HUP INT TERM
export ASCEND_HOME_PATH="${cann_path}"
export ASCEND_DRIVER_PATH=/usr/local/Ascend/driver
export TILEXR_COMM_ID="${comm_id}"
export TILEXR_DEMO_BARRIER_ADDR="${barrier_id}"
export TILEXR_ENABLE_IPC=0
export TILEXR_ENABLE_CREDIT_IPC=1
export TILEXR_ENABLE_SDMA=0
export LD_LIBRARY_PATH="${install_dir}/lib64:${cann_path}/aarch64-linux/lib64:${cann_path}/lib64:${ASCEND_DRIVER_PATH}/lib64:${ASCEND_DRIVER_PATH}/lib64/common:${ASCEND_DRIVER_PATH}/lib64/driver:${LD_LIBRARY_PATH:-}"
timeout --signal=TERM --kill-after=30 "${rank_timeout}" \
    "${install_dir}/bin/tilexr_moonep_combine_v2_perf" \
    --rank "${rank}" --world-size "${world}" --device "${device}" "$@" &
child_pid=$!
set +e
wait "${child_pid}"
status=$?
set -e
child_pid=""
rm -f "${pidfile}"
trap - EXIT HUP INT TERM
exit "${status}"
REMOTE_SCRIPT
)

build_remote_command() {
    local script=$1
    shift
    local command quoted argument
    printf -v quoted '%q' "${script}"
    command="bash -c ${quoted} --"
    for argument in "$@"; do
        printf -v quoted '%q' "${argument}"
        command+=" ${quoted}"
    done
    printf '%s' "${command}"
}

launch_rank() {
    local rank=$1
    local host="${rank_hosts[${rank}]}"
    local device="${rank_devices[${rank}]}"
    local remote_command
    remote_command=$(build_remote_command "${remote_rank_script}" \
        "${job_id}" "${rank_pidfiles[${rank}]}" "${TIMEOUT_SECONDS}" \
        "${INSTALL_DIR}" "${CANN_PATH}" "${COMM_ID}" "${BARRIER_ID}" \
        "${rank}" "${ranks}" "${device}" "${benchmark_args[@]}")
    exec ssh "${rank_ssh_options[@]}" "${SSH_USER}@${host}" "${remote_command}"
}

terminate_remote_tasks() {
    local rank host pidfile cleanup_script cleanup_command
    cleanup_script='pidfile=$1; job_id=$2; [[ -f "${pidfile}" ]] || exit 0; read -r pid stored_job <"${pidfile}"; [[ "${stored_job}" == "${job_id}" && -r "/proc/${pid}/cmdline" ]] || exit 0; cmdline=$(tr "\0" " " <"/proc/${pid}/cmdline"); [[ "${cmdline}" == *"${job_id}"* ]] || exit 0; kill -TERM "${pid}" 2>/dev/null || true'
    for ((rank = 0; rank < ranks; ++rank)); do
        host="${rank_hosts[${rank}]}"
        pidfile="${rank_pidfiles[${rank}]}"
        cleanup_command=$(build_remote_command "${cleanup_script}" \
            "${pidfile}" "${job_id}")
        ssh "${rank_ssh_options[@]}" "${SSH_USER}@${host}" "${cleanup_command}" \
            >/dev/null 2>&1 &
    done
    wait || true
}

run_active=0
ssh_pids=()
cleanup_controller() {
    status=$?
    trap - EXIT
    if (( run_active )); then
        for pid in "${ssh_pids[@]}"; do
            kill -TERM "${pid}" 2>/dev/null || true
        done
        terminate_remote_tasks
        for pid in "${ssh_pids[@]}"; do
            wait "${pid}" 2>/dev/null || true
        done
    fi
    close_rank_ssh_masters
    exit "${status}"
}
trap cleanup_controller EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo "RUN job_id=${job_id} ranks=${ranks} experts=${EXPERTS} hidden_size=${HIDDEN_SIZE} topk=${TOPK} comm_id=${COMM_ID} barrier_id=${BARRIER_ID} bs=${requested_bs} warmup=${WARMUP} iterations=${ITERATIONS}" | \
    tee -a "${LOG_FILE}"
run_active=1
for ((rank = 0; rank < ranks; ++rank)); do
    rank_log="${rank_log_dir}/rank_$(printf '%04d' "${rank}").log"
    launch_rank "${rank}" >"${rank_log}" 2>&1 &
    ssh_pids+=("$!")
done

completed=0
while (( completed < ranks )); do
    set +e
    wait -n
    rank_status=$?
    set -e
    completed=$((completed + 1))
    if (( rank_status != 0 )); then
        echo "a rank launcher failed with status ${rank_status}; see ${rank_log_dir}" | \
            tee -a "${LOG_FILE}" >&2
        exit "${rank_status}"
    fi
done
run_active=0
close_rank_ssh_masters

rank_logs=()
for ((rank = 0; rank < ranks; ++rank)); do
    rank_log="${rank_log_dir}/rank_$(printf '%04d' "${rank}").log"
    rank_logs+=("${rank_log}")
    {
        echo "===== RANK ${rank} host=${rank_hosts[${rank}]} device=${rank_devices[${rank}]} ====="
        cat "${rank_log}"
    } >>"${LOG_FILE}"
done

rank_averages_file="${rank_log_dir}/rank_averages.tsv"
rm -f "${rank_averages_file}"
if ! awk -v ranks="${ranks}" -v iterations="${ITERATIONS}" \
    -v output="${rank_averages_file}" '
    $1 == "COMBINE_V2_RANK_PERF" {
        bs = rank = logged_iterations = average = correctness = ""
        for (field = 2; field <= NF; ++field) {
            split($field, item, "=")
            if (item[1] == "bs") bs = item[2]
            else if (item[1] == "rank") rank = item[2]
            else if (item[1] == "iterations") logged_iterations = item[2]
            else if (item[1] == "avg_ms") average = item[2]
            else if (item[1] == "correctness") correctness = item[2]
        }
        rank_key = bs SUBSEP rank
        if (bs == "" || rank !~ /^[0-9]+$/ || average == "" ||
            rank + 0 < 0 || rank + 0 >= ranks ||
            (logged_iterations != "" && logged_iterations + 0 != iterations) ||
            (correctness != "passed" && correctness != "self_only_failed" &&
                correctness != "failed") || rank_result[rank_key]++) {
            invalid = 1
            next
        }
        batches[bs] = 1
        rank_average[rank_key] = average + 0
        rank_correctness[rank_key] = correctness
    }
    END {
        batch_count = 0
        for (bs in batches) {
            batch_count++
            for (rank = 0; rank < ranks; ++rank) {
                rank_key = bs SUBSEP rank
                if (rank_result[rank_key] != 1) {
                    invalid = 1
                } else {
                    print bs, rank, rank_average[rank_key], \
                        rank_correctness[rank_key] >> output
                }
            }
        }
        if (batch_count == 0 || invalid) exit 1
    }
' "${rank_logs[@]}"; then
    echo "rank logs do not contain one valid rank performance result per rank" | \
        tee -a "${LOG_FILE}" >&2
    exit 1
fi

sort -n -k1,1 -k2,2 "${rank_averages_file}" | awk \
    -v ranks="${ranks}" -v iterations="${ITERATIONS}" \
    -v experts="${EXPERTS}" -v hidden_size="${HIDDEN_SIZE}" -v topk="${TOPK}" \
    -v reduce="$((REDUCE_HIDDEN))" '
    function emit(    average, data_bytes, average_bandwidth, max_bandwidth) {
        if (count == 0) return
        if (count != ranks) exit 1
        average = total / count
        data_bytes = current_bs * topk * hidden_size * 2
        average_bandwidth = data_bytes / average / 1000000
        max_bandwidth = data_bytes / maximum / 1000000
        printf "COMBINE_V2_PERF bs=%s k=%d h=%d experts=%d dtype=bf16 ranks=%d iterations=%d avg_ms=%.6f avg_alg_bw_GBps=%.6f max_ms=%.6f max_alg_bw_GBps=%.6f reduce=%s correctness=%s\n", current_bs, topk, hidden_size, experts, ranks, iterations, average, average_bandwidth, maximum, max_bandwidth, reduce ? "enabled" : "disabled", batch_correctness
    }
    current_bs != "" && $1 != current_bs {
        emit()
        total = 0
        count = 0
        maximum = 0
        batch_correctness = "passed"
    }
    {
        if (count == 0) batch_correctness = "passed"
        current_bs = $1
        total += $3 + 0
        if (count == 0 || $3 + 0 > maximum) maximum = $3 + 0
        if ($4 == "failed") batch_correctness = "failed"
        else if ($4 == "self_only_failed" && batch_correctness == "passed")
            batch_correctness = "self_only_failed"
        count++
    }
    END { emit() }
' | tee -a "${LOG_FILE}"

echo "Combine V2 benchmark log: ${LOG_FILE}"
echo "Per-rank logs: ${rank_log_dir}"
