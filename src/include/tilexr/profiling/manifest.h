/* Copyright (c) 2026 TileXR Project. CANN Open Software License Agreement v2.0. */
/* 把 CaptureSnapshot、HostEvents 和调用方的执行计划写成 manifest shard；
 * 供 collectives 等 producer 使用，依赖 events.h 与 file.h 的不覆盖发布协议。
 * Export CaptureSnapshot, HostEvents and caller-planned execution metadata
 * as manifest shards for producers such as collectives, using events.h and the
 * non-overwriting publication contract from file.h.
 */

#ifndef TILEXR_PROFILING_MANIFEST_H
#define TILEXR_PROFILING_MANIFEST_H
#include "events.h"
#include <map>

namespace TileXR { namespace Profiling {
struct ProducerStageMetric {
    uint32_t logicalLane = 0, stageId = 0;
    std::string stage, source, byteSemantics, scope = "per_lane_total";
    uint64_t bytes = 0;
    bool applicable = true;
    std::string reason, backend, nativeStatus, kernelVersion;
};
struct ManifestProducer {
    // Supplied from the execution plan, independently of successful records.
    std::string attemptId, mappingSha256, host, deviceId;
    std::string deviceNamespace = "host-local ACL ordinal";
    std::string kind = "communication";
    std::map<std::string,std::string> environment, versions, attributes;
    std::vector<ProducerStageMetric> metrics;
};
inline Status ValidateManifestProducer(const ManifestProducer &producer)
{
    if (producer.attemptId.empty() || producer.attemptId.size()>256 || producer.mappingSha256.size()!=64 ||
        producer.host.empty() || producer.host.size()>4096 || producer.deviceId.empty() || producer.deviceId.size()>256 ||
        producer.deviceNamespace.empty() || producer.deviceNamespace.size()>256 ||
        (producer.kind!="compute" && producer.kind!="communication") || producer.metrics.size()>65536)
        return {StatusCode::InvalidArgument,"producer requires planned attempt/mapping and explicit identity"};
    for(const auto *section:{&producer.environment,&producer.versions,&producer.attributes}) {
        if(section->size()>128) return {StatusCode::InvalidArgument,"producer metadata exceeds item limit"};
        for(const auto &item:*section) if(item.first.size()>256 || item.second.size()>4096)
            return {StatusCode::InvalidArgument,"producer metadata exceeds string limit"};
    }
    for (char c:producer.mappingSha256)
        if (!((c>='0' && c<='9') || (c>='a' && c<='f')))
            return {StatusCode::InvalidArgument,"mapping fingerprint must be canonical lowercase SHA256"};
    std::set<std::pair<uint32_t,uint32_t>> unique;
    for (const auto &metric:producer.metrics) {
        if (metric.logicalLane>65535 || metric.stage.empty() || metric.source.empty() ||
            metric.stage.size()>256 || metric.source.size()>4096 || metric.byteSemantics.size()>256 ||
            metric.reason.size()>4096 || metric.backend.size()>256 || metric.nativeStatus.size()>256 || metric.kernelVersion.size()>256 ||
            !unique.insert({metric.logicalLane,metric.stageId}).second ||
            (metric.applicable && (metric.byteSemantics.empty() ||
                (metric.scope!="per_lane_total" && metric.scope!="per_interval"))) ||
            (!metric.applicable && metric.reason.empty()) ||
            metric.backend.empty() || metric.nativeStatus.empty() || metric.kernelVersion.empty())
            return {StatusCode::InvalidArgument,"invalid/duplicate stage metric or execution declaration"};
    }
    return {};
}
inline void ManifestStringMap(std::ostream &out, const std::map<std::string,std::string> &values)
{
    out << '{'; bool first=true;
    for(const auto &item:values) { if(!first)out<<',';first=false;out<<HostJsonString(item.first)<<':'<<HostJsonString(item.second); }
    out << '}';
}
inline Status ExportManifestShard(const CaptureSnapshot &snapshot, const CaptureConfig &config,
    const HostEvents &events, const ManifestProducer &producer, const std::string &path)
{
    const Status valid=ValidateManifestProducer(producer); if(!valid)return valid;
    if (events.AttemptId()!=producer.attemptId || snapshot.captureId!=config.captureId)
        return {StatusCode::InvalidArgument,"producer attempt/capture does not match collected evidence"};
    std::set<uint32_t> lanes;
    for(const auto &stream:snapshot.streams) lanes.insert(stream.id & 65535U);
    for(const auto &metric:producer.metrics) if(!lanes.count(metric.logicalLane))
        return {StatusCode::InvalidArgument,"producer metric references an unconfigured logical lane"};
    try {
        std::ostringstream out;
        out << "{\"schema\":\"tilexr_profiling_manifest_shard.v1\",\"attempt_id\":" << HostJsonString(producer.attemptId)
            << ",\"capture_id\":" << HostJsonString(CaptureIdString(snapshot.captureId))
            << ",\"mapping_sha256\":" << HostJsonString(producer.mappingSha256)
            << ",\"rank\":" << config.rank << ",\"participants\":[";
        bool first=true;
        for(const auto &stream:snapshot.streams) {
            if(!first)out<<',';first=false;
            out<<"{\"stream_id\":"<<stream.id<<",\"rank\":"<<config.rank
               <<",\"logical_lane\":"<<(stream.id&65535U)<<",\"sampled\":true,\"host\":"<<HostJsonString(producer.host)
               <<",\"device\":{\"id\":"<<HostJsonString(producer.deviceId)
               <<",\"namespace\":"<<HostJsonString(producer.deviceNamespace)<<"}}";
        }
        out<<"],\"stage_metrics\":[";first=true;
        for(const auto &metric:producer.metrics) {
            if(!first)out<<',';first=false;
            out<<"{\"stream_id\":"<<((uint32_t(config.rank)<<16)|metric.logicalLane)
               <<",\"stage_id\":"<<metric.stageId<<",\"stage\":"<<HostJsonString(metric.stage)
               <<",\"source\":"<<HostJsonString(metric.source);
            if(metric.applicable) out<<",\"status\":\"known\",\"bytes\":"<<metric.bytes
                <<",\"byte_semantics\":"<<HostJsonString(metric.byteSemantics)<<",\"scope\":"<<HostJsonString(metric.scope);
            else out<<",\"status\":\"not_applicable\",\"reason\":"<<HostJsonString(metric.reason);
            out<<'}';
        }
        out<<"],\"execution\":{\"kind\":"<<HostJsonString(producer.kind)<<",\"stages\":[";
        std::map<uint32_t,const ProducerStageMetric *> stages;first=true;
        for(const auto &metric:producer.metrics) {
            auto inserted=stages.emplace(metric.stageId,&metric);
            if(!inserted.second) {
                const auto &other=*inserted.first->second;
                if(other.stage!=metric.stage || other.backend!=metric.backend || other.nativeStatus!=metric.nativeStatus || other.kernelVersion!=metric.kernelVersion)
                    return {StatusCode::InvalidArgument,"conflicting rank/stage execution metadata"};
                continue;
            }
            if(!first)out<<',';first=false;
            out<<"{\"rank\":"<<config.rank<<",\"stage_id\":"<<metric.stageId<<",\"stage\":"<<HostJsonString(metric.stage)
               <<",\"backend\":"<<HostJsonString(metric.backend)<<",\"native_status\":"<<HostJsonString(metric.nativeStatus)
               <<",\"kernel_version\":"<<HostJsonString(metric.kernelVersion)<<'}';
        }
        out<<"]},\"environment\":";ManifestStringMap(out,producer.environment);
        out<<",\"versions\":";ManifestStringMap(out,producer.versions);
        out<<",\"attributes\":";ManifestStringMap(out,producer.attributes);
        out<<",\"events\":[";first=true;
        for(const auto &event:events.Snapshot()) {
            if(!first)out<<',';first=false;
            out<<"{\"event_id\":"<<HostJsonString(std::to_string(config.rank)+":"+event.id)<<",\"code\":"<<HostJsonString(event.code)
               <<",\"severity\":"<<HostJsonString(event.info?"info":event.warning?"warning":"error")
               <<",\"phase\":"<<HostJsonString(event.phase)<<",\"scope\":\"rank\",\"rank\":"<<config.rank
               <<",\"count\":1,\"source\":\"TileXR::Profiling::HostEvents\",\"message\":"<<HostJsonString(event.message)
               <<",\"status_code\":"<<int(event.status)<<",\"acl_error\":"<<event.aclError
               <<",\"operation_id\":"<<HostJsonString(event.operation)<<",\"retry\":"<<event.retry
               <<",\"resolved\":"<<(event.resolved?"true":"false")<<'}';
        }
        out<<"],\"dropped_events\":"<<events.DroppedEvents()<<"}\n";
        const auto json=out.str();
        if(json.size()>4U*1024U*1024U) return {StatusCode::InvalidArgument,"producer shard exceeds 4 MiB budget"};
        return PublishRecords(path,std::vector<uint8_t>(json.begin(),json.end()));
    } catch(...) { return {StatusCode::Io,"producer manifest export failed"}; }
}
}}
#endif
