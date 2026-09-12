#!/usr/bin/env bash
set -eo pipefail
breakdown=0
for argument in "$@"; do
    case "$argument" in
        breakdown=0|breakdown=1) breakdown=${argument#*=} ;;
        -h|--help)
            printf 'Usage: bash build_all.sh [breakdown=0|1]\nDefault: breakdown=0 (profiling disabled).\n'
            exit 0 ;;
        *) printf '[build] Failed: unknown argument: %s (expected breakdown=0 or breakdown=1)\n' "$argument" >&2; exit 2 ;;
    esac
done
printf '[build] Loading environment\n'
source "$(dirname -- "${BASH_SOURCE[0]}")/set_env.sh"
# Production builds select the current increment; sync/run inherit its manifest.
if [[ "$TILEXR_VALIDATION_BUILD_MODE" == production ]]; then
    export TILEXR_VALIDATION_CHECKPOINT="${TILEXR_VALIDATION_CHECKPOINT:-ep-sdedup-p1-chain-multi}"
fi
# Build options are explicit; stale case variables must not enable profiling.
export TILEXR_VALIDATION_RUN_MODE=functional TILEXR_VALIDATION_LOOPS=1 TILEXR_VALIDATION_WARMUP=0
if [[ "$breakdown" == 1 ]]; then
    if [[ "$TILEXR_VALIDATION_BUILD_MODE" != production || "$TILEXR_VALIDATION_CHECKPOINT" != ep-sdedup-p1-chain-multi ]]; then
        printf '[build] Failed: breakdown=1 requires the production cnt/cumsum checkpoint\n' >&2
        exit 2
    fi
    export TILEXR_VALIDATION_RUN_MODE=breakdown
fi
printf '[build] breakdown=%s\n' "$breakdown"
exec "$TILEXR_VALIDATION_PYTHON" -u "$TILEXR_VALIDATION_ROOT/tools/validation/workflow.py" build
