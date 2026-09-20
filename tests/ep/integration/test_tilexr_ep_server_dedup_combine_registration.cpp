#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#include <dlfcn.h>

#include "acl/acl.h"
#include "tilexr_ep.h"
#include "ep_memory_server_dedup_combine_launch.h"

namespace {
bool Acl(aclError result, const char *stage)
{
    if (result == ACL_SUCCESS) return true;
    std::cerr << stage << " failed: " << result << std::endl;
    return false;
}

bool Run(int32_t device)
{
    aclrtStream stream = nullptr;
    void *arena = nullptr;
    void *deviceTiling = nullptr;
    bool passed = false;
    constexpr size_t slotBytes = 4096;
    constexpr size_t arenaBytes = slotBytes * 9; // CommArgs plus eight business tensors.
    std::vector<uint8_t> before(arenaBytes), after(arenaBytes);
    for (size_t i = 0; i < arenaBytes; ++i) before[i] = static_cast<uint8_t>(i * 17U + i / slotBytes);
    std::array<TileXREp::EpServerDedupCombineTilingData, 2> tiling {};
    for (size_t i = 0; i < tiling.size(); ++i) {
        tiling[i].bs = 1; tiling[i].h = 13; tiling[i].moeExpertNum = 16; tiling[i].topK = 1;
        tiling[i].dtype = static_cast<uint8_t>(i == 0 ? TileXR::TILEXR_DATA_TYPE_FP16 : TileXR::TILEXR_DATA_TYPE_BFP16);
    }
    do {
        // Link the installed public symbol and report its owning library. The
        // runtime portion tests internal registration/submit; C4 supplies a real comm.
        Dl_info library {};
        if (dladdr(reinterpret_cast<const void *>(&TileXRMoeEpCombineMemoryServerDedup), &library) == 0) break;
        const char *soc = aclrtGetSocName();
        std::cout << "device=" << device << " soc=" << (soc == nullptr ? "unknown" : soc)
                  << " library=" << library.dli_fname << " block_dim=64 launch_bytes=88 tiling_bytes=16" << std::endl;
        int64_t cores = 0;
        if (!Acl(aclrtGetDeviceInfo(static_cast<uint32_t>(device), ACL_DEV_ATTR_VECTOR_CORE_NUM, &cores), "core query")) break;
        if (cores < TileXREp::kEpServerDedupCombineCoreNum) {
            std::cerr << "Combine needs at least 64 AIVs; actual=" << cores << std::endl;
            break;
        }
        int ret = TileXREp::TileXREpRegisterCombineMemoryServerDedupKernel();
        std::cout << "registration_ret=" << ret << std::endl;
        if (ret != TileXR::TILEXR_SUCCESS) break;
        if (TileXREp::TileXREpRegisterCombineMemoryServerDedupKernel() != TileXR::TILEXR_SUCCESS) break;
        if (!Acl(aclrtCreateStream(&stream), "create stream") ||
            !Acl(aclrtMalloc(&arena, arenaBytes, ACL_MEM_MALLOC_NORMAL_ONLY), "allocate sentinels") ||
            !Acl(aclrtMalloc(&deviceTiling, sizeof(tiling), ACL_MEM_MALLOC_NORMAL_ONLY), "allocate tiling") ||
            !Acl(aclrtMemcpy(arena, arenaBytes, before.data(), arenaBytes, ACL_MEMCPY_HOST_TO_DEVICE), "initialize sentinels") ||
            !Acl(aclrtMemcpy(deviceTiling, sizeof(tiling), tiling.data(), sizeof(tiling), ACL_MEMCPY_HOST_TO_DEVICE), "initialize tiling")) break;
        bool submitted = true;
        for (size_t i = 0; i < tiling.size(); ++i) {
            auto *base = static_cast<GM_ADDR>(arena);
            TileXREp::EpServerDedupCombineKernelArgs args {base,
                base + slotBytes, base + 2 * slotBytes, base + 3 * slotBytes,
                base + 4 * slotBytes, base + 5 * slotBytes, base + 6 * slotBytes,
                base + 7 * slotBytes, base + 8 * slotBytes,
                static_cast<GM_ADDR>(deviceTiling) + i * sizeof(tiling[0]), static_cast<int64_t>(i + 1)};
            ret = TileXREp::TileXREpSubmitCombineMemoryServerDedupKernel(args, TileXREp::kEpServerDedupCombineCoreNum, stream);
            std::cout << "launch=" << i << " dtype=" << unsigned(tiling[i].dtype) << " magic=" << args.magic
                      << " submit_ret=" << ret << std::endl;
            if (ret != TileXR::TILEXR_SUCCESS) { submitted = false; break; }
        }
        if (!submitted || !Acl(aclrtSynchronizeStream(stream), "empty Combine synchronization")) break;
        if (!Acl(aclrtMemcpy(after.data(), arenaBytes, arena, arenaBytes, ACL_MEMCPY_DEVICE_TO_HOST), "read sentinels")) break;
        std::array<TileXREp::EpServerDedupCombineTilingData, 2> afterTiling {};
        if (!Acl(aclrtMemcpy(afterTiling.data(), sizeof(afterTiling), deviceTiling, sizeof(tiling),
                ACL_MEMCPY_DEVICE_TO_HOST), "read tiling")) break;
        passed = before == after && std::memcmp(tiling.data(), afterTiling.data(), sizeof(tiling)) == 0;
        if (!passed) std::cerr << "Empty Combine modified input/output/CommArgs/tiling sentinels" << std::endl;
    } while (false);
    // Even a later failed submit may follow an earlier queued call. Preserve
    // both immutable tilings and all sentinels until the stream is drained.
    if (stream != nullptr && !Acl(aclrtSynchronizeStream(stream), "cleanup synchronization")) passed = false;
    if (deviceTiling != nullptr && !Acl(aclrtFree(deviceTiling), "free tiling")) passed = false;
    if (arena != nullptr && !Acl(aclrtFree(arena), "free sentinels")) passed = false;
    if (stream != nullptr && !Acl(aclrtDestroyStream(stream), "destroy stream")) passed = false;
    return passed;
}
} // namespace

int main()
{
    const char *configured = std::getenv("TILEXR_EP_SERVER_DEDUP_TEST_DEVICE");
    char *end = nullptr;
    errno = 0;
    const long device = configured == nullptr ? 0 : std::strtol(configured, &end, 10);
    bool passed = errno == 0 && device >= 0 && device <= 255 &&
        (configured == nullptr || (*configured != '\0' && *end == '\0'));
    if (!passed) std::cerr << "Invalid C2 registration device" << std::endl;
    if (passed && Acl(aclInit(nullptr), "aclInit")) {
        const bool selected = Acl(aclrtSetDevice(static_cast<int32_t>(device)), "set device");
        passed = selected && Run(static_cast<int32_t>(device));
        if (selected && !Acl(aclrtResetDevice(static_cast<int32_t>(device)), "reset device")) passed = false;
        if (!Acl(aclFinalize(), "aclFinalize")) passed = false;
    } else { passed = false; }
    std::cout << "EP_SERVER_DEDUP_COMBINE_EMPTY_KERNEL_" << (passed ? "PASS" : "FAIL")
              << " test=registration" << std::endl;
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
