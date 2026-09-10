#!/usr/bin/env bash
# 单用例核心入口。参数以 key=value 传入；构建和同步由调用方决定。
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
if [[ ! -f validation.local.sh ]]; then printf '\n================================\nFailed\n================================\n\n' ; exit 2; fi
case_id="case-$(date +%Y%m%d-%H%M%S)-$$"
result_root="${TILEXR_VALIDATION_RESULT_ROOT:-$PWD/run/$case_id}"
mkdir -p "$result_root"
declare -a assignments=()
for argument in "$@"; do
    case "$argument" in
        mode=*|run_mode=*|loops=*|warmup=*|bs=*|h=*|topk=*|moe_expert_num=*|dtype=*|block_dim=*|comm_id=*|barrier_addr=*|msprof=*|hostfile=*)
            key=${argument%%=*}; value=${argument#*=}
            case "$key" in
                mode) assignments+=("TILEXR_VALIDATION_BUILD_MODE=$value");;
                run_mode) assignments+=("TILEXR_VALIDATION_RUN_MODE=$value");; loops) assignments+=("TILEXR_VALIDATION_LOOPS=$value");;
                warmup) assignments+=("TILEXR_VALIDATION_WARMUP=$value");; bs) assignments+=("TILEXR_VALIDATION_BS=$value");;
                h) assignments+=("TILEXR_VALIDATION_H=$value");; topk) assignments+=("TILEXR_VALIDATION_TOPK=$value");;
                moe_expert_num) assignments+=("TILEXR_VALIDATION_MOE_EXPERT_NUM=$value");; dtype) assignments+=("TILEXR_VALIDATION_DTYPE=$value");;
                block_dim) assignments+=("TILEXR_VALIDATION_BLOCK_DIM=$value");; comm_id) assignments+=("TILEXR_COMM_ID=$value");;
                barrier_addr) assignments+=("TILEXR_DEMO_BARRIER_ADDR=$value");; msprof) assignments+=("TILEXR_VALIDATION_MSPROF=$value");;
                hostfile) assignments+=("TILEXR_VALIDATION_HOSTFILE=$value");;
            esac;;
        *) printf '\n================================\nFailed\n================================\n\n' ; exit 2;;
    esac
done
for assignment in "${assignments[@]}"; do export "$assignment"; done
export TILEXR_VALIDATION_RESULT_ROOT="$result_root"
if source ./set_env.sh >"$result_root/environment.log" 2>&1; then
    for assignment in "${assignments[@]}"; do export "$assignment"; done
    export TILEXR_DIRECT_RUN=1
    if "$TILEXR_VALIDATION_PYTHON" tools/validation/workflow.py run >"$result_root/workflow.log" 2>&1; then
        status=PASS
    else
        status=FAIL
    fi
else
    printf '\n================================\nFailed\n================================\n\n'
    printf 'ERROR environment setup failed; see environment.log\n' >"$result_root/summary.log"
    exit 1
fi
summary_json=$(find "$result_root" -name summary.json -type f -print -quit || true)
if [[ -n "$summary_json" ]]; then cp -- "$summary_json" "$result_root/summary.json"; fi
"${TILEXR_VALIDATION_PYTHON:-python3}" - "$result_root" "$status" <<'PY'
import json
from pathlib import Path
import sys
sys.path.insert(0, 'tools/validation')
from measurements import RESULT_SEPARATOR, render_case_description
root = Path(sys.argv[1])
status = sys.argv[2]
try:
    report = json.loads((root / 'summary.json').read_text(encoding='utf-8'))
    description = render_case_description(report)
except (OSError, ValueError, KeyError) as error:
    report = {}
    description = 'Case configuration unavailable'
    status = 'FAIL'
    with (root / 'workflow.log').open('a', encoding='utf-8') as log:
        log.write(f'\nERROR summary: {error}\n')
result = 'PASS' if status == 'PASS' else 'Failed'
if status == 'PASS':
    average = report.get('measurements', {}).get('performance', {}).get('mean_us')
    if average is not None:
        result += f' kernel average_us={average:.3f}'
text = f'\n{RESULT_SEPARATOR}\n{description}\n{result}\n{RESULT_SEPARATOR}\n\n'
(root / 'summary.log').write_text(text, encoding='utf-8')
print(text, end='')
sys.exit(0 if status == 'PASS' else 1)
PY
