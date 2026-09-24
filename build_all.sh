#!/usr/bin/env bash
set -eo pipefail
breakdown=0
dedup=1
for argument in "$@"; do
    case "$argument" in
        dedup=0|dedup=1) dedup=${argument#*=} ;;
        breakdown=0|breakdown=1) breakdown=${argument#*=} ;;
        -h|--help)
            printf 'Usage: bash build_all.sh [breakdown=0|1] [dedup=0|1]\nDefault: breakdown=0 (profiling disabled), dedup=1.\n'
            exit 0 ;;
        *) printf '[build] Failed: unknown argument: %s (expected breakdown=0|1 or dedup=0|1)\n' "$argument" >&2; exit 2 ;;
    esac
done
printf '[build] Loading environment\n'
unset TILEXR_VALIDATION_CHECKPOINT TILEXR_INTERNAL_ACCEPTANCE_STAGE
export TILEXR_VALIDATION_ACTION=build
source "$(dirname -- "${BASH_SOURCE[0]}")/set_env.sh"
# Build options are explicit and independent of all runtime case parameters.
export TILEXR_VALIDATION_BREAKDOWN="$breakdown"
export TILEXR_VALIDATION_DEDUP="$dedup"
if [[ "$breakdown" == 1 ]]; then
    if [[ "$TILEXR_VALIDATION_BUILD_MODE" != production ]]; then
        printf '[build] Failed: breakdown=1 requires production mode\n' >&2
        exit 2
    fi
fi
printf '[build] breakdown=%s dedup=%s\n' "$breakdown" "$dedup"
exec "$TILEXR_VALIDATION_PYTHON" -u "$TILEXR_VALIDATION_ROOT/tools/validation/workflow.py" build
