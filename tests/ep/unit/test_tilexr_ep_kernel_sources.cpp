#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#ifdef TILEXR_SOURCE_ROOT
const char *kSourceRoot = TILEXR_SOURCE_ROOT;
#else
const char *kSourceRoot = ".";
#endif

std::string JoinPath(const std::string &base, const std::string &path)
{
    if (base.empty() || base[base.size() - 1] == '/') {
        return base + path;
    }
    return base + "/" + path;
}

bool ReadFile(const std::string &relativePath, std::string *contents)
{
    const std::string fullPath = JoinPath(kSourceRoot, relativePath);
    std::ifstream stream(fullPath.c_str());
    if (!stream.is_open()) {
        std::cerr << "missing file: " << relativePath << std::endl;
        ++g_failures;
        return false;
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    *contents = buffer.str();
    return true;
}

void CheckContains(const std::string &label, const std::string &contents, const std::string &needle)
{
    if (contents.find(needle) == std::string::npos) {
        std::cerr << label << " missing: " << needle << std::endl;
        ++g_failures;
    }
}

void CheckNotContains(const std::string &label, const std::string &contents, const std::string &needle)
{
    if (contents.find(needle) != std::string::npos) {
        std::cerr << label << " contains forbidden string: " << needle << std::endl;
        ++g_failures;
    }
}

void CheckOccurrenceCount(const std::string &label, const std::string &contents,
    const std::string &needle, std::size_t expected)
{
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = contents.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    if (count != expected) {
        std::cerr << label << " expected " << expected << " occurrences of " << needle
                  << ", got " << count << std::endl;
        ++g_failures;
    }
}

void TestKernelUsesTileXRPeerMemory()
{
    const std::string path = "src/ep/kernels/tilexr_ep_dispatch_kernel.cpp";
    std::string contents;
    if (!ReadFile(path, &contents)) {
        return;
    }

    CheckContains(path, contents, "extern \"C\" __global__ __aicore__ void tilexr_ep_dispatch_kernel");
    CheckContains(path, contents, "launch_tilexr_ep_dispatch_kernel");
    CheckContains(path, contents, "CommArgs");
    CheckContains(path, contents, "peerMems");
    CheckContains(path, contents, "GlobalTensor<GM_ADDR> peerMems");
    CheckContains(path, contents, "peerMems.GetValue(peer)");
    CheckContains(path, contents, "IPC_DATA_OFFSET");
    CheckContains(path, contents, "SyncCollectives");
    CheckContains(path, contents, "DataCopyPad");
    CheckContains(path, contents, "kEpStepWindowCleared");
    CheckContains(path, contents, "kEpStepDispatchReady");
    CheckContains(path, contents, "kEpStepDispatchDrained");
    CheckContains(path, contents, "LoadInt32FromGm");
    CheckContains(path, contents, "LoadAssistTupleFromGm");
    CheckContains(path, contents, "StoreWindowHeader");
    CheckContains(path, contents, "StoreSlotHeader");
    CheckContains(path, contents, "StoreAssistTuple");
    CheckContains(path, contents, "TileXREpFlushDispatchSlotHeaders");
    CheckContains(path, contents, "sourceWindow = TileXREpWindowBase");
    CheckNotContains(path, contents, "expertIds[");
    CheckNotContains(path, contents, "assistBase[item]");
    CheckNotContains(path, contents, "args->peerMems[peer]");
    CheckNotContains(path, contents, "slot->count");
    CheckNotContains(path, contents, "assist[index]");
}

void TestCrossNodeDispatchUsesUDMARegistry()
{
    const std::string path = "src/ep/kernels/tilexr_ep_dispatch_kernel.cpp";
    std::string contents;
    if (!ReadFile(path, &contents)) {
        return;
    }

    CheckContains(path, contents, "tilexr_udma.h");
    CheckContains(path, contents, "TileXR::UDMARegistryEnabled(args)");
    CheckContains(path, contents, "TileXREpUsesUdmaWindow");
    CheckContains(path, contents, "TileXR::UDMAPutNbi<uint8_t>");
    CheckContains(path, contents, "TileXR::UDMAQuiet(args, dstRank)");
    CheckContains(path, contents, "TileXREpFlushDispatchSlotHeaders");
    CheckContains(path, contents, "sourceWindow = TileXREpWindowBase");
}

void TestCrossNodeDispatchPullsRemoteSlots()
{
    const std::string path = "src/ep/kernels/tilexr_ep_dispatch_kernel.cpp";
    std::string contents;
    if (!ReadFile(path, &contents)) {
        return;
    }

    CheckContains(path, contents, "tilexr_ep_dispatch_cross_node_kernel");
    CheckContains(path, contents, "launch_tilexr_ep_dispatch_cross_node_kernel");
    CheckContains(path, contents, "TileXREpPullUdmaSlots");
    CheckContains(path, contents, "TileXR::UDMAGetNbi<uint8_t>");
    CheckContains(path, contents, "TileXREpNotifyUdmaReady");
    CheckContains(path, contents, "TileXREpWaitUdmaReady");
    CheckContains(path, contents, "TileXREpNotifyAllUdmaReady");
    CheckContains(path, contents, "TileXREpWaitAllUdmaReady");
    CheckContains(path, contents, "TileXR::UDMAPutSignalNbi<uint8_t>");
}

void TestCrossNodeDispatchSeparatesLocalAndRemotePeers()
{
    const std::string path = "src/ep/kernels/tilexr_ep_dispatch_kernel.cpp";
    std::string contents;
    if (!ReadFile(path, &contents)) {
        return;
    }

    CheckContains(path, contents, "TileXREpIsSameNodePeer");
    CheckContains(path, contents, "TileXREpUsesUdmaPeer");
    CheckContains(path, contents, "if (localRankSize <= 1)");
    CheckContains(path, contents, "TileXREpPublishLocalUdmaSlot");
    CheckContains(path, contents, "TileXREpDispatchWriteWindow");
    CheckContains(path, contents, "args->localRankSize");
    CheckContains(path, contents, "if (localRankSize > 1)");
    CheckContains(path, contents, "dstRank != rank && TileXREpIsSameNodePeer(rank, dstRank, localRankSize)");
    CheckContains(path, contents, "srcRank != rank &&");
    CheckContains(path, contents, "TileXREpIsSameNodePeer(rank, srcRank, localRankSize)");
    CheckContains(path, contents, "sameNodeSource ? rank : srcRank");
    CheckContains(path, contents, "!TileXREpIsSameNodePeer(rank, peer, localRankSize)");
}

void TestHostDispatchSplitsCrossNodeKernel()
{
    const std::string path = "src/ep/host/ep_kernel_launch.cpp";
    std::string contents;
    if (!ReadFile(path, &contents)) {
        return;
    }

    CheckContains(path, contents, "launch_tilexr_ep_dispatch_cross_node_kernel");
    CheckContains(path, contents, "TileXREpUsesCrossNodeKernel");
}

void TestDispatchHelpersLiveInDispatchHelperFile()
{
    const std::string path = "src/ep/kernels/tilexr_ep_dispatch_helpers.h";
    std::string contents;
    if (!ReadFile(path, &contents)) {
        return;
    }

    CheckContains(path, contents, "TileXREpRouteToDstRank");
    CheckContains(path, contents, "TileXREpCopyRoutePayload");
    CheckContains(path, contents, "TileXREpStoreDispatchSlotHeader");
    CheckContains(path, contents, "TileXREpStoreAssistTuple");
    CheckContains(path, contents, "localWindow + PayloadOffset(dstRank, slotBytes)");
}

void TestCombineHelpersLiveInCombineHelperFile()
{
    const std::string path = "src/ep/kernels/tilexr_ep_combine_helpers.h";
    std::string contents;
    if (!ReadFile(path, &contents)) {
        return;
    }

    CheckContains(path, contents, "TileXREpDrainSourceWindow");
    CheckContains(path, contents, "TileXREpWaitDispatchSlotReady");
    CheckContains(path, contents, "slotMagic");
    CheckContains(path, contents, "TileXREpLoadAssistTuple");
    CheckContains(path, contents, "TileXREpGetCombineTokenId");
    CheckContains(path, contents, "TileXREpGetCombineTopKId");
    CheckContains(path, contents, "sourceWindow + SlotOffset(slotRank, slotBytes)");
}

void TestCombineKernelUsesTileXRPeerMemory()
{
    const std::string path = "src/ep/kernels/tilexr_ep_combine_kernel.cpp";
    std::string contents;
    if (!ReadFile(path, &contents)) {
        return;
    }

    CheckContains(path, contents, "extern \"C\" __global__ __aicore__ void tilexr_ep_combine_kernel");
    CheckContains(path, contents, "launch_tilexr_ep_combine_kernel");
    CheckContains(path, contents, "kEpStepCombineWindowCleared");
    CheckContains(path, contents, "kEpStepCombineReady");
    CheckContains(path, contents, "ScatterCombineRows");
    CheckContains(path, contents, "DrainCombineRows");
    CheckContains(path, contents, "AccumulateRow");
    CheckContains(path, contents, "tilexr_ep_combine_cross_node_kernel");
    CheckContains(path, contents, "tilexr_ep_combine_cross_node_drain_kernel");
    CheckContains(path, contents, "TileXREpNotifyRemoteUdmaReadySeparate");
    CheckContains(path, contents, "TileXREpWaitRemoteUdmaReady");
    CheckNotContains(path, contents, "tilexr_ep_dispatch_kernel");

    std::string hostLaunch;
    if (ReadFile("src/ep/host/ep_kernel_launch.cpp", &hostLaunch)) {
        CheckContains("src/ep/host/ep_kernel_launch.cpp", hostLaunch, "TileXREpLaunchCombineKernel");
        CheckContains("src/ep/host/ep_kernel_launch.cpp", hostLaunch, "launch_tilexr_ep_combine_kernel");
        CheckContains("src/ep/host/ep_kernel_launch.cpp", hostLaunch,
            "launch_tilexr_ep_combine_cross_node_kernel");
        CheckContains("src/ep/host/ep_kernel_launch.cpp", hostLaunch, "aclrtSynchronizeStream");
        CheckContains("src/ep/host/ep_kernel_launch.cpp", hostLaunch, "TILEXR_ERROR_TIMEOUT");
    }
}

void TestKernelCommonHasCombineHelpers()
{
    const std::string path = "src/ep/kernels/tilexr_ep_kernel_common.h";
    std::string contents;
    if (!ReadFile(path, &contents)) {
        return;
    }

    CheckContains(path, contents, "UDMASecondOperationOffset");
    CheckContains(path, contents, "TileXREpNotifyRemoteUdmaReadySeparate");
    CheckContains(path, contents, "TileXREpWaitRemoteUdmaReady");
    CheckContains(path, contents, "TileXREpStoreStatusValue");
    CheckContains(path, contents, "TileXREpFlushUdmaSourceWindow");
    CheckContains(path, contents, "IsValidShape");

    std::string combine;
    if (ReadFile("src/ep/kernels/tilexr_ep_combine_kernel.cpp", &combine)) {
        CheckContains("src/ep/kernels/tilexr_ep_combine_kernel.cpp", combine, "kEpStatusRemoteReadyTimeout");
    }
}

void TestDispatchDemoRunsCombine()
{
    const std::string path = "tests/ep/demo/tilexr_ep_dispatch_demo.cpp";
    std::string contents;
    if (!ReadFile(path, &contents)) {
        return;
    }

    CheckContains(path, contents, "TileXRMoeEpCombine");
    CheckContains(path, contents, "ValidateCombineOutputs");
    CheckContains(path, contents, "combine validation");
}

void TestKernelForwardsActiveMask()
{
    std::string hostLaunch;
    if (ReadFile("src/ep/host/ep_kernel_launch.cpp", &hostLaunch)) {
        CheckContains("src/ep/host/ep_kernel_launch.cpp", hostLaunch, "xActiveMask");
    }

    std::string kernel;
    if (ReadFile("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", &kernel)) {
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "xActiveMaskGM");
    }

    std::string dispatchHelpers;
    if (ReadFile("src/ep/kernels/tilexr_ep_dispatch_helpers.h", &dispatchHelpers)) {
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_helpers.h", dispatchHelpers, "TileXREpIsTokenActive");
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_helpers.h", dispatchHelpers, "xActiveMaskGM == nullptr");
    }
}

