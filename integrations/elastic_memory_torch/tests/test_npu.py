#!/usr/bin/env python3
# 双 rank Torch NPU 测试，通过 tilexr_elastic 调用生产紧凑 Dispatch/Combine，
# 用独立路由 oracle 和普通 Tensor 运算检查 FP16/BF16 输出、零接收及变化的计数/复制核。
# 同一个 communicator 按顺序完成各轮；--failure 单独注入 rank0 预算不足，检查两 rank
# 报错及失败后的准入拒绝。每个进程由参数选择 rank、设备和通信域。
#
# 生命周期用例主动丢弃输入或 Pending，保留 Storage view，并在显式登记的 stream 上
# 放置延迟消费者，检查扩容、trim 和 close 不提前回收仍被使用的输出。回读比较前等待
# 相应工作完成，释放引用后通过 collect 等待退休；测试依赖真实 torch_npu 与 NPU 环境。
import argparse
import gc
import json
import os
import time

import torch
import torch_npu

from tilexr_elastic import DeepEPBuffer, ElasticMemory


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def report(rank, case, pool, **extra):
    stats = pool.snapshot()
    require(stats["live_bytes"] + stats["pending_retire_bytes"] + stats["free_bytes"]
            == stats["committed_bytes"], "pool accounting conservation")
    print(json.dumps(dict(rank=rank, case=case, status="PASS", stats=stats, **extra)), flush=True)


def inputs(rank, bs, zero=False, dtype=torch.bfloat16, experts=4):
    values = torch.arange(bs, dtype=torch.float32) + rank * 16 + 1
    hidden = values[:, None].expand(bs, 128).contiguous().to("npu", dtype=dtype)
    tokens = torch.arange(bs, dtype=torch.int32)[:, None]
    routes = ((tokens * 2 + torch.arange(2, dtype=torch.int32)[None, :] + rank) % experts)
    if zero:
        routes = torch.arange(2, dtype=torch.int32)[None, :].expand(bs, 2).contiguous()
    return hidden, routes.to("npu")


