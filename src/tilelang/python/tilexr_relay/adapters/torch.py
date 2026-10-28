"""Convert current-NPU Torch tensors to typed borrowed Relay bindings.

Tensor validation and one-time workspace allocation belong at setup, not in
the launcher. Callers retain tensors/workspaces for their required lifetimes;
prepared executions require address-stable storage until destruction.
"""

from ..contracts import Binding


def bind_tensors(plan, tensors):
    import torch
    tensors = tuple(tensors)
    if len(tensors) != len(plan.binding_specs):
        raise ValueError("tensor binding count mismatch")
    tags = {torch.float16: 1, torch.float32: 2, torch.int64: 3}
    bindings = []
    for tensor, spec in zip(tensors, plan.binding_specs):
        if (not isinstance(tensor, torch.Tensor) or tensor.device.type != "npu" or
                tensor.device.index != torch.npu.current_device() or not tensor.is_contiguous()):
            raise ValueError("binding requires contiguous tensor on current NPU")
        if tags.get(tensor.dtype) != spec.dtype_tag or spec.layout_tag != 1:
            raise ValueError("unsupported tensor dtype/layout tag")
        size = tensor.numel() * tensor.element_size()
        if size < spec.bytes or tensor.data_ptr() % spec.alignment:
            raise ValueError("tensor capacity/alignment mismatch")
        bindings.append(Binding(tensor.data_ptr(), size, spec.dtype_tag, spec.layout_tag))
    return tuple(bindings)


def allocate_workspace(plan):
    """Allocate/zero once at setup, never in the steady-state launcher."""
    import torch
    return torch.zeros(plan.workspace_bytes, device="npu", dtype=torch.uint8)