void TestKernelForwardsExpertTokenNumsType()
{
    std::string hostLaunch;
    if (ReadFile("src/ep/host/ep_kernel_launch.cpp", &hostLaunch)) {
        CheckContains("src/ep/host/ep_kernel_launch.cpp", hostLaunch, "expertTokenNumsType");
    }

    std::string kernel;
    if (ReadFile("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", &kernel)) {
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "expertTokenNumsType");
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "TileXREpClearExpertTokenNums");
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "TileXREpFinalizeExpertTokenNums");
    }

    std::string combineHelpers;
    if (ReadFile("src/ep/kernels/tilexr_ep_combine_helpers.h", &combineHelpers)) {
        CheckContains("src/ep/kernels/tilexr_ep_combine_helpers.h", combineHelpers,
            "TileXREpIncrementExpertTokenNum");
        CheckContains("src/ep/kernels/tilexr_ep_combine_helpers.h", combineHelpers,
            "running += expertTokenNumsOut[localExpert]");
    }
}

void TestKernelForwardsTpRecvCountsOut()
{
    std::string hostLaunch;
    if (ReadFile("src/ep/host/ep_kernel_launch.cpp", &hostLaunch)) {
        CheckContains("src/ep/host/ep_kernel_launch.cpp", hostLaunch, "tpRecvCountsOut");
        CheckContains("src/ep/host/ep_kernel_launch.cpp", hostLaunch, "tpWorldSize");
        CheckContains("src/ep/host/ep_kernel_launch.cpp", hostLaunch, "tpRankId");
    }

    std::string kernel;
    if (ReadFile("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", &kernel)) {
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "tpRecvCountsOutGM");
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "TileXREpAppendTpGroupRows");
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "TileXREpTpGroupStartRank");
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "tpWorldSize");
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "tpRankId");
    }

    const std::string demoPath = "tests/ep/demo/tilexr_ep_dispatch_demo.cpp";
    std::string demo;
    if (ReadFile(demoPath, &demo)) {
        CheckContains(demoPath, demo, "TILEXR_EP_DEMO_TP_RECV_COUNTS");
        CheckContains(demoPath, demo, "TILEXR_EP_DEMO_TP_WORLD_SIZE");
        CheckContains(demoPath, demo, "TILEXR_EP_DEMO_TP_RANK_ID");
        CheckContains(demoPath, demo, "BuildExpectedTpRoutes");
        CheckContains(demoPath, demo, "ValidateTpRecvCounts");
    }
}

