// Copyright (c) 2026 TileXR Project. Licensed under Apache-2.0.
// Operator-independent Host runtime for ABI-v1 TileRelay plans and executions.
// Validates concrete memory contracts, registers trusted immutable external code,
// and launches synchronously without operator math or runtime kernel compilation.
// Owns Host state only; executions share plan validity/poison state and borrow
// device allocations, communicator mappings, context and streams from the caller.
#include "tilexr_relay.h"
#include "tilexr_api.h"
#include "tilexr_types.h"
#include "tilexr_kernel_binary_loader.h"
#include "acl/acl.h"
#include "runtime/rt.h"
#include "runtime/rt_ffts.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

struct RelayState {
    std::mutex mutex;
    bool alive = true;
    bool poisoned = false;
};

struct TileXRRelayPlanImpl {
    std::string fileName, entry, target;
    uint32_t blocks;
    uint64_t workspaceBytes, payloadBytes;
    std::vector<TileXRRelayBindingSpec> bindings;
    std::vector<TileXRRelayRegion> regions;
    std::array<TileXRRelaySlot, TILEXR_RELAY_SLOT_COUNT> slots;
    std::shared_ptr<RelayState> state = std::make_shared<RelayState>();
};

struct KernelArgs {
    void *slots[TILEXR_RELAY_SLOT_COUNT];
    int64_t epoch;
    int32_t rank, rankCount;
    uint64_t ffts;
};
struct PreparedLaunch {
    KernelArgs frame{};
    void *comm{};
    void *peerMems[2]{};
    uint64_t peerMemBytes{};
    int32_t device{};
    aclrtContext context{};
    const void *stub{};
    uint32_t blocks{};
};
namespace {
constexpr int Invalid = TileXR::TILEXR_ERROR_PARA_CHECK_FAIL;
constexpr uint64_t FlagBytes = 2ull*TILEXR_RELAY_FLAG_PLANE_BYTES;
static_assert(FlagBytes == TileXR::IPC_DATA_OFFSET, "Relay fixed flag planes");
static_assert(sizeof(void*)==8 && sizeof(KernelArgs)==152, "Relay ABI v1 frame");
static_assert(offsetof(KernelArgs,epoch)==128 && offsetof(KernelArgs,rank)==136 &&
    offsetof(KernelArgs,rankCount)==140 && offsetof(KernelArgs,ffts)==144, "Relay ABI v1 offsets");

bool Range(uint64_t offset, uint64_t bytes, uint64_t capacity)
{ return offset <= capacity && bytes <= capacity-offset; }
bool Alignment(uint32_t value) { return value>=32 && value<=4096 && !(value&(value-1)); }
bool Overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs)
{ return as && bs && a < b+bs && b < a+as; } // callers establish non-overflow first
bool Address(void *base, uint64_t bytes, uint32_t alignment)
{
    const uintptr_t value=reinterpret_cast<uintptr_t>(base);
    return base && value%alignment==0 && bytes<=std::numeric_limits<uintptr_t>::max()-value;
}
bool Text(const char *text, std::size_t max)
{ return text && ::strnlen(text,max+1)<=max && *text; }

