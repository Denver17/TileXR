#!/usr/bin/env bash
set -euo pipefail

lib="${1:-install/lib/libtile-comm.so}"

if [ ! -f "${lib}" ]; then
    echo "ERROR: ${lib} not found" >&2
    exit 1
fi

if command -v readelf >/dev/null 2>&1; then
    needed=$(readelf -d "${lib}" 2>/dev/null | grep -E 'NEEDED' || true)
else
    needed=""
fi

deps=$(ldd "${lib}" 2>/dev/null || true)
forbidden='libhcomm\.so|libhccl_v2\.so|libhccl_fwk\.so|libmc2_client\.so|HcclCcuKernel|HcclGetCcuTaskInfo|HcomGetCcuTaskInfo|HcclChannelAcquire|HcclGetChannelForCcu|HcclAllocAlgResourceCcu|HcommChannelNotify|HcommChannelFence|rtGetNotifyAddress|HrtCcuLaunch|HrtGetDevResAddress|HrtReleaseDevResAddress|HrtNotifyGetAddr|HrtRaCustomChannel|HrtCntNotify|CcuResBatchAllocator|CcuResRepository|CcuDeviceManager|CcuDevMgrImp|CcuRepContext|CcuKernelMgr|CtxMgrImp|CcuInstrInfo|CcuTaskParam|CcuTaskArg|GeneTaskParam|GetMissionKey|SetMissionId|SetMissionKey|SetInstrId|SetCcuInstrInfo|LoadInstruction|AllocIns|AllocCke|AllocXn|COMM_ENGINE_CCU|COMM_PROTOCOL_UBC_CTP|HCCL_SERVER_TYPE_CCU|RT_RES_TYPE_CCU_CKE|RT_RES_TYPE_CCU_XN'

printf '%s\n' "${needed}"
printf '%s\n' "${deps}"

if printf '%s\n%s\n' "${needed}" "${deps}" | grep -E "${forbidden}" >/dev/null; then
    echo "ERROR: libtile-comm.so links an hcomm/HCCL CCU reference library" >&2
    exit 1
fi

if command -v nm >/dev/null 2>&1; then
    symbol_hits=$(nm -D "${lib}" 2>/dev/null | c++filt | grep -E "${forbidden}" || true)
else
    symbol_hits=""
fi
if [ -n "${symbol_hits}" ]; then
    printf '%s\n' "${symbol_hits}" >&2
    echo "ERROR: libtile-comm.so exports or imports private hcomm/HCCL CCU symbols" >&2
    exit 1
fi

if command -v strings >/dev/null 2>&1; then
    string_hits=$(strings -a "${lib}" 2>/dev/null | grep -E "${forbidden}" || true)
else
    string_hits=""
fi
if [ -n "${string_hits}" ]; then
    printf '%s\n' "${string_hits}" >&2
    echo "ERROR: libtile-comm.so contains private hcomm/HCCL CCU references" >&2
    exit 1
fi

echo "TileXR CCU dependency guard passed: no hcomm/HCCL private CCU dependency or symbol reference"