void TestDispatchDemoExercisesOptionalInputs()
{
    const std::string path = "tests/ep/demo/tilexr_ep_dispatch_demo.cpp";
    std::string contents;
    if (!ReadFile(path, &contents)) {
        return;
    }

    CheckContains(path, contents, "TILEXR_EP_DEMO_ACTIVE_MASK");
    CheckContains(path, contents, "TILEXR_EP_DEMO_EXPERT_TOKEN_NUMS_TYPE");
    CheckContains(path, contents, "xActiveMaskDev");
    CheckContains(path, contents, "expertTokenNumsType");
}

void TestKernelForwardsSharedExpertConfig()
{
    std::string hostLaunch;
    if (ReadFile("src/ep/host/ep_kernel_launch.cpp", &hostLaunch)) {
        CheckContains("src/ep/host/ep_kernel_launch.cpp", hostLaunch, "sharedExpertNum");
        CheckContains("src/ep/host/ep_kernel_launch.cpp", hostLaunch, "sharedExpertRankNum");
    }

    std::string kernel;
    if (ReadFile("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", &kernel)) {
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "TileXREpRouteToDstRank");
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "sharedExpertRankNum");
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "TileXREpLocalExpertCount");
    }

    std::string demo;
    if (ReadFile("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", &demo)) {
        CheckContains("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", demo, "TILEXR_EP_DEMO_SHARED_EXPERT_NUM");
        CheckContains("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", demo, "TILEXR_EP_DEMO_SHARED_EXPERT_RANK_NUM");
    }
}