int Validate(const TileXRRelayPlanDesc &d)
{
    if (d.structSize!=sizeof(d) || d.version!=1 || d.abiVersion!=1 || d.protocolVersion!=1 ||
        d.reserved || d.rankCount!=2 || !d.blocks || d.bindingCount>16 || d.regionCount>64 ||
        d.channelCount>16384 || (d.bindingCount && !d.bindings) || (d.regionCount && !d.regions) ||
        (d.channelCount && !d.channels) || !Text(d.sha256,64) || std::strlen(d.sha256)!=64 ||
        !Text(d.fileName,128) || !Text(d.entry,128) || !Text(d.target,32)) return Invalid;
    if (std::strcmp(d.target,"Ascend910B") || std::strcmp(d.entry,"main_kernel"))
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    for (unsigned i=0;i<64;++i)
        if (!((d.sha256[i]>='0' && d.sha256[i]<='9') || (d.sha256[i]>='a' && d.sha256[i]<='f')))
            return Invalid;
    if (std::string(d.fileName)!="tilexr_relay_"+std::string(d.sha256)+".o" ||
        d.peerPayloadBytes > std::numeric_limits<uint64_t>::max()-FlagBytes) return Invalid;
    for (uint32_t i=0;i<d.bindingCount;++i) {
        const auto &b=d.bindings[i];
        if (!b.bytes || !Alignment(b.alignment) || b.reserved) return Invalid;
    }
    for (uint32_t i=0;i<d.regionCount;++i) {
        const auto &r=d.regions[i];
        if (!r.bytes || !Alignment(r.alignment) || r.offset%r.alignment || r.reserved ||
            r.space>TILEXR_RELAY_PAYLOAD || r.initialization>TILEXR_RELAY_ZERO_ONCE ||
            (r.space==TILEXR_RELAY_PAYLOAD && r.initialization!=TILEXR_RELAY_UNINITIALIZED) ||
            !Range(r.offset,r.bytes,r.space==TILEXR_RELAY_PRIVATE ? d.workspaceBytes : d.peerPayloadBytes))
            return Invalid;
        for (uint32_t j=0;j<i;++j) {
            const auto &p=d.regions[j];
            if (p.space==r.space && Overlap(r.offset,r.bytes,p.offset,p.bytes)) return Invalid;
        }
    }
    for (uint32_t i=0;i<d.channelCount;++i) {
        const auto &c=d.channels[i];
        if (!c.slotCount || c.participants!=2 || c.maxCredits!=1 ||
            !Range(c.firstSlot,c.slotCount,TILEXR_RELAY_FLAG_PLANE_BYTES/TILEXR_RELAY_FLAG_SLOT_BYTES))
            return Invalid;
        for (uint32_t j=0;j<i;++j) {
            const auto &p=d.channels[j];
            if (Overlap(c.firstSlot,c.slotCount,p.firstSlot,p.slotCount)) return Invalid;
        }
    }
    for (const auto &s:d.slots) {
        if (s.reserved || s.source>TILEXR_RELAY_PEER_FLAGS) return Invalid;
        if (s.source==TILEXR_RELAY_UNUSED) {
            if (s.index || s.offset || s.bytes || s.alignment) return Invalid;
            continue;
        }
        if (!s.bytes || !Alignment(s.alignment) || s.offset%s.alignment) return Invalid;
        if (s.source==TILEXR_RELAY_USER) {
            if (s.index>=d.bindingCount || s.alignment>d.bindings[s.index].alignment ||
                !Range(s.offset,s.bytes,d.bindings[s.index].bytes)) return Invalid;
        } else if (s.source<=TILEXR_RELAY_PEER_PAYLOAD) {
            if (s.index>=d.regionCount) return Invalid;
            const auto &r=d.regions[s.index];
            const uint32_t space=s.source==TILEXR_RELAY_WORKSPACE ? TILEXR_RELAY_PRIVATE : TILEXR_RELAY_PAYLOAD;
            if (r.space!=space || s.alignment>r.alignment || !Range(s.offset,s.bytes,r.bytes)) return Invalid;
        } else if (s.index || s.alignment<64 || !d.channelCount || !Range(s.offset,s.bytes,FlagBytes)) {
            return Invalid;
        }
    }
    return 0;
}

struct Registration { void *binary{}; unsigned char stub{}; };
using Key=std::tuple<uintptr_t,int32_t,std::string,std::string>;
std::mutex registrationMutex;
std::map<Key,std::unique_ptr<Registration>> registrations;
int Register(const TileXRRelayPlanImpl &plan, int32_t device, aclrtContext context, const void **stub)
{
    std::lock_guard<std::mutex> lock(registrationMutex);
    const Key key(reinterpret_cast<uintptr_t>(context),device,plan.fileName,plan.entry);
    const auto found=registrations.find(key);
    if (found!=registrations.end()) { *stub=&found->second->stub; return 0; }
    TileXR::KernelBinaryView view;
    if (!TileXR::LoadKernelBinary(reinterpret_cast<const void*>(&Register),plan.fileName.c_str(),&view))
        return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    // Installed content-addressed files are trusted and immutable. The loader
    // already guarantees a nonempty binary whose length fits the runtime ABI.
    // Allocate the map node before registering: no throwing insertion can invalidate a live stub.
    auto inserted=registrations.emplace(key,std::unique_ptr<Registration>(new Registration));
    auto &state=*inserted.first->second;
    rtDevBinary_t binary{};
    binary.magic=RT_DEV_BINARY_MAGIC_ELF; binary.data=view.data; binary.length=static_cast<uint32_t>(view.size);
    int ret=rtDevBinaryRegister(&binary,&state.binary);
    if (!ret) ret=rtFunctionRegister(state.binary,&state.stub,plan.entry.c_str(),plan.entry.c_str(),0);
    if (ret) {
        if (state.binary) (void)rtDevBinaryUnRegister(state.binary);
        registrations.erase(inserted.first);
        return ret;
    }
    *stub=&state.stub;
    return 0;
}

