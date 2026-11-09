/*
 * 实现 tilexr_elastic_ep.h 的紧凑 Dispatch 入口：校验 shape、dtype、rank 和 Memory 窗口，
 * 通过 Pool::Submit 在 service stream 注册并启动外置 tilexr_ep_dispatch_compact_kernel。
 * 启动闭包持有 CompactParams::keepAlive，并取得本轮通信 magic；输入及 communicator
 * 的生命周期仍由调用者保证。可选 profiling 回调只准备 capture，采集留到调用完成后。
 */
#include "tilexr_elastic_ep.h"
#include "ep_compact_layout.h"
#include "ep_host.h"
#include "ep_kernel_launch.h"
#include "tilexr_kernel_binary_loader.h"
#include "runtime/kernel.h"
#include <cstdlib>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace TileXR { namespace Elastic {
namespace {
uint8_t compactStub;
std::mutex registrationMutex;
void *binaryHandle = nullptr;

int RegisterCompact()
{
    std::lock_guard<std::mutex> lock(registrationMutex);
    if (binaryHandle != nullptr) {
        return TILEXR_SUCCESS;
    }
    KernelBinaryView binaryView;
    if (!LoadKernelBinary(&compactStub, "tilexr_ep_dispatch_compact_kernel.o", &binaryView)) {
        return TILEXR_ERROR_NOT_INITIALIZED;
    }
    rtDevBinary_t binary {};
    binary.magic = 0x41415246U;
    binary.data = binaryView.data;
    binary.length = binaryView.size;
    void *handle = nullptr;
    rtError_t rc = rtDevBinaryRegister(&binary, &handle);
    if (rc == RT_ERROR_NONE) {
        rc = rtFunctionRegister(handle, &compactStub, "tilexr_ep_dispatch_compact_kernel",
            "tilexr_ep_dispatch_compact_kernel", 0);
    }
    if (rc != RT_ERROR_NONE) {
        if (handle != nullptr) {
            (void)rtDevBinaryUnRegister(handle);
        }
        return TILEXR_ERROR_MKIRT;
    }
    binaryHandle = handle;
    return TILEXR_SUCCESS;
}
}

