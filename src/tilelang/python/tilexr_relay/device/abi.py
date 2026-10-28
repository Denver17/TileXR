"""Wrap operator bodies in the fixed sixteen-slot ABI-v1 device entrypoint.

Unused pointers are padded centrally; epoch/rank/rank_count keep their fixed
order and the pinned compiler appends FFTS. Bodies are inlined at build time,
without generated-source rewriting or device-side operator dispatch.
"""

import tilelang.language as T


def canonical_main(body, tensor_specs, scalars=("epoch", "rank", "rank_count")):
    """Retain all pointer slots, with operator-specific shape/type annotations.

    Body receives only the declared tensors, followed by the named scalars.
    Unused pointer slots are padded here, not in each operator. This emits a
    PrimFunc; the pinned compiler adds the final FFTS argument.
    """
    count = len(tensor_specs)
    if count > 16:
        raise ValueError("canonical entrypoint supports at most sixteen tensor slots")
    specs = list(tensor_specs) + [((4,), "int64")] * (16 - count)
    shapes = [spec[0] for spec in specs]
    dtypes = [spec[1] for spec in specs]

    def invoke(slots, epoch, rank, rank_count):
        values = {"epoch": epoch, "rank": rank, "rank_count": rank_count}
        body(*slots[:count], *(values[name] for name in scalars))

    @T.prim_func
    def main(
        p0: T.Tensor(shapes[0], dtypes[0]), p1: T.Tensor(shapes[1], dtypes[1]),
        p2: T.Tensor(shapes[2], dtypes[2]), p3: T.Tensor(shapes[3], dtypes[3]),
        p4: T.Tensor(shapes[4], dtypes[4]), p5: T.Tensor(shapes[5], dtypes[5]),
        p6: T.Tensor(shapes[6], dtypes[6]), p7: T.Tensor(shapes[7], dtypes[7]),
        p8: T.Tensor(shapes[8], dtypes[8]), p9: T.Tensor(shapes[9], dtypes[9]),
        p10: T.Tensor(shapes[10], dtypes[10]), p11: T.Tensor(shapes[11], dtypes[11]),
        p12: T.Tensor(shapes[12], dtypes[12]), p13: T.Tensor(shapes[13], dtypes[13]),
        p14: T.Tensor(shapes[14], dtypes[14]), p15: T.Tensor(shapes[15], dtypes[15]),
        epoch: T.int64, rank: T.int32, rank_count: T.int32,
    ):
        invoke((p0, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11, p12,
                p13, p14, p15), epoch, rank, rank_count)
    return main
