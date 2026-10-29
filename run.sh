#!/usr/bin/env bash
# Single public case entry. Build and distribution are separate manual actions.
set -euo pipefail
launcher_started=$SECONDS
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
usage() {
    cat <<'HELP'
Usage: bash run.sh -n SERVERS -e EXPERTS [-m 0|1|2] [-l LOOPS WARMUP] [key=value ...]
  -n  Number of servers from the beginning of hostfile (2..32, 8 devices each).
  -e  Total experts (1..1024, divisible by 8 * SERVERS).
  -m  0=functional (default), 1=performance, 2=breakdown.
  -l  Measurement loops and warmup loops (default: 1 0).
  -h, --help  Show help without loading the runtime environment.
Optional overrides: bs=96 h=7168 topk=6 dtype=bf16 fp8_format=e4m3
                    operator_mode=dispatch|combine|both dry_run=0|1
                    dispatch_quant_mode=0|4 combine_quant_mode=0
                    quant_mode=0|4 (legacy alias for dispatch_quant_mode; conflicts rejected)
                    route_mode=random|balance|balance_shift (default: balance_shift)
                    seed=UINT32 (default: 2026)
                    block_dim=auto comm_id=IP:PORT barrier_addr=IP:PORT
                    msprof=PATH hostfile=PATH host_validation=0|1 (default: 0)
                    case_analysis=0|1 (default: 0)
  dry_run=1 reads hostfile and validates ranks locally using the installed consumer; no SSH/CANN/NPU.
  Dry-run uses the current package without CANN, SSH or device operations.
  Supports repeated calls; rebuild without profiling for functional/performance, with profiling for breakdown.
  The current production package is selected automatically.
  host_validation=1 runs local Host/Python tests before the NPU workflow.
  case_analysis=1 reports rank deduplication min/max/avg after all NPU work finishes.
  Endpoints default to hostfile's first (local coordinator) IPv4, ports 10177/10274.
HELP
}
fail() { printf 'VALIDATION_FAIL 执行阶段失败 | %s\n' "$*" >&2; exit 2; }
declare -A seen=()
declare -a assignments=()
servers= experts= run_mode=functional loops=1 warmup=0 host_validation=0
dry_run=0 operator_mode=dispatch legacy_quant= dispatch_quant= combine_quant=0
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
                -n) [[ $value =~ ^([2-9]|[12][0-9]|3[0-2])$ ]] || fail '-n must be an integer in 2..32'; servers=$value;;
                -e) [[ $value =~ ^[1-9][0-9]{0,3}$ ]] && (( value <= 1024 )) || fail '-e must be an integer in 1..1024'; experts=$value;;
                -m) case "$value" in 0) run_mode=functional;; 1) run_mode=performance;; 2) run_mode=breakdown;; *) fail '-m must be 0, 1 or 2';; esac;;
            esac
            seen[$key]=1; shift 2;;
        -l)
            (( $# >= 3 )) || fail '-l requires two integers: LOOPS WARMUP'
            [[ $2 =~ ^[1-9][0-9]{0,9}$ && $3 =~ ^(0|[1-9][0-9]{0,9})$ ]] || fail '-l requires positive LOOPS and nonnegative WARMUP'
            (( $2 <= 4294967295 && $3 <= 4294967295 )) || fail '-l values must fit uint32'
            loops=$2; warmup=$3; seen[$key]=1; shift 3;;
        host_validation=*)
            [[ -z ${seen[host_validation]+set} ]] || fail 'duplicate argument: host_validation'
            host_validation=${key#*=}
            [[ $host_validation == 0 || $host_validation == 1 ]] || fail 'host_validation must be 0 or 1'
            seen[host_validation]=1; shift;;
        bs=*|h=*|topk=*|dtype=*|quant_mode=*|dispatch_quant_mode=*|combine_quant_mode=*|operator_mode=*|dry_run=*|fp8_format=*|block_dim=*|comm_id=*|barrier_addr=*|msprof=*|hostfile=*|case_analysis=*|route_mode=*|seed=*)
            name=${key%%=*}; value=${key#*=}
            [[ -n $value ]] || fail "missing value for $name"
            [[ -z ${seen[$name]+set} ]] || fail "duplicate argument: $name"
            seen[$name]=1
            case "$name" in
                operator_mode) case "$value" in dispatch|combine|both) operator_mode=$value;; *) fail 'operator_mode must be dispatch, combine or both';; esac;;
                dry_run) [[ $value == 0 || $value == 1 ]] || fail 'dry_run must be 0 or 1'; dry_run=$value;;
                quant_mode|dispatch_quant_mode) [[ $value == 0 || $value == 4 ]] || fail "$name must be 0 or 4"
                    if [[ $name == quant_mode ]]; then legacy_quant=$value; else dispatch_quant=$value; fi;;
                combine_quant_mode) [[ $value == 0 ]] || fail 'combine_quant_mode currently supports only 0'; combine_quant=$value;;
            esac
            if [[ $name == case_analysis ]]; then
                [[ $value == 0 || $value == 1 ]] || fail 'case_analysis must be 0 or 1'
            fi
            if [[ $name == route_mode ]]; then
                case "$value" in random|balance|balance_shift) ;; *) fail 'route_mode must be random, balance or balance_shift';; esac
            fi
            if [[ $name == seed ]]; then
                [[ $value =~ ^(0|[1-9][0-9]{0,9})$ ]] && (( value <= 4294967295 )) || fail 'seed must be an integer in 0..4294967295'
            fi
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
if [[ -n $legacy_quant && -n $dispatch_quant && $legacy_quant != "$dispatch_quant" ]]; then
    fail 'quant_mode conflicts with dispatch_quant_mode'
