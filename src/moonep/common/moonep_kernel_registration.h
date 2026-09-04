#ifndef TILEXR_MOONEP_KERNEL_REGISTRATION_H
#define TILEXR_MOONEP_KERNEL_REGISTRATION_H

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace TileXRMoonEp {

constexpr uintptr_t kPlannerKernelSignature = UINT64_C(0x4D4F4F001000);
constexpr uintptr_t kPlannerV2KernelSignature = UINT64_C(0x4D4F4F006000);
constexpr uintptr_t kDispatchKernelSignature = UINT64_C(0x4D4F4F002000);
constexpr uintptr_t kCombineKernelSignature = UINT64_C(0x4D4F4F003000);
constexpr uintptr_t kPrefetchWeightKernelSignature = UINT64_C(0x4D4F4F004000);
constexpr uintptr_t kReduceGradKernelSignature = UINT64_C(0x4D4F4F005000);
constexpr uintptr_t kCombineV2KernelSignature = UINT64_C(0x4D4F4F007000);

struct KernelRegistrationState {
    std::mutex mutex;
    bool registered = false;
    void *binaryHandle = nullptr;
};

inline const void *KernelSignature(uintptr_t value)
{
    return reinterpret_cast<const void *>(value);
}

int EnsureMoonEpKernelRegistered(KernelRegistrationState &state,
    const char *binaryFileName, const void *signature, const char *kernelName);

} // namespace TileXRMoonEp

#endif // TILEXR_MOONEP_KERNEL_REGISTRATION_H
