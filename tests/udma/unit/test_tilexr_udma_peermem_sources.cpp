/*
 * Copyright (c) 2024-2026 TileXR Project
 * Licensed under the Apache License, Version 2.0
 */

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifndef TILEXR_SOURCE_DIR
#error "TILEXR_SOURCE_DIR must be defined"
#endif

namespace {

std::string Read(const std::string& path)
{
    std::ifstream input(path);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

void Require(const std::string& text, const std::string& needle, const char* label)
{
    if (text.find(needle) == std::string::npos) {
        std::cerr << "missing " << label << ": " << needle << std::endl;
        std::exit(1);
    }
}

void Reject(const std::string& text, const std::string& needle, const char* label)
{
    if (text.find(needle) != std::string::npos) {
        std::cerr << "forbidden " << label << ": " << needle << std::endl;
        std::exit(1);
    }
}

} // namespace

int main()
{
    const std::string root = TILEXR_SOURCE_DIR;
    const std::string comm = Read(root + "/src/comm/tilexr_comm.cpp");
    const std::string kernel = Read(root +
        "/tests/udma/demo/tilexr_udma_peermem_probe_kernel.cpp");
    const std::string host = Read(root +
        "/tests/udma/demo/tilexr_udma_peermem_probe.cpp");
    const std::string layout = Read(root +
        "/tests/udma/demo/tilexr_udma_peermem_probe_layout.h");

    Require(comm, "TILEXR_UDMA_USE_PEER_MEM", "mode agreement");
    Require(comm, "RegisterMemory(\n        peerMem_[rank_], peerMemBytes", "owner registration");
    Require(comm, "cannot replace communicator-owned peer memory", "replacement guard");
    Require(comm, "udmaContext_->Shutdown();\n        udmaContext_.reset();\n    }\n    if (ipcMemInited_)",
        "UDMA-before-IPC teardown");
    Require(layout, "kPayloadOffset = static_cast<uint64_t>(TileXR::IPC_DATA_OFFSET)",
        "payload data offset");
    Require(layout, "kReadyOffset % TileXR::TILEXR_UDMA_CACHE_LINE_SIZE", "ready alignment");
    Require(kernel, "UDMAQuietStatusOnQp", "payload completion");
    Require(kernel, "WaitTokenAcquire(ownerPeer + kReadyOffset", "ready acquire");
    Require(kernel, "DcciDst::CACHELINE_OUT", "scalar cache acquire");
    Require(kernel,
        "UDMACleanCacheLines(\n                ownerPeer + kPayloadOffset, kPayloadBytes)",
        "reused payload acquire");
    Require(kernel, "kPreparedOffset", "launch-order handshake");
    Require(kernel, "CopyPayloadMte(ownerPeer + kPayloadOffset", "IPC payload consume");
    Require(kernel, "kDoneOffset", "owner lifetime closure");
    Require(host, "rtDevBinaryRegister", "pure AICore registration");
    Require(host, "rtFunctionRegister", "pure AICore function registration");
    Require(host, "rtKernelLaunchWithFlagV2", "registered kernel launch");
    Reject(host, "<<<", "Host triple-chevron launch");
    std::cout << "UDMA peer-memory source guards passed" << std::endl;
    return 0;
}
