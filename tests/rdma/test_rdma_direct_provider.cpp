#include "rdma/tilexr_rdma_transport.h"
#include <infiniband/verbs.h>
#include <array>
#include <cassert>
#include <dlfcn.h>
#include <iostream>

using namespace TileXR;
int main()
{
    void* verbs = dlopen("libibverbs.so.1", RTLD_NOW | RTLD_LOCAL);
    assert(verbs);
    auto flags = reinterpret_cast<int (*)()>(dlsym(verbs, "tilexr_fake_mr_flags"));
    auto segments = reinterpret_cast<int (*)()>(dlsym(verbs, "tilexr_fake_segments"));
    auto setMtu = reinterpret_cast<void (*)(int)>(dlsym(verbs, "tilexr_fake_active_mtu"));
    assert(flags && segments && setMtu);
    void* extension = dlopen("libibv_extend.so", RTLD_NOW | RTLD_LOCAL);
    assert(extension);
    auto capabilities = reinterpret_cast<void (*)(uint32_t)>(dlsym(extension, "tilexr_fake_nda_cap"));
    assert(capabilities);
    capabilities(0); // Actual resources, rather than the advertised bit, decide readiness.
    setMtu(6); // Actual UBNIC active port MTU=8192, exported beyond upstream enum.
    auto provider = MakeRdmaVerbsProvider();
    RdmaConfig config;
    config.device = "hrn5_mock";
    config.aivDirect = true;
    config.indepUb = true;
    config.doorbellMode = TILEXR_RDMA_DB_HARD;
    config.trafficClass = 40;
    assert(provider->Open(config) == 0 && provider->DeviceDirect());
    RdmaEndpoint local, remote;
    assert(provider->Create(1, local) == 0);
    assert(local.mtu == 4096); // Use the configured, supported MTU below the active limit.
    remote.qpn = 700;
    remote.psn = 12345;
    remote.mtu = 1024;
    remote.gid[15] = 88;
    assert(provider->Connect(1, remote) == 0);
    TileXRRDMADeviceQueue queue {};
    assert(provider->DeviceQueue(1, queue) == 0 && queue.qpn == local.qpn && queue.state != 0);
    assert(((queue.sqDbValue >> 50) & 7) == 2); // Negotiated 1024 MTU, not local 2048.
    std::array<uint8_t, 128> memory {};
    RdmaMemory mr;
    assert(provider->Register(memory.data(), memory.size(), mr) == 0);
    assert((flags() & (1 << 21)) != 0); // UBNIC NDA MR access flag, absent in Host mode.
    assert(provider->Transfer(false, 1, mr, 0, mr, 0, 1, 1) == TILEXR_ERROR_NOT_SUPPORT);
    assert(provider->DestroyQueues() == 0);
    assert(provider->DeviceQueue(1, queue) != 0 && queue.state == 0);
    assert(provider->Deregister(mr) == 0 && segments() == 0);
    assert(provider->Close() == 0 && !provider->DeviceDirect());
    provider.reset();
    setMtu(7); // Unknown enum values must still fail closed.
    provider = MakeRdmaVerbsProvider();
    assert(provider->Open(config) == TILEXR_ERROR_PARA_CHECK_FAIL);
    assert(provider->DestroyQueues() == 0 && provider->Close() == 0);
    provider.reset();
    setMtu(IBV_MTU_2048);
    dlclose(verbs);
    dlclose(extension);
    std::cout << "AIV provider integration: RC, negotiated queue export, NDA MR and no Host posting passed\n";
}
