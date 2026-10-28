// Exercise generic Relay validation, prepared execution and injected runtime failures without an NPU.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "tilexr_relay.h"
#include "tilexr_api.h"
#include "tilexr_kernel_binary_loader.h"
#include "acl/acl.h"
#include "runtime/rt.h"
#include <cassert>
#include <cstring>
#include <limits>
#include <string>

namespace {
alignas(4096) unsigned char own[2097152+4096], peer[sizeof(own)], workspace[4096], input[4096], other[4096];
unsigned char comm, stream, context;
TileXR::CommArgs args;
struct Frame { void *slots[16]; int64_t epoch; int32_t rank,rankCount; uint64_t ffts; } frame;
static_assert(sizeof(Frame)==152,"mock ABI");
int argsError,deviceError,contextError,cubeError,magicError,fftsError,registerError,functionError,launchError,syncError;
int launches,syncs,registers,unregisters,device=0;
int magicCalls, fftsCalls, cubeCalls, socCalls;
int64_t nextMagic = 17;
uint64_t nextFfts = 29;
int64_t cubes=20;
const char *soc="Ascend910B3";
void *currentContext=&context;
bool available=true;
// Deliberately not the named digest: native registration trusts the installed filename.
const unsigned char binaryBytes[]={'d','e','f'};
const char *digest="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
TileXRRelayBindingSpec specs[2];
TileXRRelayRegion regions[2];
TileXRRelayChannel channels[2];
TileXRRelayBinding bindings[2];
std::string filename;
void Reset()
{
    args={}; args.rankSize=2; args.peerMems[0]=own; args.peerMems[1]=peer; args.peerMemBytes=sizeof(own);
    argsError=deviceError=contextError=cubeError=magicError=fftsError=registerError=functionError=launchError=syncError=0;
    launches=syncs=registers=unregisters=0; cubes=20; soc="Ascend910B3";
    magicCalls=fftsCalls=cubeCalls=socCalls=0; nextMagic=17; nextFfts=29;
    available=true; currentContext=&context;
    specs[0]={256,64,1,2,0}; specs[1]={256,64,1,2,0};
    bindings[0]={input,sizeof(input),1,2}; bindings[1]={other,sizeof(other),1,2};
    regions[0]={64,512,64,TILEXR_RELAY_PRIVATE,TILEXR_RELAY_ZERO_ONCE,0};
    regions[1]={128,1024,64,TILEXR_RELAY_PAYLOAD,TILEXR_RELAY_UNINITIALIZED,0};
    channels[0]={2,8,2,1}; channels[1]={10,8,2,1};
    filename="tilexr_relay_"+std::string(digest)+".o";
}
TileXRRelayPlanDesc Desc()
{
    TileXRRelayPlanDesc d{};
    d.structSize=sizeof(d); d.version=d.abiVersion=d.protocolVersion=1;
    d.fileName=filename.c_str(); d.sha256=digest; d.entry="main_kernel"; d.target="Ascend910B";
    d.blocks=4; d.rankCount=2; d.workspaceBytes=1024; d.peerPayloadBytes=2048;
    d.bindingCount=2; d.regionCount=2; d.channelCount=2;
    d.bindings=specs; d.regions=regions; d.channels=channels;
    d.slots[0]={TILEXR_RELAY_USER,0,64,64,64,0};
    d.slots[1]={TILEXR_RELAY_WORKSPACE,0,64,64,64,0};
    d.slots[2]={TILEXR_RELAY_OWNER_PAYLOAD,1,64,64,64,0};
    d.slots[3]={TILEXR_RELAY_PEER_PAYLOAD,1,64,64,64,0};
    d.slots[4]={TILEXR_RELAY_OWNER_FLAGS,0,0,2097152,64,0};
    d.slots[5]={TILEXR_RELAY_PEER_FLAGS,0,1048576,64,64,0};
    return d;
}
TileXRRelayPlan Create()
{
    auto d=Desc(); TileXRRelayPlan p=nullptr; assert(TileXRRelayPlanCreate(&d,&p)==0); assert(p); return p;
}
int Run(TileXRRelayPlan p) { return TileXRRelayLaunch(p,&comm,bindings,2,workspace,sizeof(workspace),&stream); }
int Prepare(TileXRRelayPlan p, TileXRRelayExecution *execution)
{ return TileXRRelayExecutionCreate(p,&comm,bindings,2,workspace,sizeof(workspace),execution); }
void Reject(TileXRRelayPlanDesc &d)
{
    TileXRRelayPlan p=reinterpret_cast<TileXRRelayPlan>(1);
    assert(TileXRRelayPlanCreate(&d,&p)==-3 && !p);
}
void FreshContext() { ++device; } // registration cache identity varies without changing real buffers
}
namespace TileXR {
bool LoadKernelBinary(const void *,const char *name,KernelBinaryView *view)
{
    assert(filename==name);
    view->data=binaryBytes; view->size=sizeof(binaryBytes); return available;
}
}
extern "C" int TileXRGetCommArgsHost(TileXRCommPtr,TileXR::CommArgs *&p) { p=&args; return argsError; }
extern "C" int TileXRCommNextMagic(TileXRCommPtr,int64_t *v)
{ ++magicCalls; *v=nextMagic++; return magicError; }
extern "C" aclError aclrtGetDevice(int32_t *v) { *v=device; return deviceError; }
extern "C" aclError aclrtGetCurrentContext(aclrtContext *v) { *v=currentContext; return contextError; }
extern "C" const char *aclrtGetSocName() { ++socCalls; return soc; }
extern "C" aclError aclrtGetDeviceInfo(uint32_t,aclrtDevAttr,int64_t *v)
{ ++cubeCalls; *v=cubes; return cubeError; }
extern "C" int rtDevBinaryRegister(const rtDevBinary_t *b,void **h)
{ assert(b->magic==RT_DEV_BINARY_MAGIC_ELF && b->length==3); *h=&comm; ++registers; return registerError; }
extern "C" int rtFunctionRegister(void *,const void *,const char *name,const void *,uint32_t)
{ assert(std::strcmp(name,"main_kernel")==0); return functionError; }
extern "C" int rtDevBinaryUnRegister(void *) { ++unregisters; return 777; }
extern "C" int rtGetC2cCtrlAddr(uint64_t *v,uint32_t *b)
{ ++fftsCalls; *v=nextFfts++; *b=8; return fftsError; }
extern "C" int rtKernelLaunchWithFlagV2(const void *,uint32_t blocks,rtArgsEx_t *a,void *,rtStream_t s,
    uint32_t,const rtTaskCfgInfo_t *cfg)
{
    assert(blocks==4 && a->argsSize==sizeof(frame) && s==&stream && cfg->schemMode==1);
    std::memcpy(&frame,a->args,sizeof(frame)); ++launches; return launchError;
}
extern "C" aclError aclrtSynchronizeStreamWithTimeout(aclrtStream s,int32_t timeout)
{ assert(s==&stream && timeout==60000); ++syncs; return syncError; }

