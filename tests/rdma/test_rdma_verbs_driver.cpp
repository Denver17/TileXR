#include "rdma/tilexr_rdma_transport.h"
#include <array>
#include <cassert>
#include <dlfcn.h>
using namespace TileXR;
int main()
{
    void* library = dlopen("libibverbs.so.1", RTLD_NOW | RTLD_LOCAL);
    assert(library);
    auto fail = reinterpret_cast<void (*)(const char*, int)>(dlsym(library, "tilexr_fake_fail"));
    auto segments = reinterpret_cast<int (*)()>(dlsym(library, "tilexr_fake_segments"));
    auto queues = reinterpret_cast<int (*)()>(dlsym(library, "tilexr_fake_queues"));
    auto gidIndex = reinterpret_cast<int (*)()>(dlsym(library, "tilexr_fake_gid_index"));
    assert(fail && segments && queues && gidIndex);
    auto provider = MakeRdmaVerbsProvider();
    RdmaConfig config;
    config.device = "hrn5_mock";
    config.indepUb = true;
    config.trafficClass = 40;
    assert(provider->Open(config) == 0);
    RdmaEndpoint local, remote;
    assert(provider->Create(1, local) == 0);
    assert(local.mtu == 2048 && local.gid[15] == 2);
    remote.qpn = 700;
    remote.psn = 12345;
    remote.mtu = 1024;
    remote.gid[15] = 88;
    assert(provider->Connect(1, remote) == 0 && gidIndex() == 2);
    std::array<uint8_t, 128> source {}, target {};
    RdmaMemory memory;
    fail("pin", 1);
    assert(provider->Register(source.data(), source.size(), memory) != 0 && segments() == 0);
    fail("register", 1);
    assert(provider->Register(source.data(), source.size(), memory) != 0 && segments() == 1);
    assert(provider->Deregister(memory) == 0 && segments() == 0);
    assert(provider->Register(source.data(), source.size(), memory) == 0 && segments() == 1);
    RdmaMemory other;
    other.address = reinterpret_cast<uintptr_t>(target.data());
    other.bytes = target.size();
    other.rkey = 66;
    source[2] = 47;
    assert(provider->Transfer(false, 1, memory, 2, other, 7, 1, 100) == 0 && target[7] == 47);
    target[4] = 73;
    assert(provider->Transfer(true, 1, memory, 5, other, 4, 1, 100) == 0 && source[5] == 73);
    for (const char* operation : {"wc", "wr_id", "post", "timeout"}) {
        fail(operation, 1);
        assert(provider->Transfer(false, 1, memory, 0, other, 0, 1, 1) != 0);
    }
    fail("destroy", 1);
    assert(provider->DestroyQueues() != 0 && queues() == 1);
    assert(provider->DestroyQueues() == 0 && queues() == 0);
    fail("deregister", 1);
    assert(provider->Deregister(memory) != 0 && segments() == 1);
    fail("unpin", 1);
    assert(provider->Deregister(memory) != 0 && segments() == 1 && memory.resource == nullptr);
    assert(provider->Deregister(memory) == 0 && segments() == 0);
    assert(provider->Close() == 0);
    provider.reset();
    dlclose(library);
}
