#!/usr/bin/env bash
set -eo pipefail
printf '[sync] Loading environment\n'
source "$(dirname -- "${BASH_SOURCE[0]}")/set_env.sh"
exec "$TILEXR_VALIDATION_PYTHON" -u "$TILEXR_VALIDATION_ROOT/tools/validation/workflow.py" sync "$@"
