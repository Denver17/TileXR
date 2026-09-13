#!/usr/bin/env bash
# Single public case entry. Build and distribution are separate manual actions.
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
usage() {
    cat <<'HELP'
Usage: bash run.sh -n SERVERS -e EXPERTS [-m 0|1|2] [-l LOOPS WARMUP] [key=value ...]
  -n  Number of servers from the beginning of hostfile (2..16, 8 devices each).
  -e  Total experts (1..1024, divisible by 8 * SERVERS).
  -m  0=functional (default), 1=performance, 2=breakdown.
  -l  Measurement loops and warmup loops (default: 1 0).
  -h, --help  Show help without loading the runtime environment.
Optional overrides: bs=96 h=7168 topk=6 dtype=bf16 quant_mode=0 fp8_format=e4m3
                    block_dim=auto comm_id=IP:PORT barrier_addr=IP:PORT
                    msprof=PATH hostfile=PATH
HELP
}
fail() { printf '[run] Failed: %s\n' "$*" >&2; exit 2; }
declare -A seen=()
declare -a assignments=()
servers= experts= run_mode=functional loops=1 warmup=0
while (( $# )); do
    key=$1
    [[ -n $key ]] || fail 'empty argument'
    [[ -z ${seen[$key]+set} ]] || fail "duplicate argument: $key"
    case "$key" in
        -h|--help) usage; exit 0;;
        -n|-e|-m)
            (( $# >= 2 )) || fail "missing value for $key"
            value=$2
            case "$key" in
                -n) [[ $value =~ ^([2-9]|1[0-6])$ ]] || fail '-n must be an integer in 2..16'; servers=$value;;
                -e) [[ $value =~ ^[1-9][0-9]{0,3}$ ]] && (( value <= 1024 )) || fail '-e must be an integer in 1..1024'; experts=$value;;
                -m) case "$value" in 0) run_mode=functional;; 1) run_mode=performance;; 2) run_mode=breakdown;; *) fail '-m must be 0, 1 or 2';; esac;;
            esac
            seen[$key]=1; shift 2;;
        -l)
            (( $# >= 3 )) || fail '-l requires two integers: LOOPS WARMUP'
            [[ $2 =~ ^[1-9][0-9]{0,9}$ && $3 =~ ^(0|[1-9][0-9]{0,9})$ ]] || fail '-l requires positive LOOPS and nonnegative WARMUP'
            (( $2 <= 4294967295 && $3 <= 4294967295 )) || fail '-l values must fit uint32'
            loops=$2; warmup=$3; seen[$key]=1; shift 3;;
        bs=*|h=*|topk=*|dtype=*|quant_mode=*|fp8_format=*|block_dim=*|comm_id=*|barrier_addr=*|msprof=*|hostfile=*)
            name=${key%%=*}; value=${key#*=}
            [[ -n $value ]] || fail "missing value for $name"
            [[ -z ${seen[$name]+set} ]] || fail "duplicate argument: $name"
            seen[$name]=1
            case "$name" in
                comm_id) variable=TILEXR_COMM_ID;;
                barrier_addr) variable=TILEXR_DEMO_BARRIER_ADDR;;
                *) variable=TILEXR_VALIDATION_${name^^};;
            esac
            assignments+=("$variable=$value"); shift;;
        *) fail "unknown argument: $key (see bash run.sh --help)";;
    esac
done
[[ -n $servers && -n $experts ]] || fail '-n SERVERS and -e EXPERTS are required'
(( experts % (servers * 8) == 0 )) || fail '-e must be divisible by 8 * -n'
# Resolve each case afresh; old shell exports must not change this invocation.
export TILEXR_VALIDATION_BS=96 TILEXR_VALIDATION_H=7168 TILEXR_VALIDATION_TOPK=6
export TILEXR_VALIDATION_DTYPE=bf16 TILEXR_VALIDATION_QUANT_MODE=0 TILEXR_VALIDATION_FP8_FORMAT=e4m3
export TILEXR_VALIDATION_BLOCK_DIM=auto TILEXR_VALIDATION_ACTION=run
unset TILEXR_DIRECT_HOSTS_JSON TILEXR_DIRECT_RUN
assignments+=("TILEXR_VALIDATION_SERVERS=$servers" "TILEXR_VALIDATION_MOE_EXPERT_NUM=$experts"
              "TILEXR_VALIDATION_RUN_MODE=$run_mode" "TILEXR_VALIDATION_LOOPS=$loops"
              "TILEXR_VALIDATION_WARMUP=$warmup")
if [[ ! -f validation.local.sh ]]; then
    fail "missing $PWD/validation.local.sh; configure it from validation.local.sh.example"
fi
case_id="case-$(date +%Y%m%d-%H%M%S)-$$"
result_root="${TILEXR_VALIDATION_RESULT_ROOT:-$PWD/run/$case_id}"
mkdir -p "$result_root"
result_root=$(cd -- "$result_root" && pwd -P)
export TILEXR_VALIDATION_RESULT_ROOT="$result_root"
printf '[run] Environment log: %s/environment.log\n[run] Workflow log: %s/workflow.log\n' "$result_root" "$result_root"
# Source in this shell so the configured environment reaches the workflow.
if source ./set_env.sh >"$result_root/environment.log" 2>&1; then
    cat -- "$result_root/environment.log"
else
    environment_status=$?
    cat -- "$result_root/environment.log" >&2
    printf '[run] Failed: environment exit=%s; log=%s/environment.log\n' "$environment_status" "$result_root" >&2
    exit "$environment_status"
fi
for assignment in "${assignments[@]}"; do export "$assignment"; done
# The workflow owns validation and reports its exact summary.json path. Keep its
# stdout/stderr visible and retain the real exit code through tee.
if "$TILEXR_VALIDATION_PYTHON" -u tools/validation/workflow.py run 2>&1 | tee "$result_root/workflow.log"; then
    stage_status=("${PIPESTATUS[@]}")
else
    stage_status=("${PIPESTATUS[@]}")
fi
if (( stage_status[0] != 0 )); then
    printf '[run] Failed: workflow exit=%s; log=%s/workflow.log\n' "${stage_status[0]}" "$result_root" >&2
    exit "${stage_status[0]}"
fi
if (( stage_status[1] != 0 )); then
    printf '[run] Failed: log capture exit=%s; log=%s/workflow.log\n' "${stage_status[1]}" "$result_root" >&2
    exit "${stage_status[1]}"
fi