void TestKernelForwardsStaticQuantConfig()
{
    std::string hostLaunch;
    if (ReadFile("src/ep/host/ep_kernel_launch.cpp", &hostLaunch)) {
        CheckContains("src/ep/host/ep_kernel_launch.cpp", hostLaunch, "scales");
        CheckContains("src/ep/host/ep_kernel_launch.cpp", hostLaunch, "quantMode");
    }

    std::string kernel;
    if (ReadFile("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", &kernel)) {
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "scalesGM");
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "quantMode");
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "TileXREpCopyStaticQuantRoutePayload");
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "TileXREpClampInt8");
    }

    std::string demo;
    if (ReadFile("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", &demo)) {
        CheckContains("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", demo, "TILEXR_EP_DEMO_QUANT_MODE");
        CheckContains("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", demo, "TILEXR_EP_DEMO_STATIC_QUANT_SCALE");
    }
}

void TestKernelForwardsPerTokenDynamicQuantConfig()
{
    std::string kernel;
    if (ReadFile("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", &kernel)) {
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "kEpQuantModePerTokenDynamic");
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel,
            "TileXREpCopyPerTokenDynamicQuantRoutePayload");
        CheckContains("src/ep/kernels/tilexr_ep_dispatch_kernel.cpp", kernel, "dynamicScalesOutGM");
    }

    std::string demo;
    if (ReadFile("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", &demo)) {
        CheckContains("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", demo, "usePerTokenDynamicQuant");
        CheckContains("tests/ep/demo/tilexr_ep_dispatch_demo.cpp", demo, "DynamicScaleForXValue");
    }
}

