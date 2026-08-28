from __future__ import annotations

import base64
import copy
import ctypes
import ctypes.util
import errno
import hashlib
import json
import os
import re
import shutil
import struct
import uuid
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping

from .model_replay_cache import (
    CacheEntry,
    ReplayShape,
    begin_cache_capture,
    publish_cache,
    validate_model_performance,
    validate_route_replay,
)


META_SCHEMA_VERSION = 1
META_COMPRESSION_LEVEL = 3
META_ROUTE_DTYPE = "uint8"
META_ROUTE_CODEC = "zstd"
_META_FILENAMES = {"meta.json", "routes.u8.zst", "performance.json"}
_IPV4 = re.compile(r"(?<![0-9.])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9.])")
_WINDOWS_ABSOLUTE = re.compile(r"^[A-Za-z]:[\\/]")
_PRIVATE_KEYS = {
    "capture_id",
    "password",
    "passwd",
    "pid",
    "process_id",
    "secret",
    "timing_artifact",
    "user",
    "username",
}
_REQUIRED_PERFORMANCE_PROVENANCE = {
    "tilexr_git_sha",
    "adapter_sha256",
    "runner_sha256",
    "install_artifacts",
    "model_stack",
    "backend",
    "execution_controls",
    "kernel_version",
    "cann",
    "driver",
    "firmware",
    "soc",
    "topology",
    "rank_mapping",
}


class MetaValidationError(ValueError):
    pass


@dataclass(frozen=True)
class MetaBundle:
    path: Path
    meta: dict[str, object]
    performance: dict[str, object]
    route_bytes: bytes


def meta_case_id(shape: ReplayShape) -> str:
    return (
        f"model-replay-s{shape.tokens_per_rank}-k{shape.topk}"
        f"-h{shape.hidden_size}-ep{shape.ep_size}-r{shape.world_size}"
    )


def _meta_shape(shape: ReplayShape) -> dict[str, int]:
    return {
        "S": shape.tokens_per_rank,
        "K": shape.topk,
        "H": shape.hidden_size,
        "EP": shape.ep_size,
        "R": shape.world_size,
        "E": shape.expert_count,
        "Hf": shape.ffn_hidden_size,
        "P": shape.token_padding,
        "forward_calls": shape.forward_calls,
    }


def _canonical_bytes(value: object) -> bytes:
    try:
        text = json.dumps(
            value,
            ensure_ascii=True,
            allow_nan=False,
            separators=(",", ":"),
            sort_keys=True,
        )
    except (TypeError, ValueError) as exc:
        raise MetaValidationError("meta data must be finite JSON") from exc
    return text.encode("ascii")


def _pretty_json_bytes(value: object) -> bytes:
    try:
        text = json.dumps(
            value,
            ensure_ascii=True,
            allow_nan=False,
            indent=2,
            sort_keys=True,
        )
    except (TypeError, ValueError) as exc:
        raise MetaValidationError("meta data must be finite JSON") from exc
    return (text + "\n").encode("ascii")


def _sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _read_json(path: Path, description: str) -> dict[str, object]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            payload = json.load(handle)
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise MetaValidationError(f"invalid {description}: {path}") from exc
    if not isinstance(payload, dict):
        raise MetaValidationError(f"{description} must be a JSON object")
    return payload


def _write_bytes_fsync(path: Path, payload: bytes) -> None:
    with path.open("wb") as handle:
        handle.write(payload)
        handle.flush()
        os.fsync(handle.fileno())


