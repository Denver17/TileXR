#!/usr/bin/env bash
# 单用例核心入口。参数以 key=value 传入；构建和同步由调用方决定。
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if [[ ! -f validation.local.sh ]]; then printf '\n==========\nFailed\n==========\n\n' ; exit 2; fi
case_id="case-$(date +%Y%m%d-%H%M%S)-$$"
result_root="${TILEXR_VALIDATION_RESULT_ROOT:-$PWD/run/$case_id}"
mkdir -p "$result_root"
declare -a assignments=()
for argument in "$@"; do
    case "$argument" in
        checkpoint=*|mode=*|run_mode=*|loops=*|warmup=*|bs=*|h=*|topk=*|moe_expert_num=*|dtype=*|block_dim=*|comm_id=*|barrier_addr=*|msprof=*|hostfile=*)
            key=${argument%%=*}; value=${argument#*=}
            case "$key" in
                checkpoint) assignments+=("TILEXR_VALIDATION_CHECKPOINT=$value");; mode) assignments+=("TILEXR_VALIDATION_BUILD_MODE=$value");;
                run_mode) assignments+=("TILEXR_VALIDATION_RUN_MODE=$value");; loops) assignments+=("TILEXR_VALIDATION_LOOPS=$value");;
                warmup) assignments+=("TILEXR_VALIDATION_WARMUP=$value");; bs) assignments+=("TILEXR_VALIDATION_BS=$value");;
                h) assignments+=("TILEXR_VALIDATION_H=$value");; topk) assignments+=("TILEXR_VALIDATION_TOPK=$value");;
                moe_expert_num) assignments+=("TILEXR_VALIDATION_MOE_EXPERT_NUM=$value");; dtype) assignments+=("TILEXR_VALIDATION_DTYPE=$value");;
                block_dim) assignments+=("TILEXR_VALIDATION_BLOCK_DIM=$value");; comm_id) assignments+=("TILEXR_COMM_ID=$value");;
                barrier_addr) assignments+=("TILEXR_DEMO_BARRIER_ADDR=$value");; msprof) assignments+=("TILEXR_VALIDATION_MSPROF=$value");;
                hostfile) assignments+=("TILEXR_VALIDATION_HOSTFILE=$value");;
            esac;;
        *) printf '\n==========\nFailed\n==========\n\n' ; exit 2;;
    esac
done
for assignment in "${assignments[@]}"; do export "$assignment"; done
export TILEXR_VALIDATION_RESULT_ROOT="$result_root"
cp -- validation.local.sh "$result_root/validation.local.sh.bak"
printf '\n# run_case temporary overrides\n' >> validation.local.sh
for assignment in "${assignments[@]}"; do printf '%s\n' "$assignment" >> validation.local.sh; done
trap 'cp -- "$result_root/validation.local.sh.bak" validation.local.sh' EXIT
source ./set_env.sh >"$result_root/environment.log" 2>&1
for assignment in "${assignments[@]}"; do export "$assignment"; done
export TILEXR_DIRECT_RUN=1
if bash ./run.sh >"$result_root/workflow.log" 2>&1; then status=PASS; else status=FAIL; fi
summary_json=$(find "$result_root" -name summary.json -type f -print -quit || true)
if [[ -n "$summary_json" ]]; then cp -- "$summary_json" "$result_root/summary.json"; fi
if [[ "$status" == PASS ]]; then
    average=$("${TILEXR_VALIDATION_PYTHON:-python3}" - "$result_root/summary.json" <<'PY'
import json,sys
try:
 d=json.load(open(sys.argv[1],encoding='utf-8')); print(d.get('measurements',{}).get('performance',{}).get('mean_us',''))
except Exception: print('')
PY
)
    echo status=PASS >"$result_root/summary.log"
    if [[ -n "$average" ]]; then echo "average_us=$average" >>"$result_root/summary.log"; printf '\n==========\nPASS kernel average_us=%s\n==========\n\n' "$average"; else printf '\n==========\nPASS\n==========\n\n' ; fi
    exit 0
fi
echo status=FAIL >"$result_root/summary.log"
printf '\n==========\nFailed\n==========\n\n'
exit 1