void TestClearLocalWindowDoesNotPreclearSlotHeaders()
{
    const std::string path = "src/ep/kernels/tilexr_ep_dispatch_kernel.cpp";
    std::string contents;
    if (!ReadFile(path, &contents)) {
        return;
    }

    CheckNotContains(path, contents,
        "for (int32_t srcRank = 0; srcRank < rankSize; ++srcRank) {\n"
        "        StoreSlotHeader(localWindow + SlotOffset(srcRank, slotBytes), 0, srcRank, 0, 0, tBuf);\n"
        "    }");
}

void TestMemoryKernelsUseCommunicatorRounds()
{
    const std::string windowPath = "src/ep/common/ep_memory_window.h";
    std::string window;
    if (ReadFile(windowPath, &window)) {
        CheckContains(windowPath, window, "kEpMemoryRoundStrideBytes");
        CheckContains(windowPath, window, "kEpMemoryRequiredPeerBytes");
        CheckContains(windowPath, window, "TileXREpMemoryRoundDataBase");
    }

    const std::string path = "src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h";
    std::string contents;
    if (ReadFile(path, &contents)) {
        CheckContains(path, contents, "ctx_.Init(commArgs, magic)");
        CheckNotContains(path, contents, "EpMemoryRoundSync");
        CheckNotContains(path, contents, "roundSync_");
        CheckContains(path, contents,
            "TileXREp::TileXREpMemoryRoundDataBase(args_->peerMems[rankId], magic_)");
        CheckContains(path, contents, "DataCopy(flagRecvTensor_, dataFlagGlobal, expFlagCopyParams)");
        CheckContains(path, contents,
            "ReduceSum(flagSumOutTensor, flagRecvTensor_, flagReduceWorkTensor_, 1U, flagNum, 1U)");
        CheckContains(path, contents, "TileXREp::kEpMemoryAssistInfoWidth");
        CheckNotContains(path, contents, "EXPAND_IDX_INFO");
        CheckContains(path, contents, "class MoeDispatchMemory");
        CheckNotContains(path, contents,
            "extern \"C\" __global__ __aicore__ void tilexr_ep_dispatch_memory_kernel");
    }

    const std::string combinePath = "src/ep/kernels/tilexr_ep_combine_memory_kernel.h";
    std::string combine;
    if (ReadFile(combinePath, &combine)) {
        CheckContains(combinePath, combine, "context_.Init(commArgsGM, magic)");
        CheckNotContains(combinePath, combine, "EpMemoryRoundSync");
        CheckNotContains(combinePath, combine, "roundSync_");
        CheckContains(combinePath, combine,
            "TileXREp::TileXREpMemoryRoundDataBase(args_->peerMems[rank], magic_)");
        CheckNotContains(combinePath, combine, "halfWinSize_");
        CheckNotContains(combinePath, combine, "dataState_ * halfWinSize_");
    }

    const std::string entryPath = "src/ep/kernels/tilexr_ep_dispatch_memory_kernel.cpp";
    std::string entry;
    if (ReadFile(entryPath, &entry)) {
        CheckContains(entryPath, entry, "#include \"tilexr_ep_dispatch_memory_kernel.h\"");
        CheckContains(entryPath, entry,
            "extern \"C\" __global__ __aicore__ void tilexr_ep_dispatch_memory_kernel");
        CheckNotContains(entryPath, entry, "class MoeDispatchMemory");
    }
}

