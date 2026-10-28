"""Build a generic ABI-v1 operator as a pure device ELF and validated Plan.

Select the Python generator/planner, compile with the pinned Ascend toolchain,
and emit content-addressed artifacts for libtilexr-relay. This offline CLI does
not launch kernels, initialize communication or install compiler dependencies.
"""

import argparse
from dataclasses import replace
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src/tilelang/python"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--operator", choices=("allgather_gemm", "gemm_allreduce"), required=True)
    parser.add_argument("--rows", type=int, required=True)
    parser.add_argument("--n", type=int, required=True)
    parser.add_argument("--k", type=int, required=True)
    parser.add_argument("--blocks", type=int, required=True)
    parser.add_argument("--compiler-revision", required=True, help="deployed source attestation; compiler library is also hashed")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    from tilexr_relay.ops import plans
    layout = plans.peer_layout(args.operator, args.rows, args.n, args.k, args.blocks)
    from tilexr_relay.contracts import KernelArtifact
    from tilexr_relay.build import compile_device
    import tilelang
    if args.operator == "allgather_gemm":
        from tilexr_relay.ops.allgather_gemm import generic_kernel
        planner = plans.allgather_gemm_plan
    else:
        from tilexr_relay.ops.gemm_allreduce import generic_kernel
        planner = plans.gemm_allreduce_plan
    specialization = {"rows": args.rows, "n": args.n, "k": args.k, "blocks": args.blocks}
    # Validate operator resources before invoking expensive code generation.
    preview = KernelArtifact(args.operator, specialization, "Ascend910B", "peer_memory", {}, "0" * 64,
                             peer_layout=layout)
    planner(preview)
    kernel = generic_kernel(preview)
    stem = f"{args.operator}_m{args.rows}_n{args.n}_k{args.k}_b{args.blocks}"
    elf, build = compile_device(kernel.kernel_source, args.output, stem)
    compiler = {"revision": args.compiler_revision, "version": tilelang.__version__,
                "library_sha256": hashlib.sha256(Path(tilelang._LIB_PATH).read_bytes()).hexdigest(),
                **build}
    artifact = replace(preview, compiler=compiler, sha256=build["binary_sha256"])
    plan = planner(artifact)
    destination = elf.with_name(artifact.file_name)
    if destination.exists() and destination.read_bytes() != elf.read_bytes():
        raise RuntimeError("content-addressed artifact collision")
    elf.rename(destination)
    destination.with_suffix(".json").write_text(plan.to_json())
    print(json.dumps({"elf": str(destination), "manifest": str(destination.with_suffix('.json')),
                      "plan_identity": plan.identity}))


if __name__ == "__main__":
    main()
