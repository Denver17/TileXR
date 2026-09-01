/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "acl/acl.h"
#include "runtime/kernel.h"
#include "tilexr_api.h"
#include "tilexr_types.h"
#include "tilexr_udma_peermem_probe_layout.h"

extern "C" {
extern const unsigned char TileXRUDMAPeerMemProbeKernelBinaryData[];
extern const std::size_t TileXRUDMAPeerMemProbeKernelBinarySize;
}

namespace {

using namespace TileXRUDMAPeerMemProbe;

constexpr uintptr_t kKernelSignatureValue = UINT64_C(0x5458504d00001000);
constexpr const char* kKernelName = "tilexr_udma_peermem_probe_kernel";

int EnvInt(const char* name, int defaultValue)
{
    const char* value = std::getenv(name);
    return value == nullptr ? defaultValue : std::atoi(value);
}

int Rank()
{
    const char* names[] = {"PMI_RANK", "OMPI_COMM_WORLD_RANK", "RANK"};
    for (const char* name : names) {
        if (const char* value = std::getenv(name)) {
            return std::atoi(value);
        }
    }
    return 0;
}

int RankSize()
{
    const char* names[] = {"PMI_SIZE", "OMPI_COMM_WORLD_SIZE", "RANK_SIZE"};
    for (const char* name : names) {
        if (const char* value = std::getenv(name)) {
            return std::atoi(value);
        }
    }
    return 1;
}

int LocalRank()
{
    const char* names[] = {"MPI_LOCALRANKID", "OMPI_COMM_WORLD_LOCAL_RANK", "LOCAL_RANK"};
    for (const char* name : names) {
        if (const char* value = std::getenv(name)) {
            return std::atoi(value);
        }
    }
    return 0;
}

bool AclOk(int rank, const char* step, aclError ret)
{
    if (ret == ACL_SUCCESS) {
        return true;
    }
    std::cerr << "[rank " << rank << "] ERROR: " << step << " ret=" << ret << std::endl;
    const char* recent = aclGetRecentErrMsg();
    if (recent != nullptr && recent[0] != '\0') {
        std::cerr << "[rank " << rank << "] recent error: " << recent << std::endl;
    }
    return false;
}

bool TileXROk(int rank, const char* step, int ret)
{
    if (ret == TileXR::TILEXR_SUCCESS) {
        return true;
    }
    std::cerr << "[rank " << rank << "] ERROR: " << step << " ret=" << ret << std::endl;
    return false;
}

uint8_t Pattern(uint32_t generation, uint32_t index)
{
    const uint64_t mixed = static_cast<uint64_t>(generation + 1U) * UINT64_C(2654435761) +
        static_cast<uint64_t>(index) * UINT64_C(2246822519);
    return static_cast<uint8_t>((mixed ^ (mixed >> 13U) ^ (mixed >> 29U)) & 0xffU);
}

bool CopyH2D(int rank, GM_ADDR destination, size_t capacity,
    const void* source, size_t bytes, const char* step)
{
    return AclOk(rank, step, aclrtMemcpy(destination, capacity, source, bytes,
        ACL_MEMCPY_HOST_TO_DEVICE));
}

bool InitializeProtocol(int rank, TileXR::CommArgs* args, uint32_t generations)
{
    GM_ADDR local = args->peerMems[rank];
    if (local == nullptr || args->peerMemBytes < kRequiredPeerMemBytes) {
        return false;
    }
    if (!AclOk(rank, "clear protocol window", aclrtMemset(
            local + kPayloadOffset, args->peerMemBytes - kPayloadOffset, 0,
            kRequiredPeerMemBytes - kPayloadOffset))) {
        return false;
    }
    if ((rank == kOwnerRank || rank == kProducerRank) &&
        !CopyH2D(rank, local + kInitSourceOffset, kTokenBytes,
            &kInitToken, sizeof(kInitToken), "initialize handshake token")) {
        return false;
    }
    if (rank == kOwnerRank) {
        return true;
    }
    for (uint32_t generation = 0U; generation < generations; ++generation) {
        const uint64_t token = GenerationToken(generation);
        if (rank == kProducerRank) {
            std::vector<uint8_t> payload(kPayloadBytes);
            for (uint32_t index = 0U; index < kPayloadBytes; ++index) {
                payload[index] = Pattern(generation, index);
            }
            if (!CopyH2D(rank, local + kSourcePayloadOffset +
                    static_cast<uint64_t>(generation) * kPayloadBytes,
                    kPayloadBytes, payload.data(), payload.size(),
                    "initialize producer payload") ||
                !CopyH2D(rank, local + kReadySourceOffset +
                    static_cast<uint64_t>(generation) * kTokenBytes,
                    kTokenBytes, &token, sizeof(token), "initialize ready token")) {
                return false;
            }
        }
    }
    return true;
}

bool RegisterKernel(int rank, void*& handle)
{
    rtDevBinary_t binary {};
    binary.magic = RT_DEV_BINARY_MAGIC_ELF_AIVEC;
    binary.version = 0;
    binary.data = TileXRUDMAPeerMemProbeKernelBinaryData;
    binary.length = static_cast<uint64_t>(TileXRUDMAPeerMemProbeKernelBinarySize);
    rtError_t ret = rtDevBinaryRegister(&binary, &handle);
    if (ret == RT_ERROR_NONE) {
        ret = rtFunctionRegister(handle,
            reinterpret_cast<const void*>(kKernelSignatureValue),
            kKernelName, kKernelName, 0);
    }
    if (ret != RT_ERROR_NONE) {
        std::cerr << "[rank " << rank << "] ERROR: register kernel ret=" << ret << std::endl;
        if (handle != nullptr) {
            (void)rtDevBinaryUnRegister(handle);
            handle = nullptr;
        }
        return false;
    }
    return true;
}

bool Launch(int rank, void*& binaryHandle, aclrtStream stream,
    GM_ADDR commArgs, GM_ADDR output, GM_ADDR status, uint32_t generations)
{
    if (!RegisterKernel(rank, binaryHandle)) {
        return false;
    }
    struct Args {
        GM_ADDR commArgs;
        GM_ADDR output;
        GM_ADDR status;
        uint32_t generations;
    } args {commArgs, output, status, generations};
    rtArgsEx_t argsInfo {};
    argsInfo.args = &args;
    argsInfo.argsSize = sizeof(args);
    rtTaskCfgInfo_t cfgInfo {};
    cfgInfo.schemMode = 1;
    const rtError_t ret = rtKernelLaunchWithFlagV2(
        reinterpret_cast<const void*>(kKernelSignatureValue), 1U,
        &argsInfo, nullptr, static_cast<rtStream_t>(stream), 0U, &cfgInfo);
    if (ret != RT_ERROR_NONE) {
        std::cerr << "[rank " << rank << "] ERROR: launch kernel ret=" << ret << std::endl;
        return false;
    }
    return true;
}

bool ValidateConsumer(int rank, GM_ADDR output, uint32_t generations)
{
    std::vector<uint8_t> host(static_cast<size_t>(generations) * kPayloadBytes);
    if (!AclOk(rank, "copy consumer output", aclrtMemcpy(host.data(), host.size(),
            output, host.size(), ACL_MEMCPY_DEVICE_TO_HOST))) {
        return false;
    }
    for (uint32_t generation = 0U; generation < generations; ++generation) {
        for (uint32_t index = 0U; index < kPayloadBytes; ++index) {
            const uint8_t actual = host[static_cast<size_t>(generation) * kPayloadBytes + index];
            const uint8_t expected = Pattern(generation, index);
            if (actual != expected) {
                std::cerr << "[rank " << rank << "] ERROR: generation=" << generation
                          << " offset=" << index << " actual=" << static_cast<uint32_t>(actual)
                          << " expected=" << static_cast<uint32_t>(expected) << std::endl;
                return false;
            }
        }
    }
    return true;
}

} // namespace