int Bind(const TileXRRelayPlanImpl &plan, const TileXRRelayBinding *bindings, uint32_t count,
    void *workspace, uint64_t workspaceBytes, TileXR::CommArgs &comm, KernelArgs *frame)
{
    if (count!=plan.bindings.size() || (count && !bindings) || workspaceBytes<plan.workspaceBytes ||
        (plan.workspaceBytes && !Address(workspace,workspaceBytes,32))) return Invalid;
    std::array<std::pair<uintptr_t,uint64_t>,19> live{}; // sixteen users, workspace and two peer mappings
    std::size_t liveCount=0;
    if (plan.workspaceBytes) live[liveCount++]={reinterpret_cast<uintptr_t>(workspace),plan.workspaceBytes};
    for (uint32_t i=0;i<count;++i) {
        const auto &b=bindings[i]; const auto &s=plan.bindings[i];
        if (b.bytes<s.bytes || b.dtypeTag!=s.dtypeTag || b.layoutTag!=s.layoutTag ||
            !Address(b.data,b.bytes,s.alignment)) return Invalid;
        live[liveCount++]={reinterpret_cast<uintptr_t>(b.data),s.bytes};
    }
    for (unsigned i=0;i<2;++i) {
        if (!Address(comm.peerMems[i],comm.peerMemBytes,64)) return Invalid;
        live[liveCount++]={reinterpret_cast<uintptr_t>(comm.peerMems[i]),comm.peerMemBytes};
    }
    for (std::size_t i=0;i<liveCount;++i) for (std::size_t j=0;j<i;++j)
        if (Overlap(live[i].first,live[i].second,live[j].first,live[j].second)) return Invalid;
    for (const auto &r:plan.regions) {
        const uintptr_t base=r.space==TILEXR_RELAY_PRIVATE ? reinterpret_cast<uintptr_t>(workspace) :
            reinterpret_cast<uintptr_t>(comm.peerMems[comm.rank])+FlagBytes;
        if ((base+r.offset)%r.alignment) return Invalid;
        if (r.space==TILEXR_RELAY_PAYLOAD &&
            (reinterpret_cast<uintptr_t>(comm.peerMems[1-comm.rank])+FlagBytes+r.offset)%r.alignment)
            return Invalid;
    }
    for (unsigned i=0;i<TILEXR_RELAY_SLOT_COUNT;++i) {
        const auto &s=plan.slots[i]; uintptr_t base=0;
        if (s.source==TILEXR_RELAY_UNUSED) continue;
        if (s.source==TILEXR_RELAY_USER) base=reinterpret_cast<uintptr_t>(bindings[s.index].data);
        else if (s.source==TILEXR_RELAY_WORKSPACE) base=reinterpret_cast<uintptr_t>(workspace)+plan.regions[s.index].offset;
        else if (s.source==TILEXR_RELAY_OWNER_PAYLOAD || s.source==TILEXR_RELAY_PEER_PAYLOAD)
            base=reinterpret_cast<uintptr_t>(comm.peerMems[s.source==TILEXR_RELAY_OWNER_PAYLOAD ? comm.rank : 1-comm.rank])+
                FlagBytes+plan.regions[s.index].offset;
        else base=reinterpret_cast<uintptr_t>(comm.peerMems[s.source==TILEXR_RELAY_OWNER_FLAGS ? comm.rank : 1-comm.rank]);
        if ((base+s.offset)%s.alignment) return Invalid;
        frame->slots[i]=reinterpret_cast<void*>(base+s.offset);
    }
    return 0;
}