void TestMemoryKernelUbStagingCoversPackedPayloads()
{
    const std::string dispatchPath = "src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h";
    std::string dispatch;
    if (ReadFile(dispatchPath, &dispatch)) {
        CheckContains(dispatchPath, dispatch,
            "packedPayloadBytes_ = blockCntPerToken_ * SPLIT_BLOCK_DATA_SIZE");
        CheckContains(dispatchPath, dispatch,
            "inputTensorBytes_ = IsMxfp8 ? hAlignSize_ : packedPayloadBytes_");
        CheckContains(dispatchPath, dispatch, "quantTensorBytes_ = packedPayloadBytes_");
        CheckContains(dispatchPath, dispatch,
            "tbufPool0.InitBuffer(inQueue, BUFFER_NUM, inputTensorBytes_)");
        CheckContains(dispatchPath, dispatch,
            "validBsIndexTensor_.GetValue(activeTokenIndex)");
        CheckContains(dispatchPath, dispatch,
            "LocalTensor<int32_t> validBsIndexTensor_");
        CheckNotContains(dispatchPath, dispatch,
            "LocalTensor<uint32_t> validBsIndexTensor_");
        CheckContains(dispatchPath, dispatch,
            "DataCopyPad(validExpertIdsTensor_[activeTokenIndex * axisK_]");
        CheckContains(dispatchPath, dispatch,
            "sendTokenBufNum_ = static_cast<uint8_t>(sendTokenBufNum)");
    }

    const std::string combinePath = "src/ep/kernels/tilexr_ep_combine_memory_kernel.h";
    std::string combine;
    if (ReadFile(combinePath, &combine)) {
        CheckContains(combinePath, combine, "indexCountsBuf_");
        CheckContains(combinePath, combine, "CalcDispatchBufferNum");
        CheckContains(combinePath, combine, "MAX_DISPATCH_BUFFER_NUM");
        CheckContains(combinePath, combine, "expandXInQueue_");
        CheckContains(combinePath, combine, "xOutPackageQueue_");
        CheckContains(combinePath, combine, "moeMainSumQueue_, bufferNum_");
        CheckContains(combinePath, combine, "CheckPackedFlagRangeArrive");
        CheckContains(combinePath, combine, "ExpertScaleCopy");
        CheckContains(combinePath, combine, "tpipe_->InitBuffer(indexCountsBuf_, UB_ALIGN)");
        CheckContains(combinePath, combine,
            "const uint32_t inputBufferBytes = IsMxfp8 ? quantInputBytes_ : moeQueueBytes_");
        CheckContains(combinePath, combine,
            "toRankId < epWorldSize_ && tokenId < axisBS_ && topkId < slotCount_");
    }
}

