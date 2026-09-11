// Included inside the chain consumer's namespace to reuse its rank identity,
// bounded Host barrier, and CommArgs checks. This is test scaffolding only.

void CheckBytes(const uint8_t *data, std::size_t bytes, uint8_t expected, const char *name)
{
    for (std::size_t i = 0; i < bytes; ++i) {
        Require(data[i] == expected, std::string(name) + " byte=" + std::to_string(i) +
            " expected=" + std::to_string(expected) + " actual=" + std::to_string(data[i]));
    }
}

int RunMemoryBoundary(const Config &c)
{
    using namespace TileXRMemoryBoundaryTest;
    constexpr int dataMiB = static_cast<int>((kPeerBytes - TileXR::TILEXR_FLAG_BUFF_BYTES) / (1024 * 1024));
    bool initialized = false, deviceSet = false, safeCleanup = true, success = false;
    TileXRCommPtr comm = nullptr;
    aclrtStream stream = nullptr;
    void *result = nullptr;
    std::string stage = "acl-init";
    Barrier barrier(c, Clock::now() + std::chrono::seconds(90));
    try {
        Check(aclInit(nullptr), "aclInit"); initialized = true;
        stage = "device";
        Check(aclrtSetDevice(c.device), "aclrtSetDevice"); deviceSet = true;
        const char *soc = aclrtGetSocName();
        Require(soc != nullptr && (std::strncmp(soc, "Ascend950", 9) == 0 ||
            std::strncmp(soc, "Ascend910_9", 11) == 0 || std::strncmp(soc, "A5", 2) == 0), "A5 required");
        Check(aclrtCreateStream(&stream), "aclrtCreateStream");
        stage = "4g-communicator";
        // InitMem Host-zeroes the complete allocation before publishing any IPC mapping.
        Check(TileXRCommInitRankMemoryDomainWithSize(0, dataMiB, c.rankSize, c.rank, &comm),
            "TileXRCommInitRankMemoryDomainWithSize");
        safeCleanup = false;
        TileXR::CommArgs *host = nullptr;
        GM_ADDR device = nullptr;
        Check(TileXRGetCommArgsHost(comm, host), "TileXRGetCommArgsHost");
        Check(TileXRGetCommArgsDev(comm, device), "TileXRGetCommArgsDev");
        Require(host != nullptr && device != nullptr, "missing Host/device CommArgs");
        ValidateComm(c, *host);
        Require(host->peerMemBytes == kPeerBytes, "peer window is not exactly 4 GiB");
        TileXR::CommArgs deviceArgs {};
        Check(aclrtMemcpy(&deviceArgs, sizeof(deviceArgs), device, sizeof(deviceArgs), ACL_MEMCPY_DEVICE_TO_HOST),
            "read device CommArgs");
        ValidateComm(c, deviceArgs);
        Require(deviceArgs.peerMemBytes == kPeerBytes, "device peer window is not exactly 4 GiB");
        for (int rank = 0; rank < c.rankSize; ++rank) {
            Require(host->peerMems[rank] == deviceArgs.peerMems[rank], "Host/device peer mapping differs");
        }
        stage = "chain-barrier";
        barrier.Run(0);
        Marker(c, "EP_SERVER_DEDUP_CHAIN_PASS", ",\"peer_count\":" + std::to_string(c.rankSize) +
            ",\"peer_mem_bytes\":" + std::to_string(kPeerBytes));

        stage = "boundary-initial-zero";
        GM_ADDR tail = host->peerMems[c.rank] + kOffset;
        std::array<uint8_t, 64> values {};
        Check(aclrtMemcpy(values.data(), values.size(), tail - kProbeBytes, values.size(), ACL_MEMCPY_DEVICE_TO_HOST),
            "read initial boundary and guard");
        CheckBytes(values.data(), values.size(), 0, "initial boundary/guard");
        Check(aclrtMalloc(&result, values.size(), ACL_MEM_MALLOC_HUGE_FIRST), "allocate device readback");
        Check(aclrtMemset(result, values.size(), 0x55, values.size()), "initialize readback sentinel");
        stage = "boundary-launch";
        Check(Launch(device, result, stream), "memory boundary kernel");
        stage = "boundary-sync";
        Check(aclrtSynchronizeStream(stream), "aclrtSynchronizeStream");

        stage = "boundary-verify";
        Check(aclrtMemcpy(values.data(), values.size(), result, values.size(), ACL_MEMCPY_DEVICE_TO_HOST),
            "read device roundtrip");
        CheckBytes(values.data(), kProbeBytes, 0, "device initial read");
        CheckBytes(values.data() + kProbeBytes, kProbeBytes, 0xFF, "device readback");
        Check(aclrtMemcpy(values.data(), values.size(), tail - kProbeBytes, values.size(), ACL_MEMCPY_DEVICE_TO_HOST),
            "read final boundary and guard");
        CheckBytes(values.data(), kProbeBytes, 0, "preceding guard");
        CheckBytes(values.data() + kProbeBytes, kProbeBytes, 0xFF, "Host boundary read");
        stage = "completion-barrier";
        barrier.Run(1);
        safeCleanup = true;
        success = true;
    } catch (const std::exception &error) {
        std::cerr << "ERROR EP_SERVER_DEDUP_FAIL rank=" << c.rank << " stage=" << stage << ' ' << error.what() << std::endl;
    }
    if (!safeCleanup) {
        std::cerr << "ERROR rank=" << c.rank << " cleanup=deferred-to-process-exit: completion unconfirmed" << std::endl;
        return 1;
    }
    auto cleanup = [&](int code, const char *name) {
        if (code != 0) {
            success = false;
            std::cerr << "ERROR rank=" << c.rank << " stage=cleanup " << name << " ret=" << code << std::endl;
        }
    };
    if (result != nullptr) cleanup(aclrtFree(result), "aclrtFree");
    if (comm != nullptr) cleanup(TileXRCommDestroy(comm), "TileXRCommDestroy");
    if (stream != nullptr) cleanup(aclrtDestroyStream(stream), "aclrtDestroyStream");
    if (deviceSet) cleanup(aclrtResetDevice(c.device), "aclrtResetDevice");
    if (initialized) cleanup(aclFinalize(), "aclFinalize");
    if (success) {
        Marker(c, "EP_SERVER_DEDUP_MEMORY_BOUNDARY_PASS",
            ",\"peer_mem_bytes\":" + std::to_string(kPeerBytes) + ",\"boundary_offset\":" + std::to_string(kOffset) +
            ",\"probe_bytes\":32,\"initial_zero\":true,\"device_initial_zero\":true,\"device_readback_ff\":true,"
            "\"host_boundary_ff\":true,\"guard_zero\":true,\"submitted\":1,\"completed\":1,\"cleanup\":\"complete\"");
    }
    return success ? 0 : 1;
}