namespace {
void TestPreparedExecution()
{
    Reset(); auto p = Create();
    TileXRRelayExecution execution = reinterpret_cast<TileXRRelayExecution>(1);
    assert(TileXRRelayExecutionCreate(nullptr,&comm,bindings,2,workspace,sizeof(workspace),&execution)==-3);
    assert(!execution);
    execution = reinterpret_cast<TileXRRelayExecution>(1);
    assert(TileXRRelayExecutionCreate(p,nullptr,bindings,2,workspace,sizeof(workspace),&execution)==-3);
    assert(!execution);
    assert(Prepare(p,nullptr)==-3);
    bindings[0].bytes = 1;
    execution = reinterpret_cast<TileXRRelayExecution>(1);
    assert(Prepare(p,&execution)==-3 && !execution && !magicCalls && !fftsCalls && !launches);
    Reset(); assert(Prepare(p,&execution)==0 && execution);
    TileXRRelayExecution second = nullptr;
    assert(Prepare(p,&second)==0 && !magicCalls && !fftsCalls && !launches && !syncs);
    assert(cubeCalls==2 && socCalls==2);
    const auto original = bindings[0];
    bindings[0] = {other,0,99,99}; // preparation copied addresses, not the caller's descriptors
    assert(TileXRRelayExecutionLaunch(execution,nullptr)==-3 && !magicCalls && !fftsCalls);
    assert(TileXRRelayExecutionLaunch(execution,&stream)==0 && launches==1 && syncs==1);
    assert(frame.slots[0]==input+64); // nonzero offset survives caller descriptor mutation
    assert(frame.rank==0 && frame.rankCount==2 && frame.epoch==17 && frame.ffts==29);
    const auto firstFrame = frame;
    assert(TileXRRelayExecutionLaunch(second,&stream)==0 && frame.epoch==18 && frame.ffts==30);
    assert(std::memcmp(frame.slots,firstFrame.slots,sizeof(frame.slots))==0);
    assert(cubeCalls==2 && socCalls==2); // no capability queries on the prepared launch path
    bindings[0] = original;
    assert(Run(p)==0 && frame.epoch==19 && frame.ffts==31);
    assert(TileXRRelayExecutionLaunch(execution,&stream)==0 && frame.epoch==20 && frame.ffts==32);
    assert(magicCalls==4 && fftsCalls==4 && launches==4 && syncs==4);

    Reset(); args.rank=1;
    assert(TileXRRelayExecutionLaunch(execution,&stream)==-3 && !magicCalls && !fftsCalls && !launches);
    Reset(); args.rankSize=3;
    assert(TileXRRelayExecutionLaunch(execution,&stream)==-3 && !magicCalls);
    Reset(); args.peerMems[0]=own+64;
    assert(TileXRRelayExecutionLaunch(execution,&stream)==-3 && !magicCalls);
    Reset(); args.peerMems[1]=peer+64;
    assert(TileXRRelayExecutionLaunch(execution,&stream)==-3 && !magicCalls);
    Reset(); args.peerMems[1]=nullptr;
    assert(TileXRRelayExecutionLaunch(execution,&stream)==-3 && !magicCalls);
    Reset(); ++args.peerMemBytes;
    assert(TileXRRelayExecutionLaunch(execution,&stream)==-3 && !magicCalls);
    Reset(); argsError=801;
    assert(TileXRRelayExecutionLaunch(execution,&stream)==801 && !magicCalls);
    Reset(); currentContext=&stream;
    assert(TileXRRelayExecutionLaunch(execution,&stream)==-6 && !magicCalls);
    Reset(); const int preparedDevice=device; ++device;
    assert(TileXRRelayExecutionLaunch(execution,&stream)==-6 && !magicCalls);
    device=preparedDevice;
    Reset(); deviceError=802;
    assert(TileXRRelayExecutionLaunch(execution,&stream)==802 && !magicCalls);
    Reset(); contextError=803;
    assert(TileXRRelayExecutionLaunch(execution,&stream)==803 && !magicCalls);
    Reset(); currentContext=nullptr;
    assert(TileXRRelayExecutionLaunch(execution,&stream)==-1 && !magicCalls);
    Reset(); fftsError=804;
    assert(TileXRRelayExecutionLaunch(execution,&stream)==804 && fftsCalls==1 && !magicCalls && !launches);
    Reset(); magicError=805;
    assert(TileXRRelayExecutionLaunch(execution,&stream)==805 && fftsCalls==1 && magicCalls==1 && !launches);
    Reset(); assert(TileXRRelayExecutionLaunch(execution,&stream)==0); // prelaunch errors never poison
    assert(TileXRRelayPlanDestroy(p)==0);
    assert(TileXRRelayExecutionDestroy(execution)==0 && TileXRRelayExecutionDestroy(second)==0);
    assert(!unregisters);

    // Preparation surfaces the same full validation and registration errors as the old API.
    Reset(); p=Create(); FreshContext(); available=false;
    assert(Prepare(p,&execution)==-1 && !execution && !magicCalls && !fftsCalls);
    Reset(); FreshContext(); registerError=806;
    assert(Prepare(p,&execution)==806 && !execution && unregisters==1);
    Reset(); FreshContext(); functionError=807;
    assert(Prepare(p,&execution)==807 && !execution && unregisters==1);
    Reset(); cubeError=808;
    assert(Prepare(p,&execution)==808 && !execution);
    Reset(); cubes=3;
    assert(Prepare(p,&execution)==-6 && !execution);
    Reset(); assert(Prepare(p,&execution)==0);
    assert(TileXRRelayExecutionDestroy(execution)==0);
    assert(Run(p)==0); // child destruction does not invalidate the parent
    assert(TileXRRelayPlanDestroy(p)==0);

    // A failed attempted launch poisons siblings, future preparation and the old API.
    for (bool failDuringSync : {false,true}) {
        Reset(); p=Create(); assert(Prepare(p,&execution)==0 && Prepare(p,&second)==0);
        if (failDuringSync) syncError=809; else launchError=810;
        assert(TileXRRelayExecutionLaunch(execution,&stream)==(failDuringSync ? 809 : 810));
        assert(launches==1 && syncs==(failDuringSync ? 1 : 0));
        Reset(); TileXRRelayExecution rejected=reinterpret_cast<TileXRRelayExecution>(1);
        assert(TileXRRelayExecutionLaunch(second,&stream)==-1 && Run(p)==-1);
        assert(Prepare(p,&rejected)==-1 && !rejected && !launches && !magicCalls);
        assert(TileXRRelayExecutionDestroy(execution)==0 && TileXRRelayExecutionDestroy(second)==0);
        assert(TileXRRelayPlanDestroy(p)==0);
    }
    Reset(); p=Create(); assert(Prepare(p,&execution)==0);
    launchError=811; assert(Run(p)==811);
    Reset(); assert(TileXRRelayExecutionLaunch(execution,&stream)==-1 && !launches && !magicCalls);
    assert(TileXRRelayPlanDestroy(p)==0 && TileXRRelayExecutionDestroy(execution)==0);
    assert(TileXRRelayExecutionLaunch(nullptr,&stream)==-3 && TileXRRelayExecutionDestroy(nullptr)==-3);
}
}

