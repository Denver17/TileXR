#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TILEXR_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

REMOTE="${TILEXR_VLLM_REMOTE:?set TILEXR_VLLM_REMOTE to the SSH target for remote vllm collectives validation}"
REMOTE_BASE="${TILEXR_VLLM_REMOTE_BASE:?set TILEXR_VLLM_REMOTE_BASE to a scratch directory on the remote host}"
REMOTE_REPO="${REMOTE_BASE}/TileXR"
REMOTE_LOG="${REMOTE_BASE}/logs/deploy.log"
REMOTE_ASCEND_HOME_PATH="${TILEXR_VLLM_REMOTE_ASCEND_HOME_PATH:-}"
REMOTE_ASCEND_DRIVER_PATH="${TILEXR_VLLM_REMOTE_ASCEND_DRIVER_PATH:-}"
REMOTE_CMAKE_CCE_COMPILER="${TILEXR_VLLM_REMOTE_CMAKE_CCE_COMPILER:-}"
REMOTE_PYTHON="${TILEXR_VLLM_REMOTE_PYTHON:-}"
REMOTE_PYTHONPATH="${TILEXR_VLLM_REMOTE_PYTHONPATH:-}"
REMOTE_CONDA_ENV="${TILEXR_VLLM_REMOTE_CONDA_ENV:-}"
REMOTE_CONDA_SH="${TILEXR_VLLM_REMOTE_CONDA_SH:-/home/miniconda3/etc/profile.d/conda.sh}"
REMOTE_VLLM_SOURCE="${TILEXR_VLLM_REMOTE_VLLM_SOURCE:-}"
REMOTE_VLLM_ASCEND_SOURCE="${TILEXR_VLLM_REMOTE_VLLM_ASCEND_SOURCE:-}"
REMOTE_DUMMY_MODEL="${TILEXR_VLLM_REMOTE_DUMMY_MODEL:-}"
REMOTE_VLLM_PLUGINS="${TILEXR_VLLM_REMOTE_VLLM_PLUGINS:-ascend}"
REMOTE_NNAL_ATB_SET_ENV="${TILEXR_VLLM_REMOTE_NNAL_ATB_SET_ENV:-}"
REMOTE_SSH_OPTS="${TILEXR_VLLM_REMOTE_SSH_OPTS:-}"

branch="$(git -C "${TILEXR_ROOT}" rev-parse --abbrev-ref HEAD)"
commit="$(git -C "${TILEXR_ROOT}" rev-parse HEAD)"
staging_dir="$(mktemp -d "${TMPDIR:-/tmp}/tilexr_vllm_collectives.XXXXXX")"
trap 'rm -rf "${staging_dir}"' EXIT
staging_repo="${staging_dir}/TileXR"

echo "Deploying TileXR vllm collectives validation"
echo "  remote: ${REMOTE}"
echo "  remote base: ${REMOTE_BASE}"
echo "  branch: ${branch}"
echo "  commit: ${commit}"
if [[ -n "${REMOTE_PYTHON}" ]]; then
  echo "  remote python: ${REMOTE_PYTHON}"
fi
if [[ -n "${REMOTE_PYTHONPATH}" ]]; then
  echo "  remote pythonpath: ${REMOTE_PYTHONPATH}"
fi
if [[ -n "${REMOTE_CONDA_ENV}" ]]; then
  echo "  remote conda env: ${REMOTE_CONDA_ENV}"
fi
if [[ -n "${REMOTE_VLLM_SOURCE}" ]]; then
  echo "  remote vllm source: ${REMOTE_VLLM_SOURCE}"
fi
if [[ -n "${REMOTE_VLLM_ASCEND_SOURCE}" ]]; then
  echo "  remote vllm-ascend source: ${REMOTE_VLLM_ASCEND_SOURCE}"
fi
if [[ -n "${REMOTE_DUMMY_MODEL}" ]]; then
  echo "  remote dummy model: ${REMOTE_DUMMY_MODEL}"
fi
if [[ -n "${REMOTE_VLLM_PLUGINS}" ]]; then
  echo "  remote vllm plugins: ${REMOTE_VLLM_PLUGINS}"
fi
if [[ -n "${REMOTE_NNAL_ATB_SET_ENV}" ]]; then
  echo "  remote NNAL ATB set_env: ${REMOTE_NNAL_ATB_SET_ENV}"
fi
if [[ -n "${REMOTE_SSH_OPTS}" ]]; then
  echo "  remote ssh opts: ${REMOTE_SSH_OPTS}"