int CurrentLocation(int32_t *device, aclrtContext *context)
{
    int ret = aclrtGetDevice(device);
    if (ret) return ret;
    ret = aclrtGetCurrentContext(context);
    if (ret) return ret;
    return *context ? 0 : TileXR::TILEXR_ERROR_NOT_INITIALIZED;
}

int Prepare(const TileXRRelayPlanImpl &plan, void *comm, const TileXRRelayBinding *bindings,
    uint32_t count, void *workspace, uint64_t workspaceBytes, PreparedLaunch *prepared)
{
    TileXR::CommArgs *args = nullptr;
    int ret = TileXRGetCommArgsHost(comm, args);
    if (ret) return ret;
    if (!args || !args->peerMems[0] || !args->peerMems[1]) return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    if (args->rankSize != 2 || args->rank < 0 || args->rank > 1 ||
        args->peerMemBytes < FlagBytes + plan.payloadBytes) return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    ret = Bind(plan, bindings, count, workspace, workspaceBytes, *args, &prepared->frame);
    if (ret) return ret;
    ret = CurrentLocation(&prepared->device, &prepared->context);
    if (ret) return ret;
    const char *soc = aclrtGetSocName();
    if (!soc || std::strncmp(soc, plan.target.c_str(), plan.target.size()))
        return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    int64_t cubes = 0;
    ret = aclrtGetDeviceInfo(static_cast<uint32_t>(prepared->device), ACL_DEV_ATTR_CUBE_CORE_NUM, &cubes);
    if (ret) return ret;
    if (cubes <= 0 || plan.blocks > static_cast<uint64_t>(cubes)) return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    ret = Register(plan, prepared->device, prepared->context, &prepared->stub);
    if (ret) return ret;
    prepared->comm = comm;
    prepared->peerMems[0] = args->peerMems[0];
    prepared->peerMems[1] = args->peerMems[1];
    prepared->peerMemBytes = args->peerMemBytes;
    prepared->frame.rank = args->rank;
    prepared->frame.rankCount = args->rankSize;
    prepared->blocks = plan.blocks;
    return 0;
}

int CheckPrepared(const PreparedLaunch &prepared)
{
    int32_t device = 0;
    aclrtContext context = nullptr;
    int ret = CurrentLocation(&device, &context);
    if (ret) return ret;
    if (device != prepared.device || context != prepared.context) return TileXR::TILEXR_ERROR_NOT_SUPPORT;
    TileXR::CommArgs *args = nullptr;
    ret = TileXRGetCommArgsHost(prepared.comm, args);
    if (ret) return ret;
    if (!args) return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    if (args->rank != prepared.frame.rank || args->rankSize != prepared.frame.rankCount ||
        args->peerMems[0] != prepared.peerMems[0] || args->peerMems[1] != prepared.peerMems[1] ||
        args->peerMemBytes != prepared.peerMemBytes) return Invalid;
    return 0;
}

int Launch(RelayState &state, const PreparedLaunch &prepared, void *stream)
{
    KernelArgs frame = prepared.frame;
    uint32_t fftsBytes = 0;
    int ret = rtGetC2cCtrlAddr(&frame.ffts, &fftsBytes);
    if (ret) return ret;
    ret = TileXRCommNextMagic(prepared.comm, &frame.epoch);
    if (ret) return ret;
    rtArgsEx_t kernel{};
    kernel.args = &frame;
    kernel.argsSize = sizeof(frame);
    rtTaskCfgInfo_t config{};
    config.schemMode = 1;
    ret = rtKernelLaunchWithFlagV2(prepared.stub, prepared.blocks, &kernel, nullptr,
        static_cast<rtStream_t>(stream), 0, &config);
    if (!ret) ret = aclrtSynchronizeStreamWithTimeout(static_cast<aclrtStream>(stream), 60000);
    if (ret) state.poisoned = true;
    return ret;
}
}

