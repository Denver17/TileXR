"""Compile/link TileLang device source into an external ELF with provenance.

This build-only helper uses the pinned Ascend toolchain, not a Host launch
wrapper. SHMEM includes satisfy the pinned compiler's common.h dependency;
they do not select SHMEM communication or add a Relay runtime dependency.
"""

import hashlib
import os
from pathlib import Path
import shutil
import subprocess


def compile_device(source, directory, stem="relay"):
    """Use the pinned compiler flags, returning ELF and exact build provenance."""
    from tilelang.env import TILELANG_TEMPLATE_PATH
    from tilelang.jit.adapter.libgen import resolve_compile_flags

    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    compiler = shutil.which("bisheng")
    if compiler is None:
        raise RuntimeError("source the target CANN environment: bisheng unavailable")
    sdk = Path(os.environ["ASCEND_HOME_PATH"])
    tl_root = Path(os.environ["TL_ROOT"])
    cpp, obj = directory / (stem + ".cpp"), directory / (stem + ".device.o")
    elf = directory / (stem + ".o")
    cpp.write_text(source)
    command = [compiler, "--npu-arch=dav-2201", "-std=c++17", "--aicore-only",
               "--asc-aicore-lang", "-fPIC", "-c", str(cpp), "-o", str(obj),
               "-Wno-macro-redefined", "-Wno-ignored-attributes",
               f"-I{sdk}/include", f"-I{sdk}/pkg_inc", f"-I{sdk}/pkg_inc/runtime",
               f"-I{tl_root}/3rdparty/catlass/include", f"-I{TILELANG_TEMPLATE_PATH}",
               f"-I{tl_root}/3rdparty/shmem/include", f"-I{tl_root}/3rdparty/shmem/src/device",
               "-DBACKEND_HYBM"] + resolve_compile_flags("ascendc")
    subprocess.run(command, check=True, timeout=180)
    linker = Path(compiler).resolve().parent / "ld.lld"
    link = [str(linker), "-m", "aicorelinux", "-Ttext=0", str(obj), "-static", "-o", str(elf)]
    subprocess.run(link, check=True, timeout=60)
    return elf, {"compile": command, "link": link,
                 "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
                 "binary_sha256": hashlib.sha256(elf.read_bytes()).hexdigest()}
