#!/usr/bin/env python3
"""生成 Memory Dispatch 的 SST 输入，逐 rank 校验 FP16 或 MXFP8 输出。

从已验证的 testcase 复制仿真框架，重新生成全部 workload、地址和二进制输入。
CommArgs 的偏移由同目录的 Host 布局探针提供；本脚本不修改或编译 kernel。
"""

import argparse
import collections
import copy
import hashlib
import json
import math
import pathlib
import re
import shutil
import struct

import yaml


SAFE_LOADER = getattr(yaml, "CSafeLoader", yaml.SafeLoader)
SAFE_DUMPER = getattr(yaml, "CSafeDumper", yaml.SafeDumper)
GM_SIZE = 0x2000000000
INPUT_BASE = 0x200000000
PEER_BASE = 0x1000000000
ABI = (
    "commArgs", "x", "expertIds", "xActiveMask", "expandXOut",
    "dynamicScalesOut", "expertTokenNumsOut", "sendCountsOut",
    "assistInfoForCombineOut", "bs", "h", "topK", "moeExpertNum",
    "sharedExpertNum", "sharedExpertRankNum", "globalBs",
    "expertTokenNumsType", "activeMaskType", "quantMode", "dtype",
    "expandXOutDtype", "magic",
)
OUTPUT_TYPES = {
    "expandXOut": "e", "expertTokenNumsOut": "q", "sendCountsOut": "i",
    "assistInfoForCombineOut": "i",
}