def _fsync_directory(path: Path) -> None:
    if os.name == "nt":
        return
    descriptor = os.open(path, os.O_RDONLY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _zstd_library_candidates() -> list[str]:
    candidates = []
    override = os.environ.get("TILEXR_ZSTD_LIBRARY")
    if override:
        candidates.append(override)
    discovered = ctypes.util.find_library("zstd")
    if discovered:
        candidates.append(discovered)
    candidates.extend(("libzstd.so.1", "libzstd.so", "libzstd.dylib", "libzstd.dll"))
    if os.name == "nt":
        program_files = os.environ.get("ProgramFiles", r"C:\Program Files")
        candidates.extend(
            (
                str(Path(program_files) / "Git" / "mingw64" / "bin" / "libzstd.dll"),
                str(Path(program_files) / "Git" / "libexec" / "git-core" / "libzstd.dll"),
            )
        )
    return list(dict.fromkeys(candidates))


def _load_zstd_library():
    errors = []
    for candidate in _zstd_library_candidates():
        try:
            library = ctypes.CDLL(candidate)
        except OSError as exc:
            errors.append(f"{candidate}: {exc}")
            continue
        library.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
        library.ZSTD_compressBound.restype = ctypes.c_size_t
        library.ZSTD_compress.argtypes = [
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_int,
        ]
        library.ZSTD_compress.restype = ctypes.c_size_t
        library.ZSTD_decompress.argtypes = [
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_void_p,
            ctypes.c_size_t,
        ]
        library.ZSTD_decompress.restype = ctypes.c_size_t
        library.ZSTD_isError.argtypes = [ctypes.c_size_t]
        library.ZSTD_isError.restype = ctypes.c_uint
        library.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
        library.ZSTD_getErrorName.restype = ctypes.c_char_p
        return library
    detail = "; ".join(errors[-3:])
    raise RuntimeError(
        "standard zstd support is unavailable; install the Python zstandard package "
        f"or system libzstd ({detail})"
    )


_ZSTD_LIBRARY = None


def _libzstd():
    global _ZSTD_LIBRARY
    if _ZSTD_LIBRARY is None:
        _ZSTD_LIBRARY = _load_zstd_library()
    return _ZSTD_LIBRARY


def _zstd_error(library, result: int, action: str) -> None:
    if library.ZSTD_isError(result):
        message = library.ZSTD_getErrorName(result).decode("utf-8", errors="replace")
        raise MetaValidationError(f"route blob zstd {action} failed: {message}")


def zstd_compress(payload: bytes, *, level: int = META_COMPRESSION_LEVEL) -> bytes:
    try:
        import zstandard  # type: ignore
    except ImportError:
        library = _libzstd()
        source = ctypes.create_string_buffer(payload, len(payload))
        capacity = int(library.ZSTD_compressBound(len(payload)))
        destination = ctypes.create_string_buffer(capacity)
        result = int(
            library.ZSTD_compress(
                destination,
                capacity,
                source,
                len(payload),
                level,
            )
        )
        _zstd_error(library, result, "compression")
        return destination.raw[:result]
    return zstandard.ZstdCompressor(level=level).compress(payload)


def zstd_decompress(payload: bytes, *, expected_size: int) -> bytes:
    if expected_size < 0:
        raise MetaValidationError("route blob uncompressed size is invalid")
    try:
        import zstandard  # type: ignore
    except ImportError:
        library = _libzstd()
        source = ctypes.create_string_buffer(payload, len(payload))
        destination = ctypes.create_string_buffer(expected_size)
        result = int(
            library.ZSTD_decompress(
                destination,
                expected_size,
                source,
                len(payload),
            )
        )
        _zstd_error(library, result, "decompression")
        if result != expected_size:
            raise MetaValidationError(
                f"route blob uncompressed size mismatch: {result} != {expected_size}"
            )
        return destination.raw[:result]
    try:
        result = zstandard.ZstdDecompressor().decompress(
            payload, max_output_size=expected_size
        )
    except zstandard.ZstdError as exc:
        raise MetaValidationError(f"route blob zstd decompression failed: {exc}") from exc
    if len(result) != expected_size:
        raise MetaValidationError(
            f"route blob uncompressed size mismatch: {len(result)} != {expected_size}"
        )
    return result


def _sanitize_environment_provenance(
    identity: Mapping[str, object],
) -> dict[str, object]:
    provenance = identity.get("provenance")
    if not isinstance(provenance, Mapping):
        raise MetaValidationError("cache identity provenance is missing")
    result = copy.deepcopy(dict(provenance))
    topology = result.get("topology")
    if isinstance(topology, Mapping):
        result["topology"] = {
            name: copy.deepcopy(topology[name])
            for name in ("node_count", "devices_per_node", "accelerator_mode")
            if name in topology
        }
    return result


def _route_contract(identity: Mapping[str, object]) -> dict[str, object]:
    provenance = identity.get("provenance")
    if not isinstance(provenance, Mapping):
        raise MetaValidationError("cache identity provenance is missing")
    return {
        "cache_schema_version": identity.get("schema_version"),
        "capture_schema_version": identity.get("capture_schema_version"),
        "shape": copy.deepcopy(identity.get("shape")),
        "operator_order": copy.deepcopy(identity.get("operator_order")),
        "payload_contract": copy.deepcopy(identity.get("payload_contract")),
        "backend": copy.deepcopy(provenance.get("backend")),
        "adapter_sha256": copy.deepcopy(provenance.get("adapter_sha256")),
        "runner_sha256": copy.deepcopy(provenance.get("runner_sha256")),
        "install_artifacts": copy.deepcopy(provenance.get("install_artifacts")),
        "model_stack": copy.deepcopy(provenance.get("model_stack")),
        "kernel_version": copy.deepcopy(provenance.get("kernel_version")),
    }


def _route_compatibility_view(
    contract: Mapping[str, object],
) -> dict[str, object]:
    compatible = copy.deepcopy(dict(contract))
    # Route materialization is governed by the versioned data contract. Exact
    # code and binary identities remain part of performance compatibility.
    for name in (
        "adapter_sha256",
        "runner_sha256",
        "install_artifacts",
        "model_stack",
    ):
        compatible.pop(name, None)
    kernel_version = compatible.get("kernel_version")
    if isinstance(kernel_version, Mapping):
        compatible["kernel_version"] = {
            name: copy.deepcopy(value)
            for name, value in kernel_version.items()
            if name != "dispatch_peer_mode"
        }
    return compatible


def _dispatch_peer_mode(provenance: object) -> str:
    if isinstance(provenance, Mapping):
        kernel_version = provenance.get("kernel_version")
        if isinstance(kernel_version, Mapping):
            mode = kernel_version.get("dispatch_peer_mode")
            if isinstance(mode, str) and mode:
                return mode
    return "unknown"


def _checked_in_provenance(
    shape: ReplayShape,
    route_replay: Mapping[str, object],
    identity: Mapping[str, object],
) -> dict[str, object]:
    environment = _sanitize_environment_provenance(identity)
    route_provenance = route_replay.get("provenance")
    route_source = (
        route_provenance.get("source")
        if isinstance(route_provenance, Mapping)
        else "TileXR MindSpeed model route and plan capture"
    )
    return {
        "model": {
            "backend": environment.get("backend"),
            "model_stack": copy.deepcopy(environment.get("model_stack")),
            "shape": _meta_shape(shape),
        },
        "data": {
            "payload_contract": copy.deepcopy(identity.get("payload_contract")),
            "regeneration": "deterministic_seed_per_rank_call_stage",
        },
        "route_capture": {
            "source": route_source,
            "skip_calls": 60,
            "forward_calls": shape.forward_calls,
            "deduplication": "global-original-int32-topk-sha256",
        },
        "code": {
            "tilexr_git_sha": environment.get("tilexr_git_sha"),
            "adapter_sha256": environment.get("adapter_sha256"),
            "runner_sha256": environment.get("runner_sha256"),
        },
    }


def _assert_no_private_data(value: object, *, location: str = "meta") -> None:
    if isinstance(value, Mapping):
        for key, item in value.items():
            name = str(key).lower()
            if name in _PRIVATE_KEYS:
                raise MetaValidationError(f"private field {key!r} is forbidden in {location}")
            _assert_no_private_data(item, location=f"{location}.{key}")
        return
    if isinstance(value, list):
        for index, item in enumerate(value):
            _assert_no_private_data(item, location=f"{location}[{index}]")
        return
    if isinstance(value, str):
        if value.startswith("/") or _WINDOWS_ABSOLUTE.match(value):
            raise MetaValidationError(f"absolute path is forbidden in {location}")
        match = _IPV4.search(value)
        if match and all(0 <= int(part) <= 255 for part in match.group(0).split(".")):
            raise MetaValidationError(f"IP address is forbidden in {location}")


def _validate_performance_provenance(
    performance: Mapping[str, object], shape: ReplayShape
) -> None:
    provenance = performance.get("provenance")
    if not isinstance(provenance, Mapping) or not _REQUIRED_PERFORMANCE_PROVENANCE.issubset(
        provenance
    ):
        raise MetaValidationError("model performance provenance is incomplete")
    for name in ("tilexr_git_sha", "cann", "driver", "firmware", "soc"):
        value = provenance.get(name)
        if not isinstance(value, str) or not value or value == "unavailable":
            raise MetaValidationError(f"model performance provenance {name} is unavailable")
    controls = provenance.get("execution_controls")
    if not isinstance(controls, Mapping) or controls.get("framework_profiler") is not True:
        raise MetaValidationError("model performance framework profiler must be enabled")
    if controls.get("stage_barrier") is not True:
        raise MetaValidationError("model performance stage barrier must be enabled")
    if performance.get("profiler_enabled") is not controls.get("framework_profiler"):
        raise MetaValidationError("model performance profiler state is inconsistent")
    topology = provenance.get("topology")
    if not isinstance(topology, Mapping) or topology.get("node_count") != (
        shape.world_size + 7
    ) // 8 or topology.get("devices_per_node") != min(8, shape.world_size):
        raise MetaValidationError("model performance topology is invalid")
    rank_mapping = provenance.get("rank_mapping")
    if not isinstance(rank_mapping, list) or len(rank_mapping) != shape.world_size:
        raise MetaValidationError("model performance rank mapping is invalid")
    if [record.get("rank") for record in rank_mapping if isinstance(record, Mapping)] != list(
        range(shape.world_size)
    ):
        raise MetaValidationError("model performance rank mapping is invalid")
    reference = performance.get("reference")
    if not isinstance(reference, Mapping) or reference.get("classification") != (
        "checked-in reference"
    ):
        raise MetaValidationError("model performance reference metadata is invalid")


def _exchange_directories(left: Path, right: Path) -> bool:
    if os.name != "posix":
        return False
    library = ctypes.CDLL(None, use_errno=True)
    renameat2 = getattr(library, "renameat2", None)
    if renameat2 is None:
        return False
    renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    renameat2.restype = ctypes.c_int
    if renameat2(-100, os.fsencode(left), -100, os.fsencode(right), 2) == 0:
        return True
    error = ctypes.get_errno()
    if error in (errno.ENOSYS, errno.EINVAL, errno.ENOTSUP, errno.EXDEV):
        return False
    raise OSError(error, os.strerror(error), str(left), str(right))


def _decode_runtime_topk(record: Mapping[str, object], shape: ReplayShape) -> bytes:
    if record.get("topk_encoding") != "int32-le-zlib-base64":
        raise MetaValidationError("runtime TopK encoding is invalid")
    try:
        raw = zlib.decompress(
            base64.b64decode(record["topk_zlib_base64"], validate=True)
        )
    except (KeyError, TypeError, ValueError, zlib.error) as exc:
        raise MetaValidationError("runtime TopK payload is invalid") from exc
    expected = shape.tokens_per_rank * shape.topk * 4
    if len(raw) != expected or _sha256_bytes(raw) != record.get("topk_sha256"):
        raise MetaValidationError("runtime TopK payload checksum mismatch")
    return raw


def _sanitize_performance(
    performance: Mapping[str, object], identity: Mapping[str, object]
) -> dict[str, object]:
    sanitized = copy.deepcopy(dict(performance))
    ranks = sanitized.get("ranks")
    if not isinstance(ranks, dict):
        raise MetaValidationError("model performance ranks are missing")
    for rank_record in ranks.values():
        if isinstance(rank_record, dict):
            rank_record.pop("timing_artifact", None)
    environment = _sanitize_environment_provenance(identity)
    sanitized["provenance"] = environment
    sanitized["reference"] = {
        "classification": "checked-in reference",
        "captured_environment_sha256": _sha256_bytes(_canonical_bytes(environment)),
    }
    _assert_no_private_data(sanitized, location="performance")
    return sanitized


def _build_meta_payloads(
    shape: ReplayShape,
    route_replay: Mapping[str, object],
    performance: Mapping[str, object],
    identity: Mapping[str, object],
) -> tuple[dict[str, object], bytes, bytes]:
    if shape.expert_count > 256:
        raise MetaValidationError(
            f"E={shape.expert_count} cannot be represented by model replay meta uint8"
        )
    validate_route_replay(route_replay, shape)
    validate_model_performance(performance, identity)
    ranks = route_replay["ranks"]
    unique_by_topk: dict[str, int] = {}
    unique_raw: dict[str, bytes] = {}
    unique_routes = []
    rank_calls: dict[str, list[dict[str, object]]] = {}
    route_chunks = []
    offset = 0
    values_per_route = shape.tokens_per_rank * shape.topk
    for rank in range(shape.world_size):
        calls = []
        for call, source_record in enumerate(ranks[str(rank)]):
            record = dict(source_record)
            int32_raw = _decode_runtime_topk(record, shape)
            values = struct.unpack(f"<{values_per_route}i", int32_raw)
            if any(value < 0 or value >= shape.expert_count for value in values):
                raise MetaValidationError(
                    f"rank {rank} call {call} contains an out-of-range expert ID"
                )
            u8_raw = bytes(values)
            topk_sha256 = _sha256_bytes(int32_raw)
            route_id = unique_by_topk.get(topk_sha256)
            if route_id is None:
                route_id = len(unique_routes)
                unique_by_topk[topk_sha256] = route_id
                unique_raw[topk_sha256] = int32_raw
                unique_routes.append(
                    {
                        "route_id": route_id,
                        "offset": offset,
                        "length": len(u8_raw),
                        "sha256": topk_sha256,
                        "payload_sha256": _sha256_bytes(u8_raw),
                        "source_rank": rank,
                        "source_call": int(record.get("source_call", call)),
                    }
                )
                route_chunks.append(u8_raw)
                offset += len(u8_raw)
            elif unique_raw[topk_sha256] != int32_raw:
                raise MetaValidationError("TopK SHA256 collision has inconsistent payloads")
            calls.append(
                {
                    "call": call,
                    "source_call": int(record.get("source_call", call)),
                    "route_id": route_id,
                    "tokens_per_expert": copy.deepcopy(record.get("tokens_per_expert")),
                    "remote_stats": copy.deepcopy(record.get("remote_stats")),
                    "experts_to_copy": copy.deepcopy(record.get("experts_to_copy")),
                    "payload_seed": int(record.get("payload_seed", 0)),
                    "tensor_descriptors": copy.deepcopy(
                        record.get("tensor_descriptors", {})
                    ),
                }
            )
        rank_calls[str(rank)] = calls
    route_raw = b"".join(route_chunks)
    compressed = zstd_compress(route_raw, level=META_COMPRESSION_LEVEL)
    sanitized_performance = _sanitize_performance(performance, identity)
    performance_bytes = _pretty_json_bytes(sanitized_performance)
    route_contract = _route_contract(identity)
    meta: dict[str, object] = {
        "schema_version": META_SCHEMA_VERSION,
        "case_id": meta_case_id(shape),
        "shape": _meta_shape(shape),
        "routes": {
            "file": "routes.u8.zst",
            "codec": META_ROUTE_CODEC,
            "dtype": META_ROUTE_DTYPE,
            "compression_level": META_COMPRESSION_LEVEL,
            "uncompressed_size": len(route_raw),
            "uncompressed_sha256": _sha256_bytes(route_raw),
            "compressed_size": len(compressed),
            "compressed_sha256": _sha256_bytes(compressed),
        },
        "unique_routes": unique_routes,
        "rank_calls": rank_calls,
        "compatibility": {
            "route_contract": route_contract,
            "route_contract_sha256": _sha256_bytes(_canonical_bytes(route_contract)),
        },
        "provenance": _checked_in_provenance(shape, route_replay, identity),
        "performance": {
            "file": "performance.json",
            "sha256": _sha256_bytes(performance_bytes),
        },
    }
    _assert_no_private_data(meta)
    return meta, compressed, performance_bytes


def _validate_shape(meta: Mapping[str, object], shape: ReplayShape) -> None:
    if meta.get("shape") != _meta_shape(shape):
        raise MetaValidationError("meta shape mismatch")
    if meta.get("case_id") != meta_case_id(shape):
        raise MetaValidationError("meta case ID mismatch")


def _validate_route_contract(
    meta: Mapping[str, object], identity: Mapping[str, object]
) -> None:
    compatibility = meta.get("compatibility")
    if not isinstance(compatibility, Mapping):
        raise MetaValidationError("meta compatibility contract is missing")
    contract = compatibility.get("route_contract")
    expected = _route_contract(identity)
    if not isinstance(contract, Mapping):
        raise MetaValidationError("meta route contract is missing")
    recorded_sha = compatibility.get("route_contract_sha256")
    if recorded_sha != _sha256_bytes(_canonical_bytes(dict(contract))):
        raise MetaValidationError("meta route contract checksum mismatch")
    if _canonical_bytes(_route_compatibility_view(contract)) != _canonical_bytes(
        _route_compatibility_view(expected)
    ):
        raise MetaValidationError("meta route contract is incompatible")


def _validate_rank_calls(
    meta: Mapping[str, object], shape: ReplayShape, route_raw: bytes
) -> None:
    unique = meta.get("unique_routes")
    if not isinstance(unique, list) or not unique:
        raise MetaValidationError("meta unique routes are missing")
    expected_route_bytes = shape.tokens_per_rank * shape.topk
    decoded_values: dict[int, list[int]] = {}
    expected_offset = 0
    for route_id, route in enumerate(unique):
        if not isinstance(route, Mapping) or route.get("route_id") != route_id:
            raise MetaValidationError("meta route_id ordering is invalid")
        offset = route.get("offset")
        length = route.get("length")
        if offset != expected_offset or length != expected_route_bytes:
            raise MetaValidationError("meta unique route offset or length is invalid")
        end = int(offset) + int(length)
        payload = route_raw[int(offset) : end]
        if len(payload) != length or _sha256_bytes(payload) != route.get(
            "payload_sha256"
        ):
            raise MetaValidationError("meta unique route payload checksum mismatch")
        values = list(payload)
        if any(value >= shape.expert_count for value in values):
            raise MetaValidationError("meta unique route has out-of-range expert ID")
        int32_raw = struct.pack(f"<{len(values)}i", *values)
        if _sha256_bytes(int32_raw) != route.get("sha256"):
            raise MetaValidationError("meta unique route TopK checksum mismatch")
        decoded_values[route_id] = values
        expected_offset = end
    if expected_offset != len(route_raw):
        raise MetaValidationError("meta route offsets do not cover the route blob")

    rank_calls = meta.get("rank_calls")
    if not isinstance(rank_calls, Mapping):
        raise MetaValidationError("meta rank calls are missing")
    expected_ranks = {str(rank) for rank in range(shape.world_size)}
    if set(rank_calls) != expected_ranks:
        raise MetaValidationError("meta rank coverage is invalid")
    for rank in range(shape.world_size):
        calls = rank_calls[str(rank)]
        if not isinstance(calls, list) or len(calls) != shape.forward_calls:
            raise MetaValidationError(f"meta rank {rank} call coverage is invalid")
        for call, record in enumerate(calls):
            if not isinstance(record, Mapping) or record.get("call") != call:
                raise MetaValidationError(f"meta rank {rank} call ordering is invalid")
            route_id = record.get("route_id")
            if (
                isinstance(route_id, bool)
                or not isinstance(route_id, int)
                or route_id not in decoded_values
            ):
                raise MetaValidationError(
                    f"meta rank {rank} call {call} route_id is invalid"
                )
            source_call = record.get("source_call")
            if (
                isinstance(source_call, bool)
                or not isinstance(source_call, int)
                or source_call < 0
            ):
                raise MetaValidationError(
                    f"meta rank {rank} call {call} source_call is invalid"
                )
            counts = record.get("tokens_per_expert")
            if (
                not isinstance(counts, list)
                or len(counts) != shape.expert_count
                or any(
                    isinstance(value, bool) or not isinstance(value, int) or value < 0
                    for value in counts
                )
            ):
                raise MetaValidationError(
                    f"meta rank {rank} call {call} histogram is invalid"
                )
            expected_counts = [
                decoded_values[route_id].count(expert)
                for expert in range(shape.expert_count)
            ]
            if counts != expected_counts:
                raise MetaValidationError(
                    f"meta rank {rank} call {call} histogram mismatch"
                )
            remote_stats = record.get("remote_stats")
            if (
                not isinstance(remote_stats, list)
                or len(remote_stats) != 2
                or any(
                    isinstance(value, bool) or not isinstance(value, int) or value < 0
                    for value in remote_stats
                )
            ):
                raise MetaValidationError(
                    f"meta rank {rank} call {call} remote_stats are invalid"
                )
            experts = record.get("experts_to_copy")
            if (
                not isinstance(experts, list)
                or len(experts) != shape.world_size
                or any(
                    not isinstance(row, list) or len(row) != shape.experts_per_rank
                    for row in experts
                )
                or any(
                    isinstance(expert, bool)
                    or not isinstance(expert, int)
                    or expert < -1
                    or expert >= shape.expert_count
                    for row in experts
                    for expert in row
                )
            ):
                raise MetaValidationError(
                    f"meta rank {rank} call {call} experts_to_copy are invalid"
                )
            if not isinstance(record.get("tensor_descriptors"), Mapping):
                raise MetaValidationError(
                    f"meta rank {rank} call {call} tensor descriptors are invalid"
                )


def _load_bundle_path(
    path: Path, shape: ReplayShape, identity: Mapping[str, object]
) -> MetaBundle:
    if not path.is_dir():
        raise MetaValidationError(f"meta bundle directory is missing: {path}")
    actual_files = {item.name for item in path.iterdir() if item.is_file()}
    if actual_files != _META_FILENAMES:
        raise MetaValidationError("meta bundle file set is incomplete or unexpected")
    meta = _read_json(path / "meta.json", "model replay meta")
    if meta.get("schema_version") != META_SCHEMA_VERSION:
        raise MetaValidationError("meta schema version mismatch")
    _validate_shape(meta, shape)
    _validate_route_contract(meta, identity)
    _assert_no_private_data(meta)

    route_info = meta.get("routes")
    if not isinstance(route_info, Mapping):
        raise MetaValidationError("meta route blob descriptor is missing")
    expected_route_info = {
        "file": "routes.u8.zst",
        "codec": META_ROUTE_CODEC,
        "dtype": META_ROUTE_DTYPE,
        "compression_level": META_COMPRESSION_LEVEL,
    }
    if any(route_info.get(name) != value for name, value in expected_route_info.items()):
        raise MetaValidationError("meta route blob format is unsupported")
    compressed = (path / "routes.u8.zst").read_bytes()
    if len(compressed) != route_info.get("compressed_size") or _sha256_bytes(
        compressed
    ) != route_info.get("compressed_sha256"):
        raise MetaValidationError("route blob compressed checksum mismatch")
    expected_size = route_info.get("uncompressed_size")
    if isinstance(expected_size, bool) or not isinstance(expected_size, int):
        raise MetaValidationError("route blob uncompressed size is invalid")
    unique_routes = meta.get("unique_routes")
    if (
        not isinstance(unique_routes, list)
        or not unique_routes
        or len(unique_routes) > shape.world_size * shape.forward_calls
        or expected_size
        != len(unique_routes) * shape.tokens_per_rank * shape.topk
    ):
        raise MetaValidationError("route blob uncompressed size is invalid")
    route_raw = zstd_decompress(compressed, expected_size=expected_size)
    if _sha256_bytes(route_raw) != route_info.get("uncompressed_sha256"):
        raise MetaValidationError("route blob uncompressed checksum mismatch")
    _validate_rank_calls(meta, shape, route_raw)

    performance_info = meta.get("performance")
    if not isinstance(performance_info, Mapping):
        raise MetaValidationError("meta performance descriptor is missing")
    performance_path = path / "performance.json"
    if performance_info.get("file") != "performance.json" or _sha256_file(
        performance_path
    ) != performance_info.get("sha256"):
        raise MetaValidationError("meta performance checksum mismatch")
    performance = _read_json(performance_path, "model replay performance")
    _assert_no_private_data(performance, location="performance")
    validate_model_performance(performance, identity)
    _validate_performance_provenance(performance, shape)
    return MetaBundle(path=path, meta=meta, performance=performance, route_bytes=route_raw)


def write_meta_bundle(
    meta_root: str | Path,
    shape: ReplayShape,
    route_replay: Mapping[str, object],
    performance: Mapping[str, object],
    identity: Mapping[str, object],
    *,
    replace: bool,
) -> MetaBundle:
    root = Path(meta_root)
    root.mkdir(parents=True, exist_ok=True)
    destination = root / meta_case_id(shape)
    if destination.exists() and not replace:
        raise FileExistsError(f"model replay meta already exists: {destination}")
    meta, compressed, performance_bytes = _build_meta_payloads(
        shape, route_replay, performance, identity
    )
    staging = root / f".{destination.name}.staging-{uuid.uuid4().hex}"
    backup: Path | None = None
    staging.mkdir()
    try:
        _write_bytes_fsync(staging / "routes.u8.zst", compressed)
        _write_bytes_fsync(staging / "performance.json", performance_bytes)
        _write_bytes_fsync(staging / "meta.json", _canonical_bytes(meta) + b"\n")
        _fsync_directory(staging)
        _load_bundle_path(staging, shape, identity)
        if destination.exists() and _exchange_directories(staging, destination):
            _fsync_directory(root)
            shutil.rmtree(staging)
            _fsync_directory(root)
        elif destination.exists():
            backup = root / f".{destination.name}.backup-{uuid.uuid4().hex}"
            os.replace(destination, backup)
            try:
                os.replace(staging, destination)
            except Exception:
                os.replace(backup, destination)
                backup = None
                _fsync_directory(root)
                raise
        else:
            os.replace(staging, destination)
        _fsync_directory(root)
        if backup is not None:
            shutil.rmtree(backup)
            backup = None
            _fsync_directory(root)
    except Exception:
        if staging.exists():
            shutil.rmtree(staging)
        if backup is not None and backup.exists() and not destination.exists():
            os.replace(backup, destination)
            _fsync_directory(root)
        raise
    return _load_bundle_path(destination, shape, identity)


def load_meta_bundle(
    meta_root: str | Path,
    shape: ReplayShape,
    identity: Mapping[str, object],
) -> MetaBundle | None:
    path = Path(meta_root) / meta_case_id(shape)
    if not path.exists():
        return None
    return _load_bundle_path(path, shape, identity)


def materialize_meta_bundle(
    bundle: MetaBundle, identity: Mapping[str, object]
) -> tuple[dict[str, object], dict[str, object]]:
    shape_values = bundle.meta["shape"]
    shape = ReplayShape(
        tokens_per_rank=int(shape_values["S"]),
        topk=int(shape_values["K"]),
        hidden_size=int(shape_values["H"]),
        ep_size=int(shape_values["EP"]),
        world_size=int(shape_values["R"]),
        expert_count=int(shape_values["E"]),
        ffn_hidden_size=int(shape_values["Hf"]),
        token_padding=int(shape_values["P"]),
        forward_calls=int(shape_values["forward_calls"]),
    )
    _validate_route_contract(bundle.meta, identity)
    unique = bundle.meta["unique_routes"]
    rank_calls = bundle.meta["rank_calls"]
    records: dict[str, list[dict[str, object]]] = {}
    hashes: dict[str, list[str]] = {}
    for rank in range(shape.world_size):
        calls = []
        rank_hashes = []
        for call, compact in enumerate(rank_calls[str(rank)]):
            route = unique[compact["route_id"]]
            offset = int(route["offset"])
            length = int(route["length"])
            values = list(bundle.route_bytes[offset : offset + length])
            int32_raw = struct.pack(f"<{len(values)}i", *values)
            topk_sha256 = _sha256_bytes(int32_raw)
            calls.append(
                {
                    "call": call,
                    "source_call": compact["source_call"],
                    "topk_shape": [shape.tokens_per_rank, shape.topk],
                    "topk_dtype": "torch.int32",
                    "topk_encoding": "int32-le-zlib-base64",
                    "topk_zlib_base64": base64.b64encode(
                        zlib.compress(int32_raw)
                    ).decode("ascii"),
                    "topk_sha256": topk_sha256,
                    "tokens_per_expert": copy.deepcopy(
                        compact["tokens_per_expert"]
                    ),
                    "remote_stats": copy.deepcopy(compact["remote_stats"]),
                    "experts_to_copy": copy.deepcopy(compact["experts_to_copy"]),
                    "payload_seed": compact["payload_seed"],
                    "tensor_descriptors": copy.deepcopy(
                        compact["tensor_descriptors"]
                    ),
                }
            )
            rank_hashes.append(topk_sha256)
        records[str(rank)] = calls
        hashes[str(rank)] = rank_hashes
    replay: dict[str, object] = {
        "schema_version": 1,
        "dimensions": {
            "world_size": shape.world_size,
            "tokens_per_rank": shape.tokens_per_rank,
            "topk": shape.topk,
            "expert_count": shape.expert_count,
            "forward_calls": shape.forward_calls,
        },
        "payload_contract": {
            "captured": "topk,plan_metadata,tensor_descriptors",
            "regenerated": "payload_values_from_payload_seed",
        },
        "provenance": {
            "source": "checked-in TileXR model replay meta",
            "meta_schema_version": META_SCHEMA_VERSION,
            "topk_sha256_by_rank": hashes,
        },
        "ranks": records,
    }
    validate_route_replay(replay, shape)

    performance = copy.deepcopy(bundle.performance)
    captured = performance.get("provenance")
    current = _sanitize_environment_provenance(identity)
    compatible = _canonical_bytes(captured) == _canonical_bytes(current)
    captured_peer_mode = _dispatch_peer_mode(captured)
    current_peer_mode = _dispatch_peer_mode(current)
    performance["comparison"] = {
        "compatible": compatible,
        "classification": (
            "direct baseline" if compatible else "checked-in reference"
        ),
        "current_environment_sha256": _sha256_bytes(_canonical_bytes(current)),
        "captured_environment_sha256": _sha256_bytes(_canonical_bytes(captured)),
        "dispatch_peer_mode": {
            "compatible": captured_peer_mode == current_peer_mode,
            "model": captured_peer_mode,
            "replay": current_peer_mode,
        },
    }
    validate_model_performance(performance, identity)
    return replay, performance


def materialize_meta_cache(
    bundle: MetaBundle,
    cache_root: str | Path,
    identity: Mapping[str, object],
    *,
    capture_id: str,
) -> CacheEntry:
    staging = begin_cache_capture(cache_root, identity, capture_id=capture_id)
    replay, performance = materialize_meta_bundle(bundle, identity)
    replay_path = staging / "replay" / "route_replay.json"
    performance_path = staging / "model" / "performance.json"
    replay_path.parent.mkdir(parents=True)
    performance_path.parent.mkdir(parents=True)
    _write_bytes_fsync(replay_path, _pretty_json_bytes(replay))
    _write_bytes_fsync(performance_path, _pretty_json_bytes(performance))
    _fsync_directory(replay_path.parent)
    _fsync_directory(performance_path.parent)
    return publish_cache(staging, identity, capture_id=capture_id)
