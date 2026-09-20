#!/usr/bin/env bash
set -eo pipefail
case "$#:${1:-}" in
    0:) ;;
    1:-h|1:--help)
        cat <<'EOF'
Usage: bash run_commit_validation.sh
Build and validate the current Combine checkpoint: ep-sdedup-combine-p1-chain-single.
C4 builds an isolated production package and runs C1/C3/C4 Host regressions,
then syncs to the first two configured hosts and runs six 16P functional cases.
Requires configured CANN 9.1, real A5 devices, SSH/rsync and hostfile; no skips.
Each case uses loops=1 warmup=0; Combine remains an empty kernel with 50us wait.
Requires Python 3.9+, CMake, Make and g++. TILEXR_VALIDATION_JOBS controls build jobs.
Reports: run/validation/commit-*/summary.json (or TILEXR_VALIDATION_RESULT_ROOT).
Return the summary and logs before proceeding to the next checkpoint.
EOF
        exit 0 ;;
    *) printf '[commit-validate] Failed: no arguments expected (use --help)\n' >&2; exit 2 ;;
esac
# This entry point follows the current delivery checkpoint, independent of a
# previous Dispatch build or deployment-specific case options.
export TILEXR_VALIDATION_CHECKPOINT=ep-sdedup-combine-p1-chain-single
export TILEXR_VALIDATION_BUILD_MODE=production
export TILEXR_VALIDATION_ACTION=commit-validate
if ! source "$(dirname -- "${BASH_SOURCE[0]}")/set_env.sh"; then
    printf '失败 COMMIT_VALIDATION_FAIL checkpoint=%s type=CONFIG_FAILED (environment setup; no build or tests started)\n' "$TILEXR_VALIDATION_CHECKPOINT" >&2
    exit 1
fi
exec "$TILEXR_VALIDATION_PYTHON" -u "$TILEXR_VALIDATION_ROOT/tools/validation/workflow.py" commit-validate
