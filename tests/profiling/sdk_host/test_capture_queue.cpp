#include "mock_acl_runtime.h"
#include "../../../src/collectives/host/profiling_capture_queue.h"
#include "../../../src/comm/tilexr_internal_resource_cleanup.h"
#if defined(__linux__)
#include <dirent.h>
#endif
using TileXRCollectives::Host::ProfilingCaptureQueue;
static void CommonCleanupPhases()
{
 struct Communicator {
  bool ownsResource;
  int localStatus, peerStatus, finalizations=0, agreements=0;
  int FinalizeInternalResource() {
   if (!ownsResource) return 0;
   ++finalizations;
   if (localStatus==0) ownsResource=false;
   return localStatus;
  }
  int AgreeInternalStatus(int status) { ++agreements; return status ? status : peerStatus; }
 };
 Communicator absent {false,0,7};
 CHECK(TileXR::FinalizeInternalResourceTogether(absent)==7);
 CHECK(absent.agreements==1 && absent.finalizations==0);
 Communicator retained {true,8,0};
 CHECK(TileXR::FinalizeInternalResourceTogether(retained)==8);
 CHECK(retained.ownsResource && retained.agreements==1);
 retained.localStatus=0;
 CHECK(TileXR::FinalizeInternalResourceTogether(retained)==0);
 CHECK(!retained.ownsResource && retained.agreements==2);
 Communicator released {true,0,7};
 CHECK(TileXR::FinalizeInternalResourceTogether(released)==7);
 CHECK(!released.ownsResource);
 released.peerStatus=0;
 CHECK(TileXR::FinalizeInternalResourceTogether(released)==0);
 CHECK(released.finalizations==1 && released.agreements==2);
}
static void Env(const char *name,const char *value)
{
#if defined(_WIN32)
 CHECK(_putenv_s(name,value?value:"")==0);
#else
 CHECK(value?setenv(name,value,1)==0:unsetenv(name)==0);
#endif
}
static void SubmitEmpty(void *args)
{
 CHECK(args);const auto &view=*static_cast<DeviceView *>(args);
 auto *state=reinterpret_cast<uint32_t *>(uintptr_t(view.statesAddress));
 for(uint32_t lane=0;lane<view.logicalStreamCount;++lane) state[size_t(lane)*view.stateStride]=uint32_t(Completion::COMMITTED);
}
int main()
{
 CommonCleanupPhases();
 Env("TILEXR_PROFILING_OUTPUT_DIR",nullptr);Env("TILEXR_PROFILING_CAPTURE_ID",nullptr);
 Mock::Reset(); {ProfilingCaptureQueue q;CHECK(!q.Acquire(Mock::expectedStream,48,0,1));CHECK(Mock::allocations.empty());CHECK(q.Drain());}
 Env("TILEXR_PROFILING_OUTPUT_DIR","profiling-queue-unused");
 Mock::Reset(); {ProfilingCaptureQueue q;CHECK(!q.Acquire(Mock::expectedStream,48,0,2));CHECK(Mock::allocations.empty());CHECK(q.Drain());}
 Env("TILEXR_PROFILING_CAPTURE_ID","01234567-89ab-4cde-8123-456789abcdef");
 Mock::Reset(); {
  ProfilingCaptureQueue q;std::set<void *> descriptors;
  for(int i=0;i<4;++i) {
   void *args=q.Acquire(Mock::expectedStream,48,0,2);CHECK(args);CHECK(descriptors.insert(args).second);
   CHECK(static_cast<DeviceView *>(args)->logicalStreamCount==48);CHECK(Mock::allocations.count(args)==1);
   CHECK(Mock::calls["sync"]==0 && Mock::calls["d2h"]==0 && Mock::calls["free"]==0);
   if(i) {CHECK(Mock::calls["malloc"]==4 && Mock::calls["memset"]==4 && Mock::calls["h2d"]==4);}
  }
  CHECK(!q.Acquire(Mock::expectedStream,48,0,2));CHECK(Mock::allocations.size()==4);
  CHECK(!q.Acquire(reinterpret_cast<void *>(4),48,0,2));CHECK(!q.Acquire(Mock::expectedStream,33,0,2));
  CHECK(Mock::calls["sync"]==0 && Mock::calls["free"]==0);
  Mock::failure="sync";Mock::failureNth=1;CHECK(!q.Drain());CHECK(Mock::allocations.size()>=1);
  Mock::failure="";
#if defined(__linux__)
  CHECK(q.Drain());CHECK(Mock::allocations.empty());CHECK(q.Drain());
#else
  // Linux-only publication fails explicitly; snapshots remain owned for retry.
  CHECK(!q.Drain());CHECK(Mock::allocations.size()==4);
#endif
 }CHECK(Mock::allocations.empty());
#if defined(__linux__)
 const std::string directory="profiling-queue-"+std::to_string(getpid());
 Env("TILEXR_PROFILING_OUTPUT_DIR",directory.c_str());
 Mock::Reset();{
  ProfilingCaptureQueue q;void *args=q.Acquire(Mock::expectedStream,48,0,2);SubmitEmpty(args);
  Mock::failure="free";Mock::failureNth=1;CHECK(!q.Drain());CHECK(Mock::allocations.size()==1);
  const int copies=Mock::calls["d2h"],syncs=Mock::calls["sync"];
  Mock::failure="";CHECK(q.Drain());CHECK(Mock::calls["d2h"]==copies && Mock::calls["sync"]==syncs);CHECK(Mock::allocations.empty());
 }
 // Successful second Drain proves the published file was not exported again:
 // the no-overwrite writer would have rejected that existing target.
#endif
 Env("TILEXR_PROFILING_OUTPUT_DIR",nullptr);Env("TILEXR_PROFILING_CAPTURE_ID",nullptr);
 std::cout<<"profiling business capture queue mock contracts passed\n";
}
