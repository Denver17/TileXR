/* Copyright (c) 2026 TileXR Project. CANN Open Software License Agreement v2.0. */
/* 根据 collective launch 计划生成 workload 与 stage metrics，供 ProfilingCaptureQueue 导出通用 manifest；不从成功 records 推断参与者。
 * Build planned workload and stage metrics for ProfilingCaptureQueue manifest export; participant declarations do not come from successful records.
 */

#ifndef TILEXR_COLLECTIVES_PROFILING_PRODUCER_H
#define TILEXR_COLLECTIVES_PROFILING_PRODUCER_H

#include <tilexr/profiling.h>
#include "alltoall_a5_common.h"

namespace TileXRCollectives { namespace Host {

struct CollectiveProfilingWorkload {
    uint32_t rank = 0, rankSize = 0, lanes = 0;
    uint64_t inputBytes = 0, bytesPerPeer = 0;
    bool groupedA5 = false, firstInvocation = false;
    std::string operation, shape;
};

// Workload declarations are planned algorithm payload accounting, not hardware
// counters. No successful-record set is used to construct this input.
inline TileXR::Profiling::Status PopulateCollectiveProducer(
    const CollectiveProfilingWorkload &workload, TileXR::Profiling::ManifestProducer *producer)
{
    using namespace TileXR::Profiling;
    if (!producer || !workload.rankSize || workload.rankSize > 65536 || workload.rank >= workload.rankSize ||
        !workload.lanes || workload.lanes > 65536 || !workload.inputBytes || workload.operation.empty())
        return {StatusCode::InvalidArgument,"invalid planned collective profiling workload"};
    if (workload.groupedA5 && (!TileXRGrouped::ValidRankSize(workload.rankSize) ||
        workload.lanes != TileXRGrouped::kSendCoreCount+TileXRGrouped::kGroupWidth ||
        !workload.bytesPerPeer || workload.bytesPerPeer > UINT32_MAX))
        return {StatusCode::InvalidArgument,"grouped A5 workload requires 48 logical lanes and valid peer bytes/ranks"};
    producer->kind = "communication";
    producer->attributes["operator"] = workload.operation;
    producer->attributes["shape"] = workload.shape;
    producer->attributes["expected_rank_count"] = std::to_string(workload.rankSize);
    producer->attributes["expected_logical_lane_count"] = std::to_string(workload.lanes);
    producer->attributes["workload_source"] = "execution plan before kernel enqueue; not inferred from received records";
    producer->attributes["first_invocation"] = workload.firstInvocation ? "true" : "false";
    producer->metrics.clear();
    const char *a5Names[] = {"kernel_total","send_all","credit-wait","send-put-signal","send-quiet",
        "sdma_prewarm","self_copy","receive_loop","receive-wait","receive-copy","sdma-submit",
        "sdma-prepare","sdma-cache-clean","sdma-dsb","sdma-doorbell","sdma-wait"};
    const std::string semantics = workload.groupedA5 ?
        "planned algorithm payload bytes per movement; excludes signals and protocol overhead" :
        "algorithm input bytes deterministically apportioned across profiling lanes; not physical lane traffic";
    for (uint32_t lane=0; lane<workload.lanes; ++lane) {
        uint64_t sendBytes=0, receiveBytes=0;
        uint32_t sendPeers=0, receivePeers=0;
        if (workload.groupedA5) {
            if (lane < TileXRGrouped::kSendCoreCount) {
                for (uint32_t group=0; group<TileXRGrouped::GroupCount(workload.rankSize); ++group) {
                    const int32_t peer=TileXRGrouped::Peer(workload.rank,workload.rankSize,group,lane%TileXRGrouped::kGroupWidth);
                    if(peer<0) continue;
                    ++sendPeers;
                    const bool remote=workload.rank/TileXRGrouped::kRanksPerNode != static_cast<uint32_t>(peer)/TileXRGrouped::kRanksPerNode;
                    const uint64_t primary=remote ? workload.bytesPerPeer*TileXRGrouped::kPrimaryRouteParts/TileXRGrouped::kRouteParts : workload.bytesPerPeer;
                    sendBytes += lane<TileXRGrouped::kGroupWidth ? primary : workload.bytesPerPeer-primary;
                }
            } else {
                for(uint32_t peer=0;peer<workload.rankSize;++peer)
                    if(peer!=workload.rank && peer%16==lane-32) ++receivePeers;
                receiveBytes=uint64_t(receivePeers)*workload.bytesPerPeer;
            }
        }
        const uint32_t stages=workload.groupedA5 ? 16 : 1;
        for(uint32_t stage=0;stage<stages;++stage) {
            ProducerStageMetric metric;
            metric.logicalLane=lane; metric.stageId=stage;
            metric.stage=workload.groupedA5 ? a5Names[stage] : "kernel_total";
            metric.source="collective host execution plan; matched native marker contract v1";
            metric.kernelVersion=workload.groupedA5 ? "grouped_alltoall_a5_markers.v1" : "aiv_coarse_markers.v1";
            metric.nativeStatus="native Ascend C; planned workload, not a device-success assertion";
            // Execution provenance is rank/stage scoped. Keep it identical on
            // applicable and explicitly N/A lanes of the same stage.
            metric.backend=workload.groupedA5 ? (stage==0 ? "UDMA+SDMA" :
                (stage>=1 && stage<=4 ? "UDMA" : "SDMA")) : "peer-memory AIV";
            metric.byteSemantics=semantics;
            if (!workload.groupedA5) {
                metric.bytes=workload.inputBytes/workload.lanes + (lane<workload.inputBytes%workload.lanes ? 1 : 0);
            } else if (stage==0) {
                metric.bytes=sendBytes+receiveBytes+(lane==32 ? workload.bytesPerPeer : 0);
            } else if ((stage==1 || stage==3) && lane<32 && (stage==1 || sendPeers)) {
                metric.bytes=sendBytes;
            } else if(stage==6 && lane==32) {
                metric.bytes=workload.bytesPerPeer;
            } else if(stage==7 && lane>=32) {
                metric.bytes=receiveBytes;
            } else if((stage==9 || stage==10) && lane>=32 && receivePeers) {
                metric.bytes=receiveBytes;
            } else {
                metric.applicable=false;
                metric.reason="control/wait stage or no payload interval is scheduled on this logical lane";
            }
            producer->metrics.push_back(std::move(metric));
        }
    }
    producer->versions["kernel_version"] = workload.groupedA5 ? "grouped_alltoall_a5_markers.v1" : "aiv_coarse_markers.v1";
    return ValidateManifestProducer(*producer);
}

} }
#endif