def oracle(rank, bs, zero=False, dtype=torch.bfloat16, experts=4):
    assist, values, counts, expert_counts = [], [], [], []
    for expert in range(rank * (experts // 2), (rank + 1) * (experts // 2)):
        before = len(values)
        for source in range(2):
            for token in range(bs):
                for k in range(2):
                    route = k if zero else (token * 2 + k + source) % experts
                    if route == expert:
                        assist.append([source, token, k])
                        values.append(source * 16 + token + 1)
            counts.append(len(values))
        expert_counts.append(len(values) - before)
    hidden = torch.tensor(values, dtype=dtype)[:, None].expand(len(values), 128)
    return (hidden, torch.tensor(assist, dtype=torch.int32).reshape(-1, 3),
            torch.tensor(counts, dtype=torch.int32), torch.tensor(expert_counts, dtype=torch.int64))


def verify(result, rank, bs, zero=False, dtype=torch.bfloat16, experts=4):
    expected = oracle(rank, bs, zero, dtype, experts)
    require(type(result.hidden) is torch.Tensor, "output must be an ordinary torch.Tensor")
    require(result.hidden.device.type == "npu", "output must reside on NPU")
    for actual, wanted in zip((result.hidden, result.assist, result.send_counts, result.expert_counts), expected):
        torch.testing.assert_close(actual.cpu(), wanted, rtol=0, atol=0)
    if result.hidden.numel():
        require(result.hidden.data_ptr() == result.allocation_address, "from_blob copied the hidden data")


def retire(pool):
    gc.collect()
    deadline = time.monotonic() + 10
    while pool.snapshot()["live_bytes"] or pool.snapshot()["pending_retire_bytes"]:
        if time.monotonic() > deadline:
            raise AssertionError("leases did not retire")
        pool.collect()
        time.sleep(0.002)


def run(args):
    torch.npu.set_device(args.device)
    pool = ElasticMemory(device=f"npu:{args.device}",
                         max_committed_bytes=64 if args.failure and args.rank == 0 else 4 << 20,
                         max_cached_free_bytes=4 << 20,
                         segment_bytes=64 if args.failure and args.rank == 0 else 4096)
    require(pool.snapshot()["committed_bytes"] == 0, "initial data HBM must be zero")
    buffer = DeepEPBuffer(output_pool=pool, rank=args.rank, world_size=2,
                          comm_domain=args.comm_domain, staging_mib=2048)
    print(json.dumps(dict(rank=args.rank, torch=torch.__version__, torch_npu=torch_npu.__version__,
                          device=args.device, staging_bytes=buffer.staging_bytes,
                          peer_window_bytes=buffer.peer_window_bytes, memory_domain_guard_bytes=1 << 20)), flush=True)

    if args.failure:
        hidden, routes = inputs(args.rank, 16)
        pending = buffer.dispatch_compact_async(hidden, topk_experts=routes, num_experts=4)
        try:
            pending.materialize()
        except RuntimeError as error:
            print(json.dumps(dict(rank=args.rank, expected_failure=str(error))), flush=True)
        else:
            raise AssertionError("single-rank budget failure did not terminate the entire call")
        require(pool.snapshot()["failure_count"] == 1, "failed Device call must poison the pool")
        try:
            buffer.dispatch_compact_async(hidden, topk_experts=routes, num_experts=4)
        except RuntimeError:
            pass
        else:
            raise AssertionError("poisoned communicator admitted another call")
        pool.close()
        report(args.rank, "rank_failure_propagation", pool)
        return

    grad_input, grad_routes = inputs(args.rank, 16)
    grad_input.requires_grad_(True)
    try:
        buffer.dispatch_compact_async(grad_input, topk_experts=grad_routes, num_experts=4)
    except RuntimeError as error:
        require("inference" in str(error), "requires_grad rejection must explain inference-only support")
    else:
        raise AssertionError("requires_grad input was silently accepted")
    del grad_input, grad_routes

    prior_growth = None
    for index, bs in enumerate((16, 16, 64, 16)):
        hidden, routes = inputs(args.rank, bs)
        pending = buffer.dispatch_compact_async(hidden, topk_experts=routes, num_experts=4)
        # Native progression must not require a Python callback or materialize to start.
        time.sleep(0.02)
        result = pending.materialize()
        require(pending.materialize().hidden.untyped_storage().data_ptr()
                == result.hidden.untyped_storage().data_ptr(), "materialize changed Storage")
        verify(result, args.rank, bs)
        expert = result.hidden * 2  # Ordinary Torch operator on external NPU Storage.
        weights = torch.full((bs, 2), 0.5, device="npu", dtype=torch.float32)
        combined = buffer.combine_compact(result.handle, expert, topk_weights=weights)
        torch.testing.assert_close(combined.cpu(), hidden.cpu() * 2, rtol=0, atol=0)
        growth = pool.snapshot()["growth_count"]
        if index == 1:
            require(growth == prior_growth, "second same-size call must be a hot hit")
        if index == 2:
            require(growth > prior_growth, "larger output must grow the initially small segments")
        prior_growth = growth
        del result, pending, expert, weights, combined
        retire(pool)
        report(args.rank, ("cold", "hot", "growth", "fall_back")[index], pool)

    # Vary both count-flag dimensions across completed calls on the same communicator:
    # 32 experts / bs256 uses two count cores; blocks 2/4/9 make the owner copy.
    # Nine blocks also crosses a 256B vector-repeat scratch boundary.
    saved_blocks = os.environ.get("TILEXR_EP_DISPATCH_MEMORY_BLOCK_DIM")
    try:
        for index, (blocks, bs, experts) in enumerate(((64, 256, 32), (2, 16, 4),
                (4, 16, 4), (8, 16, 4), (9, 256, 32), (64, 256, 32), (64, 16, 4))):
            os.environ["TILEXR_EP_DISPATCH_MEMORY_BLOCK_DIM"] = str(blocks)
            hidden, routes = inputs(args.rank, bs, experts=experts)
            result = buffer.dispatch_compact(hidden, topk_experts=routes, num_experts=experts)
            verify(result, args.rank, bs, experts=experts)
            weights = torch.full((bs, 2), 0.5, device="npu", dtype=torch.float32)
            combined = buffer.combine_compact(result.handle, result.hidden * 2, topk_weights=weights)
            torch.testing.assert_close(combined.cpu(), hidden.cpu() * 2, rtol=0, atol=0)
            del result, weights, combined
            retire(pool)
            report(args.rank, "count_copy_core_transition", pool,
                   transition=index, blocks=blocks, bs=bs, experts=experts)
    finally:
        if saved_blocks is None:
            os.environ.pop("TILEXR_EP_DISPATCH_MEMORY_BLOCK_DIM", None)
        else:
            os.environ["TILEXR_EP_DISPATCH_MEMORY_BLOCK_DIM"] = saved_blocks

    # Exercise the second supported dtype through both real production operators.
    hidden, routes = inputs(args.rank, 16, dtype=torch.float16)
    result = buffer.dispatch_compact(hidden, topk_experts=routes, num_experts=4)
    verify(result, args.rank, 16, dtype=torch.float16)
    weights = torch.full((16, 2), 0.5, device="npu", dtype=torch.float32)
    combined = buffer.combine_compact(result.handle, result.hidden * 2, topk_weights=weights)
    torch.testing.assert_close(combined.cpu(), hidden.cpu() * 2, rtol=0, atol=0)
    del result, weights, combined
    retire(pool)
    report(args.rank, "fp16_dispatch_combine", pool)

    # The native launch owns inputs even if Python releases them before waiting.
    hidden, routes = inputs(args.rank, 16)
    pending = buffer.dispatch_compact_async(hidden, topk_experts=routes, num_experts=4)
    del hidden, routes
    gc.collect()
    result = pending.materialize()
    del pending
    gc.collect()
    verify(result, args.rank, 16)
    result.discard()
    del result
    retire(pool)
    report(args.rank, "dropped_inputs_and_materialized_pending", pool)

    # Dropping an unmaterialized Pending must neither stop native progression nor
    # leak its produced allocation or permanently retain the communicator turn.
    hidden, routes = inputs(args.rank, 16)
    abandoned = buffer.dispatch_compact_async(hidden, topk_experts=routes, num_experts=4)
    del abandoned, hidden, routes
    gc.collect()
    pool.trim(wait=True)  # FIFO service command observes completion of the abandoned call.
    retire(pool)
    require(pool.snapshot()["failure_count"] == 0, "abandoned Pending poisoned a successful call")
    hidden, routes = inputs(args.rank, 16)
    result = buffer.dispatch_compact(hidden, topk_experts=routes, num_experts=4)
    verify(result, args.rank, 16)
    result.discard()
    del result
    retire(pool)
    report(args.rank, "dropped_pending_before_materialize", pool)

    hidden, routes = inputs(args.rank, 16, zero=True)
    result = buffer.dispatch_compact(hidden, topk_experts=routes, num_experts=4)
    verify(result, args.rank, 16, zero=True)
    if args.rank == 1:
        require(result.hidden.shape == (0, 128), "rank without incoming tokens must expose zero rows")
    weights = torch.full((16, 2), 0.5, device="npu", dtype=torch.float32)
    combined = buffer.combine_compact(result.handle, result.hidden * 2, topk_weights=weights)
    torch.testing.assert_close(combined.cpu(), hidden.cpu() * 2, rtol=0, atol=0)
    del result, weights, combined
    retire(pool)
    report(args.rank, "zero_receive", pool, combine_verified=True)

    hidden, routes = inputs(args.rank, 16)
    result = buffer.dispatch_compact(hidden, topk_experts=routes, num_experts=4)
    alias = result.hidden.view(-1)
    expected_alias = result.hidden.cpu().view(-1)
    result.discard()
    del result
    gc.collect()
    require(pool.trim()["live_bytes"] > 0, "a Tensor view must retain the external Storage lease")
    torch.testing.assert_close(alias.cpu(), expected_alias, rtol=0, atol=0)
    del alias
    retire(pool)
    require(pool.trim()["committed_bytes"] == 0, "trim must physically return empty data segments")
    report(args.rank, "view_and_trim", pool)

    # Keep only an old Storage view while a new, larger call needs fresh HBM.
    # Trimming first makes the growth assertion independent of previous cache sizes.
    require(pool.trim()["committed_bytes"] == 0, "old-view growth test requires an empty cache")
    old_result = buffer.dispatch_compact(hidden, topk_experts=routes, num_experts=4)
    old_view = old_result.hidden.view(-1)
    old_address = old_view.data_ptr()
    old_expected = oracle(args.rank, 16)[0].reshape(-1)
    old_growth = pool.snapshot()["growth_count"]
    old_result.discard()
    del old_result
    gc.collect()
    large_input, large_routes = inputs(args.rank, 128)
    large_result = buffer.dispatch_compact(large_input, topk_experts=large_routes, num_experts=4)
    verify(large_result, args.rank, 128)
    require(pool.snapshot()["growth_count"] > old_growth, "fresh growth did not occur with old view live")
    require(old_view.data_ptr() == old_address, "growth moved the live old Storage")
    require(large_result.hidden.data_ptr() != old_address, "growth reused a live old allocation")
    torch.testing.assert_close(old_view.cpu(), old_expected, rtol=0, atol=0)
    large_result.discard()
    del large_result, large_input, large_routes
    gc.collect()
    require(pool.trim()["live_bytes"] > 0, "trim retired the still-live old view")
    torch.testing.assert_close(old_view.cpu(), old_expected, rtol=0, atol=0)
    del old_view
    retire(pool)
    require(pool.trim()["committed_bytes"] == 0, "last old view did not release its segment")
    report(args.rank, "old_view_survives_new_growth", pool)

    result = buffer.dispatch_compact(hidden, topk_experts=routes, num_experts=4)
    consumer = torch.npu.Stream(device=args.device)
    with result.use_on_stream(consumer):
        consumed = result.hidden * 3
    result.discard()
    expected = oracle(args.rank, 16)[0] * 3
    del result
    gc.collect()
    # trim waits retirement events; consuming on another stream must not permit early reuse/free.
    pool.trim(wait=True)
    consumer.synchronize()
    torch.testing.assert_close(consumed.cpu(), expected, rtol=0, atol=0)
    del consumed
    retire(pool)
    report(args.rank, "cross_stream", pool)

    # Finite, public Torch operations put a real dependency ahead of the consumer.
    # No unrecorded-event wait, private sleep primitive, or device-side infinite loop.
    result = buffer.dispatch_compact(hidden, topk_experts=routes, num_experts=4)
    old_address = result.hidden.data_ptr()
    delayed_consumer = torch.npu.Stream(device=args.device)
    gate_stream = torch.npu.Stream(device=args.device)
    gate = torch.npu.Event()
    finished = torch.npu.Event()
    with torch.npu.stream(gate_stream):
        matrix_a = torch.eye(4096, device="npu", dtype=torch.float32)
        matrix_b = torch.eye(4096, device="npu", dtype=torch.float32)
        matrix_out = torch.empty_like(matrix_a)
        for _ in range(64):
            torch.mm(matrix_a, matrix_b, out=matrix_out)
            matrix_a, matrix_out = matrix_out, matrix_a
        gate.record()
    with result.use_on_stream(delayed_consumer):
        delayed_consumer.wait_event(gate)
        delayed_value = result.hidden * 5
        finished.record()
    queued_consumer_pending = not finished.query()
    observed_pending = False
    result.discard()
    del result
    gc.collect()
    before_reuse = pool.collect()
    if not finished.query():
        observed_pending = True
        require(before_reuse["live_bytes"] + before_reuse["pending_retire_bytes"] > 0,
                "unfinished cross-stream read lost its lease before completion")

    # Changed payload makes early reuse detectable by the delayed consumer oracle.
    changed_input = hidden + 128
    new_result = buffer.dispatch_compact(changed_input, topk_experts=routes, num_experts=4)
    if not finished.query():
        observed_pending = True
        require(new_result.hidden.data_ptr() != old_address,
                "new Dispatch reused an allocation with an unfinished consumer")
    torch.testing.assert_close(new_result.hidden.cpu(), oracle(args.rank, 16)[0] + 128, rtol=0, atol=0)
    new_result.discard()
    del new_result, changed_input
    gc.collect()
    pool.trim(wait=True)
    delayed_consumer.synchronize()
    gate_stream.synchronize()
    torch.testing.assert_close(delayed_value.cpu(), oracle(args.rank, 16)[0] * 5, rtol=0, atol=0)
    require(observed_pending, "delay workload completed too early to cover in-flight consumer retirement")
    del delayed_value, matrix_a, matrix_b, matrix_out
    retire(pool)
    report(args.rank, "delayed_consumer_prevents_early_reuse", pool, observed_pending_after_drop=True,
           queued_consumer_pending=queued_consumer_pending, delay_workspace_bytes=3 * 4096 * 4096 * 4)

    result = buffer.dispatch_compact(hidden, topk_experts=routes, num_experts=4)
    live_tensor = result.hidden.view(-1)
    result.discard()
    del result
    gc.collect()
    closed = pool.close()
    require(closed["live_bytes"] > 0, "close must not invalidate live tensors")
    torch.testing.assert_close(live_tensor.cpu(), oracle(args.rank, 16)[0].reshape(-1), rtol=0, atol=0)
    del live_tensor
    retire(pool)
    require(pool.trim()["committed_bytes"] == 0, "last Storage after close must return its HBM")
    report(args.rank, "close_with_live_storage", pool)
    try:
        buffer.dispatch_compact_async(hidden, topk_experts=routes, num_experts=4)
    except RuntimeError:
        pass
    else:
        raise AssertionError("closed pool admitted Dispatch")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--rank", type=int, choices=(0, 1), required=True)
    parser.add_argument("--device", type=int, required=True)
    parser.add_argument("--comm-domain", type=int, required=True)
    parser.add_argument("--failure", action="store_true", help="inject a rank-0 output-pool budget failure")
    arguments = parser.parse_args()
    if not os.environ.get("TILEXR_COMM_ID"):
        parser.error("set the same TILEXR_COMM_ID=IPv4:PORT for both ranks")
    run(arguments)
