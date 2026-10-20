#include "mock_acl_runtime.h"
static CaptureConfig Config() { CaptureConfig c;c.recordsPerStream=8;c.logicalStreamCount=2;c.captureId[0]=1;return c; }
static void Submit(AclCapture &capture) {
 const DeviceView &view=*static_cast<DeviceView *>(capture.DeviceArgs());
 auto *states=reinterpret_cast<uint32_t *>(uintptr_t(view.statesAddress));
 auto *counts=reinterpret_cast<uint32_t *>(uintptr_t(view.countsAddress));
 auto *ids=reinterpret_cast<uint32_t *>(uintptr_t(view.eventIdsAddress));
 auto *times=reinterpret_cast<uint64_t *>(uintptr_t(view.timestampsAddress));
 states[0]=uint32_t(Completion::COMMITTED);counts[0]=2;ids[0]=1;ids[1]=2;times[0]=0;times[1]=10;
 states[view.stateStride]=uint32_t(Completion::WRITING);counts[view.stateStride]=UINT32_MAX;
}
struct BrokenUuidSource { uint32_t operator()() { throw 1; } };
int main()
{
 std::array<uint8_t,16> failedId {}; CHECK(!Detail::NewCaptureId<BrokenUuidSource>(&failedId));
 CHECK(std::all_of(failedId.begin(),failedId.end(),[](uint8_t b){return b==0;}));
 static_assert(!std::is_copy_constructible<AclCapture>::value,"not copyable");
 static_assert(std::is_move_constructible<AclCapture>::value,"movable");
 static_assert(!std::is_move_assignable<AclCapture>::value,"no move assignment");
 for(const char *op:{"context","malloc","memset","h2d"}) {
  Mock::Reset(op); { AclCapture c; CHECK(!c.Initialize(Config(),Mock::expectedStream)); CHECK(!c.DeviceArgs()); CHECK(c.WorkspaceBytes()==0); CHECK(c.Close()); } CHECK(Mock::allocations.empty());
 }
 Mock::Reset(); {
  AclCapture c; CHECK(c.WorkspaceBytes()==0);CHECK(!c.Collect());CHECK(!c.Export("unused"));CHECK(c.Close());CHECK(c.Close());
  CHECK(c.Initialize(Config(),Mock::expectedStream));CHECK(c.DeviceArgs());CHECK(!c.Initialize(Config(),Mock::expectedStream));Submit(c);
  Mock::context=reinterpret_cast<void *>(3);CHECK(!c.Collect());CHECK(!c.DeviceArgs());CHECK(!c.Close());CHECK(Mock::allocations.size()==1);
  Mock::context=reinterpret_cast<void *>(1);CHECK(c.Collect());const auto *snapshot=c.Snapshot();CHECK(snapshot&&snapshot->streams.size()==2);
  CHECK(snapshot->streams[0].records[0].timestamp==0&&!snapshot->streams[1].complete);
  const int copies=Mock::calls["d2h"], syncs=Mock::calls["sync"];CHECK(c.Collect());CHECK(Mock::calls["d2h"]==copies&&Mock::calls["sync"]==syncs);
  CHECK(!c.Export("/nonexistent/path/records.bin")); CHECK(c.Snapshot()==snapshot);
  Mock::context=reinterpret_cast<void *>(3);CHECK(!c.Export("/nonexistent/path/records.bin"));CHECK(Mock::calls["context"]==4);
  Mock::context=reinterpret_cast<void *>(1);CHECK(c.Close());CHECK(c.Snapshot()==snapshot); CHECK(c.Close());
#if defined(__linux__)
  const std::string path="profiling-acl-mock-"+std::to_string(getpid())+".bin";
  Mock::context=reinterpret_cast<void *>(3); CHECK(c.Save(path));
  CHECK(Mock::calls["d2h"]==copies && Mock::calls["sync"]==syncs); CHECK(!c.Save(path));
  CHECK(c.Save(path+".retry")); CHECK(unlink(path.c_str())==0 && unlink((path+".retry").c_str())==0);
  Mock::context=reinterpret_cast<void *>(1);
#endif
  CHECK(c.Initialize(Config(),Mock::expectedStream));CHECK(!c.Snapshot());CHECK(c.Close());
 } CHECK(Mock::allocations.empty());
 for(int nth=1;nth<=5;++nth) {
  Mock::Reset();AclCapture c;CHECK(c.Initialize(Config(),Mock::expectedStream));Submit(c);
  Mock::failure="d2h";Mock::failureNth=nth;CHECK(!c.Collect());CHECK(!c.Snapshot());CHECK(Mock::allocations.size()==1);
  Mock::failure="";CHECK(c.Collect());CHECK(c.Close());CHECK(Mock::allocations.empty());
 }
 Mock::Reset();{
  AclCapture c;CHECK(c.Initialize(Config(),Mock::expectedStream));Submit(c);Mock::failure="sync";
  CHECK(!c.Collect());CHECK(!c.Snapshot());CHECK(!c.DeviceArgs());CHECK(Mock::allocations.size()==1);
  Mock::calls["sync"]=0;CHECK(!c.Close());CHECK(Mock::allocations.size()==1);
  Mock::failure="";CHECK(c.Collect());CHECK(c.Close());
 } CHECK(Mock::allocations.empty());
 Mock::Reset();{
  AclCapture c;CHECK(c.Initialize(Config(),Mock::expectedStream));Mock::failure="free";CHECK(!c.Close());CHECK(Mock::allocations.size()==1);
  CHECK(c.WorkspaceBytes()>0);Mock::failure="";CHECK(c.Close());CHECK(c.WorkspaceBytes()==0);CHECK(c.Close());
 } CHECK(Mock::allocations.empty());
 Mock::Reset("memset");{
  // The first failure must retain its error even when cleanup also fails.
  AclCapture c; Mock::failFree=true; const Status s=c.Initialize(Config(),Mock::expectedStream);
  CHECK(!s && s.aclError==73);CHECK(Mock::allocations.size()==1);CHECK(!c.DeviceArgs());
  Mock::failFree=false; CHECK(c.Close());
 } CHECK(Mock::allocations.empty());
 Mock::Reset(); {
  void *orphan=nullptr;
  { AclCapture c; CHECK(c.Initialize(Config(),Mock::expectedStream));orphan=c.DeviceArgs();Mock::failure="sync"; }
  CHECK(Mock::allocations.count(orphan)==1);CHECK(Mock::calls["free"]==0);
  // Emulate the application's later safe recovery of conservatively retained GM.
  Mock::failure="";CHECK(aclrtSynchronizeStream(Mock::expectedStream)==0);CHECK(aclrtFree(orphan)==0);
 } CHECK(Mock::allocations.empty());
 Mock::Reset();{
  AclCapture c;CHECK(c.Initialize(Config(),Mock::expectedStream));Submit(c);void *args=c.DeviceArgs();
  const auto bytes=c.WorkspaceBytes();CHECK(bytes>0);
  AclCapture moved(std::move(c));CHECK(!c.DeviceArgs());CHECK(c.WorkspaceBytes()==0);CHECK(moved.DeviceArgs()==args);CHECK(moved.WorkspaceBytes()==bytes);CHECK(c.Close());CHECK(moved.Collect());CHECK(moved.Close());CHECK(moved.WorkspaceBytes()==0);
 } CHECK(Mock::allocations.empty());
 Mock::Reset();{
  CaptureConfig config=Config();config.enabled=false;AclCapture c;CHECK(c.Initialize(config,Mock::expectedStream));CHECK(!c.DeviceArgs());CHECK(!c.Snapshot());
  CHECK(c.Collect());CHECK(c.Export("must-not-exist"));CHECK(c.Save("must-not-exist"));CHECK(c.Close());CHECK(c.Save("must-not-exist"));CHECK(Mock::calls.empty());
 }
 Mock::Reset();{
  AclCapture c;CHECK(c.Initialize(Mock::expectedStream,1));auto first=c.Config().captureId;CHECK(c.Close());
  CHECK(c.Initialize(Mock::expectedStream,1));CHECK(first!=c.Config().captureId);CHECK(c.Close());
 } CHECK(Mock::allocations.empty());
 std::cout<<"profiling ACL lifecycle mock contracts passed (no CANN/NPU claim)\n";
}
