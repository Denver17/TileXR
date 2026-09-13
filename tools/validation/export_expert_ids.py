#!/usr/bin/env python3
"""从一次验证的 summary.json 汇总各 rank 实际使用的原始专家列表。"""

import argparse
import json
from pathlib import Path
import struct
import sys


def export_expert_ids(summary_path, pretty=False):
    summary_path = Path(summary_path).expanduser().resolve()
    report = json.loads(summary_path.read_text(encoding="utf-8"))
    config = report["chain_configuration"]
    ranks, bs, k, experts = (report["rank_size"], config["bs"],
                             config["topk"], config["moe_expert_num"])
    if any(type(value) is not int or value <= 0 for value in (ranks, bs, k, experts)):
        raise ValueError("rank_size、bs、topk、moe_expert_num 必须是正整数")
    if k > experts or experts % ranks:
        raise ValueError("topk 不能超过专家总数，专家总数必须能被 rank_size 整除")

    root = summary_path.parent
    directories = [root / "inputs", *sorted((root / "nodes").glob("*/inputs"))]
    files = {}
    for directory in directories:
        for path in sorted(directory.glob("rank-*-ids.bin")):
            rank = int(path.name.split("-")[1])
            if not 0 <= rank < ranks:
                raise ValueError(f"输入文件 rank 超出本次运行范围：{path}")
            if rank in files:
                raise ValueError(f"rank {rank} 输入重复：{files[rank]} 和 {path}")
            files[rank] = path
    missing = sorted(set(range(ranks)) - files.keys())
    if missing:
        raise ValueError(f"缺少 rank {missing} 的专家列表；检查 inputs/、nodes/*/inputs/ 及 collect-*.log")

    all_ids = {}
    for rank in range(ranks):
        raw = files[rank].read_bytes()
        expected_bytes = bs * k * 4
        if len(raw) != expected_bytes:
            raise ValueError(f"{files[rank]} 大小错误：期望 {expected_bytes} B，实际 {len(raw)} B")
        ids = struct.unpack(f"<{bs * k}i", raw)
        for index, expert in enumerate(ids):
            if not 0 <= expert < experts:
                raise ValueError(f"rank {rank} token {index // k} k {index % k} 专家编号越界：{expert}")
        all_ids[str(rank)] = [list(ids[token * k:(token + 1) * k]) for token in range(bs)]

    result = {
        "run_id": report["run_id"],
        "commit": report["commit"],
        "build_id": report["build_id"],
        "rank_size": ranks,
        "bs": bs,
        "topk": k,
        "experts": experts,
        "route_mode": config["route_mode"],
        "seed": config["seed"],
        "ordering": "expert_ids[rank][token][original_k]",
        "expert_ids": all_ids,
    }
    output = root / f"expert-ids-{ranks}ranks.json"
    output.write_text(json.dumps(
        result,
        ensure_ascii=False,
        indent=2 if pretty else None,
        separators=None if pretty else (",", ":"),
    ) + "\n", encoding="utf-8")
    return output, ranks, bs, k


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("summary", type=Path, help="本次运行的 summary.json 文件路径")
    parser.add_argument("--pretty", action="store_true", help="使用缩进格式输出，默认输出紧凑 JSON")
    args = parser.parse_args()
    try:
        output, ranks, bs, k = export_expert_ids(args.summary, pretty=args.pretty)
    except (OSError, UnicodeError, ValueError, KeyError, TypeError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"已汇总：{ranks} 卡，每卡 {bs} 个 Token、{bs * k} 条路由")
    print(f"文件：{output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
