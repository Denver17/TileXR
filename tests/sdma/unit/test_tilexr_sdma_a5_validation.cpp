#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "sdma/tilexr_sdma_a5_cleanup.h"
#include "sdma/tilexr_sdma_a5_backend.h"

namespace {

int g_failures = 0;

#define CHECK_TRUE(expr) \
    do { \
        if (!(expr)) { \
            std::cerr << "CHECK_TRUE failed at line " << __LINE__ << ": " #expr << std::endl; \
            ++g_failures; \
        } \
    } while (0)

TileXR::detail::A5BuiltinChannelInfo ValidChannel()
{
    TileXR::detail::A5BuiltinChannelInfo channel {};
    channel.sqHead = 1U;
    channel.sqTail = 2U;
    channel.sqBase = 0x1000U;
    channel.sqRegisterBase = 0x2000U;
    channel.sqDepth = 8U;
    channel.sqId = 3U;
    channel.cqId = 4U;
    channel.logicalCqId = 5U;
    channel.streamId = 6U;
    channel.deviceId = 7U;
    return channel;
}

TileXR::detail::A5HostChannelIdentity ValidIdentity()
{
    return {6U, 3U, 4U, 5U, 7U};
}

void* Handle(uintptr_t value)
{
    return reinterpret_cast<void*>(value);
}

struct FakeCleanupRuntime {
    std::string failedOperation;
    void* failedHandle = nullptr;
    bool failureConsumed = false;
    std::vector<std::string> calls;

