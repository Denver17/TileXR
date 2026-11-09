# 用当前 Torch/torch_npu 的扩展工具和 ABI，以 C++17 构建 tilexr_elastic._C。
# 动态链接 TILEXR_INSTALL_PREFIX 指定的 Host 库，使用 ASCEND_HOME_PATH 的 CANN 头文件；
# AICore binary 由 TileXR 构建和安装，本文件不编译或嵌入它们。
import os
from pathlib import Path

from setuptools import find_packages, setup
from torch.utils.cpp_extension import BuildExtension
from torch_npu.utils.cpp_extension import NpuExtension

HERE = Path(__file__).resolve().parent
PREFIX = Path(os.environ.get("TILEXR_INSTALL_PREFIX", HERE.parents[1] / "install")).resolve()
CANN = Path(os.environ["ASCEND_HOME_PATH"])
DEFAULT_LIBDIR = "lib64" if (PREFIX / "lib64" / "libtilexr-elastic-memory.so").is_file() else "lib"
LIB = PREFIX / os.environ.get("TILEXR_INSTALL_LIBDIR", DEFAULT_LIBDIR)

setup(
    name="tilexr-elastic",
    version="0.1.0",
    packages=find_packages(),
    ext_modules=[
        NpuExtension(
            "tilexr_elastic._C",
            [str(HERE / "bindings.cpp")],
            include_dirs=[str(PREFIX / "include"), str(CANN / "include")],
            library_dirs=[str(LIB), str(CANN / "lib64")],
            libraries=["tilexr-elastic-memory", "tilexr-ep", "tile-comm", "ascendcl"],
            runtime_library_dirs=[str(LIB)],
            extra_compile_args=["-O2", "-std=c++17", "-pthread"],
            extra_link_args=["-pthread"],
        )
    ],
    cmdclass={"build_ext": BuildExtension},
)