def sha256(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    pathlib.Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def read_yaml(path):
    return yaml.load(pathlib.Path(path).read_text(), Loader=SAFE_LOADER)


def encode_yaml(value):
    return yaml.dump(value, Dumper=SAFE_DUMPER, sort_keys=False)


def aligned(value, alignment=512):
    return (value + alignment - 1) // alignment * alignment


def routing(rank, token, lane, bs, top_k, experts):
    return ((rank * bs + token) * top_k + lane) % experts


def input_row(rank, token, h):
    values = [((rank * 97 + token * 17 + feature * 3) % 1023 + 1) / 16
              for feature in range(h)]
    return struct.pack("<" + "e" * h, *values)


def quantize_mxfp8_e4m3(row):
    """按 32 个 FP16 一组生成 E4M3 元素和偶数字节数的 E8M0 scales。"""
    values = struct.unpack("<" + "e" * (len(row) // 2), row)
    if not all(math.isfinite(value) for value in values):
        raise ValueError("MXFP8 oracle requires finite FP16 input")
    elements = bytearray()
    scales = bytearray(aligned((len(values) + 31) // 32, 2))
    for begin in range(0, len(values), 32):
        block = values[begin:begin + 32]
        maximum = max(abs(value) for value in block)
        shared = max(-127, min(127, math.frexp(maximum)[1] - 9)) if maximum else -127
        scales[begin // 32] = shared + 127
        for value in block:
            magnitude = min(math.ldexp(abs(value), -shared), 448.0)
            sign = 128 if math.copysign(1.0, value) < 0 else 0
            if magnitude < math.ldexp(1.0, -6):
                encoded = round(math.ldexp(magnitude, 9))
            else:
                exponent = math.frexp(magnitude)[1] - 1
                mantissa = round((math.ldexp(magnitude, -exponent) - 1) * 8)
                if mantissa == 8:
                    exponent, mantissa = exponent + 1, 0
                encoded = ((exponent + 7) << 3) | mantissa
            elements.append(sign | encoded)
    return bytes(elements), bytes(scales)


def output_types(scenario):
    types = dict(OUTPUT_TYPES)
    if scenario.get("output_format", "fp16") == "mxfp8-e4m3":
        types.update(expandXOut="B", dynamicScalesOut="B")
    return types


def oracle(ranks, bs, h, top_k, experts, token_nums_type, output_format="fp16"):
    buckets = [[] for _ in range(experts)]
    rows = [[input_row(rank, token, h) for token in range(bs)]
            for rank in range(ranks)]
    quantized = ([[quantize_mxfp8_e4m3(row) for row in rank_rows] for rank_rows in rows]
                 if output_format == "mxfp8-e4m3" else None)
    for source in range(ranks):
        for token in range(bs):
            for lane in range(top_k):
                expert = routing(source, token, lane, bs, top_k, experts)
                buckets[expert].append((source, token, lane))
    local_experts = experts // ranks
    result = []
    for rank in range(ranks):
        payload = bytearray()
        scales = bytearray()
        assist = bytearray()
        counts = []
        expert_counts = []
        cumulative = 0
        for expert in range(rank * local_experts, (rank + 1) * local_experts):
            by_source = collections.Counter(source for source, _, _ in buckets[expert])
            for source in range(ranks):
                cumulative += by_source[source]
                counts.append(cumulative)
            expert_counts.append(cumulative if token_nums_type == 0 else len(buckets[expert]))
            for source, token, lane in buckets[expert]:
                if quantized is None:
                    payload.extend(rows[source][token])
                else:
                    element_row, scale_row = quantized[source][token]
                    payload.extend(element_row)
                    scales.extend(scale_row)
                assist.extend(struct.pack("<iii", source, token, lane))
        result.append({
            "expandXOut": bytes(payload),
            "assistInfoForCombineOut": bytes(assist),
            "sendCountsOut": struct.pack("<" + "i" * len(counts), *counts),
            "expertTokenNumsOut": struct.pack("<" + "q" * len(expert_counts), *expert_counts),
        })
        if quantized is not None:
            result[-1]["dynamicScalesOut"] = bytes(scales)
    return rows, result


def comm_args(rank, ranks, layout):
    data = bytearray(layout["comm_args_bytes"])
    for field, value in (("rank", rank), ("local_rank", rank % min(ranks, 8)),
                         ("rank_size", ranks), ("local_rank_size", min(ranks, 8))):
        struct.pack_into("<i", data, layout[field], value)
    struct.pack_into("<I", data, layout["extra_flag"], layout["memory_flags"])
    struct.pack_into("<Q", data, layout["peer_mem_bytes"], layout["peer_bytes"])
    for peer in range(ranks):
        struct.pack_into("<Q", data, layout["peer_mems"] + 8 * peer, peer * GM_SIZE + PEER_BASE)
    return data


def patch_config(value, ranks, instruction_dump=False):
    if isinstance(value, list):
        for child in value:
            patch_config(child, ranks, instruction_dump)
    elif isinstance(value, dict):
        for key, child in value.items():
            if key == "chip_num":
                value[key] = ranks
            elif key == "stat_host_range":
                value[key] = [0, ranks - 1]
            elif key in {"stat_trace_flag", "stat_bw_flag", "stat_pkt_track_flag",
                         "urma_flag", "simt_urma_flag"}:
                value[key] = False
            elif key == "enable_dump":
                value[key] = int(instruction_dump)
            elif key in {"enable_dump_detail", "log_level"}:
                value[key] = 0
            else:
                patch_config(child, ranks, instruction_dump)


def check_regions(regions):
    ordered = sorted(regions)
    for (start, size, name), (next_start, _, next_name) in zip(ordered, ordered[1:]):
        if start + size > next_start:
            raise ValueError("GM overlap: %s and %s" % (name, next_name))
    if any(size <= 0 or start < 0 or start + size > GM_SIZE for start, size, _ in ordered):
        raise ValueError("Invalid rank-local GM region")


def preflight(case):
    scenario = json.loads((case / "scenario.json").read_text())
    manifest = json.loads((case / "manifest.json").read_text())
    changed = [name for name, digest in manifest.items()
               if not (case / name).is_file() or sha256(case / name) != digest]
    if changed or sha256(scenario["kernel_file"]) != scenario["kernel_sha256"]:
        raise ValueError("Case inputs or deployed kernel changed: %s" % changed)
    if (case / "workload.yml").read_bytes() != (case / "v310_config/workload_config.yml").read_bytes():
        raise ValueError("Workload mirrors differ")
    chips = read_yaml(case / "workload.yml")["workload_config"]
    if list(chips) != ["chip%d" % rank for rank in range(scenario["rank_size"])]:
        raise ValueError("Workload ranks differ from scenario")
    for rank, chip in enumerate(chips.values()):
        base = rank * GM_SIZE
        if (chip["bin_param"]["addr"] != base + 0x40000000
                or chip["param_base_addr"] != base + 0x80000000
                or chip["kernel_file"] != scenario["kernel_file"]
                or chip["block_dim"] != scenario["active_cores"]):
            raise ValueError("Kernel launch configuration differs from scenario")
        params = chip["kernel_param"]
        if list(params) != list(ABI):
            raise ValueError("Kernel ABI order mismatch")
        if scenario["format_version"] >= 3:
            quantized = scenario["output_format"] == "mxfp8-e4m3"
            expected_values = {"h": scenario["h"], "bs": scenario["bs"],
                               "topK": scenario["top_k"], "moeExpertNum": scenario["experts"],
                               "dtype": 3, "quantMode": 4 if quantized else 0,
                               "expandXOutDtype": 15 if quantized else 3}
            if (any(params[name]["value"] != value for name, value in expected_values.items())
                    or bool(params["dynamicScalesOut"]["value"]) != quantized
                    or set(chip["output_param"]) != set(output_types(scenario))):
                raise ValueError("Workload output format or shape differs from scenario")
        for index, entry in enumerate(params.values()):
            if (entry["param_offset"] != str(index * 8)
                    or entry["param_type"] != ("11" if index < 9 else "7")):
                raise ValueError("Kernel ABI slot mismatch")
            if index < 9 and entry["value"]:
                path = pathlib.Path(entry["input_bin"])
                if not path.is_file() or entry["value"] % 32:
                    raise ValueError("Missing or unaligned pointer input")
        if scenario["format_version"] >= 2:
            for name, region in chip["output_param"].items():
                extent = scenario["output_sizes"][str(rank)][name]
                semantic = scenario["semantic_output_sizes"][str(rank)][name]
                if region["size"] != extent or extent != aligned(max(1, semantic)):
                    raise ValueError("Output extent does not cover complete sparse GM blocks")
    return scenario


def generate(args):
    layout = json.loads(args.layout.read_text())
    output_format = getattr(args, "output_format", "fp16")
    if output_format not in ("fp16", "mxfp8-e4m3"):
        raise ValueError("Unsupported output format")
    quantized = output_format == "mxfp8-e4m3"
    types = output_types({"output_format": output_format})
    if args.ranks not in (2, 4, 8, 16, 32) or args.ranks > layout["max_ranks"]:
        raise ValueError("Supported rank counts: 2, 4, 8, 16, 32")
    if args.cores < 2 or args.cores > 64 or args.cores % 2:
        raise ValueError("Use an even core count from 2 through 64")
    if (args.bs <= 0 or args.h <= 0 or args.h % 256 or args.experts % args.ranks
            or not 1 <= args.top_k <= args.experts or args.magic <= 0):
        raise ValueError("Invalid shape or magic; H must be a multiple of 256")
    if not args.kernel.is_file():
        raise ValueError("Compiled kernel does not exist")
    workspace_bytes = aligned(args.cores * args.experts * 4, 32)
    if args.experts * 32 + workspace_bytes > layout["count_workspace_end"]:
        raise ValueError("Count records overlap receive-count workspace")
    scale_bytes = aligned((args.h + 31) // 32, 2) if quantized else 0
    payload_bytes = args.h if quantized else args.h * 2
    token_bytes = aligned(payload_bytes + scale_bytes, 32) + 32
    wire_bytes = aligned(token_bytes, 480) // 480 * 512
    combine_bytes = args.bs * args.top_k * aligned(args.h * 2, 480) // 480 * 512
    window_bytes = layout["state_bytes"] + combine_bytes + args.experts * args.bs * wire_bytes
    if layout["ipc_data_offset"] + window_bytes > layout["round_stride"]:
        raise ValueError("Dispatch data exceeds one peer round")
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    (output / ".tilexr-memory-dispatch-case").write_text("1\n")
    for name in ("test_UBusSim.py", "output_generator.py"):
        shutil.copy2(args.template / name, output / name)
    shutil.copytree(args.template / "UBusSim_config", output / "UBusSim_config")
    config_dir = output / "v310_config"
    config_dir.mkdir()
    allowed = ("daw_config.yml", "ddie_noc_config.yml", "udie_noc_config.yml",
               "davidV100_simulator_config.yml", "pv_simulator_config.yml")
    for name in allowed:
        source = args.template / "v310_config" / name
        if source.is_file():
            shutil.copy2(source, config_dir / name)
    for folder in (output / "UBusSim_config", config_dir):
        for path in folder.glob("*.yml"):
            config = read_yaml(path)
            patch_config(config, args.ranks, getattr(args, "instruction_dump", False))
            path.write_text(encode_yaml(config))
    template = read_yaml(args.template / "v310_config/workload_config.yml")
    chip_template = next(iter(template["workload_config"].values()))
    rows, expected = oracle(args.ranks, args.bs, args.h, args.top_k,
                            args.experts, args.token_nums_type, output_format)
    workload = {"workload_config": {}}
    output_sizes = {}
    semantic_output_sizes = {}
    for rank in range(args.ranks):
        rank_base = rank * GM_SIZE
        inputs = output / "input_bin" / ("rank%d" % rank)
        reference = output / "expected" / ("rank%d" % rank)
        inputs.mkdir(parents=True)
        reference.mkdir(parents=True)
        ids = [routing(rank, token, lane, args.bs, args.top_k, args.experts)
               for token in range(args.bs) for lane in range(args.top_k)]
        contents = {"commArgs": bytes(comm_args(rank, args.ranks, layout)),
                    "x": b"".join(rows[rank]),
                    "expertIds": struct.pack("<" + "i" * len(ids), *ids)}
        for name, data in expected[rank].items():
            (reference / (name + ".bin")).write_bytes(data)
            # 该模板按 512B dump；声明与初始化都覆盖尾部所在的完整块。
            contents[name] = b"\xa5" * aligned(max(1, len(data)))
        pointers = {}
        regions = [(0x40000000, args.kernel.stat().st_size, "kernel"),
                   (0x80000000, 512, "parameters"),
                   (PEER_BASE, layout["peer_bytes"], "peer_window")]
        cursor = INPUT_BASE
        for name, data in contents.items():
            pointers[name] = rank_base + cursor
            (inputs / (name + ".bin")).write_bytes(data)
            regions.append((cursor, len(data), name))
            cursor = aligned(cursor + len(data))
        check_regions(regions)
        values = dict(pointers)
        values.update(xActiveMask=0, bs=args.bs, h=args.h,
                      topK=args.top_k, moeExpertNum=args.experts, sharedExpertNum=0,
                      sharedExpertRankNum=0, globalBs=args.bs * args.ranks,
                      expertTokenNumsType=args.token_nums_type, activeMaskType=0,
                      quantMode=4 if quantized else 0, dtype=3,
                      expandXOutDtype=15 if quantized else 3, magic=args.magic)
        if not quantized:
            values["dynamicScalesOut"] = 0
        params = {}
        for index, name in enumerate(ABI):
            entry = {"value": values[name], "param_offset": str(index * 8),
                     "param_type": "11" if index < 9 else "7"}
            if name in pointers:
                entry["input_bin"] = str(inputs / (name + ".bin"))
            params[name] = entry
        chip = copy.deepcopy(chip_template)
        chip.update(gm_size=GM_SIZE, sys_va_base_addr=rank_base,
                    param_base_addr=rank_base + 0x80000000, block_dim=args.cores,
                    subblock_dim=1, kernel_file=str(args.kernel.resolve()),
                    vec_func="tilexr_ep_dispatch_memory_kernel", kernel_param=params,
                    bin_param={"addr": rank_base + 0x40000000,
                               "size": args.kernel.stat().st_size})
        chip["output_param"] = {
            name: {"addr": pointers[name], "size": len(contents[name])}
            for name in types
        }
        output_sizes[str(rank)] = {name: len(contents[name]) for name in types}
        semantic_output_sizes[str(rank)] = {name: len(expected[rank][name]) for name in types}
        workload["workload_config"]["chip%d" % rank] = chip
    encoded = encode_yaml(workload)
    (output / "workload.yml").write_text(encoded)
    (config_dir / "workload_config.yml").write_text(encoded)
    shutil.copy2(args.layout, output / "layout.json")
    scenario = {"format_version": 3, "transport": "MEM", "dtype": "FP16",
                "output_format": output_format, "quant_mode": 4 if quantized else 0,
                "instruction_dump": getattr(args, "instruction_dump", False),
                "output_dtype": 15 if quantized else 3,
                "token_payload_bytes": payload_bytes, "token_scale_bytes": scale_bytes,
                "token_wire_bytes": wire_bytes, "peer_window_bytes": window_bytes,
                "rank_size": args.ranks, "active_cores": args.cores, "bs": args.bs,
                "h": args.h, "top_k": args.top_k, "experts": args.experts,
                "expert_token_nums_type": args.token_nums_type, "magic": args.magic,
                "kernel_file": str(args.kernel.resolve()), "kernel_sha256": sha256(args.kernel),
                "external_abi": list(ABI), "param_types": ["11"] * 9 + ["7"] * 13,
                "output_sizes": output_sizes, "semantic_output_sizes": semantic_output_sizes,
                "routed_rows": args.ranks * args.bs * args.top_k}
    write_json(output / "scenario.json", scenario)
    files = sorted(p for p in output.rglob("*") if p.is_file())
    write_json(output / "manifest.json", {str(p.relative_to(output)): sha256(p) for p in files})
    write_json(output / "validation_summary.json", {
        "status": "generated", "simulation_run": False, "ranks": args.ranks,
        "abi_slots": len(ABI), "gm_overlap": False, "kernel_sha256": scenario["kernel_sha256"],
    })
    preflight(output)
    print(json.dumps({"status": "generated", "case": str(output), "scenario": scenario}))


def validate(args):
    case = args.case.resolve()
    scenario = preflight(case)
    log = args.log.read_text(errors="replace")
    finishes = re.findall(r"Simulation is complete, simulated time:\s*([0-9.eE+-]+)\s*(\w+)", log)
    if args.exit_code != 0 or not finishes or not any(float(value) > 0 for value, _ in finishes):
        raise ValueError("Simulation did not exit successfully with a nonzero finish time")
    if re.search(r"Traceback|\bFATAL\b|dstType error|forced timeout", log):
        raise ValueError("Fatal simulator error found")
    checks = []
    for rank in range(scenario["rank_size"]):
        for name, code in output_types(scenario).items():
            actual = case / "bin" / ("david_%d_%s.bin" % (rank, name))
            expected = (case / "expected" / ("rank%d" % rank) / (name + ".bin")).read_bytes()
            data = actual.read_bytes()
            if len(data) != scenario["output_sizes"][str(rank)][name]:
                raise ValueError("Output size mismatch: %s" % actual)
            if data[:len(expected)] != expected:
                width = struct.calcsize("<" + code)
                first = next(i for i, pair in enumerate(zip(data, expected)) if pair[0] != pair[1])
                raise ValueError("Output mismatch: rank=%d field=%s element=%d" % (rank, name, first // width))
            checks.append({"rank": rank, "output": name, "bytes_compared": len(expected), "status": "PASS"})
    report = {"status": "PASS", "transport": "MEM", "rank_size": scenario["rank_size"],
              "active_cores_per_rank": scenario["active_cores"], "kernel_sha256": scenario["kernel_sha256"],
              "simulated_finish_times": finishes, "checks": checks}
    write_json(case / "simulation_validation.json", report)
    print(json.dumps(report))


def extract(args):
    case = args.case.resolve()
    scenario = preflight(case)
    chips = read_yaml(case / "workload.yml")["workload_config"]
    extracted = 0
    for rank, chip in enumerate(chips.values()):
        for name, region in chip["output_param"].items():
            # Sparse dump 使用 chip 内地址，workload 指针使用完整 system VA。
            start = region["addr"] - chip["sys_va_base_addr"]
            size = region["size"]
            initial = pathlib.Path(chip["kernel_param"][name]["input_bin"]).read_bytes()
            merged = bytearray(initial)
            owners = {}
            files = sorted((case / "bin").glob("david_%d_die*_%s.bin" % (rank, name)))
            if not files:
                raise ValueError("Missing per-die output: rank=%d field=%s" % (rank, name))
            for path in files:
                with path.open("rb") as stream:
                    while True:
                        record = stream.read(520)
                        if not record:
                            break
                        if len(record) != 520:
                            raise ValueError("Truncated sparse GM record: %s" % path)
                        address = struct.unpack_from("<Q", record)[0]
                        if address % 512:
                            raise ValueError("Unaligned sparse GM record")
                        begin, end = max(start, address), min(start + size, address + 512)
                        if begin >= end:
                            continue
                        data = record[8 + begin - address:8 + end - address]
                        offset = begin - start
                        if data == initial[offset:offset + len(data)]:
                            continue
                        if address in owners and owners[address] != data:
                            raise ValueError("Conflicting die writes: rank=%d field=%s address=%#x"
                                             % (rank, name, address))
                        owners[address] = data
                        merged[offset:offset + len(data)] = data
            # 未写入的区域保留初始化值，由后续数值校验检查；不从 expected 补数据。
            (case / "bin" / ("david_%d_%s.bin" % (rank, name))).write_bytes(merged)
            extracted += 1
    print(json.dumps({"status": "EXTRACTED", "rank_size": scenario["rank_size"],
                      "outputs": extracted}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    gen = sub.add_parser("generate")
    for name in ("template", "output", "kernel", "layout"):
        gen.add_argument("--" + name, type=pathlib.Path, required=True)
    for name, default in (("ranks", 2), ("cores", 4), ("bs", 8), ("h", 256),
                          ("top-k", 2), ("experts", 8), ("magic", 1)):
        gen.add_argument("--" + name, type=int, default=default)
    gen.add_argument("--token-nums-type", type=int, choices=(0, 1), default=0)
    gen.add_argument("--output-format", choices=("fp16", "mxfp8-e4m3"), default="fp16")
    gen.add_argument("--instruction-dump", action="store_true",
                     help="在模板已有 enable_dump 位置启用指令 dump；不启用 UBus 统计 trace")
    gen.set_defaults(action=generate)
    flight = sub.add_parser("preflight")
    flight.add_argument("case", type=pathlib.Path)
    flight.set_defaults(action=lambda args: print(json.dumps({
        "status": "PREFLIGHT_PASS", "scenario": preflight(args.case.resolve())})))
    merge = sub.add_parser("extract")
    merge.add_argument("case", type=pathlib.Path)
    merge.set_defaults(action=extract)
    check = sub.add_parser("validate")
    check.add_argument("case", type=pathlib.Path)
    check.add_argument("--log", type=pathlib.Path, required=True)
    check.add_argument("--exit-code", type=int, required=True)
    check.set_defaults(action=validate)
    args = parser.parse_args()
    args.action(args)


if __name__ == "__main__":
    main()
