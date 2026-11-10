#!/usr/bin/env bash
# 构建或启动 Combine V2 性能探针，向 tools/moonep 和 tests/moonep_combine_v2 传递运行参数。
set -euo pipefail

REMOTE_ROOT=""
HOSTFILE=""
SOURCE_DIR=""
BUILD_DIR=""
INSTALL_DIR=""
CANN_PATH="${ASCEND_HOME_PATH:-/usr/local/Ascend/ascend-toolkit/latest}"
SSH_USER="$(id -un)"
BS=""
BS_LIST=""
WARMUP=20
ITERATIONS=80
EXPERTS=64
HIDDEN_SIZE=3584
COMM_DOMAIN=141
COMM_PORT=10067
RANK_TIMEOUT=600
BUILD_JOBS="$(nproc)"
LOG_FILE=""
SKIP_BUILD=0
SKIP_RUNTIME_SYNC=0
REDUCE_HIDDEN=0

usage() {
    cat <<'EOF'
Usage: bash tools/moonep/run_combine_v2_perf_cluster.sh --remote-root PATH [options]

Options:
  --remote-root PATH        Remote task root containing source/build/install/logs
  --source-dir PATH         Source directory (default: REMOTE_ROOT/source)
  --build-dir PATH          Build directory (default: REMOTE_ROOT/build)
  --install-dir PATH        Runtime directory (default: REMOTE_ROOT/install)
  --hostfile PATH           Rank hostfile (default: REMOTE_ROOT/hostfile)
  --cann-path PATH          CANN root
  --ssh-user USER           SSH user for worker launch/sync (default: current user)
  --bs N                    Run one batch size (default: 128)
  --bs-list N[,N...]        Run multiple batch sizes
  --warmup N                Warmup launches per BS (default: 20)
  --iterations N            Timed launches per BS (default: 80)
  --experts N               Total expert count (default: 64)
  --hidden-size N           Hidden size H (default: 3584)
  --comm-domain N           Shared-QP domain (default: 141)
  --comm-port N             Bootstrap TCP port on first host (default: 10067)
  --rank-timeout N          Per-rank timeout (default: 600)
  --build-jobs N            Parallel build jobs (default: nproc)
  --log-file PATH           Controller log path
  --skip-build              Reuse an existing install directory
  --skip-runtime-sync       Do not rsync install to worker hosts
  --skip-iteration-barriers Deprecated no-op; launches are always continuous
  --reduce-hidden           Include BF16 TopK hidden reduction in the kernel
  --help                    Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --remote-root) REMOTE_ROOT="$2"; shift 2 ;;
        --source-dir) SOURCE_DIR="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --install-dir) INSTALL_DIR="$2"; shift 2 ;;
        --hostfile) HOSTFILE="$2"; shift 2 ;;
        --cann-path) CANN_PATH="$2"; shift 2 ;;
        --ssh-user) SSH_USER="$2"; shift 2 ;;
        --bs) BS="$2"; shift 2 ;;
        --bs-list) BS_LIST="$2"; shift 2 ;;
        --warmup) WARMUP="$2"; shift 2 ;;
        --iterations) ITERATIONS="$2"; shift 2 ;;
        --experts) EXPERTS="$2"; shift 2 ;;
        --hidden-size) HIDDEN_SIZE="$2"; shift 2 ;;
        --comm-domain) COMM_DOMAIN="$2"; shift 2 ;;
        --comm-port) COMM_PORT="$2"; shift 2 ;;
        --rank-timeout) RANK_TIMEOUT="$2"; shift 2 ;;
        --build-jobs) BUILD_JOBS="$2"; shift 2 ;;
        --log-file) LOG_FILE="$2"; shift 2 ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --skip-runtime-sync) SKIP_RUNTIME_SYNC=1; shift ;;
        --skip-iteration-barriers) shift ;;
        --reduce-hidden) REDUCE_HIDDEN=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "${REMOTE_ROOT}" ]]; then
    usage >&2
    exit 2
fi
SOURCE_DIR="${SOURCE_DIR:-${REMOTE_ROOT}/source}"
BUILD_DIR="${BUILD_DIR:-${REMOTE_ROOT}/build}"
INSTALL_DIR="${INSTALL_DIR:-${REMOTE_ROOT}/install}"
HOSTFILE="${HOSTFILE:-${REMOTE_ROOT}/hostfile}"
if [[ -z "${LOG_FILE}" ]]; then
    LOG_FILE="${REMOTE_ROOT}/logs/combine_v2_cluster_$(date +%Y%m%d_%H%M%S).log"
