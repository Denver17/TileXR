// 功能：用 mock ACL 验证借用 workspace 的代次、稀疏 lane 与失败重试的 lease 归还。
// Purpose: Verify borrowed workspace generations, sparse lanes and retry-safe lease return using mock ACL.
#include "mock_acl_runtime.h"

int main()
{
    CaptureConfig config; config.rank=7; config.logicalStreamCount=3;
    config.logicalLaneIds={2,9,65535}; config.recordsPerStream=8;
    Layout layout; CHECK(BuildLayout(config,&layout));
    CHECK(layout.logicalLaneIdsOffset%64==0 && layout.bytes%64==0);
    CaptureConfig bad=config; bad.logicalLaneIds={9,2,65535}; CHECK(!BuildLayout(bad,&layout));
    bad=config; bad.logicalLaneIds={2,2,65535}; CHECK(!BuildLayout(bad,&layout));
    bad=config; bad.logicalLaneIds={2}; CHECK(!BuildLayout(bad,&layout));
    CHECK(BuildLayout(config,&layout));
    Mock::Reset();
    void *allocation=nullptr; CHECK(aclrtMalloc(&allocation,layout.bytes,ACL_MEM_MALLOC_HUGE_FIRST)==0);
    WorkspacePool pool;
    CHECK(pool.Add(allocation,layout.bytes,reinterpret_cast<uintptr_t>(Mock::context)));
    CHECK(!pool.Add(allocation,layout.bytes,reinterpret_cast<uintptr_t>(Mock::context)));
    CHECK(!pool.Add(static_cast<uint8_t *>(allocation)+64,layout.bytes-64,reinterpret_cast<uintptr_t>(Mock::context)));
    uint64_t firstGeneration=0;
    {
        WorkspaceLease lease; CHECK(pool.Acquire(layout.bytes,1,&lease));
        firstGeneration=lease.Generation(); CHECK(firstGeneration==1 && pool.BusyCount()==1);
        WorkspaceLease extra; CHECK(!pool.Acquire(layout.bytes,1,&extra));
        CHECK(!pool.Acquire(layout.bytes,1,&lease));
        WorkspaceLease moved(std::move(lease)); CHECK(!lease && moved);
    }
    CHECK(pool.BusyCount()==0);
    {
        WorkspaceLease lease; CHECK(pool.Acquire(layout.bytes,1,&lease));
        CHECK(lease.Generation()>firstGeneration);
        AclCapture capture; CHECK(capture.InitializeBorrowed(config,Mock::expectedStream,std::move(lease)));
        CHECK(!lease && Mock::calls["malloc"]==1);
        auto *view=static_cast<DeviceView *>(capture.DeviceArgs());
        CHECK(view->abiVersion==2 && view->generation>firstGeneration);
        const auto *logical=reinterpret_cast<const uint32_t *>(view->logicalLaneIdsAddress);
        CHECK(logical[0]==2 && logical[1]==9 && logical[2]==65535);
        auto *states=reinterpret_cast<uint32_t *>(view->statesAddress);
        states[0]=uint32_t(Completion::COMMITTED);
        states[view->stateStride]=uint32_t(Completion::WRITING);
        CHECK(capture.Collect());
        const auto *snapshot=capture.Snapshot(); CHECK(snapshot && snapshot->streams.size()==3);
        CHECK(snapshot->streams[0].id==((7U<<16)|2U));
        CHECK(snapshot->streams[1].id==((7U<<16)|9U));
        CHECK(snapshot->streams[2].id==((7U<<16)|65535U));
        CHECK(snapshot->streams[1].completion==Completion::WRITING);
        CHECK(snapshot->streams[2].completion==Completion::EMPTY);
        CHECK(capture.Close()); CHECK(pool.BusyCount()==0 && Mock::calls["free"]==0);
        CHECK(capture.Snapshot()==snapshot);
    }
    {
        WorkspaceLease lease; CHECK(pool.Acquire(layout.bytes,1,&lease));
        Mock::context=reinterpret_cast<void *>(3);
        AclCapture capture; CHECK(!capture.InitializeBorrowed(config,Mock::expectedStream,std::move(lease)));
        CHECK(lease); Mock::context=reinterpret_cast<void *>(1);
        CHECK(capture.InitializeBorrowed(config,Mock::expectedStream,std::move(lease)));
        CHECK(capture.DeviceArgs()); Mock::Reset("sync");
        CHECK(!capture.Close()); CHECK(pool.BusyCount()==1 && Mock::calls["free"]==0);
        WorkspaceLease extra; CHECK(!pool.Acquire(layout.bytes,1,&extra));
        Mock::failure=""; CHECK(capture.Close()); CHECK(pool.BusyCount()==0);
    }
    {
        WorkspaceLease lease; CHECK(pool.Acquire(layout.bytes,1,&lease));
        Mock::Reset("memset");
        AclCapture capture; CHECK(!capture.InitializeBorrowed(config,Mock::expectedStream,std::move(lease)));
        CHECK(!capture.DeviceArgs());
        const int writes=Mock::calls["memset"];
        CaptureConfig oversized=config;oversized.recordsPerStream=65536;
        CHECK(!capture.Initialize(oversized,Mock::expectedStream)); CHECK(Mock::calls["memset"]==writes);
        Mock::context=reinterpret_cast<void *>(3);
        CHECK(!capture.Initialize(config,Mock::expectedStream)); CHECK(Mock::calls["memset"]==writes);
        Mock::context=reinterpret_cast<void *>(1); Mock::failure="";
        CHECK(capture.Initialize(config,Mock::expectedStream));
        CHECK(static_cast<DeviceView *>(capture.DeviceArgs())->generation>firstGeneration);
        CHECK(capture.Close()); CHECK(pool.BusyCount()==0);
        CHECK(Mock::calls["free"]==0);
    }
    CHECK(aclrtFree(allocation)==0); CHECK(Mock::allocations.empty());
    std::cout << "workspace leases, generations, sparse IDs and retry contracts passed (mock)\n";
}