    int Call(const char* operation, void* handle)
    {
        calls.push_back(operation);
        if (!failureConsumed && failedOperation == operation &&
            failedHandle == handle) {
            failureConsumed = true;
            return 1;
        }
        return 0;
    }
};

int FakeSetContext(void* opaque, void* handle)
{
    return static_cast<FakeCleanupRuntime*>(opaque)->Call("set", handle);
}

int FakeDestroyStream(void* opaque, void* handle)
{
    return static_cast<FakeCleanupRuntime*>(opaque)->Call("stream", handle);
}

int FakeDestroyContext(void* opaque, void* handle)
{
    return static_cast<FakeCleanupRuntime*>(opaque)->Call("context", handle);
}

int FakeFreeDevice(void* opaque, void* handle)
{
    return static_cast<FakeCleanupRuntime*>(opaque)->Call("free", handle);
}

int FakeDestroyTensor(void* opaque, const void* handle)
{
    return static_cast<FakeCleanupRuntime*>(opaque)->Call(
        "tensor", const_cast<void*>(handle));
}

TileXR::detail::A5QueryCleanupOps FakeCleanupOps(FakeCleanupRuntime& runtime)
{
    TileXR::detail::A5QueryCleanupOps ops;
    ops.opaque = &runtime;
    ops.setCurrentContext = FakeSetContext;
    ops.destroyStream = FakeDestroyStream;
    ops.destroyContext = FakeDestroyContext;
    ops.freeDevice = FakeFreeDevice;
    ops.destroyTensor = FakeDestroyTensor;
    return ops;
}

TileXR::detail::A5PendingQueryCleanup FullCleanupState()
{
    TileXR::detail::A5PendingQueryCleanup state;
    state.ownerContext = Handle(1U);
    state.isolatedContext = Handle(2U);
    state.queryStream = Handle(3U);
    state.healthStream = Handle(4U);
    state.ownerBuffers = {Handle(5U), Handle(6U)};
    state.isolatedBuffers = {Handle(7U)};
    state.tensors = {Handle(8U), Handle(9U)};
    return state;
}

bool ContainsHandle(const TileXR::detail::A5PendingQueryCleanup& state,
                    void* handle)
{
    if (state.isolatedContext == handle || state.queryStream == handle ||
        state.healthStream == handle || state.restoreContext == handle) {
        return true;
    }
    for (void* buffer : state.ownerBuffers) {
        if (buffer == handle) {
            return true;
        }
    }
    for (void* buffer : state.isolatedBuffers) {
        if (buffer == handle) {
            return true;
        }
    }
    for (const void* tensor : state.tensors) {
        if (tensor == handle) {
            return true;
        }
    }
    return false;
}

void TestCompleteAndExpectedPartialClassification()
{
    using namespace TileXR::detail;
    A5BuiltinChannelInfo channel = ValidChannel();
    const A5HostChannelIdentity identity = ValidIdentity();
    CHECK_TRUE(ClassifyA5QueryResult(0, 1U, 48U, channel, identity) ==
               A5QueryResultKind::COMPLETE);
    channel.sqRegisterBase = 0U;
    CHECK_TRUE(ClassifyA5QueryResult(TILEXR_SDMA_A5_EXPECTED_QUERY_STATUS,
                                    0U, 0U, channel, identity) ==
               A5QueryResultKind::EXPECTED_PARTIAL);
}

void TestPartialClassificationFailsClosed()
{
    using namespace TileXR::detail;
    A5BuiltinChannelInfo channel = ValidChannel();
    channel.sqRegisterBase = 0U;
    const A5HostChannelIdentity identity = ValidIdentity();
    CHECK_TRUE(ClassifyA5QueryResult(507019, 0U, 0U, channel, identity) ==
               A5QueryResultKind::INVALID);
    CHECK_TRUE(ClassifyA5QueryResult(TILEXR_SDMA_A5_EXPECTED_QUERY_STATUS,
                                    1U, 0U, channel, identity) ==
               A5QueryResultKind::INVALID);
    channel.sqTail = channel.sqDepth;
    CHECK_TRUE(ClassifyA5QueryResult(TILEXR_SDMA_A5_EXPECTED_QUERY_STATUS,
                                    0U, 0U, channel, identity) ==
               A5QueryResultKind::INVALID);
    channel = ValidChannel();
    channel.sqRegisterBase = 0U;
    channel.streamId += 1U;
    CHECK_TRUE(ClassifyA5QueryResult(TILEXR_SDMA_A5_EXPECTED_QUERY_STATUS,
                                    0U, 0U, channel, identity) ==
               A5QueryResultKind::INVALID);
    channel = ValidChannel();
    channel.sqHead = 0U;
    channel.sqTail = channel.sqDepth - 1U;
    CHECK_TRUE(ClassifyA5QueryResult(0, 1U, TILEXR_SDMA_A5_CHANNEL_COUNT,
                                    channel, identity) == A5QueryResultKind::INVALID);
}

void TestCompleteClassificationRequiresFinishedHeader()
{
    using namespace TileXR::detail;
    const A5BuiltinChannelInfo channel = ValidChannel();
    const A5HostChannelIdentity identity = ValidIdentity();
    CHECK_TRUE(ClassifyA5QueryResult(0, 0U, TILEXR_SDMA_A5_CHANNEL_COUNT,
                                    channel, identity) == A5QueryResultKind::INVALID);
    CHECK_TRUE(ClassifyA5QueryResult(0, 1U, TILEXR_SDMA_A5_CHANNEL_COUNT - 1U,
                                    channel, identity) == A5QueryResultKind::INVALID);
    CHECK_TRUE(ClassifyA5QueryResult(0, 1U, 0U, channel, identity) ==
               A5QueryResultKind::INVALID);
}

void TestGroupedCopyWaitIsBounded()
{
    CHECK_TRUE(TileXR::detail::TILEXR_SDMA_A5_WAIT_TIMEOUT_CYCLES != 0ULL);
}

void TestSqPrewarmPageLayout()
{
    using namespace TileXR::detail;
    CHECK_TRUE(A5SdmaSqBytes(2049U) == 131136ULL);
    CHECK_TRUE(A5SdmaSqPageCount(2049U) == 33U);
    CHECK_TRUE(A5SdmaSqPageOffset(0U) == 0ULL);
    CHECK_TRUE(A5SdmaSqPageOffset(7U) == 28672ULL);
    CHECK_TRUE(A5SdmaSqPageOffset(32U) + TILEXR_SDMA_A5_PREWARM_BYTES ==
               A5SdmaSqBytes(2049U));
}

void TestCleanupFailuresRetainHandlesForRetry()
{
    struct FailureCase {
        const char* operation;
        void* handle;
    };
    const std::vector<FailureCase> failures = {
        {"set", Handle(2U)},
        {"stream", Handle(4U)},
        {"stream", Handle(3U)},
        {"free", Handle(7U)},
        {"tensor", Handle(9U)},
        {"context", Handle(2U)},
        {"free", Handle(6U)},
    };
    for (const FailureCase& failure : failures) {
        FakeCleanupRuntime runtime;
        runtime.failedOperation = failure.operation;
        runtime.failedHandle = failure.handle;
        TileXR::detail::A5PendingQueryCleanup state = FullCleanupState();
        const TileXR::detail::A5QueryCleanupOps ops = FakeCleanupOps(runtime);
        CHECK_TRUE(!TileXR::detail::CleanupA5QueryResources(
            state, ops, state.ownerContext));
        CHECK_TRUE(runtime.failureConsumed);
        CHECK_TRUE(ContainsHandle(state, failure.handle));
        CHECK_TRUE(!state.Empty());

        CHECK_TRUE(TileXR::detail::CleanupA5QueryResources(
            state, ops, state.ownerContext));
        CHECK_TRUE(state.Empty());
    }
}

void TestCleanupRestoreFailureIsRetryable()
{
    FakeCleanupRuntime runtime;
    runtime.failedOperation = "set";
    runtime.failedHandle = Handle(1U);
    TileXR::detail::A5PendingQueryCleanup state;
    state.ownerContext = Handle(1U);
    state.isolatedContext = Handle(2U);
    state.queryStream = Handle(3U);
    const TileXR::detail::A5QueryCleanupOps ops = FakeCleanupOps(runtime);

    CHECK_TRUE(!TileXR::detail::CleanupA5QueryResources(
        state, ops, state.ownerContext));
    CHECK_TRUE(state.restorePending);
    CHECK_TRUE(state.restoreContext == state.ownerContext);
    CHECK_TRUE(!state.Empty());

    CHECK_TRUE(TileXR::detail::CleanupA5QueryResources(
        state, ops, state.restoreContext));
    CHECK_TRUE(state.Empty());
}

} // namespace

int main()
{
    TestCompleteAndExpectedPartialClassification();
    TestPartialClassificationFailsClosed();
    TestCompleteClassificationRequiresFinishedHeader();
    TestGroupedCopyWaitIsBounded();
    TestSqPrewarmPageLayout();
    TestCleanupFailuresRetainHandlesForRetry();
    TestCleanupRestoreFailureIsRetryable();
    if (g_failures != 0) {
        std::cerr << g_failures << " A5 SDMA validation checks failed" << std::endl;
        return 1;
    }
    std::cout << "TileXR A5 SDMA validation checks passed" << std::endl;
    return 0;
}