fi

for path in "${REMOTE_ROOT}" "${SOURCE_DIR}" "${BUILD_DIR}" "${INSTALL_DIR}" \
    "${HOSTFILE}" "${CANN_PATH}" "${LOG_FILE}"; do
    if [[ "${path}" != /* || "${path}" == *"'"* ]]; then
        echo "paths must be absolute and cannot contain single quotes: ${path}" >&2
        exit 2
    fi
done
if [[ -n "${BS}" && -n "${BS_LIST}" ]]; then
    echo "--bs and --bs-list are mutually exclusive" >&2
    exit 2
fi
if [[ -z "${BS}" && -z "${BS_LIST}" ]]; then
    BS=128
fi
for value in "${WARMUP}" "${ITERATIONS}" "${EXPERTS}" "${HIDDEN_SIZE}" \
    "${COMM_DOMAIN}" "${COMM_PORT}" "${RANK_TIMEOUT}" "${BUILD_JOBS}"; do
    if [[ ! "${value}" =~ ^[0-9]+$ ]]; then
        echo "numeric arguments must be non-negative integers" >&2
        exit 2
    fi
done
if (( ITERATIONS == 0 || EXPERTS == 0 || COMM_DOMAIN == 0 ||
      COMM_PORT == 0 || COMM_PORT > 65535 || RANK_TIMEOUT == 0 ||
      BUILD_JOBS == 0 )); then
    echo "iterations, experts, domain, port, timeout, and jobs must be positive" >&2
    exit 2
fi
if [[ ! -f "${HOSTFILE}" ]]; then
    echo "hostfile is missing: ${HOSTFILE}" >&2
    exit 1
fi
first_host="$(awk '
    /^[[:space:]]*($|#)/ { next }
    { gsub(/[[:space:]]/, "", $0); split($0, item, ":"); print item[1]; exit }
' "${HOSTFILE}")"
if [[ -z "${first_host}" ]]; then
    echo "hostfile has no hosts: ${HOSTFILE}" >&2
    exit 1
fi

build_script="${SOURCE_DIR}/tools/moonep/build_combine_v2_perf.sh"
sync_script="${SOURCE_DIR}/tools/moonep/sync_combine_v2_perf_runtime.sh"
run_script="${SOURCE_DIR}/tools/moonep/run_combine_v2_perf_multihost.sh"
for script in "${build_script}" "${sync_script}" "${run_script}"; do
    if [[ ! -f "${script}" ]]; then
        echo "required script is missing: ${script}" >&2
        exit 1
    fi
done

if (( ! SKIP_BUILD )); then
    build_args=(
        --source-dir "${SOURCE_DIR}" \
        --build-dir "${BUILD_DIR}" \
        --install-dir "${INSTALL_DIR}" \
        --cann-path "${CANN_PATH}" \
        --jobs "${BUILD_JOBS}"
    )
    bash "${build_script}" "${build_args[@]}"
fi

if (( ! SKIP_RUNTIME_SYNC )); then
    bash "${sync_script}" \
        --hostfile "${HOSTFILE}" \
        --install-dir "${INSTALL_DIR}" \
        --ssh-user "${SSH_USER}"
fi

run_args=(
    --hostfile "${HOSTFILE}"
    --install-dir "${INSTALL_DIR}"
    --cann-path "${CANN_PATH}"
    --ssh-user "${SSH_USER}"
    --warmup "${WARMUP}"
    --iterations "${ITERATIONS}"
    --experts "${EXPERTS}"
    --hidden-size "${HIDDEN_SIZE}"
    --comm-domain "${COMM_DOMAIN}"
    --comm-id "${first_host}:${COMM_PORT}"
    --timeout "${RANK_TIMEOUT}"
    --log-file "${LOG_FILE}"
)
if [[ -n "${BS_LIST}" ]]; then
    run_args+=(--bs-list "${BS_LIST}")
else
    run_args+=(--bs "${BS}")
fi
if (( REDUCE_HIDDEN )); then
    run_args+=(--reduce-hidden)
fi
bash "${run_script}" "${run_args[@]}"
echo "Completed. Primary log: ${LOG_FILE}"
