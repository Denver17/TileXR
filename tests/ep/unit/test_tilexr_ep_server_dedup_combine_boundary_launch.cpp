#include <array>
#include <cstring>
#include <iostream>
#include <set>
#include "../integration/combine_memory_boundary_test.h"
#include "runtime/kernel.h"
#include "tilexr_kernel_binary_loader.h"
#include "tilexr_types.h"
#include "server_dedup_test_checks.h"

namespace {
using ServerDedupTest::Check;
using namespace TileXRCombineMemoryBoundaryTest;
bool loadOk = true;
int binaryResult = 0, functionResult = 0, launchResult = 0;
unsigned loads = 0, registrations = 0, unregistrations = 0, launches = 0;
const void *signature = nullptr;
struct Captured { void *comm; void *result; uint64_t bytes; uint32_t window, index; } captured {};
alignas(32) std::array<uint8_t, 64> result {};
alignas(8) std::array<uint8_t, 8> comm {};
aclrtStream stream = reinterpret_cast<aclrtStream>(uintptr_t(0x20));
}
namespace TileXR {
bool LoadKernelBinary(const void *anchor, const char *name, KernelBinaryView *view)
{
    ++loads;
    Check(anchor && std::strcmp(name, "tilexr_ep_combine_memory_boundary_test_kernel.o") == 0,
        "test helper loads its own installed binary relative to its library");
    static const unsigned char bytes[] = {1, 2, 3};
    view->data = bytes; view->size = sizeof(bytes);
    return loadOk;
}
}
extern "C" {
rtError_t rtDevBinaryRegister(const rtDevBinary_t *binary, void **handle)
{
    ++registrations;
    Check(binary && binary->magic == 0x41415246U && binary->data && binary->length == 3,
        "registered AICore binary descriptor");
    *handle = binaryResult ? nullptr : reinterpret_cast<void *>(uintptr_t(0x40));
    return binaryResult;
}
rtError_t rtDevBinaryUnRegister(void *) { ++unregistrations; return 0; }
rtError_t rtFunctionRegister(void *handle, const void *stub, const char_t *name, const void *entry, uint32_t flags)
{
    Check(handle && stub && flags == 0 && std::strcmp(name, "tilexr_ep_combine_memory_boundary_test_kernel") == 0 &&
        std::strcmp(static_cast<const char *>(entry), name) == 0, "register exact probe entry");
    signature = stub;
    return functionResult;
}
rtError_t rtKernelLaunchWithFlagV2(const void *stub, uint32_t blocks, rtArgsEx_t *info,
    rtSmDesc_t *sm, rtStream_t actualStream, uint32_t flags, const rtTaskCfgInfo_t *config)
{
    ++launches;
    Check(stub == signature && blocks == 1 && actualStream == stream && sm == nullptr && flags == 0 && config,
        "single AIV registered launch uses caller stream");
    Check(info && info->argsSize == 32 && info->hasTiling == 0 && info->hostInputInfoNum == 0,
        "probe registered argument payload remains 32 bytes without inline tiling");
    if (info && info->argsSize == sizeof(captured)) std::memcpy(&captured, info->args, sizeof(captured));
    return launchResult;
}
}
int main()
{
    const std::array<uint64_t, 4> expected {{782336, 835552, 1048576, 4290772960ULL}};
    std::set<uint32_t> patterns;
    for (uint32_t window = 0; window < 2; ++window) for (uint32_t index = 0; index < 4; ++index) {
        const uint64_t offset = expected[index] + uint64_t(window) * 4294967296ULL;
        Check(ProbeOffset(window, index) == offset, "independent expected 64-bit probe address");
        Check(LegalRange(offset, 32) && LegalRange(GuardBeforeOffset(window, index), 32), "all targets/guards are legal");
        Check(index != 1 || LegalRange(offset + 32, 32), "control-tail after-guard stays outside runtime");
        Check(ProbePattern(window, index) != 0 && patterns.insert(ProbePattern(window, index)).second,
            "both windows use distinct nonzero word patterns");
    }
    for (uint64_t address : {kLegalWindowBytes, kWindowBytes - 32, kWindowBytes + kLegalWindowBytes, kPeerBytes, UINT64_MAX})
        Check(!LegalRange(address, 32), "runtime/reserved/outside ranges rejected");
    Check(!LegalRange(kLegalWindowBytes - 32, 64) && !LegalRange(0, UINT64_MAX) && !LegalRange(0, 0),
        "range arithmetic rejects crossing and overflow");
    Check(ProbeOffset(2, 0) == UINT64_MAX && ProbeOffset(0, 4) == UINT64_MAX && ProbePattern(2, 0) == 0,
        "invalid plan indices have no valid address or pattern");
    Check(Launch(nullptr, result.data(), 0, 0, stream) != 0 &&
        Launch(comm.data(), nullptr, 0, 0, stream) != 0 &&
        Launch(comm.data(), result.data(), 0, 0, nullptr) != 0 &&
        Launch(comm.data() + 1, result.data(), 0, 0, stream) != 0 &&
        Launch(comm.data(), result.data() + 1, 0, 0, stream) != 0 &&
        Launch(comm.data(), reinterpret_cast<void *>(UINTPTR_MAX - 31U), 0, 0, stream) != 0 &&
        Launch(comm.data(), result.data(), 2, 0, stream) != 0 &&
        Launch(comm.data(), result.data(), 0, 4, stream) != 0 && loads == 0,
        "invalid parameters fail before loading/registering");
    loadOk = false;
    Check(Launch(comm.data(), result.data(), 0, 0, stream) == TileXR::TILEXR_ERROR_NOT_INITIALIZED && registrations == 0,
        "missing binary cannot launch");
    loadOk = true; binaryResult = 91;
    Check(Launch(comm.data(), result.data(), 0, 0, stream) == 91 && launches == 0, "binary registration failure propagates");
    binaryResult = 0; functionResult = 92;
    Check(Launch(comm.data(), result.data(), 0, 0, stream) == 92 && unregistrations == 1 && launches == 0,
        "function registration failure unregisters partial binary");
    functionResult = 0;
    const auto registrationsBefore = registrations;
    for (uint32_t window = 0; window < 2; ++window) for (uint32_t index = 0; index < 4; ++index) {
        Check(Launch(comm.data(), result.data(), window, index, stream) == 0, "valid probe launches");
        Check(captured.comm == comm.data() && captured.result == result.data() && captured.bytes == 8589934592ULL &&
            captured.window == window && captured.index == index, "ABI carries full 8GiB capacity and window/index unchanged");
    }
    Check(launches == 8 && registrations == registrationsBefore + 1, "eight probes reuse one successful registration");
    launchResult = 93;
    Check(Launch(comm.data(), result.data(), 1, 3, stream) == 93, "launch failure propagates");
    if (ServerDedupTest::Failures()) return 1;
    std::cout << "EP_SERVER_DEDUP_COMBINE_BOUNDARY_LAUNCH_PASS\n";
    return 0;
}
