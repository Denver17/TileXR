#!/usr/bin/env bash
# 单用例核心入口。参数以 key=value 传入；构建和同步由调用方决定。
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if [[ ! -f validation.local.sh ]]; then
    printf '[run] Failed: missing %s/validation.local.sh; configure it from validation.local.sh.example\n' "$PWD" >&2
    exit 2
fi
case_id="case-$(date +%Y%m%d-%H%M%S)-$$"
result_root="${TILEXR_VALIDATION_RESULT_ROOT:-$PWD/run/$case_id}"
mkdir -p "$result_root"
result_root=$(cd -- "$result_root" && pwd -P)
declare -a assignments=()
for argument in "$@"; do
    case "$argument" in
        mode=*|run_mode=*|loops=*|warmup=*|bs=*|h=*|topk=*|moe_expert_num=*|dtype=*|quant_mode=*|fp8_format=*|block_dim=*|comm_id=*|barrier_addr=*|msprof=*|hostfile=*)
            key=${argument%%=*}; value=${argument#*=}
            case "$key" in
                mode) assignments+=("TILEXR_VALIDATION_BUILD_MODE=$value");;
                run_mode) assignments+=("TILEXR_VALIDATION_RUN_MODE=$value");; loops) assignments+=("TILEXR_VALIDATION_LOOPS=$value");;
                warmup) assignments+=("TILEXR_VALIDATION_WARMUP=$value");; bs) assignments+=("TILEXR_VALIDATION_BS=$value");;
                h) assignments+=("TILEXR_VALIDATION_H=$value");; topk) assignments+=("TILEXR_VALIDATION_TOPK=$value");;
                moe_expert_num) assignments+=("TILEXR_VALIDATION_MOE_EXPERT_NUM=$value");; dtype) assignments+=("TILEXR_VALIDATION_DTYPE=$value");;
                quant_mode) assignments+=("TILEXR_VALIDATION_QUANT_MODE=$value");;
                fp8_format) assignments+=("TILEXR_VALIDATION_FP8_FORMAT=$value");;
                block_dim) assignments+=("TILEXR_VALIDATION_BLOCK_DIM=$value");; comm_id) assignments+=("TILEXR_COMM_ID=$value");;
                barrier_addr) assignments+=("TILEXR_DEMO_BARRIER_ADDR=$value");; msprof) assignments+=("TILEXR_VALIDATION_MSPROF=$value");;
                hostfile) assignments+=("TILEXR_VALIDATION_HOSTFILE=$value");;
            esac;;
        *) printf '[run] Failed: unknown argument: %s (expected key=value; see docs/EP_VALIDATION_GUIDE.md)\n' "$argument" >&2; exit 2;;
    esac
done
for assignment in "${assignments[@]}"; do export "$assignment"; done
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
export TILEXR_DIRECT_RUN=1
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