void TestMemoryCombineUsesRegisteredBinaryLaunch()
{
    const std::string entryPath = "src/ep/kernels/tilexr_ep_combine_memory_kernel.cpp";
    std::string entry;
    if (ReadFile(entryPath, &entry)) {
        CheckContains(entryPath, entry, "#include \"tilexr_ep_combine_memory_kernel.h\"");
        CheckContains(entryPath, entry,
            "extern \"C\" __global__ __aicore__ void tilexr_ep_combine_memory_kernel");
        CheckContains(entryPath, entry,
            "activeMaskType == Mc2Kernel::ACTIVE_MASK_TOKEN && xActiveMaskGM == nullptr");
        CheckNotContains(entryPath, entry, "class MoeCombineMemory");
        CheckNotContains(entryPath, entry, "<<<blockDim, nullptr, stream>>>");
        CheckNotContains(entryPath, entry, "launch_tilexr_ep_combine_memory_kernel");
    }

    const std::string algorithmPath = "src/ep/kernels/tilexr_ep_combine_memory_kernel.h";
    std::string algorithm;
    if (ReadFile(algorithmPath, &algorithm)) {
        CheckContains(algorithmPath, algorithm, "class MoeCombineMemory");
        CheckNotContains(algorithmPath, algorithm, "EpMemoryRoundSync");
        CheckNotContains(algorithmPath, algorithm, "roundSync_");
        CheckNotContains(algorithmPath, algorithm, "halfWinSize_");
        CheckNotContains(algorithmPath, algorithm, "dataState_");
        CheckNotContains(algorithmPath, algorithm, "SendOneRow");
        CheckContains(algorithmPath, algorithm, "TokenActive");
        CheckContains(algorithmPath, algorithm,
            "xActiveMaskGM_.SetGlobalBuffer(reinterpret_cast<__gm__ bool *>(xActiveMaskGM))");
        CheckContains(algorithmPath, algorithm,
            "const bool tokenActive = TokenActive(tokenIndex)");
        CheckNotContains(algorithmPath, algorithm,
            "extern \"C\" __global__ __aicore__ void tilexr_ep_combine_memory_kernel");
    }

    const std::string hostPath = "src/ep/host/ep_kernel_launch.cpp";
    std::string host;
    if (ReadFile(hostPath, &host)) {
        CheckContains(hostPath, host, "struct CombineMemoryKernelArgs");
        CheckContains(hostPath, host, "TileXREpCombineMemoryKernelBinaryData");
        CheckContains(hostPath, host, "EnsureCombineMemoryKernelRegistered");
        CheckContains(hostPath, host,
            "rtKernelLaunchWithFlagV2(&gCombineMemoryKernelStub");
        CheckContains(hostPath, host, "TileXRCommNextMagic(params.comm, &magic)");
        CheckNotContains(hostPath, host, "extern void launch_tilexr_ep_combine_memory_kernel");
    }

    const std::string cmakePath = "src/ep/CMakeLists.txt";
    std::string cmake;
    if (ReadFile(cmakePath, &cmake)) {
        CheckContains(cmakePath, cmake, "tilexr_ep_combine_memory_kernel_tmp OBJECT");
        CheckContains(cmakePath, cmake, "kernels/tilexr_ep_dispatch_memory_kernel.h");
        CheckNotContains(cmakePath, cmake, "tilexr_ep_memory_round_sync.h");
        CheckContains(cmakePath, cmake, "kernels/tilexr_ep_combine_memory_kernel.h");
        CheckContains(cmakePath, cmake, "TILEXR_EP_COMBINE_MEMORY_EMBED_CPP");
        CheckContains(cmakePath, cmake, "embed_combine_memory_kernel.cmake");
        CheckOccurrenceCount(cmakePath, cmake, "--cce-auto-sync", 2U);
        CheckNotContains(cmakePath, cmake, "libtilexr_ep_combine_memory_kernel.so");
    }

    const std::string embedPath = "src/ep/cmake/embed_combine_memory_kernel.cmake";
    std::string embed;
    if (ReadFile(embedPath, &embed)) {
        CheckContains(embedPath, embed, "TileXREpCombineMemoryKernelBinaryData");
        CheckContains(embedPath, embed, "TileXREpCombineMemoryKernelBinarySize");
    }
}

