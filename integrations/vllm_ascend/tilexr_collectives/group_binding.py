"""One native communicator per framework CPU group; control traffic is startup-only."""
from __future__ import annotations

import os
from itertools import count

from .runtime import TileXRCollectivesRuntime

_root_sequence = count(1)


def bind_group(communicator, install_prefix: str):
    import torch
    import torch.distributed as dist

    from .vllm_adapter import TileXRVllmCollectivesAdapter

    group = communicator.cpu_group
    members = tuple(dist.get_process_group_ranks(group))
    rank = dist.get_rank(group)
    size = dist.get_world_size(group)
    device = int(communicator.device)
    torch.npu.set_device(device)

    runtime = None
    error = None
    try:
        runtime = TileXRCollectivesRuntime(rank, size, install_prefix, initialize=False)
        runtime.device_index = device
    except (OSError, RuntimeError, AttributeError, ValueError) as exc:
        error = f"global rank {dist.get_rank()}: {type(exc).__name__}: {exc}"
    readiness = [None] * size
    dist.all_gather_object(readiness, error, group=group)
    failures = [value for value in readiness if value is not None]
    if failures:
        raise RuntimeError("TileXR group initialization failed: " + "; ".join(failures))

    metadata = [None]
    if rank == 0:
        # Distinct root/sequence pairs have distinct domains. Reserve a device
        # range because the native default endpoint also includes root device ID.
        domain = (next(_root_sequence) * dist.get_world_size() + members[0]) * torch.npu.device_count()
        try:
            endpoint = os.environ.get("TILEXR_COMM_ID")
            port = int(endpoint.rsplit(":", 1)[1]) if endpoint else 10067 + device
            if domain + port > 65535:
                raise RuntimeError("TileXR domain exceeds bootstrap port range")
            metadata[0] = (domain, runtime.unique_id(domain), None)
        except (OSError, RuntimeError, AttributeError, ValueError) as exc:
            metadata[0] = (domain, None, str(exc))
    dist.broadcast_object_list(metadata, src=members[0], group=group)
    domain, uid, error = metadata[0]
    if error is not None:
        raise RuntimeError(f"TileXR unique ID failed: {error}")
    runtime.initialize(uid)
    return TileXRVllmCollectivesAdapter(
        rank=rank, world_size=size, install_prefix=install_prefix,
        runtime=runtime, domain=domain, members=members,
    )
