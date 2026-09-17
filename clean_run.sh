#!/usr/bin/env bash
# Remove only run/ contents from this test deployment on the hostfile nodes.
set -eo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
args=()
for arg in "$@"; do
    case "$arg" in
        -h|--help)
            cat <<'HELP'
Usage: bash clean_run.sh [--dry-run] [hostfile=PATH]
Clean this deployment's run/ contents on every hostfile node, including local.
Stop tests first. Logs, traces and sync receipts are removed; run/ is retained.
Run bash sync.sh before starting the next test. No CANN or NPU access is needed.
  --dry-run       Check targets and show entry counts without deleting anything.
  hostfile=PATH   Override the configured hostfile (relative to this directory).
HELP
            exit 0;;
        --dry-run) args+=("$arg");;
        hostfile=*) args+=(--hostfile "${arg#*=}");;
        *) printf 'CLEAN_FAIL unknown argument: %s\n' "$arg" >&2; exit 2;;
    esac
done
# Load deployment settings only: cleanup also works when CANN is unavailable.
if [[ -f validation.local.sh ]]; then source ./validation.local.sh; fi
export TILEXR_VALIDATION_HOSTFILE="${TILEXR_VALIDATION_HOSTFILE:-hostfile}"
export TILEXR_VALIDATION_SSH_USER="${TILEXR_VALIDATION_SSH_USER:-}"
exec "${TILEXR_VALIDATION_PYTHON:-python3}" tools/validation/clean_run.py "${args[@]}"