fi

ssh_extra_args=()
if [[ -n "${REMOTE_SSH_OPTS}" ]]; then
  read -r -a ssh_extra_args <<< "${REMOTE_SSH_OPTS}"
fi
ssh_args=("${ssh_extra_args[@]}" "${REMOTE}")
rsync_ssh_cmd=(ssh "${ssh_extra_args[@]}")

git clone --no-hardlinks --no-checkout "${TILEXR_ROOT}" "${staging_repo}"
git -C "${staging_repo}" checkout --detach "${commit}"

sync_local_submodule() {
  local rel_path="$1"
  local src="${TILEXR_ROOT}/${rel_path}"
  local dst="${staging_repo}/${rel_path}"
  if [[ ! -d "${src}" ]]; then
    echo "ERROR: required local submodule is missing: ${rel_path}" >&2
    echo "Initialize local submodules before running this script." >&2
    exit 1
  fi
  mkdir -p "${dst}"
  rsync -a --delete --exclude='.git' "${src}/" "${dst}/"
}

sync_local_submodule "3rdparty/spdlog"

remote_prepare=$(cat <<EOF
set -euo pipefail
remote_base=$(printf '%q' "${REMOTE_BASE}")
remote_repo=$(printf '%q' "${REMOTE_REPO}")
case "\${remote_repo}" in
  "\${remote_base}"/TileXR)
    rm -rf -- "\${remote_repo}"
    mkdir -p -- "\${remote_repo}"
    mkdir -p -- "\${remote_base}/logs" "\${remote_base}/artifacts"
    ;;
  *)
    echo "Refusing to clean unexpected remote repo: \${remote_repo}" >&2
    exit 2
    ;;
esac
EOF
)

ssh "${ssh_args[@]}" "bash -lc $(printf '%q' "${remote_prepare}")"

rsync -e "${rsync_ssh_cmd[*]}" -a --delete \
  --exclude='.worktrees' \
  --exclude='build' \
  --exclude='build_*' \
  --exclude='build-*' \
  --exclude='install' \
  --exclude='run' \
  --exclude='env/temp' \
  "${staging_repo}/" "${REMOTE}:${REMOTE_REPO}/"