fi
dispatch_quant=${dispatch_quant:-${legacy_quant:-0}}
# Resolve each case afresh; old shell exports must not change this invocation.
export TILEXR_VALIDATION_BS=96 TILEXR_VALIDATION_H=7168 TILEXR_VALIDATION_TOPK=6
export TILEXR_VALIDATION_DTYPE=bf16 TILEXR_VALIDATION_QUANT_MODE="$dispatch_quant" TILEXR_VALIDATION_FP8_FORMAT=e4m3
export TILEXR_VALIDATION_DISPATCH_QUANT_MODE="$dispatch_quant" TILEXR_VALIDATION_COMBINE_QUANT_MODE="$combine_quant"
export TILEXR_VALIDATION_OPERATOR_MODE="$operator_mode" TILEXR_VALIDATION_DRY_RUN="$dry_run"
export TILEXR_VALIDATION_BLOCK_DIM=auto TILEXR_VALIDATION_ACTION=run
export TILEXR_VALIDATION_CASE_ANALYSIS=0
export TILEXR_VALIDATION_ROUTE_MODE=balance_shift TILEXR_VALIDATION_SEED=2026
unset TILEXR_DIRECT_HOSTS_JSON TILEXR_DIRECT_RUN
unset TILEXR_COMM_ID TILEXR_DEMO_BARRIER_ADDR
assignments+=("TILEXR_VALIDATION_SERVERS=$servers" "TILEXR_VALIDATION_MOE_EXPERT_NUM=$experts"
              "TILEXR_VALIDATION_RUN_MODE=$run_mode" "TILEXR_VALIDATION_LOOPS=$loops"
              "TILEXR_VALIDATION_WARMUP=$warmup")
export TILEXR_VALIDATION_BREAKDOWN=0
[[ $run_mode != breakdown ]] || export TILEXR_VALIDATION_BREAKDOWN=1
if [[ $dry_run == 0 ]]; then
    export TILEXR_VALIDATION_BUILD_MODE=production
fi
if [[ $dry_run == 1 ]]; then
    export TILEXR_VALIDATION_ACTION=case-dry-run TILEXR_VALIDATION_BUILD_MODE=host
fi
if [[ $dry_run == 0 && ! -f validation.local.sh ]]; then
    fail "missing $PWD/validation.local.sh; configure it from validation.local.sh.example"
fi
case_id="case-$(date +%Y%m%d-%H%M%S)-$$"
result_root="${TILEXR_VALIDATION_RESULT_ROOT:-$PWD/run/$case_id}"
mkdir -p "$result_root"
result_root=$(cd -- "$result_root" && pwd -P)
export TILEXR_VALIDATION_RESULT_ROOT="$result_root"
# Keep all stage/rank details on disk. The EXIT trap is the only terminal writer.
# Clear the previous result when an explicit result root is reused.
: >"$result_root/result.txt"
exec 3>&1
declare -A launcher_seconds=([setup]=0 [host_tests]=0 [environment]=0 [workflow]=0)
launcher_phase=setup
launcher_phase_started=$launcher_started
launcher_next() {
    launcher_seconds[$launcher_phase]=$(( SECONDS - launcher_phase_started ))
    launcher_phase=$1
    launcher_phase_started=$SECONDS
}
finish() {
    local status=$?
    trap - EXIT
    launcher_next finished
    local total=$(( SECONDS - launcher_started ))
    if [[ -s $result_root/result.txt ]]; then
        cat -- "$result_root/result.txt" >&3
    else
        printf 'VALIDATION_FAIL 执行阶段失败 | exit=%s | logs=%s\n' "$status" "$result_root" >&3
        # Missing a workflow result cannot certify a successful case.
        (( status != 0 )) || status=1
    fi
    printf 'RUN_WALL(s,1s-resolution) total=%s setup=%s host_tests=%s env=%s workflow=%s\n' \
        "$total" "${launcher_seconds[setup]}" \
        "${launcher_seconds[host_tests]}" "${launcher_seconds[environment]}" "${launcher_seconds[workflow]}" >&3
    printf '{"unit":"seconds","resolution_seconds":1,"total":%s,"setup":%s,"host_tests":%s,"environment":%s,"workflow":%s}\n' \
        "$total" "${launcher_seconds[setup]}" \
        "${launcher_seconds[host_tests]}" "${launcher_seconds[environment]}" "${launcher_seconds[workflow]}" \
        >"$result_root/launcher_timing.json" || true
    exit "$status"
}
trap finish EXIT
exec >"$result_root/launcher.log" 2>&1
if [[ $host_validation == 1 && $dry_run == 0 ]]; then
    launcher_next host_tests
    bash ./run_host_validation.sh >"$result_root/host-validation.log" 2>&1
fi
# Source in this shell so the configured environment reaches the workflow.
launcher_next environment
source ./set_env.sh >"$result_root/environment.log" 2>&1
for assignment in "${assignments[@]}"; do export "$assignment"; done
launcher_next workflow
"$TILEXR_VALIDATION_PYTHON" -u tools/validation/workflow.py "$TILEXR_VALIDATION_ACTION" >"$result_root/workflow.log" 2>&1
