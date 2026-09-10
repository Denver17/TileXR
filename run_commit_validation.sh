#!/usr/bin/env bash
# 提交验收入口：只列出本提交实际涉及的检查点。
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
bash ./build_all.sh >/dev/null
bash ./sync.sh >/dev/null
run_case() { bash ./run_case.sh "$@"; }
run_case checkpoint=ep-sdedup-p1-chain-multi run_mode=functional loops=50 warmup=0
printf '\n==========\nPASS\n==========\n\n'