remote_script=$(cat <<EOF
set -euo pipefail
remote_ascend_home_path=$(printf '%q' "${REMOTE_ASCEND_HOME_PATH}")
remote_ascend_driver_path=$(printf '%q' "${REMOTE_ASCEND_DRIVER_PATH}")
remote_cmake_cce_compiler=$(printf '%q' "${REMOTE_CMAKE_CCE_COMPILER}")
remote_python=$(printf '%q' "${REMOTE_PYTHON}")
remote_pythonpath=$(printf '%q' "${REMOTE_PYTHONPATH}")
remote_conda_env=$(printf '%q' "${REMOTE_CONDA_ENV}")
remote_conda_sh=$(printf '%q' "${REMOTE_CONDA_SH}")
remote_vllm_source=$(printf '%q' "${REMOTE_VLLM_SOURCE}")
remote_vllm_ascend_source=$(printf '%q' "${REMOTE_VLLM_ASCEND_SOURCE}")
remote_dummy_model=$(printf '%q' "${REMOTE_DUMMY_MODEL}")
remote_vllm_plugins=$(printf '%q' "${REMOTE_VLLM_PLUGINS}")
remote_nnal_atb_set_env=$(printf '%q' "${REMOTE_NNAL_ATB_SET_ENV}")
cd $(printf '%q' "${REMOTE_REPO}")
select_remote_python() {
  selected_python="python3"
  if [[ -n "\${remote_python}" ]]; then
    if [[ -n "\${remote_conda_env}" ]]; then
      echo "INFO: TILEXR_VLLM_REMOTE_PYTHON is set; ignoring TILEXR_VLLM_REMOTE_CONDA_ENV=\${remote_conda_env}"
    fi
    selected_python="\${remote_python}"
  elif [[ -n "\${remote_conda_env}" ]]; then
    if [[ ! -f "\${remote_conda_sh}" ]]; then
      echo "ERROR: conda activation script not found: \${remote_conda_sh}" >&2
      return 2
    fi
    set +u
    source "\${remote_conda_sh}"
    conda activate "\${remote_conda_env}"
    set -u
    selected_python="\$(command -v python)"
  fi
  if ! command -v "\${selected_python}" >/dev/null 2>&1; then
    echo "ERROR: selected Python not found: \${selected_python}" >&2
    return 2
  fi
  selected_python="\$(command -v "\${selected_python}")"
  export TILEXR_VLLM_SELECTED_PYTHON="\${selected_python}"
}

dump_selected_python_environment() {
  echo "Selected Python: \${selected_python}"
  "\${selected_python}" --version
  "\${selected_python}" - <<'PY'
import importlib.util
import sys

print("Python executable:", sys.executable)
print("Python version:", sys.version.replace("\n", " "))
print("sys.path prefix:", sys.path[:5])
for name in ["torch", "torch_npu", "vllm", "vllm_ascend"]:
    spec = importlib.util.find_spec(name)
    print(f"{name}: {spec.origin if spec else 'MISSING'}")
PY
  "\${selected_python}" -m pip show torch || true
  "\${selected_python}" -m pip show torch-npu || true
  "\${selected_python}" -m pip show vllm || true
  "\${selected_python}" -m pip show vllm-ascend || true
}

build_vllm_probe_pythonpath() {
  local pythonpath_entries=("integrations/vllm_ascend")
  if [[ -n "\${remote_pythonpath}" ]]; then
    pythonpath_entries+=("\${remote_pythonpath}")
  fi
  if [[ -n "\${remote_vllm_source}" ]]; then
    if [[ -d "\${remote_vllm_source}" ]]; then
      pythonpath_entries+=("\${remote_vllm_source}")
    else
      echo "WARN: TILEXR_VLLM_REMOTE_VLLM_SOURCE does not exist: \${remote_vllm_source}"
    fi
  fi
  if [[ -n "\${remote_vllm_ascend_source}" ]]; then
    if [[ -d "\${remote_vllm_ascend_source}" ]]; then
      pythonpath_entries+=("\${remote_vllm_ascend_source}")
    else
      echo "WARN: TILEXR_VLLM_REMOTE_VLLM_ASCEND_SOURCE does not exist: \${remote_vllm_ascend_source}"
    fi
  fi
  local IFS=:
  vllm_probe_pythonpath="\${pythonpath_entries[*]}"
}

probe_vllm_environment() {
  local probe_label="\${1:?probe label required}"
  build_vllm_probe_pythonpath
  VLLM_ASCEND_TILEXR_COLLECTIVES=0 TILEXR_VLLM_PROBE_LABEL="\${probe_label}" PYTHONPATH="\${vllm_probe_pythonpath}:\${PYTHONPATH:-}" "\${selected_python}" - <<'PY'
import importlib.util
import os
import subprocess
import sys

print("TileXR vllm environment probe:", os.environ.get("TILEXR_VLLM_PROBE_LABEL", "unknown"))
print("VLLM_ASCEND_TILEXR_COLLECTIVES:", os.environ.get("VLLM_ASCEND_TILEXR_COLLECTIVES", "unset"))
for mod in ["vllm", "vllm_ascend"]:
    spec = importlib.util.find_spec(mod)
    if spec is None:
        print(f"{mod}: MISSING")
    else:
        locations = list(spec.submodule_search_locations or [])
        origin = spec.origin if spec.origin else ",".join(locations)
        print(f"{mod}: {origin}")
try:
    from tilexr_collectives.vllm_adapter import TileXRVllmCollectivesAdapter, enabled

    print("tilexr vllm adapter:", TileXRVllmCollectivesAdapter.__name__)
    print("tilexr vllm adapter enabled:", enabled())
except Exception as exc:
    print(f"tilexr vllm adapter probe: {type(exc).__name__}: {exc}")


def run_vllm_import_probe(module: str, *, disable_backend_autoload: bool = False) -> None:
    env = os.environ.copy()
    if disable_backend_autoload:
        env["TORCH_DEVICE_BACKEND_AUTOLOAD"] = "0"
    child = subprocess.run(
        [
            sys.executable,
            "-c",
            (
                "import importlib;"
                "module = importlib.import_module(%r);"
                "print(getattr(module, '__file__', None))"
            )
            % module,
        ],
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=30,
    )
    mode = "no_backend_autoload" if disable_backend_autoload else "default"
    print(f"vllm import probe {mode} {module}: rc={child.returncode}")
    if child.stdout:
        print(child.stdout.rstrip())
    if child.stderr:
        print(child.stderr.rstrip())


for module_name in [
    "vllm",
    "vllm_ascend",
    "vllm.distributed.device_communicators.base_device_communicator",
    "vllm_ascend.distributed.device_communicators.npu_communicator",
]:
    try:
        run_vllm_import_probe(module_name)
    except Exception as exc:
        print(f"vllm import probe default {module_name}: {type(exc).__name__}: {exc}")
    try:
        run_vllm_import_probe(module_name, disable_backend_autoload=True)
    except Exception as exc:
        print(f"vllm import probe no_backend_autoload {module_name}: {type(exc).__name__}: {exc}")
PY
}

probe_vllm_communicator_patch() {
  build_vllm_probe_pythonpath
  TILEXR_VLLM_TRACE=1 PYTHONPATH="\${vllm_probe_pythonpath}:\${PYTHONPATH:-}" "\${selected_python}" - <<'PY'
import os
from unittest.mock import patch

import tilexr_collectives.vllm_patch as vllm_patch
from vllm_ascend.distributed.device_communicators.npu_communicator import NPUCommunicator

calls = []


class FakeAdapter:
    def __init__(self, rank, world_size, install_prefix):
        calls.append(("init", rank, world_size, install_prefix))
        self.last_error = RuntimeError("forced fallback")

    def all_reduce(self, input_):
        calls.append(("all_reduce", input_))
        return ("tilexr-allreduce", input_)

    def all_gather(self, input_, dim=-1):
        calls.append(("all_gather", input_, dim))
        return None

    def broadcast(self, tensor, src=0):
        calls.append(("broadcast", tensor, src))
        return ("tilexr-broadcast", tensor, src)


def fallback_all_gather(self, input_, dim=-1):
    return ("fallback-allgather", input_, dim)


NPUCommunicator.all_gather = fallback_all_gather
os.environ["VLLM_ASCEND_TILEXR_COLLECTIVES"] = "1"
with patch.object(vllm_patch, "bind_group", lambda comm, prefix: FakeAdapter(comm.rank, comm.world_size, prefix)):
    patched = vllm_patch.patch_npu_communicator("install")
    communicator = NPUCommunicator.__new__(NPUCommunicator)
    communicator.rank = 0
    communicator.world_size = 2
    communicator._tilexr_collectives_adapter = vllm_patch.bind_group(communicator, "install")
    all_reduce_result = communicator.all_reduce("tensor-a")
    all_gather_result = communicator.all_gather("tensor-b", dim=0)
    broadcast_result = communicator.broadcast("tensor-c", src=1)
    counts = communicator._tilexr_collectives_route_counts

assert patched is True
assert all_reduce_result == ("tilexr-allreduce", "tensor-a")
assert all_gather_result == ("fallback-allgather", "tensor-b", 0)
assert broadcast_result == ("tilexr-broadcast", "tensor-c", 1)
assert counts["all_reduce"]["tilexr"] == 1
assert counts["all_gather"]["fallback"] == 1
assert counts["broadcast"]["tilexr"] == 1
print("PASS TileXR vllm NPUCommunicator patch probe")
print("TileXR vllm NPUCommunicator patch calls:", calls)
print("TileXR vllm NPUCommunicator route counts:", counts)
PY
}

probe_vllm_dummy_inference() {
  if [[ -z "\${remote_dummy_model}" ]]; then
    echo "Skipping TileXR vllm dummy inference probe; TILEXR_VLLM_REMOTE_DUMMY_MODEL is not set."
    return 0
  fi
  if [[ ! -d "\${remote_dummy_model}" ]]; then
    echo "ERROR: TILEXR_VLLM_REMOTE_DUMMY_MODEL does not exist: \${remote_dummy_model}" >&2
    return 2
  fi
  build_vllm_probe_pythonpath
  local probe_script
  probe_script="\$(mktemp "\${TMPDIR:-/tmp}/tilexr_vllm_dummy_inference.XXXXXX.py")"
  cat > "\${probe_script}" <<'PY'
from __future__ import annotations

import os

from vllm import LLM, SamplingParams


def main() -> None:
    model = os.environ["TILEXR_VLLM_REMOTE_DUMMY_MODEL"]
    llm = LLM(
        model=model,
        skip_tokenizer_init=True,
        load_format="dummy",
        tensor_parallel_size=2,
        max_model_len=64,
        gpu_memory_utilization=0.2,
        enforce_eager=True,
        trust_remote_code=False,
    )
    outputs = llm.generate(
        {"prompt_token_ids": [1, 2, 3]},
        SamplingParams(max_tokens=1, temperature=0.0, detokenize=False),
    )
    token_ids = [list(sample.token_ids) for sample in outputs[0].outputs]
    if not token_ids or not token_ids[0]:
        raise RuntimeError(f"empty dummy inference output: {token_ids}")
    print("PASS TileXR vllm dummy inference probe")
    print("TileXR vllm dummy inference token ids:", token_ids)


if __name__ == "__main__":
    main()
PY
  set +e
  VLLM_PLUGINS="\${remote_vllm_plugins}" \
    VLLM_ASCEND_TILEXR_COLLECTIVES=1 \
    TILEXR_VLLM_TRACE=1 \
    TILEXR_INSTALL_PREFIX="\${PWD}/install" \
    TILEXR_VLLM_REMOTE_DUMMY_MODEL="\${remote_dummy_model}" \
    PYTHONPATH="\${vllm_probe_pythonpath}:\${PYTHONPATH:-}" \
    "\${selected_python}" "\${probe_script}"
  local rc="$?"
  set -e
  rm -f "\${probe_script}"
  return "\${rc}"
}

run_selected_python_preflight() {
  "\${selected_python}" - <<'PY'
import sys

missing = []
for name in ["torch", "torch_npu"]:
    try:
        __import__(name)
    except Exception as exc:
        missing.append(f"{name}: {type(exc).__name__}: {exc}")
if missing:
    raise SystemExit("missing required Python packages: " + "; ".join(missing))

import torch
import torch_npu  # noqa: F401

print("torch:", getattr(torch, "__version__", "unknown"))
print("torch_npu:", getattr(torch_npu, "__version__", "unknown"))
if not torch.npu.is_available():
    raise SystemExit("torch.npu.is_available() is false")
device_count = torch.npu.device_count()
print("npu device_count:", device_count)
if device_count < 2:
    raise SystemExit(f"need at least 2 visible NPUs, got {device_count}")
torch.npu.set_device(0)
stream = torch.npu.current_stream()
stream_fields = {
    "npu_stream": getattr(stream, "npu_stream", None),
    "stream": getattr(stream, "stream", None),
}
print("current stream type:", type(stream))
print("current stream fields:", stream_fields)
if stream_fields["npu_stream"] is None and stream_fields["stream"] is None:
    raise SystemExit("torch.npu.current_stream() exposes neither npu_stream nor stream")
PY
}

{
  echo "Remote branch source: ${branch}"
  echo "Remote commit source: ${commit}"
  echo "Remote host: \$(hostname)"
  echo "Remote user: \$(whoami)"
  command -v npu-smi || true
  npu-smi info || true
  select_remote_python
  dump_selected_python_environment
  probe_vllm_environment "pre-cann"
  cmake --version 2>/dev/null | sed -n '1p' || true
  gcc --version 2>/dev/null | sed -n '1p' || true
  g++ --version 2>/dev/null | sed -n '1p' || true
  git submodule status --recursive || true
  if [[ -n "\${remote_ascend_home_path}" ]]; then
    export ASCEND_HOME_PATH="\${remote_ascend_home_path}"
    if [[ -f "\${ASCEND_HOME_PATH}/set_env.sh" ]]; then
      set +u
      source "\${ASCEND_HOME_PATH}/set_env.sh"
      set -u
    fi
  fi
  if [[ -n "\${remote_nnal_atb_set_env}" ]]; then
    if [[ ! -f "\${remote_nnal_atb_set_env}" ]]; then
      echo "ERROR: TILEXR_VLLM_REMOTE_NNAL_ATB_SET_ENV not found: \${remote_nnal_atb_set_env}" >&2
      exit 2
    fi
    set +u
    source "\${remote_nnal_atb_set_env}"
    set -u
  fi
  if [[ -n "\${remote_ascend_driver_path}" ]]; then
    export ASCEND_DRIVER_PATH="\${remote_ascend_driver_path}"
  fi
  if [[ -n "\${remote_cmake_cce_compiler}" ]]; then
    export CMAKE_CCE_COMPILER="\${remote_cmake_cce_compiler}"
  fi
  set +u
  source scripts/common_env.sh
  set -u
  probe_vllm_environment "post-cann"
  run_selected_python_preflight
  probe_vllm_communicator_patch
  cmake_args=(
    -DCMAKE_INSTALL_PREFIX=install
    -DTILEXR_BUILD_COLLECTIVES=ON
    -DTILEXR_BUILD_TESTS=ON
    -DBUILD_TESTING=ON
  )
  if [[ -n "\${remote_cmake_cce_compiler}" ]]; then
    cmake_args+=("-DCMAKE_CCE_COMPILER=\${remote_cmake_cce_compiler}")
  fi
  if [[ -n "\${remote_ascend_driver_path}" ]]; then
    cmake_args+=("-DASCEND_DRIVER_PATH=\${remote_ascend_driver_path}")
  fi
  cmake -S . -B build-vllm-collectives \
    "\${cmake_args[@]}"
  cmake --build build-vllm-collectives --target install test_tilexr_collectives_correctness tilexr_collective_perf -j"\$(nproc)"
  probe_vllm_dummy_inference
  ctest --test-dir build-vllm-collectives --output-on-failure
  (
    cd tests/collectives
    TILEXR_COLLECTIVES_RUN_TIMEOUT_SEC=600 ./run_collectives_correctness.sh 2 16 0 ../../build-vllm-collectives/tests/collectives allgather
  )
  (
    cd integrations/vllm_ascend
    VLLM_ASCEND_TILEXR_COLLECTIVES=0 TILEXR_VLLM_SMOKE_TIMEOUT_SEC=600 TILEXR_VLLM_SMOKE_PYTHON="\${selected_python}" TILEXR_VLLM_SMOKE_PYTHONPATH="\${remote_pythonpath}" ./run_tilexr_collectives_smoke.sh 2 16 0 ../../install allgather int32
    VLLM_ASCEND_TILEXR_COLLECTIVES=0 TILEXR_VLLM_SMOKE_TIMEOUT_SEC=600 TILEXR_VLLM_SMOKE_PYTHON="\${selected_python}" TILEXR_VLLM_SMOKE_PYTHONPATH="\${remote_pythonpath}" ./run_tilexr_collectives_smoke.sh 2 16 0 ../../install allgather fp16
    VLLM_ASCEND_TILEXR_COLLECTIVES=0 TILEXR_VLLM_SMOKE_TIMEOUT_SEC=600 TILEXR_VLLM_SMOKE_PYTHON="\${selected_python}" TILEXR_VLLM_SMOKE_PYTHONPATH="\${remote_pythonpath}" ./run_tilexr_collectives_smoke.sh 2 16 0 ../../install allreduce int32
    VLLM_ASCEND_TILEXR_COLLECTIVES=0 TILEXR_VLLM_SMOKE_TIMEOUT_SEC=600 TILEXR_VLLM_SMOKE_PYTHON="\${selected_python}" TILEXR_VLLM_SMOKE_PYTHONPATH="\${remote_pythonpath}" ./run_tilexr_collectives_smoke.sh 2 16 0 ../../install allreduce fp16
    VLLM_ASCEND_TILEXR_COLLECTIVES=0 TILEXR_VLLM_SMOKE_TIMEOUT_SEC=600 TILEXR_VLLM_SMOKE_PYTHON="\${selected_python}" TILEXR_VLLM_SMOKE_PYTHONPATH="\${remote_pythonpath}" ./run_tilexr_collectives_smoke.sh 2 16 0 ../../install reducescatter int32
    VLLM_ASCEND_TILEXR_COLLECTIVES=0 TILEXR_VLLM_SMOKE_TIMEOUT_SEC=600 TILEXR_VLLM_SMOKE_PYTHON="\${selected_python}" TILEXR_VLLM_SMOKE_PYTHONPATH="\${remote_pythonpath}" ./run_tilexr_collectives_smoke.sh 2 16 0 ../../install reducescatter fp16
    VLLM_ASCEND_TILEXR_COLLECTIVES=0 TILEXR_VLLM_SMOKE_TIMEOUT_SEC=600 TILEXR_VLLM_SMOKE_PYTHON="\${selected_python}" TILEXR_VLLM_SMOKE_PYTHONPATH="\${remote_pythonpath}" ./run_tilexr_collectives_smoke.sh 2 16 0 ../../install broadcast int32
    VLLM_ASCEND_TILEXR_COLLECTIVES=0 TILEXR_VLLM_SMOKE_TIMEOUT_SEC=600 TILEXR_VLLM_SMOKE_PYTHON="\${selected_python}" TILEXR_VLLM_SMOKE_PYTHONPATH="\${remote_pythonpath}" ./run_tilexr_collectives_smoke.sh 2 16 0 ../../install broadcast fp16
  )
} 2>&1 | tee $(printf '%q' "${REMOTE_LOG}")
EOF
)

set +e
ssh "${ssh_args[@]}" "bash -lc $(printf '%q' "${remote_script}")"
ssh_rc="$?"
set -e

echo "Remote validation log: ${REMOTE}:${REMOTE_LOG}"
exit "${ssh_rc}"