static std::shared_ptr<Pending> DispatchCompactImpl(Pool &pool, const CompactParams &params,
    aclrtStream inputStream, CompactProfilePrepare prepare)
{
    if (params.comm == nullptr || params.x == nullptr || params.expertIds == nullptr ||
        params.expertTokenNums == nullptr || params.sendCounts == nullptr || params.bs <= 0 ||
        params.h <= 0 || params.topK <= 0 || params.experts <= 0 ||
        uint64_t(params.bs) > UINT32_MAX || uint64_t(params.h) > UINT32_MAX ||
        uint64_t(params.topK) > UINT32_MAX || uint64_t(params.experts) > UINT32_MAX ||
        (params.dtype != TILEXR_DATA_TYPE_FP16 && params.dtype != TILEXR_DATA_TYPE_BFP16)) {
        throw std::invalid_argument("invalid compact EP input");
    }
    // Validation precedes submission; the caller keeps the communicator alive.
    TileXREp::EpDispatchParams legacy {};
    legacy.comm = params.comm;
    legacy.x = params.x;
    legacy.expertIds = params.expertIds;
    legacy.expertTokenNumsOut = params.expertTokenNums;
    legacy.epRecvCountsOut = params.sendCounts;
    legacy.bs = params.bs;
    legacy.h = params.h;
    legacy.topK = params.topK;
    legacy.moeExpertNum = params.experts;
    legacy.dtype = params.dtype;
    legacy.expandXOutDtype = params.dtype;
    legacy.expertTokenNumsType = 1;
    TileXREp::EpHostLaunchContext context {};
    if (TileXREp::TileXREpPrepareMemoryLaunchContext(legacy, &context) != TILEXR_SUCCESS ||
        !TileXREp::TileXREpIsA5Soc()) {
        throw std::runtime_error("compact EP requires an initialized A5 memory communicator");
    }
    if (context.hostArgs->rankSize <= 0 ||
        uint64_t(params.bs) > uint64_t(INT32_MAX) /
            (uint64_t(context.hostArgs->rankSize) * uint64_t(params.topK))) {
        throw std::invalid_argument("compact EP receive prefixes exceed int32 range");
    }
    int32_t device = 0;
    int64_t vectorCores = 0;
    uint32_t blocks = 0;
    if (aclrtGetDevice(&device) != ACL_SUCCESS ||
        aclrtGetDeviceInfo(static_cast<uint32_t>(device), ACL_DEV_ATTR_VECTOR_CORE_NUM, &vectorCores) != ACL_SUCCESS ||
        vectorCores < 2 || vectorCores > std::numeric_limits<uint32_t>::max() ||
        TileXREp::TileXREpResolveMemoryBlockDim(static_cast<uint32_t>(vectorCores), 2,
            std::getenv("TILEXR_EP_DISPATCH_MEMORY_BLOCK_DIM"), &blocks) != TILEXR_SUCCESS) {
        throw std::runtime_error("compact EP cannot resolve vector core count");
    }
    TileXREp::EpMemoryDispatchReferenceConfig layout {};
    if (TileXREp::TileXREpValidateDispatchMemoryConfig(legacy, *context.hostArgs, blocks, &layout) != TILEXR_SUCCESS) {
        throw std::invalid_argument("unsupported compact EP shape or memory window");
    }
    const uint64_t argsAddress = reinterpret_cast<uint64_t>(context.devArgs);
    return pool.Submit([params, argsAddress, blocks, prepare](DeviceView view, aclrtStream stream) {
        int rc = RegisterCompact();
        if (rc != TILEXR_SUCCESS) {
            return rc;
        }
        int64_t magic = 0;
        rc = TileXRCommNextMagic(params.comm, &magic);
        if (rc != TILEXR_SUCCESS) {
            return rc;
        }
        const uint64_t profilingGM = prepare ? prepare(stream, blocks) : 0;
        TileXREp::CompactKernelArgs args {argsAddress, reinterpret_cast<uint64_t>(params.x),
            reinterpret_cast<uint64_t>(params.expertIds), reinterpret_cast<uint64_t>(params.expertTokenNums),
            reinterpret_cast<uint64_t>(params.sendCounts), view.state, view.mailbox, params.bs, params.h,
            params.topK, params.experts, static_cast<int64_t>(params.dtype), magic, profilingGM};
        rtArgsEx_t runtimeArgs {};
        runtimeArgs.args = &args;
        runtimeArgs.argsSize = sizeof(args);
        rtTaskCfgInfo_t config {};
        return rtKernelLaunchWithFlagV2(&compactStub, blocks, &runtimeArgs, nullptr,
            static_cast<rtStream_t>(stream), 0, &config) == RT_ERROR_NONE ? TILEXR_SUCCESS : TILEXR_ERROR_MKIRT;
    }, inputStream);
}
std::shared_ptr<Pending> DispatchCompact(Pool &pool, const CompactParams &params, aclrtStream inputStream)
{
    return DispatchCompactImpl(pool, params, inputStream, {});
}
std::shared_ptr<Pending> DispatchCompactProfiled(Pool &pool, const CompactParams &params,
    aclrtStream inputStream, CompactProfilePrepare prepare)
{
#if defined(TILEXR_ELASTIC_PROFILE_ENABLED)
    if (!prepare) throw std::invalid_argument("profiling requires an SDK capture preparer");
    return DispatchCompactImpl(pool, params, inputStream, std::move(prepare));
#else
    (void)pool; (void)params; (void)inputStream; (void)prepare;
    throw std::runtime_error("compact profiling is disabled at build time");
#endif
}
} }
