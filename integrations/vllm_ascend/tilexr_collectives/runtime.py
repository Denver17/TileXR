from __future__ import annotations

import ctypes
import os
from pathlib import Path


TILEXR_SUCCESS = 0
TILEXR_DATA_TYPE_INT8 = 0
TILEXR_DATA_TYPE_INT32 = 2
TILEXR_DATA_TYPE_FP16 = 3
TILEXR_DATA_TYPE_FP32 = 4
TILEXR_DATA_TYPE_INT64 = 5
TILEXR_DATA_TYPE_UINT8 = 7
TILEXR_DATA_TYPE_BFP16 = 11
TILEXR_REDUCE_SUM = 0


class TileXRCollectivesError(RuntimeError):
    def __init__(self, operation: str, ret: int, detail: str):
        super().__init__(f"{operation} failed with TileXR ret={ret}: {detail}")
        self.operation = operation
        self.ret = ret
        self.detail = detail


def _resolve_install_prefix(install_prefix: str | os.PathLike[str] | None) -> Path:
    if install_prefix is not None:
        return Path(install_prefix).resolve()
    env_prefix = os.environ.get("TILEXR_INSTALL_PREFIX")
    if env_prefix:
        return Path(env_prefix).resolve()
    return Path.cwd().resolve() / "install"


def _resolve_library(name: str, explicit_env: str, install_prefix: Path) -> str:
    explicit = os.environ.get(explicit_env)
    if explicit:
        path = Path(explicit).resolve()
        if not path.exists():
            raise FileNotFoundError(f"{explicit_env} points to missing library: {path}")
        return str(path)

    candidates = [
        install_prefix / "lib" / name,
        install_prefix / "lib64" / name,
    ]
    for path in candidates:
        if path.exists():
            return str(path)
    candidate_text = ", ".join(str(path) for path in candidates)
    raise FileNotFoundError(f"could not find {name}; checked {candidate_text}")


def _void_p(value: int | ctypes.c_void_p | None) -> ctypes.c_void_p:
    if isinstance(value, ctypes.c_void_p):
        return value
    if value is None:
        return ctypes.c_void_p()
    return ctypes.c_void_p(int(value))


class TileXRUniqueId(ctypes.Structure):
    _fields_ = [("data", ctypes.c_char * 128)]


