#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 4 || $# -gt 5 ]]; then
    echo "usage: $0 <producer-host> <owner-host> <comm-id> <install-dir> [generations]" >&2
    exit 2
fi

producer_host=$1
owner_host=$2
comm_id=$3
install_dir=$(readlink -f "$4")
generations=${5:-8}
probe="${install_dir}/bin/tilexr_udma_peermem_probe"
mpi_bin=${MPI_HOME:+${MPI_HOME}/bin/mpirun}
mpi_bin=${mpi_bin:-$(command -v mpirun || true)}

if [[ ! -x "${probe}" ]]; then
    echo "probe executable not found: ${probe}" >&2
    exit 2
fi
if [[ -z "${mpi_bin}" || ! -x "${mpi_bin}" ]]; then
    echo "mpirun not found; set MPI_HOME" >&2
    exit 2
fi
if [[ ! "${generations}" =~ ^[0-9]+$ ]] || (( generations < 1 || generations > 16 )); then
    echo "generations must be in [1, 16]" >&2
    exit 2
fi

export TILEXR_COMM_ID="${comm_id}"
export TILEXR_ENABLE_IPC=1
export TILEXR_ENABLE_UDMA=1
export TILEXR_ENABLE_SDMA=0
export TILEXR_UDMA_USE_PEER_MEM=1
export TILEXR_UDMA_PEERMEM_GENERATIONS="${generations}"
tilexr_install=$(readlink -f "${install_dir}/../../../install")
export LD_LIBRARY_PATH="${install_dir}/lib64:${install_dir}/lib:${tilexr_install}/lib64:${tilexr_install}/lib:${LD_LIBRARY_PATH:-}"

if [[ "${producer_host}" != "${owner_host}" ]]; then
    echo "probe requires all three ranks on one host" >&2
    exit 2
fi
host_spec="${producer_host}:3"

exec timeout 180s "${mpi_bin}" \
    -hosts "${host_spec}" \
    -n 3 \
    -genv TILEXR_COMM_ID "${TILEXR_COMM_ID}" \
    -genv TILEXR_ENABLE_IPC "${TILEXR_ENABLE_IPC}" \
    -genv TILEXR_ENABLE_UDMA "${TILEXR_ENABLE_UDMA}" \
    -genv TILEXR_ENABLE_SDMA "${TILEXR_ENABLE_SDMA}" \
    -genv TILEXR_UDMA_USE_PEER_MEM "${TILEXR_UDMA_USE_PEER_MEM}" \
    -genv TILEXR_UDMA_PEERMEM_GENERATIONS "${TILEXR_UDMA_PEERMEM_GENERATIONS}" \
    -genv LD_LIBRARY_PATH "${LD_LIBRARY_PATH}" \
    "${probe}"