void TestMemoryDispatchProfilingContract()
{
    const std::string profilePath = "src/ep/common/ep_dispatch_memory_profile.h";
    std::string profile;
    if (ReadFile(profilePath, &profile)) {
        CheckContains(profilePath, profile, "struct alignas(64) EpDispatchMemoryProfileRecord");
        CheckContains(profilePath, profile, "EP_DISPATCH_MEMORY_ROLE_SENDER");
        CheckContains(profilePath, profile, "EP_DISPATCH_MEMORY_ROLE_COUNT_CUMSUM");
        CheckContains(profilePath, profile, "sizeof(EpDispatchMemoryProfileRecord) == 512U");
    }

    const std::string windowPath = "src/ep/common/ep_memory_window.h";
    std::string window;
    if (ReadFile(windowPath, &window)) {
        CheckContains(windowPath, window, "kEpMemoryDispatchProfileBuffer = 0");
        CheckContains(windowPath, window, "kEpMemoryCombineProfileBuffer = 1");
        CheckContains(windowPath, window,
            "kEpMemoryDispatchProfileBuffer != kEpMemoryCombineProfileBuffer");
    }

    const std::string dispatchPath = "src/ep/kernels/tilexr_ep_dispatch_memory_kernel.h";
    std::string dispatch;
    if (ReadFile(dispatchPath, &dispatch)) {
        CheckContains(dispatchPath, dispatch, "#include \"ep_dispatch_memory_profile.h\"");
        CheckContains(dispatchPath, dispatch,
            "TILEXR_EP_DISPATCH_MEMORY_ENABLE_PROFILING");
        CheckContains(dispatchPath, dispatch, "ctx_.ProfileRecordBase");
        CheckContains(dispatchPath, dispatch,
            "TileXREp::kEpMemoryDispatchProfileBuffer");
        CheckContains(dispatchPath, dispatch,
            "EP_DISPATCH_MEMORY_TIME_FINAL_END");
        CheckContains(dispatchPath, dispatch, "WriteProfile()");
    }

    const std::string combinePath = "src/ep/kernels/tilexr_ep_combine_memory_kernel.h";
    std::string combine;
    if (ReadFile(combinePath, &combine)) {
        CheckContains(combinePath, combine,
            "TileXREp::kEpMemoryCombineProfileBuffer");
    }

    const std::string cmakePath = "src/ep/CMakeLists.txt";
    std::string cmake;
    if (ReadFile(cmakePath, &cmake)) {
        CheckContains(cmakePath, cmake,
            "option(TILEXR_EP_DISPATCH_MEMORY_ENABLE_PROFILING");
        CheckContains(cmakePath, cmake,
            "TILEXR_EP_DISPATCH_MEMORY_ENABLE_PROFILING=1");
    }
}

void TestNoForbiddenDependencies()
{
    const std::vector<std::string> paths = {
        "src/ep/kernels/tilexr_ep_dispatch_kernel.cpp",
        "src/ep/kernels/tilexr_ep_combine_kernel.cpp",
        "src/ep/kernels/tilexr_ep_kernel_common.h",
        "src/ep/kernels/tilexr_ep_dispatch_helpers.h",
        "src/ep/kernels/tilexr_ep_combine_helpers.h",
        "src/ep/host/ep_kernel_launch.cpp",
        "src/ep/CMakeLists.txt",
    };
    const std::vector<std::string> forbidden = {
        "ops-transformer",
        "GetHcclContext",
        "TileXRUDMARegister",
        "shmem",
    };

    for (std::vector<std::string>::const_iterator path = paths.begin(); path != paths.end(); ++path) {
        std::string contents;
        if (!ReadFile(*path, &contents)) {
            continue;
        }
        for (std::vector<std::string>::const_iterator needle = forbidden.begin(); needle != forbidden.end(); ++needle) {
            CheckNotContains(*path, contents, *needle);
        }
    }
}

} // namespace

int main()
{
    TestKernelUsesTileXRPeerMemory();
    TestCrossNodeDispatchUsesUDMARegistry();
    TestCrossNodeDispatchPullsRemoteSlots();
    TestCrossNodeDispatchSeparatesLocalAndRemotePeers();
    TestHostDispatchSplitsCrossNodeKernel();
    TestDispatchHelpersLiveInDispatchHelperFile();
    TestCombineHelpersLiveInCombineHelperFile();
    TestCombineKernelUsesTileXRPeerMemory();
    TestKernelCommonHasCombineHelpers();
    TestDispatchDemoRunsCombine();
    TestKernelForwardsActiveMask();
    TestKernelForwardsExpertTokenNumsType();
    TestKernelForwardsTpRecvCountsOut();
    TestDispatchDemoExercisesOptionalInputs();
    TestKernelForwardsSharedExpertConfig();
    TestKernelForwardsStaticQuantConfig();
    TestKernelForwardsPerTokenDynamicQuantConfig();
    TestClearLocalWindowDoesNotPreclearSlotHeaders();
    TestMemoryKernelsUseCommunicatorRounds();
    TestMemoryKernelUbStagingCoversPackedPayloads();
    TestMemoryCombineUsesRegisteredBinaryLaunch();
    TestMemoryDispatchProfilingContract();
    TestNoForbiddenDependencies();
    if (g_failures != 0) {
        std::cerr << g_failures << " TileXR EP kernel source checks failed" << std::endl;
        return 1;
    }
    std::cout << "TileXR EP kernel source checks passed" << std::endl;
    return 0;
}
