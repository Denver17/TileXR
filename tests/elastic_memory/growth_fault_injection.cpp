/*
 * 在真实 HBM 分配成功后注入 Host 记账异常，检查 Pool 向驻留 kernel 返回失败的路径。
 * 定义 TILEXR_GROWTH_FAULT_PRELOAD 时构建预加载库：拦截选定大小的 aclrtMalloc，
 * 令该线程下一次 operator new 抛出 bad_alloc，并统计这块 HBM 的实际释放。
 * 另一构建入口加载外置 ElasticPoolTest，通过 Pending::Wait 检查错误、分配前副作用
 * 和失败后的准入拒绝。未发布的 HBM 不应计入 pool 增长；测试在调用完成并关闭 pool
 * 后释放辅助内存、stream 和已注册 kernel。此文件用于故障注入，不参与生产分配。
 */
#include <acl/acl.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <new>

#ifdef TILEXR_GROWTH_FAULT_PRELOAD
namespace {
thread_local bool failNextAllocation = false;
uint64_t armed = 0, injected = 0, freed = 0;
void* selectedAllocation = nullptr;
uint64_t SelectedBytes()
{
    const char* text = std::getenv("TILEXR_TEST_GROWTH_FAULT_BYTES");
    return text ? std::strtoull(text, nullptr, 10) : 0;
}
}
extern "C" aclError aclrtMalloc(void** address, size_t bytes, aclrtMemMallocPolicy policy)
{
    using Function = aclError (*)(void**, size_t, aclrtMemMallocPolicy);
    static auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "aclrtMalloc"));
    if (!real) std::abort();
    const aclError status = real(address, bytes, policy);
    if (status == ACL_SUCCESS && bytes != 0 && bytes == SelectedBytes() &&
        __atomic_fetch_add(&armed, uint64_t(1), __ATOMIC_RELAXED) == 0) {
        __atomic_store_n(&selectedAllocation, *address, __ATOMIC_RELEASE);
        failNextAllocation = true;
    }
    return status;
}
extern "C" aclError aclrtFree(void* address)
{
    using Function = aclError (*)(void*);
    static auto real = reinterpret_cast<Function>(dlsym(RTLD_NEXT, "aclrtFree"));
    if (!real) std::abort();
    const bool selected = address != nullptr &&
        address == __atomic_load_n(&selectedAllocation, __ATOMIC_ACQUIRE);
    const aclError status = real(address);
    if (selected && status == ACL_SUCCESS) __atomic_fetch_add(&freed, uint64_t(1), __ATOMIC_RELEASE);
    return status;
}
void* operator new(std::size_t bytes)
{
    if (failNextAllocation) {
        failNextAllocation = false;
        __atomic_fetch_add(&injected, uint64_t(1), __ATOMIC_RELEASE);
        throw std::bad_alloc();
    }
    if (void* result = std::malloc(bytes ? bytes : 1)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* address) noexcept { std::free(address); }
void operator delete[](void* address) noexcept { std::free(address); }
void operator delete(void* address, std::size_t) noexcept { std::free(address); }
void operator delete[](void* address, std::size_t) noexcept { std::free(address); }
extern "C" void TileXRGrowthFaultCounts(uint64_t* allocations, uint64_t* failures, uint64_t* frees)
{
    *allocations = __atomic_load_n(&armed, __ATOMIC_ACQUIRE);
    *failures = __atomic_load_n(&injected, __ATOMIC_ACQUIRE);
    *frees = __atomic_load_n(&freed, __ATOMIC_ACQUIRE);
}
#else
#include "tilexr_elastic_memory.h"
#include <runtime/kernel.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
using namespace TileXR::Elastic;
namespace {
void Check(int status, const char* operation)
{
    if (status) throw std::runtime_error(std::string(operation) + ": " + std::to_string(status));
}
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}
int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: TILEXR_TEST_GROWTH_FAULT_BYTES=1048593 LD_PRELOAD=... "
                     "test_elastic_growth_fault DEVICE KERNEL\n";
        return 2;
    }
    try {
        using Counts = void (*)(uint64_t*, uint64_t*, uint64_t*);
        auto counts = reinterpret_cast<Counts>(dlsym(RTLD_DEFAULT, "TileXRGrowthFaultCounts"));
        Require(counts != nullptr, "growth fault preload library is required");
        const char* selected = std::getenv("TILEXR_TEST_GROWTH_FAULT_BYTES");
        Require(selected && std::strtoull(selected, nullptr, 10) == 1048593,
                "set TILEXR_TEST_GROWTH_FAULT_BYTES=1048593");
        Check(aclInit(nullptr), "aclInit");
        Check(aclrtSetDevice(std::stoi(argv[1])), "aclrtSetDevice");
        Check(aclrtSetOpExecuteTimeOut(30), "aclrtSetOpExecuteTimeOut");
        aclrtStream input = nullptr;
        Check(aclrtCreateStream(&input), "aclrtCreateStream");
        void* side = nullptr;
        Check(aclrtMalloc(&side, 64, ACL_MEM_MALLOC_NORMAL_ONLY), "side allocation");
        Check(aclrtMemset(side, 64, 0, 64), "side clear");
        std::ifstream file(argv[2], std::ios::binary);
        std::vector<char> image((std::istreambuf_iterator<char>(file)), {});
        Require(!image.empty(), "kernel binary is empty");
        rtDevBinary_t binary{};
        binary.data = image.data(); binary.length = image.size(); binary.magic = 0x41415246;
        void* registered = nullptr;
        Check(rtDevBinaryRegister(&binary, &registered), "rtDevBinaryRegister");
        void* signature = reinterpret_cast<void*>(0x454d003000ULL);
        Check(rtFunctionRegister(registered, signature, "ElasticPoolTest", "ElasticPoolTest", 0),
              "rtFunctionRegister");
        Config config;
        config.segmentBytes = 1048593;
        config.maxCommittedBytes = 2 * config.segmentBytes;
        auto pool = Pool::Create(config);
        auto launch = [side, signature](DeviceView view, aclrtStream stream) {
            struct { uint64_t state, mailbox, seed; void* side; } args{view.state, view.mailbox, 7, side};
            rtArgsEx_t info{}; info.args = &args; info.argsSize = sizeof(args);
            rtTaskCfgInfo_t task{}; task.schemMode = 1;
            return int(rtKernelLaunchWithFlagV2(signature, 1, &info, nullptr, stream, 0, &task));
        };
        auto pending = pool->Submit(launch, input);
        std::string failure;
        try { pending->Wait(); }
        catch (const std::exception& error) { failure = error.what(); }
        Require(failure == "ElasticMemory Device call failed: 5", "expected Device RuntimeFailure response");
        const Stats stats = pool->Snapshot();
        Require(stats.committedBytes == 0 && stats.growthCount == 0 && stats.failureCount == 1,
                "unpublished allocation must not count as committed growth");
        bool rejected = false;
        try { pool->Submit(launch, input); } catch (const std::exception&) { rejected = true; }
        Require(rejected, "failed pool must reject later work");
        uint64_t sideEffects = 0;
        Check(aclrtMemcpy(&sideEffects, sizeof(sideEffects), side, sizeof(sideEffects), ACL_MEMCPY_DEVICE_TO_HOST),
              "side readback");
        Require(sideEffects == 1, "failed growth must not replay the kernel");
        uint64_t allocations = 0, failures = 0, frees = 0;
        counts(&allocations, &failures, &frees);
        Require(allocations == 1 && failures == 1 && frees == 1,
                "expected one successful HBM allocation, one Host fault and one real HBM free");
        pending.reset(); pool->Close(); pool.reset();
        Check(aclrtFree(side), "side free");
        Check(aclrtDestroyStream(input), "input stream destroy");
        Check(rtDevBinaryUnRegister(registered), "kernel unregister");
        Check(aclFinalize(), "aclFinalize");
        std::cout << "GROWTH_FAULT_PASS allocations=" << allocations << " injected=" << failures
                  << " frees=" << frees << " side_effect=" << sideEffects << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
#endif
