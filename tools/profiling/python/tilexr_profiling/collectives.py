# 根据 collective 执行计划生成预期参与者和共享 capture UUID，供 launcher 产生的 manifest shards 合并使用。
# Derive expected participants and shared capture UUIDs from collective plans for merging launcher-produced manifest shards.
"""Create the collective participant plan independently of returned shards."""

from uuid import UUID

from .manifest import MANIFEST_SCHEMA, load_manifest, mapping_fingerprint
from .model import ProfilingInputError


def collective_capture_id(seed, ordinal):
    """Predict the bounded queue's capture UUID from its shared seed before launch."""
    if isinstance(ordinal, bool) or not isinstance(ordinal, int) or not 0 <= ordinal < 4:
        raise ProfilingInputError("collective capture ordinal must be in [0, 3]")
    try:
        value = UUID(seed)
    except (ValueError, TypeError, AttributeError) as exc:
        raise ProfilingInputError("collective seed must be a UUID") from exc
    if not value.int:
        raise ProfilingInputError("collective seed must be nonzero")
    raw = bytearray(value.bytes)
    raw[15] ^= ordinal + 1
    raw[6] = (raw[6] & 15) | 64
    raw[8] = (raw[8] & 63) | 128
    return str(UUID(bytes=bytes(raw)))


def build_collective_expected(mapping, capture_id, rank_size, logical_lane_count, *, attempt_id=None):
    """Build an expected manifest before execution from the actual launch plan.

    Identity and stage metrics are supplied by per-rank producer shards. Queue
    capture IDs (and default attempt IDs) come from the shared seed and ordinal;
    this helper never discovers participants by inspecting those shards.
    """
    for value, label in ((rank_size, "rank_size"), (logical_lane_count, "logical_lane_count")):
        if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= 65536:
            raise ProfilingInputError(label + " must be an integer in [1, 65536]")
    if rank_size * logical_lane_count > 65536:
        raise ProfilingInputError("collective expected participant count exceeds manifest limit")
    try:
        canonical_id = str(UUID(capture_id))
    except (ValueError, TypeError, AttributeError) as exc:
        raise ProfilingInputError("collective capture_id must be a UUID") from exc
    manifest = {"schema": MANIFEST_SCHEMA, "capture_id": canonical_id,
                "attempt_id": canonical_id if attempt_id is None else attempt_id,
                "mapping_sha256": mapping_fingerprint(mapping),
                "participants": [{"stream_id": (rank << 16) | lane, "rank": rank,
                                  "logical_lane": lane, "sampled": True}
                                 for rank in range(rank_size) for lane in range(logical_lane_count)],
                "stage_metrics": []}
    return load_manifest(manifest, mapping)
