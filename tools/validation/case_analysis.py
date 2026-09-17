"""Post-run analysis of the actual saved expert lists, independent of timing."""

import hashlib
from pathlib import Path
import statistics
import struct


def analyze_rank(path, rank, config):
    ranks, experts = config["rank_size"], config["moe_expert_num"]
    bs, topk = config["bs"], config["topk"]
    if (ranks <= 0 or ranks % 8 or not 0 <= rank < ranks or experts <= 0
            or experts % ranks or bs <= 0 or not 0 < topk <= experts):
        raise ValueError("invalid case analysis shape or rank")
    data = Path(path).read_bytes()
    if len(data) != bs * topk * 4:
        raise ValueError(f"rank {rank}: expert ID file size does not match BS * TopK * int32")
    experts_per_server = experts // ranks * 8
    source_server = rank // 8
    before, after = 0, 0
    # A5's retained input format is little-endian int32 [BS, TopK]. Group only
    # within each token: different tokens never share one network packet.
    for token_ids in struct.iter_unpack(f"<{topk}i", data):
        destinations = set()
        for expert in token_ids:
            if not 0 <= expert < experts:
                raise ValueError(f"rank {rank}: expert ID {expert} outside [0, {experts})")
            server = expert // experts_per_server
            if server != source_server:
                before += 1
                destinations.add(server)
        after += len(destinations)
    return dict(rank=rank, before_tokens=before, after_tokens=after,
                dedup_percent=100.0 * (before - after) / before if before else 0.0,
                expert_ids_sha256=hashlib.sha256(data).hexdigest())


def analyze_case(directory, config, records):
    """Called by the coordinator after every node has exited and logs arrived."""
    ranks = config["rank_size"]
    actual = [record.get("rank") for record in records]
    if (len(actual) != ranks or any(type(rank) is not int for rank in actual)
            or set(actual) != set(range(ranks))):
        raise ValueError("case analysis requires completed records for every rank")
    rows = []
    for rank in range(ranks):
        node = Path(directory) if rank < 8 else Path(directory) / "nodes" / str(rank // 8)
        rows.append(analyze_rank(node / "inputs" / f"rank-{rank}-ids.bin", rank, config))
    rates = [row["dedup_percent"] for row in rows]
    return dict(status="complete", phase="after_npu", unit="percent",
                formula="100 * (A - B) / A", zero_cross_server_policy="0% included in rank average",
                rank_count=ranks, zero_cross_server_ranks=sum(row["before_tokens"] == 0 for row in rows),
                min=min(rates), max=max(rates), avg=statistics.mean(rates), ranks=rows)
