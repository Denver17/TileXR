from __future__ import annotations

import ctypes
from enum import IntEnum, IntFlag


TILEXR_SUCCESS = 0
TILEXR_MOONEP_ABI_VERSION_V1 = 1
TILEXR_MOONEP_ABI_VERSION_V2 = 2
TILEXR_MOONEP_ABI_VERSION = TILEXR_MOONEP_ABI_VERSION_V2
TILEXR_MOONEP_MAX_TENSOR_RANK = 4
TILEXR_MOONEP_FLAG_NONE = 0
TILEXR_MOONEP_FLAG_BUILD_DEDUP = 1 << 0
TILEXR_MOONEP_FLAG_SKIP_INTER_RANK_SYNC = 1 << 1
TILEXR_MOONEP_FLAG_ZERO_COPY = 1 << 2
TILEXR_MOONEP_FLAG_RESET_STATUS = 1 << 5
TILEXR_MOONEP_REDUCE_GRAD_UDMA_THRESHOLD_BYTES = 1 << 20


class TileXRMoonEPDType(IntEnum):
    INT32 = 2
    FLOAT16 = 3
    FLOAT32 = 4
    BFLOAT16 = 11


class TileXRMoonEPStage(IntFlag):
    PLANNING = 1 << 0
    DISPATCH = 1 << 1
    PREFETCH_WEIGHT = 1 << 2
    COMBINE = 1 << 3
    REDUCE_GRAD = 1 << 4


class TileXRMoonEPTensorV1(ctypes.Structure):
    _fields_ = [
        ("structSize", ctypes.c_uint32),
        ("abiVersion", ctypes.c_uint32),
        ("data", ctypes.c_void_p),
        ("elementCount", ctypes.c_uint64),
        ("dtype", ctypes.c_uint32),
        ("rank", ctypes.c_uint32),
        ("shape", ctypes.c_int64 * TILEXR_MOONEP_MAX_TENSOR_RANK),
    ]


class TileXRMoonEPPlanV1(ctypes.Structure):
    _fields_ = [
        ("structSize", ctypes.c_uint32),
        ("abiVersion", ctypes.c_uint32),
        ("n", ctypes.c_int64),
        ("r", ctypes.c_int64),
        ("e", ctypes.c_int64),
        ("b", ctypes.c_int64),
        ("nvS", ctypes.c_int64),
        ("k", ctypes.c_int64),
        ("dst", ctypes.c_void_p),
        ("expertsToCopy", ctypes.c_void_p),
        ("zeroFillRanges", ctypes.c_void_p),
        ("remoteStats", ctypes.c_void_p),
        ("dupGroups", ctypes.c_void_p),
        ("dupLoffs", ctypes.c_void_p),
        ("dupCounts", ctypes.c_void_p),
        ("status", ctypes.c_void_p),
    ]