int main()
{
    const int rank = Rank();
    const int rankSize = RankSize();
    const int device = LocalRank();
    const int generationsValue = EnvInt("TILEXR_UDMA_PEERMEM_GENERATIONS",
        static_cast<int>(kDefaultGenerations));
    if (rankSize != kRankSize || generationsValue <= 0 ||
        generationsValue > static_cast<int>(kMaxGenerations)) {
        std::cerr << "[rank " << rank << "] ERROR: probe requires exactly 3 ranks and 1.."
                  << kMaxGenerations << " generations" << std::endl;
        return 2;
    }
    const uint32_t generations = static_cast<uint32_t>(generationsValue);
    (void)setenv("TILEXR_UDMA_USE_PEER_MEM", "1", 1);
    (void)setenv("TILEXR_ENABLE_IPC", "1", 1);

    TileXRCommPtr comm = nullptr;
    aclrtStream stream = nullptr;
    void* output = nullptr;
    void* statusDev = nullptr;
    void* binaryHandle = nullptr;
    bool ok = AclOk(rank, "aclInit", aclInit(nullptr)) &&
        AclOk(rank, "aclrtSetDevice", aclrtSetDevice(device)) &&
        TileXROk(rank, "TileXRCommInitRankLocal",
            TileXRCommInitRankLocal(rankSize, rank, &comm));

    TileXR::CommArgs* argsHost = nullptr;
    GM_ADDR argsDev = nullptr;
    const TileXR::TileXRUDMARegistry* registry = nullptr;
    if (ok) {
        ok = TileXROk(rank, "TileXRGetCommArgsHost", TileXRGetCommArgsHost(comm, argsHost)) &&
            TileXROk(rank, "TileXRGetCommArgsDev", TileXRGetCommArgsDev(comm, argsDev)) &&
            TileXROk(rank, "TileXRGetUDMARegistryHost", TileXRGetUDMARegistryHost(comm, &registry)) &&
            argsHost != nullptr && argsDev != nullptr && registry != nullptr &&
            registry->regions[rank].base == argsHost->peerMems[rank] &&
            registry->regions[rank].bytes == argsHost->peerMemBytes &&
            InitializeProtocol(rank, argsHost, generations);
    }
    const size_t outputBytes = static_cast<size_t>(generations) * kPayloadBytes;
    if (ok) {
        ok = AclOk(rank, "allocate output", aclrtMalloc(&output, outputBytes,
                ACL_MEM_MALLOC_HUGE_FIRST)) &&
            AclOk(rank, "allocate status", aclrtMalloc(&statusDev, sizeof(Status),
                ACL_MEM_MALLOC_HUGE_FIRST)) &&
            AclOk(rank, "clear output", aclrtMemset(output, outputBytes, 0, outputBytes)) &&
            AclOk(rank, "clear status", aclrtMemset(statusDev, sizeof(Status), 0, sizeof(Status))) &&
            AclOk(rank, "create stream", aclrtCreateStream(&stream)) &&
            Launch(rank, binaryHandle, stream, argsDev, static_cast<GM_ADDR>(output),
                static_cast<GM_ADDR>(statusDev), generations) &&
            AclOk(rank, "synchronize stream", aclrtSynchronizeStream(stream));
    }

    Status status {};
    bool statusCopied = false;
    if (statusDev != nullptr) {
        statusCopied = AclOk(rank, "copy status", aclrtMemcpy(&status, sizeof(status),
            statusDev, sizeof(status), ACL_MEMCPY_DEVICE_TO_HOST));
    }
    if (ok) {
        ok = statusCopied &&
            status.magic == kStatusMagic &&
            status.result == 0U &&
            status.completedGenerations == generations &&
            status.registryBase == status.localPeerMem;
        if (rank == kConsumerRank) {
            ok = ok && status.observedOwnerPeerMem != 0U &&
                status.registryOwnerBase != 0U &&
                status.observedOwnerPeerMem != status.registryOwnerBase &&
                ValidateConsumer(rank, static_cast<GM_ADDR>(output), generations);
        }
    }

    std::cout << "[rank " << rank << "] peerMem=" << reinterpret_cast<void*>(status.localPeerMem)
              << " ownerMapping=" << reinterpret_cast<void*>(status.observedOwnerPeerMem)
              << " ownerRegistry=" << reinterpret_cast<void*>(status.registryOwnerBase)
              << " generations=" << status.completedGenerations
              << " result=" << status.result
              << " failureStep=" << status.failureStep << std::endl;
    if (binaryHandle != nullptr) {
        (void)rtDevBinaryUnRegister(binaryHandle);
    }
    if (stream != nullptr) {
        (void)aclrtDestroyStream(stream);
    }
    if (statusDev != nullptr) {
        (void)aclrtFree(statusDev);
    }
    if (output != nullptr) {
        (void)aclrtFree(output);
    }
    if (comm != nullptr) {
        const int destroyRet = TileXRCommDestroy(comm);
        ok = ok && destroyRet == TileXR::TILEXR_SUCCESS;
    }
    (void)aclrtResetDevice(device);
    (void)aclFinalize();
    std::cout << "[rank " << rank << "] TileXR UDMA peer-memory alias probe "
              << (ok ? "success" : "failed") << std::endl;
    return ok ? 0 : 1;
}
