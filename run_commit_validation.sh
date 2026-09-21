#!/usr/bin/env bash
set -eo pipefail
case "$#:${1:-}" in
    0:) unset TILEXR_VALIDATION_RESUME_SUMMARY ;;
    2:--resume)
        [[ -n $2 ]] || { echo 'Missing prior summary path' >&2; exit 2; }
        export TILEXR_VALIDATION_RESUME_SUMMARY="$2" ;;
    1:-h|1:--help)
        cat <<'EOF'
Usage: bash run_commit_validation.sh [--resume PRIOR_SUMMARY.json]
Build and validate Combine P2.1: ep-sdedup-combine-p2-m12 (M0/M1/M2, 50us retained).
Default: builds isolated profiling OFF/ON packages and runs Host regressions for both.
First two configured hosts, fixed 8P each: 18 base + 5 targeted serial cases.
F1: single functional (0 warmup + 1 loop); F2/P/B: 2 warmup + 5 loops.
Each gate covers BF16/FP16 combine/both; plus Dispatch regression, quant4 connection,
and sparse/tail/empty/10000-row fixtures. All four gates must pass; first failure stops.
Requires configured CANN 9.1, real A5 devices, SSH/rsync, msprof and hostfile.
The manifest prints every case and [i/23] progress; no automatic retry/restart.
--resume is for P2.1 stopped at OFF/ON source mismatch after all 19 OFF cases passed.
It verifies retained reports and installed package hashes, skips those 19 cases and
both builds, then runs ON Host checks, sync and the four breakdown cases.
Keep both original installed packages and reports; do not rebuild before resuming.
The report preserves the historical source mismatch and marks reused PASS evidence.
Does not validate M3-M5 business data or yOut arithmetic.
Requires Python 3.9+, CMake, Make and g++. TILEXR_VALIDATION_JOBS controls build jobs.
Reports: run/validation/commit-*/summary.json (or TILEXR_VALIDATION_RESULT_ROOT).
Return the summary and logs before proceeding to the next checkpoint.
EOF
        exit 0 ;;
    *) printf '[commit-validate] Failed: use no arguments or --resume PRIOR_SUMMARY.json (see --help)\n' >&2; exit 2 ;;
esac
# This entry point follows the current delivery checkpoint, independent of a
# previous Dispatch build or deployment-specific case options.
export TILEXR_VALIDATION_CHECKPOINT=ep-sdedup-combine-p2-m12
export TILEXR_VALIDATION_BUILD_MODE=production
export TILEXR_VALIDATION_ACTION=commit-validate
export TILEXR_VALIDATION_BOUNDARY_PROBE=0
export TILEXR_VALIDATION_BREAKDOWN=0
if ! source "$(dirname -- "${BASH_SOURCE[0]}")/set_env.sh"; then
    printf '失败 COMMIT_VALIDATION_FAIL checkpoint=%s type=CONFIG_FAILED (environment setup; no build or tests started)\n' "$TILEXR_VALIDATION_CHECKPOINT" >&2
    exit 1
fi
exec "$TILEXR_VALIDATION_PYTHON" -u "$TILEXR_VALIDATION_ROOT/tools/validation/workflow.py" commit-validate
