// Test-only registered-launch ABI declarations for the Relay runtime.
#pragma once
#include <cstdint>
using rtStream_t = void *;
constexpr uint32_t RT_DEV_BINARY_MAGIC_ELF = 0x43554245;
struct rtDevBinary_t { uint32_t magic{}, version{}; const void *data{}; uint32_t length{}; };
struct rtArgsEx_t { void *args{}; uint32_t argsSize{}; };
struct rtTaskCfgInfo_t { uint8_t schemMode{}; };
extern "C" {
int rtDevBinaryRegister(const rtDevBinary_t *, void **);
int rtDevBinaryUnRegister(void *);
int rtFunctionRegister(void *, const void *, const char *, const void *, uint32_t);
int rtKernelLaunchWithFlagV2(const void *, uint32_t, rtArgsEx_t *, void *, rtStream_t,
    uint32_t, const rtTaskCfgInfo_t *);
}
