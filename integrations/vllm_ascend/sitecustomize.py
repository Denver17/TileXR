from __future__ import annotations

import os


value = os.environ.get("VLLM_ASCEND_TILEXR_COLLECTIVES", "")
if value.strip().lower() in {"1", "true", "yes", "on"}:
    try:
        from tilexr_collectives.vllm_patch import patch_npu_communicator

        patch_npu_communicator()
    except Exception as exc:
        # Python ignores ordinary sitecustomize import errors. Explicit opt-in
        # must stop startup rather than silently run the original backend.
        raise SystemExit(f"TileXR bootstrap failed: {type(exc).__name__}: {exc}") from exc