class TileXRCollectivesRuntime:
    def __init__(
        self,
        rank: int,
        world_size: int,
        install_prefix: str | os.PathLike[str] | None = None,
        *,
        initialize: bool = True,
    ):
        if world_size <= 0:
            raise ValueError(f"world_size must be positive, got {world_size}")
        if rank < 0 or rank >= world_size:
            raise ValueError(f"rank must be in [0, {world_size}), got {rank}")

        self.rank = int(rank)
        self.world_size = int(world_size)
        self.install_prefix = _resolve_install_prefix(install_prefix)
        self._comm = ctypes.c_void_p()
        self._closed = False

        comm_lib_path = _resolve_library("libtile-comm.so", "TILEXR_COMM_LIB", self.install_prefix)
        collectives_lib_path = _resolve_library(
            "libtilexr-collectives.so",
            "TILEXR_COLLECTIVES_LIB",
            self.install_prefix,
        )
        self._comm_lib = ctypes.CDLL(comm_lib_path, mode=ctypes.RTLD_GLOBAL)
        self._collectives_lib = ctypes.CDLL(collectives_lib_path, mode=ctypes.RTLD_GLOBAL)
        self._configure_symbols()
        if initialize:
            self.initialize()

    def unique_id(self, domain: int) -> bytes:
        uid = TileXRUniqueId()
        ret = self._comm_lib.TileXRGetUniqueId(ctypes.byref(uid), ctypes.c_int(domain))
        self._check("TileXRGetUniqueId", ret, f"domain={domain}")
        return bytes(uid)

    def initialize(self, unique_id: bytes | None = None) -> None:
        if self._closed or self._comm.value:
            raise RuntimeError("TileXR runtime is closed or already initialized")
        if unique_id is None:
            operation = "TileXRCommInitRankLocal"
            args = (ctypes.c_int(self.world_size), ctypes.c_int(self.rank), ctypes.byref(self._comm))
        else:
            operation = "TileXRCommInitRank"
            uid = TileXRUniqueId.from_buffer_copy(unique_id)
            args = (uid, ctypes.c_int(self.world_size), ctypes.c_int(self.rank), ctypes.byref(self._comm))
        ret = getattr(self._comm_lib, operation)(*args)
        self._check(operation, ret, f"rank={self.rank} world_size={self.world_size}")

    def _configure_symbols(self) -> None:
        self._comm_lib.TileXRGetUniqueId.argtypes = [ctypes.POINTER(TileXRUniqueId), ctypes.c_int]
        self._comm_lib.TileXRGetUniqueId.restype = ctypes.c_int
        self._comm_lib.TileXRCommInitRank.argtypes = [
            TileXRUniqueId, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_void_p),
        ]
        self._comm_lib.TileXRCommInitRank.restype = ctypes.c_int
        self._comm_lib.TileXRCommInitRankLocal.argtypes = [
            ctypes.c_int,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        self._comm_lib.TileXRCommInitRankLocal.restype = ctypes.c_int
        self._comm_lib.TileXRCommDestroy.argtypes = [ctypes.c_void_p]
        self._comm_lib.TileXRCommDestroy.restype = ctypes.c_int

        self._collectives_lib.TileXRAllGather.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_int64,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_void_p,
        ]
        self._collectives_lib.TileXRAllGather.restype = ctypes.c_int
        self._collectives_lib.TileXRAllToAll.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_int64,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_void_p,
        ]
        self._collectives_lib.TileXRAllToAll.restype = ctypes.c_int
        self._collectives_lib.TileXRAllReduce.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_int64,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_void_p,
        ]
        self._collectives_lib.TileXRAllReduce.restype = ctypes.c_int
        self._collectives_lib.TileXRReduceScatter.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_int64,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_void_p,
        ]
        self._collectives_lib.TileXRReduceScatter.restype = ctypes.c_int
        self._collectives_lib.TileXRBroadcast.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int64,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_void_p,
        ]
        self._collectives_lib.TileXRBroadcast.restype = ctypes.c_int

    def _check(self, operation: str, ret: int, detail: str) -> None:
        if int(ret) != TILEXR_SUCCESS:
            raise TileXRCollectivesError(operation, int(ret), detail)

    @property
    def comm_ptr(self) -> int:
        if not self._comm.value:
            raise TileXRCollectivesError("comm_ptr", -1, "TileXR communicator is not initialized")
        return int(self._comm.value)

    def all_gather(
        self,
        send_ptr: int,
        recv_ptr: int,
        send_count: int,
        tilexr_dtype: int,
        stream_ptr: int | None,
    ) -> None:
        ret = self._collectives_lib.TileXRAllGather(
            _void_p(send_ptr),
            _void_p(recv_ptr),
            ctypes.c_int64(int(send_count)),
            ctypes.c_int(int(tilexr_dtype)),
            _void_p(self.comm_ptr),
            _void_p(stream_ptr),
        )
        self._check("TileXRAllGather", ret, f"rank={self.rank} count={send_count} dtype={tilexr_dtype}")

    def all_to_all(
        self,
        send_ptr: int,
        recv_ptr: int,
        send_count_per_peer: int,
        tilexr_dtype: int,
        stream_ptr: int | None,
    ) -> None:
        ret = self._collectives_lib.TileXRAllToAll(
            _void_p(send_ptr),
            _void_p(recv_ptr),
            ctypes.c_int64(int(send_count_per_peer)),
            ctypes.c_int(int(tilexr_dtype)),
            _void_p(self.comm_ptr),
            _void_p(stream_ptr),
        )
        self._check(
            "TileXRAllToAll",
            ret,
            f"rank={self.rank} count_per_peer={send_count_per_peer} dtype={tilexr_dtype}",
        )

    def all_reduce(
        self,
        send_ptr: int,
        recv_ptr: int,
        count: int,
        tilexr_dtype: int,
        stream_ptr: int | None,
    ) -> None:
        ret = self._collectives_lib.TileXRAllReduce(
            _void_p(send_ptr),
            _void_p(recv_ptr),
            ctypes.c_int64(int(count)),
            ctypes.c_int(int(tilexr_dtype)),
            ctypes.c_int(TILEXR_REDUCE_SUM),
            _void_p(self.comm_ptr),
            _void_p(stream_ptr),
        )
        self._check("TileXRAllReduce", ret, f"rank={self.rank} count={count} dtype={tilexr_dtype}")

    def reduce_scatter(
        self,
        send_ptr: int,
        recv_ptr: int,
        recv_count: int,
        tilexr_dtype: int,
        stream_ptr: int | None,
    ) -> None:
        ret = self._collectives_lib.TileXRReduceScatter(
            _void_p(send_ptr),
            _void_p(recv_ptr),
            ctypes.c_int64(int(recv_count)),
            ctypes.c_int(int(tilexr_dtype)),
            ctypes.c_int(TILEXR_REDUCE_SUM),
            _void_p(self.comm_ptr),
            _void_p(stream_ptr),
        )
        self._check("TileXRReduceScatter", ret, f"rank={self.rank} recv_count={recv_count} dtype={tilexr_dtype}")

    def broadcast(
        self,
        buf_ptr: int,
        count: int,
        tilexr_dtype: int,
        root: int,
        stream_ptr: int | None,
    ) -> None:
        ret = self._collectives_lib.TileXRBroadcast(
            _void_p(buf_ptr),
            ctypes.c_int64(int(count)),
            ctypes.c_int(int(tilexr_dtype)),
            ctypes.c_int(int(root)),
            _void_p(self.comm_ptr),
            _void_p(stream_ptr),
        )
        self._check("TileXRBroadcast", ret, f"rank={self.rank} count={count} dtype={tilexr_dtype} root={root}")

    def close(self) -> None:
        if self._closed:
            return
        if self._comm.value:
            ret = self._comm_lib.TileXRCommDestroy(self._comm)
            self._check("TileXRCommDestroy", ret, f"rank={self.rank}")
            self._comm = ctypes.c_void_p()
        self._closed = True

    def __enter__(self) -> "TileXRCollectivesRuntime":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass
