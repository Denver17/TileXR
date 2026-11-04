#include "rdma/tilexr_rdma_transport.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

using namespace TileXR;
namespace {
constexpr int RANKS = 3;
struct Gather {
    std::mutex mutex;
    std::condition_variable cv;
    int generation = 0;
    int arrived = 0;
    size_t size = 0;
    bool mismatch = false;
    bool corruptEndpoint = false;
    int failExchangeRank = -1;
    size_t failExchangeBytes = 40;
    std::array<std::vector<uint8_t>, RANKS> send;
    std::array<void*, RANKS> dest {};
    int Run(int rank, const void* input, size_t bytes, void* output)
    {
        std::unique_lock<std::mutex> lock(mutex);
        const int current = generation;
        if (arrived == 0) { size = bytes; mismatch = false; }
        mismatch |= size != bytes;
        send[rank].assign(static_cast<const uint8_t*>(input), static_cast<const uint8_t*>(input) + bytes);
        dest[rank] = output;
        if (++arrived == RANKS) {
            assert(!mismatch);
            if (corruptEndpoint && bytes == 48 * RANKS) { send[1][3] = 99; }
            for (int target = 0; target < RANKS; ++target) {
                for (int source = 0; source < RANKS; ++source) {
                    std::memcpy(static_cast<uint8_t*>(dest[target]) + source * bytes, send[source].data(), bytes);
                }
            }
            arrived = 0;
            ++generation;
            cv.notify_all();
        } else {
            assert(cv.wait_for(lock, std::chrono::seconds(3), [&] { return generation != current; }));
        }
        // A successful exchange can still report an error to only one caller.
        // All ranks must agree on that outcome before exposing any MR.
        return bytes == failExchangeBytes && rank == failExchangeRank ? TILEXR_ERROR_INTERNAL : TILEXR_SUCCESS;
    }
};
struct DriverState {
    int openFailure = 0;
    int createFailure = 0;
    int connectFailure = 0;
    // Successful operations to allow before returning the injected failure.
    int createFailureAfter = 0;
    int connectFailureAfter = 0;
    int registerFailure = 0;
    int deregisterFailures = 0;
    int queueDestroyFailures = 0;
    int closeFailures = 0;
    int transferFailure = 0;
    int queueCount = 0;
    int mrCount = 0;
    int connections = 0;
    int transfers = 0;
    bool closed = false;
    bool direct = false;
    int exportFailure = -1;
    bool aliasQueueState = false;
};
struct MockProvider final : RdmaProvider {
    DriverState& state;
    int rank;
    explicit MockProvider(DriverState& s, int r) : state(s), rank(r) {}
    int Open(const RdmaConfig&) override { return state.openFailure; }
    bool DeviceDirect() const override { return state.direct; }
    int DeviceQueue(int peer, TileXRRDMADeviceQueue& out) override
    {
        if (!state.direct) { return TILEXR_ERROR_NOT_SUPPORT; }
        if (peer == state.exportFailure) { return TILEXR_ERROR_INTERNAL; }
        out = TileXRRDMADeviceQueue {};
        out.state = 0x1000U + (state.aliasQueueState ? 0U : static_cast<uint32_t>(peer) * 64);
        out.qpn = static_cast<uint32_t>(rank * 10 + peer + 1);
        return TILEXR_SUCCESS;
    }
    int Create(int peer, RdmaEndpoint& out) override
    {
        if (state.createFailure) {
            if (state.createFailureAfter == 0) { return state.createFailure; }
            --state.createFailureAfter;
        }
        ++state.queueCount;
        out.gid[15] = static_cast<uint8_t>(rank + 1);
        out.qpn = static_cast<uint32_t>(rank * 10 + peer + 1);
        out.psn = static_cast<uint32_t>(rank * 100 + peer);
        out.mtu = 1024;
        return TILEXR_SUCCESS;
    }
    int Connect(int peer, const RdmaEndpoint& remote) override
    {
        const int rankPeer = peer % RANKS;
        const int qpOffset = peer - rankPeer;
        assert(remote.qpn == static_cast<uint32_t>(rankPeer * 10 + rank + qpOffset + 1));
        assert(remote.psn == static_cast<uint32_t>(rankPeer * 100 + rank + qpOffset));
        assert(remote.gid[15] == rankPeer + 1);
        if (state.connectFailure) {
            if (state.connectFailureAfter == 0) { return state.connectFailure; }
            --state.connectFailureAfter;
        }
        ++state.connections;
        return TILEXR_SUCCESS;
    }
    int Register(void* ptr, size_t bytes, RdmaMemory& out) override
    {
        if (state.registerFailure) { return state.registerFailure; }
        ++state.mrCount;
        out.address = reinterpret_cast<uintptr_t>(ptr);
        out.bytes = bytes;
        out.lkey = 20 + rank;
        out.rkey = 30 + rank;
        out.resource = ptr;
        return TILEXR_SUCCESS;
    }
    int Deregister(RdmaMemory& memory) override
    {
        if (!memory.resource) { return TILEXR_SUCCESS; }
        if (state.deregisterFailures > 0) { --state.deregisterFailures; return TILEXR_ERROR_INTERNAL; }
        --state.mrCount;
        memory = RdmaMemory {};
        return TILEXR_SUCCESS;
    }
    int Transfer(bool read, int peer, const RdmaMemory& local, uint64_t localOffset,
        const RdmaMemory& remote, uint64_t remoteOffset, uint32_t bytes, uint32_t) override
    {
        ++state.transfers;
        if (state.transferFailure) { return state.transferFailure; }
        assert(local.lkey == static_cast<uint32_t>(20 + rank));
        assert(remote.rkey == static_cast<uint32_t>(30 + peer));
        void* src = reinterpret_cast<void*>(read ? remote.address + remoteOffset : local.address + localOffset);
        void* dst = reinterpret_cast<void*>(read ? local.address + localOffset : remote.address + remoteOffset);
        std::memcpy(dst, src, bytes);
        return TILEXR_SUCCESS;
    }
    int DestroyQueues() override
    {
        if (state.queueDestroyFailures > 0) { --state.queueDestroyFailures; return TILEXR_ERROR_INTERNAL; }
        state.queueCount = 0;
        return TILEXR_SUCCESS;
    }
    int Close() override
    {
        assert(state.queueCount == 0 && state.mrCount == 0);
        if (state.closeFailures > 0) { --state.closeFailures; return TILEXR_ERROR_INTERNAL; }
        state.closed = true;
        return TILEXR_SUCCESS;
    }
};
struct Fixture {
    Gather gather;
    std::array<DriverState, RANKS> states {};
    std::array<std::unique_ptr<TileXRRDMATransport>, RANKS> transports;
    std::array<std::array<uint8_t, 128>, RANKS> buffers {};
    std::array<TileXRRDMAMemHandle, RANKS> handles {};
    Fixture()
    {
        for (int rank = 0; rank < RANKS; ++rank) {
            transports[rank].reset(new TileXRRDMATransport(
                std::unique_ptr<RdmaProvider>(new MockProvider(states[rank], rank))));
        }
    }
    template <class F> void Together(F function, int expected = TILEXR_SUCCESS)
    {
        std::array<int, RANKS> results {};
        std::vector<std::thread> threads;
        for (int rank = 0; rank < RANKS; ++rank) {
            threads.emplace_back([&, rank] { results[rank] = function(rank); });
        }
        for (auto& thread : threads) { thread.join(); }
        for (int result : results) { assert(result == expected); }
    }
    void Init(int expected = TILEXR_SUCCESS, int directMask = 0)
    {
        Together([&](int rank) {
            RdmaOptions options;
            options.rank = rank;
            options.rankSize = RANKS;
            options.config.aivDirect = (directMask & (1 << rank)) != 0;
            options.allGather = [&, rank](const void* input, size_t bytes, void* output) {
                return gather.Run(rank, input, bytes, output);
            };
            return transports[rank]->Init(options);
        }, expected);
    }
    void Register(int expected = TILEXR_SUCCESS)
    {
        Together([&](int rank) {
            return transports[rank]->Register(buffers[rank].data(), buffers[rank].size() - rank, &handles[rank]);
        }, expected);
    }
    void Shutdown(int expected = TILEXR_SUCCESS)
    {
        Together([&](int rank) { return transports[rank]->Shutdown(); }, expected);
    }
};

void SuccessAndBounds()
{
    Fixture f;
    f.Init();
    f.Register();
    auto& transport = *f.transports[0];
    const auto info = transport.Query();
    assert(info.connected && info.hostDriven && !info.deviceDirect && info.peerCount == 2);
    assert(info.registrationCount == 1);
    TileXRRDMADeviceView unsupported {};
    assert(transport.DeviceView(f.handles[0], 1, &unsupported) == TILEXR_ERROR_NOT_SUPPORT);
    f.buffers[0][4] = 99;
    assert(transport.Transfer(false, f.handles[0], 2, 4, 8, 1) == 0);
    assert(f.buffers[2][8] == 99);
    f.buffers[1][12] = 77;
    assert(transport.Transfer(true, f.handles[0], 1, 15, 12, 1) == 0);
    assert(f.buffers[0][15] == 77);
    assert(transport.Transfer(false, f.handles[0], 2, 128, 0, 1) == TILEXR_ERROR_PARA_CHECK_FAIL);
    assert(transport.Transfer(false, f.handles[0], 2, 0, 126, 1) == TILEXR_ERROR_PARA_CHECK_FAIL);
    assert(transport.Transfer(false, f.handles[0], 2, SIZE_MAX, 0, 2) == TILEXR_ERROR_PARA_CHECK_FAIL);
    assert(transport.Transfer(false, f.handles[0], 0, 0, 0, 1) == TILEXR_ERROR_PARA_CHECK_FAIL);
    assert(transport.Transfer(false, 999, 1, 0, 0, 1) == TILEXR_ERROR_PARA_CHECK_FAIL);
    assert(f.states[0].transfers == 2);
    const auto previous = f.handles;
    f.Together([&](int rank) { return f.transports[rank]->Unregister(f.handles[rank]); });
    assert(transport.Transfer(false, previous[0], 1, 0, 0, 1) == TILEXR_ERROR_PARA_CHECK_FAIL);
    f.Register();
    assert(f.handles[0] > previous[0]);
    f.Shutdown();
    assert(!transport.Query().connected);
}

void MultipleQps()
{
    Fixture f;
    for (auto& state : f.states) { state.direct = true; }
    f.Init(TILEXR_SUCCESS, 7);
    for (uint32_t count : {1U, 2U, 4U, 8U, 16U}) {
        f.Together([&](int rank) { return f.transports[rank]->ConfigureQps(count); });
        for (const auto& state : f.states) { assert(state.queueCount == int(count) * (RANKS - 1)); }
    }
    f.Register();
    for (int rank = 0; rank < RANKS; ++rank) {
        std::array<TileXRRDMADeviceView, TILEXR_RDMA_MAX_QPS> views {};
        const int peer = (rank + 1) % RANKS;
        auto& transport = *f.transports[rank];
        assert(transport.DeviceViews(f.handles[rank], peer, views.data(), views.size()) == 0);
        TileXRRDMADeviceView first {};
        assert(transport.DeviceView(f.handles[rank], peer, &first) == 0);
        assert(first.queue.qpn == views[0].queue.qpn);
        for (uint32_t qp = 0; qp < views.size(); ++qp) {
            assert(views[qp].localAddress == reinterpret_cast<uintptr_t>(f.buffers[rank].data()));
            assert(views[qp].remoteAddress == reinterpret_cast<uintptr_t>(f.buffers[peer].data()));
            assert(views[qp].localKey == uint32_t(20 + rank));
            assert(views[qp].remoteKey == uint32_t(30 + peer));
            assert(views[qp].queue.qpn == uint32_t(rank * 10 + peer + qp * RANKS + 1));
            if (qp) { assert(views[qp].queue.state != views[qp - 1].queue.state); }
        }
        assert(transport.DeviceViews(f.handles[rank], peer, views.data(), 2) == TILEXR_ERROR_PARA_CHECK_FAIL);
        assert(views[0].version == 0 && views[1].queue.state == 0);
        assert(transport.DeviceViews(f.handles[rank], peer, nullptr, 16) == TILEXR_ERROR_PARA_CHECK_FAIL);
        assert(transport.DeviceViews(f.handles[rank], peer, views.data(), 0) == TILEXR_ERROR_PARA_CHECK_FAIL);
        f.states[rank].exportFailure = peer + 8 * RANKS;
        assert(transport.DeviceViews(f.handles[rank], peer, views.data(), 16) == TILEXR_ERROR_INTERNAL);
        for (const auto& view : views) { assert(view.version == 0); }
        f.states[rank].exportFailure = -1;
        f.states[rank].aliasQueueState = true;
        assert(transport.DeviceViews(f.handles[rank], peer, views.data(), 16) == TILEXR_ERROR_INTERNAL);
        f.states[rank].aliasQueueState = false;
    }
    f.Together([&](int rank) { return f.transports[rank]->ConfigureQps(16); }, TILEXR_ERROR_PARA_CHECK_FAIL);
    f.Together([&](int rank) { return f.transports[rank]->Unregister(f.handles[rank]); });
    f.Shutdown();
    for (const auto& state : f.states) { assert(state.closed && state.mrCount == 0 && state.queueCount == 0); }
}

void MultipleQpFailures()
{
    Fixture host;
    host.Init();
    host.Together([&](int rank) { return host.transports[rank]->ConfigureQps(2); }, TILEXR_ERROR_NOT_SUPPORT);
    host.Shutdown();
    Fixture f;
    for (auto& state : f.states) { state.direct = true; }
    f.Init(TILEXR_SUCCESS, 7);
    for (uint32_t count : {0U, 17U}) {
        f.Together([&](int rank) { return f.transports[rank]->ConfigureQps(count); }, TILEXR_ERROR_PARA_CHECK_FAIL);
    }
    f.Together([&](int rank) { return f.transports[rank]->ConfigureQps(rank == 0 ? 4 : 2); }, TILEXR_ERROR_PARA_CHECK_FAIL);
    for (const auto& state : f.states) { assert(state.queueCount == RANKS - 1); }
    f.Together([&](int rank) { return f.transports[rank]->ConfigureQps(2); });
    f.Together([&](int rank) { return f.transports[rank]->ConfigureQps(1); }, TILEXR_ERROR_PARA_CHECK_FAIL);
    f.Shutdown();
    for (bool connectFailure : {false, true}) {
        Fixture broken;
        for (auto& state : broken.states) { state.direct = true; }
        broken.Init(TILEXR_SUCCESS, 7);
        if (connectFailure) { broken.states[1].connectFailure = TILEXR_ERROR_INTERNAL; }
        else { broken.states[1].createFailure = TILEXR_ERROR_INTERNAL; }
        broken.Together([&](int rank) { return broken.transports[rank]->ConfigureQps(4); }, TILEXR_ERROR_INTERNAL);
        for (const auto& transport : broken.transports) { assert(transport->Query().failed); }
        broken.Register(TILEXR_ERROR_NOT_INITIALIZED);
        broken.Shutdown();
        for (const auto& state : broken.states) { assert(state.closed && state.queueCount == 0); }
    }
}

void DelayedMultipleQpFailures()
{
    for (uint32_t existingQps : {1U, 2U}) {
        for (bool connectFailure : {false, true}) {
            Fixture f;
            for (auto& state : f.states) { state.direct = true; }
            f.Init(TILEXR_SUCCESS, 7);
            if (existingQps > 1) {
                f.Together([&](int rank) { return f.transports[rank]->ConfigureQps(existingQps); });
            }
            const int peers = RANKS - 1;
            const int originalQueues = int(existingQps) * peers;
            for (const auto& state : f.states) {
                assert(state.queueCount == originalQueues && state.connections == originalQueues);
            }
            // Complete one added QP to every peer, then fail the second peer
            // of the next QP. Existing and newly connected queues must survive.
            auto& fault = f.states[1];
            if (connectFailure) {
                fault.connectFailure = TILEXR_ERROR_INTERNAL;
                fault.connectFailureAfter = peers + 1;
            } else {
                fault.createFailure = TILEXR_ERROR_INTERNAL;
                fault.createFailureAfter = peers + 1;
            }
            f.Together([&](int rank) { return f.transports[rank]->ConfigureQps(4); }, TILEXR_ERROR_INTERNAL);
            std::array<int, RANKS> retained {};
            for (int rank = 0; rank < RANKS; ++rank) {
                const auto& state = f.states[rank];
                assert(f.transports[rank]->Query().failed);
                retained[rank] = state.queueCount;
                assert(state.queueCount >= originalQueues + peers);
                assert(state.connections >= originalQueues + peers);
            }
            assert(fault.queueCount == originalQueues + peers + (connectFailure ? peers : 1));
            assert(fault.connections == originalQueues + peers + (connectFailure ? 1 : 0));
            // Removing the injected fault must not unpoison any rank.
            fault.createFailure = fault.connectFailure = 0;
            f.Register(TILEXR_ERROR_NOT_INITIALIZED);
            f.Together([&](int rank) { return f.transports[rank]->ConfigureQps(4); }, TILEXR_ERROR_NOT_INITIALIZED);
            for (int rank = 0; rank < RANKS; ++rank) {
                assert(f.handles[rank] == 0 && f.states[rank].mrCount == 0);
                assert(f.states[rank].queueCount == retained[rank]);
            }
            f.states[2].queueDestroyFailures = 1;
            f.states[0].closeFailures = 1;
            f.Shutdown(TILEXR_ERROR_INTERNAL);
            for (int rank = 0; rank < RANKS; ++rank) {
                assert(!f.states[rank].closed && !f.transports[rank]->Query().connected);
                assert(f.states[rank].queueCount == (rank == 2 ? retained[rank] : 0));
            }
            f.Shutdown(TILEXR_ERROR_INTERNAL);
            for (const auto& state : f.states) { assert(state.queueCount == 0); }
            assert(!f.states[0].closed);
            f.Shutdown();
            for (const auto& state : f.states) {
                assert(state.closed && state.queueCount == 0 && state.mrCount == 0);
            }
        }
    }
}

void FailedRanks()
{
    for (int phase = 0; phase < 3; ++phase) {
        Fixture f;
        auto& state = f.states[1];
        if (phase == 0) { state.openFailure = TILEXR_ERROR_NOT_FOUND; }
        if (phase == 1) { state.createFailure = TILEXR_ERROR_INTERNAL; }
        if (phase == 2) { state.connectFailure = TILEXR_ERROR_INTERNAL; }
        f.Init(phase == 0 ? TILEXR_ERROR_NOT_FOUND : TILEXR_ERROR_INTERNAL);
        for (const auto& transport : f.transports) { assert(!transport->Query().connected); }
        f.Shutdown();
        for (const auto& other : f.states) { assert(other.closed); }
    }
    Fixture invalidWire;
    invalidWire.gather.corruptEndpoint = true;
    invalidWire.Init(TILEXR_ERROR_PARA_CHECK_FAIL);
    invalidWire.Shutdown();
}
void RegistrationAndCleanupFailures()
{
    Fixture f;
    f.Init();
    f.Together([&](int rank) { return f.transports[rank]->Register(
        rank == 1 ? nullptr : f.buffers[rank].data(), 8, &f.handles[rank]); },
        TILEXR_ERROR_PARA_CHECK_FAIL);
    f.states[1].registerFailure = TILEXR_ERROR_INTERNAL;
    f.Register(TILEXR_ERROR_INTERNAL);
    for (int rank = 0; rank < RANKS; ++rank) {
        assert(f.states[rank].mrCount == 0 && f.handles[rank] == 0);
    }
    f.states[1].registerFailure = 0;
    f.Register();
    auto invalid = f.handles;
    f.Together([&](int rank) { return f.transports[rank]->Register(
        f.buffers[rank].data(), 8, &invalid[rank]); }, TILEXR_ERROR_PARA_CHECK_FAIL);
    f.states[2].deregisterFailures = 1;
    f.Together([&](int rank) { return f.transports[rank]->Unregister(f.handles[rank]); }, TILEXR_ERROR_INTERNAL);
    f.Together([&](int rank) { return f.transports[rank]->Register(
        f.buffers[rank].data(), 8, &invalid[rank]); }, TILEXR_ERROR_NOT_INITIALIZED);
    for (int rank = 0; rank < RANKS; ++rank) {
        assert(f.transports[rank]->Transfer(false, f.handles[rank], (rank + 1) % RANKS, 0, 0, 1) ==
            TILEXR_ERROR_PARA_CHECK_FAIL);
    }
    f.Together([&](int rank) { return f.transports[rank]->Unregister(f.handles[rank]); });
    f.Shutdown();
}
void TimeoutAndDestroyRetry()
{
    Fixture f;
    f.Init();
    f.Register();
    f.states[0].transferFailure = TILEXR_ERROR_TIMEOUT;
    assert(f.transports[0]->Transfer(false, f.handles[0], 1, 0, 0, 1) == TILEXR_ERROR_TIMEOUT);
    assert(f.transports[0]->Query().failed);
    assert(f.transports[0]->Transfer(false, f.handles[0], 1, 0, 0, 1) == TILEXR_ERROR_NOT_INITIALIZED);
    f.Together([&](int rank) { return f.transports[rank]->Unregister(f.handles[rank]); },
        TILEXR_ERROR_NOT_INITIALIZED);
    f.states[1].queueDestroyFailures = 1;
    f.Shutdown(TILEXR_ERROR_INTERNAL);
    for (const auto& state : f.states) { assert(state.mrCount == 1); }
    f.states[2].deregisterFailures = 1;
    f.Shutdown(TILEXR_ERROR_INTERNAL);
    assert(f.states[2].mrCount == 1);
    f.states[1].closeFailures = 1;
    f.Shutdown(TILEXR_ERROR_INTERNAL);
    f.Shutdown();
}
void RegistrationRollbackFailure()
{
    Fixture f;
    f.Init();
    f.states[1].registerFailure = TILEXR_ERROR_INTERNAL;
    f.states[2].deregisterFailures = 1;
    f.Register(TILEXR_ERROR_INTERNAL);
    for (int rank = 0; rank < RANKS; ++rank) {
        assert(f.handles[rank] == 0 && f.transports[rank]->Query().failed);
        assert(f.transports[rank]->Query().registrationCount == 0); // No usable handle was published.
    }
    assert(f.states[0].mrCount == 0 && f.states[1].mrCount == 0 && f.states[2].mrCount == 1);
    f.Register(TILEXR_ERROR_NOT_INITIALIZED);
    f.states[0].queueDestroyFailures = 1;
    f.Shutdown(TILEXR_ERROR_INTERNAL);
    assert(f.states[2].mrCount == 1); // Retain the allocation until every rank's QPs are gone.
    f.Shutdown();
    for (const auto& state : f.states) { assert(state.closed && state.mrCount == 0); }
}
void Config()
{
    for (const char* name : {"DEVICE", "PORT", "GID_INDEX", "MTU", "TRAFFIC_CLASS", "HOP_LIMIT", "TIMEOUT_MS", "DMA_MODE", "MODE", "DB_MODE"}) {
        unsetenv((std::string("TILEXR_RDMA_") + name).c_str());
    }
    RdmaConfig config;
    std::string error;
    unsetenv("TILEXR_RDMA_DEVICE");
    assert(LoadRdmaConfig(config, error) != 0);
    setenv("TILEXR_RDMA_DEVICE", "hrn5_0", 1);
    assert(LoadRdmaConfig(config, error) == 0 && config.indepUb);
    setenv("TILEXR_RDMA_MTU", "512", 1);
    assert(LoadRdmaConfig(config, error) == 0 && config.mtu == 512);
    setenv("TILEXR_RDMA_MTU", "513", 1);
    assert(LoadRdmaConfig(config, error) != 0);
    unsetenv("TILEXR_RDMA_MTU");
    for (const char* value : {"-1", "+1", "", "1garbage", "256", "9999999999999999999999"}) {
        setenv("TILEXR_RDMA_GID_INDEX", value, 1);
        assert(LoadRdmaConfig(config, error) != 0);
    }
    unsetenv("TILEXR_RDMA_GID_INDEX");
    setenv("TILEXR_RDMA_MODE", "aiv", 1);
    assert(LoadRdmaConfig(config, error) != 0); // No guessed firmware doorbell mode.
    setenv("TILEXR_RDMA_DB_MODE", "hard", 1);
    assert(LoadRdmaConfig(config, error) == 0 && config.aivDirect && config.doorbellMode == 2);
    setenv("TILEXR_RDMA_DB_MODE", "soft", 1);
    assert(LoadRdmaConfig(config, error) == 0 && config.doorbellMode == 1);
    setenv("TILEXR_RDMA_DMA_MODE", "default", 1);
    assert(LoadRdmaConfig(config, error) != 0);
    unsetenv("TILEXR_RDMA_DMA_MODE");
    setenv("TILEXR_RDMA_DB_MODE", "auto", 1);
    assert(LoadRdmaConfig(config, error) != 0);
    unsetenv("TILEXR_RDMA_DB_MODE");
    setenv("TILEXR_RDMA_MODE", "guess", 1);
    assert(LoadRdmaConfig(config, error) != 0);
    unsetenv("TILEXR_RDMA_MODE");
    setenv("TILEXR_RDMA_DEVICE", "../../bad", 1);
    assert(LoadRdmaConfig(config, error) != 0);
    unsetenv("TILEXR_RDMA_DEVICE");
}

void DeviceViewsAndModeAgreement()
{
    Fixture mismatch;
    mismatch.Init(TILEXR_ERROR_PARA_CHECK_FAIL, 1);
    mismatch.Shutdown();
    Fixture noFallback;
    noFallback.Init(TILEXR_ERROR_NOT_SUPPORT, 7);
    noFallback.Shutdown();

    Fixture f;
    for (auto& state : f.states) { state.direct = true; }
    f.Init(TILEXR_SUCCESS, 7);
    f.Register();
    TileXRRDMADeviceView first {}, second {};
    auto& transport = *f.transports[0];
    assert(transport.Query().deviceDirect && !transport.Query().hostDriven);
    assert(transport.DeviceView(f.handles[0], 1, &first) == 0);
    assert(first.version == TILEXR_RDMA_DEVICE_VERSION && first.structBytes == sizeof(first));
    assert(first.localAddress == reinterpret_cast<uintptr_t>(f.buffers[0].data()));
    assert(first.remoteAddress == reinterpret_cast<uintptr_t>(f.buffers[1].data()));
    assert(first.localKey == 20 && first.remoteKey == 31);
    assert(first.remoteBytes == f.buffers[1].size() - 1);
    assert(transport.Transfer(false, f.handles[0], 1, 0, 0, 1) == TILEXR_ERROR_NOT_SUPPORT);
    assert(!transport.Query().failed && f.states[0].transfers == 0);
    std::array<std::array<uint8_t, 64>, RANKS> other {};
    std::array<TileXRRDMAMemHandle, RANKS> handles {};
    f.Together([&](int rank) {
        return f.transports[rank]->Register(other[rank].data(), other[rank].size(), &handles[rank]);
    });
    assert(transport.DeviceView(handles[0], 1, &second) == 0);
    assert(first.queue.state == second.queue.state && first.queue.qpn == second.queue.qpn);
    assert(first.localAddress != second.localAddress && first.remoteAddress != second.remoteAddress);
    assert(transport.DeviceView(handles[0], 0, &second) == TILEXR_ERROR_PARA_CHECK_FAIL);
    assert(second.version == 0 && second.queue.state == 0);
    assert(transport.DeviceView(handles[0], 1, nullptr) == TILEXR_ERROR_PARA_CHECK_FAIL);
    f.Together([&](int rank) { return f.transports[rank]->Unregister(f.handles[rank]); });
    assert(transport.DeviceView(f.handles[0], 1, &first) == TILEXR_ERROR_PARA_CHECK_FAIL);
    assert(first.version == 0);
    assert(transport.DeviceView(handles[0], 1, &second) == 0);
    f.Shutdown();
    assert(transport.DeviceView(handles[0], 1, &second) == TILEXR_ERROR_NOT_INITIALIZED);
}

void AsymmetricMemoryExchangeFailure()
{
    Fixture f;
    f.Init();
    f.gather.failExchangeRank = 1;
    f.Register(TILEXR_ERROR_INTERNAL);
    const int generation = f.gather.generation;
    for (int rank = 0; rank < RANKS; ++rank) {
        assert(f.handles[rank] == 0 && f.transports[rank]->Query().failed);
        assert(f.states[rank].mrCount == 1); // Retain unpublished MRs on a broken control channel.
        assert(f.transports[rank]->Transfer(false, 1, (rank + 1) % RANKS, 0, 0, 1) ==
            TILEXR_ERROR_NOT_INITIALIZED);
    }
    f.Register(TILEXR_ERROR_NOT_INITIALIZED);
    f.Together([&](int rank) { return f.transports[rank]->Unregister(1); }, TILEXR_ERROR_NOT_INITIALIZED);
    f.Shutdown(TILEXR_ERROR_NOT_INITIALIZED);
    for (const auto& state : f.states) { assert(state.queueCount == 0 && state.mrCount == 1 && !state.closed); }
    assert(f.gather.generation == generation); // Never reuse the potentially misframed bootstrap.
    Fixture unregisterFailure;
    unregisterFailure.Init();
    unregisterFailure.Register();
    unregisterFailure.gather.failExchangeRank = 1;
    unregisterFailure.gather.failExchangeBytes = 8;
    unregisterFailure.Together([&](int rank) {
        return unregisterFailure.transports[rank]->Unregister(unregisterFailure.handles[rank]);
    }, TILEXR_ERROR_INTERNAL);
    for (int rank = 0; rank < RANKS; ++rank) {
        assert(unregisterFailure.transports[rank]->Query().failed && unregisterFailure.states[rank].mrCount == 1);
    }
    unregisterFailure.Shutdown(TILEXR_ERROR_NOT_INITIALIZED);

}

void LocalExchangeErrorCannotBeAcknowledgedByStaleZero()
{
    DriverState state;
    TileXRRDMATransport transport(std::unique_ptr<RdmaProvider>(new MockProvider(state, 0)));
    RdmaOptions options;
    options.rank = 0;
    options.rankSize = 2;
    bool failMemory = false;
    bool memoryExchangeFailed = false;
    options.allGather = [&](const void* input, size_t bytes, void* output) {
        auto* out = static_cast<uint8_t*>(output);
        std::memcpy(out, input, bytes);
        std::memcpy(out + bytes, input, bytes);
        if (bytes == 96) {
            // Peer 1's endpoint addressed to rank 0 in the two-rank matrix.
            auto* endpoint = out + 96;
            std::memset(endpoint, 0, 48);
            auto be = [&](size_t offset, uint32_t value) {
                for (size_t i = 0; i < 4; ++i) { endpoint[offset + 3 - i] = value >> (8 * i); }
            };
            be(0, 1); be(4, 1); be(8, 0); be(12, 11); be(16, 100); be(20, 1024);
            endpoint[39] = 2;
        }
        // A known local MR exchange failure followed by a stale all-zero Agree
        // snapshot. The caller must retain this failure, never accept that ACK.
        if (failMemory && bytes == 40) {
            memoryExchangeFailed = true;
            return TILEXR_ERROR_INTERNAL;
        }
        if (memoryExchangeFailed && bytes == 4) { std::memset(out, 0, bytes * options.rankSize); }
        return TILEXR_SUCCESS;
    };
    assert(transport.Init(options) == 0);
    failMemory = true;
    std::array<uint8_t, 128> memory {};
    TileXRRDMAMemHandle handle = 99;
    assert(transport.Register(memory.data(), memory.size(), &handle) == TILEXR_ERROR_INTERNAL);
    assert(handle == 0 && transport.Query().failed && state.mrCount == 1);
    assert(transport.Shutdown() == TILEXR_ERROR_NOT_INITIALIZED && state.queueCount == 0 && state.mrCount == 1);
}
}
int main()
{
    Config();
    MultipleQps();
    MultipleQpFailures();
    DelayedMultipleQpFailures();
    DeviceViewsAndModeAgreement();
    AsymmetricMemoryExchangeFailure();
    LocalExchangeErrorCannotBeAcknowledgedByStaleZero();
    SuccessAndBounds();
    FailedRanks();
    RegistrationAndCleanupFailures();
    RegistrationRollbackFailure();
    TimeoutAndDestroyRetry();
    std::cout << "RDMA collective lifecycle, registered read/write and failure tests passed\n";
}