struct TileXRRelayExecutionImpl {
    std::shared_ptr<RelayState> state;
    PreparedLaunch prepared;
};

extern "C" int TileXRRelayPlanCreate(const TileXRRelayPlanDesc *d, TileXRRelayPlan *output)
{
    if (!output) return Invalid;
    *output=nullptr;
    if (!d) return Invalid;
    try {
        const int ret=Validate(*d); if (ret) return ret;
        std::unique_ptr<TileXRRelayPlanImpl> p(new TileXRRelayPlanImpl);
        p->fileName=d->fileName; p->entry=d->entry; p->target=d->target;
        p->blocks=d->blocks; p->workspaceBytes=d->workspaceBytes; p->payloadBytes=d->peerPayloadBytes;
        if (d->bindingCount) p->bindings.assign(d->bindings,d->bindings+d->bindingCount);
        if (d->regionCount) p->regions.assign(d->regions,d->regions+d->regionCount);
        std::copy(std::begin(d->slots),std::end(d->slots),p->slots.begin());
        *output=p.release(); return 0;
    } catch (const std::bad_alloc&) { return TileXR::TILEXR_ERROR_INTERNAL; }
}
extern "C" int TileXRRelayPlanGetWorkspaceSize(TileXRRelayPlan plan, uint64_t *bytes)
{
    if (!plan || !bytes) return Invalid;
    *bytes=plan->workspaceBytes; return 0;
}
extern "C" int TileXRRelayLaunch(TileXRRelayPlan plan, void *comm,
    const TileXRRelayBinding *bindings, uint32_t count, void *workspace, uint64_t workspaceBytes, void *stream)
{
    if (!plan || !comm || !stream) return Invalid;
    auto &state = *plan->state;
    std::unique_lock<std::mutex> lock(state.mutex, std::try_to_lock);
    if (!lock.owns_lock() || !state.alive || state.poisoned) return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    try {
        PreparedLaunch prepared;
        const int ret = Prepare(*plan, comm, bindings, count, workspace, workspaceBytes, &prepared);
        return ret ? ret : Launch(state, prepared, stream);
    } catch (const std::bad_alloc&) { return TileXR::TILEXR_ERROR_INTERNAL; }
}
extern "C" int TileXRRelayPlanDestroy(TileXRRelayPlan plan)
{
    if (!plan) return Invalid;
    const auto state = plan->state;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->alive = false;
    delete plan;
    return 0;
}

extern "C" int TileXRRelayExecutionCreate(TileXRRelayPlan plan, void *comm,
    const TileXRRelayBinding *bindings, uint32_t count, void *workspace, uint64_t workspaceBytes,
    TileXRRelayExecution *output)
{
    if (!output) return Invalid;
    *output = nullptr;
    if (!plan || !comm) return Invalid;
    const auto state = plan->state;
    std::unique_lock<std::mutex> lock(state->mutex, std::try_to_lock);
    if (!lock.owns_lock() || !state->alive || state->poisoned) return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    try {
        std::unique_ptr<TileXRRelayExecutionImpl> execution(new TileXRRelayExecutionImpl);
        const int ret = Prepare(*plan, comm, bindings, count, workspace, workspaceBytes, &execution->prepared);
        if (ret) return ret;
        execution->state = state;
        *output = execution.release();
        return 0;
    } catch (const std::bad_alloc&) { return TileXR::TILEXR_ERROR_INTERNAL; }
}

extern "C" int TileXRRelayExecutionLaunch(TileXRRelayExecution execution, void *stream)
{
    if (!execution || !stream) return Invalid;
    auto &state = *execution->state;
    std::unique_lock<std::mutex> lock(state.mutex, std::try_to_lock);
    if (!lock.owns_lock() || !state.alive || state.poisoned) return TileXR::TILEXR_ERROR_NOT_INITIALIZED;
    const int ret = CheckPrepared(execution->prepared);
    return ret ? ret : Launch(state, execution->prepared, stream);
}

extern "C" int TileXRRelayExecutionDestroy(TileXRRelayExecution execution)
{
    if (!execution) return Invalid;
    const auto state = execution->state;
    std::lock_guard<std::mutex> lock(state->mutex);
    delete execution;
    return 0;
}
