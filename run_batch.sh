#!/usr/bin/env bash
# e5a4bdb 的补充验证清单：Dispatch 多轮 functional + performance。
# 这是当前提交点的批次脚本；后续按需要编辑末尾 run_case，不要求通用化。
set -eo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"

if [[ ! -f validation.local.sh ]]; then
    echo '请先按七步指南配置 validation.local.sh 和 hostfile。' >&2
    exit 2
fi
mkdir -p run/validation
batch_dir=$(mktemp -d "$PWD/run/validation/batch-XXXXXXXX")
batch_backup="$batch_dir/validation.local.sh"
cp -- validation.local.sh "$batch_backup"
trap 'cp -- "$batch_backup" validation.local.sh && rm -- "$batch_backup"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

batch_built=0
batch_completed=0
run_case() {
    local mode=$1 loops=$2 warmup=$3
    # 写入实际会被 sync.sh 分发的配置，保证 SSH 节点读取同一组参数。
    {
        cat -- "$batch_backup"
        printf '\nTILEXR_VALIDATION_BUILD_MODE=production\n'
        printf 'TILEXR_VALIDATION_CHECKPOINT=ep-sdedup-p1-chain-multi\n'
        printf 'TILEXR_VALIDATION_RUN_MODE=%s\nTILEXR_VALIDATION_LOOPS=%s\nTILEXR_VALIDATION_WARMUP=%s\n' \
            "$mode" "$loops" "$warmup"
    } > validation.local.sh
    echo "BATCH_CASE mode=$mode loops=$loops warmup=$warmup 日志=$batch_dir/$mode.log"
    # 当前两项的 kernel 都关闭设备打点，共用一次构建。
    if [[ "$batch_built" == 0 ]]; then
        bash build_all.sh 2>&1 | tee "$batch_dir/build.log"
        batch_built=1
    fi
    bash sync.sh 2>&1 | tee "$batch_dir/$mode-sync.log"
    bash run.sh 2>&1 | tee "$batch_dir/$mode.log"
    batch_completed=$((batch_completed + 1))
}

# 本提交点的测试项；可修改轮数或注释其中一项。首个保留项负责构建。
# 如以后加入其他 checkpoint 或 breakdown，需相应修改配置并重新构建。
run_case functional 50 0
run_case performance 50 50
echo "BATCH_PASS cases=$batch_completed 日志=$batch_dir"
