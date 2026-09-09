#!/usr/bin/env bash
set -eo pipefail
source "$(dirname -- "${BASH_SOURCE[0]}")/set_env.sh"
exec "$TILEXR_VALIDATION_PYTHON" "$TILEXR_VALIDATION_ROOT/tools/validation/workflow.py" run "$@"