int main()
{
    Reset(); auto p=Create(); uint64_t bytes=0;
    assert(TileXRRelayPlanGetWorkspaceSize(p,&bytes)==0 && bytes==1024);
    specs[0].bytes=9000000; regions[0].offset=4000; channels[0].firstSlot=100000;
    filename="borrowed-string-was-mutated"; // all descriptor strings/arrays were copied
    filename="tilexr_relay_"+std::string(digest)+".o";
    assert(Run(p)==0 && launches==1 && syncs==1 && registers==1);
    assert(frame.slots[0]==input+64 && frame.slots[1]==workspace+128);
    assert(frame.slots[2]==own+2097152+192 && frame.slots[3]==peer+2097152+192);
    assert(frame.slots[4]==own && frame.slots[5]==peer+1048576 && !frame.slots[15]);
    assert(frame.epoch==17 && frame.rank==0 && frame.rankCount==2 && frame.ffts==29);
    args.rank=1; assert(Run(p)==0 && frame.slots[2]==peer+2097152+192 && frame.rank==1 && registers==1);
    assert(TileXRRelayPlanDestroy(p)==0 && unregisters==0);
    Reset(); p=Create(); FreshContext(); assert(Run(p)==0 && registers==1); TileXRRelayPlanDestroy(p);
    Reset(); p=Create(); currentContext=&stream;
    assert(Run(p)==0 && registers==1); TileXRRelayPlanDestroy(p);
    Reset(); auto d=Desc(); d.version=2; Reject(d);
    d=Desc(); d.structSize--; Reject(d);
    d=Desc(); d.abiVersion=0; Reject(d);
    d=Desc(); d.protocolVersion=2; Reject(d);
    d=Desc(); d.reserved=1; Reject(d);
    d=Desc(); d.fileName="../escaped.o"; Reject(d);
    d=Desc(); d.sha256="A"; Reject(d);
    d=Desc(); d.bindingCount=17; Reject(d);
    d=Desc(); d.bindings=nullptr; Reject(d);
    d=Desc(); d.peerPayloadBytes=std::numeric_limits<uint64_t>::max(); Reject(d);
    d=Desc(); d.slots[10].offset=1; Reject(d);
    d=Desc(); d.slots[0].source=7; Reject(d);
    d=Desc(); d.slots[0].reserved=1; Reject(d);
    d=Desc(); d.slots[0].offset=256; Reject(d);
    d=Desc(); d.slots[2].index=0; Reject(d);
    d=Desc(); d.slots[5].offset=2097152; Reject(d);
    d=Desc(); specs[0].alignment=48; Reject(d); Reset();
    d=Desc(); specs[0].reserved=1; Reject(d); Reset();
    d=Desc(); regions[0].bytes=10000; Reject(d); Reset();
    d=Desc(); regions[0].offset=std::numeric_limits<uint64_t>::max()-63; Reject(d); Reset();
    d=Desc(); regions[0].reserved=1; Reject(d); Reset();
    d=Desc(); regions[0].initialization=2; Reject(d); Reset();
    d=Desc(); regions[1]={64,128,64,TILEXR_RELAY_PRIVATE,0,0}; Reject(d); Reset();
    d=Desc(); channels[1].firstSlot=9; Reject(d); Reset();
    d=Desc(); channels[0].firstSlot=16384; Reject(d); Reset();
    d=Desc(); channels[0].maxCredits=2; Reject(d); Reset();
    d=Desc(); channels[0].participants=3; Reject(d); Reset();
    p=Create(); bindings[0].bytes=1; assert(Run(p)==-3 && !launches); Reset();
    bindings[0].data=input+1; assert(Run(p)==-3); Reset();
    bindings[0].dtypeTag=9; assert(Run(p)==-3); Reset();
    bindings[0].layoutTag=9; assert(Run(p)==-3); Reset();
    bindings[0].data=other; assert(Run(p)==-3); Reset();
    bindings[0].data=workspace; assert(Run(p)==-3); Reset();
    bindings[0].data=own; assert(Run(p)==-3); Reset();
    bindings[0].data=reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max()-63);
    assert(Run(p)==-3); Reset();
    assert(TileXRRelayLaunch(p,&comm,bindings,2,workspace,1,&stream)==-3); Reset();
    args.rankSize=3; assert(Run(p)==-6); Reset();
    args.rank=2; assert(Run(p)==-6); Reset();
    args.peerMemBytes=2097153; assert(Run(p)==-6); Reset();
    args.peerMems[1]=nullptr; assert(Run(p)==-1); Reset();
    soc="Ascend910_9391"; assert(Run(p)==-6); Reset();
    cubes=3; assert(Run(p)==-6); Reset();
    currentContext=nullptr; assert(Run(p)==-1); Reset();
    argsError=701; assert(Run(p)==701); Reset();
    deviceError=702; assert(Run(p)==702); Reset();
    contextError=703; assert(Run(p)==703); Reset();
    cubeError=704; assert(Run(p)==704); Reset();
    fftsError=705; assert(Run(p)==705); Reset();
    magicError=706; assert(Run(p)==706 && !launches); Reset();
    FreshContext(); available=false; assert(Run(p)==-1); Reset();
    FreshContext(); registerError=707; assert(Run(p)==707 && unregisters==1 && !launches); Reset();
    FreshContext(); functionError=708; assert(Run(p)==708 && unregisters==1 && !launches); Reset();
    launchError=709; assert(Run(p)==709 && launches==1 && !syncs);
    Reset(); assert(Run(p)==-1 && !launches); TileXRRelayPlanDestroy(p);
    Reset(); p=Create(); syncError=710; assert(Run(p)==710 && launches==1 && syncs==1);
    Reset(); assert(Run(p)==-1 && !launches); TileXRRelayPlanDestroy(p);
    assert(TileXRRelayPlanCreate(nullptr,nullptr)==-3 && TileXRRelayPlanDestroy(nullptr)==-3);
    TestPreparedExecution();
}
