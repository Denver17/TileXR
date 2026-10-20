#!/usr/bin/env python3
"""Run with torchrun on an already qualified single-node 910B environment."""
import json
import os
from datetime import timedelta
from pathlib import Path

import torch
import torch_npu  # noqa: F401
import torch.distributed as dist
from vllm_ascend.distributed.device_communicators.npu_communicator import NPUCommunicator
from tilexr_collectives.vllm_patch import patch_npu_communicator


def check(comm, members, dtype):
    rank = dist.get_rank()
    size = len(members)
    device = f'npu:{rank}'
    total = sum(r + 1 for r in members)
    x = torch.full((4, size, 16), rank + 1, dtype=dtype, device=device)
    reduced = comm.all_reduce(x)
    assert reduced is x
    torch.npu.synchronize()
    assert torch.equal(x.cpu(), torch.full(x.shape, total, dtype=dtype))

    source = torch.full((4, size, 16), rank + 1, dtype=dtype, device=device)
    # Non-contiguous input and a nonzero gather dimension.
    source = source.transpose(0, 1)
    gathered = comm.all_gather(source, dim=-1)
    torch.npu.synchronize()
    assert torch.equal(gathered.cpu(), torch.cat([
        torch.full(source.shape, r + 1, dtype=dtype) for r in members
    ], dim=-1))

    source = torch.full((4, size, 16), rank + 1, dtype=dtype, device=device)
    scattered = comm.reduce_scatter(source, dim=2)
    torch.npu.synchronize()
    reference = torch.full(source.movedim(0, 2).shape, total, dtype=dtype)
    reference = reference.chunk(size, dim=0)[comm.rank].movedim(0, 2).contiguous()
    assert torch.equal(scattered.cpu(), reference)

    root = size - 1
    source = torch.full((4, size, 16), rank + 1, dtype=dtype, device=device).transpose(0, 1)
    result = comm.broadcast(source, src=root)
    assert result is source
    torch.npu.synchronize()
    assert torch.equal(source.cpu(), torch.full(source.shape, members[root] + 1, dtype=dtype))

    # Alternate execution streams with no device synchronize between launches.
    streams = [torch.npu.Stream(), torch.npu.Stream()]
    outputs = []
    for i in range(4):
        with torch.npu.stream(streams[i % 2]):
            tensor = torch.full((256,), rank + 1, dtype=dtype, device=device)
            outputs.append(comm.all_reduce(tensor))
    torch.npu.synchronize()
    assert all(torch.equal(x.cpu(), torch.full((256,), total, dtype=dtype)) for x in outputs)


def main():
    rank = int(os.environ['RANK'])
    size = int(os.environ['WORLD_SIZE'])
    torch.npu.set_device(rank)
    dist.init_process_group('gloo', timeout=timedelta(seconds=120))
    patch_npu_communicator()
    all_members = list(range(size))
    groups = []
    for members in [all_members, all_members, all_members[::2], all_members[1::2]]:
        cpu = dist.new_group(members, backend='gloo')
        device = dist.new_group(members, backend='hccl')
        if rank in members:
            groups.append((cpu, device, members))
    # One rank cannot load the native artifact: every member must fail startup
    # before any rank enters native bootstrap. Restore selection and reuse groups.
    saved_library = os.environ.get('TILEXR_COLLECTIVES_LIB')
    if rank == 1:
        os.environ['TILEXR_COLLECTIVES_LIB'] = '/nonexistent/tilexr-validation.so'
    try:
        cpu, device, _ = groups[0]
        try:
            NPUCommunicator(cpu, torch.device('npu', rank), device, unique_name='startup-failure')
        except RuntimeError as exc:
            assert 'global rank 1' in str(exc) and 'missing library' in str(exc)
        else:
            raise AssertionError('missing artifact did not fail the whole group')
    finally:
        if saved_library is None:
            os.environ.pop('TILEXR_COLLECTIVES_LIB', None)
        else:
            os.environ['TILEXR_COLLECTIVES_LIB'] = saved_library

    records = []
    for cycle in range(2):
        communicators = [NPUCommunicator(cpu, torch.device('npu', rank), device,
                                        unique_name=f'validation:{cycle}:{i}')
                         for i, (cpu, device, members) in enumerate(groups)]
        domains = [c._tilexr_collectives_adapter.domain for c in communicators]
        assert len(set(domains)) == len(domains)
        for comm, (_, _, members) in zip(communicators, groups):
            for dtype in [torch.float16, torch.bfloat16]:
                for iteration in range(2):
                    check(comm, members, dtype)
            counts = comm._tilexr_collectives_route_counts
            assert counts['all_reduce'] == dict(tilexr=20, fallback=0, error=0)
            for op in ['all_gather', 'reduce_scatter', 'broadcast']:
                assert counts[op] == dict(tilexr=4, fallback=0, error=0)
            records.append(dict(cycle=cycle, members=members, domain=comm._tilexr_collectives_adapter.domain,
                                counts=counts))
        for comm in communicators:
            comm.destroy()
            comm.destroy()  # Idempotent native cleanup.
    row = dict(rank=rank, correct=True, startup_failure_agreed=True, groups=records)
    print('GROUP_VALIDATION', json.dumps(row), flush=True)
    if os.getenv('TILEXR_VALIDATION_OUTPUT'):
        Path(os.environ['TILEXR_VALIDATION_OUTPUT'], f'rank{rank}.json').write_text(json.dumps(row, indent=2))
    dist.destroy_process_group()


if __name__ == '__main__':
    main()
