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
export TILEXR_VALIDATION_ACTION=build
source "$(dirname -- "${BASH_SOURCE[0]}")/set_env.sh"
# Production builds select the current increment; sync/run inherit its manifest.
if [[ "$TILEXR_VALIDATION_BUILD_MODE" == production ]]; then
    export TILEXR_VALIDATION_CHECKPOINT="${TILEXR_VALIDATION_CHECKPOINT:-ep-sdedup-p2-dispatch}"
fi
# Build options are explicit and independent of all runtime case parameters.
export TILEXR_VALIDATION_BREAKDOWN="$breakdown"
if [[ "$breakdown" == 1 ]]; then
    if [[ "$TILEXR_VALIDATION_BUILD_MODE" != production || ( "$TILEXR_VALIDATION_CHECKPOINT" != ep-sdedup-p2-dispatch && "$TILEXR_VALIDATION_CHECKPOINT" != ep-sdedup-combine-p1-profile && "$TILEXR_VALIDATION_CHECKPOINT" != ep-sdedup-combine-p2-m12 && "$TILEXR_VALIDATION_CHECKPOINT" != ep-sdedup-combine-p2-m3 && "$TILEXR_VALIDATION_CHECKPOINT" != ep-sdedup-combine-p2-m4 && "$TILEXR_VALIDATION_CHECKPOINT" != ep-sdedup-combine-p2-m5 ) ]]; then
        printf '[build] Failed: breakdown=1 requires production Dispatch or Combine C6/P2.1/P2.2/P2.3/P2.4\n' >&2
        exit 2
    fi
fi
printf '[build] breakdown=%s\n' "$breakdown"
exec "$TILEXR_VALIDATION_PYTHON" -u "$TILEXR_VALIDATION_ROOT/tools/validation/workflow.py" build
