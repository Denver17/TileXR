#include <cstdlib>
#include <iostream>
#include <dlfcn.h>
#include "acl/acl.h"
#include "tilexr_ep.h"

// 仅测试内部注册入口；不将它安装为公共 API，也不复制生产实现。
namespace TileXREp { int TileXREpRegisterDispatchMemoryServerDedupKernel(); }

int main()
{
    // volatile 保留对已安装公共声明及真实导出符号的链接引用。
    auto *volatile ordinary = &TileXRMoeEpDispatchMemory;
    auto *volatile dedup = &TileXRMoeEpDispatchMemoryServerDedup;
    if (ordinary == nullptr || dedup == nullptr) { return 1; }
    const char *configured = std::getenv("TILEXR_EP_SERVER_DEDUP_TEST_DEVICE");
    char *end = nullptr;
    const long device = configured == nullptr ? 0 : std::strtol(configured, &end, 10);
    if (device < 0 || device > 255 || (configured != nullptr && (*configured == '\0' || *end != '\0'))) {
        std::cerr << "Invalid registration device" << std::endl;
        return 1;
    }
    if (aclInit(nullptr) != ACL_SUCCESS) { return 1; }
    const aclError setResult = aclrtSetDevice(static_cast<int32_t>(device));
    bool passed = setResult == ACL_SUCCESS;
    if (passed) {
        Dl_info library {};
        passed = dladdr(reinterpret_cast<const void *>(dedup), &library) != 0;
        if (passed) {
            std::cout << "device=" << device << " soc=" << aclrtGetSocName()
                      << " library=" << library.dli_fname << std::endl;
            const int ret = TileXREp::TileXREpRegisterDispatchMemoryServerDedupKernel();
            std::cout << "registration_ret=" << ret << std::endl;
            passed = ret == TileXR::TILEXR_SUCCESS;
        }
        if (aclrtResetDevice(static_cast<int32_t>(device)) != ACL_SUCCESS) { passed = false; }
    } else {
        std::cerr << "aclrtSetDevice failed: " << setResult << std::endl;
    }
    if (aclFinalize() != ACL_SUCCESS) { passed = false; }
    std::cout << "EP_SERVER_DEDUP_EMPTY_KERNEL_" << (passed ? "PASS" : "FAIL")
              << " test=registration" << std::endl;
    return passed ? 0 : 1;
}