class TileXRMoonEPPlanningArgsV1(ctypes.Structure):
    _fields_ = [
        ("structSize", ctypes.c_uint32),
        ("abiVersion", ctypes.c_uint32),
        ("comm", ctypes.c_void_p),
        ("topkExperts", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("tokensPerExpert", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("workspace", ctypes.c_void_p),
        ("workspaceBytes", ctypes.c_uint64),
        ("cuSeqlens", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("plan", ctypes.POINTER(TileXRMoonEPPlanV1)),
        ("waitIterations", ctypes.c_uint64),
        ("flags", ctypes.c_uint64),
    ]


class TileXRMoonEPDispatchTraceV1(ctypes.Structure):
    _fields_ = [
        ("structSize", ctypes.c_uint32),
        ("abiVersion", ctypes.c_uint32),
        ("buffer", ctypes.c_void_p),
        ("bufferBytes", ctypes.c_uint64),
        ("iteration", ctypes.c_uint32),
        ("iterationCount", ctypes.c_uint32),
        ("eventCapacity", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
    ]


class TileXRMoonEPDispatchArgsV1(ctypes.Structure):
    _fields_ = [
        ("structSize", ctypes.c_uint32),
        ("abiVersion", ctypes.c_uint32),
        ("comm", ctypes.c_void_p),
        ("plan", ctypes.POINTER(TileXRMoonEPPlanV1)),
        ("hiddenSh", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("routeWeightsSk", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("hiddenNvsh", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("routeWeightsNvs", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("flags", ctypes.c_uint64),
        ("registeredWorkspace", ctypes.c_void_p),
        ("registeredWorkspaceBytes", ctypes.c_uint64),
        ("trace", ctypes.POINTER(TileXRMoonEPDispatchTraceV1)),
    ]


class TileXRMoonEPDispatchArgsV2(ctypes.Structure):
    _fields_ = TileXRMoonEPDispatchArgsV1._fields_


class TileXRMoonEPPrefetchWeightArgsV1(ctypes.Structure):
    _fields_ = [
        ("structSize", ctypes.c_uint32),
        ("abiVersion", ctypes.c_uint32),
        ("comm", ctypes.c_void_p),
        ("plan", ctypes.POINTER(TileXRMoonEPPlanV1)),
        ("gate", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("up", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("down", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("flags", ctypes.c_uint64),
    ]


class TileXRMoonEPReduceGradArgsV1(ctypes.Structure):
    _fields_ = [
        ("structSize", ctypes.c_uint32),
        ("abiVersion", ctypes.c_uint32),
        ("comm", ctypes.c_void_p),
        ("plan", ctypes.POINTER(TileXRMoonEPPlanV1)),
        ("input", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("output", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("flags", ctypes.c_uint64),
    ]


class TileXRMoonEPReduceGradWorkspaceQueryV2(ctypes.Structure):
    _fields_ = [
        ("structSize", ctypes.c_uint32),
        ("abiVersion", ctypes.c_uint32),
        ("comm", ctypes.c_void_p),
        ("plan", ctypes.POINTER(TileXRMoonEPPlanV1)),
        ("gate", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("up", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("down", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("requestedUdmaChunkBytes", ctypes.c_uint64),
        ("flags", ctypes.c_uint64),
    ]


class TileXRMoonEPReduceGradWorkspaceInfoV2(ctypes.Structure):
    _fields_ = [
        ("structSize", ctypes.c_uint32),
        ("abiVersion", ctypes.c_uint32),
        ("workspaceBytes", ctypes.c_uint64),
        ("workspaceAlignment", ctypes.c_uint64),
        ("udmaChunkBytes", ctypes.c_uint64),
        ("laneStateBytes", ctypes.c_uint64),
        ("laneStateStrideBytes", ctypes.c_uint64),
        ("bankStrideBytes", ctypes.c_uint64),
        ("laneStrideBytes", ctypes.c_uint64),
        ("rowBytes", ctypes.c_uint64 * 3),
        ("chunkCounts", ctypes.c_uint64 * 3),
        ("projectionQpCounts", ctypes.c_uint32 * 3),
        ("qpCount", ctypes.c_uint32),
        ("blockDim", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
    ]


class TileXRMoonEPReduceGradSourceSliceV2(ctypes.Structure):
    _fields_ = [
        ("data", ctypes.c_void_p),
        ("bytes", ctypes.c_uint64),
        ("registrationBase", ctypes.c_void_p),
        ("registrationBytes", ctypes.c_uint64),
    ]


class TileXRMoonEPReduceGradPrepareArgsV2(ctypes.Structure):
    _fields_ = [
        ("structSize", ctypes.c_uint32),
        ("abiVersion", ctypes.c_uint32),
        ("comm", ctypes.c_void_p),
        ("plan", ctypes.POINTER(TileXRMoonEPPlanV1)),
        ("gate", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("up", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("down", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("sources", TileXRMoonEPReduceGradSourceSliceV2 * 3),
        ("workspace", ctypes.c_void_p),
        ("workspaceBytes", ctypes.c_uint64),
        ("requestedUdmaChunkBytes", ctypes.c_uint64),
        ("flags", ctypes.c_uint64),
    ]


class TileXRMoonEPReduceGradArgsV2(ctypes.Structure):
    _fields_ = [
        ("structSize", ctypes.c_uint32),
        ("abiVersion", ctypes.c_uint32),
        ("prepared", ctypes.c_void_p),
        ("plan", ctypes.POINTER(TileXRMoonEPPlanV1)),
        ("gate", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("up", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("down", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("sources", TileXRMoonEPReduceGradSourceSliceV2 * 3),
        ("status", ctypes.POINTER(TileXRMoonEPTensorV1)),
        ("waitIterations", ctypes.c_uint64),
        ("flags", ctypes.c_uint64),
    ]


def initialize_struct(
    value: ctypes.Structure, *, abi_version: int | None = None
) -> ctypes.Structure:
    value.structSize = ctypes.sizeof(type(value))
    if abi_version is None:
        abi_version = (
            TILEXR_MOONEP_ABI_VERSION_V2
            if type(value).__name__.endswith("V2")
            else TILEXR_MOONEP_ABI_VERSION_V1
        )
    value.abiVersion = int(abi_version)
    return value


def void_p(value: int | ctypes.c_void_p | None) -> ctypes.c_void_p:
    if isinstance(value, ctypes.c_void_p):
        return value
    return ctypes.c_void_p(0 if value is None else int(value))


def dtype_code(dtype) -> int:
    name = str(dtype).lower()
    if "bfloat16" in name:
        return int(TileXRMoonEPDType.BFLOAT16)
    if "float16" in name or name.endswith(".half"):
        return int(TileXRMoonEPDType.FLOAT16)
    if "float32" in name or name.endswith(".float"):
        return int(TileXRMoonEPDType.FLOAT32)
    if "int32" in name:
        return int(TileXRMoonEPDType.INT32)
    raise TypeError(f"unsupported MoonEP dtype: {dtype}")


def tensor_nbytes(tensor) -> int:
    return int(tensor.numel()) * int(tensor.element_size())


def tensor_ptr(tensor) -> ctypes.c_void_p:
    return ctypes.c_void_p(int(tensor.data_ptr()))


def tensor_registration_range(tensor) -> tuple[ctypes.c_void_p, int]:
    backing = getattr(tensor, "_tilexr_registration_backing", tensor)
    base = int(backing.data_ptr())
    size = tensor_nbytes(backing)
    data = int(tensor.data_ptr())
    logical_bytes = tensor_nbytes(tensor)
    if base <= data and logical_bytes <= size - (data - base):
        return ctypes.c_void_p(base), size
    raise ValueError("tensor view is outside its registration backing")


def make_tensor_v1(tensor) -> TileXRMoonEPTensorV1:
    shape = tuple(int(value) for value in tensor.shape)
    if not shape or len(shape) > TILEXR_MOONEP_MAX_TENSOR_RANK:
        raise ValueError(
            f"MoonEP native tensors must have rank in [1, {TILEXR_MOONEP_MAX_TENSOR_RANK}]"
        )
    value = initialize_struct(TileXRMoonEPTensorV1())
    value.data = tensor_ptr(tensor)
    value.elementCount = int(tensor.numel())
    value.dtype = dtype_code(tensor.dtype)
    value.rank = len(shape)
    for index, dimension in enumerate(shape):
        value.shape[index] = dimension
    return value
