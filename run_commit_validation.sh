#!/usr/bin/env bash
# 提交验收入口：只列出本提交实际涉及的检查点。
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
# checkpoint 只在提交验收入口选择一次，构建、同步与所有 case 继承。
export TILEXR_VALIDATION_CHECKPOINT=ep-sdedup-p1-chain-multi
run_mode=functional
export TILEXR_VALIDATION_RUN_MODE="$run_mode"
bash ./build_all.sh >/dev/null
bash ./sync.sh >/dev/null
run_case() { bash ./run_case.sh "$@"; }
run_case run_mode="$run_mode" loops=50 warmup=0
printf '\n================================\nPASS\n================================\n\n'
