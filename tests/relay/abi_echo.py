"""Build the canonical argument echo probe; run via registered Relay launcher.

Slot 7 is deliberately unused and must be retained. Slot 15 and all scalars
are observed after it, exposing pruning or frame-offset mistakes on hardware.
No generated C++ rewriting or compiler patch is used.
"""

import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src/tilelang/python"))

import tilelang
import tilelang.language as T
from tilexr_relay.build import compile_device
from tilexr_relay.device.abi import canonical_main
from echo_plan import echo_plan


@tilelang.jit(out_idx=[], target="ascendc", platform="A2")
def echo():
    @T.macro
    def body(p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12,
             p13, p14, p15, epoch, rank, rank_count):
        with T.Kernel(1, is_npu=True) as (cid, vid):
            result = T.alloc_ub((32,), "int64")
            token = T.alloc_ub((4,), "int64")
            with T.Scope("C"):
                T.set_cross_flag("FIX", 1, 2)
            with T.Scope("V"):
                T.wait_cross_flag(1)
                if vid == 0:
                    for i in T.serial(32):
                        result[i] = -1
                    T.copy(p1, token)
                    result[1] = token[0]
                    T.copy(p2, token)
                    result[2] = token[0]
                    T.copy(p3, token)
                    result[3] = token[0]
                    T.copy(p4, token)
                    result[4] = token[0]
                    T.copy(p5, token)
                    result[5] = token[0]
                    T.copy(p6, token)
                    result[6] = token[0]
                    T.copy(p8, token)
                    result[8] = token[0]
                    T.copy(p9, token)
                    result[9] = token[0]
                    T.copy(p10, token)
                    result[10] = token[0]
                    T.copy(p11, token)
                    result[11] = token[0]
                    T.copy(p12, token)
                    result[12] = token[0]
                    T.copy(p13, token)
                    result[13] = token[0]
                    T.copy(p14, token)
                    result[14] = token[0]
                    T.copy(p15, token)
                    result[15] = token[0]
                    result[16] = epoch
                    result[17] = T.Cast("int64", rank)
                    result[18] = T.Cast("int64", rank_count)
                    T.copy(result, p0)
    return canonical_main(body, [((32,), "int64")] + [((4,), "int64")] * 15)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    kernel = echo()
    elf, provenance = compile_device(kernel.kernel_source, args.output, "abi_echo")
    digest = hashlib.sha256(elf.read_bytes()).hexdigest()
    destination = elf.with_name(f"tilexr_relay_{digest}.o")
    if destination.exists() and destination.read_bytes() != elf.read_bytes():
        raise RuntimeError("content-addressed artifact collision")
    elf.rename(destination)
    metadata = {
        "file_name": destination.name, "sha256": digest, "abi_version": 1,
        "symbol": "main_kernel", "params": [str(p) for p in kernel.params],
        **provenance}
    (args.output / "abi_echo.json").write_text(json.dumps(metadata, indent=2))
    destination.with_suffix(".json").write_text(echo_plan(metadata).to_json())
    print(destination)


if __name__ == "__main__":
    main()
