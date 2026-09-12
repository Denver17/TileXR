#!/usr/bin/env bash
# 提交验收入口：只列出本提交实际涉及的检查点。
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
# checkpoint 只在提交验收入口选择一次，构建、同步与所有 case 继承。
export TILEXR_VALIDATION_CHECKPOINT=ep-sdedup-p1-chain-multi
run_mode=functional
warmup=0
# 当前主流程基线：hostfile 配置两台各 8 卡服务器，共 16P。
moe_expert_num=96
bs=96
h=7168
topk=6
dtype=bf16
export TILEXR_VALIDATION_RUN_MODE="$run_mode"
export TILEXR_VALIDATION_LOOPS=3 TILEXR_VALIDATION_WARMUP="$warmup"
export TILEXR_VALIDATION_MOE_EXPERT_NUM="$moe_expert_num"
export TILEXR_VALIDATION_BS="$bs" TILEXR_VALIDATION_H="$h"
export TILEXR_VALIDATION_TOPK="$topk" TILEXR_VALIDATION_DTYPE="$dtype"
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
# 固定 shape 和 balance_shift 输入，先校验两种 count 输出，再采集 kernel 性能。
case_shape=(bs="$bs" h="$h" topk="$topk" moe_expert_num="$moe_expert_num" dtype="$dtype")
run_case run_mode="$run_mode" loops=3 warmup="$warmup" "${case_shape[@]}"
run_case run_mode="$run_mode" loops=4 warmup="$warmup" "${case_shape[@]}"
run_case run_mode=performance loops=50 warmup=50 "${case_shape[@]}"
if (( completed_cases > 0 )); then
    printf '\n================================\nBatch PASS cases=%s\n================================\n\n' "$completed_cases"
else
    printf '\n================================\nBatch Failed (no cases)\n================================\n\n'
    exit 1
fi
