"""Aggregate Worker-local Host batch averages; never parse profiler artifacts."""
import math
import statistics

def summarize(values):
    values = list(values)
    if not values or any(type(value) not in (int, float) or not math.isfinite(value)
                         or value <= 0 for value in values):
        raise ValueError("latency samples must be finite positive numbers")
    return {"count": len(values), "min_us": min(values),
            "median_us": statistics.median(values), "max_us": max(values),
            "mean_us": statistics.mean(values),
            "display": "median %.3f us (min %.3f, max %.3f)" %
                       (statistics.median(values), min(values), max(values))}


def aggregate_host(batches, init, case, warmup, iterations):
    """Summarize batch averages, never label them per-call latency samples."""
    ranks = {str(rank) for rank in range(case["world_size"])}
    if not batches or set(init) != ranks:
        raise ValueError("Host timing requires complete initialized ranks and batches")
    per_rank = {rank: [] for rank in sorted(ranks, key=int)}
    per_batch = []
    calls_per_iteration = 2 if case["operator"] == "ep_dispatch_combine" else 1
    for index, batch in enumerate(batches):
        samples = batch["samples"]
        if set(samples) != ranks or set(batch["validate"]) != ranks:
            raise ValueError("incomplete Host batch rank set")
        averages = []
        for rank, response in samples.items():
            sample = response["data"]
            elapsed_ns = sample.get("elapsed_ns")
            elapsed_us, average_us = sample.get("elapsed_us"), sample.get("host_batch_average_us")
            if (response.get("status") != "passed" or batch["validate"][rank].get("status") != "passed"
                    or type(sample.get("batch")) is not int or sample["batch"] != index
                    or type(sample.get("iterations")) is not int or sample["iterations"] != iterations
                    or type(elapsed_ns) is not int or not 0 < elapsed_ns <= 2**63-1):
                raise ValueError("invalid Host batch identity/count")
            summarize([elapsed_us, average_us])
            if (not math.isclose(elapsed_us, elapsed_ns / 1000, rel_tol=1e-12)
                    or not math.isclose(average_us, elapsed_us / iterations, rel_tol=1e-12)):
                raise ValueError("Host average disagrees with elapsed/count")
            per_rank[rank].append(dict(sample))
            averages.append(average_us)
        per_batch.append({"batch": index, "rank_average_us": {
            "min": min(averages), "median": statistics.median(averages), "max": max(averages)}})
    rank_medians = []
    summaries = {}
    for rank, samples in per_rank.items():
        summary = summarize(sample["host_batch_average_us"] for sample in samples)
        summaries[rank] = {"batches": samples, "batch_average_summary": summary}
        rank_medians.append(summary["median_us"])
    maxima = [batch["rank_average_us"]["max"] for batch in per_batch]
    return {"status": "valid", "method": "host_batch", "source": "C++ std::chrono::steady_clock",
            "execution_object": case["operator"], "warmup": warmup, "iterations": iterations,
            "batches": len(batches), "unit": "us", "profiler_enabled": False, "instrumentation": "OFF",
            "per_rank": summaries, "per_batch": per_batch,
            "rank_median_batch_average_us": {"min": min(rank_medians),
                "median": statistics.median(rank_medians), "max": max(rank_medians)},
            "maximum_rank_batch_average_us": {"samples_us": maxima, "summary": summarize(maxima),
                "scope": "throughput indicator; independent rank clocks, not synchronized global makespan"},
            "denominator": "chain iterations" if calls_per_iteration == 2 else "operator calls",
            "timing_scope": "Host API submission, device work/waits, submission gaps and final stream sync; not Kernel Duration",
            "barriers": "global ready gate, output reset and pre-sync excluded; no per-iteration controller gate; production dependencies and API-internal synchronization preserved",
            "production_sync": "ordinary Memory source at build revision has no explicit per-call stream sync; replacement installed API behavior is included unchanged",
            "distribution_scope": "multiple batch averages, not a per-call latency distribution",
            "logical_payload_bandwidth": {"status": "not_applicable", "display": "N/A",
                "reason": "Host batch time is a separate API-throughput measure; no device Kernel Duration or physical-link bandwidth is inferred"}}
