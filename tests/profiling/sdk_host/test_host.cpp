#include <tilexr/profiling.h>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <iterator>
#include <limits>
#if defined(__linux__)
#include <sys/wait.h>
#endif
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while(0)
using namespace TileXR::Profiling;
uint32_t OtherTranslationUnit();
#if defined(__linux__)
struct FaultOps : Detail::LinuxFileOps {
    enum Fault { None, WriteError, SyncError, CloseError, LinkError, UnlinkError, TempConflict };
    static Fault fault;
    static int outputFd;
    static std::string ownedPath;
    static int Open(const char *p) {
        ownedPath=p;
        if (fault==TempConflict) { std::ofstream(p)<<"foreign"; errno=EEXIST; return -1; }
        return outputFd=LinuxFileOps::Open(p);
    }
    static ssize_t Write(int fd,const void *p,size_t n) { if(fault==WriteError) {errno=ENOSPC;return -1;} return LinuxFileOps::Write(fd,p,n); }
    static int Sync(int fd) { if(fault==SyncError) {errno=EIO;return -1;} return LinuxFileOps::Sync(fd); }
    static int Close(int fd) { const int result=LinuxFileOps::Close(fd); if(fault==CloseError && fd==outputFd) {errno=EIO;return -1;} return result; }
    static int Link(const char *a,const char *b) { if(fault==LinkError) {errno=EOPNOTSUPP;return -1;} return LinuxFileOps::Link(a,b); }
    static int Unlink(const char *p) { if(fault==UnlinkError) {errno=EACCES;return -1;} return LinuxFileOps::Unlink(p); }
};
FaultOps::Fault FaultOps::fault=FaultOps::None;
int FaultOps::outputFd=-1;
std::string FaultOps::ownedPath;
static std::vector<uint8_t> Read(const std::string &p) { std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()}; }
static void Files(const CaptureSnapshot &snapshot)
{
    const std::string base="profiling-sdk-test-"+std::to_string(getpid());
    std::vector<uint8_t> bytes; CHECK(EncodeV1(snapshot,&bytes));
    CHECK(ExportSnapshot(snapshot,base)); CHECK(Read(base)==bytes);
    CHECK(!ExportSnapshot(snapshot,base)); CHECK(Read(base)==bytes); CHECK(unlink(base.c_str())==0);
    for(auto fault : {FaultOps::WriteError,FaultOps::SyncError,FaultOps::CloseError,FaultOps::LinkError}) {
        FaultOps::fault=fault; FaultOps::outputFd=-1;
        CHECK(!Detail::PublishLinux<FaultOps>(base,bytes));
        CHECK(access(base.c_str(),F_OK)!=0); CHECK(access(FaultOps::ownedPath.c_str(),F_OK)!=0);
    }
    FaultOps::fault=FaultOps::TempConflict; FaultOps::outputFd=-1;
    CHECK(!Detail::PublishLinux<FaultOps>(base,bytes));
    CHECK(Read(FaultOps::ownedPath)==std::vector<uint8_t>({'f','o','r','e','i','g','n'}));
    CHECK(unlink(FaultOps::ownedPath.c_str())==0);
    FaultOps::fault=FaultOps::UnlinkError; FaultOps::outputFd=-1;
    const Status warning=Detail::PublishLinux<FaultOps>(base,bytes);
    CHECK(warning.ok() && warning.code==StatusCode::PublishedCleanupWarning); CHECK(Read(base)==bytes);
    CHECK(unlink(FaultOps::ownedPath.c_str())==0); CHECK(unlink(base.c_str())==0);
    CHECK(!ExportSnapshot(snapshot,"/path-that-does-not-exist/records.bin"));
    // Independent processes race with distinct temporary names. Exactly one
    // link wins; the resulting complete bytes equal the intended payload.
    pid_t children[6];
    for(int i=0;i<6;++i) { children[i]=fork(); CHECK(children[i]>=0); if(!children[i]) _exit(ExportSnapshot(snapshot,base).ok()?0:1); }
    int winners=0;
    for(pid_t child:children) { int status=0; CHECK(waitpid(child,&status,0)==child); CHECK(WIFEXITED(status)); winners+=WEXITSTATUS(status)==0; }
    CHECK(winners==1); CHECK(Read(base)==bytes); CHECK(unlink(base.c_str())==0);
}
#endif
int main()
{
    CHECK(OtherTranslationUnit()==1);
    CaptureConfig c; c.logicalStreamCount=3; c.recordsPerStream=8; c.rank=65535;
    Layout l; CHECK(BuildLayout(c,&l)); CHECK(l.recordElements==24 && l.stateElements==24);
    CaptureConfig bad=c; bad.rank=-1; CHECK(!BuildLayout(bad,&l)); bad.rank=65536; CHECK(!BuildLayout(bad,&l));
    bad=c; bad.logicalStreamCount=0; CHECK(!BuildLayout(bad,&l)); bad.logicalStreamCount=65537; CHECK(!BuildLayout(bad,&l));
    bad=c; bad.recordsPerStream=0; CHECK(!BuildLayout(bad,&l)); bad.recordsPerStream=65537; CHECK(!BuildLayout(bad,&l));
    bad.recordsPerStream=9; CHECK(!BuildLayout(bad,&l)); bad=c; bad.maxWorkspaceBytes=1; CHECK(!BuildLayout(bad,&l));
    bad=c; bad.maxFileBytes=1; CHECK(!BuildLayout(bad,&l)); CHECK(!BuildLayout(c,nullptr));
    bad=c; bad.maxFileBytes=MAX_RECORD_FILE_BYTES+1; CHECK(!BuildLayout(bad,&l));
    bad=c; bad.logicalStreamCount=65536; bad.recordsPerStream=65536;
    bad.maxWorkspaceBytes=std::numeric_limits<uint64_t>::max(); bad.maxFileBytes=std::numeric_limits<uint64_t>::max(); CHECK(!BuildLayout(bad,&l));
    CHECK(BuildLayout(c,&l));
    DeviceView view {};
    CHECK(MakeDeviceView(c,reinterpret_cast<void *>(uintptr_t(4096)),l.bytes,&view));
    CHECK(view.timestampsAddress==4096+l.timestampsOffset);
    CHECK(!MakeDeviceView(c,nullptr,l.bytes,&view));
    CHECK(!MakeDeviceView(c,reinterpret_cast<void *>(uintptr_t(4097)),l.bytes,&view));
    CHECK(!MakeDeviceView(c,reinterpret_cast<void *>(uintptr_t(4096)),l.bytes-1,&view));
    CHECK(!MakeDeviceView(c,reinterpret_cast<void *>(uintptr_t(UINTPTR_MAX&~uintptr_t(31))),l.bytes,&view));
    std::vector<uint32_t> counts(24),dropped(24),states(24),ids(24); std::vector<uint64_t> times(24);
    states[0]=uint32_t(Completion::COMMITTED); counts[0]=3; dropped[0]=7;
    ids[0]=1;ids[1]=2;ids[2]=3; times[0]=4;times[1]=0;times[2]=4;
    // Incomplete lanes' nonsensical counts and payloads must be ignored.
    states[8]=uint32_t(Completion::WRITING); counts[8]=UINT32_MAX; ids[8]=99;
    states[16]=uint32_t(Completion::COMMITTED);
    CaptureSnapshot snapshot; CHECK(BuildSnapshot(c,counts,dropped,states,times,ids,&snapshot));
    CHECK(snapshot.streams.size()==3 && snapshot.streams[0].id==0xffff0000U);
    CHECK(snapshot.streams[0].records.size()==3 && snapshot.streams[0].records[0].timestamp==0);
    CHECK(snapshot.streams[0].records[1].eventId==1 && snapshot.streams[0].records[2].eventId==3);
    CHECK(snapshot.streams[0].dropped==7 && snapshot.streams[0].complete);
    CHECK(!snapshot.streams[1].complete && snapshot.streams[1].records.empty());
    CHECK(snapshot.streams[2].complete && snapshot.streams[2].records.empty());
    std::vector<uint8_t> bytes; CHECK(EncodeV1(snapshot,&bytes)); CHECK(bytes.size()==48+3*24+3*16);
    CHECK(bytes[0]=='T' && bytes[7]==0 && bytes[24]==0 && bytes[40]==168);
    CHECK(bytes[48+12]==3); CHECK(bytes[48+24+48+12]==0);
    counts[0]=9; CHECK(!BuildSnapshot(c,counts,dropped,states,times,ids,&snapshot)); counts[0]=3;
    for(uint32_t invalid:{uint32_t(Completion::INVALID),4U,UINT32_MAX}) { states[0]=invalid; CHECK(!BuildSnapshot(c,counts,dropped,states,times,ids,&snapshot)); }
    states[0]=uint32_t(Completion::COMMITTED); ids[2]=0; CHECK(!BuildSnapshot(c,counts,dropped,states,times,ids,&snapshot)); ids[2]=3;
    CHECK(BuildSnapshot(c,counts,dropped,states,times,ids,&snapshot));
    states[0]=uint32_t(Completion::EMPTY); CHECK(BuildSnapshot(c,counts,dropped,states,times,ids,&snapshot)); CHECK(!snapshot.streams[0].complete);
    states[0]=uint32_t(Completion::COMMITTED); CHECK(BuildSnapshot(c,counts,dropped,states,times,ids,&snapshot));
    const auto valid=snapshot;
    snapshot.streams[1].id=snapshot.streams[0].id; CHECK(!EncodeV1(snapshot,&bytes)); snapshot=valid;
    snapshot.streams[0].records[0].eventId=0; CHECK(!EncodeV1(snapshot,&bytes)); snapshot=valid;
    snapshot.streams[0].records[0].timestamp=UINT64_MAX; CHECK(!EncodeV1(snapshot,&bytes)); snapshot=valid;
    snapshot.maxFileBytes=1; CHECK(!EncodeV1(snapshot,&bytes)); snapshot=valid;
#if defined(__linux__)
    Files(snapshot);
#endif
    std::cout<<"profiling Host layout/snapshot/v1/file contracts passed\n";
}
