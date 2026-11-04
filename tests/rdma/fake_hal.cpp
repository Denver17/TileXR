// Keep a dependency on the fake verbs DSO, where the shared HAL test state lives.
extern "C" void tilexr_fake_fail(const char*, int);
extern "C" void tilexr_fake_hal_anchor() { tilexr_fake_fail("", 0); }

#include "rdma/tilexr_rdma_ub_res.h"
#include <cassert>
#include <map>
#include <cstdlib>
namespace {
struct Resource { void* memory; TileXR::RdmaUbRes::Doorbell db; };
std::map<uint32_t, Resource> resources;
int mapFailures = 0;
int unmapFailures = 0;
}
extern "C" int tilexr_fake_ub_mappings() { return static_cast<int>(resources.size()); }
extern "C" void tilexr_fake_ub_fail(int map, int unmap) { mapFailures = map; unmapFailures = unmap; }
extern "C" int halResAddrMapV2(uint32_t device, TileXR::RdmaUbRes::MapIn* in,
    TileXR::RdmaUbRes::MapOut* out)
{
    using namespace TileXR::RdmaUbRes;
    assert(device == 0 && in->type == kNdaDoorbell && in->process == kProcessCp1);
    assert(in->privateBytes == sizeof(Doorbell) && in->privateData && in->flags == 0);
    const auto& db = *static_cast<Doorbell*>(in->privateData);
    assert(db.count > 0 && (in->resource & 0xfffU) == db.index && !resources.count(in->resource));
    if (mapFailures > 0) { --mapFailures; return 1; }
    void* memory = nullptr;
    assert(posix_memalign(&memory, kPageBytes, db.count * kPageBytes) == 0);
    resources[in->resource] = {memory, db};
    out->address = reinterpret_cast<uintptr_t>(memory);
    out->bytes = db.count * kPageBytes;
    return 0;
}
extern "C" int halResAddrUnmapV2(uint32_t device, TileXR::RdmaUbRes::MapIn* in)
{
    using namespace TileXR::RdmaUbRes;
    assert(device == 0 && in->type == kNdaDoorbell && in->process == kProcessCp1);
    assert(in->privateBytes == sizeof(Doorbell));
    auto it = resources.find(in->resource);
    assert(it != resources.end());
    const auto& db = *static_cast<Doorbell*>(in->privateData);
    assert(db.guidLow == it->second.db.guidLow && db.guidHigh == it->second.db.guidHigh &&
        db.index == it->second.db.index && db.count == it->second.db.count);
    if (unmapFailures > 0) { --unmapFailures; return 1; }
    std::free(it->second.memory);
    resources.erase(it);
    return 0;
}
