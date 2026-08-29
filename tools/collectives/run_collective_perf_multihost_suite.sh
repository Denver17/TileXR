#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
Usage:
  run_collective_perf_multihost_suite.sh prepare
  run_collective_perf_multihost_suite.sh build
  run_collective_perf_multihost_suite.sh host-checks
  run_collective_perf_multihost_suite.sh case <name> <op> <bytes>
  run_collective_perf_multihost_suite.sh suite
  run_collective_perf_multihost_suite.sh profile-probe
  run_collective_perf_multihost_suite.sh verify <profile_case_dir>

Environment:
  TILEXR_PROFILE_REPO_DIR      TileXR repo path. Defaults to this script's repo.
  TILEXR_PROFILE_BUILD_DIR     Profile build dir. Defaults to <repo>/build-profile-950.
  TILEXR_PROFILE_DIR           Multi-host profile output root. Defaults to <repo>/run/prof/collectives-2host.
  TILEXR_PROFILE_BUILD_JOBS    Build parallelism. Defaults to nproc or 8.
  TILEXR_PROFILE_ENV_SCRIPT    Optional environment script sourced before build/checks.
  CMAKE                        Optional cmake executable name. Defaults to cmake from PATH.

Multi-host runs also require TILEXR_MULTIHOST_PEERS and usually TILEXR_COMM_ID.
EOF
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="${TILEXR_PROFILE_REPO_DIR:-$(cd "${script_dir}/../.." && pwd)}"
build_dir="${TILEXR_PROFILE_BUILD_DIR:-${repo_dir}/build-profile-950}"
profile_root="${TILEXR_PROFILE_DIR:-${repo_dir}/run/prof/collectives-2host}"
cmake_bin="${CMAKE:-cmake}"

build_jobs="${TILEXR_PROFILE_BUILD_JOBS:-}"
if [[ -z "${build_jobs}" ]]; then
  if command -v nproc >/dev/null 2>&1; then
    build_jobs="$(nproc)"
  else
    build_jobs=8
  fi
fi

source_profile_env() {
  if [[ -n "${TILEXR_PROFILE_ENV_SCRIPT:-}" ]]; then
    # shellcheck disable=SC1090
    source "${TILEXR_PROFILE_ENV_SCRIPT}"
    return
  fi
  if [[ -f "${repo_dir}/scripts/common_env.sh" ]]; then
    # shellcheck disable=SC1091
    source "${repo_dir}/scripts/common_env.sh"
  fi
}

build_profile() {
  cd "${repo_dir}"
  source_profile_env
  "${cmake_bin}" -S . -B "${build_dir}" \
    -DTILEXR_BUILD_COLLECTIVES=ON \
    -DTILEXR_BUILD_TESTS=ON \
    -DTILEXR_COLLECTIVES_ENABLE_PROFILING=ON \
    -DBUILD_TESTING=OFF
  "${cmake_bin}" --build "${build_dir}" --target \
    test_tilexr_collectives_header_compile \
    test_tilexr_int32_pattern \
    test_tilexr_perf_trace_layout \
    tilexr_collective_perf -j"${build_jobs}"
}

run_host_checks() {
  cd "${repo_dir}"
  source_profile_env
  "${build_dir}/tests/collectives/test_tilexr_collectives_header_compile"
  "${build_dir}/tests/collectives/test_tilexr_int32_pattern"
  "${build_dir}/tests/collectives/test_tilexr_perf_trace_layout"
  python3 "${repo_dir}/tests/collectives/unit/test_collective_profile_report.py"
}

run_profile_case() {
  local name="$1"
  local op="$2"
  local bytes="$3"
  bash "${script_dir}/run_collective_perf_multihost.sh" \
    "${profile_root}/${name}" "${build_dir}/tests/collectives" \
    --op "${op}" \
    --min-bytes "${bytes}" --max-bytes "${bytes}" \
    --datatype "${TILEXR_PROFILE_DATATYPE:-int32}" --check "${TILEXR_PROFILE_CHECK:-0}" \
    --warmup-iters "${TILEXR_PROFILE_WARMUP_ITERS:-1}" \
    --iters "${TILEXR_PROFILE_ITERS:-2}" \
    --profile 1 \
    --profile-sample-every "${TILEXR_PROFILE_SAMPLE_EVERY:-1}" \
    --profile-ai-prompt "${TILEXR_PROFILE_AI_PROMPT:-1}" </dev/null
  verify_profile "${profile_root}/${name}"
}

run_suite() {
  run_profile_case fine-allgather-big allgather 16777216
  run_profile_case coarse-allreduce-big allreduce 16777216
  run_profile_case coarse-reducescatter-big reducescatter 16777216
  run_profile_case coarse-alltoall-big alltoall 16777216
  run_profile_case coarse-broadcast-128m broadcast 134217728
}

run_profile_probe() {
  TILEXR_PROFILE_DATATYPE="${TILEXR_PROFILE_DATATYPE:-int8}" run_profile_case \
    fine-profile-probe profile-probe 1048576
}

verify_profile() {
  local root="$1"
  python3 - "${root}" <<'PY'
import json
import sys
from pathlib import Path

root = Path(sys.argv[1])
expected = {
    "kernel_total",
    "chunk_total",
    "local_input_to_ipc",
    "flag_poll_wait",
    "peer_ipc_to_output",
    "chunk_barrier",
    "post_sync",
}

trace_path = root / "trace.json"
trace = json.loads(trace_path.read_text(encoding="utf-8"))
names = {
    str(event.get("name", "")).rsplit("/", 1)[-1]
    for event in trace.get("traceEvents", [])
    if isinstance(event, dict)
}
missing = sorted(expected - names)

diagnostics = []
for rank_trace in root.glob("rank*/launch*/trace.json"):
    data = json.loads(rank_trace.read_text(encoding="utf-8"))
    diagnostics.extend(data.get("diagnostics", []))

print("trace:", trace_path)
print("stages:", ",".join(sorted(names)))
print("missing:", ",".join(missing) if missing else "none")
print("diagnostics:", len(diagnostics))
raise SystemExit(1 if missing or diagnostics else 0)
PY
}

command="${1:-}"
case "${command}" in
  -h|--help|"")
    usage
    exit 0
    ;;
  build)
    build_profile
    ;;
  host-checks)
    run_host_checks
    ;;
  prepare)
    build_profile
    run_host_checks
    ;;
  case)
    if [[ $# -ne 4 ]]; then
      usage
      exit 2
    fi
    run_profile_case "$2" "$3" "$4"
    ;;
  suite)
    run_suite
    ;;
  profile-probe)
    run_profile_probe
    ;;
  verify)
    if [[ $# -ne 2 ]]; then
      usage
      exit 2
    fi
    verify_profile "$2"
    ;;
  *)
    usage
    exit 2
    ;;
esac
