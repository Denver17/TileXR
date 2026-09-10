#!/usr/bin/env bash
# 提交验收入口：只列出本提交实际涉及的检查点。
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
# checkpoint 只在提交验收入口选择一次，构建、同步与所有 case 继承。
export TILEXR_VALIDATION_CHECKPOINT=ep-sdedup-p1-chain-multi
run_mode=functional
export TILEXR_VALIDATION_RUN_MODE="$run_mode"
for step in build_all sync; do
    if bash "./$step.sh" >/dev/null; then
        :
    else
        step_status=$?
        printf '\n================================\nBatch Failed\n================================\n\n'
        exit "$step_status"
    fi
done
completed_cases=0
run_case() {
    if bash ./run_case.sh "$@"; then
        completed_cases=$((completed_cases + 1))
    else
        local case_status=$?
        printf '\n================================\nBatch Failed\n================================\n\n'
        exit "$case_status"
    fi
}
run_case run_mode="$run_mode" loops=50 warmup=0
if (( completed_cases > 0 )); then
    printf '\n================================\nBatch PASS cases=%s\n================================\n\n' "$completed_cases"
else
    printf '\n================================\nBatch Failed (no cases)\n================================\n\n'
    exit 1
fi
